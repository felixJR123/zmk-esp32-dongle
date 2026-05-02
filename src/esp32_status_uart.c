#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/events/wpm_state_changed.h>
#endif
#include <zmk/usb.h>

#define STATUS_UART_NODE DT_NODELABEL(uart0)
#define HID_LED_CAPS_LOCK BIT(1)

static const struct device *const status_uart = DEVICE_DT_GET(STATUS_UART_NODE);

static int current_wpm;
static uint8_t peripheral_batteries[CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS];
static bool peripheral_battery_seen[CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS];

static void send_status_work_handler(struct k_work *work);
static void periodic_status_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(send_status_work, send_status_work_handler);
K_WORK_DELAYABLE_DEFINE(periodic_status_work, periodic_status_work_handler);

static void uart_write_string(const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        uart_poll_out(status_uart, *p);
    }
}

static int peripheral_count(void) {
    int count = 0;

    for (int i = 0; i < CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS; i++) {
        if (peripheral_battery_seen[i]) {
            count = i + 1;
        }
    }

    return count;
}

static void append_battery_list(char *line, size_t line_len, int count) {
    size_t used = strlen(line);

    for (int i = 0; i < count && used < line_len; i++) {
        int written = snprintf(line + used, line_len - used, "%s%u", i == 0 ? "" : ",",
                               peripheral_battery_seen[i] ? peripheral_batteries[i] : 0);
        if (written < 0) {
            return;
        }
        used += MIN((size_t)written, line_len - used);
    }
}

static void sanitize_value(const char *input, char *output, size_t output_len) {
    if (output_len == 0) {
        return;
    }

    size_t out = 0;
    for (const char *p = input; *p != '\0' && out < output_len - 1; p++) {
        char c = *p;
        output[out++] = (c == ' ' || c == ';' || c == '=') ? '_' : c;
    }
    output[out] = '\0';
}

static void send_status_line(void) {
    if (!device_is_ready(status_uart)) {
        return;
    }

    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);
    const char *layer_name = zmk_keymap_layer_name(layer_id);
    if (layer_name == NULL || layer_name[0] == '\0') {
        layer_name = "Default";
    }
    char layer_value[32];
    sanitize_value(layer_name, layer_value, sizeof(layer_value));

    int bt_slot = zmk_ble_active_profile_index() + 1;
    bool bt_connected = zmk_ble_profile_is_connected(bt_slot - 1);
    bool usb_connected = zmk_usb_is_hid_ready();
    int count = peripheral_count();
    zmk_mod_flags_t modifiers = zmk_hid_get_keyboard_report()->body.modifiers;
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    int capslock = (zmk_hid_indicators_get_current_profile() & HID_LED_CAPS_LOCK) ? 1 : 0;
#else
    int capslock = 0;
#endif

    char line[192];
    snprintf(line, sizeof(line),
             "layer=%s usb=%d bt=%d bt_slot=%d ctrl=%d alt=%d win=%d shift=%d "
             "capslock=%d wpm=%d peripherals=%d batt=",
             layer_value, usb_connected ? 1 : 0, bt_connected ? 1 : 0, bt_slot,
             (modifiers & (MOD_LCTL | MOD_RCTL)) ? 1 : 0,
             (modifiers & (MOD_LALT | MOD_RALT)) ? 1 : 0,
             (modifiers & (MOD_LGUI | MOD_RGUI)) ? 1 : 0,
             (modifiers & (MOD_LSFT | MOD_RSFT)) ? 1 : 0,
             capslock, current_wpm, count);

    append_battery_list(line, sizeof(line), count);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);

    uart_write_string(line);
}

static void schedule_status_send(void) {
    k_work_reschedule(&send_status_work, K_MSEC(CONFIG_ZMK_ESP32_STATUS_UART_EVENT_DELAY_MS));
}

static void send_status_now(void) {
    k_work_reschedule(&send_status_work, K_NO_WAIT);
}

static void send_status_work_handler(struct k_work *work) { send_status_line(); }

static void periodic_status_work_handler(struct k_work *work) {
    send_status_line();
    k_work_reschedule(&periodic_status_work, K_MSEC(CONFIG_ZMK_ESP32_STATUS_UART_PERIODIC_MS));
}

static int esp32_status_listener(const zmk_event_t *eh) {
    if (as_zmk_keycode_state_changed(eh) != NULL ||
        as_zmk_modifiers_state_changed(eh) != NULL) {
        schedule_status_send();
        return 0;
    }

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    if (as_zmk_hid_indicators_changed(eh) != NULL) {
        send_status_now();
        return 0;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
    const struct zmk_wpm_state_changed *wpm_event = as_zmk_wpm_state_changed(eh);
    if (wpm_event != NULL) {
        current_wpm = wpm_event->state;
    }
#endif

    const struct zmk_peripheral_battery_state_changed *battery_event =
        as_zmk_peripheral_battery_state_changed(eh);
    if (battery_event != NULL &&
        battery_event->source < CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS) {
        peripheral_batteries[battery_event->source] = battery_event->state_of_charge;
        peripheral_battery_seen[battery_event->source] = true;
    }

    schedule_status_send();
    return 0;
}

static int esp32_status_init(void) {
    if (!device_is_ready(status_uart)) {
        return -ENODEV;
    }

    schedule_status_send();
    k_work_reschedule(&periodic_status_work, K_MSEC(CONFIG_ZMK_ESP32_STATUS_UART_PERIODIC_MS));
    return 0;
}

SYS_INIT(esp32_status_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

ZMK_LISTENER(esp32_status_uart, esp32_status_listener);
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_keycode_state_changed);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_hid_indicators_changed);
#endif
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_modifiers_state_changed);
#if IS_ENABLED(CONFIG_ZMK_WPM)
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_wpm_state_changed);
#endif
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_peripheral_battery_state_changed);
