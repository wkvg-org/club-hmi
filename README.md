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

## SD Card Configuration

Configuration is loaded from the FAT32 SD card at boot. All files are JSON
and optional — built-in defaults are used when a file is absent.

### `/SD:/bar_dashboard.json`

```json
{
  "data": {
    "device_name": "bar-dashboard",
    "mqtt_server_ip": "192.168.2.128",
    "mqtt_server_domain": "mqtt.moai.sk",
    "mqtt_server_port": 1883,
    "directus_server_ip": "192.168.2.128",
    "directus_server_domain": "directus.moai.sk",
    "directus_server_port": 443,
    "ntp_server_domain_1": "192.53.103.108",
    "ntp_server_domain_2": "192.53.103.104",
    "ntp_server_domain_3": "192.53.103.103",
    "ntp_server_port": 123
  }
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `device_name` | string | `bar-dashboard` | Device identifier |
| `mqtt_server_ip` | string | `192.168.2.128` | MQTT broker IP (preferred over domain) |
| `mqtt_server_domain` | string | `mqtt.moai.sk` | MQTT broker hostname |
| `mqtt_server_port` | number | `1883` | MQTT broker port |
| `directus_server_ip` | string | `192.168.2.128` | Directus CMS IP |
| `directus_server_domain` | string | `directus.moai.sk` | Directus CMS hostname |
| `directus_server_port` | number | `443` | Directus HTTPS port |
| `ntp_server_domain_1-3` | string | PTB servers | NTP time server IPs or hostnames |
| `ntp_server_port` | number | `123` | NTP port |

### `/SD:/creds.json`

```json
{
  "mqtt": {
    "username": "",
    "password": ""
  },
  "directus": {
    "bearer_token": ""
  }
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `mqtt.username` | string | `""` | MQTT broker username (empty = anonymous) |
| `mqtt.password` | string | `""` | MQTT broker password |
| `directus.bearer_token` | string | `""` | Directus API bearer token |

### `/SD:/settings.json`

Written by the firmware when the backlight slider is moved. Read at boot.

```json
{"backlight": 80}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `backlight` | number | `100` | LCD backlight brightness (0–100 %) |

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
