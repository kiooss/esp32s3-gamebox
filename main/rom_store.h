/*
 * 从 flash 的 roms 分区里读游戏列表
 *
 * 分区内容是 tools/pack_roms.py 生成的镜像：一张定长目录表 + 拼接的 ROM 数据。
 * 格式定义见那个脚本的顶部注释。
 *
 * 整个分区一次 mmap 进地址空间，所以 rom_store_entry() 给出的 `data` 是个
 * 可以直接读的普通指针 —— 原样传给对应模拟器就行，走 flash
 * cache，零拷贝、不占 RAM。这也是当初不用 SPIFFS 的原因：文件系统不能 mmap，
 * 得把 ROM 整份读进内存，大卡 512 KB 只能落 PSRAM，比 flash cache 慢。
 *
 * 烧 ROM 分区：`idf.py flash-roms`（见顶层 CMakeLists.txt）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef struct {
    const char    *name;   /* 显示名，指向 mmap 区域，NUL 结尾 */
    const uint8_t *data;   /* ROM 首字节 */
    size_t         size;
    rom_system_t   system;
} rom_store_entry_t;

/* 映射 roms 分区并校验目录。返回认到的游戏数，0 表示不可用
 * （没烧过分区、magic 不对、或者目录自相矛盾）—— 调用方应当回退到
 * 编译期嵌入的那个 ROM。可以反复调用，只有第一次真的做事。 */
int rom_store_init(void);

/* 第 i 个游戏（0 <= i < rom_store_init() 的返回值）。
 * 越界返回 NULL。返回的指针在整个运行期间有效（mmap 不解除）。 */
const rom_store_entry_t *rom_store_entry(int i);
