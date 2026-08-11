/*
 * USB HID 游戏手柄
 *
 * 目标首先是包装上印着 Raspberry Pi 2/3 的廉价复古 USB 手柄。这类产品常见
 * DragonRise 0079:0006，也有换芯片但 HID 描述符仍标准的批次，所以不能只按
 * VID/PID 写死 8 字节结构。这里在设备接入时解析 Report Descriptor，找出
 * X/Y、Hat、D-pad 和 Button 的位偏移，再把报告归一成 GAMEPAD_BIT_*。
 *
 * USB 事件和 HID 报告都放在核 1 的低优先级任务；核 1 上优先级 5 的 LCD 推屏
 * 可以随时抢占它。模拟器核 0 每帧只读一个原子字节，不解析 USB、不等队列。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input_usb.h"
#include "input_gamepad.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"

static const char *TAG = "usb_pad";

/* 通用 SNES/RetroPie 手柄通常按 HID Button 1=B、2=A、9=SELECT、10=START。
 * 3/4 是 Y/X，也映成 B/A，让四个面键的两行都能直接玩双键游戏。 */
#define USB_BUTTON_B          1
#define USB_BUTTON_A          2
#define USB_BUTTON_B_ALT      3
#define USB_BUTTON_A_ALT      4
#define USB_BUTTON_SELECT     9
#define USB_BUTTON_START     10

#define USB_EVENT_QUEUE_LEN   4
#define USB_REPORT_MAX       64
#define HID_CANDIDATE_MAX     8
#define HID_LOCAL_USAGE_MAX  16

typedef struct {
    uint16_t bit_offset;
    uint8_t bit_size;
    int32_t logical_min;
    int32_t logical_max;
    bool valid;
} hid_field_t;

typedef struct {
    uint8_t report_id;
    uint16_t input_bits;
    hid_field_t x, y, hat;
    hid_field_t dpad_up, dpad_down, dpad_right, dpad_left;
    uint16_t button_offset;
    uint8_t button_size;
    uint8_t button_count;
    uint8_t button_usage_min;
    bool buttons_valid;
} hid_candidate_t;

typedef struct {
    hid_candidate_t report;
    bool valid;
    bool has_report_id;
} hid_layout_t;

typedef struct {
    uint32_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
} hid_globals_t;

typedef struct {
    uint32_t usages[HID_LOCAL_USAGE_MAX];
    uint8_t usage_count;
    uint32_t usage_min;
    bool has_usage_min;
} hid_locals_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} usb_device_event_t;

static atomic_uchar s_state = ATOMIC_VAR_INIT(0);
static atomic_bool s_connected = ATOMIC_VAR_INIT(false);
static QueueHandle_t s_device_events;
static hid_host_device_handle_t s_device;
static hid_layout_t s_layout;
static bool s_started;
static uint16_t s_vid, s_pid;
static uint8_t s_last_raw[USB_REPORT_MAX];
static size_t s_last_raw_len;
static unsigned s_raw_log_budget;

static uint32_t item_unsigned(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static int32_t item_signed(const uint8_t *p, int size)
{
    uint32_t v = item_unsigned(p, size);
    if (size > 0 && size < 4 && (v & (1U << (size * 8 - 1))))
        v |= ~((1U << (size * 8)) - 1U);
    return (int32_t)v;
}

static uint32_t bits_unsigned(const uint8_t *data, size_t len,
                              uint16_t offset, uint8_t size)
{
    if (size == 0 || size > 24 || (size_t)offset + size > len * 8) return 0;
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) {
        uint16_t bit = offset + i;
        if (data[bit / 8] & (1U << (bit % 8))) v |= 1U << i;
    }
    return v;
}

static int32_t field_value(const hid_field_t *f, const uint8_t *data, size_t len)
{
    uint32_t v = bits_unsigned(data, len, f->bit_offset, f->bit_size);
    if (f->logical_min < 0 && f->bit_size < 32 &&
        (v & (1U << (f->bit_size - 1))))
        v |= ~((1U << f->bit_size) - 1U);
    return (int32_t)v;
}

static hid_candidate_t *candidate_for(hid_candidate_t *all, int *count,
                                      uint8_t report_id)
{
    for (int i = 0; i < *count; i++)
        if (all[i].report_id == report_id) return &all[i];
    if (*count >= HID_CANDIDATE_MAX) return NULL;
    hid_candidate_t *c = &all[(*count)++];
    memset(c, 0, sizeof(*c));
    c->report_id = report_id;
    return c;
}

static uint32_t local_usage(const hid_locals_t *local, int index)
{
    if (index < local->usage_count) return local->usages[index];
    if (local->has_usage_min) return local->usage_min + (uint32_t)index;
    return 0;
}

static void remember_field(hid_field_t *field, uint16_t offset,
                           const hid_globals_t *g)
{
    if (field->valid) return;
    *field = (hid_field_t) {
        .bit_offset = offset,
        .bit_size = g->report_size,
        .logical_min = g->logical_min,
        .logical_max = g->logical_max,
        .valid = true,
    };
}

static int candidate_score(const hid_candidate_t *c)
{
    int score = 0;
    if (c->x.valid && c->y.valid) score += 4;
    if (c->hat.valid) score += 4;
    if (c->dpad_up.valid || c->dpad_down.valid ||
        c->dpad_left.valid || c->dpad_right.valid) score += 4;
    if (c->buttons_valid) score += 4;
    return score;
}

static bool parse_report_descriptor(const uint8_t *desc, size_t len,
                                    hid_layout_t *layout)
{
    hid_globals_t g = {0};
    hid_locals_t local = {0};
    hid_candidate_t all[HID_CANDIDATE_MAX] = {0};
    int count = 0;
    bool has_report_id = false;

    candidate_for(all, &count, 0);
    for (size_t pos = 0; pos < len;) {
        uint8_t prefix = desc[pos++];
        if (prefix == 0xFE) {
            if (pos + 2 > len) break;
            uint8_t long_size = desc[pos];
            pos += 2 + long_size;
            if (pos > len) break;
            continue;
        }

        int size_code = prefix & 0x03;
        int size = size_code == 3 ? 4 : size_code;
        int type = (prefix >> 2) & 0x03;
        int tag = (prefix >> 4) & 0x0F;
        if (pos + (size_t)size > len) break;
        const uint8_t *value = &desc[pos];
        uint32_t u = item_unsigned(value, size);
        pos += size;

        if (type == 1) {                 /* Global item */
            switch (tag) {
            case 0: g.usage_page = u; break;
            case 1: g.logical_min = item_signed(value, size); break;
            case 2: g.logical_max = g.logical_min < 0
                                      ? item_signed(value, size) : (int32_t)u; break;
            case 7: g.report_size = (uint8_t)u; break;
            case 8:
                g.report_id = (uint8_t)u;
                has_report_id = true;
                candidate_for(all, &count, g.report_id);
                break;
            case 9: g.report_count = (uint8_t)u; break;
            default: break;
            }
            continue;
        }

        if (type == 2) {                 /* Local item */
            if (tag == 0 && local.usage_count < HID_LOCAL_USAGE_MAX)
                local.usages[local.usage_count++] = u;
            else if (tag == 1) {
                local.usage_min = u;
                local.has_usage_min = true;
            }
            continue;
        }

        if (type != 0) continue;
        hid_candidate_t *c = candidate_for(all, &count, g.report_id);
        if (!c) return false;

        if (tag == 8) {                  /* Input main item */
            bool constant = (u & 0x01) != 0;
            bool variable = (u & 0x02) != 0;
            if (!constant && variable && g.report_size && g.report_count) {
                for (int i = 0; i < g.report_count; i++) {
                    uint32_t usage = local_usage(&local, i);
                    uint32_t page = usage > 0xFFFF ? usage >> 16 : g.usage_page;
                    usage &= 0xFFFF;
                    uint16_t bit = c->input_bits + i * g.report_size;
                    if (page == 0x01) {
                        if (usage == 0x30) remember_field(&c->x, bit, &g);
                        if (usage == 0x31) remember_field(&c->y, bit, &g);
                        if (usage == 0x39) remember_field(&c->hat, bit, &g);
                        if (usage == 0x90) remember_field(&c->dpad_up, bit, &g);
                        if (usage == 0x91) remember_field(&c->dpad_down, bit, &g);
                        if (usage == 0x92) remember_field(&c->dpad_right, bit, &g);
                        if (usage == 0x93) remember_field(&c->dpad_left, bit, &g);
                    } else if (page == 0x09 && !c->buttons_valid) {
                        c->button_offset = bit;
                        c->button_size = g.report_size;
                        c->button_count = g.report_count;
                        c->button_usage_min = usage ? (uint8_t)usage : 1;
                        c->buttons_valid = true;
                    }
                }
            }
            c->input_bits += (uint16_t)g.report_size * g.report_count;
        }

        /* Local 状态只对紧接着的一个 Main item 有效。 */
        memset(&local, 0, sizeof(local));
    }

    int best = -1, best_score = 0;
    for (int i = 0; i < count; i++) {
        int score = candidate_score(&all[i]);
        if (score > best_score) { best = i; best_score = score; }
    }
    if (best < 0 || best_score < 8) return false; /* 至少方向和按键各一组 */
    layout->report = all[best];
    layout->has_report_id = has_report_id;
    layout->valid = true;
    return true;
}

static bool button_pressed(const hid_candidate_t *r, const uint8_t *data,
                           size_t len, uint8_t usage)
{
    if (!r->buttons_valid || usage < r->button_usage_min) return false;
    uint8_t index = usage - r->button_usage_min;
    if (index >= r->button_count) return false;
    uint16_t offset = r->button_offset + (uint16_t)index * r->button_size;
    return bits_unsigned(data, len, offset, r->button_size) != 0;
}

static void decode_axis(const hid_field_t *f, const uint8_t *data, size_t len,
                        uint8_t low_bit, uint8_t high_bit, uint8_t *state)
{
    if (!f->valid || f->logical_max <= f->logical_min) return;
    int32_t v = field_value(f, data, len);
    int32_t span = f->logical_max - f->logical_min;
    if (v < f->logical_min + span / 3) *state |= low_bit;
    if (v > f->logical_max - span / 3) *state |= high_bit;
}

static bool decode_report(const uint8_t *data, size_t len, uint8_t *state)
{
    if (!s_layout.valid || !data || len == 0) return false;
    const hid_candidate_t *r = &s_layout.report;
    if (s_layout.has_report_id) {
        if (data[0] != r->report_id) return false;
        data++;
        len--;
    }

    uint8_t out = 0;
    decode_axis(&r->x, data, len, GAMEPAD_BIT_LEFT, GAMEPAD_BIT_RIGHT, &out);
    decode_axis(&r->y, data, len, GAMEPAD_BIT_UP, GAMEPAD_BIT_DOWN, &out);

    if (r->hat.valid) {
        int32_t hat = field_value(&r->hat, data, len);
        if (r->hat.logical_min == 1) hat--;
        if (hat >= 0 && hat <= 7) {
            if (hat == 7 || hat <= 1) out |= GAMEPAD_BIT_UP;
            if (hat >= 1 && hat <= 3) out |= GAMEPAD_BIT_RIGHT;
            if (hat >= 3 && hat <= 5) out |= GAMEPAD_BIT_DOWN;
            if (hat >= 5 && hat <= 7) out |= GAMEPAD_BIT_LEFT;
        }
    }
    if (r->dpad_up.valid && field_value(&r->dpad_up, data, len)) out |= GAMEPAD_BIT_UP;
    if (r->dpad_down.valid && field_value(&r->dpad_down, data, len)) out |= GAMEPAD_BIT_DOWN;
    if (r->dpad_left.valid && field_value(&r->dpad_left, data, len)) out |= GAMEPAD_BIT_LEFT;
    if (r->dpad_right.valid && field_value(&r->dpad_right, data, len)) out |= GAMEPAD_BIT_RIGHT;

    if (button_pressed(r, data, len, USB_BUTTON_A) ||
        button_pressed(r, data, len, USB_BUTTON_A_ALT)) out |= GAMEPAD_BIT_A;
    if (button_pressed(r, data, len, USB_BUTTON_B) ||
        button_pressed(r, data, len, USB_BUTTON_B_ALT)) out |= GAMEPAD_BIT_B;
    if (button_pressed(r, data, len, USB_BUTTON_SELECT)) out |= GAMEPAD_BIT_SELECT;
    if (button_pressed(r, data, len, USB_BUTTON_START)) out |= GAMEPAD_BIT_START;
    *state = out;
    return true;
}

static void log_raw_report(const uint8_t *data, size_t len, uint8_t state)
{
    if (s_raw_log_budget == 0 || len > USB_REPORT_MAX) return;
    if (len == s_last_raw_len && memcmp(data, s_last_raw, len) == 0) return;
    memcpy(s_last_raw, data, len);
    s_last_raw_len = len;
    s_raw_log_budget--;

    char hex[USB_REPORT_MAX * 2 + 1];
    size_t shown = len < 16 ? len : 16;
    for (size_t i = 0; i < shown; i++) snprintf(&hex[i * 2], 3, "%02X", data[i]);
    hex[shown * 2] = '\0';
    ESP_LOGI(TAG, "%04X:%04X 报告[%u]=%s%s -> %c%c%c%c %c%c%c%c",
             s_vid, s_pid, (unsigned)len, hex, len > shown ? "..." : "",
             state & GAMEPAD_BIT_UP ? 'U' : '-', state & GAMEPAD_BIT_DOWN ? 'D' : '-',
             state & GAMEPAD_BIT_LEFT ? 'L' : '-', state & GAMEPAD_BIT_RIGHT ? 'R' : '-',
             state & GAMEPAD_BIT_A ? 'A' : '-', state & GAMEPAD_BIT_B ? 'B' : '-',
             state & GAMEPAD_BIT_SELECT ? 'S' : '-', state & GAMEPAD_BIT_START ? 'T' : '-');
}

static void interface_callback(hid_host_device_handle_t handle,
                               hid_host_interface_event_t event, void *arg)
{
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[USB_REPORT_MAX];
        size_t len = 0;
        esp_err_t err = hid_host_device_get_raw_input_report_data(
            handle, data, sizeof(data), &len);
        if (err != ESP_OK) return;
        uint8_t state = 0;
        bool decoded = decode_report(data, len, &state);
        if (decoded) atomic_store_explicit(&s_state, state, memory_order_relaxed);
        log_raw_report(data, len, decoded ? state : 0);
        return;
    }

    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        atomic_store_explicit(&s_state, 0, memory_order_relaxed);
        atomic_store_explicit(&s_connected, false, memory_order_relaxed);
        memset(&s_layout, 0, sizeof(s_layout));
        ESP_LOGI(TAG, "USB 手柄已拔出");
        esp_err_t err = hid_host_device_close(handle);
        if (err != ESP_OK) ESP_LOGW(TAG, "关闭 HID 接口失败：%s", esp_err_to_name(err));
        if (s_device == handle) s_device = NULL;
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "USB HID 传输错误");
    }
}

static void open_device(hid_host_device_handle_t handle)
{
    if (s_device) {
        ESP_LOGW(TAG, "目前只接收第一只 USB 手柄，忽略额外 HID 接口");
        return;
    }
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) return;
    if (params.proto == HID_PROTOCOL_KEYBOARD || params.proto == HID_PROTOCOL_MOUSE) {
        ESP_LOGI(TAG, "忽略 USB HID 键盘/鼠标接口");
        return;
    }

    const hid_host_device_config_t config = {
        .callback = interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(handle, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 HID 接口失败：%s", esp_err_to_name(err));
        return;
    }

    hid_host_dev_info_t info = {0};
    if (hid_host_get_device_info(handle, &info) == ESP_OK) {
        s_vid = info.VID;
        s_pid = info.PID;
    } else {
        s_vid = s_pid = 0;
    }
    size_t desc_len = 0;
    uint8_t *desc = hid_host_get_report_descriptor(handle, &desc_len);
    memset(&s_layout, 0, sizeof(s_layout));
    bool parsed = desc && parse_report_descriptor(desc, desc_len, &s_layout);
    if (parsed) {
        const hid_candidate_t *r = &s_layout.report;
        ESP_LOGI(TAG,
                 "USB 手柄 %04X:%04X 就绪：报告ID=%u，%u bit，方向=%s%s%s，按键=%u",
                 s_vid, s_pid, r->report_id, r->input_bits,
                 r->x.valid && r->y.valid ? "XY " : "", r->hat.valid ? "Hat " : "",
                 r->dpad_up.valid ? "Dpad " : "", r->button_count);
    } else {
        ESP_LOGW(TAG,
                 "USB HID %04X:%04X 描述符暂未识别（%u 字节）；会打印原始报告供适配",
                 s_vid, s_pid, (unsigned)desc_len);
    }

    s_device = handle;
    s_last_raw_len = 0;
    s_raw_log_budget = 40;
    atomic_store_explicit(&s_state, 0, memory_order_relaxed);
    atomic_store_explicit(&s_connected, true, memory_order_relaxed);
    err = hid_host_device_start(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动 HID 输入失败：%s", esp_err_to_name(err));
        atomic_store_explicit(&s_connected, false, memory_order_relaxed);
        hid_host_device_close(handle);
        s_device = NULL;
    }
}

static void device_callback(hid_host_device_handle_t handle,
                            hid_host_driver_event_t event, void *arg)
{
    usb_device_event_t queued = { .handle = handle, .event = event };
    if (s_device_events) xQueueSend(s_device_events, &queued, 0);
}

static void usb_daemon_task(void *arg)
{
    while (1) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) ESP_LOGW(TAG, "USB Host 事件错误：%s", esp_err_to_name(err));
    }
}

static void hid_worker_task(void *arg)
{
    while (1) {
        esp_err_t err = hid_host_handle_events(pdMS_TO_TICKS(20));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
            ESP_LOGW(TAG, "HID 事件错误：%s", esp_err_to_name(err));
        usb_device_event_t event;
        while (xQueueReceive(s_device_events, &event, 0) == pdTRUE)
            if (event.event == HID_HOST_DRIVER_EVENT_CONNECTED) open_device(event.handle);
    }
}

esp_err_t input_usb_init(void)
{
    if (s_started) return ESP_OK;
    s_device_events = xQueueCreate(USB_EVENT_QUEUE_LEN, sizeof(usb_device_event_t));
    if (!s_device_events) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        vQueueDelete(s_device_events);
        s_device_events = NULL;
        ESP_LOGW(TAG, "USB Host 未启动：%s", esp_err_to_name(err));
        return err;
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = false,
        .task_priority = 2,
        .stack_size = 4096,
        .core_id = 1,
        .callback = device_callback,
        .callback_arg = NULL,
    };
    err = hid_host_install(&hid_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB HID 未启动：%s", esp_err_to_name(err));
        return err;
    }

    BaseType_t daemon_ok = xTaskCreatePinnedToCore(
        usb_daemon_task, "usb_events", 3072, NULL, 2, NULL, 1);
    BaseType_t hid_ok = xTaskCreatePinnedToCore(
        hid_worker_task, "usb_hid", 4096, NULL, 2, NULL, 1);
    if (daemon_ok != pdPASS || hid_ok != pdPASS) {
        ESP_LOGE(TAG, "USB Host 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB HID 手柄已启用，等待丝印 USB 的 OTG 口接入设备");
    return ESP_OK;
}

uint8_t input_usb_poll(void)
{
    return atomic_load_explicit(&s_state, memory_order_relaxed);
}

bool input_usb_connected(void)
{
    return atomic_load_explicit(&s_connected, memory_order_relaxed);
}
