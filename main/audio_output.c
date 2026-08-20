/*
 * MAX98357 I2S 音频输出
 *
 * 所有模拟器共用的非阻塞宿主后端。nofrendo 没有音频回调，所以仍用
 * --wrap=apu_emulate 接出 PCM；gnuboy 则直接调用 audio_output_submit_stereo()。
 *
 * 模拟线程只生成采样并向队列复制，绝不等 I2S。消费任务留在核 0 的低优先级；
 * 核 1 同时承担 LCD 和 Genesis 声音，实机把 I2S 也迁过去会让推屏从 15 ms
 * 恶化到 21 ms，并开始丢音频包。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
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
#define AUDIO_FADE_MS      20      /* 音量变化时缓变，避免 MAX98357 突然跳变发出爆音 */
#define AUDIO_GAIN_MAX_Q15 ((32768 * 9) / 10)  /* 满量程留 10% 余量防削波，档位 100% 封顶在这 */
#define AUDIO_VOLUME_DEFAULT 50    /* 开机默认档位（0~100，10 的整数倍） */

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
static atomic_int s_volume = ATOMIC_VAR_INIT(AUDIO_VOLUME_DEFAULT);

/* 供 rgb_led.c 轮询的音量联动峰值：audio_task() 按已经过档位缩放的采样
 * （不是模拟器产出的原始 PCM）记录峰值，静音或调低音量时灯自然跟着暗，
 * 不会出现"听不见但灯还在闪"的错位。take 语义（读后清零）让每次轮询
 * 拿到的是"上次轮询以来"的峰值，不会被同一窗口内的旧值覆盖漏掉瞬态。 */
static atomic_int s_recent_peak = ATOMIC_VAR_INIT(0);

esp_err_t audio_output_settings_init(void)
{
    /* 音量档位只服务于当前菜单选择，不再读写 NVS。这样即使用户调低或静音，
     * 按 RST 换游戏或重新上电后也一定从默认档位开始。 */
    atomic_store_explicit(&s_volume, AUDIO_VOLUME_DEFAULT, memory_order_relaxed);
    ESP_LOGI(TAG, "音量默认：%d%%（仅本次开机有效）", AUDIO_VOLUME_DEFAULT);
    return ESP_OK;
}

int audio_output_get_volume(void)
{
    return atomic_load_explicit(&s_volume, memory_order_relaxed);
}

esp_err_t audio_output_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    atomic_store_explicit(&s_volume, percent, memory_order_relaxed);
    ESP_LOGI(TAG, "音量：%d%%（仅本次开机有效）", percent);
    return ESP_OK;
}

int audio_output_take_peak(void)
{
    return atomic_exchange_explicit(&s_recent_peak, 0, memory_order_relaxed);
}

/* 原样透传满量程 PCM——不再像早期那样固定 >>2 留 25% 天花板。缩放全部
 * 交给 audio_task() 里的 gain_q15，档位 100% 封顶在 AUDIO_GAIN_MAX_Q15
 * （满量程的 90%，留 10% 余量防削波）。这段还没在新喇叭上测过高档位
 * 会不会削波/失真，听到破音就把默认档位往下调，或者把这个余量再调大。 */
void audio_output_submit_stereo(const int16_t *samples, size_t frame_count)
{
    if (!s_queue || !samples || frame_count == 0) return;

    /* 超过一个包的量拆开排队，不再截断。SNES 跑不满 60 fps 时按墙钟补的
     * 采样数会超过 AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET（见 snes_emu.c），
     * 旧的截断写法会把多出来的那截静默丢掉 —— 正好是最难查的那类
     * “声音有点不对”。
     * ⚠ samples 不能是 s_producer_packet.stereo 本身：拆包后第二轮的
     *   memcpy 就会自我重叠。__wrap_apu_emulate() 那条路径靠 NES 每帧
     *   不超过一个包来保证只循环一次。 */
    while (frame_count > 0) {
        size_t chunk = frame_count > AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET
                     ? AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET : frame_count;

        s_producer_packet.sample_count = chunk;
        memcpy(s_producer_packet.stereo, samples, chunk * 2 * sizeof(int16_t));

        if (xQueueSend(s_queue, &s_producer_packet, 0) != pdTRUE) {
            s_dropped++;
        }
        samples     += chunk * 2;
        frame_count -= chunk;
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
    int32_t gain_q15 = AUDIO_GAIN_MAX_Q15 * audio_output_get_volume() / 100;
    uint32_t fade_frames = (s_sample_rate * AUDIO_FADE_MS + 999) / 1000;
    int32_t fade_step = (AUDIO_GAIN_MAX_Q15 + (int32_t)fade_frames - 1) / (int32_t)fade_frames;

    while (1) {
        if (xQueueReceive(s_queue, &packet, portMAX_DELAY) != pdTRUE) continue;

        /* 不停 I2S、不停队列：静音仍持续送零采样，因此恢复时不用重建 DMA，
         * 也不会让模拟线程因为队列状态变化而丢帧。20ms 线性淡变只在消费侧
         * 修改栈上的包，NES/GB/GBC 的生产路径完全不用分叉。 */
        int32_t target_gain = AUDIO_GAIN_MAX_Q15 * audio_output_get_volume() / 100;
        size_t packet_bytes = packet.sample_count * 2 * sizeof(int16_t);
        if (gain_q15 == 0 && target_gain == 0) {
            /* 稳定静音是常态路径，整包清零比每个采样做乘法快得多。 */
            memset(packet.stereo, 0, packet_bytes);
        } else {
            /* AUDIO_GAIN_MAX_Q15 不是真正的酉增益（90% 满量程），所以
             * "稳定不淡变就跳过缩放"这条快捷路径已经不安全——之前直接
             * 判等 32768 的写法在满量程封顶改成 90% 后会漏乘，把没缩放
             * 过的原始采样直接送 I2S。改成每个采样都过一遍，顺便把缩放
             * 后（也就是耳朵真正听到）的峰值记下来供 LED 联动读取。 */
            int32_t peak = 0;
            for (uint16_t i = 0; i < packet.sample_count; i++) {
                if (gain_q15 < target_gain) {
                    gain_q15 += fade_step;
                    if (gain_q15 > target_gain) gain_q15 = target_gain;
                } else if (gain_q15 > target_gain) {
                    gain_q15 -= fade_step;
                    if (gain_q15 < target_gain) gain_q15 = target_gain;
                }
                int16_t l = (int16_t)((int32_t)packet.stereo[i * 2] * gain_q15 / 32768);
                int16_t r = (int16_t)((int32_t)packet.stereo[i * 2 + 1] * gain_q15 / 32768);
                packet.stereo[i * 2] = l;
                packet.stereo[i * 2 + 1] = r;

                int32_t mag = l < 0 ? -(int32_t)l : l;
                if (mag > peak) peak = mag;
                mag = r < 0 ? -(int32_t)r : r;
                if (mag > peak) peak = mag;
            }
            if (peak > atomic_load_explicit(&s_recent_peak, memory_order_relaxed)) {
                atomic_store_explicit(&s_recent_peak, peak, memory_order_relaxed);
            }
        }

        size_t bytes_written = 0;
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
            ESP_LOGI(TAG,
                     "I2S %uHz：%u 帧，排队 %u，丢帧 %u，写错 %u，音量 %d%%，栈余 %uB",
                     (unsigned)s_sample_rate, (unsigned)frames_written,
                     (unsigned)uxQueueMessagesWaiting(s_queue),
                     (unsigned)s_dropped, (unsigned)s_write_errors,
                     audio_output_get_volume(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

esp_err_t audio_output_init(uint32_t sample_rate)
{
    if (s_tx) return ESP_OK;
    if (sample_rate == 0) return ESP_ERR_INVALID_ARG;
    if (audio_output_get_volume() == 0) {
        /* 档位只在开机选单里改，进入游戏后本局状态固定。0% 时连 I2S、DMA、
         * 队列和消费任务都不创建；各模拟器仍可推进自己的混音器状态，只把
         * PCM 丢掉。下次在菜单调到非零档位后重启游戏即可正常初始化输出。 */
        ESP_LOGI(TAG, "音量 0%%：不启动 MAX98357/I2S");
        return ESP_OK;
    }
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

    /* 包缓冲本身接近 2 KB，I2S 写入和带中文的诊断日志还会叠加调用栈。
     * 4096 字节在连续运行到约 900 包时实测触发栈溢出；6144 留出余量，
     * 同时用上面的 high-water mark 持续观察，而不是靠短时启动判断。 */
    BaseType_t created = xTaskCreatePinnedToCore(
        audio_task, "game_audio", 6144, NULL, 3, NULL, 0);
    if (created != pdPASS) {
        err = ESP_ERR_NO_MEM;
        i2s_channel_disable(s_tx);
        goto fail_channel;
    }

    ESP_LOGI(TAG,
             "MAX98357 就绪：%uHz/16-bit，BCLK=%d LRC=%d DIN=%d，音量 %d%%",
             (unsigned)sample_rate, I2S_PIN_BCLK, I2S_PIN_LRC, I2S_PIN_DOUT,
             audio_output_get_volume());
    return ESP_OK;

fail_channel:
    i2s_del_channel(s_tx);
    s_tx = NULL;
fail_queue:
    vQueueDelete(s_queue);
    s_queue = NULL;
    return err;
}
