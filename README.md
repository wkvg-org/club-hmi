# club-display

Bar-dashboard touch-screen firmware for the Waveshare ESP32-P4-NANO
10.1" MIPI-DSI panel. LVGL-based UI with time/date display, event
schedule (Directus CMS), DMX lighting control, and settings.

## Hardware

- **MCU**: ESP32-P4 (hpcore, pre-v3 silicon, 360 MHz)
- **Display**: 10.1" MIPI-DSI panel, 800×1280 portrait, mounted landscape @ 270°
- **Touch**: GT911 controller, polled via I²C
- **Ethernet**: EMAC with TLK110 PHY, static IP 192.168.2.130

## Architecture

```
src/
├── main.c              Entry point, display init, backend threads, LVGL loop
└── modules/
    ├── config/          SD-card configuration (bar_dashboard.json, creds.json)
    ├── net/             Ethernet bring-up, MQTT client, DMX UDP sender, Directus
    ├── time/            NTP time sync (raw UDP, CET/CEST DST)
    ├── ui/              LVGL dashboard (tabs: overview, pager, DMX, alarm, settings)
    ├── shell/           Zephyr shell commands (dmx, net, ui, backlight)
    └── ui/assets/       LVGL fonts (Free Mono Bold 24/48, Oswald Regular 36)

module/                  Out-of-tree Zephyr module
├── boards/              Custom board definition (waveshare_esp32p4_nano)
├── drivers/
│   ├── mipi_dsi/        MIPI-DSI host driver (espressif,esp-mipi-dsi)
│   ├── display/         DPI framebuffer scan-out + JD9365 panel + panel-ctl
│   └── input/           GT911 polled touch driver
├── dts/bindings/        Devicetree binding YAMLs
└── include/             Public driver API headers
```

## Build

Requires a Zephyr workspace at `~/zephyrproject`.

```bash
source ~/zephyrproject/.venv/bin/activate
west blobs fetch hal_espressif

west build --board waveshare_esp32p4_nano/esp32p4/hpcore \
           --build-dir build-nano club-display

west flash --build-dir build-nano --esp-device /dev/ttyACM0
```

## Shell commands

```
dmx color <name|0..255>   DMX colour wheel
dmx dimmer <0..255>       DMX dimmer
dmx blackout              Blackout
dmx home                  Home position

backlight <0..100>        LCD backlight percentage

net status                Show network status

ui tab <overview|pager|light|alarm|settings>
ui alarm <on|off>         Blink alarm tab button
```

## License

SPDX-License-Identifier: Apache-2.0

## Copyright

Copyright (c) 2025–2026 Wolfenbütteler Kultur- und Veranstaltungsgesellschaft
gemeinnützige UG (haftungsbeschränkt)
