/*
 * 从 flash 的 roms 分区里读游戏列表
 *
 * 分区内容是 tools/pack_roms.py 生成的镜像：一张定长目录表 + 各自独立压缩的 ROM。
 * 格式定义见那个脚本的顶部注释。
 *
 * 整个分区仍一次 mmap。原样条目可继续零拷贝；Deflate 条目只在用户选中后
 * 由 rom_store_load() 解到 PSRAM，不必为菜单或其他游戏占 RAM。
 *
 * 烧 ROM 分区：`idf.py flash-roms`（见顶层 CMakeLists.txt）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 目录里最多认这么多条。镜像里声明的数量超过它就当镜像坏了 ——
 * 这道上限是防御性的：count 是从 flash 读来的，没烧过分区时那片是 0xFF，
 * 会被解读成 40 亿条，拿去循环就跑飞了。 */
#define ROM_STORE_MAX      64

/* 显示名的缓冲长度，和打包脚本里的 NAME_LEN 必须一致。 */
#define ROM_STORE_NAME_LEN 40

typedef enum {
    ROM_SYSTEM_NES  = 1,
    ROM_SYSTEM_GB   = 2,
    ROM_SYSTEM_GBC  = 3,
    ROM_SYSTEM_SNES = 4,
} rom_system_t;

typedef enum {
    ROM_CODEC_RAW     = 0,
    ROM_CODEC_DEFLATE = 1,
} rom_codec_t;

typedef struct {
    const char    *name;        /* 显示名，指向 mmap 区域，NUL 结尾 */
    const uint8_t *data;        /* flash 中的原样或压缩数据 */
    size_t         size;        /* 解压后的 ROM 字节数 */
    size_t         stored_size; /* flash 中实际占用的字节数 */
    uint32_t       crc32;       /* 原始 ROM 的 CRC32 */
    bool           crc_valid;   /* 旧镜像没有保存 CRC，加载时现算 */
    rom_system_t   system;
    rom_codec_t    codec;
} rom_store_entry_t;

typedef struct {
    uint8_t  *data;
    size_t    size;
    uint32_t  crc32;
    bool      owned; /* true 时由调用方最终 free；false 时仍指向 flash mmap */
} rom_store_image_t;

/* 映射 roms 分区并校验目录。返回认到的游戏数，0 表示不可用
 * （没烧过分区、magic 不对、或者目录自相矛盾）—— 调用方应当回退到
 * 编译期嵌入的那个 ROM。可以反复调用，只有第一次真的做事。 */
int rom_store_init(void);

/* 第 i 个游戏（0 <= i < rom_store_init() 的返回值）。
 * 越界返回 NULL。返回的指针在整个运行期间有效（mmap 不解除）。 */
const rom_store_entry_t *rom_store_entry(int i);

/* 取得一份模拟器可直接使用的完整 ROM。Deflate 条目解到 PSRAM；原样条目在
 * extra_bytes == 0 时继续返回 mmap 指针。SNES 传入映射所需余量后会直接得到
 * 最终可写缓冲，避免 4 MiB ROM 同时保留两份而耗尽 8 MiB PSRAM。 */
esp_err_t rom_store_load(const rom_store_entry_t *entry, size_t extra_bytes,
                         rom_store_image_t *out);

/* 只释放 rom_store_load() 真正分配的缓冲；mmap 指针不会被误 free。 */
void rom_store_image_release(rom_store_image_t *image);
