# Code provenance

Cosmic Desk is assembled from existing open-source projects rather than written from
scratch. **Every piece of third-party code in this repository is listed here**, with the
exact upstream revision it came from and any modifications we made. This file is the
authoritative record; `README.md` links to it.

All upstream projects listed below are GPL-3.0 or a GPL-3.0-compatible permissive
licence, and Cosmic Desk as a whole is distributed under **GPL-3.0** (`LICENSE`).

## Status legend

- **Vendored** — the source was copied into this repository and may be modified.
- **Submodule** — tracked as a git submodule and used unmodified.
- **Pattern** — no code copied verbatim; our implementation follows the upstream
  approach closely enough that credit is owed.

## Inventory

| Component | Upstream | Revision | Status | Location here |
|---|---|---|---|---|
| Dear ImGui + SDL2/SDL_Renderer backends | https://github.com/ocornut/imgui (MIT) | tag `v1.91.9`, commit `97428e8ac99e339ce05eee531cf55b77b29ea709` | Vendored, unmodified | `third-party/imgui/` |
| tray (single-header tray icon) | https://github.com/zserge/tray (MIT) | commit `8dd1358b92562faf7c032cf5362fa97cbc7e13e9` | Vendored, **modified** | `third-party/tray/tray.h` |
| moonlight-common-c (+ bundled `enet`, `nanors`) | https://github.com/moonlight-stream/moonlight-common-c (GPL-3.0) | see `git submodule status` | Submodule, unmodified | `third-party/moonlight-common-c/` |

### Planned entries (not yet imported)

These are scheduled by the milestones in `PLAN.md`; each gets a row above with its
pinned revision at the moment it is imported.

| Component | Upstream | Milestone | Planned location |
|---|---|---|---|
| Sunshine host core (`nvhttp`, `rtsp`, `stream`, `video`, `audio`, `input`, `crypto`, `config`, platform capture/injection) | https://github.com/LizardByte/Sunshine (GPL-3.0) | M1 | `host/sunshine/` |
| Simple-Web-Server (Sunshine's HTTP(S) server dependency) | https://gitlab.com/eidheim/Simple-Web-Server (MIT) | M1 | `third-party/Simple-Web-Server/` |
| Sunshine uinput udev rule | https://github.com/LizardByte/Sunshine (GPL-3.0) | M1 | `packaging/linux/60-cosmicdesk-input.rules` |
| libgamestream (client pairing: `client.c`, `http.c`, `mkcert.c`, `xml.c`) | https://github.com/moonlight-stream/moonlight-embedded (GPL-3.0) | M2 | `third-party/libgamestream/` |
| FFmpeg decode + SDL render + Opus audio playback structure | https://github.com/moonlight-stream/moonlight-embedded (GPL-3.0) | M2 | Pattern → `src/viewer/{decoder,vrenderer,audio}.cpp` |
| SDL scancode → Windows VK translation table | https://github.com/moonlight-stream/moonlight-qt (GPL-3.0) | M2 | `src/viewer/keymap.cpp` |
| Keyboard grab / fullscreen / escape-combo handling | https://github.com/moonlight-stream/moonlight-qt (GPL-3.0) | M4 | Pattern → `src/viewer/input.cpp` |
| inputtino (Linux uinput injection) | https://github.com/games-on-whales/inputtino (MIT) | M1 | `third-party/inputtino/` |

## Modifications to vendored code

Each modification is marked in the source with a `COSMIC MODIFICATION` comment so it can
be found without diffing.

### `third-party/tray/tray.h`

1. **Win32 `tray_loop` non-blocking fix.** Upstream calls `TranslateMessage`/
   `DispatchMessage` on an uninitialized `MSG` when `PeekMessage` finds nothing pending.
   Cosmic Desk pumps the tray once per frame in non-blocking mode, so this fires
   constantly. The loop now returns early unless a message was actually retrieved, and
   treats `GetMessage` failure as "stop".
2. **Ayatana AppIndicator header.** Upstream includes
   `<libappindicator/app-indicator.h>`, which current distributions no longer ship;
   Ubuntu 24.04 provides the Ayatana fork at `<libayatana-appindicator/app-indicator.h>`.
   Defining `TRAY_AYATANA_APPINDICATOR` (the build does this on Linux) selects the
   Ayatana header; the original path remains as the fallback.

## Diffing a vendored component against upstream

Vendored code is a hard fork of a pinned snapshot; we do not track upstream
continuously. To see what we changed:

```bash
git clone https://github.com/zserge/tray /tmp/tray-upstream
git -C /tmp/tray-upstream checkout 8dd1358b92562faf7c032cf5362fa97cbc7e13e9
diff -u /tmp/tray-upstream/tray.h third-party/tray/tray.h
```

The same procedure applies to every vendored component: clone upstream, check out the
revision from the table above, and `diff -ru` against our copy.
