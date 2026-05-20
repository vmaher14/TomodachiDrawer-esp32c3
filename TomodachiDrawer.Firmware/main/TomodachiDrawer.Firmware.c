#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "tusb.h"

static const char *TAG = "TomodachiDrawer";

#define NEOPIXEL_PIN    GPIO_NUM_2    // WS2812 on DevKitRust-1
#define LED_PIN         GPIO_NUM_7    // Regular LED on DevKitRust-1

typedef uint16_t gamepad_button_t;

#define BTN_Y       ((gamepad_button_t)0x0001)
#define BTN_B       ((gamepad_button_t)0x0002)
#define BTN_A       ((gamepad_button_t)0x0004)
#define BTN_X       ((gamepad_button_t)0x0008)
#define BTN_L       ((gamepad_button_t)0x0010)
#define BTN_R       ((gamepad_button_t)0x0020)
#define BTN_ZL      ((gamepad_button_t)0x0040)
#define BTN_ZR      ((gamepad_button_t)0x0080)
#define BTN_MINUS   ((gamepad_button_t)0x0101)
#define BTN_PLUS    ((gamepad_button_t)0x0102)
#define BTN_LCLICK  ((gamepad_button_t)0x0104)
#define BTN_RCLICK  ((gamepad_button_t)0x0108)
#define BTN_HOME    ((gamepad_button_t)0x0110)
#define BTN_CAPTURE ((gamepad_button_t)0x0120)

#define DPAD_UP        0
#define DPAD_UPRIGHT   1
#define DPAD_RIGHT     2
#define DPAD_DOWNRIGHT 3
#define DPAD_DOWN      4
#define DPAD_DOWNLEFT  5
#define DPAD_LEFT      6
#define DPAD_UPLEFT    7
#define DPAD_NEUTRAL   8

#define STICK_LX     3
#define STICK_LY     4
#define STICK_RX     5
#define STICK_RY     6
#define STICK_CENTER 128

#define NEOPIXEL_BRIGHT  127
#define RAINBOW_DIVISOR  4

#define TDLD_VERSION            0x03
#define OPCODE_INVALID          0x0
#define OPCODE_PRESS_BUTTON     0x1
#define OPCODE_RELEASE_BUTTON   0x2
#define OPCODE_PRESS_DPAD       0x3
#define OPCODE_RELEASE_DPAD     0x4
#define OPCODE_RELEASE_ALL      0x5
#define OPCODE_DELAY            0x6
#define OPCODE_SET_STICK        0x7
#define OPCODE_TAP_BUTTON       0x8
#define OPCODE_TAP_DPAD         0x9
#define OPCODE_REPEAT_LAST_1    0xE
#define OPCODE_REPEAT_LAST_2    0xF

uint8_t current_report[8] = {0x00, 0x00, 0x08, 128, 128, 128, 128, 0x00};

const gamepad_button_t button_map[] = {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R,
    BTN_ZL, BTN_ZR, BTN_MINUS, BTN_PLUS, BTN_LCLICK,
    BTN_RCLICK, BTN_HOME, BTN_CAPTURE
};

const uint8_t stick_axis_map[] = {
    STICK_LX, STICK_LY, STICK_RX, STICK_RY
};

// ----------------------------------------------------------------
// NeoPixel via RMT
// ----------------------------------------------------------------
static rmt_channel_handle_t rmt_chan = NULL;
static rmt_encoder_handle_t rmt_encoder = NULL;

static void neopixel_init(void) {
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num          = NEOPIXEL_PIN,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000, // 10MHz = 100ns/tick
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &rmt_chan));

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 4,   // 400ns high
                  .level1 = 0, .duration1 = 8 },  // 800ns low
        .bit1 = { .level0 = 1, .duration0 = 8,   // 800ns high
                  .level1 = 0, .duration1 = 4 },  // 400ns low
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &rmt_encoder));
    ESP_ERROR_CHECK(rmt_enable(rmt_chan));
}

static void neopixel_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t grb[3] = { g, r, b }; // WS2812 expects GRB order
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_tx_wait_all_done(rmt_chan, portMAX_DELAY);
    rmt_transmit(rmt_chan, rmt_encoder, grb, sizeof(grb), &tx_cfg);
}

// ----------------------------------------------------------------
// Regular LED
// ----------------------------------------------------------------
static void boringpixel_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
}

static void boringpixel_set(bool on) {
    gpio_set_level(LED_PIN, on ? 1 : 0);
}

// ----------------------------------------------------------------
// HID helpers
// ----------------------------------------------------------------
static inline void hid_press(gamepad_button_t btn) {
    current_report[btn >> 8] |= (btn & 0xFF);
}
static inline void hid_release(gamepad_button_t btn) {
    current_report[btn >> 8] &= ~(btn & 0xFF);
}
static inline void hid_release_all(void) {
    current_report[0] = 0x00;
    current_report[1] = 0x00;
    current_report[2] = DPAD_NEUTRAL;
    current_report[3] = STICK_CENTER;
    current_report[4] = STICK_CENTER;
    current_report[5] = STICK_CENTER;
    current_report[6] = STICK_CENTER;
    current_report[7] = 0x00;
}
static inline void hid_set_dpad(uint8_t direction) {
    current_report[2] = direction;
}
static inline void hid_set_stick(uint8_t axis, uint8_t value) {
    current_report[axis] = value;
}

// ----------------------------------------------------------------
// USB / reporting
// ----------------------------------------------------------------
static void delay_ms_usb(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void send_report_raw(void) {
    while (!tud_hid_ready()) {
        vTaskDelay(1);
    }
    tud_hid_report(0, current_report, sizeof(current_report));
}

static void push_report(void) {
    send_report_raw();
    if (current_report[0] != 0 || current_report[1] != 0) {
        neopixel_set_rgb(0, NEOPIXEL_BRIGHT, 0);
        boringpixel_set(true);
    } else {
        neopixel_set_rgb(10, 10, 10);
        boringpixel_set(false);
    }
}

// ----------------------------------------------------------------
// Error and rainbow states
// ----------------------------------------------------------------
static void error_flash(int interval_ms) {
    while (true) {
        neopixel_set_rgb(NEOPIXEL_BRIGHT, 0, 0);
        boringpixel_set(true);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        neopixel_set_rgb(0, 0, 0);
        boringpixel_set(false);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

static void get_good_rainbow(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (hue < 85) {
        *r = 255 - hue * 3; *g = hue * 3;       *b = 0;
    } else if (hue < 170) {
        hue -= 85;
        *r = 0;             *g = 255 - hue * 3; *b = hue * 3;
    } else {
        hue -= 170;
        *r = hue * 3;       *g = 0;             *b = 255 - hue * 3;
    }
}

static void done_rainbow(void) {
    uint8_t hue = 0, r, g, b;
    boringpixel_set(true);
    while (true) {
        get_good_rainbow(hue, &r, &g, &b);
        neopixel_set_rgb(r / RAINBOW_DIVISOR, g / RAINBOW_DIVISOR, b / RAINBOW_DIVISOR);
        vTaskDelay(pdMS_TO_TICKS(10));
        hue++;
    }
}

// ----------------------------------------------------------------
// Single-byte opcode runner
// ----------------------------------------------------------------
static void run_single_byte_opcode(uint8_t record) {
    uint8_t opcode = record >> 4;
    uint8_t nibble = record & 0xF;
    switch (opcode) {
        case OPCODE_PRESS_BUTTON:
            hid_press(button_map[nibble]);
            push_report();
            break;
        case OPCODE_RELEASE_BUTTON:
            hid_release(button_map[nibble]);
            push_report();
            break;
        case OPCODE_PRESS_DPAD:
            hid_set_dpad(nibble);
            push_report();
            break;
        case OPCODE_RELEASE_DPAD:
            hid_set_dpad(DPAD_NEUTRAL);
            push_report();
            break;
        case OPCODE_RELEASE_ALL:
            hid_release_all();
            push_report();
            break;
        case OPCODE_TAP_BUTTON:
            hid_press(button_map[nibble]);
            push_report();
            delay_ms_usb(25);
            hid_release(button_map[nibble]);
            push_report();
            delay_ms_usb(25);
            break;
        case OPCODE_TAP_DPAD:
            hid_set_dpad(nibble);
            push_report();
            delay_ms_usb(25);
            hid_set_dpad(DPAD_NEUTRAL);
            push_report();
            delay_ms_usb(25);
            break;
        default:
            error_flash(5000);
            break;
    }
}

// ----------------------------------------------------------------
// Flash reading
// ----------------------------------------------------------------
static uint8_t *flash_buf = NULL;

static bool load_flash_data(void) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!part) {
        ESP_LOGE(TAG, "No 'storage' partition found!");
        return false;
    }
    size_t read_size = part->size < (512 * 1024) ? part->size : (512 * 1024);
    flash_buf = malloc(read_size);
    if (!flash_buf) {
        ESP_LOGE(TAG, "malloc failed");
        return false;
    }
    esp_err_t err = esp_partition_read(part, 0, flash_buf, read_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_read failed: %s", esp_err_to_name(err));
        free(flash_buf);
        flash_buf = NULL;
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
// Main task
// ----------------------------------------------------------------
static void tdld_task(void *arg) {
    while (!tud_mounted()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 3-flash countdown
    for (int i = 0; i < 3; i++) {
        neopixel_set_rgb(NEOPIXEL_BRIGHT, NEOPIXEL_BRIGHT, 0);
        send_report_raw();
        vTaskDelay(pdMS_TO_TICKS(500));
        neopixel_set_rgb(0, 0, 0);
        send_report_raw();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!load_flash_data()) {
        error_flash(100);
    }

    const uint8_t *ptr = flash_buf;

    if (ptr[0] != 'T' || ptr[1] != 'D' || ptr[2] != 'L' || ptr[3] != 'D') {
        ESP_LOGE(TAG, "Bad TDLD magic");
        error_flash(250);
    }
    if (ptr[4] != TDLD_VERSION) {
        ESP_LOGE(TAG, "Wrong TDLD version: got %d expected %d", ptr[4], TDLD_VERSION);
        error_flash(1000);
    }
    ptr += 6;

    uint8_t last_1byte_record = 0;
    bool working = true;

    while (working) {
        uint8_t record = *ptr++;
        uint8_t opcode = record >> 4;
        uint8_t nibble = record & 0x0F;

        switch (opcode) {
            case OPCODE_INVALID:
                working = false;
                break;
            case OPCODE_DELAY: {
                uint8_t data      = *ptr++;
                uint16_t delay_ms = (nibble << 8) | data;
                delay_ms_usb(delay_ms);
                break;
            }
            case OPCODE_SET_STICK: {
                uint8_t val = *ptr++;
                hid_set_stick(stick_axis_map[nibble], val);
                push_report();
                break;
            }
            case OPCODE_REPEAT_LAST_1: {
                uint8_t count = nibble;
                for (int i = 0; i < count; i++)
                    run_single_byte_opcode(last_1byte_record);
                break;
            }
            case OPCODE_REPEAT_LAST_2: {
                uint8_t data   = *ptr++;
                uint16_t count = (nibble << 8) | data;
                for (uint16_t i = 0; i < count; i++)
                    run_single_byte_opcode(last_1byte_record);
                break;
            }
            default:
                run_single_byte_opcode(record);
                last_1byte_record = record;
                break;
        }
    }

    free(flash_buf);
    done_rainbow();
    vTaskDelete(NULL);
}

// ----------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------
void app_main(void) {
    neopixel_init();
    boringpixel_init();

    neopixel_set_rgb(0, 0, NEOPIXEL_BRIGHT); // blue = booting
    vTaskDelay(pdMS_TO_TICKS(200));
    neopixel_set_rgb(0, 0, 0);

    xTaskCreate(tdld_task, "tdld", 8192, NULL, 5, NULL);
}

// ----------------------------------------------------------------
// TinyUSB HID callbacks
// ----------------------------------------------------------------
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id,
    hid_report_type_t type, uint8_t *buf, uint16_t len)
{
    (void)itf; (void)id;
    if (type == HID_REPORT_TYPE_INPUT) {
        uint16_t sz = sizeof(current_report);
        if (sz > len) sz = len;
        memcpy(buf, current_report, sz);
        return sz;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t id,
    hid_report_type_t type, uint8_t const *buf, uint16_t len)
{
    (void)itf; (void)id; (void)type; (void)buf; (void)len;
}