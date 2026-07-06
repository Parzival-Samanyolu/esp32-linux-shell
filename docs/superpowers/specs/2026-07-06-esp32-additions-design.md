# ESP32 Shell — Four Additions (GPIO/LED, Fun, Web Dashboard, Expanded Diagnostics)

**Date:** 2026-07-06
**Board:** Deneyap Kart (ESP32-WROVER-E), existing "Linux shell" firmware.

## Goal

Add four self-contained capabilities to the existing shell firmware, each built,
flashed, and verified independently on the hardware at `192.168.1.150`:

1. **GPIO / onboard-LED control**
2. **Fun commands** (`cmatrix`, `cowsay`, `fortune`, `snake`)
3. **Web dashboard** (live camera + stats + browser command runner)
4. **Expanded `selftest` + `stress`**

## Hardware pin reality (constrains GPIO work)

Pins already claimed on this board:

- **Camera (OV2640):** 5, 18, 19, 21, 22, 23, 25, 26, 32, 33, 34, 35, 36, 39 (+ LEDC timer 0 / channel 0)
- **SD card (SPI):** 0 (CS), 13 (MOSI), 14 (CLK), 27 (MISO)
- **PSRAM (WROVER, internal):** 16, 17 — never expose
- **Boot-strapping (avoid driving):** 0, 2, 12, 15
- **UART0 console:** 1 (TX), 3 (RX)

**Onboard LED:** `LED_BUILTIN = GPIO4` (blue channel of the on-board RGB LED).
The RGB LED is red=GPIO3, green=GPIO1, blue=GPIO4. Red/green share the UART0
console pins, so driving them corrupts the USB serial monitor. **GPIO4 is the one
clean, unused, header-free pin** and is the default target for all LED/PWM work.

## 1. GPIO / LED control — `src/gpio_cmd.c` (+ `.h`)

Commands (dispatched from `cmd_execute`):

- `gpio info` — table of every GPIO and its status (free / camera / sd / psram /
  strapping / uart), plus the onboard-LED note.
- `gpio read <pin>` — configure as input (pull-up), print `0`/`1`.
- `gpio write <pin> <0|1>` — configure as output, drive level.
- `gpio blink [pin] [count] [ms]` — blink; defaults `pin=4 count=5 ms=200`.
- `pwm <pin> <0-255>` — LEDC brightness. Uses **LEDC timer 1 / channel 2**
  (camera uses timer 0 / channel 0), 5 kHz, 8-bit.
- `led on | off | blink` — convenience alias for the onboard LED (GPIO4).
- `rgb <r> <g> <b>` — drive all three RGB channels (0/1 each). Opt-in; prints a
  warning that red/green disturb the serial console.

**Pin guard:** a `gpio_pin_ok(pin, for_output)` helper classifies each pin. Camera,
SD, PSRAM, and input-only (34-39 for output) pins are rejected with a clear
message. Strapping pins (0,2,12,15) and UART pins (1,3) are allowed only with a
printed warning, never silently.

Files: new `src/gpio_cmd.c/.h`; `src/commands.c` (dispatch + help); `src/CMakeLists.txt`.

## 2. Fun commands — `src/fun.c` (+ `.h`)

- `cowsay <text>` — ASCII speech-bubble cow. Word-wraps the bubble.
- `fortune` — random line from a built-in quote table. (Deterministic-free RNG via
  `esp_random()`.)
- `cmatrix` — ANSI green digital rain until the user presses a key. Requires
  char-mode; in line-mode it prints a short animation then returns.
- `snake` — full ANSI game: WASD or arrow keys, growing snake, food, score,
  wall/self collision ends the game. Requires char-mode (telnet/PuTTY); if the
  client is line-mode, print "snake needs a telnet/PuTTY (char-mode) terminal".

Reuses the char-mode terminal + arrow-key decoding already built for `editor.c`.
Reads input with the existing per-byte socket read; renders with ANSI escape codes.

Files: new `src/fun.c/.h`; `src/commands.c` (dispatch + help); `src/CMakeLists.txt`.

## 3. Web dashboard — extend `src/fileserver.c`

A single self-contained page plus two JSON/text endpoints on the existing port-80
HTTP server:

- `GET /dash` — HTML page: embeds the MJPEG camera stream (`http://<ip>:81/stream`,
  auto-starting the stream if needed), a live stats panel, and a command box.
- `GET /api/stats` — JSON: heap free/total, PSRAM free/total, uptime, min-free-heap,
  camera sensor, SD mounted + free bytes, IP, client count. Polled ~1 Hz by the page.
- `POST /api/cmd` — body is one command line. Runs it through `cmd_execute` with
  output captured (see below), returns the plain-text output.

**Output capture (minimal, no rewrite of commands.c):** all command output flows
through `shell_send_all(int sock, ...)` (`shell_printf` calls it too, at
`src/shell.c:50`). Add a small capture registry in `shell.c`:

- `int shell_capture_begin(void)` — allocate a capture slot + PSRAM buffer, return a
  **negative sentinel fd** (e.g. `-(1000 + slot)`).
- In `shell_send_all`, if `sock < 0`, append to that slot's buffer instead of
  `send()`.
- `char *shell_capture_end(int fd, size_t *len)` — detach and return the buffer.
- `shell_read_line` / per-byte reads return `-1` (EOF) for `sock < 0`, so REPLs
  exit immediately instead of blocking.

The dashboard builds a `shell_ctx_t{ .sock = vfd, .cwd = "/sdcard", .char_mode = 0 }`,
calls `cmd_execute`, then returns the captured text.

**Command guard (per user choice — "confirm destructive only"):** the `/api/cmd`
handler denies interactive/fullscreen commands (`htop`, `nano`, `snake`, `cmatrix`,
REPLs) with a hint to use telnet. Destructive commands (`rm`, `reboot`, `format`,
`rmdir`, `mv` overwrite) require the POST to include `confirm=1`; without it the
endpoint returns a "needs confirmation" message and the page shows a confirm dialog.
Everything else runs freely.

Files: `src/fileserver.c` (routes + page + capture use), `src/shell.c/.h`
(capture API + `sock<0` handling), `src/commands.c` (expose a small
`cmd_is_interactive`/`cmd_is_destructive` helper or a denylist in fileserver).

## 4. Expanded `selftest` + `stress` — `src/commands.c`

**`selftest`** keeps the current heap/PSRAM/SD/camera/wifi checks and adds:

- **SD speed** — write then read a ~256 KB temp file, report MB/s.
- **PSRAM span** — walk the full free PSRAM in blocks with a pattern (not just 1 MB).
- **Flash/partition** — running partition label + app size + free app space.
- **Filesystem free** — SD free / total.
- **NTP time** — pass if the clock year >= 2024 (time was synced).
- **GPIO4 loopback** — drive GPIO4 high, read back, then low, read back.
- **Temperature** — internal temp sensor reading (informational).

**`stress`** keeps the 2-core CPU + PSRAM burn and adds:

- **SD I/O burn** — write + verify a multi-MB file, report throughput (skipped if no SD).
- **Full PSRAM sweep** — allocate/verify as much PSRAM as available, not a fixed 2 MB.
- **Per-core iteration counts** — report each core's Mops separately.
- **Temperature** — before/after internal temp.
- **Heap fragmentation** — largest free block vs. total free, before/after.
- Nicer incremental progress output during the run.

Files: `src/commands.c` only (plus `esp_temperature_sensor` / `temp_sensor` include).

## Build & verification order

Each step: build → `pio run -t upload` on `/dev/cu.usbserial-1130` → verify over
telnet/browser, confirm clean boot at `192.168.1.150`.

1. **Expanded selftest/stress** — no wiring, lowest risk, quick confidence check.
2. **GPIO / LED** — verify `led blink` visibly blinks the onboard blue LED;
   `gpio info` prints; guard rejects a camera pin.
3. **Fun** — `cowsay hi`, `fortune`, `cmatrix` (key stops it), `snake` playable over telnet.
4. **Web dashboard** — browse `http://192.168.1.150/dash`: stream shows, stats tick,
   `ls`/`free` run from the box, `rm x` asks to confirm, `htop` is refused.

## Out of scope / non-goals

- mDNS, MQTT, OTA, motion detection, auth/login (offered earlier, not selected now).
- No new managed components; all four features use only ESP-IDF built-ins + existing code.

## Risks

- **Dashboard capture** is the only cross-cutting change (touches `shell.c`). Kept
  minimal via the single `shell_send_all` interception point; negative-fd sentinel
  can't collide with real lwIP fds (always >= 0).
- **Flash size** — these are small (~10-20 KB code, mostly strings/HTML); no partition
  change expected. Verify `Flash:` % after the dashboard step.
- **Snake/cmatrix** need char-mode; guarded to degrade gracefully in line-mode.
