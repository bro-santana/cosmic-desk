# Code provenance

Cosmic Desk is assembled from existing open-source projects rather than written from
scratch. **Every piece of third-party code in this repository is listed here**, with the
exact upstream revision it came from and any modifications we made. This file is the
authoritative record; `README.md` links to it.

Upstream projects below are GPL-3.0 or permissively licensed. The repository's
`LICENSE` file is GPL-3.0.

## Status legend

- **Vendored** — the source was copied into this repository and may be modified.
- **Submodule** — tracked as a git submodule and used unmodified.
- **Pattern** — no code copied verbatim; our implementation follows the upstream
  approach closely enough that credit is owed.

## Inventory

| Component | Upstream | Revision | Status | Location here |
|---|---|---|---|---|
| Dear ImGui + SDL2 and SDL3 platform/SDL_Renderer backends | https://github.com/ocornut/imgui (MIT) | tag `v1.91.9`, commit `97428e8ac99e339ce05eee531cf55b77b29ea709` | Vendored, unmodified. Both the SDL2 (`imgui_impl_sdl2`, `imgui_impl_sdlrenderer2`) and SDL3 (`imgui_impl_sdl3`, `imgui_impl_sdlrenderer3`) backends come from this tag; only the SDL3 pair is compiled (see `third-party/imgui/CMakeLists.txt`); the SDL2 pair is retained in the tree but unbuilt | `third-party/imgui/` |
| SDL (window/render/audio/input library) | https://github.com/libsdl-org/SDL (zlib) | tag `release-3.4.12`, commit `f87239e71e42da91ca317a12eefb82cfbf3393eb` | Submodule, unmodified. Built only where no system SDL3 exists (Windows uses the MSYS2 package) — see `CMakeLists.txt` | `third-party/SDL/` |
| tray (single-header tray icon) | https://github.com/zserge/tray (MIT) | commit `8dd1358b92562faf7c032cf5362fa97cbc7e13e9` | Vendored, **modified** | `third-party/tray/tray.h` |
| moonlight-common-c (+ bundled `enet`, `nanors`) | https://github.com/moonlight-stream/moonlight-common-c (GPL-3.0) | see `git submodule status` | Submodule, unmodified. The full library is built for the viewer (`cosmic_moonlight_common`); the host uses its headers and the bundled `enet` | `third-party/moonlight-common-c/` |
| Sunshine host core (`nvhttp`, `rtsp`, `stream`, `video`, `audio`, `input`, `crypto`, `config`, platform capture/injection) | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` (newer Sunshine releases removed Linux input injection (inputtino), which is why this tag is pinned) | Vendored, **modified** | `host/sunshine/` |
| Sunshine uinput udev rule (renamed) | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833` | Adapted | `packaging/linux/60-cosmicdesk-input.rules` |
| Simple-Web-Server (Sunshine's HTTP(S) server dependency) | https://github.com/LizardByte-infrastructure/Simple-Web-Server (MIT) | commit `546895a93a29062bb178367b46c7afb72da9881e` | Vendored, unmodified | `third-party/Simple-Web-Server/` |
| libdisplaydevice (display enumeration) | https://github.com/LizardByte/libdisplaydevice (MIT) | commit `fe7e6a81f65deae91594702e1a185f47229745b9` | Vendored, **modified** | `third-party/libdisplaydevice/` |
| nv-codec-headers (NVENC API headers) | https://github.com/FFmpeg/nv-codec-headers (MIT) | commit `33a9ede8d9914299d9262539c576a15bd0a19621` | Vendored, unmodified | `third-party/nv-codec-headers/` |
| nvapi (NVIDIA NVAPI headers, used by the vendored `nvprefs` code) | https://github.com/NVIDIA/nvapi (MIT) | commit `9b181ea572f680327fe01a14a0f1f41c78034104` | Vendored, unmodified | `third-party/nvapi/` |
| glad (GL/EGL loader generator, Linux VAAPI) | https://github.com/Dav1dde/glad (MIT; Khronos spec headers Apache-2.0) | commit `73db193f853e2ee079bf3ca8a64aa2eaf6459043` | Vendored, unmodified | `third-party/glad/` |
| inputtino (Linux uinput injection) | https://github.com/games-on-whales/inputtino (MIT) | commit `f4ce2b0df536ef309e9ff318f75b460f7097d7c1` | Submodule, unmodified | `third-party/inputtino/` |
| lunasvg (runtime SVG rasterizer; bundled plutovg, no submodule) | https://github.com/sammycage/lunasvg (MIT) | tag `v3.5.0`, commit `83c58df8103dc7dca423dfd824992af94d49bed6` | Vendored, unmodified | `third-party/lunasvg/` |
| stb_image (single-header image decoder; Bridge wallpaper backdrop) | https://github.com/nothings/stb (public domain / MIT dual-licensed) | v2.30, commit `013ac3beddff3dbffafd5177e7972067cd2b5083` | Vendored, unmodified | `third-party/stb/stb_image.h` |
| IBM Plex Mono (Regular/Medium/Bold) | https://github.com/google/fonts `ofl/ibmplexmono` (SIL OFL 1.1) | `main` branch at vendor time (2026-08-28) | Vendored, unmodified | `assets/fonts/IBMPlexMono-*.ttf` |
| IBM Plex Sans (Regular/Medium/SemiBold) | https://github.com/IBM/plex `packages/plex-sans/fonts/complete/ttf` (SIL OFL 1.1) | `master` branch at vendor time (2026-08-28) | Vendored, unmodified | `assets/fonts/IBMPlexSans-*.ttf`, `OFL.txt` |
| Parallax layer artwork (desk, monitor, planets, stars, nebula, objects, reflex, screen-logo) | Cosmic Desk design handoff (`design_handoff_cosmic_desk_launcher/layers/*.svg`; nebula extracted from `Cosmic Desk.dc.html`) | local design files | Vendored, unmodified | `assets/ui/layers/*.svg` |
| Sunshine capture shaders (DXGI HLSL + OpenGL) | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` | Vendored, unmodified | `assets/shaders/` (`directx/` for the Windows DXGI capture, `opengl/` for the Linux GL capture) |
| libgamestream (client pairing: `client.c`, `http.c`, `mkcert.c`, `xml.c`) | https://github.com/moonlight-stream/moonlight-embedded (GPL-3.0) | tag `v2.7.1`, commit `775444287305849ebdf4736c75298ad0713e2d5d` | Vendored, **modified** | `third-party/libgamestream/` |
| FFmpeg decode + SDL render + Opus audio playback structure | https://github.com/moonlight-stream/moonlight-embedded (GPL-3.0) | tag `v2.7.1`, commit `775444287305849ebdf4736c75298ad0713e2d5d` | Pattern | `src/viewer/decoder.cpp`, `src/viewer/vrenderer.cpp`, `src/viewer/audio.cpp` |
| SDL scancode → Windows VK table (`input/keymap.cpp`) | https://github.com/moonlight-stream/moonlight-qt (GPL-3.0) | tag `v6.1.0`, commit `f786e94c7b2f943e24e65d7d74deb539b827fc84` | Pattern/lifted, adapted | `src/viewer/keymap.cpp` (+ `src/viewer/input.cpp` follows the same upstream's input handling pattern) |
| Sunshine service launcher (`tools/sunshinesvc.cpp`), adapted line-by-line: service renamed to `SERVICE_NAME "CosmicDeskService"`; child spawn `cosmicdesk.exe` with a mutable command line `cosmicdesk.exe --hidden --service`; log `%TEMP%\cosmicsvc.log` with inlined `MoveFileExW` rotation replacing `logging::rotate_log_file` (no Boost dependency); the Ctrl-C termination helper removed — the GUI child cannot receive console events and the upstream dance stalled every stop in STOP_PENDING for 20 s, so STOP/PRESHUTDOWN now `TerminateProcess` directly; header/provenance comment | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` | Vendored, **modified** | `tools/cosmicsvc.cpp` |
| Sunshine service-control helpers (`src/entry_handler.cpp`, `namespace service_ctrl`, lines ~123-287), reimplemented: SCM helpers `is_service_running`/`start_service` and `wait_for_ui_ready` with a `GetTcpTable` readiness poll on the app's `port_base`; Boost logging replaced with `std::cout`/`fprintf(stderr)`; service name `CosmicDeskService` | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` | Adapted | `src/app/service_ctrl.{h,cpp}` |
| Sunshine tray quit callback (`src/system_tray.cpp` `tray_quit_cb`, lines ~229-241), adapted: exit code `ERROR_SHUTDOWN_IN_PROGRESS` (1115) handshake so the service stops instead of respawning; explicit `--service` flag replaces upstream's `GetConsoleWindow()==nullptr` heuristic (our GUI build has no console) | https://github.com/LizardByte/Sunshine (GPL-3.0) | tag `v2026.516.143833`, commit `14ffa6fdaa53f7b51512be2b3d24f3939695403c` | Adapted | `src/main.cpp` tray Quit path |

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

- `appdata()` redirected to the CosmicDesk config dir (plan D6/D9) in
  `platform/windows/misc.cpp` and `platform/linux/misc.cpp`: on Windows,
  `%ProgramData%\CosmicDesk` when the process is elevated (the service spawns
  the app as SYSTEM), `%APPDATA%\CosmicDesk` otherwise; on Linux,
  `~/.config/cosmicdesk`.
- Default config file is `host.conf` (plan D6/M1.3) instead of `sunshine.conf`
  (`config.cpp`); `src/hostglue/host.cpp` writes the `port` key from cosmic.json.
- `pin_bridge` hook in `nvhttp.cpp` `pair()`: pending pairing requests are
  surfaced to Cosmic Desk's native PIN dialog (M1.4).
- `nvhttp.cpp` gains `GET`/`POST /cosmic/clipboard` on the HTTPS server for
  bidirectional text clipboard sync, and bumps `CosmicVersion` from 2 to 3.
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
- `boost::process::v1` usage ported to the flat `boost::process` API so the host
  compiles with Boost 1.83 (Ubuntu 24.04) as well as >= 1.86.

### `third-party/libdisplaydevice/`

1. Removed the nested `third-party/doxyconfig` and `third-party/googletest`
   subdirectories (docs/tests only; not needed to build).
2. Removed the top-level `tests/` directory and the stale `.gitmodules` /
   `.readthedocs.yaml` files (they referenced the removed nested submodules/docs).
3. Updated the `LICENSE` and `scripts/pyproject.toml` to reflect the MIT
   relicense (upstream commit `1cf4cbe299002a21133f02d564e74898d4d07b22`); the
   pinned revision `fe7e6a81f65deae91594702e1a185f47229745b9` predates it.

### `third-party/libgamestream/`

1. **Windows UUID shim.** Upstream `client.c` includes `<uuid/uuid.h>` and calls
   `uuid_generate_random`/`uuid_unparse` for the `uniqueid`/`uuid` query
   parameters. MSYS2 UCRT64 has no mingw-w64 libuuid package, so
   `uuid/uuid.h` is a new shim (marked `COSMIC MODIFICATION`) that provides the
   same API on Windows via `UuidCreate` from `<rpc.h>` (linked from `rpcrt4`);
   on Linux it includes the system `<uuid/uuid.h>` unchanged. The shim is only
   on the include path on Windows (see `CMakeLists.txt`).
2. `CMakeLists.txt` is new (upstream builds a shared library against avahi and
   its own moonlight-common-c copy; we build the four-file subset as a static
   library against system packages).
3. `client.c` Windows portability fixes (upstream is Linux-only), each marked
   `COSMIC MODIFICATION` in the source: `<arpa/inet.h>` → `<winsock2.h>`
   (for `htonl`), two-argument `mkdir()` → `_mkdir()` (from `<direct.h>`),
   and BSD `u_int32_t` → standard `uint32_t`.
4. **`http.c` timeouts.** Upstream leaves curl's connect and total timeouts at
   their OS defaults, so an unreachable host hangs the viewer worker thread for
   minutes. Cosmic Desk sets `CURLOPT_CONNECTTIMEOUT` to 5 s and `CURLOPT_TIMEOUT`
   to 30 s so the worker regains control on unresponsive hosts or a pairing that
   parks forever waiting for PIN approval (marked `COSMIC MODIFICATION` in
   `http.c`).
5. **`<CosmicDisplays>` parsing.** `xml.c` gains an expat handler that reads the
   `CosmicVersion`/`CosmicDisplays` block from `/serverinfo` (plan D3a/M5.1) into a
   `COSMIC_DISPLAY` list; `client.c` invokes it during `gs_init` and frees the
   list; the types and the refresh-only `/serverinfo` fetch (plan M5.3) live in
   `client.h`/`xml.h` (all marked `COSMIC MODIFICATION`).

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
