#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 24 kHz 能同时被 NTSC 60 fps 和 PAL 50 fps 整除：每帧恰好 400 / 480
 * 个采样，不会因整数截断每秒少送几十个采样而逐渐产生 I2S 欠载。 */
#define AUDIO_OUTPUT_SAMPLE_RATE          24000
#define AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET (AUDIO_OUTPUT_SAMPLE_RATE / 50)

/* nofrendo 适配层仍沿用这些名字，避免把 NES 的制式知识泄漏进通用 I2S 后端。 */
#define NES_AUDIO_SAMPLE_RATE AUDIO_OUTPUT_SAMPLE_RATE
#define NES_AUDIO_SAMPLES_PER_FRAME (NES_AUDIO_SAMPLE_RATE / 60)
#define NES_AUDIO_MAX_SAMPLES_PER_FRAME AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET

/* 游戏装载后初始化 MAX98357 和音频消费任务。失败时模拟器仍可静音运行。 */
esp_err_t audio_output_init(uint32_t sample_rate);

/* 在开机选单前把声音重置为默认开启。开关只在本次运行中有效，不读写 NVS。 */
esp_err_t audio_output_settings_init(void);

/* 菜单里的声音开关；set 立即作用，但重启后一定恢复默认开启。 */
bool audio_output_is_muted(void);
esp_err_t audio_output_set_muted(bool muted);

/* 向 I2S 队列提交交错排列的立体声 S16 帧。只复制、不等待 DMA；队列满时
 * 丢当前包并记入诊断计数。NES 和 GB/GBC 共用这一条宿主接口。 */
void audio_output_submit_stereo(const int16_t *samples, size_t frame_count);
