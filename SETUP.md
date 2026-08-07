# Setting up a client

How to get a device — laptop, phone, tablet — connected to the thumb-nas drive.

You need two things: the **certificate**, and the **login**. The certificate comes
from the device itself; the login is whatever `WEBDAV_USER` / `WEBDAV_PASS` were
set to when the firmware was built.

The device advertises itself over mDNS as **`keychain-nas.local`**. Use that
name rather than an IP address — the IP changes whenever DHCP reassigns it, and
has done so repeatedly; the name does not. It prints both on startup:

```
STEP 1, get the certificate:  http://keychain-nas.local/ca.crt
STEP 2, mount the drive:      net use Z: https://keychain-nas.local/ *
The name above survives DHCP changes. Current address is 192.168.x.x if
mDNS is blocked on your network.
```

> [!IMPORTANT]
> **mDNS needs your client and the device on the same WiFi network — not just
> the same router.** The ESP32-S3 has no 5 GHz radio, so it is always on the
> 2.4 GHz SSID. If your laptop is on a separate 5 GHz SSID, many routers will
> route ordinary traffic between them but *not* forward the multicast that mDNS
> depends on. This was confirmed during development: unicast worked while a raw
> mDNS query got no reply at all.
>
> If `keychain-nas.local` does not resolve, join the same SSID as the device, or
> use the IP fallback documented at each step below.

---

## Step 1 — Get the certificate from the device

**Open this in any browser:**

```
http://keychain-nas.local/ca.crt
```

Save the file. That is the whole step. No flags, no GitHub, no USB stick — the
device serves its own certificate over plain HTTP for exactly this purpose.

If that name does not resolve, use the address from the boot log instead —
`http://<board-ip>/ca.crt` — and see
[mDNS does not resolve](#keychain-naslocal-does-not-resolve).

It downloads as `ca.crt` (652 bytes, beginning `-----BEGIN CERTIFICATE-----`).
Most browsers will offer to save or install it rather than displaying it,
because the device sends it as `application/x-x509-ca-cert`.

<details>
<summary>Why this one path is plain HTTP when everything else is HTTPS</summary>

A client that does not yet have the certificate cannot complete a TLS handshake
with the device, so it cannot use HTTPS to fetch the thing it needs in order to
use HTTPS. Something has to break that circle.

Serving it unencrypted is safe in the sense that matters: this is the *public*
half of the certificate. The device already hands it to every client during
every TLS handshake. Nothing is disclosed by publishing it.

The plain-HTTP listener serves **only** this one path. It is a separate server
from the WebDAV one, with a single registered route and no wildcard matching, so
it has no ability to reach the SD card at all. Requests for anything else return
404, and any method other than GET returns 405.

</details>

> [!WARNING]
> **Do this step on a network you trust.** Someone positioned between you and
> the device could substitute their own certificate during this one unencrypted
> fetch, and you would then trust *them*. Every step afterwards is protected,
> but this first fetch is trust-on-first-use. On your home network that is fine.
> On a café hotspot, do it later.
>
> To verify you got the right file, compare its fingerprint against the one from
> the machine that generated it:
> ```bash
> certutil -hashfile ca.crt SHA256
> ```

## Step 2 — Tell your machine to trust it

This is per-machine and only needs doing once.

### Windows

In an **Administrator** prompt:

```bash
certutil -addstore -f "Root" path\to\ca.crt
```

Check it took:

```bash
certutil -store "Root" "thumb-nas local CA"
```

To remove it later:

```bash
certutil -delstore "Root" "thumb-nas local CA"
```

### Linux (Debian / Ubuntu)

> Not tested — no Linux machine was available during development.

```bash
sudo cp ca.crt /usr/local/share/ca-certificates/thumb-nas.crt && sudo update-ca-certificates
```

### macOS

> Not tested.

Double-click `ca.crt` to open Keychain Access, add it to the **System**
keychain, then open it and set *Trust → When using this certificate* to
**Always Trust**.

### iOS / iPadOS

> Not tested.

Two stages, and **the first alone is not enough** — this catches people out:

1. Open `http://<board-ip>/ca.crt` in Safari. It downloads a configuration
   profile. Install it under *Settings → General → VPN & Device Management*.
2. Then separately go to *Settings → General → About → Certificate Trust
   Settings* and switch on full trust for "thumb-nas local CA".

Skipping stage 2 leaves the certificate installed but untrusted, and mounting
will still fail.

### Android

> Not tested.

*Settings → Security → Encryption & credentials → Install a certificate → CA
certificate*. Android will warn that a third party may monitor traffic; that
warning is about CA certificates in general.

## Step 3 — Mount the drive

### Windows

```bash
net use Z: https://keychain-nas.local/ *
```

The `*` makes it prompt for the password. To pass credentials directly:

```bash
net use Z: https://keychain-nas.local/ /user:your-webdav-username your-webdav-password
```

Via the GUI: *This PC* → *Map Network Drive* → `https://keychain-nas.local/` →
tick **Connect using different credentials**.

Using the hostname is what makes the mapping survive a DHCP change. A mapping
made against a raw IP breaks silently the next time the lease moves — it stays
listed in `net use` while being unreachable.

> [!NOTE]
> The username and password are the `WEBDAV_USER` / `WEBDAV_PASS` pair from the
> firmware. **Not** your Windows login, and **not** your WiFi password.

To disconnect:

```bash
net use Z: /delete
```

### Linux

> Not tested.

```bash
gio mount davs://<board-ip>/
```

Note `davs://`, not `dav://` — the `s` selects TLS. Or with davfs2, which
prompts for the login:

```bash
sudo mount -t davfs https://<board-ip>/ /mnt/thumbnas
```

### Without mounting

Any HTTPS client works against any path:

```bash
curl --cacert ca.crt -u your-webdav-username https://<board-ip>/test.md
```

---

## If something goes wrong

### `keychain-nas.local` does not resolve

Symptoms: `ping keychain-nas.local` says the host could not be found, and
browsers hang or fail immediately.

**Most likely cause: your client and the device are on different WiFi networks.**
The ESP32-S3 is 2.4 GHz only. If your laptop is on a 5 GHz SSID — even one from
the same router, where the two SSIDs differ only by a `_5G` style suffix —
normal traffic is usually routed between them but multicast often is not, and
mDNS is built entirely on multicast.

This is not hypothetical. During development, from a laptop on the 5 GHz SSID:

- `https://<board-ip>/status` returned **200** — unicast fine
- a raw mDNS query to `224.0.0.251:5353` got **no response at all**

To confirm it is the network rather than your machine, run the same raw query
yourself. On Linux or macOS:

```bash
dig +short @224.0.0.251 -p 5353 keychain-nas.local
```

Fixes, in order of preference:

1. **Join the same SSID as the device** — the 2.4 GHz one. This is the real fix.
2. **Use the IP address** from the boot log as a fallback. It works, but breaks
   whenever the lease changes, which is the problem mDNS exists to solve.
3. **Reserve the address** on your router (bind it to the board's MAC) so the IP
   at least stops moving.

Other causes worth checking if you are on the same SSID:

- **AP isolation / guest mode** on the router blocks client-to-client traffic
  including multicast
- **Windows network profile set to Public** — mDNS is blocked there. Check with
  `Get-NetConnectionProfile`; it should say `Private`
- **Mesh systems and VLANs** frequently drop multicast between nodes

### `System error 1790` / "The network logon failed"

Step 2 has not been done on this machine, or was done for the wrong file.
Install **`ca.crt`**, not `server.crt`.

### curl says `CRYPT_E_NO_REVOCATION_CHECK`

Expected, and it does not affect mounting. Windows tries to check whether the
certificate has been revoked, which needs a CRL or OCSP server that a private CA
does not run. Add `--ssl-no-revoke`:

```bash
curl --ssl-no-revoke --cacert ca.crt -u your-webdav-username https://<board-ip>/test.md
```

Windows' WebClient service — the thing behind `net use` — is less strict about
this than curl, which is why the drive mounts even though curl complains.

### The drive worked yesterday and does not today

The device's IP most likely changed. It uses DHCP, and the certificate is issued
against the address it had at the time. Check the current address in the boot
log, then remove and re-add the mapping:

```bash
net use Z: /delete
```

If the address really has changed, the certificate needs reissuing for the new
one — see the project README. The CA covers the whole subnet, so this does not
mean re-trusting anything on your clients.

### `http://<board-ip>/ca.crt` does not load

The device may not have finished joining WiFi, or did not join at all. Watch the
boot log over USB:

```bash
idf.py -p COM13 monitor
```

A successful boot prints `Connected to <your-ssid>` followed by both servers
starting. If it prints `Failed to connect`, check the credentials compiled into
the firmware and confirm the SSID is 2.4 GHz.
