# Cosmic Desk

A minimal AnyDesk/RustDesk-style remote desktop tool built on the **Moonlight protocol**.

Cosmic Desk is **one application that is both ends of the connection**: the same binary
hosts your screen and views someone else's. It runs quietly in the tray, starts with the
PC, and connects directly by IP — there is no account, no connection-ID broker and no
relay server.

> **Status: early development.** Milestone M1 is complete — the app now hosts the
> desktop over the Moonlight protocol: pair with a stock Moonlight client using
> Cosmic Desk's native PIN dialog, then stream video, audio and input. The app
> boots to the tray and runs the host automatically. The viewer role (connecting
> *to* a host from Cosmic Desk) is still not implemented (M2). The two-machine
> interop matrix from [`PLAN.md`](PLAN.md) §9 still has to be run manually against
> a stock Moonlight client on a second machine — this has not been machine-validated
> yet. See [`PLAN.md`](PLAN.md) for the full milestone plan.

## Features (target scope)

- Connect to a host by IP address
- Moonlight PIN pairing, confirmed with a **native dialog** — no web interface;
  after the first pairing a machine reconnects without confirmation
- Background tray application that starts with the PC
- Remote view in **fullscreen or windowed** mode
- Top bar to **switch which monitor is being presented**, seamlessly mid-stream, and to
  exit the session
- **Captures all input, including Alt+Tab**, while the viewer window is focused
- Selectable **resolution and encoding bitrate**; defaults to the host's desktop resolution
- Windows and Linux, in both roles

Deliberately out of scope: file transfer, clipboard sync, gamepads, session recording,
unattended access via a relay, and the rest of the commercial feature set.

## Building

See [`docs/BUILDING.md`](docs/BUILDING.md). In short:

- **Windows:** MSYS2 **UCRT64** shell (MSVC is not supported — the vendored host code
  requires GCC/Clang), then `cmake -B build -G Ninja && ninja -C build`.
- **Linux:** install the listed `apt` packages, then the same CMake commands.

## Documentation

| Document | Contents |
|---|---|
| [`PLAN.md`](PLAN.md) | Architecture, design decisions, milestones, risks |
| [`docs/BUILDING.md`](docs/BUILDING.md) | Toolchain setup and troubleshooting |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | Ports, pairing flow, and the Cosmic protocol extensions |
| [`docs/VENDOR.md`](docs/VENDOR.md) | **Provenance of every piece of copied code** |

## Code provenance & licensing

**Cosmic Desk is assembled from existing open-source projects rather than written from
scratch.** The streaming stack — capture, encoding, pairing, RTSP, and the client-side
protocol core — comes from mature GPL-3.0 projects. Code is copied, adapted and in some
cases merely imitated; in all cases it is credited.

[`docs/VENDOR.md`](docs/VENDOR.md) is the authoritative inventory: it lists every
component, the exact upstream revision it was taken from, whether it was modified, and
how to diff our copy against upstream. Modifications to vendored files are marked inline
with `COSMIC MODIFICATION` comments.

| Upstream project | Licence | What we use it for |
|---|---|---|
| [Sunshine](https://github.com/LizardByte/Sunshine) | GPL-3.0 | The host role: GameStream HTTP/HTTPS server, pairing state machine, RTSP, control stream, screen capture, encoding, audio, input injection |
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) | GPL-3.0 | The viewer protocol core: RTSP client, control stream, RTP depacketization, FEC, input events |
| [moonlight-embedded](https://github.com/moonlight-stream/moonlight-embedded) | GPL-3.0 | Client-side pairing (`libgamestream`), plus the FFmpeg-decode / SDL-render / Opus-playback structure |
| [moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) | GPL-3.0 | Keyboard-grab and fullscreen handling, and the SDL-scancode-to-Windows-VK translation table |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | All user interface |
| [tray](https://github.com/zserge/tray) | MIT | Tray icon and menu |

Because Cosmic Desk builds on GPL-3.0 code, **Cosmic Desk is licensed under GPL-3.0** —
see [`LICENSE`](LICENSE). One vendored component, libdisplaydevice, is AGPL-3.0;
combining it with the GPL-3.0 code makes the combined work subject to AGPL-3.0 terms.
Cosmic Desk is not affiliated with or endorsed by the LizardByte or Moonlight projects.

## Known limitations

- **Windows: no session-0 service.** A real Windows service cannot capture the
  interactive desktop or inject input into it, so Cosmic Desk autostarts as a per-user
  tray application instead. It therefore cannot be reached before a user logs in, and
  cannot see UAC secure-desktop prompts.
- **Linux: X11 only.** Wayland capture (portal/PipeWire) is planned but not in v1; log in
  with an Xorg session.
- **Direct connections only.** For access across the internet you must forward the ports
  listed in [`docs/PROTOCOL.md`](docs/PROTOCOL.md) yourself.
