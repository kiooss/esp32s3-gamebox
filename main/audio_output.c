/*
 * MAX98357 I2S 音频输出
 *
 * NES 和 GB/GBC 共用的非阻塞宿主后端。nofrendo 没有音频回调，所以仍用
 * --wrap=apu_emulate 接出 PCM；gnuboy 则直接调用 audio_output_submit_stereo()。
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
    int16_t stereo[AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET * 2];
} audio_packet_t;

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_queue;
static int16_t s_mono[NES_AUDIO_MAX_SAMPLES_PER_FRAME];
static audio_packet_t s_producer_packet;
static uint32_t s_dropped;
static uint32_t s_write_errors;
static uint32_t s_sample_rate;

void audio_output_submit_stereo(const int16_t *samples, size_t frame_count)
{
    if (!s_queue || !samples || frame_count == 0) return;
    if (frame_count > AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET) {
        frame_count = AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET;
    }

    s_producer_packet.sample_count = frame_count;
    for (size_t i = 0; i < frame_count * 2; i++) {
        s_producer_packet.stereo[i] = samples[i] >> AUDIO_VOLUME_SHIFT;
    }

    if (xQueueSend(s_queue, &s_producer_packet, 0) != pdTRUE) {
        s_dropped++;
    }
}

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
    apu_process(s_mono, sample_count, false);

    for (int i = 0; i < sample_count; i++) {
        int16_t sample = s_mono[i];
        s_producer_packet.stereo[i * 2] = sample;
        s_producer_packet.stereo[i * 2 + 1] = sample;
    }
    audio_output_submit_stereo(s_producer_packet.stereo, sample_count);
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
            ESP_LOGI(TAG, "I2S %uHz：%u 帧，排队 %u，丢帧 %u，写错 %u",
                     (unsigned)s_sample_rate, (unsigned)frames_written,
                     (unsigned)uxQueueMessagesWaiting(s_queue),
                     (unsigned)s_dropped, (unsigned)s_write_errors);
        }
    }
}

esp_err_t audio_output_init(uint32_t sample_rate)
{
    if (s_tx) return ESP_OK;
    if (sample_rate == 0) return ESP_ERR_INVALID_ARG;
    s_sample_rate = sample_rate;

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
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
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
        audio_task, "game_audio", 4096, NULL, 3, NULL, 0);
    if (created != pdPASS) {
        err = ESP_ERR_NO_MEM;
        i2s_channel_disable(s_tx);
        goto fail_channel;
    }

    ESP_LOGI(TAG,
             "MAX98357 就绪：%uHz/16-bit，BCLK=%d LRC=%d DIN=%d，音量 25%%",
             (unsigned)sample_rate, I2S_PIN_BCLK, I2S_PIN_LRC, I2S_PIN_DOUT);
    return ESP_OK;

fail_channel:
    i2s_del_channel(s_tx);
    s_tx = NULL;
fail_queue:
    vQueueDelete(s_queue);
    s_queue = NULL;
    return err;
}
