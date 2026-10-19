# claude_status

An animated **Claude Code status display** for the Waveshare
ESP32-S3-Touch-AMOLED-1.8. A Claude Code hook on your computer pushes the
current state to the board over USB; the board plays a matching animated GIF
and chimes when a task finishes.

| State     | Shows                                   | Color |
|-----------|-----------------------------------------|-------|
| `working` | `working.gif`                           | amber |
| `waiting` | `thinking.gif` (waiting for your input) | cyan  |
| `error`   | `error.gif`                             | red   |
| `idle`    | rotates `bored` / `dancing` / `taking_a_stroll` (every 5 min) | green |

A short two-note chime plays on the **working → idle** transition (task done).

## Hardware

- **Waveshare ESP32-S3-Touch-AMOLED-1.8** (board revision **V2**: CO5300 panel,
  CST816S touch). The V1 panel (SH8601 / FT3168) also works — the BSP detects it.
  - ESP32-S3R8 · dual-core LX7 @240 MHz · 16 MB flash · 8 MB octal PSRAM
  - 1.8" AMOLED, 368 × 448
  - ES8311 audio codec + NS4150B speaker amp (used for the chime)
  - Native USB-Serial-JTAG on the Type-C port (no separate UART bridge)
- A **data** USB-C cable (not charge-only).
- Only the display and speaker are used; touch, IMU, RTC, SD and PMU are idle.

## Repo layout

```
claude_status/
  main/
    main.c            firmware: gif UI + serial reader + chime
    gif_assets.c/.h   embedded GIFs (generated from assets/)
    CMakeLists.txt    component: BSP + usb_serial_jtag + esp_codec_dev
    idf_component.yml  BSP dependency
  assets/             source GIFs (128x128) — replace with your own
  tools/gen_gif_assets.py   regenerates gif_assets.c/.h from assets/
  host/cc-status.sh   Claude Code hook -> serial
  sdkconfig.defaults  16MB flash, octal PSRAM, LVGL + GIF + CLIB malloc
  partitions.csv      LVGL-sized partition table
```

## Prerequisites

[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v6.0.2** (v5.5.x also
works). Quick install (macOS/Linux):

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --depth 1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
. ./export.sh          # run in each new shell (or alias it)
idf.py --version
```

## Build & flash

```sh
cd claude_status
idf.py set-target esp32s3            # once; downloads the BSP + LVGL
idf.py build
idf.py -p <PORT> flash               # PORT: /dev/cu.usbmodem* (mac), /dev/ttyACM* (Linux), COMx (Win)
```

> **Power-cycle the board after flashing.** Flashing resets the ESP32 over USB
> (a *warm* reset), and the CO5300 AMOLED can come up dark after a warm reset.
> Unplug and replug the USB-C cable once and the display runs normally. A
> wire-only reset (`idf.py monitor`, esptool) is **not** enough — only a real
> power cycle clears it.

The board boots to green **IDLE** and shows "waiting for host" until the first
`STATE:` line arrives.

## Connect it to Claude Code

`host/cc-status.sh <state>` pushes a state to the board. It auto-detects the
port (`/dev/cu.usbmodem*` / `/dev/ttyACM*`, or set `CC_STATUS_PORT`) and never
blocks or fails your session — problems go to `$CC_STATUS_LOG`
(default `/tmp/cc-status.log`).

Add to `~/.claude/settings.json`, adjusting the absolute path:

```json
{
  "hooks": {
    "UserPromptSubmit": [ { "hooks": [ { "type": "command", "command": "/ABS/PATH/claude_status/host/cc-status.sh working" } ] } ],
    "PreToolUse":       [ { "matcher": "*", "hooks": [ { "type": "command", "command": "/ABS/PATH/claude_status/host/cc-status.sh working" } ] } ],
    "Notification":     [ { "hooks": [ { "type": "command", "command": "/ABS/PATH/claude_status/host/cc-status.sh waiting" } ] } ],
    "Stop":             [ { "hooks": [ { "type": "command", "command": "/ABS/PATH/claude_status/host/cc-status.sh idle" } ] } ],
    "SessionStart":     [ { "hooks": [ { "type": "command", "command": "/ABS/PATH/claude_status/host/cc-status.sh idle" } ] } ]
  }
}
```

Restart Claude Code for hooks to load. `error` has no native hook — send it
manually (`host/cc-status.sh error`) or wire your own trigger.

**Note:** only one process can own the serial port. `idf.py monitor` and the
hook can't run at the same time — quit the monitor during normal use.

### Wire protocol

Line-based over USB-Serial-JTAG, newline-terminated. Unknown lines (and ESP-IDF
logs on the same port) are ignored:

```
STATE:idle
STATE:working
STATE:waiting
STATE:error
```

Smoke test without Claude Code:

```sh
PORT=$(ls /dev/cu.usbmodem* | head -1)
for s in working waiting error idle; do printf 'STATE:%s\n' "$s" > "$PORT"; sleep 2; done
```

## Sample GIFs & customizing

`assets/` holds six 128×128 sample GIFs (`working`, `thinking`, `bored`,
`dancing`, `taking_a_stroll`, `error`). Replace them with your own (keep them
small and ideally square), then regenerate the embedded arrays and rebuild:

```sh
python3 -I tools/gen_gif_assets.py main/gif_assets.c main/gif_assets.h \
  working=assets/working.gif thinking=assets/thinking.gif \
  bored=assets/bored.gif dancing=assets/dancing.gif \
  taking_a_stroll=assets/taking_a_stroll.gif error=assets/error.gif
idf.py build
```

Tweak in `main/main.c`:
- **GIF size** — `lv_image_set_scale(s_gif, 720)` (256 = 100%; ~360 px here).
- **Idle rotation** — `IDLE_ROTATE_MS` (default 5 min) and the `IDLE_GIFS` list.
- **Chime** — `notes[]` in `beep_init()`, volume in `speaker_init()` (0–100).
- **State → gif mapping** — `gif_for()`.

## Troubleshooting

- **Screen black after flashing** — power-cycle the board (see above).
- **GIFs don't appear** — the LVGL GIF decoder needs a ~65 KB per-frame buffer.
  This project sets `CONFIG_LV_USE_CLIB_MALLOC=y` so LVGL uses the PSRAM-backed
  heap; the default builtin 64 KB pool is too small and the decode fails
  silently (LVGL logging is off by default).
- **No chime** — check the speaker is connected; start volume is 60.
