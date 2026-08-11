#pragma once

#include "esp_err.h"

/* 24 kHz 能同时被 NTSC 60 fps 和 PAL 50 fps 整除：每帧恰好 400 / 480
 * 个采样，不会因整数截断每秒少送几十个采样而逐渐产生 I2S 欠载。 */
#define NES_AUDIO_SAMPLE_RATE       24000
#define NES_AUDIO_SAMPLES_PER_FRAME (NES_AUDIO_SAMPLE_RATE / 60)
#define NES_AUDIO_MAX_SAMPLES_PER_FRAME (NES_AUDIO_SAMPLE_RATE / 50)

/* 游戏装载后初始化 MAX98357 和音频消费任务。失败时模拟器仍可静音运行。 */
esp_err_t audio_output_init(void);
