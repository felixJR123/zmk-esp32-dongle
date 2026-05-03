#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/ble.h>
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/position_state_changed.h>
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
#define ESP32_DECK_TILE_1_NODE DT_PATH(esp32_deck, tile_1)
#define ESP32_DECK_TILE_2_NODE DT_PATH(esp32_deck, tile_2)
#define ESP32_DECK_TILE_3_NODE DT_PATH(esp32_deck, tile_3)
#define ESP32_DECK_TILE_4_NODE DT_PATH(esp32_deck, tile_4)
#define ESP32_DECK_TILE_5_NODE DT_PATH(esp32_deck, tile_5)
#define ESP32_DECK_TILE_6_NODE DT_PATH(esp32_deck, tile_6)
#define ESP32_DECK_TILE_7_NODE DT_PATH(esp32_deck, tile_7)
#define ESP32_DECK_TILE_8_NODE DT_PATH(esp32_deck, tile_8)
#define ESP32_DECK_TILE_9_NODE DT_PATH(esp32_deck, tile_9)
#define ESP32_DECK_TILE_LABEL(node, fallback)                                                      \
    COND_CODE_1(DT_NODE_EXISTS(node), (DT_PROP_OR(node, label, fallback)), (fallback))
#define ESP32_DECK_EMPTY_BINDING                                                                   \
    { .behavior_dev = NULL, .param1 = 0, .param2 = 0 }
#define ESP32_DECK_TILE_BINDING(node)                                                              \
    COND_CODE_1(DT_NODE_EXISTS(node),                                                              \
                (COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                     \
                             (ZMK_KEYMAP_EXTRACT_BINDING(0, node)),                                \
                             (ESP32_DECK_EMPTY_BINDING))),                                         \
                (ESP32_DECK_EMPTY_BINDING))

static const struct device *const status_uart = DEVICE_DT_GET(STATUS_UART_NODE);

static int current_wpm;
static uint8_t peripheral_batteries[CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS];
static bool peripheral_battery_seen[CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS];
static char command_line[96];
static size_t command_line_len;
static const char *const deck_tile_labels[] = {
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_1_NODE, CONFIG_ZMK_ESP32_DECK_TILE_1_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_2_NODE, CONFIG_ZMK_ESP32_DECK_TILE_2_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_3_NODE, CONFIG_ZMK_ESP32_DECK_TILE_3_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_4_NODE, CONFIG_ZMK_ESP32_DECK_TILE_4_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_5_NODE, CONFIG_ZMK_ESP32_DECK_TILE_5_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_6_NODE, CONFIG_ZMK_ESP32_DECK_TILE_6_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_7_NODE, CONFIG_ZMK_ESP32_DECK_TILE_7_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_8_NODE, CONFIG_ZMK_ESP32_DECK_TILE_8_LABEL),
    ESP32_DECK_TILE_LABEL(ESP32_DECK_TILE_9_NODE, CONFIG_ZMK_ESP32_DECK_TILE_9_LABEL),
};
static const struct zmk_behavior_binding deck_tile_bindings[] = {
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_1_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_2_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_3_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_4_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_5_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_6_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_7_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_8_NODE),
    ESP32_DECK_TILE_BINDING(ESP32_DECK_TILE_9_NODE),
};
extern int set_state(enum zmk_activity_state state);

static void send_status_work_handler(struct k_work *work);
static void periodic_status_work_handler(struct k_work *work);
static void receive_command_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(send_status_work, send_status_work_handler);
K_WORK_DELAYABLE_DEFINE(periodic_status_work, periodic_status_work_handler);
K_WORK_DELAYABLE_DEFINE(receive_command_work, receive_command_work_handler);

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

    struct zmk_endpoint_instance selected_endpoint = zmk_endpoint_get_selected();
    int bt_slot = zmk_ble_active_profile_index() + 1;
    bool usb_connected = selected_endpoint.transport == ZMK_TRANSPORT_USB && zmk_usb_is_hid_ready();
    bool bt_connected = false;
    if (selected_endpoint.transport == ZMK_TRANSPORT_BLE) {
        bt_slot = selected_endpoint.ble.profile_index + 1;
        bt_connected = zmk_ble_profile_is_connected(selected_endpoint.ble.profile_index);
    }
    const char *conn = selected_endpoint.transport == ZMK_TRANSPORT_USB
                           ? "usb"
                           : selected_endpoint.transport == ZMK_TRANSPORT_BLE ? "bt" : "none";
    int count = peripheral_count();
    enum zmk_activity_state activity = zmk_activity_get_state();
    const char *display = activity == ZMK_ACTIVITY_ACTIVE ? "on" : "off";
    zmk_mod_flags_t modifiers = zmk_hid_get_keyboard_report()->body.modifiers;
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    int capslock = (zmk_hid_indicators_get_current_profile() & HID_LED_CAPS_LOCK) ? 1 : 0;
#else
    int capslock = 0;
#endif

    char line[192];
    snprintf(line, sizeof(line),
             "layer=%s usb=%d bt=%d bt_slot=%d conn=%s ctrl=%d alt=%d win=%d shift=%d "
             "capslock=%d wpm=%d display=%s bt_profiles=%d peripherals=%d batt=",
             layer_value, usb_connected ? 1 : 0, bt_connected ? 1 : 0, bt_slot, conn,
             (modifiers & (MOD_LCTL | MOD_RCTL)) ? 1 : 0,
             (modifiers & (MOD_LALT | MOD_RALT)) ? 1 : 0,
             (modifiers & (MOD_LGUI | MOD_RGUI)) ? 1 : 0,
             (modifiers & (MOD_LSFT | MOD_RSFT)) ? 1 : 0,
             capslock, current_wpm, display, ZMK_BLE_PROFILE_COUNT, count);

    append_battery_list(line, sizeof(line), count);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);

    uart_write_string(line);
}

static void send_deck_label_line(int tile, const char *label) {
    if (label == NULL || label[0] == '\0') {
        return;
    }

    char safe_label[48];
    sanitize_value(label, safe_label, sizeof(safe_label));

    char line[80];
    snprintf(line, sizeof(line), "deck_tile=%d label=%s\n", tile, safe_label);
    uart_write_string(line);
}

static void send_deck_label_lines(void) {
    if (!device_is_ready(status_uart)) {
        return;
    }

    for (int i = 0; i < ARRAY_SIZE(deck_tile_labels); i++) {
        send_deck_label_line(i + 1, deck_tile_labels[i]);
    }
}

static void send_deck_result_line(int tile, int result) {
    char line[48];
    snprintf(line, sizeof(line), "deck_result=%d status=%s code=%d\n", tile,
             result == 0 ? "ok" : "error", result);
    uart_write_string(line);
}

static void send_command_ack_line(const char *command)
{
    const char *name = "unknown";

    if (strstr(command, "cmd=usb") != NULL) {
        name = "usb";
    } else if (strstr(command, "cmd=bt") != NULL) {
        name = "bt";
    } else if (strstr(command, "cmd=deck") != NULL) {
        name = "deck";
    } else if (strstr(command, "cmd=clear_profile") != NULL) {
        name = "clear_profile";
    } else if (strstr(command, "cmd=clear_all_profiles") != NULL) {
        name = "clear_all_profiles";
    } else if (strstr(command, "cmd=wake") != NULL) {
        name = "wake";
    }

    char line[40];
    snprintf(line, sizeof(line), "cmd_ack=%s\n", name);
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
    send_deck_label_lines();
    k_work_reschedule(&periodic_status_work, K_MSEC(CONFIG_ZMK_ESP32_STATUS_UART_PERIODIC_MS));
}

static int command_profile_index(const char *line) {
    const char *profile = strstr(line, "profile=");
    if (profile == NULL) {
        return zmk_ble_active_profile_index();
    }

    int one_based = atoi(profile + strlen("profile="));
    if (one_based <= 0) {
        return 0;
    }
    return one_based - 1;
}

static int command_tile_number(const char *line) {
    const char *tile = strstr(line, "tile=");
    if (tile == NULL) {
        return 0;
    }

    return atoi(tile + strlen("tile="));
}

static int invoke_deck_tile(int tile) {
    if (tile < 1 || tile > ARRAY_SIZE(deck_tile_bindings)) {
        return -EINVAL;
    }

    const struct zmk_behavior_binding *binding = &deck_tile_bindings[tile - 1];
    if (binding->behavior_dev == NULL) {
        return -ENOENT;
    }

    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);
    struct zmk_behavior_binding_event event = {
        .layer = layer_id,
        .position = 0xE000 + tile,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int ret = zmk_behavior_invoke_binding(binding, event, true);
    if (ret < 0) {
        return ret;
    }

    k_msleep(10);
    event.timestamp = k_uptime_get();
    return zmk_behavior_invoke_binding(binding, event, false);
}

static void handle_command_line(const char *line) {
    send_command_ack_line(line);

    if (strstr(line, "cmd=usb") != NULL) {
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=bt") != NULL) {
        int profile = command_profile_index(line);
        if (profile >= 0 && profile < ZMK_BLE_PROFILE_COUNT) {
            zmk_ble_prof_select(profile);
        }
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
        send_status_now();
        k_msleep(80);
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=clear_profile") != NULL) {
        zmk_ble_clear_bonds();
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=clear_all_profiles") != NULL) {
        zmk_ble_clear_all_bonds();
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=deck") != NULL) {
        int tile = command_tile_number(line);
        int ret = invoke_deck_tile(tile);
        send_deck_result_line(tile, ret);
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=wake") != NULL) {
        set_state(ZMK_ACTIVITY_ACTIVE);
        send_status_now();
        return;
    }
}

static void receive_command_work_handler(struct k_work *work) {
    unsigned char c;
    while (uart_poll_in(status_uart, &c) == 0) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            command_line[command_line_len] = '\0';
            if (command_line_len > 0) {
                handle_command_line(command_line);
            }
            command_line_len = 0;
            continue;
        }
        if (command_line_len < sizeof(command_line) - 1) {
            command_line[command_line_len++] = c;
        } else {
            command_line_len = 0;
        }
    }

    k_work_reschedule(&receive_command_work, K_MSEC(30));
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

    const struct zmk_activity_state_changed *activity_ev = as_zmk_activity_state_changed(eh);
    if (activity_ev != NULL) {
        send_status_now();
        if (activity_ev->state == ZMK_ACTIVITY_ACTIVE) {
            k_work_reschedule(&periodic_status_work,
                              K_MSEC(CONFIG_ZMK_ESP32_STATUS_UART_PERIODIC_MS));
        }
        return 0;
    }

    if (as_zmk_endpoint_changed(eh) != NULL) {
        send_status_now();
        return 0;
    }

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
    send_deck_label_lines();
    k_work_reschedule(&periodic_status_work, K_MSEC(CONFIG_ZMK_ESP32_STATUS_UART_PERIODIC_MS));
    k_work_reschedule(&receive_command_work, K_MSEC(30));
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
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_endpoint_changed);
