# ZMK ESP32 Display Dongle Module

This is a stackable ZMK module/shield that sends keyboard status to the ESP32-S3
touch display over UART.

## Wiring

| nice!nano v2 | ESP32 screen |
| --- | --- |
| P0.06 TX | GPIO43 RXD |
| P0.08 RX | GPIO44 TXD |
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

## Status Protocol

The module sends newline-terminated text packets:

```text
layer=Default usb=1 bt=0 bt_slot=1 ctrl=0 alt=0 win=0 shift=0 wpm=42 peripherals=2 batt=91,88
```

The ESP32 display currently uses:

- `layer`
- `usb`
- `bt`
- `bt_slot`
- `ctrl`, `alt`, `win`, `shift`
- `wpm`
- `peripherals`
- `batt`, comma-separated peripheral battery percentages

If no split peripheral battery reports have arrived yet, the module sends
`peripherals=0`.
