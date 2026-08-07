# thumb-nas

A pocket-sized network drive built on the Waveshare ESP32-S3-Touch-AMOLED-2.06.
An SD card in the board's TF slot, mountable as a real network drive over WiFi.

![Platform](https://img.shields.io/badge/platform-ESP32--S3-informational)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.5.2-red)
![Bus](https://img.shields.io/badge/SD%20bus-SPI%20(1--bit)-yellow)
![WebDAV](https://img.shields.io/badge/WebDAV-class%202-blue)
![Status](https://img.shields.io/badge/Windows-read%2Fwrite-brightgreen)

A minimal WebDAV server built directly on ESP-IDF's `esp_http_server` — no
Arduino core, no third-party WebDAV library. Mounts as a real network drive on
Windows with full read/write, verified by a byte-identical 910 KB round trip
through Explorer.

---

## Table of contents

- [What this does](#what-this-does)
- [Hardware](#hardware)
- [Requirements](#requirements)
- [Setup](#setup)
- [Build and flash](#build-and-flash)
- [Mounting as a network drive](#mounting-as-a-network-drive)
- [Verified results](#verified-results)
- [How it works](#how-it-works)
- [Roadmap](#roadmap)
- [Troubleshooting](#troubleshooting)
- [Project structure](#project-structure)

---

## What this does

On boot the firmware:

1. Mounts the TF card over SPI at `/sdcard`
2. Writes a small `test.md` to it
3. Joins WiFi as a station
4. Prints its IP address and starts a WebDAV server on port 80

Supported methods: `OPTIONS`, `PROPFIND`, `PROPPATCH`, `GET`, `HEAD`, `PUT`,
`DELETE`, `MKCOL`, `MOVE`, `LOCK`, `UNLOCK`. Announced as DAV class 2.

`COPY` and `Depth: infinity` are not implemented.

## Hardware

**Waveshare ESP32-S3-Touch-AMOLED-2.06** — ESP32-S3 (rev v0.2), 8 MB PSRAM,
32 MB flash, USB-Serial/JTAG.

### TF card pinout

Waveshare's documentation for this board is inconsistent: one section is titled
"using SDMMC", but the pin table beneath it lists only four signals. Four signals
is SPI wiring, not the six lines (CLK, CMD, D0–D3) that 4-bit SDMMC requires.

This project uses the SPI driver (`esp_vfs_fat_sdspi_mount`) against those four
documented pins:

| Signal | GPIO |
| :----- | :--- |
| CS     | 17   |
| MOSI   | 1    |
| MISO   | 3    |
| SCK    | 2    |

The boot log settles the question empirically — `sdmmc_card_print_info()` reports
`SSR: bus_width=1`, confirming single-bit SPI.

## Requirements

- **ESP-IDF v5.5.2** (earlier 5.x will likely work but is untested here)
- A TF card formatted **FAT32** — not exFAT

> [!IMPORTANT]
> Windows formats cards above 32 GB as exFAT by default. The mount config sets
> `format_if_mount_failed = false`, so an exFAT card fails the mount rather than
> being silently reformatted. That is deliberate — a diagnostic should not
> destroy your data — but the card must be FAT32 before you start.

## Setup

WiFi credentials are kept out of version control. Copy the template and fill in
your own network:

```bash
copy main\wifi_credentials.h.example main\wifi_credentials.h
```

Then edit `main/wifi_credentials.h`:

```c
#define WIFI_SSID     "your-network-name"
#define WIFI_PASSWORD "your-password"
```

If the file is missing, the build fails with an explicit `#error` rather than a
confusing undefined-symbol error.

> [!WARNING]
> `build/` is gitignored **for security, not just tidiness**. The linked firmware
> image contains your WiFi password as a plain string — `strings` on the `.bin`
> will print it. Never commit `build/`, and never hand someone a prebuilt `.bin`
> of this firmware.

The board joins as a 2.4 GHz station. If your router broadcasts a separate
5 GHz-only SSID, use the 2.4 GHz one.

## Build and flash

```bash
idf.py set-target esp32s3
```

```bash
idf.py build
```

```bash
idf.py -p COM13 flash monitor
```

Replace `COM13` with your board's port — on Linux usually `/dev/ttyACM0` or
`/dev/ttyUSB0`. On Windows, look for a port whose hardware ID contains `VID_303A`
(Espressif's vendor ID). Press `Ctrl+]` to exit the monitor.

Build settings live in `sdkconfig.defaults`, which **is** tracked. The generated
`sdkconfig` is not. Changes to the defaults only apply to a fresh config, so
delete `sdkconfig` and rebuild after editing them.

## Mounting as a network drive

Take the IP address from the boot log.

### Linux

```bash
gio mount dav://<board-ip>/
```

Or GNOME Files → *Other Locations* → *Connect to Server* → `dav://<board-ip>/`.
For a system-wide mount with davfs2:

```bash
sudo mount -t davfs http://<board-ip>/ /mnt/thumbnas
```

### Windows

```bash
net use Z: http://<board-ip>/
```

Or *This PC* → *Map Network Drive* → `http://<board-ip>/`.

Windows maps this internally to `\\<board-ip>\DavWWWRoot`. Browsing, reading,
writing, creating folders, renaming and deleting all work.

### What Windows actually required

Worth recording, because the failure modes were misleading and the error
messages actively point the wrong way.

Windows needs **three** verbs beyond the obvious set, and omitting any of them
fails in a way that looks like something else entirely:

| Verb | Symptom when missing |
| :--- | :------------------- |
| `LOCK` / `UNLOCK` | Writes fail with *"A device attached to the system is not functioning"* |
| `PROPPATCH` | Copies fail with *"The specified network name is no longer available"* — **after the upload has already fully succeeded** |
| `HEAD` | Issued during copies; `esp_http_server` does not derive it from `GET` |

The `PROPPATCH` case is the trap. The board log during a failing 1 KB copy:

```
I (13491) webdav: PUT /size_1.bin (0 bytes)          <- create
I (13681) webdav: LOCK /size_1.bin (existing)        <- lock OK
W (13771) httpd_uri: Method '13' not allowed -> 405  <- PROPPATCH
W (13821) httpd_uri: Method '2'  not allowed -> 405  <- HEAD
I (13881) webdav: PUT /size_1.bin (1024 bytes)       <- data arrives intact
W (13981) httpd_uri: Method '13' not allowed -> 405  <- PROPPATCH again
I (14041) webdav: DELETE /size_1.bin                 <- Windows rolls it back
```

The file transfers completely, then Windows fails to stamp its timestamps,
decides the copy failed, and deletes what it just wrote. The "network name is no
longer available" message is Windows describing its own rollback — nothing about
the network was wrong.

This also explains a confusing intermediate result: `[System.IO.File]::WriteAllText`
succeeded while `Copy-Item` failed, because only the latter preserves file times
and therefore triggers `PROPPATCH`.

> [!NOTE]
> `LOCK`/`UNLOCK` are a **stub**. They return a well-formed synthetic token
> without tracking ownership, enforcing locks, or expiring them — enough for the
> Windows redirector, which never verifies the token means anything. Two clients
> writing the same file concurrently will still race. Acceptable for a
> single-user pocket drive; not a general-purpose implementation.
>
> `PROPPATCH` likewise acknowledges without persisting. File contents are
> correct; client-supplied timestamps are dropped in favour of the card's own
> mtime.

Linux's gvfs and davfs2 require neither `LOCK` nor `PROPPATCH` and should mount
read/write, but this has **not been tested on hardware**.

### Without mounting

Any HTTP client works against any path on the card:

```bash
curl http://<board-ip>/test.md
```

On Windows PowerShell use `curl.exe`, not `curl` — the latter is an alias for
`Invoke-WebRequest`, which takes different arguments.

## Verified results

Recorded from real hardware, not expected values.

### Hardware and network

| Check | Result |
| :---- | :----- |
| SD card mounts | PASS — SanDisk SC16G, SDHC, 15193 MB |
| Bus width | 1-bit SPI at 20 MHz (confirms SPI, not SDMMC) |
| WiFi association | PASS — WPA2-PSK, RSSI −66 dBm, channel 10 |
| DHCP lease | PASS — but the address moved between reboots |

### WebDAV verb matrix (via curl)

| Method | Case | Expected | Result |
| :----- | :--- | :------- | :----- |
| OPTIONS | any path | 200 + `DAV: 1` | PASS |
| MKCOL | new collection | 201 | PASS |
| MKCOL | already exists | 405 | PASS |
| PUT | new file | 201 | PASS |
| PUT | overwrite | 204 | PASS |
| GET | existing file | 200 | PASS |
| GET | missing file | 404 | PASS |
| PROPFIND | `Depth: 1` | 207 + well-formed XML | PASS |
| MOVE | to new name | 201 | PASS |
| MOVE | onto existing target | 204 | PASS |
| DELETE | file | 204 | PASS |
| DELETE | missing | 404 | PASS |
| DELETE | folder, recursive | 204 | PASS |

PROPFIND XML was validated with a real XML parser, not eyeballed — malformed
multistatus is the most common reason a client silently refuses to mount.

### Real client mounts

| Client | Browse | Read | Create folder | Rename | Delete | Write file |
| :----- | :----- | :--- | :------------ | :----- | :----- | :--------- |
| curl | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Windows `net use` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Linux gvfs / davfs2 | not tested on hardware | | | | | |

Integrity check: a 910 896-byte firmware binary copied through the Windows mount
came back **SHA-256 identical** (`AF1653C4…87CF4`), and survived a subsequent
move into a subdirectory intact. Throughput was roughly 105 KB/s, consistent
between curl and Explorer — that is the 1-bit SPI SD bus, not the network.

`VER-FRM-01`: **PASS** on Windows. WebDAV works on ESP32-S3 as a genuine
mountable network drive, which retires `RISK-FRM-01`. Linux remains unverified
only because no Linux machine was reachable during testing.

## How it works

ESP-IDF turns out to support WebDAV cleanly without any external library. Three
things were verified in the IDF source before a line was written:

| Assumption | Verified at |
| :--------- | :---------- |
| `httpd_method_t` exposes WebDAV verbs | `esp_http_server.h:115` — a direct `typedef enum http_method`, no subsetting |
| The verbs exist in the parser | `http_parser.h:105-115` — `COPY`=8, `LOCK`=9, `MKCOL`=10, `MOVE`=11, `PROPFIND`=12, `UNLOCK`=15 |
| Custom verbs are not filtered out | `httpd_parse.c:69` — rejects only `method < 0`; no whitelist, no upper bound |

One handler is registered per method against the URI `/*`, with
`httpd_config_t.uri_match_fn = httpd_uri_match_wildcard` so a single handler per
verb serves any path.

The commonly suggested ESPWebDAV library was deliberately not used — it lists
ESP8266 as its primary target, not ESP32.

## Roadmap

Known limitations, deliberately deferred. Each is separate work rather than
something to fold into the current milestone.

### Real locking, if more than one client ever writes

`LOCK`/`UNLOCK` and `PROPPATCH` are stubs (see
[What Windows actually required](#what-windows-actually-required)). They unblock
Windows but enforce nothing. If this ever serves more than one writer, they need
real state: a lock table with tokens, owners and timeouts, enforced on
`PUT`/`DELETE`/`MOVE`, and `PROPPATCH` persisting timestamps via `utime()`.

### Flash size is misconfigured — 2 MB of 32 MB usable

The board has 32 MB of flash but the build is configured for 2 MB, so roughly
30 MB is unreachable. The bootloader reports this every boot:

```
W (355) spi_flash: Detected size(32768k) larger than the size in the
binary image header(2048k). Using the size in the binary image header.
```

The app partition is a single 1 MB and now sits at **87% full** (`0x219d0 bytes
(13%) free`) after adding WebDAV. Adding mDNS and AP-mode fallback on top of
that will hit the ceiling. This is the next thing worth doing.

**Fix:** `idf.py menuconfig` → *Serial flasher config* → *Flash size* → 32 MB,
plus a custom partition table.

### The IP address is not stable

This is not theoretical — it happened during testing. The board came up as
`<board-ip>`, and after a reset the lease moved to `<board-ip>`, silently
invalidating an already-established `net use` mapping. Every bookmark and mount
command breaks the same way.

**Fix:** mDNS, so the board answers to `thumbnas.local` regardless of address. A
DHCP reservation on the router is a stopgap that only helps on one network.

### No access when away from a known network

Station mode needs an access point to join. Away from home there is none.

- **Phone hotspot** — a one-line SSID change. Every device must join the hotspot;
  the phone acts as router and devices keep internet via cellular.
- **SoftAP** — the board becomes the access point. Right answer for a pocket NAS
  with no infrastructure. Two catches: it hands out no internet, and iOS notices
  this and may silently fall back to cellular unless told to stay.

**Planned:** APSTA with fallback — try the known network for ~10 s at boot, bring
up its own AP if that fails. With mDNS the address stays the same in both modes.

### Not addressed at all

- **No authentication.** Anyone on the network can read, write and delete the
  entire card. Fine on a trusted LAN, not fine on a hotspot in public.
- **No HTTPS.** Traffic is plaintext.
- **`COPY` and `Depth: infinity`** are unimplemented. Windows does not appear to
  need either for normal Explorer use.

## Troubleshooting

**`idf.py` not found** — the ESP-IDF environment is not active. On Windows:

```bash
C:\Espressif\frameworks\esp-idf-v5.5.2\export.ps1
```

**`tool xtensa-esp-elf has no installed versions`** — toolchains are present but
at versions a different IDF release installed. Repair with:

```bash
python.exe C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf_tools.py install
```

**Filenames appear as `KART_2~1.CSV`** — FAT long filename support is off. It is
enabled via `CONFIG_FATFS_LFN_HEAP` in `sdkconfig.defaults`; delete `sdkconfig`
and rebuild so the defaults are picked up.

**`Failed to mount filesystem. Card may need FAT32 formatting.`** — the card is
exFAT or has no partition table. Reformat as FAT32.

**`Failed to connect ... check SSID/password`** — the board retries 10 times then
gives up. Check `main/wifi_credentials.h`, and confirm the SSID is 2.4 GHz.

**Windows mount succeeds but the drive is empty or unreachable** — check the IP
in the boot log. A DHCP lease change invalidates an existing mapping while
leaving it listed in `net use`. Remove and re-add it:

```bash
net use Z: /delete
```

**Build fails with a `-Werror=format-truncation` error** — IDF compiles with that
enabled. Use the bounded `path_join` / `str_copy` helpers in `webdav.c` rather
than `snprintf("%s/%s", ...)` between two same-sized buffers.

## Project structure

```
thumb-nas/
├── CMakeLists.txt                     Top-level IDF project file
├── sdkconfig.defaults                 Tracked build settings (FAT LFN, HTTP limits)
├── .gitignore                         Excludes credentials and build output
├── README.md
└── main/
    ├── CMakeLists.txt                 Component registration and requirements
    ├── main.c                         SD mount, WiFi, startup
    ├── webdav.c                       WebDAV method handlers
    ├── webdav.h
    ├── wifi_credentials.h             Your credentials — gitignored
    └── wifi_credentials.h.example     Tracked template
```

## Acknowledgements

Built from Espressif's official examples: `storage/sd_card/sdspi`,
`wifi/getting_started/station`, and the `esp_http_server` component.
