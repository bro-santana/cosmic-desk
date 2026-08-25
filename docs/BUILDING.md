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

## Common problems

| Symptom | Cause | Fix |
|---|---|---|
| `Could not find a package configuration file provided by "SDL2"` | Dependencies not installed, or you are in the wrong MSYS2 shell | Use the **UCRT64** shell; re-run the `pacman -S` line |
| `cmake: command not found` in MSYS2 | You installed `cmake` instead of `mingw-w64-ucrt-x86_64-cmake`, or you are in the MSYS shell | Install the ucrt64 package and use the UCRT64 shell |
| Build succeeds but the app fails to start with a missing DLL | Running the exe outside the MSYS2 environment | Run from the UCRT64 shell, or bundle DLLs (see `packaging/windows/`, milestone M6) |
| Tray icon missing | `assets/` not next to the executable | The build copies it automatically; re-run `ninja -C build` |
| Port already in use | Stock Sunshine is installed and holding 47989 | Change `port_base` in the app settings (`cosmic.json`) |
| Windows Firewall prompt on first launch | The host listens on the six GameStream ports | Allow the prompt (private networks) or add an inbound rule; the ports are listed in `docs/PROTOCOL.md` |
