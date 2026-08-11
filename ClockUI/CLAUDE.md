# ClockUI — session notes for Claude

LVGL clock (digital + analog tabs, NTP-backed, RTC fallback) running on a Waveshare ESP32-S3-Touch-LCD-**2.8C**.

## Board: ESP32-S3-Touch-LCD-2.8C (NOT the plain 2.8)

| | |
|---|---|
| MCU | ESP32-S3R8, 16MB flash, 8MB OPI PSRAM |
| Display | ST7701 RGB, 480×480 round |
| Touch | GT911 (I²C, addr 0x5D) |
| RTC | PCF85063 (I²C, addr 0x51) |
| I/O expander | TCA9554PWR (I²C, addr 0x20) — gates peripheral resets/power |
| IMU | QMI8658 |
| USB bridge | **CH343** (vendor 0x1A86 / product 0x55D3) — UART0, not native USB |
| I²C pins | **SDA=15, SCL=7** (single bus shared with touch + RTC + expander + IMU) |
| Port | `/dev/cu.usbmodem5AB01629621` |

The plain 2.8 demo (240×320 ST7789 SPI + CST328) **does not work** on this board. Display driver, touch driver, and I²C pins all differ. If `Wire` transactions fail at every address, you have the wrong demo.

## Build

```
FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default
```

`CDCOnBoot=default` is required (not `cdc`). The CH343 bridges UART0 to USB; native CDC isn't wired. Setting `cdc` redirects `Serial` to native USB CDC → host sees the port but reads nothing.

Compile + flash:
```bash
cd ~/Documents/Arduino/ClockUI
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default"
arduino-cli upload  -p /dev/cu.usbmodem5AB01629621 --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default"
```

## Tailing serial

`cat /dev/cu.*`, `screen`, and `arduino-cli monitor` all toggle DTR/RTS on open. On the CH343 auto-reset circuit, that holds the ESP32-S3 in download mode → you see only boot-ROM garbage at the wrong baud rate. Symptom: bytes that look like `ea df da fb` repeating.

Use `/tmp/read_serial.py [seconds]`. It opens with `CLOCAL` set and explicitly deasserts DTR/RTS via `TIOCMSET` so the chip stays in run mode.

## Bring-up sequence (2.8C — order matters)

```
Flash_test → BAT_Init → I2C_Init → delay(120) → TCA9554PWR_Init(0x00)
  → Set_EXIO(EXIO_PIN8, Low) → PCF85063_Init → LCD_Init → Lvgl_Init
```

The TCA9554PWR_Init + EXIO_PIN8 step releases the LCD/touch reset path. Skipping it makes every I²C device return NAK.

## lv_conf.h tweaks

In `~/Documents/Arduino/libraries/lvgl/src/lv_conf.h`, set:
- `LV_FONT_MONTSERRAT_20 = 1`
- `LV_FONT_MONTSERRAT_48 = 1`

Both default to 0 and our digital tab uses them.

## Demo bundle URL

`https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-Demo.zip`

CDN path uses `2.8C` (no hyphen). Wiki URL uses `2.8-C` (with hyphen). Don't confuse them.

`www.waveshare.com` and `docs.waveshare.com` cloak against WebFetch (return blocked/divergent responses to scripted clients). The `files.waveshare.com` CDN serves binaries fine. For docs, mirror via the GitHub community ports or pull the demo zip and read its source directly.

## WiFi / NTP

Network credentials are in `ClockUI.ino` placeholders. NTP sync usually fires within ~4s of association on the local network (validated). NTP callback writes the synced time back to PCF85063, so a coin cell would let cold boots come up with the right time without WiFi. No battery currently installed → every cold boot has to wait for NTP.

TZ: `CST6CDT,M3.2.0,M11.1.0` (US Central, DST starts 2nd Sun of March, ends 1st Sun of November).

## Files

- `ClockUI.ino` — top-level, drives bring-up sequence, includes credentials
- `ClockApp.h/.cpp` — our module: WiFi task + NTP callback + LVGL tabview UI (digital + analog meter)
- Everything else copied verbatim from the 2.8C demo's `Arduino/examples/LVGL_Arduino/`

## Open status (as of last session)

Compile passes, flash succeeds. After adding staged `Serial.println` debug prints to setup(), capture was not yet re-run to verify the display actually renders. Resume by reflashing the current `.ino` and running `/tmp/read_serial.py 25` to see which init step (if any) hangs.
