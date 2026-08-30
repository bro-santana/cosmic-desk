# Protocol notes

Cosmic Desk speaks the **Moonlight / NVIDIA GameStream protocol**, the same one used by
Sunshine (host) and Moonlight (client). Anything not described here is stock protocol
behaviour inherited from the vendored upstreams.

## Ports

Every port derives from a single `port_base` setting (default **47989**), matching
Sunshine's scheme. Change `port_base` in `cosmic.json` if a stock Sunshine install is
already using these ports on the same machine.

| Offset | Default | Proto | Purpose |
|---|---|---|---|
| −5 | 47984 | TCP/HTTPS | Pairing completion, authenticated API (applist, launch, resume, cancel, wallpaper, clipboard) |
| 0 | 47989 | TCP/HTTP | `serverinfo`, pairing phases 1–4 |
| +9 | 47998 | UDP | Video RTP |
| +10 | 47999 | UDP | Control / input (ENet, encrypted) |
| +11 | 48000 | UDP | Audio RTP |
| +21 | 48010 | TCP | RTSP |

Sunshine's web-configuration port (`port_base + 1`, 47990) is **not** used: Cosmic Desk
has no web UI.

## Pairing

Standard Moonlight PIN pairing, unchanged on the wire:

1. `GET /pair?phrase=getservercert&salt=<hex>&clientcert=<hex>&uniqueid=...`
2. `GET /pair?clientchallenge=<hex>`
3. `GET /pair?serverchallengeresp=<hex>`
4. `GET /pair?clientpairingsecret=<hex>`
5. `GET /pair?phrase=pairchallenge` over HTTPS with the client certificate

The pairing AES key is derived from `SHA-256(salt || PIN)`. The only Cosmic Desk change
is **where the PIN is entered**: stock Sunshine requires the user to open its web UI,
while Cosmic Desk surfaces a native dialog from the tray application and feeds the
result to the same internal entry point. Pairing is persistent — once a client
certificate is stored, later connections need no confirmation.

## Cosmic extension: `/serverinfo` display list

The viewer needs the host's monitor list *and* its desktop resolution **before** the
stream starts, so that "Host native" resolution works and the top-bar monitor selector
can be populated. Cosmic Desk adds two elements to the existing `/serverinfo` XML
response:

```xml
<CosmicVersion>2</CosmicVersion>
<CosmicDisplays>
  <Display index="0" name="\\.\DISPLAY1" width="2560" height="1440" fps="165" primary="1" active="1"/>
  <Display index="1" name="\\.\DISPLAY2" width="1920" height="1080" fps="60" primary="0" active="0"/>
</CosmicDisplays>
```

- `active="1"` marks the display currently being captured.
- Version 2 adds `<CosmicWallpaperHash>`, described in the next section; version 3 adds
  the clipboard-sync routes described further below.
- Stock Moonlight clients ignore unknown XML elements, so a stock client still pairs and
  streams against a Cosmic Desk host.
- When the elements are absent (i.e. the host is stock Sunshine), the viewer falls back
  to 1920×1080 for "Host native".

## Cosmic extension: wallpaper sync

The viewer can show the host's desktop wallpaper before a session starts. `/serverinfo`
gains one more optional element alongside `<CosmicDisplays>`:

```xml
<CosmicVersion>2</CosmicVersion>
<CosmicWallpaperHash>9f86d081884c7d65...</CosmicWallpaperHash>
```

- The value is the lowercase hex SHA-256 (64 characters) of the wallpaper image file.
  The element is served on both the HTTP and HTTPS ports.
- Omitted when there is nothing to hash: a solid-colour desktop, no interactive session,
  an unreadable file, a file over 8 MB, or the host's `share_wallpaper` setting off.

The image itself is fetched from `GET /cosmic/wallpaper`, available **only on the HTTPS
port** (`port_base − 5`, default 47984) — i.e. only to clients whose certificate has
already passed verification. There is no equivalent route on the plain HTTP port. It
returns the wallpaper file bytes as-is, with `Content-Type` sniffed from magic bytes
(`image/jpeg`, `image/png`, `image/bmp`); anything else, or no current wallpaper, is a
404. Same ~8 MB size cap as the hash above.

Wallpapers are frequently personal photos, so the split mirrors the stream's own trust
boundary: the hash, which leaks nothing meaningful, rides the open port, while the image
is confined to the client-certificate-authenticated port. `share_wallpaper` in
`cosmic.json` (default on) turns the whole feature off host-side.

On the client, the existing ~10 s `/serverinfo` presence poll extracts the hash; a change
from the cached hash triggers exactly one download, cached at
`<config dir>/wallpapers/<host-key>.<hash>.img`. The host never pushes an update.

Stock Moonlight clients ignore the unknown elements and still pair and stream; against a
stock Sunshine host the hash is simply absent, so the client shows no wallpaper.

(The `<CosmicVersion>2</CosmicVersion>` shown above is the version wallpaper sync itself
requires; the clipboard-sync extension below bumps the live value to `3`. The example is
unchanged because wallpaper sync's own contract still only needs version 2.)

## Cosmic extension: clipboard sync

The viewer and host share the text clipboard while a stream is running, in both
directions. `/serverinfo` bumps to:

```xml
<CosmicVersion>3</CosmicVersion>
```

Version 3 is what adds the two routes below; nothing else about `/serverinfo` changes.

Both routes live **only on the client-certificate-authenticated HTTPS port**
(`port_base − 5`, default 47984), like `/cosmic/wallpaper` above — there is no plain-HTTP
equivalent. Both GET success statuses (200 and 204) also carry
`X-Cosmic-Clipboard-Seq: <seq>`; the 404 carries no header:

| Route | Condition | Status | Response |
|---|---|---|---|
| `GET /cosmic/clipboard?since=<seq>` | sharing on, stream active, seq > `since` | 200 | clipboard text body |
| `GET /cosmic/clipboard?since=<seq>` | sharing on, stream active, unchanged | 204 | no body |
| `GET /cosmic/clipboard?since=<seq>` | sharing off, or no stream active | 404 | no header |
| `POST /cosmic/clipboard` | sharing on, stream active, body ≤ 1 MiB | 200 | — |
| `POST /cosmic/clipboard` | sharing off, or no stream active | 404 | — |
| `POST /cosmic/clipboard` | body > 1 MiB | 413 | rejected, never truncated |

- Sequence numbers are a monotonically increasing `uint64` per host clipboard store,
  starting at 1; the client starts at `since=0`. Republishing identical text does not
  bump the sequence. The same 1 MiB cap applies host-side: an oversized local clipboard
  change is simply not published, and the sequence does not advance.
- The counter is process-local and resets to 0 when the host process restarts, so a
  client cursor can end up *ahead* of the store. The GET handler treats a `since` value
  greater than the current sequence as stale and serves the current text rather than
  answering 204 forever; when the store has never been written, the 204 simply reports
  sequence 0 and the client resets its cursor from the header.
- Text only: no file transfer, no images, no paste-as-typing.
- A single `share_clipboard` setting in `cosmic.json` (default on, exposed in the Bridge
  settings panel as "Share clipboard") governs **both** directions; turning it off makes
  both routes answer 404.

Sync is active **only while a streaming session is running** — outside a session both
routes answer 404. The gate is "is any stream currently active on this host", not "is
the requesting client the one streaming": the vendored `rtsp_stream` (`host/sunshine/src/rtsp.h:56`)
exposes only a session count, with no public mapping from an active session back to a
client certificate. This is a known limitation, stated plainly rather than buried: any
*paired* client can read or write the clipboard while some other paired client is
streaming. It does not go further than that — an unpaired client cannot reach these
routes at all, because the HTTPS server's certificate-verification hook rejects it before
either handler runs.

On the client, while streaming, the viewer polls the GET route roughly once a second
with the last sequence it saw, and POSTs when the local clipboard changes. The client
requires the `X-Cosmic-Clipboard-Seq` response header before trusting a 200 body: an
unauthorized or stock Sunshine host answers 200 with a Sunshine-style error XML body
(401 for an unauthorized client, 404 from a stock host), so the status code alone proves
nothing. This is the same defensive lesson as the wallpaper fetch's magic-byte check
above.

Each poll opens a fresh HTTPS connection rather than reusing one, so an active stream
costs roughly one connection (and TLS handshake) per second. This is an accepted v1
cost, not an oversight; a long-poll variant is the intended future fix.

Stock Moonlight clients simply never call these routes; against a stock Sunshine host the
routes 404 and the client syncs nothing. Neither case affects pairing or streaming.

## Cosmic convention: mid-stream monitor switching

Switching the captured monitor uses **no new protocol**. Sunshine already maps the
client hotkey `Ctrl+Alt+Shift+F1…F13` to an internal display-switch event that reinitialises
capture *without tearing down the session*. The Cosmic viewer's monitor dropdown simply
synthesises that key combination over the existing encrypted input channel:

```
index i  ->  Ctrl+Alt+Shift+F(1+i)   (key down, then key up in reverse order)
```

### The ordering contract

`index` in `<CosmicDisplays>` and the F-key offset consumed by the host must refer to the
same display. Both sides index into the **same `platf::display_names()` ordering**, which
is what makes the mapping correct by construction. Any change to how displays are
enumerated must update both call sites together; each is marked with a comment saying so.

Consequences:

- At most 13 monitors can be addressed (F1–F13).
- The viewer re-fetches `/serverinfo` whenever the monitor dropdown is opened, so hotplug
  changes are picked up within seconds.
- After a switch the viewer sends explicit key-up events for all modifiers, so no stuck
  Ctrl/Alt/Shift state can leak into the host.
