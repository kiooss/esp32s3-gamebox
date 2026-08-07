/*
 * roms 分区的读取端
 *
 * 只做两件事：把分区 mmap 进来，校验目录表。校验写得比较啰嗦是有原因的 ——
 * 目录里的 offset/size 会被直接当指针用，而这些数字来自 flash：没烧过分区时
 * 那片全是 0xFF，烧坏了或者版本不匹配时是任意值。一个没查边界的 offset
 * 就是一次越界读，表现为 LoadProhibited 崩溃或者更糟的静默乱码。
 *
 * 所有校验失败都只是让 rom_store_init() 返回 0，不 abort —— 选单是新功能，
 * 它坏了不该让整块板子玩不了游戏。调用方回退到编译期嵌入的 ROM。
 */

#include <string.h>
#include "rom_store.h"
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "romstore";

#define MAGIC       "NESROMS\0"
#define MAGIC_LEN   8
#define HEADER_LEN  12          /* magic[8] + count(u32) */
#define ENTRY_LEN   48          /* name[40] + offset(u32) + size(u32) */

/* iNES 文件的下限：16 字节头 + 至少一个 16 KB PRG bank。
 * 比这还小的一定不是能跑的卡。 */
#define ROM_MIN_SIZE  (16 + 16 * 1024)

static rom_store_entry_t s_entries[ROM_STORE_MAX];
static int  s_count = -1;       /* -1 = 还没试过 */

/* 小端读一个 u32。镜像是小端，ESP32 也是小端，但显式读避免对齐假设 ——
 * 目录项是 48 字节对齐的，u32 字段落在 4 字节边界上，其实直接解引用也行，
 * 不过写成这样就不用在意格式以后会不会变。 */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rom_store_init(void)
{
    if (s_count >= 0) return s_count;    /* 已经试过了 */
    s_count = 0;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "roms");
    if (!part) {
        ESP_LOGW(TAG, "找不到 roms 分区（分区表是旧的？）");
        return 0;
    }

    /* 整个分区映射进来。ESP32-S3 的 flash mmap 窗口足够大，4 MB 没问题。
     * 不解除映射：ROM 指针要在整个运行期间一直有效。 */
    const void *base = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &base, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "roms 分区映射失败: %s", esp_err_to_name(err));
        return 0;
    }

    const uint8_t *img = base;
    if (memcmp(img, MAGIC, MAGIC_LEN) != 0) {
        ESP_LOGW(TAG, "roms 分区里没有镜像 —— 跑一次 `idf.py flash-roms`");
        return 0;
    }

    uint32_t count = rd32(img + MAGIC_LEN);
    if (count == 0 || count > ROM_STORE_MAX) {
        ESP_LOGE(TAG, "目录声明了 %u 条，超出 1~%d 的合理范围，镜像可能坏了",
                 (unsigned)count, ROM_STORE_MAX);
        return 0;
    }

    /* 目录表本身得落在分区内 */
    size_t dir_end = HEADER_LEN + (size_t)count * ENTRY_LEN;
    if (dir_end > part->size) {
        ESP_LOGE(TAG, "目录表超出分区大小");
        return 0;
    }

    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = img + HEADER_LEN + (size_t)i * ENTRY_LEN;
        uint32_t off  = rd32(e + ROM_STORE_NAME_LEN);
        uint32_t size = rd32(e + ROM_STORE_NAME_LEN + 4);

        /* 每一条都验：数据落在分区内、不和目录表重叠、大小像个 ROM。
         * off + size 用 64 位算，避免 32 位回绕把越界算成合法。 */
        if ((uint64_t)off + size > part->size || off < dir_end ||
            size < ROM_MIN_SIZE) {
            ESP_LOGW(TAG, "第 %u 条越界或过小（off=%u size=%u），跳过",
                     (unsigned)i, (unsigned)off, (unsigned)size);
            continue;
        }

        /* 名字必须是 NUL 结尾的 —— 后面要当 C 字符串用。
         * 打包脚本保证了这点，但 flash 内容不可信。 */
        const char *name = (const char *)e;
        if (memchr(name, '\0', ROM_STORE_NAME_LEN) == NULL) {
            ESP_LOGW(TAG, "第 %u 条的名字没有结尾符，跳过", (unsigned)i);
            continue;
        }

        /* 顺手确认是 iNES。坏文件在打包时就该挡掉，这里是第二道 ——
         * 让它在选单里就消失，而不是选中之后才崩。 */
        if (memcmp(img + off, "NES\x1a", 4) != 0) {
            ESP_LOGW(TAG, "第 %u 条（%s）不是 iNES 文件，跳过", (unsigned)i, name);
            continue;
        }

        s_entries[n].name = name;
        s_entries[n].data = img + off;
        s_entries[n].size = size;
        n++;
    }

    s_count = n;
    ESP_LOGI(TAG, "roms 分区：%d 个游戏可用", n);
    return n;
}

const rom_store_entry_t *rom_store_entry(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return &s_entries[i];
}
