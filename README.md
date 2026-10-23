# ESP32-S3 WiFi Keystroke Typer

Turn a **Pocket-Dongle-S3** (ESP32-S3 + 0.96" ST7735 LCD) into a USB keyboard you
drive from a web page. Paste long text or code into the page, hit **Send**, and the
dongle — plugged into a target PC — types it out as real USB keystrokes. The on-board
screen shows the connection info and live **line X / Y** progress.

Useful for pasting into places that block paste: remote/KVM consoles, locked-down
fields, BIOS/recovery shells, etc.
<img width="680" height="560" alt="web_typer" src="https://github.com/user-attachments/assets/59e14ad7-8a0c-4c62-8d60-bf6afa059843" />
<img width="1740" height="520" alt="lcd_states" src="https://github.com/user-attachments/assets/e30d15bf-fc38-4171-8964-9d1ffa5dd019" />
<img width="460" height="560" alt="web_setup" src="https://github.com/user-attachments/assets/e25e8ad1-824a-48f8-8550-36e93db5c947" />


## Features

- USB HID keyboard (TinyUSB) — types into whatever window is focused on the host PC.
- Web UI served by the dongle: code editor with line numbers, Tab support, speed control.
- **WiFiManager-style networking**: joins your saved home WiFi; if none/fails, it starts
  a `ESP32_Typer` setup hotspot with a captive config page. Credentials persist in NVS.
- 160×80 ST7735 status screen: SSID/IP + typing progress bar.
- Standalone `web/typer_controller.html` to drive the device from a PC browser too.

## Hardware

- **Pocket-Dongle-S3** (AliExpress): ESP32-S3 + 0.96" IPS ST7735, 160×80, software SPI.
- Display pins: `MOSI=11 SCLK=10 CS=12 DC=13 RST=14`. Backlight always on. Origin offset X=1 Y=26.

## Flash (no build needed)

Use the prebuilt image in `bin/`:

```
esptool.py --chip esp32s3 write_flash 0x0 bin/typer_FULL_flash_at_0x0.bin
```

Or a browser flasher (Chrome): flash `bin/typer_FULL_flash_at_0x0.bin` at offset `0x0`.
If it won't enter flash mode: hold **BOOT**, tap **RESET**, release BOOT.
**After flashing, fully unplug and replug** — a reset alone won't re-enumerate USB.

`bin/typer_app_flash_at_0x10000.bin` is the app-only image for quick re-flashes at
`0x10000` (needs bootloader already present).

## First run

1. Plug the dongle into the target PC (it enumerates as "HID Keyboard").
2. No WiFi saved yet → it broadcasts **`ESP32_Typer` / `12345678`**. Join it; the setup
   page opens (or go to `http://192.168.4.1`). Enter your home WiFi → Save. It reboots
   and joins your network.
3. Open `http://esptype.local` (IP also shown on screen). Focus the target window, paste,
   Send. Change/forget WiFi via the "WiFi settings" link.

## Build from source

Arduino IDE or arduino-cli. Board **ESP32S3 Dev Module**, **USB Mode = USB-OTG (TinyUSB)**,
**USB CDC On Boot = Enabled**. Libraries: *Adafruit GFX* + *Adafruit ST7735 and ST7789*.

```
arduino-cli compile -b "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc" \
  --export-binaries firmware/esp32_keystroke_typer
```

## Notes / caveats

- Typing assumes a **US keyboard layout** on the target PC; other layouts may mistype symbols.
- Pasting code into **auto-indenting editors** can double indentation — turn off auto-indent
  or type into a plain editor first. `test/hid_test_characters.txt` exercises all printable
  ASCII + tabs/spaces for verification.

## Layout

```
firmware/esp32_keystroke_typer/   Arduino sketch (.ino)
web/typer_controller.html         PC-side controller page
test/hid_test_characters.txt      character/indent self-test
bin/                              prebuilt flashable images
docs/CLAUDE_CONTEXT.md            project handoff / decision log
docs/typer_mock_preview.html      offline UI mock
```

## Credits

Made by **poordevman**.

## License

MIT. See [LICENSE](LICENSE).
