# Album artwork on ClockUI — attempt notes

We tried adding album artwork as the Music tab background by fetching from
iTunes Search API over WiFi. It didn't work on this build. This doc captures
what we tried, why it failed, and what would still be possible.

## What we tried

Background WiFi task that, on every AMS Track change:

1. URL-encodes `artist + " " + title` and GETs
   `https://itunes.apple.com/search?term=...&entity=song&limit=1`
2. Substring-parses `"artworkUrl100":"..."` from the JSON
3. Rewrites `100x100bb` → `300x300bb` for a usable thumbnail
4. GETs that JPEG into a PSRAM buffer
5. Hands the raw JPEG bytes to LVGL with `LV_USE_SJPG=1` and points an
   `lv_img` background at it

Code is in git history (now removed): `AlbumArt.h`, `AlbumArt.cpp`,
`LV_USE_SJPG=1` in `lv_conf.h`, plus calls from `ClockApp.cpp`.

## Why it failed

Steady-state free heap during a fetch was **22–25 KB**. mbedTLS's TLS
handshake on ESP-IDF / Arduino-ESP32 3.3.8 needs ~30 KB minimum (default
record buffers are 16 KB recv + 16 KB tx, plus session state).

Cause: BLE NimBLE host + active GATT client subscription to AMS + WiFi STA
together consume ~100 KB of the ~125 KB we have after boot. There is no
slack for a TLS handshake on top.

## What we ruled out

- **HTTP fallback to iTunes.** `http://itunes.apple.com/search` returns 302
  to HTTPS — Apple closed plain HTTP on this endpoint. The redirect chain
  cannot be followed without TLS.
- **HTTP fallback to mzstatic CDN.** Rewriting `is1-ssl.mzstatic.com` →
  `is1.mzstatic.com` is a known trick on older firmwares; the non-SSL host
  no longer accepts connections. (We never got that far — the search itself
  fails first.)
- **`WiFiClientSecure::setBufferSizes(...)`.** Doesn't exist in
  `NetworkClientSecure` on ESP32 core 3.3.8. The 16 KB record buffer size is
  baked into the precompiled mbedTLS in the SDK; changing it requires
  rebuilding the platform package or moving to ESP-IDF.
- **Bumping the fetch-task stack to 16 KB.** Stack was never the issue; the
  failure was on heap during the TLS handshake.
- **Larger PSRAM allocator for TLS.** mbedTLS buffers must be in internal
  DRAM (DMA-reachable for the WiFi driver). PSRAM doesn't help here.

## What would still work

1. **Local proxy on the same WiFi.** A tiny endpoint (Laravel route on
   Herd, Node script on the Mac, Cloudflare Worker, etc.) that takes
   `?artist=X&title=Y`, fetches iTunes + the image server-side over HTTPS,
   and returns the JPEG over plain HTTP. ESP32 needs no TLS. Reliability
   depends on the proxy being reachable (home WiFi only, unless hosted).

2. **Custom mbedTLS build with smaller record buffers.** Patch the SDK to
   set `MBEDTLS_SSL_IN_CONTENT_LEN` / `MBEDTLS_SSL_OUT_CONTENT_LEN` to
   ~2048 each, rebuild the Arduino-ESP32 platform package. Several days
   of work and a non-standard toolchain to maintain.

3. **Move to ESP-IDF directly.** Same `sdkconfig` knobs as above, but with
   the right build system around them. Bigger refactor: the project would
   stop being an Arduino sketch.

4. **Tear down BLE during fetch.** `BLEDevice::deinit()` reclaims ~80 KB
   but kills the AMS subscription. Re-init takes ~5 s and forces the
   iPhone to renegotiate. Bad UX for "track changed → flash of art".

5. **Bluetooth Classic A2DP/AVRCP sink.** AVRCP supports cover art via
   OBEX. Means dual-stacking BR/EDR + BLE and presenting as a speaker;
   the iPhone wants to send audio to us. Significantly more invasive than
   AMS-over-BLE.

## Decision

Shipped without artwork. Music tab keeps title, artist, album, and the
prev/play-pause/next controls — all working over AMS. Reverted:
`AlbumArt.*` removed, `LV_USE_SJPG = 0`, no WiFi/HTTPS code in the BLE
data path. Sketch is back to **48 %** flash, no extra runtime cost.

If artwork comes back later, option 1 (local proxy) is the lowest-effort
path that doesn't require a custom firmware build.
