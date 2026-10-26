# Durk for Stack-chan (M5Stack CoreS3)

A talking, head-turning desk robot built on the M5Stack CoreS3 Stack-chan. Durk runs custom
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) firmware and talks to a
[xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) running on your own PC,
using xAI Grok for the brain and voice.

**New here? Start with [SETUP.md](SETUP.md).**

## Features
- Animated Stack-chan face with lip sync, plus themes (classic, galaxy, more in `server/data/faces`)
- Head servos (yaw/pitch), face tracking with the camera, idle glances
- 12 body RGB LEDs: timer countdown ring, mood colors
- Reminders, timers, a morning rundown, long-term memory (saved on PC + SD card)
- Optional Google Calendar and an optional Google Sheets job tracker (off by default)
- NFC card actions, kid mode, "unhinged" mode, smart-home trigger endpoint

## Layout
```
firmware/   full modified xiaozhi-esp32 v2.5.2 tree (build output excluded)
  sdkconfig.durk      sdkconfig for the M5Stack CoreS3 build
server/     files added/changed in xiaozhi-esp32-server v0.9.7 (main/xiaozhi-server)
  upstream-changes.patch  diff of modified upstream files
  config.template.yaml    copy to data/.config.yaml and fill in
  components/esp-wifi-connect  local copy of 78/esp-wifi-connect 3.3.x (MIT, Terrence) with a Server address field
tools/      helper scripts (galaxy background generator)
```

## Settings you will change
| Setting | Where | Default |
|---|---|---|
| Server address | Wi-Fi setup page "Server address" field (saved on the robot); `CONFIG_OTA_URL` is the fallback; `server.websocket` / `vision_explain` in config | `YOUR_PC_IP` |
| xAI API key | `server/data/.config.yaml` | `YOUR_XAI_API_KEY` |
| Time zone (server) | `DURK_TZ` environment variable (IANA name, e.g. `America/Chicago`) | `UTC` |
| Time zone (robot) | `CONFIG_DURK_TZ` (POSIX string, e.g. `CST6CDT,M3.2.0,M11.1.0`) | `UTC0` |
| Home city (weather) | `DURK_LOCATION` environment variable | `New York, NY` |
| Job tracker sheet | `DURK_JOB_SHEET_ID` environment variable | off |

## Firmware details
### Partition table (`partitions/v2/16m_durk.csv`, 16 MB flash)
| name | type | offset | size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x4000 |
| otadata | data/ota | 0xd000 | 0x2000 |
| phy_init | data/phy | 0xf000 | 0x1000 |
| ota_0 | app | **0x20000** | 0x7E0000 |
| assets | data/spiffs | 0x800000 | 8M |

Single app slot (esp-dl needs the space), so OTA firmware updates are not supported.

### Flash
Full (first time / partition change — erases Wi-Fi):
```
python -m esptool --chip esp32s3 -p COM7 -b 921600 erase-flash
python -m esptool --chip esp32s3 -p COM7 -b 921600 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0xd000 build/ota_data_initial.bin 0x20000 build/xiaozhi.bin 0x800000 build/generated_assets.bin
```
App only (normal updates):
```
python -m esptool --chip esp32s3 -p COM7 -b 921600 write-flash 0x20000 build/xiaozhi.bin
```
Wi-Fi setup after an erase: join the `Xiaozhi-XXXX` hotspot from your phone, open the setup page it shows, pick your network; leave the OTA field blank.

### Device MCP tools
`robot.look` (left/right from the *user's* view), `robot.set_head_angles` (negative yaw = user's left),
`robot.get_head_angles`, `robot.nod`, `robot.shake_head`, `robot.look_around`,
`robot.set_led_color`, `robot.led_off`, `robot.set_led_brightness`,
`robot.set_timer`, `robot.cancel_timer`, `robot.get_timer`, face-tracking tools, `self.camera.take_photo`.


## Server details
### HTTP endpoints (port 8003)
| method | path | body / notes |
|---|---|---|
| POST | `/durk/say` | `{"text": "...", "llm": false}` — speak now (or queue if device idle) |
| POST | `/durk/remind` | `{"time": "15:00" \| "2026-10-15T15:00" \| "in 10 minutes", "message": "..."}` |
| GET | `/durk/reminders` | pending reminders + settings |
| POST | `/durk/rundown` | speak the morning rundown now |
| GET | `/durk/pending` | polled by firmware every 5 s while idle |

## Never commit
`data/.config.yaml`, `google_client.json`, `google_token.json`, flash backups. See `.gitignore`.

## microSD card (sound effects, face themes, logs)
- Card on shared SPI3 (MOSI37/SCK36/MISO35/CS4), FATFS at /sdcard; boot continues if absent.
- `/sdcard/sfx/*.ogg` (Opus 16 kHz mono, 60 ms): wake, timer, boot, laugh, boop, chirp. Missing files auto-download from server `/durk/sfx/<name>` (`server/data/sfx`, `index.txt`).
- Face themes: classic (default), cyclops, angry-bot, or `/sdcard/faces/<name>.json` (server `/durk/faces/`). Tools: `robot.set_face_theme`, `robot.list_face_themes`.
- Tools: `robot.play_sound`, `robot.list_sounds`, `robot.get_logs`.
- Logs: `/sdcard/logs/durk-YYYYMMDD.log`, 5 MB rotation, 50 MB cap, reset reasons in `/sdcard/logs/resets.log`.

## License
Durk's own code: MIT (see [LICENSE](LICENSE)). Built on MIT-licensed upstream projects, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
