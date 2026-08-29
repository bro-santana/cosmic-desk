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
| −5 | 47984 | TCP/HTTPS | Pairing completion, authenticated API (applist, launch, resume, cancel, wallpaper) |
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
- Version 2 adds `<CosmicWallpaperHash>`, described in the next section.
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
