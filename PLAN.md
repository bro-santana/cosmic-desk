# Cosmic Desk — Minimal Moonlight-Protocol Remote Desktop — Implementation Plan

## 1. Context

Cosmic Desk is a minimal, GPL-3.0, AnyDesk/RustDesk-style remote desktop tool for
Windows and Linux built from proven Moonlight-protocol components. **One unified app**
acts as both **host** (screen sharer, always-on tray presence) and **viewer** (connect
to a host by IP). No ID broker or relay: direct IP connections; pairing uses the
standard Moonlight PIN flow surfaced through a native dialog; after one pairing,
subsequent connections are certificate-authenticated with no interaction (standard
Moonlight semantics).

We do not write a streaming stack; we assemble one from GPL-3.0 upstreams:

- **Sunshine** (LizardByte/Sunshine) — the entire host: GameStream HTTP/HTTPS server,
  pairing state machine, RTSP, ENet control, capture, encode, audio, input injection.
- **moonlight-common-c** — the entire viewer protocol core: RTSP client, control
  stream, RTP depacketization + FEC, input event sending (`src/Limelight.h`:
  `LiStartConnection`, `LiSend*`).
- **moonlight-embedded** (`libgamestream/`) — viewer-side pairing/HTTPS client
  (`gs_pair`, cert generation) plus the FFmpeg-decode + SDL-render + Opus patterns.
- **moonlight-qt** — patterns only: keyboard grab (Alt+Tab capture), SDL scancode →
  Windows VK table, fullscreen handling.

UI is SDL2 + Dear ImGui everywhere. Tray via the tiny `tray` C library Sunshine
vendors. No Qt, no web UI, no gamepad, no UPnP.

**Copied code must be documented explicitly in the README.** `docs/VENDOR.md` holds
the provenance table (upstream, tag/SHA, files, modifications); README links it.
Project license: GPL-3.0.

Audience: an inexperienced coder. Each milestone has file-level tasks, exact commands,
and "run X, see Y" acceptance criteria. The interop sequencing (our host vs. stock
Moonlight first; our viewer vs. stock Sunshine second) guarantees every bug has one
known-good side.

## 2. Requirements checklist

- [ ] Connect to a host by IP from the app (M2/M3)
- [ ] Host accepts pairing via native PIN dialog, Moonlight PIN flow (M1); pair once →
      later connections free
- [ ] Autostart with the PC + tray icon (M0 tray, M6 autostart)
- [ ] Viewer: fullscreen/windowed toggle (M4)
- [ ] Viewer top bar: monitor selector with seamless mid-stream switch + exit (M4/M5)
- [ ] Capture all input incl. Alt+Tab when focused (M4)
- [ ] Resolution + bitrate settings; default = host desktop resolution (M4/M5)
- [ ] Windows + Linux, both roles (all milestones; Linux capture X11-first)
- [ ] README documents all copied code (M6)

## 3. Architecture

### 3.1 One process, three roles

Single process. SDL requires its event loop on the main thread; host streaming and
viewer network/decode already live on worker threads in the upstream code.

```
                Cosmic Desk (one process per logged-in user, autostarted)
 ┌────────────────────────────────────────────────────────────────────────────────┐
 │ MAIN THREAD (SDL event loop + ImGui, 60 Hz)                                    │
 │   • App state machine: MAIN_WINDOW | VIEWING | HIDDEN_TO_TRAY                  │
 │   • Main UI: "Connect to <IP>", settings, pairing status                       │
 │   • PIN dialog (ImGui modal; window forced visible + raised on pair request)   │
 │   • Viewer mode: SDL_Renderer draws YUV video texture + ImGui top bar overlay  │
 │   • tray_loop(0) pumped each frame (tray lib is non-blocking)                  │
 ├────────────────────────────────────────────────────────────────────────────────┤
 │ HOST THREADS (vendored Sunshine, started at app launch, run forever)           │
 │   nvhttp thread ── HTTP :47989  (serverinfo, pair phases 1–4)                  │
 │               └── HTTPS :47984 (serverinfo, pairchallenge, applist, launch,    │
 │                                 resume, cancel)  [+ Cosmic serverinfo fields]  │
 │   rtsp thread  ── TCP :48010                                                   │
 │   per-session: capture thread → encode thread → video UDP :47998               │
 │                audio capture → Opus encode → audio UDP :48000                  │
 │                control/input ── ENet UDP :47999 (input injection happens here) │
 ├────────────────────────────────────────────────────────────────────────────────┤
 │ VIEWER THREADS (only while a session is open)                                  │
 │   moonlight-common-c internal threads (RTSP, control ENet, RTP receive, FEC)   │
 │   decode thread: DU queue → avcodec → latest-frame slot → main thread uploads  │
 │   audio: decodeAndPlaySample cb → opus_multistream_decode → SDL_QueueAudio     │
 └────────────────────────────────────────────────────────────────────────────────┘

  Viewer machine ──/pair, /serverinfo──▶ Host :47989/:47984 (HTTPS after pairing)
                 ──RTSP setup─────────▶ Host :48010
                 ◀─video RTP──────────  Host :47998        ◀─audio RTP── :48000
                 ◀─control/input ENet─▶ Host :47999 (encrypted input both ways)
```

**Windows "service" reality (documented in README):** a real Windows Service runs in
session 0 and cannot capture the interactive desktop or inject input. The realistic
service is an autostart-at-login tray app (`HKCU\...\Run`). Consequences: host is
unreachable before login; UAC secure-desktop prompts are invisible. Sunshine solves
this with an elevated service+helper architecture — out of scope for v1; README
documents it and offers a Task Scheduler "run elevated" workaround note.

### 3.2 Ports (all derived from one `port_base` setting, default 47989)

| Port  | Proto     | Purpose                            |
|-------|-----------|------------------------------------|
| 47984 | TCP/HTTPS | Pairing completion, authed API     |
| 47989 | TCP/HTTP  | serverinfo, pair phases 1–4        |
| 47998 | UDP       | Video RTP                          |
| 47999 | UDP       | Control/input (ENet, encrypted)    |
| 48000 | UDP       | Audio RTP                          |
| 48010 | TCP       | RTSP                               |

Web UI port 47990 is gone (confighttp deleted). If a test machine has stock Sunshine
installed, change Cosmic's `port_base` (setting in the UI) — top setup footgun, called
out in M1 acceptance.

## 4. Key decisions (with rationale)

### D1. Sunshine reuse strategy: vendor a stripped copy, modify in place
Vendor Sunshine's `src/` (stripped) into `host/sunshine/` at a pinned stable release
tag; edit directly; record provenance in `docs/VENDOR.md` (URL + tag/SHA + deletions/
modifications). We're making invasive edits (delete confighttp/web UI, hook
`nvhttp::pin` to a dialog, extend `/serverinfo`, strip gamepad/UPnP) — submodule+patch
stacks break constantly for a novice. Diff-against-upstream stays cheap: clone upstream
at the pinned tag and `diff -ru` (documented in VENDOR.md). `moonlight-common-c` stays
a proper git submodule because it's used unmodified.

### D2. Process model: single process, single SDL window with modes
One SDL window: hidden (tray), main/settings UI, or viewer (video + top bar). PIN
dialog is an ImGui modal (pair-request callback un-hides + raises window; also fires a
tray notification). Viewer crash taking down the host role is accepted for v1 (post-v1
option: spawn `cosmicdesk --view <ip>` child). While VIEWING, frame hand-off is a
mutex-protected single-slot "latest frame" exchange so the main loop never blocks.

### D3. Protocol extensions (minimal, all inside code we own)

**(a) Host desktop resolution + monitor list before streaming — extend `/serverinfo`
XML** (stock Moonlight clients ignore unknown nodes, so M1 interop is unaffected):

```xml
<CosmicVersion>1</CosmicVersion>
<CosmicDisplays>
  <Display index="0" name="\\.\DISPLAY1" width="2560" height="1440" fps="165" primary="1" active="1"/>
  <Display index="1" name="\\.\DISPLAY2" width="1920" height="1080" fps="60" primary="0" active="0"/>
</CosmicDisplays>
```

Populated from the same `platf::display_names()` call Sunshine's hotkey path uses, in
the same order. `active` = display currently captured. The viewer already fetches
`/serverinfo` (libgamestream `gs_init`); we just parse extra fields.

**(b) Mid-stream monitor switch — synthesize Ctrl+Alt+Shift+F(1+i) over the existing
input channel.** Sunshine's `input.cpp::apply_shortcut()` already turns that hotkey
into the internal `mail::switch_display` event; the capture thread reinits capture
**without tearing down the session** — the exact seamless behavior required, with zero
new protocol. Viewer sends VK_CONTROL, VK_MENU, VK_SHIFT, VK_F1+i downs then ups via
`LiSendKeyboardEvent`. Caveats handled in M5: ensure the shortcut feature is enabled in
our config defaults; verify no stray modifier state leaks (mitigation: send explicit
key-ups for all modifiers after the switch).

**(c) Index contract:** both the serverinfo extension and `apply_shortcut` index into
the same `platf::display_names()` ordering, so `index` i ⇔ F(1+i) by construction —
written as a comment at both call sites. Viewer re-fetches `/serverinfo` each time the
monitor dropdown opens (handles hotplug). Limit 13 monitors (F1–F13) — fine.

### D4. FFmpeg: system FFmpeg for both roles
Sunshine normally uses its own patched prebuilt FFmpeg. We link the vendored host
against MSYS2/apt FFmpeg (avcodec/avutil/swscale/avfilter) — one FFmpeg for viewer
decode and host encode, far simpler CMake. If an encoder path fights vanilla FFmpeg
headers, delete that encoder (keep NVENC + x264 on Windows, VAAPI + x264 on Linux;
x264 software is the guaranteed floor). Most likely place M1 needs code surgery —
budgeted there. Escape hatch: restore LizardByte's prebuilt FFmpeg for the host only.

### D5. Scope cuts (locked)
- **Gamepad: dropped** (no ViGEm, no libvirtualhid → avoids its paid Windows driver
  licensing entirely). Keyboard/mouse only.
- **UPnP: dropped** (delete `upnp.cpp`; WAN users forward the 6 ports manually —
  README table).
- **Web UI: dropped** (delete `confighttp.cpp`, `src_assets/` web, npm/Vue tooling).
- **Audio: IN for v1** — host side free via Sunshine `audio.cpp`; viewer ~150 lines
  (libopus multistream + `SDL_QueueAudio`). Cuttable: if it stalls > 2 days, ship
  silent, revisit.
- **Linux input injection: inputtino → uinput** (as Sunshine stable), udev rule
  shipped as `packaging/linux/60-cosmicdesk-input.rules`.
- **Linux capture v1: X11 only** (`x11grab.cpp`). Wayland (portal/PipeWire) and KMS
  are post-v1; on Wayland the host shows a clear "log in with Xorg session" error.
- **No clipboard/file transfer.**

### D6. Config & state locations
- Windows: `%APPDATA%\CosmicDesk\`; Linux: `~/.config/cosmicdesk/` (respect
  `$XDG_CONFIG_HOME`). Contents: `cosmic.json` (app/viewer settings: recent IPs,
  bitrate, resolution mode, port_base, autostart flag — via nlohmann-json),
  `host.conf` (generated key=value consumed by Sunshine's `config.cpp`),
  `credentials/` (host TLS cert+key), `client/` (viewer cert+key from `mkcert.c`,
  paired server certs), host-side paired-clients state (Sunshine's existing format,
  relocated).

## 5. Repository layout

```
cosmic-desk/
├── CMakeLists.txt                 # superbuild; links everything into one `cosmicdesk` binary
├── cmake/                         # FindFFmpeg helpers, flags, install rules
├── src/
│   ├── main.cpp                   # SDL init, tray init, host start, main loop, state machine
│   ├── app/
│   │   ├── settings.{h,cpp}       # cosmic.json load/save, per-OS config paths
│   │   ├── state.{h,cpp}          # AppMode enum + transitions
│   │   └── autostart.{h,cpp}      # HKCU Run key (Win) / XDG autostart .desktop (Linux)
│   ├── ui/
│   │   ├── main_window.cpp        # connect-by-IP screen, recent hosts list
│   │   ├── settings_window.cpp    # resolution/bitrate/port/autostart
│   │   ├── pin_dialog.cpp         # ImGui modal; bridges to nvhttp::pin()
│   │   ├── viewer_topbar.cpp      # monitor dropdown, fullscreen toggle, exit
│   │   └── tray.cpp               # tray menu (Show, Pause hosting, Quit)
│   ├── hostglue/
│   │   ├── host.{h,cpp}           # start/stop Sunshine subsystems; owns its threads
│   │   ├── pin_bridge.{h,cpp}     # pair request → UI queue → nvhttp::pin()
│   │   └── displays.{h,cpp}       # display_names() wrapper (serverinfo ext + ordering contract)
│   └── viewer/
│       ├── session.{h,cpp}        # gs_init/gs_pair/gs_start_app + LiStartConnection lifecycle
│       ├── decoder.{h,cpp}        # avcodec decode thread (pattern: moonlight-embedded ffmpeg.c)
│       ├── vrenderer.{h,cpp}      # SDL IYUV texture upload/present (pattern: video/sdl.c)
│       ├── audio.{h,cpp}          # opus multistream + SDL_QueueAudio (pattern: audio/sdl.c)
│       ├── input.{h,cpp}          # SDL events → LiSend*; grab; escape combos; monitor-switch synth
│       └── keymap.{h,cpp}         # SDL scancode → Windows VK (lifted from moonlight-qt keyboard.cpp)
├── host/
│   └── sunshine/                  # VENDORED stripped Sunshine src/ (upstream file names kept)
│       └── src/ + src/platform/{windows,linux}/
├── third-party/
│   ├── moonlight-common-c/        # git submodule (recursive: bundled patched enet/, nanors)
│   ├── imgui/                     # vendored core + imgui_impl_sdl2 + imgui_impl_sdlrenderer2
│   ├── Simple-Web-Server/         # vendored header-only (nvhttp dependency)
│   ├── tray/                      # vendored (Sunshine's fork)
│   ├── libgamestream/             # vendored from moonlight-embedded: client.c http.c mkcert.c xml.c
│   └── inputtino/                 # git submodule (Linux only)
├── packaging/
│   ├── windows/                   # make-zip.ps1 (ntldd DLL bundling); Inno Setup optional
│   └── linux/                     # .desktop files, systemd user unit, 60-cosmicdesk-input.rules
├── docs/
│   ├── BUILDING.md                # exact commands from §6, kept current
│   ├── VENDOR.md                  # provenance table — README links here
│   └── PROTOCOL.md                # serverinfo extension schema, monitor-switch contract, ports
├── PLAN.md                        # this document
└── README.md                      # incl. mandatory "Code provenance & licensing" section (GPL-3.0)
```

CMake: top level finds shared deps (SDL2, OpenSSL, CURL, expat, Opus, FFmpeg via
pkg-config, Boost), builds third-party as static libs (moonlight-common-c has its own
CMake; imgui/tray/libgamestream get tiny hand-written CMakeLists), builds
`host/sunshine` as static lib `cosmic_host`, links one executable `cosmicdesk`.

## 6. Dependencies & exact setup commands

### Windows (MSYS2 UCRT64 — Sunshine's hard constraint; MSVC is NOT supported)

```
# 1. Install MSYS2 from https://www.msys2.org (default C:\msys64)
# 2. Open the "MSYS2 UCRT64" shell (not MSYS, not MINGW64):
pacman -Syu            # restart shell if asked, run again
pacman -S --needed git \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-SDL2 \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-expat \
  mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-boost \
  mingw-w64-ucrt-x86_64-MinHook \
  mingw-w64-ucrt-x86_64-nlohmann-json

git clone <repo-url> cosmic-desk && cd cosmic-desk
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/cosmicdesk.exe
```

Note: the exact Boost component set and any extra Sunshine deps are confirmed at
vendoring time from the pinned tag's `docs/building.md` — VENDOR.md records the final
package list. MinHook (`libMinHook.a`, `find_library(... REQUIRED)` in
`host/sunshine/CMakeLists.txt`) is a confirmed extra Sunshine dependency on Windows.

### Linux (Ubuntu 24.04)

```
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build git pkg-config \
  libsdl2-dev \
  libavcodec-dev libavutil-dev libswscale-dev libavfilter-dev \
  libssl-dev libcurl4-openssl-dev libexpat1-dev libopus-dev \
  uuid-dev \
  libboost-dev libboost-log-dev libboost-locale-dev libboost-program-options-dev \
  nlohmann-json3-dev \
  libx11-dev libxfixes-dev libxrandr-dev libxtst-dev libxcb1-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libdrm-dev libcap-dev libevdev-dev libudev-dev \
  libpulse-dev libva-dev \
  libayatana-appindicator3-dev libgtk-3-dev libnotify-dev \
  python3-pip python3-setuptools

# glad generator needs Python + jinja2 at build time (host/sunshine CMake checks it)
pip3 install jinja2 setuptools

git clone <repo-url> cosmic-desk && cd cosmic-desk
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && ninja -C build

# input injection (one-time):
sudo cp packaging/linux/60-cosmicdesk-input.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### Dependency disposition

| Dependency | Role | Windows (MSYS2 ucrt64) | Ubuntu 24.04 | Obtained |
|---|---|---|---|---|
| SDL2 | window/render/audio/input | `…-SDL2` | `libsdl2-dev` | system |
| Dear ImGui (+2 backends) | all UI | — | — | vendored |
| FFmpeg | host encode + viewer decode | `…-ffmpeg` | `libav*-dev` | system (D4) |
| OpenSSL | pairing crypto, TLS | `…-openssl` | `libssl-dev` | system |
| libcurl | libgamestream HTTPS | `…-curl` | `libcurl4-openssl-dev` | system |
| expat | libgamestream XML | `…-expat` | `libexpat1-dev` | system |
| libuuid | libgamestream `uniqueid`/`uuid` params | none (vendored shim, `rpcrt4`) | `uuid-dev` | shim on Windows, system on Linux |
| libopus | audio enc/dec | `…-opus` | `libopus-dev` | system |
| Boost | Sunshine internals | `…-boost` | `libboost-*-dev` | system |
| nlohmann-json | cosmic.json | `…-nlohmann-json` | `nlohmann-json3-dev` | system |
| moonlight-common-c | viewer protocol | — | — | submodule (bundled enet mandatory) |
| Simple-Web-Server | nvhttp server | — | — | vendored header-only |
| tray | tray icon | — | — | vendored |
| libgamestream (4 files) | viewer pairing | — | — | vendored |
| inputtino | Linux uinput | n/a | — | submodule, Linux only |
| miniupnpc / ViGEm / npm | — | **dropped** | **dropped** | — |

## 7. Code-lift map (every row becomes a row in docs/VENDOR.md + README)

| Source (upstream) | Destination | Notes |
|---|---|---|
| Sunshine `src/nvhttp.cpp,h` | `host/sunshine/src/` | Pairing FSM + HTTP/HTTPS. Modify: `<CosmicDisplays>` serverinfo ext; keep `nvhttp::pin()` for pin_bridge |
| Sunshine `src/rtsp.cpp`, `stream.cpp`, `video.cpp`, `audio.cpp`, `input.cpp`, `crypto.cpp`, `config.cpp`, `network.cpp`, `process.cpp`, `globals.*`, `logging.*`, utility headers | `host/sunshine/src/` | Streaming core. Modify: trim encoders fighting vanilla FFmpeg; delete gamepad branches; hardcode single "Desktop" app |
| Sunshine `src/platform/windows/*` | `host/sunshine/src/platform/windows/` | DXGI capture, WASAPI, SendInput. Strip ViGEm/gamepad |
| Sunshine `src/platform/linux/` (x11grab, graphics, pulse audio, inputtino glue, misc) | `host/sunshine/src/platform/linux/` | X11-only v1: omit kmsgrab/wlgrab/portalgrab; strip gamepad |
| Sunshine `confighttp.*`, web assets, `upnp.cpp` | **not copied** | dropped |
| Sunshine udev rule (`60-sunshine.rules`) | `packaging/linux/60-cosmicdesk-input.rules` | renamed |
| Sunshine `third-party/Simple-Web-Server`, `third-party/tray` | `third-party/` | vendored as-is |
| moonlight-common-c (whole repo) | submodule | unmodified; Limelight.h API |
| moonlight-embedded `libgamestream/{client.c,http.c,mkcert.c,xml.c,*.h}` | `third-party/libgamestream/` | Modify: parse `<CosmicDisplays>` |
| moonlight-embedded `src/video/ffmpeg.c`, `src/video/sdl.c`, `src/audio/sdl.c` | *patterns* → `src/viewer/{decoder,vrenderer,audio}.cpp` | rewrite portably, don't port (repo is Linux-only) |
| moonlight-qt `keyboard.cpp` VK table | `src/viewer/keymap.cpp` | lift wholesale + escape-combo pattern |
| moonlight-qt `session.cpp`/`input/input.cpp` | *pattern* → `src/viewer/input.cpp` | SDL_SetWindowKeyboardGrab, NO_CLOSE_ON_ALT_F4 hint, relative mouse, FULLSCREEN_DESKTOP |
| Dear ImGui core + sdl2/sdlrenderer2 backends | `third-party/imgui/` | vendored |

## 8. Milestones (one inexperienced dev, part-guided; total ≈ 8–11 weeks)

### M0 — Scaffold, toolchains, hello-window + tray (3–5 days)
**Goal:** both OS toolchains proven; SDL+ImGui window and tray icon from our CMake.
1. `git init`; create §5 layout; save this doc as `PLAN.md`; `.gitignore` for `build/`.
2. Run §6 dependency commands on Windows (MSYS2 UCRT64) and Ubuntu 24.04.
3. Vendor ImGui + backends and `tray`; add `moonlight-common-c` submodule
   (`--recursive`) and build it now as a smoke test.
4. `src/main.cpp`: SDL window + renderer + ImGui demo; pump `tray_loop(0)`; tray menu
   Show/Quit; window close → hide to tray.
5. `src/app/settings.cpp`: `cosmic.json` load/save in the D6 path.
6. Add a GitHub Actions build-only job (MSYS2 + Ubuntu) — compile breakage caught
   per-commit.

**Accept:** `ninja -C build && ./build/cosmicdesk` on both OSes shows the ImGui demo;
close → tray icon remains; tray Show restores; tray Quit exits; `cosmic.json` appears.

### M1 — Our host streams to a STOCK Moonlight client (2.5–3 weeks; the hard one)
**Goal:** vendored stripped Sunshine inside our process; pair from stock moonlight-qt
using our native PIN dialog; stream desktop video+audio+input.
1. Clone Sunshine at a pinned stable tag to a scratch dir; copy the §7 file set into
   `host/sunshine/`; start `docs/VENDOR.md` immediately (tag + SHA + file list).
2. `host/sunshine/CMakeLists.txt` as static lib vs. system FFmpeg/Boost/OpenSSL/Opus.
   Iterate on compile errors; encoders that fight vanilla FFmpeg get deleted (keep
   NVENC + x264 on Win, VAAPI + x264 on Linux; QSV/AMF/MF/Vulkan/CUDA off).
3. Delete confighttp/web/UPnP/gamepad paths; stub `process.cpp` to a single "Desktop"
   app (empty command = desktop stream, Sunshine convention).
4. `src/hostglue/host.cpp`: init logging/config/crypto (certs into `credentials/` on
   first run); start nvhttp + rtsp threads; clean stop on quit.
5. `pin_bridge.cpp` + `pin_dialog.cpp`: incoming `/pair` surfaces client name in a
   queue → main loop raises window → ImGui modal, 4-digit input → `nvhttp::pin()`.
6. `host.conf` generated from `cosmic.json` (port_base, output_name); ensure the
   Ctrl+Alt+Shift+Fn shortcut feature is ON in defaults (needed by M5).

**Accept:** stock moonlight-qt on a second machine: Add PC by IP → pair (PIN typed
into our native dialog) → "Desktop" streams with video, audio, mouse+keyboard.
`https://host:47990` refuses (web UI gone). Changing `port_base` avoids clash with an
installed stock Sunshine. Repeat pair+stream with a Linux (X11) host.

### M2 — Our viewer streams from a STOCK Sunshine host (1.5–2 weeks)
**Goal:** viewer pipeline validated against a known-good host.
1. Vendor `libgamestream/` with a small CMakeLists (curl/openssl/expat).
2. `src/viewer/session.cpp`: IP → `gs_init` (client cert via mkcert on first run) →
   if unpaired: generate 4-digit PIN, show "Enter this PIN on the host" screen,
   `gs_pair` → `gs_applist` → pick "Desktop" → `gs_start_app` → fill
   `SERVER_INFORMATION` + `STREAM_CONFIGURATION` (hardcode 1920x1080@60, 20 Mbps,
   H.264 first) → `LiStartConnection`.
3. `decoder.cpp`: `submitDecodeUnit` → decode thread (avcodec_send_packet/
   receive_frame, padded buffers) → latest-frame slot.
4. `vrenderer.cpp`: `SDL_UpdateYUVTexture` (IYUV) + present in main loop.
5. `audio.cpp`: opus multistream decode → `SDL_QueueAudio`.
6. `input.cpp` + `keymap.cpp`: SDL events → `LiSendMousePositionEvent` (absolute,
   ref = stream res), buttons, scroll, `LiSendKeyboardEvent` (VK table).
   Ctrl+Alt+Shift+Q ends the session.
7. Clean teardown: `LiStopConnection` + `gs_quit_app`; return to main window.

**Accept:** vs. a stock Sunshine machine: enter IP → PIN screen → type PIN into
Sunshine's web UI → stream with video+audio; typing/clicking controls the host;
reconnect later needs no PIN; Ctrl+Alt+Shift+Q exits cleanly, reconnect without
restart works.

### M3 — Unification: our viewer ↔ our host (1 week)
1. Both halves in one binary: resolve symbol clashes (OpenSSL init, logging, ENet —
   preferred: point the vendored Sunshine at the submodule's enet, since it's the same
   shared fork).
2. Our viewer's PIN screen ↔ our host's PIN dialog end-to-end.
3. Main window: recent hosts from `cosmic.json`; status line ("Hosting on :47989 —
   1 client paired").

**Accept:** two machines both running Cosmic Desk: A views B and B views A
simultaneously; pairing entirely via native dialogs; no stock apps involved.

### M4 — Viewer UX: fullscreen, top bar, full input grab, settings (1 week)
1. `viewer_topbar.cpp`: ImGui strip at top, auto-hide after 2 s, reappear on
   mouse-to-top: monitor dropdown (placeholder until M5), fullscreen toggle, exit.
2. Fullscreen: `SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP)` toggle;
   also Ctrl+Alt+Shift+Enter.
3. Grab: on focus → `SDL_SetWindowKeyboardGrab(win, SDL_TRUE)` (captures Alt+Tab/Win
   key), `SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4 "1"`; on focus loss → release grab +
   flush key-ups (moonlight-qt pattern). Escape combos (never forwarded):
   Ctrl+Alt+Shift+Z toggle grab, +Q quit, +Enter fullscreen.
4. `settings_window.cpp`: resolution mode (**Host native** default / 1080p / 1440p /
   4K / custom WxH), bitrate slider 5–150 Mbps (default 20), port_base, autostart
   checkbox (wired M6). Feeds `STREAM_CONFIGURATION` on next connect.

**Accept:** focused viewer: Alt+Tab and Win key act on the remote machine;
Ctrl+Alt+Shift+Z then Alt+Tab acts locally; fullscreen toggles both ways mid-stream;
bitrate change visibly affects next connection.

### M5 — Host-resolution default + seamless monitor switching (3–5 days)
1. Host: `<CosmicDisplays>` in `nvhttp.cpp` serverinfo via `hostglue/displays.cpp`
   (wraps `platf::display_names()`; ordering contract commented at both call sites).
2. Viewer: parse the block; "Host native" resolves to the active display's WxH before
   `LiStartConnection` (fallback 1920x1080 when absent → stock-Sunshine compat).
3. Dropdown: on open, re-GET `/serverinfo`; select index i →
   `send_monitor_switch(i)` synthesizes Ctrl+Alt+Shift+F(1+i) via
   `LiSendKeyboardEvent`; update `active` locally.
4. Test the host path first: physically press Ctrl+Alt+Shift+F2 during a stream —
   capture must switch without disconnect (validates the vendored
   `mail::switch_display` → `video.cpp` reinit path before blaming synthesis).

**Accept:** 2+ monitor host: viewer connects at host native resolution by default
(debug overlay shows stream dimensions); dropdown switch < 2 s, no disconnect, audio
uninterrupted; 10× switch loop survives; no stuck modifiers on host (type to confirm).

### M6 — Autostart, packaging, provenance docs (1 week)
1. `autostart.cpp`: Win — `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\
   CosmicDesk = "<exe> --hidden"`; Linux — `~/.config/autostart/cosmicdesk.desktop`
   (`Exec=cosmicdesk --hidden`). `--hidden` starts to tray. Optional systemd user unit
   documented as alternative.
2. Windows packaging: `make-zip.ps1` bundling exe + MinGW DLLs (`ntldd -R`);
   Inno Setup installer (`installer.iss`) for the per-user setup exe.
3. Linux packaging: install target, .desktop files, udev rule + instructions;
   tarball (`make-tarball.sh`) and Debian package (`make-deb.sh`, udev rule
   activated, `Depends:` via `dpkg-shlibdeps`).
4. Release plumbing: CI publishes zip + setup exe + tarball + deb on `v*` tags and
   the rolling `nightly` prerelease; CI builds are Release-mode. The version is
   derived once by `cmake/CosmicDeskVersion.cmake` (`git describe`) and handed to
   the packaging scripts through the build tree, so no script re-derives it.
5. Docs: finish `VENDOR.md`; README — what it is, GPL-3.0, **Code provenance table**
   (every §7 row), WAN port-forwarding table, session-0/UAC limitation, Linux X11-only
   + udev note, build links.

**Accept:** fresh Windows machine (no MSYS2): unzip or run the setup exe → runs;
autostart on → reboot → tray present → another machine pairs and connects. Fresh
Ubuntu 24.04: someone other than the developer follows README alone (or installs the
deb) → same result. README provenance table covers every row of §7.

## 9. Verification strategy

1. **Interop bisection:** M1 = our host vs. stock moonlight-qt; M2 = our viewer vs.
   stock Sunshine. Any pre-M3 protocol bug has exactly one suspect side. Keep both
   stock apps installed permanently as reference oracles.
2. **Two-machine matrix:** minimum rig = one Windows + one Ubuntu 24.04 (X11) machine
   on a LAN. Passes per release: Win→Win, Win→Linux, Linux→Win, Linux→Linux.
   Loopback (127.0.0.1) allowed for pairing/UI smoke tests only (mirror-tunnel effect;
   useless for grab tests).
3. **Software-encoder pass:** each host acceptance run once with hardware encoders
   force-disabled (x264 path) — catches "works only on the dev GPU".
4. **Robustness checklist (M3, M5, M6):** connect/disconnect 10×; kill viewer
   mid-stream (host returns to idle, accepts next connect); sleep/wake host; unplug
   monitor mid-stream; wrong PIN 3× (host stays healthy); simultaneous bidirectional
   sessions.
5. **Input-state audit:** after every grab/ungrab/monitor-switch test, type on the
   host physically to detect stuck modifiers.
6. **CI:** build-only GitHub Actions job (MSYS2 + Ubuntu) from M0; no unit-test CI in
   v1 (nothing here unit-tests well without hardware).

## 10. Risk register

| # | Risk | L | I | Mitigation |
|---|---|---|---|---|
| R1 | MSYS2 toolchain friction (wrong shell, PATH, missing DLLs) | High | Med | BUILDING.md pictures the UCRT64 shell; M0 exists to burn this down; `ntldd` bundling scripted; never touch MSVC |
| R2 | Sunshine strip-down scope creep (one file drags ten) | High | High | Strip by deleting features, not minimizing files: copy whole `src/` minus drop list, get compiling, then delete. Timebox: subsystem resisting removal >1 day stays dormant |
| R3 | System FFmpeg breaks Sunshine encoder code (D4) | Med | High | Budgeted in M1.2; deletion order QSV/AMF/MF/Vulkan first; x264 floor tested (§9.3); escape hatch: LizardByte prebuilt FFmpeg for host only |
| R4 | No hardware encoder on user machines | Med | Med | x264 always compiled in; verify Sunshine's probe fall-through survives the strip |
| R5 | Wayland (Ubuntu default) has no capture in v1 | High | Med | Detect Wayland, show actionable "log in with Xorg" error; portal/PipeWire is documented post-v1 stretch |
| R6 | Keyboard-grab edge cases (stuck keys, grab persists, Win-key leaks) | Med | Med | Lift moonlight-qt handling verbatim (focus-loss release + key-up flush); escape combos always live; input-state audit each pass |
| R7 | Monitor-switch reinit races | Med | Med | Reuse Sunshine's shipped path unmodified; test via physical hotkey first (M5.4); 10× loop in acceptance |
| R8 | Port clash with installed Sunshine/Moonlight on dev machines | High | Low | `port_base` setting since M1; in M1 acceptance + README |
| R9 | Single process: viewer bug kills host role | Low | Low | Accepted for v1 (D2); teardown exercised by §9.4; post-v1: `--view <ip>` child process |
| R10 | UAC secure desktop / lock screen invisible | Certain | Med | Documented limitation (README); Task Scheduler elevated-run workaround documented; fix = service architecture, explicitly out of scope |
| R11 | Novice drowns in Sunshine's 90 KB files while debugging | Med | Med | Vendored code is a black box; debug at seams (hostglue, callbacks) with logging; stock-app oracles localize bugs first |
