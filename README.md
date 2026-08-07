# thumb-nas

A pocket-sized network drive built on the Waveshare ESP32-S3-Touch-AMOLED-2.06.
An SD card in the board's TF slot, mountable as a real network drive over WiFi.

![Platform](https://img.shields.io/badge/platform-ESP32--S3-informational)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.5.2-red)
![Bus](https://img.shields.io/badge/SD%20bus-SPI%20(1--bit)-yellow)
![WebDAV](https://img.shields.io/badge/WebDAV-class%202-blue)
![Transport](https://img.shields.io/badge/transport-HTTPS%20only-brightgreen)
![Auth](https://img.shields.io/badge/auth-HTTP%20Basic-brightgreen)

A minimal WebDAV server built directly on ESP-IDF's `esp_https_server` — no
Arduino core, no third-party WebDAV library. Mounts as a real network drive on
Windows over TLS with password authentication, verified by a byte-identical
981 KB round trip through Explorer.

---

## Table of contents

- [What this does](#what-this-does)
- [Hardware](#hardware)
- [Requirements](#requirements)
- [Setup](#setup)
- [Build and flash](#build-and-flash)
- [Mounting as a network drive](#mounting-as-a-network-drive)
- [Troubleshooting](#troubleshooting)
- [Verified results](#verified-results)
- [How it works](#how-it-works)
- [Security model](#security-model)
- [Roadmap](#roadmap)
- [Project structure](#project-structure)

---

## What this does

On boot the firmware:

1. Mounts the TF card over SPI at `/sdcard`
2. Writes a small `test.md` to it
3. Joins WiFi as a station
4. Prints its IP address, then starts two listeners:
   - **port 443** — the WebDAV server, TLS + password required
   - **port 80** — serves only `GET /ca.crt`, so a new client can bootstrap
     trust. Nothing else is reachable there.

Supported methods: `OPTIONS`, `PROPFIND`, `PROPPATCH`, `GET`, `HEAD`, `PUT`,
`DELETE`, `MKCOL`, `MOVE`, `LOCK`, `UNLOCK`. Announced as DAV class 2.

Everything is HTTPS-only and behind HTTP Basic authentication. `COPY` and
`Depth: infinity` are not implemented.

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

The boot log settles it empirically — `sdmmc_card_print_info()` reports
`SSR: bus_width=1`, confirming single-bit SPI.

## Requirements

- **ESP-IDF v5.5.2** (earlier 5.x will likely work but is untested here)
- A TF card formatted **FAT32** — not exFAT
- Python `cryptography` (already present in the ESP-IDF Python environment)

> [!IMPORTANT]
> Windows formats cards above 32 GB as exFAT by default. The mount config sets
> `format_if_mount_failed = false`, so an exFAT card fails the mount rather than
> being silently reformatted. That is deliberate — a diagnostic should not
> destroy your data — but the card must be FAT32 before you start.

## Setup

### 1. Credentials

Copy the template and fill in your own values:

```bash
copy main\wifi_credentials.h.example main\wifi_credentials.h
```

```c
#define WIFI_SSID     "your-network-name"
#define WIFI_PASSWORD "your-wifi-password"
#define WEBDAV_USER   "your-webdav-username"
#define WEBDAV_PASS   "your-webdav-password"
```

If the file is missing, the build fails with an explicit `#error` rather than a
confusing undefined-symbol error.

The board joins as a 2.4 GHz station. If your router broadcasts a separate
5 GHz-only SSID, use the 2.4 GHz one.

### 2. TLS certificates

Generate the keypair. Pass the board's IP and your LAN's subnet:

```bash
python tools/gencert.py main/certs 192.168.1.42 192.168.1.0/24
```

This writes four files into `main/certs/` (all gitignored):

| File | Role |
| :--- | :--- |
| `ca.crt` | The certificate authority. **This is the one you install on client machines.** |
| `ca.key` | The CA's key. Never leaves your machine, never goes on the board. |
| `server.crt` | Leaf + CA chain, embedded in the firmware. |
| `server.key` | The leaf's key, embedded in the firmware. Cannot sign anything. |

Why two certificates rather than one self-signed cert is explained under
[Security model](#security-model). It matters more than it looks.

> [!WARNING]
> `build/` is gitignored **for security, not tidiness**. The linked firmware
> image contains your WiFi password, your WebDAV password and the TLS private
> key as plain data. Never commit `build/`, and never hand anyone a prebuilt
> `.bin` of this firmware.

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

Take the IP address from the boot log. Examples below use `<board-ip>`.

> [!TIP]
> Client-side instructions live in **[SETUP.md](SETUP.md)** — that is the file
> to hand to anyone who just wants to use the drive.

### Step 1 — get the CA from the device, and trust it (once per client machine)

Because the certificate is issued by your own CA rather than a public one,
**every machine that mounts the drive must be told to trust `ca.crt` first.**
There is no way around this short of owning a public domain name. Windows will
refuse to mount with `System error 1790` until you do.

The device serves its own certificate for this purpose. Open it in any browser,
no flags and no prior trust required, and save the file:

```
http://<board-ip>/ca.crt
```

That is a separate plain-HTTP listener on port 80 which serves **only** that one
path — see [Certificate distribution](#certificate-distribution) for why it is
unencrypted and why it cannot reach the SD card.

Then trust it. **Windows** — in an **Administrator** prompt:

```bash
certutil -addstore -f "Root" path\to\ca.crt
```

To undo:

```bash
certutil -delstore "Root" "thumb-nas local CA"
```

Install **`ca.crt`**, not `server.crt`.

### Step 2 — mount

**Windows**, in a normal prompt:

```bash
net use Z: https://<board-ip>/ /user:your-webdav-username your-webdav-password
```

Omit the credentials to be prompted for them instead:

```bash
net use Z: https://<board-ip>/ *
```

Via the GUI: *This PC* → *Map Network Drive* → `https://<board-ip>/` → tick
**Connect using different credentials** → enter the `WEBDAV_USER` /
`WEBDAV_PASS` pair from `wifi_credentials.h`. It is **not** your Windows login
and not your WiFi password.

Windows maps this internally to `\\<board-ip>@SSL\DavWWWRoot`. Browsing,
reading, writing, creating folders, renaming and deleting all work.

To disconnect:

```bash
net use Z: /delete
```

### Linux

> [!NOTE]
> **Not tested.** No Linux machine was reachable during development, so the
> commands below are written from the protocol behaviour rather than confirmed
> on hardware. They are the conventional invocations and the server implements
> everything they need, but treat them as untested until someone runs them.

Trusting the CA, on Debian/Ubuntu:

```bash
sudo cp ca.crt /usr/local/share/ca-certificates/thumb-nas.crt && sudo update-ca-certificates
```

GNOME Files → *Other Locations* → *Connect to Server* → `davs://<board-ip>/`
(note `davs`, not `dav` — the `s` selects TLS). Or:

```bash
gio mount davs://<board-ip>/
```

With davfs2, which prompts for the username and password:

```bash
sudo mount -t davfs https://<board-ip>/ /mnt/thumbnas
```

### macOS / iOS / Android

Also untested. All of them require the CA to be installed as a profile first,
and iOS additionally requires manually enabling it under *Settings → General →
About → Certificate Trust Settings* — installing the profile alone is not
enough there.

### Without mounting

Any HTTPS client works against any path:

```bash
curl --cacert ca.crt -u your-webdav-username https://<board-ip>/test.md
```

On Windows PowerShell use `curl.exe`, not `curl` — the latter is an alias for
`Invoke-WebRequest`, which takes different arguments. See the revocation note in
[Troubleshooting](#troubleshooting) if that fails.

## Troubleshooting

### "The certificate is not trusted" / `System error 1790`

The CA has not been installed on that machine. See
[Step 1](#step-1--trust-the-ca-once-per-client-machine). This is the single most
likely first-time snag, and Windows' phrasing — *"The network logon failed"* —
gives no hint that certificates are involved.

Verify the CA is present:

```bash
certutil -store "Root" "thumb-nas local CA"
```

### curl fails with `CRYPT_E_NO_REVOCATION_CHECK` even after trusting the CA

**This one is expected, and the mount still works.** Windows' schannel tries to
check whether the certificate has been revoked, which requires a CRL or OCSP
endpoint. A private CA publishes neither, so the check cannot complete and curl
treats that as fatal:

```
schannel: next InitializeSecurityContext failed: CRYPT_E_NO_REVOCATION_CHECK
(0x80092012) - The revocation function was unable to check revocation
```

Add `--ssl-no-revoke`:

```bash
curl --ssl-no-revoke -u your-webdav-username https://<board-ip>/test.md
```

Windows' WebClient service — the thing behind `net use` — is *less* strict here
than curl and mounts without complaint. So this affects command-line testing
only, not the drive itself.

### Mount succeeds but the drive is unreachable later

Check the IP in the boot log. A DHCP lease change invalidates an existing
mapping while leaving it listed in `net use`, and the leaf certificate pins the
IP it was issued for. Remove and re-add the mapping:

```bash
net use Z: /delete
```

If the address changed, regenerate the leaf for the new one — the CA covers the
whole subnet, so it is one command and no re-trusting:

```bash
python tools/gencert.py main/certs <new-ip> 192.168.1.0/24
```

### Filenames appear as `KART_2~1.CSV`

FAT long filename support is off. It is enabled via `CONFIG_FATFS_LFN_HEAP` in
`sdkconfig.defaults`; delete `sdkconfig` and rebuild so the defaults are picked
up.

### `Failed to mount filesystem. Card may need FAT32 formatting.`

The card is exFAT or has no partition table. Reformat as FAT32.

### `idf.py` not found

The ESP-IDF environment is not active. On Windows:

```bash
C:\Espressif\frameworks\esp-idf-v5.5.2\export.ps1
```

### `tool xtensa-esp-elf has no installed versions`

Toolchains are present but at versions a different IDF release installed:

```bash
python.exe C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf_tools.py install
```

### Build fails with `-Werror=format-truncation`

IDF compiles with that enabled. Use the bounded `path_join` / `str_copy` helpers
in `webdav.c` rather than `snprintf("%s/%s", ...)` between two same-sized
buffers.

## Verified results

Recorded from real hardware, not expected values.

### Hardware and network

| Check | Result |
| :---- | :----- |
| SD card mounts | PASS — SanDisk SC16G, SDHC, 15193 MB |
| Bus width | 1-bit SPI at 20 MHz (confirms SPI, not SDMMC) |
| WiFi association | PASS — WPA2-PSK, RSSI −66 dBm, channel 10 |
| DHCP lease | PASS — but the address moved between reboots |

### WebDAV verb matrix (curl, over HTTPS)

| Method | Case | Expected | Result |
| :----- | :--- | :------- | :----- |
| OPTIONS | any path | 200 + `DAV: 1,2` | PASS |
| MKCOL | new collection | 201 | PASS |
| MKCOL | already exists | 405 | PASS |
| PUT | new file | 201 | PASS |
| PUT | overwrite | 204 | PASS |
| GET | existing / missing | 200 / 404 | PASS |
| PROPFIND | `Depth: 1` | 207 + well-formed XML | PASS |
| MOVE | new / existing target | 201 / 204 | PASS |
| DELETE | file / missing / recursive | 204 / 404 / 204 | PASS |
| LOCK | new path | 201 + `Lock-Token` | PASS |
| UNLOCK | — | 204 | PASS |

PROPFIND XML was validated with a real XML parser, not eyeballed — malformed
multistatus is the most common reason a client silently refuses to mount.

### Authentication

| Case | Result |
| :--- | :----- |
| No `Authorization` header | 401 + `WWW-Authenticate: Basic realm="Keychain NAS"` |
| Wrong password | 401 |
| Wrong username | 401 |
| Correct, across PROPFIND / PUT / GET / DELETE | 207 / 201 / 200 / 204 |

### Certificate chain

| Test | Result |
| :--- | :----- |
| Validate by IP against `ca.crt` | 200 |
| Validate by `keychain-nas.local` | 200 |
| Validate for a name **outside** the CA's constraints | **rejected** |

That last row is the point of the two-cert design — the CA cannot vouch for
anything beyond its permitted subtrees, and this was confirmed rather than
assumed.

### Real client mounts

| Client | Browse | Read | Write | Notes |
| :----- | :----- | :--- | :---- | :---- |
| curl (HTTPS) | ✅ | ✅ | ✅ | needs `--ssl-no-revoke` on Windows |
| Windows `net use` (HTTPS) | ✅ | ✅ | ✅ | after trusting `ca.crt` |
| Linux gvfs / davfs2 | — | — | — | **not tested, no machine available** |

A 981 824-byte firmware binary copied through the Windows HTTPS mount came back
**SHA-256 identical** (`E078A671…62D1`). Throughput ≈ 84 KB/s over TLS, against
≈ 105 KB/s over plain HTTP — the gap is TLS overhead on top of the 1-bit SPI SD
bus, which is the real ceiling.

## How it works

ESP-IDF supports WebDAV cleanly without any external library. Everything was
verified in the IDF source before code was written:

| Assumption | Verified at |
| :--------- | :---------- |
| `httpd_method_t` exposes WebDAV verbs | `esp_http_server.h:115` — a direct `typedef enum http_method` |
| The verbs exist in the parser | `http_parser.h:105-115` — `COPY`=8, `LOCK`=9, `MKCOL`=10, `MOVE`=11, `PROPFIND`=12, `PROPPATCH`=13, `UNLOCK`=15 |
| Custom verbs are not filtered out | `httpd_parse.c:69` — rejects only `method < 0` |
| TLS config field names | `esp_https_server.h:74` — `servercert` (**not** `cacert_pem`, which older docs show), and `httpd_config_t` nested as `.httpd` |

One handler is registered per method against the URI `/*`, with
`config.httpd.uri_match_fn = httpd_uri_match_wildcard`. There is no global
pre-handler hook in this IDF version, so each handler opens with a
`DAV_REQUIRE_AUTH` guard.

The commonly suggested ESPWebDAV library was deliberately not used — it lists
ESP8266 as its primary target, not ESP32.

### What Windows actually required

Worth recording, because the failure modes were misleading. Windows needs three
verbs beyond the obvious set:

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
decides the copy failed, and deletes what it just wrote.

### Hidden files

PROPFIND omits `System Volume Information`, `$RECYCLE.BIN`, `desktop.ini`,
`Thumbs.db`, `.DS_Store` and AppleDouble `._*` files from listings.

This is a **display filter, not access control**. Those paths can still be
created, read and deleted by explicit request — which matters, because Windows
and macOS actively write some of them and would break if the writes were
refused.

## Security model

What this protects against, and what it does not.

**File access is TLS-only.** Basic auth sends credentials as base64, which is
reversible, so it is only defensible underneath TLS.

### Certificate distribution

There is exactly one plain-HTTP endpoint: `GET /ca.crt` on port 80.

It exists because a client that does not yet hold the CA cannot complete a TLS
handshake, and therefore cannot use HTTPS to fetch the thing it needs in order
to use HTTPS. Something has to break that circle.

Serving it unencrypted discloses nothing. `ca.crt` is the *public* half of the
certificate — the device already presents it to every client during every TLS
handshake.

It is confined by construction, not by convention:

- a **separate `httpd` instance** in its own translation unit
  (`cacert_server.c`), so it cannot inherit a WebDAV handler by accident
- `max_uri_handlers = 1` and `uri_match_fn = NULL`, so matching is exact and
  there is no wildcard route to fall through to
- one registered route, `GET /ca.crt`. Nothing else is reachable, and it never
  touches `/sdcard`

Verified on hardware: `/`, `/test.md`, `/algoarts`, `/server.key`, `/ca.key` and
`/ca.crt/../test.md` all return 404, and `PUT`/`DELETE`/`PROPFIND` on `/ca.crt`
return 405.

The residual risk is trust-on-first-use: someone on-path during that single
unencrypted fetch could substitute their own CA. Everything afterwards is
protected. Fetch it on a network you trust, or verify the fingerprint out of
band.

**The certificate is a two-cert chain, deliberately.** A single self-signed
certificate has to be marked `CA=TRUE` to be trusted, and installing that into a
machine's root store makes it a full certificate authority there. Its private
key ships in the board's flash, recoverable by anyone who can read the chip over
USB — so a stolen board would yield a key able to impersonate *any* HTTPS site
to every machine that trusted it.

`nameConstraints` cannot fix that on a single certificate: per RFC 5280 the
extension binds *subordinate* certificates, not the one carrying it, so a
self-signed leaf is not constrained by its own extension.

Splitting them does fix it:

- `ca.key` never leaves your machine and never goes near the board
- the CA carries a critical `nameConstraints` limited to `keychain-nas.local`
  and your subnet
- the leaf on the board is `CA=FALSE` and cannot sign anything

A dumped board therefore yields a key that can impersonate one hostname on one
subnet, not your bank. This was tested, not assumed — see
[Certificate chain](#certificate-chain).

### Known weaknesses

- **Locking is a stub.** `LOCK`/`UNLOCK` return a well-formed synthetic token
  without tracking ownership or expiry. Enough for the Windows redirector, which
  never verifies the token means anything. Two clients writing the same file
  concurrently will still race.
- **`PROPPATCH` acknowledges without persisting.** File contents are correct;
  client-supplied timestamps are dropped in favour of the card's own mtime.
- **One shared credential pair**, compiled into the firmware. No per-user
  accounts, no rotation without a reflash.
- **No brute-force protection.** Nothing rate-limits failed logins.
- **Data at rest is unencrypted.** Anyone holding the physical card reads
  everything; TLS protects the wire only.

## Roadmap

### Flash size is misconfigured — 2 MB of 32 MB usable

**The most urgent item.** The board has 32 MB of flash but the build is
configured for 2 MB, so roughly 30 MB is unreachable. The bootloader says so
every boot:

```
W (355) spi_flash: Detected size(32768k) larger than the size in the
binary image header(2048k). Using the size in the binary image header.
```

The app partition is a single 1 MB and now sits at **94% full** (`0x104c0
bytes, 6% free`) after TLS and auth. The next feature of any size will not link.

**Fix:** `idf.py menuconfig` → *Serial flasher config* → *Flash size* → 32 MB,
plus a custom partition table.

### The IP address is not stable

Not theoretical — it happened during development. The board came up on one
address, and after a reset the lease moved, silently invalidating an established
`net use` mapping. The leaf certificate also pins the IP in its SAN, so a lease
change breaks TLS validation by address too.

**Fix:** mDNS, so the board answers to `keychain-nas.local` regardless of
address — which is already the certificate's primary SAN, so trust survives the
move. A DHCP reservation is a stopgap that helps on one network only.

### No access away from a known network

Station mode needs an access point to join.

- **Phone hotspot** — a one-line SSID change; the phone acts as router and
  devices keep internet via cellular.
- **SoftAP** — the board becomes the access point. Right answer for a pocket NAS
  with no infrastructure. Two catches: it hands out no internet, and iOS notices
  this and may silently fall back to cellular unless told to stay.

**Planned:** APSTA with fallback — try the known network for ~10 s at boot, bring
up its own AP if that fails. With mDNS the address stays stable in both modes.

### Smaller items

- Real lock state, if more than one client ever writes
- `PROPPATCH` persisting timestamps via `utime()`
- `COPY` and `Depth: infinity` (Windows appears not to need either)
- Verifying the Linux mount path on actual hardware

## Project structure

```
thumb-nas/
├── CMakeLists.txt                     Top-level IDF project file
├── partitions.csv                     4MB app partition out of the 32MB chip
├── sdkconfig.defaults                 Tracked build settings (flash size, LFN, HTTPS)
├── .gitignore                         Excludes credentials, certs, build output
├── README.md                          This file — how it works, how it was built
├── SETUP.md                           Client-side instructions for end users
├── tools/
│   └── gencert.py                     Generates the constrained CA and leaf
└── main/
    ├── CMakeLists.txt                 Component registration, EMBED_TXTFILES
    ├── main.c                         SD mount, WiFi, startup
    ├── webdav.c                       Auth, WebDAV handlers, TLS server start
    ├── webdav.h
    ├── cacert_server.c                Plain-HTTP CA distribution, one route only
    ├── cacert_server.h
    ├── certs/                         Generated TLS material — gitignored
    ├── wifi_credentials.h             Your secrets — gitignored
    └── wifi_credentials.h.example     Tracked template
```

## Acknowledgements

Built from Espressif's official examples: `storage/sd_card/sdspi`,
`wifi/getting_started/station`, and the `esp_http_server` /
`esp_https_server` components.
