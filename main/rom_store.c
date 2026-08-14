/*
 * roms 分区的读取端
 *
 * 先把分区 mmap 进来并校验目录；用户选中游戏后，再把那一项按需解压到 PSRAM。
 * 校验写得比较啰嗦是有原因的 —— 目录里的 offset/size 来自 flash：没烧过分区时
 * 那片全是 0xFF，烧坏了或者版本不匹配时是任意值。一个没查边界的 offset
 * 就是一次越界读，表现为 LoadProhibited 崩溃或者更糟的静默乱码。
 *
 * 所有校验失败都只是让 rom_store_init() 返回 0，不 abort —— 选单是新功能，
 * 它坏了不该让整块板子玩不了游戏。调用方回退到编译期嵌入的 ROM。
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "rom_store.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miniz.h"

static const char *TAG = "romstore";

#define DEFLATE_MAGIC "GBOXDFL\0"
#define MAGIC         "GAMEBOX\0"
#define LEGACY_MAGIC  "NESROMS\0"
#define MAGIC_LEN   8
#define HEADER_LEN  12          /* magic[8] + count(u32) */
#define DEFLATE_ENTRY_LEN 64    /* name[40] + system/codec/off/stored/raw/crc */
#define ENTRY_LEN   52          /* 旧多平台镜像：name[40] + system + off + size */
#define LEGACY_ENTRY_LEN 48     /* 旧镜像：name[40] + offset + size */

/* 防止损坏的 flash 目录声明一个远超 PSRAM 的解压大小。当前最大卡带 DKC 是
 * 4 MiB；留到 8 MiB 既覆盖合理的 SNES 卡，也绝不会做失控的大分配。 */
#define ROM_MAX_SIZE (8u * 1024u * 1024u)

/* iNES 文件的下限：16 字节头 + 至少一个 16 KB PRG bank。
 * 比这还小的一定不是能跑的卡。 */
#define NES_ROM_MIN_SIZE  (16 + 16 * 1024)
#define GB_ROM_MIN_SIZE   0x4000

/* SNES 卡带最小 128 KB，且内部头必须整个落在 ROM 里（LoROM 在 0x7FC0）。 */
#define SNES_ROM_MIN_SIZE 0x20000
#define SNES_LOROM_HEADER 0x7FC0
#define SNES_HIROM_HEADER 0xFFC0
#define GENESIS_ROM_MIN_SIZE 0x200

/* SNES 没有 magic。业界通行的判据是内部头里那对校验和：
 * checksum ^ complement 必须等于 0xFFFF。再要求标题是可打印 ASCII，
 * 基本不会把随机数据认成卡带。LoROM / HiROM 各试一次。 */
static bool snes_header_ok(const uint8_t *rom, size_t size, size_t base)
{
    if (base + 0x20 > size) return false;

    for (int i = 0; i < 21; i++) {
        uint8_t c = rom[base + i];
        if (c != 0 && (c < 0x20 || c > 0x7E)) return false;
    }

    uint32_t comp = (uint32_t)rom[base + 0x1C] | ((uint32_t)rom[base + 0x1D] << 8);
    uint32_t ck   = (uint32_t)rom[base + 0x1E] | ((uint32_t)rom[base + 0x1F] << 8);
    return (ck ^ comp) == 0xFFFF;
}

static bool rom_header_ok(rom_system_t system, const uint8_t *rom, size_t size)
{
    static const uint8_t gb_logo_head[4] = {0xCE, 0xED, 0x66, 0x66};

    if (system == ROM_SYSTEM_NES) {
        return size >= NES_ROM_MIN_SIZE && memcmp(rom, "NES\x1a", 4) == 0;
    }
    if (system == ROM_SYSTEM_SNES) {
        return snes_header_ok(rom, size, SNES_LOROM_HEADER) ||
               snes_header_ok(rom, size, SNES_HIROM_HEADER);
    }
    if (system == ROM_SYSTEM_GENESIS) {
        /* 标准卡带头 0x100 起始处是 `SEGA ...`。SMD 交错格式不在打包阶段支持，
         * 避免把桌面模拟器能猜出来的任意 .bin 误烧进设备。 */
        return size >= GENESIS_ROM_MIN_SIZE && memcmp(rom + 0x100, "SEGA", 4) == 0;
    }
    return size >= 0x150 && memcmp(rom + 0x104, gb_logo_head, 4) == 0;
}

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

    /* 整个分区映射进来。ESP32-S3 的 flash mmap 窗口足够容纳当前 13 MB 分区。
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
    bool deflate = memcmp(img, DEFLATE_MAGIC, MAGIC_LEN) == 0;
    bool legacy_multi = memcmp(img, MAGIC, MAGIC_LEN) == 0;
    bool legacy_nes = memcmp(img, LEGACY_MAGIC, MAGIC_LEN) == 0;
    if (!deflate && !legacy_multi && !legacy_nes) {
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
    size_t entry_len = deflate ? DEFLATE_ENTRY_LEN
                     : legacy_nes ? LEGACY_ENTRY_LEN : ENTRY_LEN;
    size_t dir_end = HEADER_LEN + (size_t)count * entry_len;
    if (dir_end > part->size) {
        ESP_LOGE(TAG, "目录表超出分区大小");
        return 0;
    }

    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = img + HEADER_LEN + (size_t)i * entry_len;
        rom_system_t system = legacy_nes
                                  ? ROM_SYSTEM_NES
                                  : (rom_system_t)rd32(e + ROM_STORE_NAME_LEN);
        uint32_t codec = deflate ? rd32(e + ROM_STORE_NAME_LEN + 4)
                                 : ROM_CODEC_RAW;
        size_t value_off = ROM_STORE_NAME_LEN + (legacy_nes ? 0 : 4);
        if (deflate) value_off += 4;
        uint32_t off = rd32(e + value_off);
        uint32_t stored_size = rd32(e + value_off + 4);
        uint32_t raw_size = deflate ? rd32(e + value_off + 8) : stored_size;
        uint32_t raw_crc = deflate ? rd32(e + value_off + 12) : 0;

        /* 每一条都验：数据落在分区内、不和目录表重叠、大小像个 ROM。
         * off + size 用 64 位算，避免 32 位回绕把越界算成合法。 */
        size_t min_size = system == ROM_SYSTEM_NES     ? NES_ROM_MIN_SIZE
                        : system == ROM_SYSTEM_SNES    ? SNES_ROM_MIN_SIZE
                        : system == ROM_SYSTEM_GENESIS ? GENESIS_ROM_MIN_SIZE
                                                       : GB_ROM_MIN_SIZE;
        if ((system != ROM_SYSTEM_NES && system != ROM_SYSTEM_GB &&
             system != ROM_SYSTEM_GBC && system != ROM_SYSTEM_SNES &&
             system != ROM_SYSTEM_GENESIS) ||
            (codec != ROM_CODEC_RAW && codec != ROM_CODEC_DEFLATE) ||
            (uint64_t)off + stored_size > part->size || off < dir_end ||
            stored_size == 0 || raw_size < min_size || raw_size > ROM_MAX_SIZE ||
            (codec == ROM_CODEC_RAW && stored_size != raw_size)) {
            ESP_LOGW(TAG,
                     "第 %u 条越界或大小异常（off=%u stored=%u raw=%u codec=%u），跳过",
                     (unsigned)i, (unsigned)off, (unsigned)stored_size,
                     (unsigned)raw_size, (unsigned)codec);
            continue;
        }

        /* 名字必须是 NUL 结尾的 —— 后面要当 C 字符串用。
         * 打包脚本保证了这点，但 flash 内容不可信。 */
        const char *name = (const char *)e;
        if (memchr(name, '\0', ROM_STORE_NAME_LEN) == NULL) {
            ESP_LOGW(TAG, "第 %u 条的名字没有结尾符，跳过", (unsigned)i);
            continue;
        }

        /* 原样条目可以现在验头；压缩条目必须等用户选中并解压后再验。 */
        if (codec == ROM_CODEC_RAW &&
            !rom_header_ok(system, img + off, raw_size)) {
            ESP_LOGW(TAG, "第 %u 条（%s）ROM 头无效，跳过", (unsigned)i, name);
            continue;
        }

        s_entries[n].name = name;
        s_entries[n].data = img + off;
        s_entries[n].size = raw_size;
        s_entries[n].stored_size = stored_size;
        s_entries[n].crc32 = raw_crc;
        s_entries[n].crc_valid = deflate;
        s_entries[n].system = system;
        s_entries[n].codec = (rom_codec_t)codec;
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

esp_err_t rom_store_load(const rom_store_entry_t *entry, size_t extra_bytes,
                         rom_store_image_t *out)
{
    if (!entry || !out || entry->size > SIZE_MAX - extra_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    uint8_t *rom;
    bool owned = entry->codec == ROM_CODEC_DEFLATE || extra_bytes != 0;
    int64_t t0 = esp_timer_get_time();

    if (!owned) {
        rom = (uint8_t *)entry->data;
    } else {
        rom = heap_caps_malloc(entry->size + extra_bytes,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rom) {
            ESP_LOGE(TAG, "%s 解压缓冲分配失败：需要 %u KB PSRAM",
                     entry->name, (unsigned)((entry->size + extra_bytes) / 1024));
            return ESP_ERR_NO_MEM;
        }

        if (entry->codec == ROM_CODEC_DEFLATE) {
            /* 不能调用便捷版 tinfl_decompress_mem_to_mem()：ESP-ROM 的实现会把
             * 约 11 KiB tinfl_decompressor 放在调用栈，而 main task 只有
             * 3584 字节，实物首次解 1 MiB GBC 时直接覆盖 task_wdt 链表并崩溃。
             * 状态显式放内部堆，ROM 例程只在栈上保留少量游标。 */
            tinfl_decompressor *decomp = heap_caps_calloc(
                1, sizeof(*decomp), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!decomp) {
                ESP_LOGE(TAG, "%s 解压状态分配失败：需要 %u 字节内部 RAM",
                         entry->name, (unsigned)sizeof(*decomp));
                free(rom);
                return ESP_ERR_NO_MEM;
            }
            tinfl_init(decomp);
            size_t in_bytes = entry->stored_size;
            size_t out_bytes = entry->size;
            tinfl_status status = tinfl_decompress(
                decomp, entry->data, &in_bytes, rom, rom, &out_bytes,
                TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            free(decomp);
            if (status != TINFL_STATUS_DONE || out_bytes != entry->size ||
                in_bytes != entry->stored_size) {
                ESP_LOGE(TAG,
                         "%s 解压失败：status=%d，输入 %u/%u，输出 %u/%u",
                         entry->name, status, (unsigned)in_bytes,
                         (unsigned)entry->stored_size, (unsigned)out_bytes,
                         (unsigned)entry->size);
                free(rom);
                return ESP_ERR_INVALID_CRC;
            }
        } else {
            memcpy(rom, entry->data, entry->size);
        }
        if (extra_bytes) memset(rom + entry->size, 0, extra_bytes);
    }

    if (!rom_header_ok(entry->system, rom, entry->size)) {
        ESP_LOGE(TAG, "%s 解压后 ROM 头无效", entry->name);
        if (owned) free(rom);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t crc = esp_crc32_le(0, rom, entry->size);
    if (entry->crc_valid && crc != entry->crc32) {
        ESP_LOGE(TAG, "%s CRC 错误：得到 %08" PRIx32 "，预期 %08" PRIx32,
                 entry->name, crc, entry->crc32);
        if (owned) free(rom);
        return ESP_ERR_INVALID_CRC;
    }

    out->data = rom;
    out->size = entry->size;
    out->crc32 = crc;
    out->owned = owned;
    if (entry->codec == ROM_CODEC_DEFLATE) {
        ESP_LOGI(TAG, "%s 已解压：%u -> %u KB，耗时 %lld ms",
                 entry->name, (unsigned)(entry->stored_size / 1024),
                 (unsigned)(entry->size / 1024),
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }
    return ESP_OK;
}

void rom_store_image_release(rom_store_image_t *image)
{
    if (!image) return;
    if (image->owned) free(image->data);
    memset(image, 0, sizeof(*image));
}
