# CLAUDE_CONTEXT — project handoff

Paste this to another Claude instance to continue work (e.g. publishing to GitHub,
adding features). It captures the hardware quirks, the hard-won fixes, and build steps
so nothing has to be rediscovered.

## What this project is

A **web-driven USB keystroke typer** on a **Pocket-Dongle-S3** (ESP32-S3 + 0.96" ST7735
LCD). The dongle acts as a USB HID keyboard plugged into a target PC; a web page served by
the dongle (or the standalone `web/typer_controller.html`) sends text/code that the dongle
types out. The LCD shows connection info and live line progress. Primary use: pasting long
text/code into fields that block paste.

## Hardware: Pocket-Dongle-S3 (AliExpress)

- MCU: ESP32-S3, native USB (USB-OTG). WiFi.
- Display: ST7735 controller, 0.96" IPS, **160×80 landscape**, **software SPI**.
- Backlight: **always on, no control pin.**
- Display pins: `MOSI=11  SCLK=10  CS=12  DC=13  RST=14`. No MISO.

## CRITICAL FIX #1 — USB HID won't enumerate without disabling the JTAG PHY

Symptom: with `USBMode=default` (TinyUSB) the host still saw **no keyboard**.
Cause: the fixed-function **USB-Serial-JTAG PHY** holds GPIO19/20, blocking USB-OTG.
Fix (runs first in `setup()`), same as the user's prior project:

```cpp
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#endif
...
CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
```

Also set `USB.productName("HID Keyboard");` before `USB.begin();` for a clean device name.
**After flashing, fully UNPLUG and REPLUG** — a reset alone won't re-enumerate USB.

## CRITICAL FIX #2 — ST7735 display driver that actually works on this panel

Use **Adafruit_GFX + Adafruit_ST7735** (software-SPI constructor), with:

- `initR(INITR_MINI160x80_PLUGIN)` (fallback `INITR_MINI160x80`)
- `setRotation(1)` → 160×80 landscape
- **Origin offset X=1, Y=26** via a subclass exposing `_xstart`/`_ystart`
- `clearRam()` that blanks the full controller RAM at 4 origin combos before first draw
  (otherwise stale pixels show)
- Constructor order: `LcdPanel lcd(CS, DC, MOSI, SCLK, RST);`

If a different unit is shifted: scan offsets (X 0..4, Y 24..29), pick the pair whose
full-screen `drawRect` border is clean on all 4 edges. Colors inverted → flip the `ips`
assumption / try the other init variant. Upside down → `setRotation(3)`.

## Networking: WiFiManager-style (home WiFi + fallback hotspot)

- On boot, load SSID/pass from NVS (`Preferences`, namespace "wifi"). If present, try STA
  for ~12s.
- If no creds or join fails → start AP `ESP32_Typer` / `12345678` at `192.168.4.1` with a
  `DNSServer` captive portal and a config page (`CONFIG_HTML`).
- `/save` writes creds to NVS and `ESP.restart()`s. `/forget` clears creds and reboots to
  setup. mDNS hostname `esptype` → `http://esptype.local`.
- `apMode` flag switches `/` between the config page and the typer page, and switches the
  LCD header (SETUP vs connected).

## Web UI

- `/` typer page (when connected): monospace editor with a line-number gutter, Tab inserts
  a real tab, `wrap=off` so line numbers stay accurate. POSTs to `/type` as
  `application/x-www-form-urlencoded` (preserves tabs/spaces/newlines/symbols).
- `/type` (POST): types char-by-char with a per-char delay (default 8 ms) to avoid dropped
  HID reports; updates the LCD line counter + progress bar; `'\n'`→Enter, `'\t'`→Tab.
- Top bar on the LCD is **green** (`ST77XX_GREEN`, black text) per the user's request.
- `web/typer_controller.html` is a standalone PC-side controller: has a device-address field,
  POSTs to `http://<addr>/type` with `mode:'no-cors'`, and mirrors progress on a simulated
  LCD. Must be opened as a local file (file://) — an https page can't reach the device over
  http (mixed content).

## Build

Toolchain: arduino-cli + `esp32:esp32` core + libs *Adafruit GFX*, *Adafruit ST7735 and
ST7789* (pulls Adafruit BusIO).

```
arduino-cli compile -b "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc" \
  --export-binaries firmware/esp32_keystroke_typer
```

FQBN options confirmed on this board: `USBMode=default` = USB-OTG (TinyUSB),
`CDCOnBoot=cdc` = USB CDC Serial on boot. Flash size default (4MB). Sketch ~81% of a
1.3MB app partition.

Outputs: `*.ino.merged.bin` (flash at 0x0, complete) and `*.ino.bin` (app only, flash at
0x10000).

## Known caveats / good next steps

- Typing assumes **US keyboard layout** on the target; add a layout/keymap option for others.
- Auto-indenting editors double indentation — could add a "paste mode" that sends Home or
  strips leading whitespace after newlines.
- Done: config page has a **network scan dropdown** (`/scan` returns JSON of SSID+RSSI;
  page dedupes, sorts by signal, "Other…" allows manual entry). AP setup uses `WIFI_AP_STA`
  so scanning works while the hotspot is up. LCD shows the AP **password** in setup mode.
- Captive portal: `DNSServer` wildcard + `onNotFound` 302 to `http://192.168.4.1/` makes
  most phones/PCs auto-pop the setup page on first connect. Not 100% across every OS; if it
  doesn't pop, the user opens `192.168.4.1`. (Normal/connected mode has no auto-pop.)
- Could add: OTA updates, a physical button to force setup mode, chunked typing for very
  large payloads, basic auth on `/type`.
- `test/hid_test_characters.txt` is the manual verification payload (all printable ASCII +
  tabs/spaces + a code sample).

## For publishing to GitHub

Repo is already structured (see README "Layout"). Suggested steps for the next instance:
1. `git init`, add a `.gitignore` (ignore `build/`, Arduino temp).
2. Add a `LICENSE` (README says MIT).
3. Consider moving `bin/` images to a GitHub **Release** rather than committing 4MB blobs.
4. Add a photo/GIF of the dongle screen to the README.
