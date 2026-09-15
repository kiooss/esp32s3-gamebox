/*
 * 收藏必须跟着卡上完整路径走：同名游戏可能分属不同平台，ZIP 的显示名也可能
 * 在解析前后变化。独立文件还让 SELECT 重扫只刷新目录，不改变用户的收藏。
 *
 * 路径内容和排序指针表都放 PSRAM；查询二分查找，菜单遍历上千游戏时不用
 * 反复逐个比对全部收藏。格式里只存 NUL 分隔路径，不落盘指针或 size_t。
 */
#include "rom_favorites.h"
#include "sd_card.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define FAVORITES_PATH SD_MOUNT_POINT "/.gamebox-favorites"
#define FAVORITES_TEMP FAVORITES_PATH ".tmp"
#define FAVORITES_BACKUP FAVORITES_PATH ".bak"
#define FAVORITES_MAGIC UINT32_C(0x56464247) /* 小端文件中为 GBFV */
#define FAVORITES_VERSION 1
#define IO_CHUNK 4096

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t count;
    uint32_t payload_size;
    uint32_t payload_crc32;
} favorites_header_t;

typedef struct {
    char *payload;
    char **paths;
    size_t size;
    size_t count;
} favorites_t;

static const char *TAG = "favorites";
static favorites_t s_favorites;
static bool s_initialized;
/* 从备份恢复时，主文件可能还是坏文件；保存不能拿它覆盖唯一有效的备份。 */
static bool s_primary_valid;

static void free_favorites(favorites_t *favorites)
{
    free(favorites->paths);
    free(favorites->payload);
    *favorites = (favorites_t){0};
}

static esp_err_t alloc_favorites(favorites_t *favorites, size_t count, size_t size)
{
    *favorites = (favorites_t){.count = count, .size = size};
    if (count == 0 && size == 0) return ESP_OK;
    if (count == 0 || size == 0 || count > SIZE_MAX / sizeof(char *)) {
        return ESP_ERR_INVALID_SIZE;
    }
    favorites->payload = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    favorites->paths = heap_caps_malloc(count * sizeof(char *),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!favorites->payload || !favorites->paths) {
        free_favorites(favorites);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool valid_path(const char *path, size_t size)
{
    const size_t prefix_size = sizeof(SD_MOUNT_POINT "/") - 1;
    return size > prefix_size && size < ROM_STORE_PATH_LEN &&
           memcmp(path, SD_MOUNT_POINT "/", prefix_size) == 0;
}

static bool io_exact(int fd, void *buffer, size_t size, bool writing)
{
    uint8_t *p = buffer;
    while (size > 0) {
        ssize_t done = writing ? write(fd, p, size) : read(fd, p, size);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return false;
        p += done;
        size -= (size_t)done;
    }
    return true;
}

static esp_err_t transfer_payload(int fd, char *payload, size_t size, bool writing)
{
    if (size == 0) return ESP_OK;
    /* SD 直接读 PSRAM 会退化为逐扇区命令；这份临时内部缓冲在 I/O 后立即归还。 */
    uint8_t *chunk = heap_caps_malloc(IO_CHUNK,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!chunk) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_OK;
    while (size > 0) {
        size_t want = size < IO_CHUNK ? size : IO_CHUNK;
        if (writing) memcpy(chunk, payload, want);
        if (!io_exact(fd, chunk, want, writing)) {
            result = ESP_FAIL;
            break;
        }
        if (!writing) memcpy(payload, chunk, want);
        payload += want;
        size -= want;
    }
    free(chunk);
    return result;
}

static int compare_paths(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static esp_err_t index_paths(favorites_t *favorites)
{
    size_t offset = 0;
    for (size_t i = 0; i < favorites->count; i++) {
        if (offset >= favorites->size) return ESP_ERR_INVALID_RESPONSE;
        char *path = favorites->payload + offset;
        char *end = memchr(path, '\0', favorites->size - offset);
        if (!end || !valid_path(path, (size_t)(end - path))) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        favorites->paths[i] = path;
        offset += (size_t)(end - path) + 1;
    }
    if (offset != favorites->size) return ESP_ERR_INVALID_RESPONSE;
    if (favorites->count > 1) {
        qsort(favorites->paths, favorites->count, sizeof(char *), compare_paths);
        for (size_t i = 1; i < favorites->count; i++) {
            if (strcmp(favorites->paths[i - 1], favorites->paths[i]) == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    return ESP_OK;
}

static uint32_t payload_crc(const favorites_t *favorites)
{
    return favorites->size ? esp_crc32_le(0, (const uint8_t *)favorites->payload,
                                         favorites->size) : 0;
}

static esp_err_t load_file(const char *path, favorites_t *favorites)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;

    favorites_header_t header;
    struct stat st;
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(header) ||
        !io_exact(fd, &header, sizeof(header), false)) goto done;
    if (header.magic != FAVORITES_MAGIC || header.version != FAVORITES_VERSION ||
        header.header_size != sizeof(header) ||
        (uint64_t)sizeof(header) + header.payload_size != (uint64_t)st.st_size ||
        header.count > header.payload_size / (sizeof(SD_MOUNT_POINT "/") + 1)) {
        goto done;
    }
    result = alloc_favorites(favorites, header.count, header.payload_size);
    if (result != ESP_OK) goto done;
    result = transfer_payload(fd, favorites->payload, favorites->size, false);
    if (result != ESP_OK) goto done;
    if (payload_crc(favorites) != header.payload_crc32) {
        result = ESP_ERR_INVALID_CRC;
        goto done;
    }
    result = index_paths(favorites);

done:
    if (close(fd) != 0 && result == ESP_OK) result = ESP_FAIL;
    if (result != ESP_OK) free_favorites(favorites);
    return result;
}

esp_err_t rom_favorites_init(void)
{
    if (s_initialized) return ESP_OK;
    if (!sd_card_mounted()) return ESP_ERR_INVALID_STATE;

    esp_err_t primary = load_file(FAVORITES_PATH, &s_favorites);
    if (primary == ESP_OK) {
        s_primary_valid = true;
    } else {
        /* 内存不够并不说明主文件坏了；改用更小的旧备份会静默丢掉新收藏。 */
        if (primary == ESP_ERR_NO_MEM) return primary;
        esp_err_t backup = load_file(FAVORITES_BACKUP, &s_favorites);
        if (backup == ESP_OK) {
            s_primary_valid = false;
            ESP_LOGW(TAG, "主收藏文件不可用，从备份恢复 %u 项",
                     (unsigned)s_favorites.count);
        } else if (primary == ESP_ERR_NOT_FOUND && backup == ESP_ERR_NOT_FOUND) {
            s_primary_valid = false;
        } else {
            ESP_LOGE(TAG, "收藏读取失败，保留原文件（主文件 %s，备份 %s）",
                     esp_err_to_name(primary), esp_err_to_name(backup));
            return backup == ESP_ERR_NO_MEM ? backup :
                   primary == ESP_ERR_NOT_FOUND ? backup : primary;
        }
    }
    s_initialized = true;
    ESP_LOGI(TAG, "收藏已载入：%u 项", (unsigned)s_favorites.count);
    return ESP_OK;
}

static size_t lower_bound(const char *path)
{
    size_t first = 0, last = s_favorites.count;
    while (first < last) {
        size_t middle = first + (last - first) / 2;
        if (strcmp(s_favorites.paths[middle], path) < 0) first = middle + 1;
        else last = middle;
    }
    return first;
}

bool rom_favorites_contains(const rom_store_entry_t *entry)
{
    if (!s_initialized || !entry || !entry->path) return false;
    size_t position = lower_bound(entry->path);
    return position < s_favorites.count &&
           strcmp(s_favorites.paths[position], entry->path) == 0;
}

static esp_err_t save_file(favorites_t *favorites)
{
    favorites_header_t header = {
        .magic = FAVORITES_MAGIC,
        .version = FAVORITES_VERSION,
        .header_size = sizeof(header),
        .count = (uint32_t)favorites->count,
        .payload_size = (uint32_t)favorites->size,
        .payload_crc32 = payload_crc(favorites),
    };
    int fd = open(FAVORITES_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return ESP_FAIL;
    esp_err_t result = io_exact(fd, &header, sizeof(header), true) ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK) {
        result = transfer_payload(fd, favorites->payload, favorites->size, true);
    }
    if (result == ESP_OK && fsync(fd) != 0) result = ESP_FAIL;
    if (close(fd) != 0) result = ESP_FAIL;
    if (result != ESP_OK) goto failed;

    bool rotated = false;
    if (s_primary_valid) {
        if (unlink(FAVORITES_BACKUP) != 0 && errno != ENOENT) goto failed;
        if (rename(FAVORITES_PATH, FAVORITES_BACKUP) != 0) goto failed;
        s_primary_valid = false;
        rotated = true;
    } else {
        /* 主文件损坏或上次在两次 rename 之间断电时，唯一可信的旧状态在 .bak。
         * 只移除坏主文件，始终保留备份直到新文件完成提交。 */
        if (unlink(FAVORITES_PATH) != 0 && errno != ENOENT) goto failed;
    }
    if (rename(FAVORITES_TEMP, FAVORITES_PATH) != 0) {
        if (rotated && rename(FAVORITES_BACKUP, FAVORITES_PATH) == 0) {
            s_primary_valid = true;
        }
        goto failed;
    }
    s_primary_valid = true;
    /* 留着上一版备份，主文件日后 CRC 损坏时仍有恢复机会。 */
    return ESP_OK;

failed:
    unlink(FAVORITES_TEMP);
    return result == ESP_OK ? ESP_FAIL : result;
}

esp_err_t rom_favorites_toggle(const rom_store_entry_t *entry)
{
    if (!entry || !entry->path) return ESP_ERR_INVALID_ARG;
    size_t path_size = strnlen(entry->path, ROM_STORE_PATH_LEN);
    if (!valid_path(entry->path, path_size)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = rom_favorites_init();
    if (result != ESP_OK) return result;

    size_t position = lower_bound(entry->path);
    bool removing = position < s_favorites.count &&
                    strcmp(s_favorites.paths[position], entry->path) == 0;
    size_t added_size = path_size + 1;
    if (!removing && (s_favorites.count == UINT32_MAX ||
                     s_favorites.size > UINT32_MAX - added_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t count = removing ? s_favorites.count - 1 : s_favorites.count + 1;
    size_t size = removing ? s_favorites.size - added_size : s_favorites.size + added_size;
    favorites_t next;
    result = alloc_favorites(&next, count, size);
    if (result != ESP_OK) return result;

    /* 先构造完整新状态；写卡失败只丢弃 next，屏幕上的星标与已保存状态一致。 */
    char *dst = next.payload;
    size_t old_index = 0;
    for (size_t i = 0; i < count; i++) {
        const char *source;
        if (!removing && i == position) source = entry->path;
        else {
            if (removing && old_index == position) old_index++;
            source = s_favorites.paths[old_index++];
        }
        size_t length = strlen(source) + 1;
        next.paths[i] = dst;
        memcpy(dst, source, length);
        dst += length;
    }
    result = save_file(&next);
    if (result == ESP_OK) {
        free_favorites(&s_favorites);
        s_favorites = next;
    } else {
        free_favorites(&next);
        ESP_LOGE(TAG, "收藏保存失败，保留原收藏：%s", esp_err_to_name(result));
    }
    return result;
}

void rom_favorites_deinit(void)
{
    free_favorites(&s_favorites);
    s_initialized = false;
    s_primary_valid = false;
}
