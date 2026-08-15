# Arduino sketchbook

An ESP32-S3 desk clock that tells the time, tells you what your iPhone is playing, and lets you skip the track without touching your phone. Plus the small diagnostic sketches that were needed to get there.

Everything here targets the **Waveshare ESP32-S3-Touch-LCD-2.8C**, the round 480x480 one. Not the plain 2.8. That distinction costs an evening if you get it wrong, so it is repeated below in bold.

## What's in the box

| Folder | What it is |
|---|---|
| `ClockUI/` | The main event. LVGL clock with digital, analog, and now-playing tabs |
| `ClockSmokeTest/` | Sixty lines that answer "is it the WiFi or is it my code?" |
| `I2CScan/` | Sweeps a few candidate pin pairs looking for signs of life on the bus |
| `WaveshareDemo/` | The vendor demo bundle, kept for reference |
| `libraries/` | Vendored LVGL 8.3.10 and ESP32-audioI2S 2.0.0 |

## The board

| | |
|---|---|
| MCU | ESP32-S3R8, 16MB flash, 8MB OPI PSRAM |
| Display | ST7701 RGB, 480x480 round |
| Touch | GT911 (I2C, 0x5D) |
| RTC | PCF85063 (I2C, 0x51) |
| I/O expander | TCA9554PWR (I2C, 0x20), gates the peripheral reset lines |
| IMU | QMI8658 |
| I2C pins | SDA=15, SCL=7, one bus shared by touch, RTC, expander, and IMU |

Toolchain: `esp32:esp32` core **3.3.8**, arduino-cli, LVGL 8.3.10.

**The plain 2.8 demo does not work on this board.** It expects a 240x320 ST7789 over SPI and a CST328 touch controller on different pins. If every `Wire` transaction NAKs at every address, you are running the wrong demo, not debugging a dead board.

## Working on a different machine? Read this first

Three things are per-machine and none of them can be copied from someone else's notes.

### 1. Your USB port name

Never hardcode it. Ask:

```bash
arduino-cli board list
```

### 2. Which USB path you are on, because it changes a build flag

This board can present itself two different ways, and they need opposite settings. Find out which you have:

```bash
ioreg -p IOUSB -w0 -l | grep -iE '"USB Product Name"|"idVendor"|"idProduct"'
```

| What you see | Meaning | Use |
|---|---|---|
| Espressif `USB JTAG_serial debug unit` (0x303A / 0x1001) | Native USB, port looks like `/dev/cu.usbmodem101` | `CDCOnBoot=cdc` |
| `CH343` (0x1A86 / 0x55D3) | UART0 over a bridge chip, longer port name | `CDCOnBoot=default` |

Get this backwards and `Serial` goes somewhere the host cannot hear. The port enumerates, the silence is total, and you conclude the board is bricked. It is not. On native USB with `default`, you still see ESP-IDF's own log output, which makes it especially convincing, but none of the sketch's own `[1]`..`[11]` stage prints appear.

The board is the same either way. Only `CDCOnBoot` and the port name change.

### 3. Your `secrets.h`

It is gitignored, so a fresh clone does not have one and will not build until you make it:

```bash
cp ClockUI/secrets.example.h ClockUI/secrets.h
cp ClockSmokeTest/secrets.example.h ClockSmokeTest/secrets.h
```

Fill in `WIFI_SSID` and `WIFI_PASS`. A missing file stops the build with a one line `#error` telling you exactly this, rather than fifty undefined symbols.

**This repo is public.** Credentials live in `secrets.h` and nowhere else. Before committing, `git diff --cached` and look for your SSID. (For the record, commit `fb297b0` predates this arrangement and still carries an old network's password, so treat that one as burned.)

## Getting it running

### LVGL config

In `libraries/lvgl/src/lv_conf.h`, both of these need to be on (they ship as 0):

```c
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_48 1
```

### Build and flash

```bash
# CDCOnBoot: cdc for native USB, default for CH343. See above.
FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc"
PORT=$(arduino-cli board list | awk '/esp32/ {print $1; exit}')

cd ClockUI
arduino-cli compile --fqbn "$FQBN"
arduino-cli upload -p "$PORT" --fqbn "$FQBN"
```

A healthy build lands around 53% of program storage and 31% of dynamic memory. If dynamic memory climbs much past that, read the radio section below before you do anything else.

### Do not leave duplicate files in a sketch folder

arduino-cli compiles **every** source file in the sketch root, not just the ones you include. A folder full of Finder's `Whatever 2.cpp` copies will be built alongside the real thing, and since those copies came from the plain 2.8 demo you get a wall of redefinition and link errors that look like the toolchain has lost its mind.

They now live in `ClockUI/_unused_2.8_demo/`, which is outside the build path because only the sketch root and `src/` are compiled. If you ever see `multiple definition of LCD_Init`, something wrong-board has crept back into the root.

## Reading the serial output

`cat /dev/cu.*`, `screen`, and `arduino-cli monitor` all toggle DTR and RTS when they open the port, which on the auto-reset circuit pins the S3 in download mode. You get boot-ROM chatter at the wrong baud rate, and the tell is a repeating `ea df da fb`.

Open the port with `CLOCAL` set and deassert DTR/RTS via `TIOCMSET` first. This script does that and is worth keeping around:

```python
#!/usr/bin/env python3
"""Read ESP32 serial without toggling DTR/RTS."""
import fcntl, os, struct, sys, termios, time

PORT = os.environ.get("PORT", "/dev/cu.usbmodem101")
BAUD = int(os.environ.get("BAUD", "115200"))
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0

TIOCMGET, TIOCMSET = 0x4004746A, 0x8004746D
TIOCM_DTR, TIOCM_RTS = 0x002, 0x004

fd = os.open(PORT, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
try:
    a = termios.tcgetattr(fd)
    a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.CRTSCTS
    a[0] = a[1] = a[3] = 0
    a[4] = a[5] = BAUD
    termios.tcsetattr(fd, termios.TCSANOW, a)
    bits = struct.unpack("I", fcntl.ioctl(fd, TIOCMGET, struct.pack("I", 0)))[0]
    fcntl.ioctl(fd, TIOCMSET, struct.pack("I", bits & ~(TIOCM_DTR | TIOCM_RTS)))
    deadline = time.time() + SECONDS
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.05); continue
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace")); sys.stdout.flush()
finally:
    os.close(fd)
```

Boot prints numbered stages `[1]` through `[11]`, so whichever number you last saw is the step that hung. `setup()` opens with a 1.5s delay, which is just enough time to reset the board and attach before the interesting part starts.

### When it panics

Decode the backtrace rather than reading tea leaves:

```bash
ELF=$(ls -t ~/Library/Caches/arduino/sketches/*/ClockUI.ino.elf | head -1)
A2L=$(ls ~/Library/Arduino15/packages/esp32/tools/esp-x32/*/bin/xtensa-esp32s3-elf-addr2line | head -1)
"$A2L" -pfiaC -e "$ELF" 0xADDR 0xADDR ...
```

`InstrFetchProhibited` with a PC like `0x0000001f` is a call through a garbage function pointer, which around here usually means something got freed or the heap is shot. `assert failed: xQueueGenericSend queue.c:936 (pxQueue)` is a FreeRTOS primitive that came back NULL, which means you ran out of internal heap.

## ClockUI

Three full-screen tabs, swipe between them (the tab bar is hidden, the gesture is the interface):

- **Digital.** Big Montserrat 48 clock, date underneath, and a grey status line that tells you when it is still hunting for NTP.
- **Analog.** An `lv_meter` wearing a watch face. Hour, minute, and a red second hand.
- **Music.** Title, artist, album scrolling if they're long, plus prev / play-pause / next and volume, all over Apple Media Service.

Time comes from SNTP on boot and gets written straight back to the PCF85063, so a coin cell would let a cold boot come up correct without a network. No cell is fitted at the moment, which means every cold boot waits on NTP.

Timezone is `CST6CDT,M3.2.0,M11.1.0` (US Central), set in `ClockUI.ino`.

### The radio does not do sharing

This is the single most expensive thing to rediscover, so it gets the detail it deserves.

WiFi and Bluedroid will not both fit in what is left of internal RAM after LVGL takes its cut, and they want the same 2.4GHz radio besides. Every arrangement except taking turns fails, and each one fails in a way that points somewhere else entirely:

| Arrangement | What you get |
|---|---|
| Both at once | BLE initialising during association starves the auth exchange. The AP deauths with `reason=2` (AUTH_EXPIRE) forever, SNTP never starts, and the screen sits on "syncing NTP..." |
| WiFi first, BLE after | BLE gets ~73KB, `BLECharacteristic`'s semaphore comes back NULL, `assert(pxQueue)` fires, reboot loop |
| BLE first, WiFi after | `esp_wifi_start` never comes up and the station sits at `WL_STOPPED` (status 254) |

So they take turns. `wifi_task` associates, chases a sync, then powers the radio all the way down (heap goes from about 73KB back up to 99KB) and raises `radio_free`. `loop()` watches for that flag and only then starts BLE, which is comfortable at ~103KB. Time survives the handoff in the PCF85063. If the network never shows up, the task gives up after 90 seconds and hands the radio over anyway, so the music tab still works at a friend's house.

**Consequence:** album art cannot fetch, because it needs HTTPS and WiFi is gone by then. The way out is NimBLE, which is roughly 40KB lighter than Bluedroid and would let both stacks coexist. That means a new dependency and rewriting `BLE_Clock.cpp` and `BLE_AMS.cpp`, so it has not been done.

### Debugging network problems: use the control

`ClockSmokeTest` is the instrument for this. It is the same board, the same antenna, the same spot, and the same `secrets.h`, but with no LCD, no LVGL, and no BLE. Flash it and you learn in thirty seconds whether the radio and the network are fine.

This matters because a weak-looking signal is a spectacularly convincing red herring. ClockUI reported the AP at -74 to -78 dBm and found only two networks in a scan while a laptop beside it saw six, which reads exactly like a bad antenna. The smoke test then associated at -72 dBm in 2.5 seconds and synced on the first tick. The signal was never the problem. Contention was.

Also worth knowing: at the 802.11 layer, `reason=2` and `reason=201` happen *before* the PSK is ever tested, so they never mean a wrong password. A rejected password is `reason=15` or `reason=202`. If you are not seeing those, stop retyping the password.

### Files

- `ClockUI.ino`, bring-up sequence, timezone, and the BLE handoff in `loop()`
- `ClockApp.h/.cpp`, WiFi task, NTP callback, and the LVGL tabview
- `BLE_Clock.h/.cpp`, GATT server: Current Time (0x1805) and Battery (0x180F)
- `BLE_AMS.h/.cpp`, Apple Media Service client, the bit that reads your now-playing
- Everything else is copied verbatim from the 2.8C demo's `Arduino/examples/LVGL_Arduino/`
- `_unused_2.8_demo/`, the wrong-board drivers, kept as a cautionary tale

`AlbumArt.h/.cpp` are still tracked and still compiled, though nothing calls them. See [ALBUM_ART_ATTEMPT.md](ClockUI/ALBUM_ART_ATTEMPT.md) for the full story: the short version is that a TLS handshake wants roughly 30KB and there were 22 to 25KB going spare, and mbedTLS's 16KB record buffers are baked into the precompiled SDK.

One trap if you touch that file: it used to call `WiFiClientSecure::setBufferSizes()` to shrink those buffers, and core 3.3.8 renamed the class to `NetworkClientSecure` and dropped the method with no replacement. The calls had to go, which means the TLS path now has nothing standing between it and heap exhaustion.

## Bring-up order matters

```
Flash_test -> BAT_Init -> I2C_Init -> delay(120) -> TCA9554PWR_Init(0x00)
  -> Set_EXIO(EXIO_PIN8, Low) -> PCF85063_Init -> LCD_Init -> Lvgl_Init
```

The TCA9554 step releases the LCD and touch reset lines. Skip it and every I2C device on the bus plays dead, which looks exactly like a hardware fault and is not one.

## Where things stand

Working: boots clean, GT911 touch enumerates, all three tabs render, WiFi associates, NTP syncs and writes the RTC, BLE advertises and AMS connects. Verified over a 150 second run with no panic, no assert, and no reboot.

Not working: album art, for the heap reasons above.

Next, if you want it: move BLE to NimBLE so WiFi can stay up, which brings album art back and lets the clock re-sync during the day instead of only at boot. Fit a coin cell to the PCF85063 and cold boots stop waiting on the network.

## Vendor demo

`WaveshareDemo/` holds the official bundle. Grab it from:

```
https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-Demo.zip
```

Note that the CDN path spells it `2.8C` while the wiki URL spells it `2.8-C`. Also, `www.waveshare.com` and `docs.waveshare.com` serve scripted clients something other than the real page, so pull the zip and read its source rather than trusting a fetched doc.
