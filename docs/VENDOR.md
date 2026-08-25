# Code provenance

Cosmic Desk is assembled from existing open-source projects rather than written from
scratch. **Every piece of third-party code in this repository is listed here**, with the
exact upstream revision it came from and any modifications we made. This file is the
authoritative record; `README.md` links to it.

Upstream projects below are GPL-3.0, AGPL-3.0 (libdisplaydevice — combining it with the
GPL-3.0 code makes the combined work subject to AGPL-3.0 terms), or permissively
licensed. The repository's `LICENSE` file is GPL-3.0.

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
| Sunshine host core (`nvhttp`, `rtsp`, `stream`, `video`, `audio`, `input`, `crypto`, `config`, platform capture/injection) | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` (newer Sunshine releases removed Linux input injection (inputtino), which is why this tag is pinned) | Vendored, **modified** | `host/sunshine/` |
| Sunshine uinput udev rule (renamed) | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833` | Adapted | `packaging/linux/60-cosmicdesk-input.rules` |
| Simple-Web-Server (Sunshine's HTTP(S) server dependency) | https://github.com/LizardByte-infrastructure/Simple-Web-Server (MIT) | commit `546895a93a29062bb178367b46c7afb72da9881e` | Vendored, unmodified | `third-party/Simple-Web-Server/` |
| libdisplaydevice (display enumeration) | https://github.com/LizardByte/libdisplaydevice (AGPL-3.0) | commit `fe7e6a81f65deae91594702e1a185f47229745b9` | Vendored, **modified** | `third-party/libdisplaydevice/` |
| nv-codec-headers (NVENC API headers) | https://github.com/FFmpeg/nv-codec-headers (MIT) | commit `33a9ede8d9914299d9262539c576a15bd0a19621` | Vendored, unmodified | `third-party/nv-codec-headers/` |
| nvapi (NVIDIA NVAPI headers, used by the vendored `nvprefs` code) | https://github.com/NVIDIA/nvapi (MIT) | commit `9b181ea572f680327fe01a14a0f1f41c78034104` | Vendored, unmodified | `third-party/nvapi/` |
| glad (GL/EGL loader generator, Linux VAAPI) | https://github.com/Dav1dde/glad (MIT; Khronos spec headers Apache-2.0) | commit `73db193f853e2ee079bf3ca8a64aa2eaf6459043` | Vendored, unmodified | `third-party/glad/` |
| inputtino (Linux uinput injection) | https://github.com/games-on-whales/inputtino (MIT) | commit `f4ce2b0df536ef309e9ff318f75b460f7097d7c1` | Submodule, unmodified | `third-party/inputtino/` |
| Sunshine DXGI HLSL capture shaders | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` | Vendored, unmodified | `assets/shaders/` |

### Planned entries (not yet imported)

These are scheduled by the milestones in `PLAN.md`; each gets a row above with its
pinned revision at the moment it is imported.

| Component | Upstream | Milestone | Planned location |
|---|---|---|---|
| libgamestream (client pairing: `client.c`, `http.c`, `mkcert.c`, `xml.c`) | https://github.com/moonlight-stream/moonlight-embedded (GPL-3.0) | M2 | `third-party/libgamestream/` |
| FFmpeg decode + SDL render + Opus audio playback structure | https://github.com/moonlight-stream/moonlight-embedded (GPL-3.0) | M2 | Pattern → `src/viewer/{decoder,vrenderer,audio}.cpp` |
| SDL scancode → Windows VK translation table | https://github.com/moonlight-stream/moonlight-qt (GPL-3.0) | M2 | `src/viewer/keymap.cpp` |
| Keyboard grab / fullscreen / escape-combo handling | https://github.com/moonlight-stream/moonlight-qt (GPL-3.0) | M4 | Pattern → `src/viewer/input.cpp` |

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

### `host/sunshine/` (Sunshine host core)

Files deleted from the upstream `src/` tree (plan D5 scope cuts):

- `main.cpp`, `main.h` — upstream entry point; Cosmic Desk has its own `main`.
- `entry_handler.cpp`, `entry_handler.h` — upstream service entry point.
- `confighttp.cpp`, `confighttp.h` — web UI dropped (plan D5).
- `upnp.cpp`, `upnp.h` — UPnP dropped (plan D5).
- `system_tray.cpp`, `system_tray.h` — Cosmic Desk has its own tray from M0.
- `cbs.cpp`, `cbs.h` — needs Sunshine's patched FFmpeg libcbs; dropped (plan R3).
- `platform/macos/` — macOS out of scope.
- `platform/linux/cuda.cpp`, `cuda.cu`, `cuda.h` — CUDA encoder dropped (plan R3).
- `platform/linux/kmsgrab.cpp` — KMS capture dropped (Linux capture v1 is X11-only).
- `platform/linux/kwingrab.cpp` — KWin capture dropped (X11-only v1).
- `platform/linux/pipewire.cpp` — PipeWire capture dropped (X11-only v1).
- `platform/linux/portalgrab.cpp` — portal capture dropped (X11-only v1).
- `platform/linux/vulkan_encode.cpp`, `vulkan_encode.h` — Vulkan encoder dropped (plan R3).
- `platform/linux/wayland.cpp`, `wayland.h` — Wayland dropped (X11-only v1).
- `platform/linux/wlgrab.cpp` — Wayland grab dropped (X11-only v1).
- `platform/linux/input/inputtino_gamepad.cpp`, `inputtino_gamepad.h` — gamepad dropped (plan D5).

Modifications made so far (each marked in the source with a `COSMIC MODIFICATION`
comment):

- `appdata()` redirected to the CosmicDesk config dir (`%APPDATA%\CosmicDesk` /
  `~/.config/cosmicdesk`, plan D6) in `platform/windows/misc.cpp` and
  `platform/linux/misc.cpp`.
- Default config file is `host.conf` (plan D6/M1.3) instead of `sunshine.conf`
  (`config.cpp`); `src/hostglue/host.cpp` writes the `port` key from cosmic.json.
- `pin_bridge` hook in `nvhttp.cpp` `pair()`: pending pairing requests are
  surfaced to Cosmic Desk's native PIN dialog (M1.4).
- Deleted encoder/capture/gamepad paths with the existing `COSMIC MODIFICATION`
  markers: `cbs` (SPS/VPS injection), AMF, QSV, MediaFoundation, WGC capture
  backend, and ViGEm/inputtino gamepad emulation.
- `system_tray` calls stubbed out (removed) — Cosmic Desk owns its tray (M0).
- `process.cpp` `parse()` replaced with a hardcoded single "Desktop" app stub
  (plan M1.3); `apps.json` is never read.
- `rswrapper.c` rewritten as a thin bridge to the nanors copy bundled in the
  moonlight-common-c submodule (plan R3).
- `entry_handler_shim.h` added: no-op `launch_ui()`, `lifetime::*` and stubbed
  `service_ctrl::*` replacements for the deleted upstream `entry_handler.h`.

### `third-party/libdisplaydevice/`

1. Removed the nested `third-party/doxyconfig` and `third-party/googletest`
   subdirectories (docs/tests only; not needed to build).
2. Removed the top-level `tests/` directory and the stale `.gitmodules` /
   `.readthedocs.yaml` files (they referenced the removed nested submodules/docs).

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

Sunshine itself can be diffed the same way:

```bash
git clone --branch v2026.516.143833 --depth 1 https://github.com/LizardByte/Sunshine.git /tmp/sunshine-upstream
diff -ru /tmp/sunshine-upstream/src host/sunshine/src
```
