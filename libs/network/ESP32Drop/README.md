# ESP32Drop (receive-only)

AirDrop receiver for the Read Pico (ESP32-S3 N16R8): the AWDL link layer plus the
TLS/HTTP application layer. An iPhone or Mac in **Everyone** mode finds the device on
the AirDrop share sheet, connects, and uploads its `cpio` archive; the library hands
every file in that archive to a caller callback. No file system and no display access —
the caller decides where files go.

Ported from [ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop) by MIRO, upstream
licence **0BSD** (see [`LICENSE`](LICENSE)). Not affiliated with Apple; AirDrop is a
trademark of Apple Inc.

Upstream revision ported from: `92bdc1221924ca522f3ccbebbd9353612ce5b2d7`
("License detection, fixture line endings, and a host-test workflow"), the same revision
the reference port used. This tree is ESP-IDF **5.5.5** / mbedTLS **3**, which is what the
upstream Arduino library targets, so the mbedTLS code is upstream's own, unmodified.

## Interface

Use only [`include/esp32drop.h`](include/esp32drop.h). `src/` is upstream code and keeps
its original English comments.

```c
esp32drop_start(name, max_receive, cb, ctx);   // awdl_begin() then ad_begin()
esp32drop_poll();                              // deliver files + drain library logs
esp32drop_get_status(&st);                     // snapshot
esp32drop_stop();                              // ad_end() then awdl_end()
```

`esp32drop_poll()` must be called from the same task as `esp32drop_start()` /
`esp32drop_stop()`: it is the only place a file callback runs, and it is what frees the
received archive.

## What is included

Receive path only:

- `src/awdl/core/**` — frame parsing, the census, the election, the gauge, the window
- `src/awdl/port/**` — the ESP32 radio backend (promiscuous mode + `esp_wifi_80211_tx`)
- `src/airdrop/core/**` — bplist, cpio, DNS-SD, DvZip, HTTP, the service responder,
  status, the receiver certificate
- `src/airdrop/port/**` — the TLS listener and the `ad_*` entry points
- `src/ESP32AWDL.h` — the umbrella header the port includes

Excluded: the sender (`airdrop/ad_send.cpp/h`), the send-only helpers
(`ad_pack.h`, `ad_zlib.h`, `ad_uti.h`), `ble/**`, `ESP32Drop.h` (it re-exports the
sender) and the upstream examples and host tests.

## Local changes

Everything below is a deviation from upstream revision `92bdc12`; nothing else was
touched.

1. **`ad_end()` teardown** (`src/airdrop/port/ad_port_esp32.cpp` / `.h`), ported from
   the reference port (`read_pico_firmware` commit `25ad4ae`). Upstream had no way to
   stop the listener: `ad_begin()` could only be called once per boot. Now the listener
   loop tests a `g_ls_stop` flag, `ad_end()` `shutdown()`s the fd `serve_http()` is
   blocked on so a transfer in flight ends within one 100 ms select tick instead of a
   10 s read timeout, all sockets are closed, an undelivered archive is dropped and the
   receiver role is released — so `ad_begin()` can run again. A failed stop (task did not
   leave within 5 s) frees nothing, so the caller can retry.
2. **`ad_poll()` name buffers grown from `path[128], name[64]` to
   `path[384], name[256]`**, also from the reference port. Chinese and other long
   filenames arrive as multi-byte UTF-8 and were cut mid-name, losing the extension.
3. **PSRAM for the two allocations that would otherwise sit in internal RAM** (same
   reference port, same board). The 32 KB TLS listener stack is created with
   `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` and the 21 KB AWDL netif TX
   queue with `xQueueCreateWithCaps(..., MALLOC_CAP_SPIRAM)`; both fall back to internal
   RAM if PSRAM cannot supply the block, so neither is a new failure mode. On this board
   there is no 32 KB of *contiguous internal* RAM left once AWDL is up (the panel alone
   needs ~45 KB), and the failure is silent — `ad_begin()` returns -1 and the device is
   simply never discovered. `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` is already set
   in the base config.
4. **`src/esp32drop.cpp`** — the new C wrapper and start/stop order, adapted from the
   reference port's `src/esp32drop.cpp` (Apache-2.0, `SPDX-FileCopyrightText: 2026
   mindreset`). It also fixes the reference's leak of the callback pointers when
   `awdl_begin()` fails, and re-enables `wifi` log level on that path.
5. **`src/airdrop/port/ad_port_esp32.h`** documents `ad_end()`.

Not changed: **no mbedTLS adaptation was ported.** The reference port was retargeted to
ESP-IDF v6.1 / mbedTLS 4 and had to drop `mbedtls_entropy_*` / `mbedtls_ctr_drbg_*`,
`mbedtls_ssl_conf_rng()` and the five-argument `mbedtls_pk_parse_key()`. Our tree is
mbedTLS 3 and upstream's code is already correct for it, so those files are upstream
verbatim. For the same reason `mbedtls_ssl_conf_max_tls_version()` is *not* pinned to
TLS 1.2 here: `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3` is off in this build, so TLS 1.3 cannot
be negotiated anyway.

## Build wiring

`platformio.ini`, `[readpico_hardware]` **only** (so no other board's binary changes):

- `ESP32Drop=symlink://freeink-sdk/libs/network/ESP32Drop` in `lib_deps`
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` in `custom_sdkconfig`. **This one is not
  optional.** With mbedTLS allocating from internal RAM, the TLS handshake runs out on
  this board and the device goes *silently undiscoverable* — no error, with the radio,
  the AWDL election and the cadence all reporting healthy. That is the exact symptom the
  reference port chased down.

## Limits

- **Takes the radio.** AWDL parks the interface on 2.4 GHz channel 6 and keeps it
  promiscuous; it cannot share the interface with `WiFi.begin()`. Start it only when no
  ordinary WiFi session is running, and stop it before starting one.
- **Everyone mode only.** "Contacts Only" needs an Apple-signed identity.
- The decoded archive ceiling is the caller's (`max_receive`, 0 = 6 MiB); anything larger
  is refused to the sender with HTTP 413 rather than silently truncated.
- Upstream was measured on an M5Stack StopWatch (Arduino core 3.3.8 / IDF 5.x). This port
  compiles for readpico; it has **not** been validated against a real iPhone or Mac from
  this tree.
