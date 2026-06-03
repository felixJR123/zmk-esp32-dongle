#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <hal/nrf_power.h>
#include <hal/nrf_usbd.h>

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

#if IS_ENABLED(CONFIG_ZMK_ESP32_STATUS_UART_ZMK_V3)
/* Layer type alias: ZMK v3 already declares zmk_keymap_layer_index_to_id. */
typedef zmk_keymap_layer_index_t zmk_keymap_layer_id_t;
/* ZMK v3 has no ZMK_TRANSPORT_NONE sentinel — define one for our state tracking. */
#ifndef ZMK_TRANSPORT_NONE
#define ZMK_TRANSPORT_NONE 0xFF
#endif
/* Endpoint API shims: ZMK v3 renamed these functions. */
static inline struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return zmk_endpoints_selected();
}
static inline int zmk_endpoint_set_preferred_transport(enum zmk_transport t) {
    if ((uint8_t)t == ZMK_TRANSPORT_NONE) {
        /* zmk_endpoints_select_transport rejects 0xFF (invalid transport) in ZMK v3.
           Instead, flush the HID keyboard report so nothing is sent to the host
           while text input mode is active. */
        zmk_hid_keyboard_clear();
        zmk_hid_consumer_clear();
        return 0;
    }
    return zmk_endpoints_select_transport(t);
}
static inline enum zmk_transport zmk_endpoint_get_preferred_transport(void) {
    return zmk_endpoints_selected().transport;
}
#endif /* CONFIG_ZMK_ESP32_STATUS_UART_ZMK_V3 */

#define STATUS_UART_NODE DT_NODELABEL(uart0)
#define HID_LED_CAPS_LOCK BIT(1)
#define BT_PROFILE_NAMES_NODE DT_PATH(bt_profile_names)
#define BT_PROFILE_NAME_NODE(n) DT_PATH(bt_profile_names, profile_##n)
#define BT_PROFILE_NAME_LABEL(n, fallback)                                                         \
    COND_CODE_1(DT_NODE_EXISTS(BT_PROFILE_NAME_NODE(n)),                                           \
                (DT_PROP_OR(BT_PROFILE_NAME_NODE(n), label, fallback)), (fallback))

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

#define ESP32_VOL_NODE    DT_PATH(esp32_volume)
#define ESP32_VOL_UP_NODE DT_PATH(esp32_volume, vol_up)
#define ESP32_VOL_DN_NODE DT_PATH(esp32_volume, vol_dn)
#define ESP32_VOL_MUTE_NODE DT_PATH(esp32_volume, mute)
#define ESP32_VOL_BINDING(node)                                                                    \
    COND_CODE_1(DT_NODE_EXISTS(node),                                                              \
                (ZMK_KEYMAP_EXTRACT_BINDING(0, node)),                                             \
                (ESP32_DECK_EMPTY_BINDING))
#define ESP32_DECK_TILE_BINDING(node)                                                              \
    COND_CODE_1(DT_NODE_EXISTS(node),                                                              \
                (COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                     \
                             (ZMK_KEYMAP_EXTRACT_BINDING(0, node)),                                \
                             (ESP32_DECK_EMPTY_BINDING))),                                         \
                (ESP32_DECK_EMPTY_BINDING))

/* Sub-tile DT helpers: esp32_deck_menu_tiles / menu_tile_M / subtile_S */
#define ESP32_SUBTILE_NODE(m, s) \
    DT_PATH(esp32_deck_menu_tiles, menu_tile_##m, subtile_##s)
#define ESP32_SUBTILE_BINDING(m, s)                                                                \
    COND_CODE_1(DT_NODE_EXISTS(ESP32_SUBTILE_NODE(m, s)),                                          \
                (COND_CODE_1(DT_NODE_HAS_PROP(ESP32_SUBTILE_NODE(m, s), bindings),                 \
                             (ZMK_KEYMAP_EXTRACT_BINDING(0, ESP32_SUBTILE_NODE(m, s))),            \
                             (ESP32_DECK_EMPTY_BINDING))),                                         \
                (ESP32_DECK_EMPTY_BINDING))
#define ESP32_SUBTILE_LABEL(m, s, fb)                                                              \
    COND_CODE_1(DT_NODE_EXISTS(ESP32_SUBTILE_NODE(m, s)),                                          \
                (DT_PROP_OR(ESP32_SUBTILE_NODE(m, s), label, fb)), (fb))
#define ESP32_SUBTILE_BINDINGS_ROW(m) {                                                            \
    ESP32_SUBTILE_BINDING(m, 1), ESP32_SUBTILE_BINDING(m, 2), ESP32_SUBTILE_BINDING(m, 3),        \
    ESP32_SUBTILE_BINDING(m, 4), ESP32_SUBTILE_BINDING(m, 5), ESP32_SUBTILE_BINDING(m, 6),        \
    ESP32_SUBTILE_BINDING(m, 7), ESP32_SUBTILE_BINDING(m, 8), ESP32_SUBTILE_BINDING(m, 9),        \
}
#define ESP32_SUBTILE_LABELS_ROW(m) {                                                              \
    ESP32_SUBTILE_LABEL(m, 1, ""), ESP32_SUBTILE_LABEL(m, 2, ""), ESP32_SUBTILE_LABEL(m, 3, ""),  \
    ESP32_SUBTILE_LABEL(m, 4, ""), ESP32_SUBTILE_LABEL(m, 5, ""), ESP32_SUBTILE_LABEL(m, 6, ""),  \
    ESP32_SUBTILE_LABEL(m, 7, ""), ESP32_SUBTILE_LABEL(m, 8, ""), ESP32_SUBTILE_LABEL(m, 9, ""),  \
}

static const struct device *const status_uart = DEVICE_DT_GET(STATUS_UART_NODE);

static int current_wpm;
static uint8_t peripheral_batteries[CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS];
static bool peripheral_battery_seen[CONFIG_ZMK_ESP32_STATUS_UART_MAX_PERIPHERALS];
static char command_line[96];
static char pending_command_line[96];
static size_t command_line_len;
static volatile bool pending_command_ready;
static int esp32_requested_transport = -1;
static int esp32_requested_bt_profile = -1;
static bool esp32_volume_muted = false;
static int64_t esp32_mute_suppress_until = 0;
static bool esp32_skip_invoked_mute_report = false;
static enum zmk_transport esp32_last_host_transport = ZMK_TRANSPORT_USB;
static int esp32_last_host_bt_profile = -1;
static bool esp32_text_input_mode = false;
static bool esp32_text_input_endpoint_parked = false;
static enum zmk_transport esp32_text_input_saved_preferred = ZMK_TRANSPORT_NONE;
static int esp32_text_input_saved_requested_transport = -1;
static int esp32_text_input_saved_requested_bt_profile = -1;
static bool esp32_text_input_caps_lock = false;
static void send_status_now(void);
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
/* Up to 8 host BT profile labels (peripherals are not counted).
 * Set labels via the bt_profile_names DTS node in your keyboard overlay,
 * or via CONFIG_ZMK_ESP32_BT_PROFILE_N_LABEL in your .conf file.
 * Empty labels are skipped; the screen falls back to "BTN" automatically. */
static const char *const bt_profile_names[] = {
    BT_PROFILE_NAME_LABEL(1, CONFIG_ZMK_ESP32_BT_PROFILE_1_LABEL),
    BT_PROFILE_NAME_LABEL(2, CONFIG_ZMK_ESP32_BT_PROFILE_2_LABEL),
    BT_PROFILE_NAME_LABEL(3, CONFIG_ZMK_ESP32_BT_PROFILE_3_LABEL),
    BT_PROFILE_NAME_LABEL(4, CONFIG_ZMK_ESP32_BT_PROFILE_4_LABEL),
    BT_PROFILE_NAME_LABEL(5, CONFIG_ZMK_ESP32_BT_PROFILE_5_LABEL),
    BT_PROFILE_NAME_LABEL(6, CONFIG_ZMK_ESP32_BT_PROFILE_6_LABEL),
    BT_PROFILE_NAME_LABEL(7, CONFIG_ZMK_ESP32_BT_PROFILE_7_LABEL),
    BT_PROFILE_NAME_LABEL(8, CONFIG_ZMK_ESP32_BT_PROFILE_8_LABEL),
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

static const struct zmk_behavior_binding sub_tile_bindings[9][9] = {
    ESP32_SUBTILE_BINDINGS_ROW(1),
    ESP32_SUBTILE_BINDINGS_ROW(2),
    ESP32_SUBTILE_BINDINGS_ROW(3),
    ESP32_SUBTILE_BINDINGS_ROW(4),
    ESP32_SUBTILE_BINDINGS_ROW(5),
    ESP32_SUBTILE_BINDINGS_ROW(6),
    ESP32_SUBTILE_BINDINGS_ROW(7),
    ESP32_SUBTILE_BINDINGS_ROW(8),
    ESP32_SUBTILE_BINDINGS_ROW(9),
};

static const struct zmk_behavior_binding vol_bindings[3] = {
    ESP32_VOL_BINDING(ESP32_VOL_UP_NODE),
    ESP32_VOL_BINDING(ESP32_VOL_DN_NODE),
    ESP32_VOL_BINDING(ESP32_VOL_MUTE_NODE),
};

static const char *const sub_tile_labels[9][9] = {
    ESP32_SUBTILE_LABELS_ROW(1),
    ESP32_SUBTILE_LABELS_ROW(2),
    ESP32_SUBTILE_LABELS_ROW(3),
    ESP32_SUBTILE_LABELS_ROW(4),
    ESP32_SUBTILE_LABELS_ROW(5),
    ESP32_SUBTILE_LABELS_ROW(6),
    ESP32_SUBTILE_LABELS_ROW(7),
    ESP32_SUBTILE_LABELS_ROW(8),
    ESP32_SUBTILE_LABELS_ROW(9),
};

extern int set_state(enum zmk_activity_state state);

static void send_status_work_handler(struct k_work *work);
static void periodic_status_work_handler(struct k_work *work);
static void receive_command_work_handler(struct k_work *work);
static void bt_names_retry_work_handler(struct k_work *work);
static void deck_labels_retry_work_handler(struct k_work *work);
static void sub_tile_labels_retry_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(send_status_work, send_status_work_handler);
K_WORK_DELAYABLE_DEFINE(periodic_status_work, periodic_status_work_handler);
K_WORK_DELAYABLE_DEFINE(receive_command_work, receive_command_work_handler);
K_WORK_DELAYABLE_DEFINE(bt_names_retry_work, bt_names_retry_work_handler);
K_WORK_DELAYABLE_DEFINE(deck_labels_retry_work, deck_labels_retry_work_handler);
K_WORK_DELAYABLE_DEFINE(sub_tile_labels_retry_work, sub_tile_labels_retry_work_handler);

static bool bt_names_ack_pending = false;
static bool deck_labels_ack_pending = false;
static bool sub_tile_labels_ack_pending = false;

static void uart_write_string(const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        uart_poll_out(status_uart, *p);
    }
}

static void send_mute_state_line(void) {
    char line[16];
    snprintf(line, sizeof(line), "mute=%d\n", esp32_volume_muted ? 1 : 0);
    uart_write_string(line);
}

static bool is_mute_keycode(const struct zmk_keycode_state_changed *ev) {
    return (ev->usage_page == HID_USAGE_CONSUMER && ev->keycode == HID_USAGE_CONSUMER_MUTE) ||
           (ev->usage_page == HID_USAGE_KEY && ev->keycode == HID_USAGE_KEY_KEYBOARD_MUTE) ||
           (ev->usage_page == HID_USAGE_KEY && ev->keycode == 0xEF);
}

static bool is_mute_usage(uint16_t usage_page, uint32_t keycode) {
    return (usage_page == HID_USAGE_CONSUMER && keycode == HID_USAGE_CONSUMER_MUTE) ||
           (usage_page == HID_USAGE_KEY && keycode == HID_USAGE_KEY_KEYBOARD_MUTE) ||
           (usage_page == HID_USAGE_KEY && keycode == 0xEF);
}

static bool binding_is_mute_key(const struct zmk_behavior_binding *binding) {
    if (binding == NULL || binding->behavior_dev == NULL) {
        return false;
    }

    uint16_t usage_page = ZMK_HID_USAGE_PAGE(binding->param1);
    uint32_t keycode = ZMK_HID_USAGE_ID(binding->param1);
    if (!usage_page) {
        usage_page = HID_USAGE_KEY;
    }

    return is_mute_usage(usage_page, keycode);
}

static void report_mute_toggle_from_binding(void) {
    esp32_volume_muted = !esp32_volume_muted;
    send_mute_state_line();
    esp32_mute_suppress_until = k_uptime_get() + 100;
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

static bool usb_vbus_detected(void) {
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}

static bool usb_host_frames_active(void) {
    if (nrf_usbd_event_check(NRF_USBD, NRF_USBD_EVENT_SOF)) {
        nrf_usbd_event_clear(NRF_USBD, NRF_USBD_EVENT_SOF);
    }

    k_sleep(K_MSEC(4));

    bool active = nrf_usbd_event_check(NRF_USBD, NRF_USBD_EVENT_SOF);
    if (active) {
        nrf_usbd_event_clear(NRF_USBD, NRF_USBD_EVENT_SOF);
    }

    return active;
}

static const char *requested_conn_name(struct zmk_endpoint_instance selected_endpoint) {
    if (esp32_requested_transport == ZMK_TRANSPORT_USB) {
        return "usb";
    }
    if (esp32_requested_transport == ZMK_TRANSPORT_BLE) {
        return "bt";
    }
    return selected_endpoint.transport == ZMK_TRANSPORT_USB
               ? "usb"
               : selected_endpoint.transport == ZMK_TRANSPORT_BLE ? "bt" : "none";
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
    bool usb_vbus = usb_vbus_detected();
    bool usb_hid_ready = zmk_usb_is_hid_ready();
    bool usb_host_active = usb_host_frames_active();
    bool usb_ready = usb_vbus && usb_hid_ready && usb_host_active;
    bool bt_connected = false;
    if (esp32_requested_transport == ZMK_TRANSPORT_BLE && esp32_requested_bt_profile >= 0 &&
        esp32_requested_bt_profile < ZMK_BLE_PROFILE_COUNT) {
        bt_slot = esp32_requested_bt_profile + 1;
        bt_connected = zmk_ble_profile_is_connected(esp32_requested_bt_profile);
    } else if (selected_endpoint.transport == ZMK_TRANSPORT_BLE) {
        bt_slot = selected_endpoint.ble.profile_index + 1;
        bt_connected = zmk_ble_profile_is_connected(selected_endpoint.ble.profile_index);
    }
    const char *conn = requested_conn_name(selected_endpoint);
    int count = peripheral_count();
    enum zmk_activity_state activity = zmk_activity_get_state();
    const char *display = activity == ZMK_ACTIVITY_ACTIVE ? "on" : "off";
    zmk_mod_flags_t modifiers = zmk_hid_get_keyboard_report()->body.modifiers;
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    int capslock = (zmk_hid_indicators_get_current_profile() & HID_LED_CAPS_LOCK) ? 1 : 0;
#else
    int capslock = 0;
#endif

    char line[260];
    snprintf(line, sizeof(line),
             "layer=%s usb=%d bt=%d bt_slot=%d conn=%s ctrl=%d alt=%d win=%d shift=%d "
             "capslock=%d wpm=%d mute=%d display=%s bt_open=%d usb_avail=%d usb_vbus=%d usb_hid=%d "
             "usb_sof=%d bt_profiles=%d peripherals=%d batt=",
             layer_value, usb_ready ? 1 : 0, bt_connected ? 1 : 0, bt_slot, conn,
             (modifiers & (MOD_LCTL | MOD_RCTL)) ? 1 : 0,
             (modifiers & (MOD_LALT | MOD_RALT)) ? 1 : 0,
             (modifiers & (MOD_LGUI | MOD_RGUI)) ? 1 : 0,
             (modifiers & (MOD_LSFT | MOD_RSFT)) ? 1 : 0,
             capslock, current_wpm, esp32_volume_muted ? 1 : 0, display,
             zmk_ble_active_profile_is_open() ? 1 : 0,
             usb_ready ? 1 : 0,
             usb_vbus ? 1 : 0,
             usb_hid_ready ? 1 : 0,
             usb_host_active ? 1 : 0,
             ZMK_BLE_PROFILE_COUNT, count);

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

static void send_sub_tile_label_line(int parent, int sub, const char *label)
{
    if (label == NULL || label[0] == '\0') {
        return;
    }

    char safe_label[48];
    sanitize_value(label, safe_label, sizeof(safe_label));

    char line[80];
    snprintf(line, sizeof(line), "sub_tile_label=%d_%d label=%s\n", parent, sub, safe_label);
    uart_write_string(line);
}

static void send_sub_tile_label_lines(void)
{
    if (!device_is_ready(status_uart)) {
        return;
    }

    for (int m = 0; m < 9; m++) {
        for (int s = 0; s < 9; s++) {
            send_sub_tile_label_line(m + 1, s + 1, sub_tile_labels[m][s]);
        }
    }
}

static void send_deck_labels_with_done(void) {
    if (!device_is_ready(status_uart)) {
        return;
    }
    send_deck_label_lines();
    uart_write_string("deck_labels_done\n");
    deck_labels_ack_pending = true;
    k_work_reschedule(&deck_labels_retry_work, K_MSEC(1000));
}

static void deck_labels_retry_work_handler(struct k_work *work) {
    if (deck_labels_ack_pending) {
        send_deck_labels_with_done();
    }
}

static void send_sub_tile_labels_with_done(void) {
    if (!device_is_ready(status_uart)) {
        return;
    }
    send_sub_tile_label_lines();
    uart_write_string("sub_tile_labels_done\n");
    sub_tile_labels_ack_pending = true;
    k_work_reschedule(&sub_tile_labels_retry_work, K_MSEC(1000));
}

static void sub_tile_labels_retry_work_handler(struct k_work *work) {
    if (sub_tile_labels_ack_pending) {
        send_sub_tile_labels_with_done();
    }
}

static void send_bt_profile_name_line(int profile, const char *name) {
    if (name == NULL || name[0] == '\0') {
        return;
    }
    char safe_name[48];
    sanitize_value(name, safe_name, sizeof(safe_name));
    char line[72];
    snprintf(line, sizeof(line), "bt_name=%d label=%s\n", profile, safe_name);
    uart_write_string(line);
}

static void send_bt_profile_name_lines(void) {
    if (!device_is_ready(status_uart)) {
        return;
    }
    int count = MIN((int)ARRAY_SIZE(bt_profile_names), ZMK_BLE_PROFILE_COUNT);
    for (int i = 0; i < count; i++) {
        send_bt_profile_name_line(i + 1, bt_profile_names[i]);
    }
}

static void send_bt_bond_status_lines(void) {
    int active = zmk_ble_active_profile_index();
    int count = MIN((int)ARRAY_SIZE(bt_profile_names), ZMK_BLE_PROFILE_COUNT);
    char line[32];
    for (int i = 0; i < count; i++) {
        bool bonded;
        if (i == active) {
            bonded = !zmk_ble_active_profile_is_open();
        } else if (zmk_ble_profile_is_connected(i)) {
            bonded = true;
        } else {
            continue; /* unknown — let dongle keep its NVS state */
        }
        snprintf(line, sizeof(line), "bt_bond=%d bonded=%d\n", i + 1, bonded ? 1 : 0);
        uart_write_string(line);
    }
}

static void send_bt_names_with_done(void) {
    if (!device_is_ready(status_uart)) {
        return;
    }
    send_bt_profile_name_lines();
    send_bt_bond_status_lines();
    uart_write_string("bt_names_done\n");
    bt_names_ack_pending = true;
    k_work_reschedule(&bt_names_retry_work, K_MSEC(1000));
}

static void bt_names_retry_work_handler(struct k_work *work) {
    if (bt_names_ack_pending) {
        send_bt_names_with_done();
    }
}

static void send_deck_result_line(int tile, int sub, int result) {
    char line[64];
    if (sub > 0) {
        snprintf(line, sizeof(line), "deck_result=%d_%d status=%s code=%d\n",
                 tile, sub, result == 0 ? "ok" : "error", result);
    } else {
        snprintf(line, sizeof(line), "deck_result=%d status=%s code=%d\n",
                 tile, result == 0 ? "ok" : "error", result);
    }
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
    } else if (strstr(command, "cmd=sync") != NULL) {
        name = "sync";
    } else if (strstr(command, "cmd=clear_profile") != NULL) {
        name = "clear_profile";
    } else if (strstr(command, "cmd=clear_all_profiles") != NULL) {
        name = "clear_all_profiles";
    } else if (strstr(command, "cmd=wake") != NULL) {
        name = "wake";
    } else if (strstr(command, "cmd=boot") != NULL) {
        name = "boot";
    } else if (strstr(command, "cmd=vol") != NULL) {
        name = "vol";
    } else if (strstr(command, "cmd=input") != NULL) {
        name = "input";
    } else if (strstr(command, "cmd=req_names") != NULL) {
        name = "req_names";
    } else if (strstr(command, "cmd=req_deck_labels") != NULL) {
        name = "req_deck_labels";
    } else if (strstr(command, "cmd=req_sub_labels") != NULL) {
        name = "req_sub_labels";
    }

    char line[40];
    snprintf(line, sizeof(line), "cmd_ack=%s\n", name);
    uart_write_string(line);
}

static char ascii_from_keycode(uint32_t keycode, bool shifted, bool caps_lock) {
    if (keycode >= 0x04 && keycode <= 0x1D) {
        char c = (char)('a' + (keycode - 0x04));
        return (shifted != caps_lock) ? (char)(c - 32) : c;
    }
    if (keycode >= 0x1E && keycode <= 0x27) {
        static const char normal[] = "1234567890";
        static const char shifted_chars[] = "!@#$%^&*()";
        int idx = keycode == 0x27 ? 9 : (int)(keycode - 0x1E);
        return shifted ? shifted_chars[idx] : normal[idx];
    }
    if (keycode >= 0x59 && keycode <= 0x61) {
        return (char)('1' + (keycode - 0x59));
    }

    switch (keycode) {
    case 0x2C: return ' ';
    case 0x2D: return shifted ? '_' : '-';
    case 0x2E: return shifted ? '+' : '=';
    case 0x2F: return shifted ? '{' : '[';
    case 0x30: return shifted ? '}' : ']';
    case 0x31: return shifted ? '|' : '\\';
    case 0x33: return shifted ? ':' : ';';
    case 0x34: return shifted ? '"' : '\'';
    case 0x35: return shifted ? '~' : '`';
    case 0x36: return shifted ? '<' : ',';
    case 0x37: return shifted ? '>' : '.';
    case 0x38: return shifted ? '?' : '/';
    case 0x62: return '0';
    case 0x63: return '.';
    default: return '\0';
    }
}

static bool send_text_input_key(const struct zmk_keycode_state_changed *ev) {
    if (!esp32_text_input_mode || ev == NULL || !ev->state || ev->usage_page != 0x07) {
        return false;
    }

    if (ev->keycode == 0x2A) {
        uart_write_string("input=bsp\n");
        return true;
    }
    if (ev->keycode == 0x28) {
        uart_write_string("input=enter\n");
        return true;
    }
    if (ev->keycode == 0x58) {
        uart_write_string("input=enter\n");
        return true;
    }
    if (ev->keycode == 0x39) {
        esp32_text_input_caps_lock = !esp32_text_input_caps_lock;
        return true;
    }

    zmk_mod_flags_t mods = zmk_hid_get_keyboard_report()->body.modifiers | ev->implicit_modifiers;
    bool shifted = (mods & (MOD_LSFT | MOD_RSFT)) != 0;
    char c = ascii_from_keycode(ev->keycode, shifted, esp32_text_input_caps_lock);
    if (c == '\0') {
        zmk_hid_keyboard_clear();
        return true;
    }

    char line[16];
    snprintf(line, sizeof(line), "input=%02X\n", (unsigned char)c);
    uart_write_string(line);
    /* Clear the HID keyboard report for every intercepted key.  In ZMK v3 the
       HID listener may run before this one (link order), so it could have
       already added the key to the report.  Clearing here ensures the async
       HID-send work item sees an empty report and nothing reaches the PC. */
    zmk_hid_keyboard_clear();
    return true;
}

static void remember_host_transport(enum zmk_transport transport, int bt_profile) {
    if (transport != ZMK_TRANSPORT_USB && transport != ZMK_TRANSPORT_BLE) {
        return;
    }

    esp32_last_host_transport = transport;
    esp32_last_host_bt_profile = transport == ZMK_TRANSPORT_BLE ? bt_profile : -1;
}

static void park_endpoint_for_text_input(void) {
    if (esp32_text_input_endpoint_parked) {
        return;
    }

    esp32_text_input_saved_preferred = zmk_endpoint_get_preferred_transport();
    esp32_text_input_saved_requested_transport = esp32_requested_transport;
    esp32_text_input_saved_requested_bt_profile = esp32_requested_bt_profile;

    struct zmk_endpoint_instance selected_endpoint = zmk_endpoint_get_selected();
    if (esp32_text_input_saved_preferred == ZMK_TRANSPORT_NONE &&
        selected_endpoint.transport != ZMK_TRANSPORT_NONE) {
        esp32_text_input_saved_preferred = selected_endpoint.transport;
    }
    if (esp32_requested_transport == ZMK_TRANSPORT_USB) {
        remember_host_transport(ZMK_TRANSPORT_USB, -1);
    } else if (esp32_requested_transport == ZMK_TRANSPORT_BLE) {
        remember_host_transport(ZMK_TRANSPORT_BLE, esp32_requested_bt_profile);
    } else if (selected_endpoint.transport == ZMK_TRANSPORT_USB) {
        remember_host_transport(ZMK_TRANSPORT_USB, -1);
    } else if (selected_endpoint.transport == ZMK_TRANSPORT_BLE) {
        remember_host_transport(ZMK_TRANSPORT_BLE, selected_endpoint.ble.profile_index);
    }

    esp32_text_input_endpoint_parked = true;
    esp32_text_input_caps_lock = false;

    esp32_requested_transport = ZMK_TRANSPORT_NONE;
    esp32_requested_bt_profile = -1;
    zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_NONE);
    send_status_now();
}

static void restore_endpoint_after_text_input(void) {
    if (!esp32_text_input_endpoint_parked) {
        return;
    }

    zmk_hid_keyboard_clear();
    zmk_hid_consumer_clear();

    enum zmk_transport restore_transport = esp32_text_input_saved_preferred;
    int restore_bt_profile = esp32_text_input_saved_requested_bt_profile;
    if (restore_transport == ZMK_TRANSPORT_NONE) {
        if (esp32_text_input_saved_requested_transport == ZMK_TRANSPORT_USB ||
            esp32_text_input_saved_requested_transport == ZMK_TRANSPORT_BLE) {
            restore_transport = esp32_text_input_saved_requested_transport;
        } else {
            restore_transport = esp32_last_host_transport;
            restore_bt_profile = esp32_last_host_bt_profile;
        }
    }

    esp32_requested_transport = esp32_text_input_saved_requested_transport;
    esp32_requested_bt_profile = esp32_text_input_saved_requested_bt_profile;
    if (esp32_requested_transport != ZMK_TRANSPORT_USB &&
        esp32_requested_transport != ZMK_TRANSPORT_BLE) {
        esp32_requested_transport = restore_transport;
        esp32_requested_bt_profile = restore_transport == ZMK_TRANSPORT_BLE ? restore_bt_profile : -1;
    }
    esp32_text_input_endpoint_parked = false;

    if (restore_transport == ZMK_TRANSPORT_BLE && restore_bt_profile >= 0 &&
        restore_bt_profile < ZMK_BLE_PROFILE_COUNT) {
        zmk_ble_prof_select(restore_bt_profile);
    }

    zmk_endpoint_set_preferred_transport(restore_transport);
    send_status_now();
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

static bool command_bool_value(const char *line, const char *key, bool *value) {
    const char *match = strstr(line, key);
    if (match == NULL) {
        return false;
    }

    match += strlen(key);
    if (*match == '1' || strncmp(match, "on", 2) == 0 || strncmp(match, "true", 4) == 0) {
        *value = true;
        return true;
    }
    if (*match == '0' || strncmp(match, "off", 3) == 0 || strncmp(match, "false", 5) == 0) {
        *value = false;
        return true;
    }

    return false;
}

static void parse_tile_spec(const char *line, int *parent, int *sub)
{
    *parent = 0;
    *sub = 0;
    const char *tile = strstr(line, "tile=");
    if (tile == NULL) {
        return;
    }
    tile += strlen("tile=");
    *parent = atoi(tile);
    const char *underscore = strchr(tile, '_');
    if (underscore != NULL) {
        *sub = atoi(underscore + 1);
    }
}

static void queue_received_command(void)
{
    if (command_line_len == 0) {
        return;
    }

    command_line[command_line_len] = '\0';
    strncpy(pending_command_line, command_line, sizeof(pending_command_line) - 1);
    pending_command_line[sizeof(pending_command_line) - 1] = '\0';
    pending_command_ready = true;
    command_line_len = 0;
    k_work_reschedule(&receive_command_work, K_NO_WAIT);
}

static void append_received_command_byte(uint8_t c)
{
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        queue_received_command();
        return;
    }

    if (command_line_len < sizeof(command_line) - 1) {
        command_line[command_line_len++] = (char)c;
    } else {
        command_line_len = 0;
    }
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
    const bool reports_mute = binding_is_mute_key(binding);
    if (reports_mute) {
        esp32_mute_suppress_until = k_uptime_get() + 100;
    }

    int ret = zmk_behavior_invoke_binding(binding, event, true);
    if (ret < 0) {
        return ret;
    }

    k_msleep(10);
    event.timestamp = k_uptime_get();
    ret = zmk_behavior_invoke_binding(binding, event, false);
    if (ret >= 0 && reports_mute) {
        report_mute_toggle_from_binding();
    }
    return ret;
}

static int invoke_sub_tile(int parent, int sub)
{
    if (parent < 1 || parent > 9 || sub < 1 || sub > 9) {
        return -EINVAL;
    }

    const struct zmk_behavior_binding *binding = &sub_tile_bindings[parent - 1][sub - 1];
    if (binding->behavior_dev == NULL) {
        return -ENOENT;
    }

    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);
    struct zmk_behavior_binding_event event = {
        .layer = layer_id,
        .position = 0xE100 + (parent - 1) * 9 + (sub - 1),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    const bool reports_mute = binding_is_mute_key(binding);
    if (reports_mute) {
        esp32_mute_suppress_until = k_uptime_get() + 100;
    }

    int ret = zmk_behavior_invoke_binding(binding, event, true);
    if (ret < 0) {
        return ret;
    }

    k_msleep(10);
    event.timestamp = k_uptime_get();
    ret = zmk_behavior_invoke_binding(binding, event, false);
    if (ret >= 0 && reports_mute) {
        report_mute_toggle_from_binding();
    }
    return ret;
}

static int invoke_volume_binding(int index) {
    if (index < 0 || index >= 3) {
        return -EINVAL;
    }
    const struct zmk_behavior_binding *binding = &vol_bindings[index];
    if (binding->behavior_dev == NULL) {
        return -ENOENT;
    }
    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);
    struct zmk_behavior_binding_event event = {
        .layer = layer_id,
        .position = 0xE200,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    const bool reports_mute = binding_is_mute_key(binding);
    if (reports_mute) {
        esp32_mute_suppress_until = k_uptime_get() + 100;
    }
    int ret = zmk_behavior_invoke_binding(binding, event, true);
    if (ret < 0) {
        return ret;
    }
    k_msleep(10);
    event.timestamp = k_uptime_get();
    ret = zmk_behavior_invoke_binding(binding, event, false);
    if (ret >= 0 && reports_mute) {
        if (esp32_skip_invoked_mute_report) {
            esp32_skip_invoked_mute_report = false;
        } else {
            report_mute_toggle_from_binding();
        }
    }
    return ret;
}

static void handle_command_line(const char *line) {
    if (strstr(line, "ack=bt_names") != NULL) {
        bt_names_ack_pending = false;
        k_work_cancel_delayable(&bt_names_retry_work);
        return;
    }

    if (strstr(line, "ack=deck_labels") != NULL) {
        deck_labels_ack_pending = false;
        k_work_cancel_delayable(&deck_labels_retry_work);
        return;
    }

    if (strstr(line, "ack=sub_tile_labels") != NULL) {
        sub_tile_labels_ack_pending = false;
        k_work_cancel_delayable(&sub_tile_labels_retry_work);
        return;
    }

    send_command_ack_line(line);

    if (strstr(line, "cmd=input") != NULL) {
        if (strstr(line, "off") != NULL) {
            esp32_text_input_mode = false;
            restore_endpoint_after_text_input();
        } else {
            esp32_text_input_mode = true;
            park_endpoint_for_text_input();
        }
        return;
    }

    if (strstr(line, "cmd=usb") != NULL) {
        esp32_requested_transport = ZMK_TRANSPORT_USB;
        esp32_requested_bt_profile = -1;
        remember_host_transport(ZMK_TRANSPORT_USB, -1);
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=bt") != NULL) {
        int profile = command_profile_index(line);
        if (profile >= 0 && profile < ZMK_BLE_PROFILE_COUNT) {
            zmk_ble_prof_select(profile);
            esp32_requested_bt_profile = profile;
        }
        esp32_requested_transport = ZMK_TRANSPORT_BLE;
        remember_host_transport(ZMK_TRANSPORT_BLE, esp32_requested_bt_profile);
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
        int parent = 0, sub = 0;
        parse_tile_spec(line, &parent, &sub);
        if (sub > 0) {
            int ret = invoke_sub_tile(parent, sub);
            send_deck_result_line(parent, sub, ret);
        } else {
            int ret = invoke_deck_tile(parent);
            send_deck_result_line(parent, 0, ret);
        }
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=wake") != NULL) {
        set_state(ZMK_ACTIVITY_ACTIVE);
        send_status_now();
        return;
    }

    if (strstr(line, "cmd=sync") != NULL) {
        send_status_now();
        send_deck_labels_with_done();
        send_sub_tile_labels_with_done();
        send_bt_names_with_done();
        return;
    }

    if (strstr(line, "cmd=req_names") != NULL) {
        send_bt_names_with_done();
        return;
    }

    if (strstr(line, "cmd=req_deck_labels") != NULL) {
        send_deck_labels_with_done();
        return;
    }

    if (strstr(line, "cmd=req_sub_labels") != NULL) {
        send_sub_tile_labels_with_done();
        return;
    }

    if (strstr(line, "cmd=boot") != NULL) {
        nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
        /* Use direct ARM reset to bypass Zephyr reboot hooks that can clear GPREGRET in Zephyr 3.x (ZMK v3). */
        NVIC_SystemReset();
        return;
    }

    if (strstr(line, "cmd=vol") != NULL) {
        if (strstr(line, "up") != NULL) {
            invoke_volume_binding(0);
        } else if (strstr(line, "down") != NULL) {
            invoke_volume_binding(1);
        } else if (strstr(line, "mute") != NULL) {
            bool requested_muted;
            if (command_bool_value(line, "state=", &requested_muted) ||
                command_bool_value(line, "mute=", &requested_muted)) {
                esp32_volume_muted = requested_muted;
                send_mute_state_line();
                esp32_mute_suppress_until = k_uptime_get() + 100;
                esp32_skip_invoked_mute_report = true;
            }
            invoke_volume_binding(2);
        }
        return;
    }
}

static void receive_command_work_handler(struct k_work *work) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    if (pending_command_ready) {
        char line[sizeof(pending_command_line)];
        strncpy(line, pending_command_line, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        pending_command_ready = false;
        handle_command_line(line);
    }
#else
    unsigned char c;
    while (uart_poll_in(status_uart, &c) == 0) {
        append_received_command_byte(c);
        if (pending_command_ready) {
            char line[sizeof(pending_command_line)];
            strncpy(line, pending_command_line, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            pending_command_ready = false;
            handle_command_line(line);
        }
    }

    k_work_reschedule(&receive_command_work, K_MSEC(30));
#endif
}

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
static void status_uart_callback(const struct device *dev, void *user_data)
{
    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (!uart_irq_rx_ready(dev)) {
            continue;
        }

        uint8_t buffer[16];
        int read;
        do {
            read = uart_fifo_read(dev, buffer, sizeof(buffer));
            for (int i = 0; i < read; ++i) {
                append_received_command_byte(buffer[i]);
            }
        } while (read == sizeof(buffer));
    }
}
#endif

static int esp32_status_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *keycode_ev = as_zmk_keycode_state_changed(eh);
    if (keycode_ev != NULL) {
        if (send_text_input_key(keycode_ev)) {
            return ZMK_EV_EVENT_HANDLED;
        }
        if (keycode_ev->state && is_mute_keycode(keycode_ev)) {
            int64_t now = k_uptime_get();
            if (esp32_mute_suppress_until > 0 && now < esp32_mute_suppress_until) {
                esp32_mute_suppress_until = 0;
            } else {
                esp32_volume_muted = !esp32_volume_muted;
                send_mute_state_line();
            }
        }
        schedule_status_send();
        return 0;
    }

    if (as_zmk_modifiers_state_changed(eh) != NULL) {
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

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    int ret = uart_irq_callback_user_data_set(status_uart, status_uart_callback, NULL);
    if (ret < 0) {
        return ret;
    }
    uart_irq_rx_enable(status_uart);
#else
    k_work_reschedule(&receive_command_work, K_MSEC(30));
#endif

    schedule_status_send();
    send_deck_labels_with_done();
    send_sub_tile_labels_with_done();
    send_bt_names_with_done();
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
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(esp32_status_uart, zmk_endpoint_changed);
