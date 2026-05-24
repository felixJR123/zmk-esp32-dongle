# ZMK ESP32 Display Dongle Module

This is a stackable ZMK module/shield that sends keyboard status to the ESP32-S3
touch display over UART.

The ESP32 screen can show keyboard state from ZMK and can also send simple
commands back to ZMK.

Current features:

- sends active layer name
- sends USB/Bluetooth connection state
- sends selected Bluetooth profile
- sends preferred transport as `conn=usb`, `conn=bt`, or `conn=none`
- sends modifier state: Ctrl, Alt, GUI/Win, Shift
- sends Caps Lock state when ZMK HID indicators are enabled
- sends WPM when ZMK WPM is enabled
- sends display wake/sleep state as `display=on` or `display=off`
- sends split peripheral battery reports when available
- sends the number of configured Bluetooth host profiles
- receives commands from the ESP32 to switch USB/Bluetooth profiles
- receives commands from the ESP32 to clear the active host profile or all host profiles
- receives a wake command from the ESP32 touchscreen
- receives deck tile presses from the ESP32 and invokes configured ZMK bindings
- sends simple deck tile labels from ZMK config to the ESP32

The ESP32 still owns the visual side of the deck profile: icons, colors,
transparency, and SD card picture assets. If the SD card has an icon or explicit
label for a tile, that SD card visual config wins over labels sent by ZMK.

## Wiring

**nice!nano v2**

| nice!nano v2 | ESP32 screen |
| --- | --- |
| P0.06 TX | GPIO43 RXD |
| P0.08 RX | GPIO44 TXD |
| GND | GND |

**Xiao BLE**

| Xiao BLE | ESP32 screen |
| --- | --- |
| P1.11 TX (D6) | GPIO43 RXD |
| P1.12 RX (D7) | GPIO44 TXD |
| GND | GND |

UART speed is `115200`.

## Build

Add this folder as a ZMK module, then stack the shield with your real keyboard
shield:

```sh
west build -b nice_nano//zmk -- -DSHIELD="your_keyboard_shield esp32_status_dongle"
```

For a GitHub Actions ZMK config, add this repository/module to `config/west.yml`
and include `esp32_status_dongle` in the shield list for the nice!nano build.

## Keyboard Firmware Setup

Most users should modify their keyboard firmware repo, not this module. The
module provides the shield and UART code; your keyboard config chooses to include
it and sets any labels/options you want.

To make the ESP32 screen and backlight turn off after 5 minutes of inactivity,
set the normal ZMK idle timeout in your keyboard `.conf`:

```conf
CONFIG_ZMK_IDLE_TIMEOUT=300000
```

The ESP32 still owns brightness. ZMK only sends whether the display should be on
or off; Settings > Backlight on the screen controls the local backlight level.

### 1. Add The Module To `config/west.yml`

In your ZMK config repo, add this module under `projects`. Use your fork if you
forked the module:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: felix
      url-base: https://github.com/felixJR123

  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-esp32-dongle
      remote: felix
      revision: main

  self:
    path: config
```

### 2. Stack The Shield In Your Build

Add `esp32_status_dongle` beside your normal keyboard shield. Example
`build.yaml`:

```yaml
include:
  - board: nice_nano//zmk
    shield: your_keyboard_shield esp32_status_dongle
```

For Xiao BLE builds, use your Xiao board name and the same shield:

```yaml
include:
  - board: xiao_ble//zmk
    shield: your_keyboard_shield esp32_status_dongle
```

You can also spell the same ZMK board variants with the full SoC qualifier, for
example `nice_nano/nrf52840/zmk` or `xiao_ble/nrf52840/zmk`.

Local build example:

```sh
west build -b nice_nano//zmk -- -DSHIELD="your_keyboard_shield esp32_status_dongle"
```

### 3. Add Deck Labels In Your `.overlay`

Put deck labels in your keyboard `.overlay` file, for example
`config/your_keyboard.overlay`:

```dts
#include <dt-bindings/zmk/keys.h>

&esp32_deck {
    tile_1 {
        label = "Mute";
    };

    tile_2 {
        label = "Layer";
    };

    tile_3 {
        label = "Build";
    };

    tile_4 {
        label = "Terminal";
    };

    tile_5 {
        label = "Weather";
    };

    tile_6 {
        label = "Blank";
    };

    tile_7 {
        label = "Desktop";
    };

    tile_8 {
        label = "Media";
    };

    tile_9 {
        label = "Settings";
    };
};
```

The shield creates the `&esp32_deck` node, so your keyboard overlay only fills in
the tile settings. The ESP32 displays these labels when the SD card does not
provide an icon for that tile. If the SD card has `mute1.bmp`, `terminal4.bmp`,
or another tile icon, the SD card icon visual config wins.

Optional `.conf` labels still work as a fallback or quick test:

```conf
CONFIG_ZMK_ESP32_DECK_TILE_1_LABEL="Mute"
CONFIG_ZMK_ESP32_DECK_TILE_2_LABEL="Layer"
CONFIG_ZMK_ESP32_DECK_TILE_3_LABEL="Build"
CONFIG_ZMK_ESP32_DECK_TILE_4_LABEL="Terminal"
CONFIG_ZMK_ESP32_DECK_TILE_5_LABEL="Weather"
CONFIG_ZMK_ESP32_DECK_TILE_6_LABEL="Blank"
CONFIG_ZMK_ESP32_DECK_TILE_7_LABEL="Desktop"
CONFIG_ZMK_ESP32_DECK_TILE_8_LABEL="Media"
CONFIG_ZMK_ESP32_DECK_TILE_9_LABEL="Settings"
```

If both are set, the `.overlay` label wins over the `.conf` fallback. All nine
fallback label options are available:

```conf
CONFIG_ZMK_ESP32_DECK_TILE_1_LABEL="Tile 1"
CONFIG_ZMK_ESP32_DECK_TILE_2_LABEL="Tile 2"
CONFIG_ZMK_ESP32_DECK_TILE_3_LABEL="Tile 3"
CONFIG_ZMK_ESP32_DECK_TILE_4_LABEL="Tile 4"
CONFIG_ZMK_ESP32_DECK_TILE_5_LABEL="Tile 5"
CONFIG_ZMK_ESP32_DECK_TILE_6_LABEL="Tile 6"
CONFIG_ZMK_ESP32_DECK_TILE_7_LABEL="Tile 7"
CONFIG_ZMK_ESP32_DECK_TILE_8_LABEL="Tile 8"
CONFIG_ZMK_ESP32_DECK_TILE_9_LABEL="Tile 9"
```

### 4. What The ESP32 Sends Back To ZMK

The ESP32 sends one-line UART commands back to ZMK.

Switch to USB:

```text
cmd=usb
```

Switch to Bluetooth profile 1:

```text
cmd=bt profile=1
```

Switch to Bluetooth profile 5:

```text
cmd=bt profile=5
```

Clear only the currently selected Bluetooth host profile:

```text
cmd=clear_profile
```

Clear all Bluetooth host profiles:

```text
cmd=clear_all_profiles
```

Peripherals are intentionally not cleared by these commands. Use a full ZMK
settings reset when you really want to remove peripheral pairing information.

Wake the dongle from a screen touch:

```text
cmd=wake
```

### 5. Deck Action Behavior

When you press a deck tile, the ESP32 sends:

```text
cmd=deck tile=1
```

The module looks up `tile_1`, presses its `bindings`, waits briefly, then
releases the same binding. That lets the ESP32 stay simple while ZMK owns the
actual keyboard behavior.

### 6. Deck Actions In `.overlay`

Labels and actions both live in the same `.overlay` tile nodes:

```dts
&esp32_deck {
    tile_1 {
        label = "Mute";
        bindings = <&kp C_MUTE>;
    };

    tile_2 {
        label = "Layer";
        bindings = <&to 1>;
    };
};
```

The `bindings` property can run normal ZMK behavior such as:

```dts
bindings = <&kp C_MUTE>;
bindings = <&to 1>;
bindings = <&macro_my_custom_macro>;
```

If a tile has no `bindings`, pressing it on the ESP32 only refreshes status.

## Status Protocol

The module sends newline-terminated text packets:

```text
layer=Default usb=1 bt=0 bt_slot=1 conn=usb ctrl=0 alt=0 win=0 shift=0 capslock=0 wpm=42 display=on bt_profiles=5 peripherals=2 batt=91,88
```

The ESP32 display currently uses:

- `layer`
- `usb`
- `bt`
- `bt_slot`
- `conn`, preferred transport: `usb`, `bt`, or `none`
- `ctrl`, `alt`, `win`, `shift`
- `capslock`
- `wpm`
- `display`, `on` while ZMK is active and `off` while ZMK is idle or asleep
- `bt_profiles`
- `peripherals`
- `batt`, comma-separated peripheral battery percentages

If no split peripheral battery reports have arrived yet, the module sends
`peripherals=0`.

## Display Commands

The ESP32 can send newline-terminated commands back to ZMK over the same UART:

```text
cmd=usb
cmd=bt profile=1
cmd=clear_profile
cmd=clear_all_profiles
cmd=wake
```

Profile numbers are one-based on the display, so `profile=1` selects ZMK BLE
profile index `0`.

Deck tile presses use the same UART command channel:

```text
cmd=deck tile=1
```

## Deck Labels

Simple deck labels can be configured in your keyboard `.overlay` and sent to the
display:

```dts
&esp32_deck {
    tile_1 {
        label = "Mute";
        bindings = <&kp C_MUTE>;
    };
};
```

The shield creates `&esp32_deck`; your overlay just adds labels and optional
bindings to each tile.

The older `.conf` fallback also works:

```conf
CONFIG_ZMK_ESP32_DECK_TILE_1_LABEL="Mute"
CONFIG_ZMK_ESP32_DECK_TILE_2_LABEL="Layer"
CONFIG_ZMK_ESP32_DECK_TILE_3_LABEL="Build"
CONFIG_ZMK_ESP32_DECK_TILE_4_LABEL="Terminal"
CONFIG_ZMK_ESP32_DECK_TILE_5_LABEL="Weather"
CONFIG_ZMK_ESP32_DECK_TILE_6_LABEL="Blank"
CONFIG_ZMK_ESP32_DECK_TILE_7_LABEL="Desktop"
CONFIG_ZMK_ESP32_DECK_TILE_8_LABEL="Media"
CONFIG_ZMK_ESP32_DECK_TILE_9_LABEL="Settings"
```

The module sends them as:

```text
deck_tile=1 label=Mute
```

Spaces are sent as underscores over UART and displayed as spaces by the ESP32.
If the ESP32 SD card has an icon or explicit label for a tile, that SD card
visual config wins over these simple ZMK labels.
