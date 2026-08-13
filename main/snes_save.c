/*
 * SNES 即时存档的持久化外壳
 *
 * Snes9x 核心已经有 S9xSaveState/S9xLoadState，但接口只认 stdio 文件。本项目
 * 的 ROM 分区为了 mmap 故意没有文件系统，所以在 Flash 尾部另划 960 KiB FAT，
 * 再套 ESP-IDF 的 wear levelling。这样不改上游核心，也不会让约 356.6 KiB 的
 * 整体快照反复磨同一组物理扇区。
 *
 * 两个槽交替写：先删目标槽元数据使它失效，保留上一槽；状态文件完整关闭并
 * 重新读取 CRC 通过后，最后才写元数据。保存过程中断电，启动时仍会选上一份
 * 序号较小但完整的存档。
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snes_save.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

/* snapshot.h 位于上游核心的私有 src/ 目录，组件只公开根目录头文件。
 * 这两个函数本来就是宿主入口，在适配层声明即可，不为它扩大整个私有 include 面。 */
bool S9xSaveState(const char *filename);
bool S9xLoadState(const char *filename);

static const char *TAG = "snes_save";

#define SAVE_PARTITION_LABEL  "snes_save"
#define SAVE_MOUNT_PATH       "/snes"
#define SAVE_SLOT_COUNT       2
#define SAVE_META_MAGIC       0x534D5753u  /* "SMWS" */
#define SAVE_FORMAT_VERSION   1u

static const char *const s_state_path[SAVE_SLOT_COUNT] = {
    SAVE_MOUNT_PATH "/smw0.sav",
    SAVE_MOUNT_PATH "/smw1.sav",
};
static const char *const s_meta_path[SAVE_SLOT_COUNT] = {
    /* 默认 FAT 配置只支持 8.3 文件名，扩展名必须不超过 3 字符。 */
    SAVE_MOUNT_PATH "/smw0.dat",
    SAVE_MOUNT_PATH "/smw1.dat",
};

typedef struct {
    uint32_t magic;
    uint32_t format_version;
    uint32_t rom_crc;
    uint32_t state_size;
    uint32_t state_crc;
    uint32_t sequence;
    uint32_t meta_crc;
} save_meta_t;

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_mounted;

static uint32_t meta_crc(const save_meta_t *meta)
{
    return esp_crc32_le(0, (const uint8_t *)meta,
                        offsetof(save_meta_t, meta_crc));
}

static bool read_meta(int slot, uint32_t rom_crc, save_meta_t *meta)
{
    FILE *fp = fopen(s_meta_path[slot], "rb");
    if (!fp) return false;
    bool ok = fread(meta, sizeof(*meta), 1, fp) == 1;
    fclose(fp);

    if (!ok || meta->magic != SAVE_META_MAGIC ||
        meta->format_version != SAVE_FORMAT_VERSION ||
        meta->rom_crc != rom_crc || meta->state_size == 0 ||
        meta->meta_crc != meta_crc(meta)) {
        return false;
    }

    struct stat st;
    return stat(s_state_path[slot], &st) == 0 &&
           st.st_size == (off_t)meta->state_size;
}

static bool file_crc(const char *path, uint32_t *size_out, uint32_t *crc_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    uint8_t buffer[1024];
    uint32_t size = 0;
    uint32_t crc = 0;
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        crc = esp_crc32_le(crc, buffer, got);
        size += got;
    }
    bool ok = !ferror(fp);
    fclose(fp);
    if (!ok) return false;

    *size_out = size;
    *crc_out = crc;
    return true;
}

static bool state_matches(const save_meta_t *meta, int slot)
{
    uint32_t size, crc;
    return file_crc(s_state_path[slot], &size, &crc) &&
           size == meta->state_size && crc == meta->state_crc;
}

esp_err_t snes_save_init(uint32_t rom_crc)
{
    if (s_mounted) return ESP_OK;

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 2,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        SAVE_MOUNT_PATH, SAVE_PARTITION_LABEL, &cfg, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载即时存档分区失败：%s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(SAVE_MOUNT_PATH, &total, &free) == ESP_OK) {
        ESP_LOGI(TAG, "SMW 即时存档就绪：FAT %u KB，可用 %u KB，ROM CRC %08" PRIx32,
                 (unsigned)(total / 1024), (unsigned)(free / 1024), rom_crc);
    }
    return ESP_OK;
}

bool snes_save_load_latest(uint32_t rom_crc)
{
    if (!s_mounted) return false;

    save_meta_t meta[SAVE_SLOT_COUNT];
    bool valid[SAVE_SLOT_COUNT];
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        valid[i] = read_meta(i, rom_crc, &meta[i]);
    }

    int first = -1, second = -1;
    if (valid[0] && valid[1]) {
        first = meta[1].sequence > meta[0].sequence ? 1 : 0;
        second = 1 - first;
    } else if (valid[0]) {
        first = 0;
    } else if (valid[1]) {
        first = 1;
    } else {
        ESP_LOGI(TAG, "没有与当前 SMW ROM 匹配的即时存档");
        return false;
    }

    int candidates[2] = { first, second };
    for (int i = 0; i < 2 && candidates[i] >= 0; i++) {
        int slot = candidates[i];
        if (!state_matches(&meta[slot], slot)) {
            ESP_LOGW(TAG, "槽 %d 的 CRC 校验失败，尝试上一份", slot);
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        if (S9xLoadState(s_state_path[slot])) {
            ESP_LOGI(TAG, "已恢复 SMW 即时存档：槽 %d，序号 %" PRIu32
                     "，%" PRIu32 " 字节，耗时 %lld ms",
                     slot, meta[slot].sequence, meta[slot].state_size,
                     (esp_timer_get_time() - t0) / 1000);
            return true;
        }
        ESP_LOGW(TAG, "槽 %d 通过 CRC，但 Snes9x 拒绝加载", slot);
    }
    return false;
}

bool snes_save_write(uint32_t rom_crc)
{
    if (!s_mounted) return false;

    save_meta_t old_meta[SAVE_SLOT_COUNT];
    bool valid[SAVE_SLOT_COUNT];
    uint32_t newest_seq = 0;
    int newest_slot = -1;
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        valid[i] = read_meta(i, rom_crc, &old_meta[i]);
        if (valid[i] && (newest_slot < 0 || old_meta[i].sequence > newest_seq)) {
            newest_slot = i;
            newest_seq = old_meta[i].sequence;
        }
    }
    int target = newest_slot < 0 ? 0 : 1 - newest_slot;

    /* 元数据先失效；状态文件写坏也不会被下次启动误认成好档。 */
    unlink(s_meta_path[target]);
    int64_t t0 = esp_timer_get_time();
    if (!S9xSaveState(s_state_path[target])) {
        struct stat st;
        long size = stat(s_state_path[target], &st) == 0 ? (long)st.st_size : -1;
        ESP_LOGE(TAG, "Snes9x 写即时存档失败（槽 %d，文件 %ld 字节，预期 365120）",
                 target, size);
        return false;
    }

    save_meta_t meta = {
        .magic = SAVE_META_MAGIC,
        .format_version = SAVE_FORMAT_VERSION,
        .rom_crc = rom_crc,
        .sequence = newest_seq + 1,
    };
    if (!file_crc(s_state_path[target], &meta.state_size, &meta.state_crc)) {
        ESP_LOGE(TAG, "即时存档写完后无法校验（槽 %d）", target);
        return false;
    }
    meta.meta_crc = meta_crc(&meta);

    FILE *fp = fopen(s_meta_path[target], "wb");
    if (!fp) return false;
    bool ok = fwrite(&meta, sizeof(meta), 1, fp) == 1 && fflush(fp) == 0 &&
              fsync(fileno(fp)) == 0;
    fclose(fp);
    if (!ok) {
        unlink(s_meta_path[target]);
        ESP_LOGE(TAG, "即时存档元数据提交失败（槽 %d）", target);
        return false;
    }

    ESP_LOGI(TAG, "SMW 即时存档完成：槽 %d，序号 %" PRIu32
             "，%" PRIu32 " 字节，CRC %08" PRIx32 "，耗时 %lld ms",
             target, meta.sequence, meta.state_size, meta.state_crc,
             (esp_timer_get_time() - t0) / 1000);
    return true;
}
