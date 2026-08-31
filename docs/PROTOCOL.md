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
- Version 2 adds `<CosmicWallpaperHash>`, described in the next section; version 3 added
  the clipboard-sync routes described further below; version 4 added `wait=1`
  long-polling and the per-certificate owner gate on those routes; version 5 added
  image (PNG) clipboard support and the `X-Cosmic-Clipboard-Version` response header —
  all described further down.
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
requires; the clipboard-sync extension below bumps the live value to `5`. The example is
unchanged because wallpaper sync's own contract still only needs version 2.)

## Cosmic extension: clipboard sync

The viewer and host share the clipboard while a stream is running, in both directions.
Both text and PNG images are supported; there is still no file transfer and no
paste-as-typing. `/serverinfo` bumps to:

```xml
<CosmicVersion>5</CosmicVersion>
```

Version 3 is what added the two routes below; version 4 added `wait=1` long-polling on
the GET route and the per-certificate owner gate; version 5 added image (PNG) support —
a second `Content-Type`, larger size cap, `Accept`-based negotiation, and the
`X-Cosmic-Clipboard-Version` response header, all described further down. Nothing else
about `/serverinfo` changes.

Both routes live **only on the client-certificate-authenticated HTTPS port**
(`port_base − 5`, default 47984), like `/cosmic/wallpaper` above — there is no plain-HTTP
equivalent. Both GET success statuses (200 and 204) also carry
`X-Cosmic-Clipboard-Seq: <seq>` and `X-Cosmic-Clipboard-Version: 5` — including a
`wait=1` long-poll completion; the 404 carries neither header:

| Route | Condition | Status | Response |
|---|---|---|---|
| `GET /cosmic/clipboard?since=<seq>` | sharing on, stream active, seq > `since`, entry is text, or entry is an image and the request's `Accept` header advertises `image/png` | 200 | clipboard body, `Content-Type: text/plain; charset=utf-8` or `image/png` |
| `GET /cosmic/clipboard?since=<seq>` | sharing on, stream active, seq > `since`, entry is an image but the request did not advertise `image/png` | 204 | no body; `X-Cosmic-Clipboard-Seq` still advances past the image |
| `GET /cosmic/clipboard?since=<seq>` | sharing on, stream active, unchanged | 204 | no body |
| `GET /cosmic/clipboard?since=<seq>` | sharing off, or no stream active | 404 | no header |
| `GET /cosmic/clipboard?since=<seq>&wait=1` | new data arrives before the hold expires (same accept-vs-entry rule as above) | 200 or 204 | as above |
| `GET /cosmic/clipboard?since=<seq>&wait=1` | hold expires with nothing new | 204 | no body |
| `GET /cosmic/clipboard?since=<seq>&wait=1` | sharing switched off, last session ends, or host shutdown mid-hold | 404 | no header |
| `POST /cosmic/clipboard` | sharing on, stream active, classified as text (see below), body ≤ 1 MiB | 200 | — |
| `POST /cosmic/clipboard` | sharing on, stream active, `Content-Type: image/png`, body ≤ 8 MiB | 200 | — |
| `POST /cosmic/clipboard` | sharing off, or no stream active | 404 | — |
| `POST /cosmic/clipboard` | body exceeds the cap for its kind (1 MiB text, 8 MiB image) | 413 | rejected, never truncated |
| `POST /cosmic/clipboard` | `Content-Type` present and not absent/`text/plain`/`application/x-www-form-urlencoded`/`image/png` (an empty value included) | 415 | rejected |

- Sequence numbers are a monotonically increasing `uint64` per host clipboard store,
  starting at 1; the client starts at `since=0`. Republishing identical bytes of the same
  kind does not bump the sequence. The store holds either a text or an image entry at a
  time — never both — and last-write-wins across kinds: copying an image replaces a
  stored text entry and vice versa. The same per-kind cap applies host-side: an oversized
  local clipboard change is simply not published, and the sequence does not advance.
- The POST body's `Content-Type` header (ignoring any `; charset=...` parameter and case)
  decides how it is stored, and this is deliberate rather than incidental: an absent
  header, a literal `text/plain`, or `application/x-www-form-urlencoded` (libcurl's
  default `Content-Type` for `CURLOPT_POSTFIELDS`, and so what every v1–v4 client
  actually sends, since those versions predate this route distinguishing mime types) are
  all treated as text. `image/png` is treated as an image. Anything else — **including an
  empty `Content-Type` value** — is rejected with 415; this route only ever stores text
  or PNG, and a value that names neither is refused rather than guessed at.
- Backward compatibility for GET: the client advertises `Accept: text/plain, image/png`.
  When the stored entry is an image and the requester's `Accept` did not include
  `image/png`, the host still advances the caller past it — answering 204 with the
  entry's `X-Cosmic-Clipboard-Seq` — rather than serving PNG bytes as `text/plain`. This
  is what lets a v1–v4 client coexist with an image on the clipboard: it moves its cursor
  past the image and simply receives nothing for that update, instead of receiving
  corrupted "text".
- `X-Cosmic-Clipboard-Version: 5` is served on every GET 200 and 204 (not on 404, and not
  on `/serverinfo`). This header, not `/serverinfo`'s `<CosmicVersion>`, is what the
  client actually checks to decide whether the connected host understands image/png at
  all — see the client-side behaviour below.
- The counter is process-local and resets to 0 when the host process restarts, so a
  client cursor can end up *ahead* of the store. The GET handler treats a `since` value
  greater than the current sequence as stale and serves the current entry rather than
  answering 204 forever; when the store has never been written, the 204 simply reports
  sequence 0 and the client resets its cursor from the header.
- A single `share_clipboard` setting in `cosmic.json` (default on, exposed in the Bridge
  settings panel as "Share clipboard") governs **both** directions; turning it off makes
  both routes answer 404.

`wait=1` turns the GET route into a long-poll (CosmicVersion 4): when there is nothing
new to report, the host parks the response for up to 20 s instead of answering 204
immediately.

- The hold completes early — with its normal 200 outcome — as soon as new clipboard
  data (text or image) is published.
- The hold also completes early, but with a 404, if clipboard sharing is switched off,
  if the last streaming session on the host ends, or if the HTTPS server shuts down
  while the request is parked.
- A v1/stock host ignores the unrecognised `wait` query parameter and answers
  immediately, so `wait=1` is backward compatible with older hosts.
- The client raises its request timeout to 45 s to cover the 20 s hold plus TLS
  handshake and network margin.

Sync is active **only while a streaming session is running** — outside a session both
routes answer 404. On top of that, both routes are gated to a single owner: the
SHA-256 fingerprint of the TLS client certificate that authenticated the connection
behind the most recent successful `/launch` or `/resume`. A `/resume` refreshes
ownership rather than dropping it, so reconnecting the same client keeps it as owner.
The owner is cleared when the last streaming session on the host ends. Both routes
answer 404, the same as sharing being off, when no owner is currently recorded or when
the requesting connection's certificate does not match the recorded owner — the gate
fails closed. Note that the `uniqueid` query parameter plays no part in this and is not
an auth token: any paired certificate can claim any `uniqueid`, so it cannot stand in
for the certificate that actually authenticated the connection. Separately, an
unpaired client cannot reach these routes at all, because the HTTPS server's
certificate-verification hook rejects it before either handler runs.

This narrows the old hole rather than closing it, stated plainly rather than buried: a
paired client must now actively issue its own `/launch` or `/resume` to take ownership,
instead of merely being paired while someone else streams. But ownership follows
whichever paired certificate most recently launched or resumed — it is not bound to the
live RTSP session — so a second paired client can still call `/resume` and take over as
owner without ever joining the stream. Binding ownership to the actual RTSP session is a
later design item, not something shipped today. The owner check itself only runs when a
request arrives, so a `wait=1` GET already parked when ownership changes still completes
for its original caller — meaning the displaced owner can receive one final clipboard
payload for up to 20 s after a takeover.

On the client, while streaming, the viewer repeatedly GETs with `wait=1`, the last
sequence it saw, and `Accept: text/plain, image/png`, reissuing immediately whenever the
request actually blocked on the hold; against a v1 Cosmic host that answers a 200/204
instantly instead of holding the request, this falls back to a roughly once-a-second
pacing instead. A failed or refused poll (a stock Sunshine host with no route at all, or
a v4 host with sharing off or a non-owner certificate) backs off exponentially from 1 s
to a 30 s ceiling, reset on the next success. The viewer also POSTs when the local
clipboard changes. The client requires the `X-Cosmic-Clipboard-Seq` response header
before trusting a 200 body: an unauthorized or stock Sunshine host answers 200 with a
Sunshine-style error XML body (401 for an unauthorized client, 404 from a stock host),
so the status code alone proves nothing. This is the same defensive lesson as the
wallpaper fetch's magic-byte check above.

The client tracks the connected host's image support as a tri-state latch — Unknown,
Capable, or Incapable — that starts at Unknown for every new session and resolves out of
it on the first trustworthy GET response (well-formed seq header, HTTP 200 or 204),
based on whether that response carried `X-Cosmic-Clipboard-Version` ≥ 5. It never
regresses once resolved. If the user copies an image before the latch resolves, the
image is **held**, not dropped — POSTing takes priority over polling once it goes out,
so the first image of a session can be delayed by up to one 20 s long-poll hold while the
latch is still Unknown. Once the host proves incapable, a held or newly copied image is
dropped silently instead of ever being POSTed, and text continues to sync normally; the
client never sends an `image/png` body to a host it has not confirmed understands one,
since an older host would otherwise store the raw PNG bytes mislabeled as text.

Each poll opens a fresh HTTPS connection rather than reusing one. With `wait=1`, a poll
that finds nothing new is held open for up to the 20 s hold before it resolves, so an
idle stream now costs roughly one connection (and TLS handshake) per hold period rather
than one per second; a stream with frequent clipboard activity still opens a connection
per change, since each hold resolves as soon as new data is published.

Stock Moonlight clients simply never call these routes; against a stock Sunshine host the
routes 404 and the client syncs nothing. Neither case affects pairing or streaming.

Three limitations are worth stating plainly rather than burying:

- **Text wins when the clipboard holds both.** Whether the local clipboard "has text" is
  decided purely by whether SDL returns clipboard text at all; a browser image copy
  typically also places HTML/plain-text alongside the image, so that copy syncs as text,
  not as the image.
- **Paste flattens transparency onto white.** The Windows paste path re-encodes an
  incoming PNG into a 24-bit `BI_RGB` DIB for the clipboard, which has no alpha channel;
  any transparent pixels are composited onto opaque white first.
- **Oversized images are rejected, not decoded.** An image whose *declared* dimensions
  imply more than 64 megapixels of decoded pixels is refused before decoding is
  attempted, both when capturing a local copy and when applying one received from the
  peer — a small compressed file can otherwise decode to a huge allocation (a
  "decompression bomb"), and this bounds that regardless of the file's on-wire size.

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
