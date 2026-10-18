# club-hmi

Bar-dashboard touch-screen firmware for the Waveshare ESP32-P4-NANO
10.1" MIPI-DSI panel. LVGL-based UI with time/date display, event
schedule (Directus CMS), DMX lighting control, and settings.

## Hardware

- **MCU**: ESP32-P4 (hpcore, pre-v3 silicon)
- **Display**: 10.1" MIPI-DSI panel, 800×1280 portrait, mounted landscape @
  270° (logical 1280×800). Framebuffer lives in PSRAM (does not fit in the
  384 KB hpcore SRAM).
- **Touch**: GT911 controller, polled via I²C.
- **Ethernet**: EMAC with TLK110 PHY (see *Network* below).

## Architecture

```
src/
├── main.c              Entry point, display init, backend threads, LVGL loop
└── modules/
    ├── config/          SD-card configuration (bar_dashboard.json, creds.json, settings.json)
    ├── net/             Ethernet bring-up, MQTT client, DMX sender, Directus
    ├── time/            NTP time sync (SNTP, CET/CEST DST)
    ├── ui/              LVGL dashboard (tabs: overview, pager, light, alarm, settings)
    ├── shell/           Zephyr shell commands (dmx, net, ui, backlight)
    └── ui/assets/       LVGL fonts (Free Mono Bold 24/48, Oswald Regular 36)

module/                  Out-of-tree Zephyr module
├── boards/              Custom board definition (waveshare_esp32p4_nano)
├── drivers/
│   ├── display/         JD9365 panel + Waveshare panel-ctl (power/backlight)
│   └── input/           GT911 polled touch driver
├── dts/bindings/        Devicetree binding YAMLs
└── include/             Public driver API headers
```

The MIPI-DSI host and the DPI scan-out controller are the **upstream Zephyr
drivers** (`drivers/mipi_dsi/dsi_esp32.c`, `drivers/display/display_esp32_dsi.c`,
enabled by `CONFIG_MIPI_DSI_ESP32` / `CONFIG_DISPLAY_ESP32_DSI`). The 800×1280
panel is mounted landscape; `src/display_rotation.c` rotates the LVGL output
(270°) into a portrait framebuffer that the display controller scans out.

### UI tabs

| Index | Tab | Content |
|-------|-----|---------|
| 0 | Overview (Übersicht) | Day acts (from Directus) |
| 1 | Pager | Placeholder |
| 2 | Light (Lichtsteuerung) | DMX moving-head control (colour grid, dimmer) |
| 3 | Alarm (Alarmanlage) | Placeholder |
| 4 | Settings (Einstellungen) | Backlight / settings |

A right-hand sidebar shows the current time/date and the tab buttons.

### Network

Ethernet (EMAC + TLK110 PHY) is currently **disabled in the board DTS** due to
an EMAC/LVGL conflict on this pre-v3 silicon. The network stack is still
compiled so the backend threads build, and they stay dormant until
`net_is_up()` reports a preferred IPv4 address.

- Static IPv4 `192.168.2.130/24`, gateway `192.168.2.1` (`prj.conf`, DHCP
  disabled).
- `net_init()` registers a net-mgmt watcher for `NET_EVENT_IPV4_ADDR_ADD/DEL`
  and flips `net_is_up()`.

### Backend threads

Three threads start after `net_init()` and each block internally on
`net_is_up()`:

| Thread | Priority | Purpose |
|--------|----------|---------|
| NTP | 6 | SNTP time sync, CET/CEST conversion, periodic re-sync (10 min) |
| MQTT | 5 | DMX publisher + lamp control (`node-red/...`) |
| Directus | 4 | Fetches the day's acts over HTTPS |

The main thread runs the LVGL event loop at ~200 Hz (5 ms tick) plus a 1 s
timer for the clock/overview refresh.

## MQTT

The HMI is a pure publisher (never subscribes). Broker is `192.168.2.128:1883`
(anonymous), client ID `bar-dashboard`.

| Topic | Payload | Purpose |
|-------|---------|---------|
| `dmx/mh/<addr>/color` | 0..255 | Moving-head colour wheel |
| `dmx/mh/<addr>/dimmer` | 0..255 | Dimmer |
| `dmx/mh/<addr>/pan` | 0..540 | Pan |
| `dmx/mh/<addr>/tilt` | 0..200 | Tilt |
| `dmx/mh/<addr>/strobe` | 0..255 | Strobe |
| `dmx/mh/<addr>/home` | — | Home position |
| `dmx/mh/<addr>/blackout` | — | Blackout |
| `dmx/fixture/<addr>` | 0/255 | Relay fixture on/off |
| `dmx/addr` | 1..504 | DMX start address |
| `node-red/lamp/power/set` | `ON`/`OFF` | Lamp power |
| `node-red/lamp/brightness/set` | 0..100 | Lamp brightness |

## SD Card Configuration

Configuration is loaded from the FAT32 SD card at boot (`/SD:`). All files are
JSON and optional — built-in defaults are used when a file is absent.

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
| `mqtt_server_ip` | string | `192.168.2.128` | MQTT broker IP |
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

## Startup & system dependencies

Boot order in `src/main.c`:

| # | Step | Depends on |
|---|------|-----------|
| 1 | Panel power sequencing (backlight held off), MIPI-DSI attach + JD9365 init | on-board panel |
| 2 | LVGL display setup + software rotation framebuffer (270°) | display device, PSRAM |
| 3 | `init_config()` + `settings_load()` (brightness is saved, not yet applied) | SD card (optional; compiled defaults otherwise) |
| 4 | `ui_init_and_build()` — LVGL dashboard, then first complete framebuffer write | display |
| 5 | Apply saved backlight level after first successful full-frame `display_write()` | panel-ctl driver |
| 6 | `net_init()` — Ethernet EMAC watcher | network link (static IP, DHCP disabled) |
| 7 | NTP thread (prio 6) — time sync | NTP servers (`192.53.103.108/104/103:123`) |
| 8 | MQTT thread (prio 5) — publishes DMX/lamp topics | MQTT broker `192.168.2.128:1883` (anonymous) |
| 9 | Directus thread (prio 4) — fetches event schedule | Directus CMS `directus.moai.sk:443` (HTTPS, bearer token) |
| 10 | LVGL event loop (main thread, ~200 Hz) | display + all backends |

**Key external dependencies**:
- **MQTT broker** `192.168.2.128:1883` (mosquitto, anonymous). The HMI is a
  pure publisher.
- **Directus CMS** at `directus.moai.sk` (HTTPS 443, bearer token from
  `creds.json`) for the daily event schedule. TLS verification is disabled
  (matching the Arduino reference).
- **NTP** for wall-clock time / date on the dashboard sidebar.

## Build

Requires a Zephyr workspace at `~/zephyrproject`.

```bash
source ~/zephyrproject/.venv/bin/activate
west blobs fetch hal_espressif

west build --board waveshare_esp32p4_nano/esp32p4/hpcore \
           --build-dir build-nano club-hmi

west flash --build-dir build-nano --esp-device /dev/ttyACM0
```

## Shell commands

```
dmx color <name|0..255>    DMX colour wheel (white red orange yellow green blue cyan purple)
dmx dimmer <0..255>        DMX dimmer
dmx blackout               Blackout
dmx home                   Home position
dmx fixture <on|off>       Relay fixture (address 19)

backlight <0..100>         LCD backlight percentage

net status                 Show network status

ui tab <overview|pager|light|alarm|settings>
ui alarm <on|off>          Blink alarm tab button
```

## License

SPDX-License-Identifier: Apache-2.0

## Copyright

Copyright (c) 2025–2026 Wolfenbütteler Kultur- und Veranstaltungsgesellschaft
gemeinnützige UG (haftungsbeschränkt)
