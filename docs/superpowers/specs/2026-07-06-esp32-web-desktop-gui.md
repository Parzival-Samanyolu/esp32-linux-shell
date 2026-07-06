# ESP32-OS — a web-desktop GUI served from the board

**Date:** 2026-07-06
**Board:** Deneyap Kart (ESP32-WROVER-E), existing shell/camera/dashboard firmware.

## Goal

A full graphical "web desktop" served from the board at **`http://<ip>/gui`** (the
existing `/dash` dashboard and `/` file manager stay as-is). Works fully offline over
the hotspot. Draggable/resizable windows, a taskbar with start menu + clock + tray, and
ten apps — a mix of genuinely useful tools and fun/wow pieces.

## Architecture

- **One self-contained page**, authored as a real file `src/gui.html` and embedded into
  the firmware with ESP-IDF `EMBED_TXTFILES` (so it stays maintainable, not a giant C
  string). Served by the existing port-80 `httpd` in `fileserver.c` at `/gui`.
- **Vanilla HTML/CSS/JS only** — no frameworks, no CDNs, no external fonts/images. All
  inline so it runs with no internet (hotspot use).
- Talks to the board over the existing HTTP API plus a few new endpoints. Camera stream
  stays on its own server (port 81).

### New backend endpoints (`fileserver.c`)
- `GET /gui` — serve the embedded desktop page.
- `GET /api/ls?path=<dir>` — JSON directory listing: `[{"name","size","dir"}...]`.
- `POST /api/save` — write a text file (`path=`, `body=` form-encoded). For the editor.
- `GET /api/qr?text=<t>` — QR as a JSON bit-matrix `{"size":N,"rows":["0101...",...]}`
  (reuses vendored `qrcodegen`); the browser draws it on a `<canvas>`.
- Reused as-is: `GET /api/stats`, `POST /api/cmd`, `GET /dl?f=`, `POST /upload`,
  camera stream `http://<ip>:81/stream`.

### Desktop shell (window manager, in gui.html JS)
- Boot splash → desktop with wallpaper + app icons.
- `createWindow(title, node, opts)`: draggable (titlebar), resizable (corner grip),
  focus/z-order, minimize/maximize/close, taskbar button per window.
- Taskbar: Start button → app-launcher grid; running-window buttons; tray with live
  temperature, Wi-Fi/hotspot indicator, and a clock.
- `api` helper object wraps all fetch calls (cmd/stats/ls/save/qr/dl/upload).

## The ten apps

**Functional:**
1. **Terminal** — runs any shell command via `/api/cmd`; scrollback + command history.
2. **File Explorer** — icon grid from `/api/ls`; image thumbnails (`/dl?f=`), double-click
   to open (text → Editor), drag-drop upload (`/upload`), download, delete, rename, mkdir.
3. **Text Editor** — load (`/dl`) / edit / save (`/api/save`) text & code files.
4. **Code Runner** — type or pick Python/JS/C, Run (writes a temp file + `run`), show output.
5. **System Monitor** — live `<canvas>` line charts (heap free, PSRAM free, temperature)
   from `/api/stats`, plus a task list (`ps`).
6. **GPIO Control** — visual pin switches, LED brightness slider (`pwm 4 <v>`), effect
   buttons (blink/breathe/pulse/rainbow), RGB color picker (`rgb r g b`).
7. **Camera** — live stream `<img>`, Capture (`photo`) with thumbnail, Record (`video`).

**Fun / wow:**
8. **Games** — native in-browser Snake, 2048, Tic-Tac-Toe, Minesweeper (canvas/grid, no ANSI).
9. **Settings/About** — device info (stats + `uname`), Wi-Fi status, Reboot (confirm), and
   the **hotspot QR drawn on canvas** (from `/api/qr`) for phones to scan.
10. **Paint** — canvas doodle: color, brush, clear, save-as-PNG (download).

## Build plan (phases; each builds → flashes → verifies)

1. **Backend + shell** — new endpoints, embed gui.html, `/gui` route, window manager +
   taskbar + start menu with a placeholder app. Verify the desktop loads and windows work.
2. **Core apps** — Terminal, File Explorer, System Monitor.
3. **Hardware apps** — Camera, GPIO Control.
4. **Editor + Code Runner.**
5. **Games + Settings/QR + Paint.**

## Constraints / risks
- **Page size:** gui.html is tens of KB; embedded in flash (plenty free, ~46%) and sent
  chunked. Keep JS lean/vanilla.
- **Single-threaded httpd:** stats polling is light (~1 Hz); the camera stream is on the
  separate port-81 server, so it doesn't block the API.
- **Capture slots:** `/api/cmd` has 4 capture slots; the GUI fires few concurrent calls.
- **Offline:** absolutely no external resources — everything inline for hotspot use.
- **Security:** same posture as the dashboard — `/api/cmd` still refuses interactive
  commands and confirms destructive ones.

## Out of scope
- On-device physical display (no video hardware; pins taken by camera/SD).
- OTA (declined earlier — needs risky repartition).
