# ZMK ESP32 Display Dongle Module

This is a stackable ZMK module/shield that sends keyboard status to the ESP32-S3
touch display over UART.

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
west build -b nice_nano_v2 -- -DSHIELD="your_keyboard_shield esp32_status_dongle"
```

For a GitHub Actions ZMK config, add this repository/module to `config/west.yml`
and include `esp32_status_dongle` in the shield list for the nice!nano build.

## Keyboard Firmware Setup

Most users should modify their keyboard firmware repo, not this module. The
module provides the shield and UART code; your keyboard config chooses to include
it and sets any labels/options you want.

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
  - board: nice_nano_v2
    shield: your_keyboard_shield esp32_status_dongle
```

For Xiao BLE builds, use your Xiao board name and the same shield:

```yaml
include:
  - board: seeeduino_xiao_ble
    shield: your_keyboard_shield esp32_status_dongle
```

Local build example:

```sh
west build -b nice_nano_v2 -- -DSHIELD="your_keyboard_shield esp32_status_dongle"
```

### 3. Add Optional Labels In Your `.conf`

Put simple deck labels in your keyboard `.conf` file, for example
`config/your_keyboard.conf`:

```conf
CONFIG_ZMK_ESP32_DECK_TILE_1_LABEL="Mute"
CONFIG_ZMK_ESP32_DECK_TILE_2_LABEL="Layer"
CONFIG_ZMK_ESP32_DECK_TILE_3_LABEL="Build"
CONFIG_ZMK_ESP32_DECK_TILE_4_LABEL="Terminal"
CONFIG_ZMK_ESP32_DECK_TILE_5_LABEL="Weather"
CONFIG_ZMK_ESP32_DECK_TILE_6_LABEL="Blank"
```

The ESP32 displays these labels when the SD card does not provide an explicit
label or icon for that tile. If the SD card has `mute1.bmp`, `terminal4.bmp`, or
a `/settings/deck.json` label, the SD card visual config wins.

### 4. Current Deck Action Behavior

When you press a deck tile, the ESP32 sends:

```text
cmd=deck tile=1
```

At the moment, the module receives that command and refreshes status. The next
piece is to map `tile=1`, `tile=2`, etc. to real ZMK behaviors/macros from your
keyboard config, so actions can stay in ZMK instead of on the ESP32.

Planned shape:

```dts
esp32_deck {
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

That binding layer is not implemented yet.

## Status Protocol

The module sends newline-terminated text packets:

```text
layer=Default usb=1 bt=0 bt_slot=1 conn=usb ctrl=0 alt=0 win=0 shift=0 capslock=0 wpm=42 bt_profiles=5 peripherals=2 batt=91,88
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
```

Profile numbers are one-based on the display, so `profile=1` selects ZMK BLE
profile index `0`.

Deck tile presses use the same UART command channel:

```text
cmd=deck tile=1
```

## Deck Labels

Simple deck labels can be configured in ZMK Kconfig and sent to the display:

```conf
CONFIG_ZMK_ESP32_DECK_TILE_1_LABEL="Mute"
CONFIG_ZMK_ESP32_DECK_TILE_2_LABEL="Layer"
CONFIG_ZMK_ESP32_DECK_TILE_3_LABEL="Build"
```

The module sends them as:

```text
deck_tile=1 label=Mute
```

Spaces are sent as underscores over UART and displayed as spaces by the ESP32.
If the ESP32 SD card has an icon or explicit label for a tile, that SD card
visual config wins over these simple ZMK labels.
