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
| Port (CH343 cable) | `/dev/cu.usbmodem5AB01629621` |
| Port (native USB cable) | `/dev/cu.usbmodem101` — enumerates as Espressif "USB JTAG_serial debug unit" (0x303A/0x1001) |

The board can enumerate two different ways depending on which cable/connector is
used. Identify which you have with `ioreg -p IOUSB` before trusting any serial
assumptions below.

The plain 2.8 demo (240×320 ST7789 SPI + CST328) **does not work** on this board. Display driver, touch driver, and I²C pins all differ. If `Wire` transactions fail at every address, you have the wrong demo.

## Build

```
FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default
```

`CDCOnBoot` must match the cable in use:
- **CH343 cable** (`usbmodem5AB...`): use `CDCOnBoot=default`. `Serial` goes to UART0 → CH343. Setting `cdc` means the host reads nothing.
- **Native USB cable** (`usbmodem101`): use `CDCOnBoot=cdc` to see `Serial` output. With `default`, only `printf()` (IDF console secondary output) reaches this port — the Waveshare drivers print, but all our `[NET]/[BLE]/[AMS]` `Serial.printf` logs are invisible.

**The device is currently flashed with `CDCOnBoot=cdc`** (native-USB debugging session, 2026-08-15). Rebuild with `default` before going back to the CH343 cable.

Compile + flash:
```bash
cd ~/Documents/Arduino/ClockUI
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default"
arduino-cli upload  -p /dev/cu.usbmodem5AB01629621 --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default"
```

## Tailing serial

`cat /dev/cu.*`, `screen`, and `arduino-cli monitor` all toggle DTR/RTS on open. On the CH343 auto-reset circuit, that holds the ESP32-S3 in download mode → you see only boot-ROM garbage at the wrong baud rate. Symptom: bytes that look like `ea df da fb` repeating.

Use `/tmp/read_serial.py [seconds]`. It opens with `CLOCAL` set and explicitly deasserts DTR/RTS via `TIOCMSET` so the chip stays in run mode.

On the **native USB port**, opening the serial port sometimes hard-resets the
board (and sometimes doesn't — it depends on prior DTR/RTS state). Don't treat
a silent capture as a dead board: a healthy steady-state app prints nothing
unless events fire. The reliable liveness check is a BLE scan from the Mac
(`bleak` in a venv): if `ClockUI` is advertising, the app is up.

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
- `ClockApp.h/.cpp` — our module: WiFi task + NTP callback + LVGL tabview UI (digital + analog meter + Signal music tab)
- Everything else copied verbatim from the 2.8C demo's `Arduino/examples/LVGL_Arduino/`

## Music tab: "Signal" oscilloscope

The music tab renders a synthesized oscilloscope trace regulated by real AMS
data (the board never sees audio: iPhone plays it, no mic, PCM5101 unused).
Play/pause ramps amplitude (pause = flatline), PlaybackInfo rate drives scroll
speed, iPhone volume scales amplitude, title+artist hash seeds the waveform
shape per track, track changes kick a decaying surge. `wave_cb` runs a 33 ms
lv_timer updating a 121-point `lv_line` in a 480x150 band; only that band
invalidates. BLE_AMS additionally subscribes to Track.Duration and
Player.Volume, and parses rate + elapsed out of PlaybackInfo (elapsed is
extrapolated between notifications in `BLE_AMS_GetElapsedSec`).

## Boot crash: never re-add the touch attachInterrupt

`Touch_Init` used to call `attachInterrupt(GT911_INT_PIN, ...)`. With an
**empty NVS** this deterministically crash-loops the board at boot:
`Stack canary watchpoint triggered (ipc1)` — the 1KB ESP-IDF ipc1 task
overflows inside `gpio_isr_register → esp_intr_alloc → heap_caps_malloc`.
The interrupt was pure debug plumbing anyway (LVGL polls `Touch_Read_Data()`
every tick; the ISR flag only gated a printf in `Touch_Loop`). Removed
2026-08-15. Touch works fine without it.

Related hard-won lessons:
- **Do not erase NVS casually** (`esptool erase_region 0x9000 0x5000`). It
  exposed the crash above and cost a recovery session.
- If the board ends up dark/unresponsive after partial flashing: do a **full
  `arduino-cli upload`** (writes bootloader + partitions + boot_app0 + app).
  An app-only `esptool write_flash 0x10000` from manual download mode once
  left the board black even though the hash verified.
- Manual download mode: hold BOOT, tap RESET, release BOOT. esptool then
  connects even when a crash loop breaks the auto-reset handshake.

## BLE pairing status (open as of 2026-08-15)

The clock advertises correctly — verified over the air from the Mac with a
`bleak` scan (name `ClockUI`, CTS + BAS in scan response, strong RSSI) and
via CoreBluetooth's own decode. The advertisement now carries **Service
Solicitation (AD 0x15) for ANCS** in the main packet and **AMS** in the scan
response (`BLE_Clock.cpp`), because iOS Settings filters generic BLE
peripherals out of "Other Devices".

**Despite all that, modern iOS still does not list ClockUI in Settings >
Bluetooth.** Neither plain CTS advertising, AMS solicitation, nor ANCS
solicitation earned a row. iPhone-side bond is gone (user forgot it at some
point) and clock-side bonds were wiped with the NVS.

**Next step, untested:** pair via a BLE utility app on the iPhone (LightBlue
or nRF Connect): connect to ClockUI from the app; the firmware's
`setForceAuthentication(true)` sends a Security Request on connect, which
pops the iOS system pairing dialog and creates a normal system-wide bond.
After bonding, AMS discovery runs from `onAuthenticationComplete` and the
music tab lights up. How the original pairing was ever created is unknown —
possibly an older iOS listed the device, or this app path was used.

## Signal music tab: session log 2026-08-15

Built and flashed the "Signal" oscilloscope music tab (see section above),
plus AMS additions: `Track.Duration` + `Player.Volume` subscriptions and
full `PlaybackInfo` parsing (state, rate, elapsed with extrapolation).
Display + touch verified working on hardware; the trace idles at NO SIGNAL
until an iPhone bonds (blocked on pairing, above).
