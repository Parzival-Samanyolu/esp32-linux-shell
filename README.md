# ESP32 Linux Shell — Deneyap WROVER-E

A Linux-style TCP shell firmware for the **Deneyap Kart (WROVER-E)** — connect over Wi-Fi
with `telnet`/`nc`/PuTTY and it behaves like a real terminal. Files live on an SD card, HTTP
requests are real, and every system stat comes from live ESP-IDF APIs. On top of the shell
it adds a **camera** (photo / video / live stream), a **browser file manager**, a full-screen
**nano** editor, and embedded interpreters for **Python, JavaScript, and C**.

> 🌐 **Project page:** enable GitHub Pages (Settings → Pages → `/docs`) — see [`docs/index.html`](docs/index.html).

## Features

- **Linux-style shell** — `ls`, `cat`, `cd`, `echo`, `mkdir`, `rm`, `df`, `wget`, `curl`,
  `ping`, `free`, `uptime`, `ifconfig`, `dmesg`, `uname`, `reboot`, `whoami`, `date` (NTP),
  `ps`, `neofetch`, plus **arrow-key history** and a live **`htop`**.
- **Camera (OV2640)** — `photo` (1600×1200 JPEG → SD), `video [secs]` (MJPEG `.avi` → SD),
  `stream start` (live MJPEG in a browser, port 81).
- **SD card** — FAT32 over SPI, real files/dirs/timestamps.
- **Browser file manager** — `files start` → `http://<ip>/` to list, preview, download, upload.
- **nano** — full-screen editor over telnet (arrows, `^O` save, `^X` exit).
- **Three languages** — `python` (pocketpy), `js` (Duktape), `cc` (PicoC). Run scripts from
  the SD card: `run script.py` / `run app.js` / `run prog.c`.
- **Diagnostics** — `selftest`, `stress`, `get`/`put` (base64 file transfer).

## Hardware

| | |
|---|---|
| Board | Deneyap Kart (WROVER-E) |
| SoC | ESP32-D0WD-V3, dual Xtensa LX6 @ 240 MHz |
| PSRAM | 8 MB @ 80 MHz (4 MB mapped) · Flash 4 MB |
| Camera | OV2640 (ribbon connector) |

**SD card (SPI)** — Deneyap silkscreen pins map to non-obvious GPIOs; these are the working ones:

| SD | Pad | GPIO |
|---|---|---|
| CS | D8 | 0 |
| MOSI | D12 | 13 |
| MISO | D15 | 27 |
| SCK | D14 | 14 |
| VCC | 5V | — |
| GND | GND | — |

## Build & flash (PlatformIO)

1. Install PlatformIO. Set your Wi-Fi in `include/config.h` (`WIFI_NETWORKS`).
2. Insert a FAT32 SD card and connect the camera ribbon.
3. Build & upload:
   ```bash
   pio run -t upload
   pio device monitor          # prints the IP (static 192.168.1.150 by default)
   ```
4. Connect: `telnet 192.168.1.150 2222`

> The first build downloads the ESP-IDF toolchain and managed components (esp32-camera,
> pocketpy) — a few hundred MB. Duktape and PicoC are vendored under `components/`.

## Project layout

```
platformio.ini            build config (board, PSRAM 80MHz, partitions)
sdkconfig.defaults        PSRAM / FreeRTOS / flash config
include/config.h          Wi-Fi networks, static IP, SD pins
src/                      main, wifi, sdcard, shell, commands, htop, dmesg,
                          camera, fileserver, editor, http_client,
                          lang_python.c / lang_js.c / lang_c.c
components/duktape/       vendored Duktape JS engine
components/picoc/         vendored PicoC C interpreter (+ ESP-IDF platform layer)
docs/index.html          project page (GitHub Pages)
```

## Notes

- `curl`/`wget` are plain HTTP (HTTPS needs a cert bundle).
- The languages are compact **embedded interpreters** (subsets) — the chip can't host a real
  compiler or JVM.
- The board only supports **2.4 GHz** Wi-Fi.
