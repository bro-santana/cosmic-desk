# Building Cosmic Desk

Cosmic Desk builds into a **single executable** (`cosmicdesk`) that contains both the
host and viewer roles.

> **Windows uses MSYS2/MinGW, not MSVC.** This is not a preference: the Sunshine host
> code we vendor (from milestone M1 onward) only builds with GCC/Clang under MSYS2.
> Do not try to configure this project with Visual Studio generators.

## Windows (MSYS2 UCRT64)

### 1. Install MSYS2

Download and run the installer from https://www.msys2.org (default location
`C:\msys64`).

### 2. Open the right shell

From the Start menu, launch **"MSYS2 UCRT64"** — *not* "MSYS2 MSYS" and *not*
"MSYS2 MINGW64". The prompt must show `UCRT64` in magenta. Everything below is run in
that shell. If you get "command not found" for `cmake` or `gcc`, you are almost
certainly in the wrong shell.

### 3. Install the toolchain and dependencies

```bash
pacman -Syu            # if it asks you to restart the shell, do so and run it again
pacman -S --needed git \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-SDL2 \
  mingw-w64-ucrt-x86_64-nlohmann-json
```

From milestone M1 (host streaming) onward you also need:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-expat \
  mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-boost \
  mingw-w64-ucrt-x86_64-MinHook
```

`libMinHook.a` (required by `host/sunshine/CMakeLists.txt` via
`find_library(MINHOOK_LIBRARY libMinHook.a REQUIRED)`) comes from the
`mingw-w64-ucrt-x86_64-MinHook` package above.

**If a download stalls** ("Operation too slow"), just run the same `pacman -S` command
again — partial downloads are cached and resumed. The MSYS2 mirrors are occasionally
slow; this is not a problem with the project.

### 4. Clone and build

```bash
git clone <repo-url> cosmic-desk
cd cosmic-desk
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/cosmicdesk.exe
```

To produce a self-contained zip (no MSYS2 needed on the target machine), run the
bundling script from the repo root in the **UCRT64** shell. It discovers DLLs
with `ntldd` when available and falls back to a built-in list otherwise; either
way it fails loudly if the bundle is missing load-critical DLLs:

```bash
powershell -ExecutionPolicy Bypass -File packaging\windows\make-zip.ps1
```

The script is `packaging/windows/make-zip.ps1`; it produces
`dist\CosmicDesk-windows-x64.zip` (exe + assets + MinGW DLLs + LICENSE + README).

### Running the Windows service

To stream UAC prompts, the lock screen and the logon screen, run the app as a
service (PLAN.md M7–M10). On a dev tree, produce the bundle first (the step
above) — the service spawns `cosmicdesk.exe` with no MSYS2 PATH, so it needs
the DLLs the bundle places next to it. Then, from the repo root:

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\install-service.ps1
```

`install-service.ps1` self-elevates through one UAC prompt. It prefers the
packaged bundle (`dist\CosmicDesk\tools\cosmicsvc.exe`) and warns when it falls
back to the dev build; pass `-ServiceExe <path>` to force a specific binary.
`uninstall-service.ps1` removes the service. The installer (above) offers the
same service as an optional task. To update an installed service after a
rebuild: stop it first (`sc.exe stop CosmicDeskService` — it locks
`dist\CosmicDesk\tools\cosmicsvc.exe` while running), re-run `make-zip.ps1`,
then start it again (`sc.exe start CosmicDeskService`).

To produce an Inno Setup installer from that bundle (requires Inno Setup 6 or 7,
`ISCC.exe` on PATH), run from the repo root:

```bash
ISCC.exe packaging\windows\installer.iss
```

The script is `packaging/windows/installer.iss`; it produces
`dist\CosmicDesk-windows-x64-setup.exe`, with Start Menu and optional desktop
shortcuts and an uninstaller. It defaults to a machine-wide install under
`Program Files` (one UAC prompt) and offers a per-user install on the first
wizard page (also reachable as `/CURRENTUSER`). The Cosmic Desk service task
is checked by default and only applies to the machine-wide install — the
LocalSystem service must never run binaries from a user-writable per-user
folder. No version is passed on the command line — it comes from
`build\packaging\windows\version.iss`, so the build must be configured first
(see [Versioning](#versioning)). The installer is unsigned; SmartScreen will
warn until a code-signing certificate is set up.

## Linux (Ubuntu 24.04)

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build git pkg-config \
  libsdl2-dev nlohmann-json3-dev \
  libayatana-appindicator3-dev libgtk-3-dev
```

From M1 onward, also:

```bash
sudo apt install -y \
  libavcodec-dev libavutil-dev libswscale-dev libavfilter-dev \
  libssl-dev libcurl4-openssl-dev libexpat1-dev libopus-dev \
  uuid-dev \
  libboost-dev libboost-log-dev libboost-locale-dev libboost-program-options-dev \
  libx11-dev libxfixes-dev libxrandr-dev libxtst-dev \
  libxcb1-dev libxcb-shm0-dev libxcb-xfixes0-dev \
  libdrm-dev libcap-dev libevdev-dev libudev-dev \
  libpulse-dev libva-dev libnotify-dev \
  python3-pip python3-setuptools
```

The glad generator (used by the Linux VAAPI build) needs Python with jinja2 and
setuptools at configure time, so also run:

```bash
pip3 install jinja2 setuptools
```

Then:

```bash
git clone <repo-url> cosmic-desk
cd cosmic-desk
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
./build/cosmicdesk
```

Input injection on Linux needs a udev rule (from M1 onward):

```bash
sudo cp packaging/linux/60-cosmicdesk-input.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**Linux capture is X11-only for v1.** On a Wayland session the host role will refuse to
start capture; log in with an "Ubuntu on Xorg" session.

### Install

After a successful build, install the binary and support files system-wide with:

```bash
sudo cmake --install build
```

This installs into `/usr/local` by default:

- `<libdir>/cosmicdesk/cosmicdesk` — the executable (`/usr/local/lib/...` by
  default; `lib64` on some distros)
- `<libdir>/cosmicdesk/assets/` — runtime assets (icons, capture shaders), kept
  next to the binary in one self-contained dir (mirrors the Windows bundle)
- `share/applications/cosmicdesk.desktop` — the application-menu entry
  (generated at configure time so its Exec path always matches the prefix)
- `share/icons/hicolor/64x64/apps/cosmicdesk.png` — the menu icon
- `share/cosmicdesk/60-cosmicdesk-input.rules` — the udev rule (not activated; see above)

The binary chdirs to its own directory at startup, so the CWD-relative
`assets/` path the vendored host uses for its shaders resolves regardless of
how the app is launched (terminal, menu, or autostart).

Alternatively, `packaging/linux/make-tarball.sh` installs the same rules into a staging
prefix and produces a self-contained `build/dist-linux/cosmicdesk-linux-x64.tar.gz`
(no root required). Input injection still needs the udev rule copied manually as shown
above.

For a Debian package, `packaging/linux/make-deb.sh [VERSION]` stages the same rules and
builds `build/dist-deb/cosmicdesk_<version>_amd64.deb` (install with
`sudo apt install ./cosmicdesk_<version>_amd64.deb`). Differences from the tarball: it
installs into `/usr` (configure the build with `-DCMAKE_INSTALL_PREFIX=/usr` so the
`.desktop` Exec path matches, as CI does), the udev rule is installed into
`/usr/lib/udev/rules.d/` so input injection works out of the box, and `Depends:` is
computed with `dpkg-shlibdeps`, so the package targets the same distribution it is
built on (CI builds on Ubuntu 24.04; older releases need a rebuild there). The
`VERSION` argument is optional and only overrides the version CMake derived (see
[Versioning](#versioning)).

## Versioning

The release version is derived **once**, by `cmake/CosmicDeskVersion.cmake`, from
`git describe --tags --long --always` at configure time. Nothing else runs `git
describe` — that is how the Windows installer and the Debian package would end up
disagreeing about what they are.

CMake writes the derived value into the build tree in the two forms the packaging
scripts need:

| Generated file | Read by | Variable |
|---|---|---|
| `build/packaging/version.env` | `packaging/linux/make-deb.sh` | `COSMICDESK_VERSION_DEB` |
| `build/packaging/windows/version.iss` | `packaging/windows/installer.iss` | `AppVersion`, `VersionInfoVersion` |

Four shapes are derived, because the packaging formats disagree about what a version
may look like:

| Tag state | `_VERSION` | `_FULL` | `_INFO` (Windows) | `_DEB` |
|---|---|---|---|---|
| on `v1.2.3` | `1.2.3` | `1.2.3` | `1.2.3.0` | `1.2.3` |
| 3 commits past `v1.2.3` | `1.2.3` | `1.2.3-3-gabc1234` | `1.2.3.3` | `1.2.3+3+gabc1234` |
| 2 commits past `v2.0.0-rc1` | `2.0.0` | `2.0.0-rc1-2-gabc1234` | `2.0.0.2` | `2.0.0~rc1+2+gabc1234` |
| no tag reachable | `0.0.0` | `0.0.0+gabc1234` | `0.0.0.0` | `0.0.0+gabc1234` |

The commit count is the fourth Windows `VERSIONINFO` field, so successive nightlies
are distinguishable instead of all reporting `0.0.0.0`. In the Debian version a
pre-release tag's hyphen becomes `~` (`2.0.0~rc1` sorts *before* `2.0.0`, which is
what such a tag means) and the remaining separators become `+`, since dpkg reads the
last hyphen as the `debian_revision` separator.

Two consequences worth knowing:

- **CI must check out full history.** `git describe` sees no tags in a shallow clone,
  so both jobs in `.github/workflows/build.yml` set `fetch-depth: 0`. Without it every
  build silently reports `0.0.0`.
- Re-running CMake is what refreshes the version, so the module registers `.git/HEAD`
  and the branch ref as configure dependencies — committing locally re-runs configure
  on the next build.

## Common problems

| Symptom | Cause | Fix |
|---|---|---|
| `Could not find a package configuration file provided by "SDL2"` | Dependencies not installed, or you are in the wrong MSYS2 shell | Use the **UCRT64** shell; re-run the `pacman -S` line |
| `cmake: command not found` in MSYS2 | You installed `cmake` instead of `mingw-w64-ucrt-x86_64-cmake`, or you are in the MSYS shell | Install the ucrt64 package and use the UCRT64 shell |
| Build succeeds but the app fails to start with a missing DLL | Running the exe outside the MSYS2 environment | Run from the UCRT64 shell, or bundle DLLs (see `packaging/windows/`, milestone M6) |
| A *bundled* exe fails at startup with a missing-DLL dialog (e.g. `avcodec-62.dll`) | The bundle was made on a machine with an incomplete MSYS2 install | Reinstall the section-3 packages (without `--needed` to restore deleted files), re-run `make-zip.ps1`; the script now fails loudly instead of writing an incomplete bundle |
| `make-zip.ps1`: "No DLLs were bundled" or "missing critical DLLs" | MSYS2 not found or its DLLs were deleted | Check the path it reports; reinstall the section-3 packages; the script locates MSYS2 via `build\CMakeCache.txt` |
| Tray icon missing | `assets/` not next to the executable | The build copies it automatically; re-run `ninja -C build` |
| Port already in use | Stock Sunshine is installed and holding 47989 | Change `port_base` in the app settings (`cosmic.json`) |
| Windows Firewall prompt on first launch | The host listens on the six GameStream ports | Allow the prompt (private networks) or add an inbound rule; the ports are listed in `docs/PROTOCOL.md` |
