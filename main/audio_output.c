/*
 * MAX98357 I2S 音频输出
 *
 * nofrendo 的 apu_emulate() 把一帧 PCM 写进自己的私有缓冲，却没有宿主回调。
 * 不改上游源码：链接时用 --wrap=apu_emulate 把那一次调用接到这里，仍由公开的
 * apu_process() 生成完全相同的 PCM，再交给独立 I2S 任务。这样以后可以直接
 * 覆盖 components/nofrendo 更新上游，不需要重打补丁。
 *
 * 核 0 的模拟线程只生成采样并向队列复制约 1.6 KB，绝不等 I2S。消费任务阻塞
 * 在 DMA 写入上；即使喇叭或驱动异常，也不会把 60 fps 主循环一起卡住。
 */

#include <stdint.h>
#include "audio_output.h"
#include "nes/nes.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define I2S_PIN_BCLK       4
#define I2S_PIN_LRC        5
#define I2S_PIN_DOUT       6
#define AUDIO_QUEUE_FRAMES 4
#define AUDIO_VOLUME_SHIFT 2       /* 除以 4：先从 25% 软件音量开始，避免突然过响 */

typedef struct {
    uint16_t sample_count;
    int16_t stereo[NES_AUDIO_MAX_SAMPLES_PER_FRAME * 2];
} audio_packet_t;

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_queue;
static int16_t s_mono[NES_AUDIO_MAX_SAMPLES_PER_FRAME];
static audio_packet_t s_producer_packet;
static uint32_t s_dropped;
static uint32_t s_write_errors;

/* 由链接器替换 nofrendo 的 apu_emulate() 调用。函数仍在模拟线程里运行，
 * 所以这里不能阻塞等待 I2S；队列满时宁可丢当前帧并计数。 */
void __wrap_apu_emulate(void)
{
    int refresh_rate = nes_getptr()->refresh_rate;
    int sample_count = NES_AUDIO_SAMPLE_RATE / refresh_rate;
    if (sample_count > NES_AUDIO_MAX_SAMPLES_PER_FRAME) {
        /* 目前 nofrendo 只会给 50/60 Hz；遇到意外制式时宁可截断，也不能写出缓冲。 */
        sample_count = NES_AUDIO_MAX_SAMPLES_PER_FRAME;
    }
    s_producer_packet.sample_count = sample_count;
    apu_process(s_mono, sample_count, false);

    for (int i = 0; i < sample_count; i++) {
        int16_t sample = s_mono[i] >> AUDIO_VOLUME_SHIFT;
        s_producer_packet.stereo[i * 2] = sample;
        s_producer_packet.stereo[i * 2 + 1] = sample;
    }

    if (s_queue && xQueueSend(s_queue, &s_producer_packet, 0) != pdTRUE) {
        s_dropped++;
    }
}

static void audio_task(void *arg)
{
    audio_packet_t packet;
    uint32_t frames_written = 0;

    while (1) {
        if (xQueueReceive(s_queue, &packet, portMAX_DELAY) != pdTRUE) continue;

        size_t bytes_written = 0;
        size_t packet_bytes = packet.sample_count * 2 * sizeof(int16_t);
        esp_err_t err = i2s_channel_write(s_tx, packet.stereo, packet_bytes,
                                          &bytes_written,
                                          pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_written != packet_bytes) {
            s_write_errors++;
            ESP_LOGW(TAG, "I2S 写入异常：%s，%u/%u 字节",
                     esp_err_to_name(err), (unsigned)bytes_written,
                     (unsigned)packet_bytes);
        }

        frames_written++;
        if (frames_written % 300 == 0) {
            ESP_LOGI(TAG, "I2S 24kHz：%u 帧，排队 %u，丢帧 %u，写错 %u",
                     (unsigned)frames_written,
                     (unsigned)uxQueueMessagesWaiting(s_queue),
                     (unsigned)s_dropped, (unsigned)s_write_errors);
        }
    }
}

esp_err_t audio_output_init(void)
{
    if (s_tx) return ESP_OK;

    s_queue = xQueueCreate(AUDIO_QUEUE_FRAMES, sizeof(audio_packet_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = NES_AUDIO_MAX_SAMPLES_PER_FRAME;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) goto fail_queue;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(NES_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCLK,
            .ws = I2S_PIN_LRC,
            .dout = I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) goto fail_channel;
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) goto fail_channel;

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_task, "nes_audio", 4096, NULL, 3, NULL, 0);
    if (created != pdPASS) {
        err = ESP_ERR_NO_MEM;
        i2s_channel_disable(s_tx);
        goto fail_channel;
    }

    ESP_LOGI(TAG,
             "MAX98357 就绪：24kHz/16-bit，BCLK=%d LRC=%d DIN=%d，音量 25%%",
             I2S_PIN_BCLK, I2S_PIN_LRC, I2S_PIN_DOUT);
    return ESP_OK;

fail_channel:
    i2s_del_channel(s_tx);
    s_tx = NULL;
fail_queue:
    vQueueDelete(s_queue);
    s_queue = NULL;
    return err;
}
