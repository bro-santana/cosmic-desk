# File transfer addon — design (F1)

Status: F1 (client→host) designed and scheduled. F2 (host→client) designed on paper
only, not scheduled — see the end.

## Scope

Send files from the viewer to the host while a stream is running, by dropping them
onto the viewer window. This is the README's "file transfer" TODO line. It shares no
code with clipboard sync beyond the same HTTPS-authenticated-route pattern and the
same owner gate; clipboard stays text/PNG-only with its existing caps (frozen owner
decision — a >1 MiB clipboard payload is *not* routed through this feature).

Locked scope cuts for F1:

- **Direction: client→host only.** Host→client has no natural trigger UI from inside
  a streamed desktop; it is F2.
- **Files only, no folders.** A dropped directory is rejected with a visible message.
- **No destination-folder setting.** Files land in the host console user's
  `Downloads\CosmicDesk\`. The Settings panel has no folder picker or text-row
  precedent; adding one is not worth it for v1.
- **No resume across sessions.** A transfer id dies with the stream session.
- **One active upload at a time host-side.** The client queues multiple drops.

## Transport

Same trust boundary as clipboard sync: routes live **only on the
client-certificate-authenticated HTTPS port** (`port_base − 5`), gated by the same
three conditions as `/cosmic/clipboard` (feature toggle on, `session_count() > 0`,
requester cert fingerprint == recorded owner — reuse `cosmic_request_cert_fingerprint`
+ the owner store verbatim; the owner store lives in `cosmic::clipboard` today and the
file routes call it as the generic "stream owner" gate, with a comment).

Simple-Web-Server buffers every POST body fully before the handler runs and has no
streaming-read hook, so large files are **chunked at the application layer** —
bounded POSTs, nothing large ever buffered:

| Route | Purpose | Status codes |
|---|---|---|
| `POST /cosmic/file/begin?name=<pct-encoded utf-8>&size=<bytes>` | open a transfer; empty body | 200 + `X-Cosmic-File-Id: <id>`, 404 (gate), 409 (transfer already active), 507 (insufficient disk space), 400 (bad name/size) |
| `POST /cosmic/file/chunk?id=<id>&offset=<bytes>` | raw bytes body, ≤ 8 MiB | 200, 404 (gate or unknown id), 409 (offset ≠ bytes received — strictly sequential), 413 (chunk > 8 MiB) |
| `POST /cosmic/file/end?id=<id>` | finalize; empty body | 200, 404, 409 (size mismatch) |
| `POST /cosmic/file/abort?id=<id>` | client-side cancel; deletes partial | 200, 404 |

- All success responses carry `X-Cosmic-File-Version: 6`. `/serverinfo`
  `CosmicVersion` bumps 5→6 (nvhttp.cpp serverinfo, the only place).
- The client needs no capability latch: it only talks when the user drops a file, and
  a 404 from a stock/old host surfaces as "host doesn't support file transfer".
- Chunk size client-side: 4 MiB. File routes do **not** set
  `close_connection_after_response`, and the client reuses one curl easy handle for
  the whole transfer, so chunks ride one keep-alive connection instead of one TLS
  handshake each. (Fallback if review finds a problem: close per request — a perf
  hit only.) Note the HTTPS thread pool is 4; a transfer occupies one slot while a
  chunk is in flight, alongside the parked clipboard GET and Moonlight's idle
  keep-alive connection — that fits.
- Per-request curl timeouts: the clipsync `CURLOPT_TIMEOUT 45` handle config must
  NOT be reused blindly; the transfer handle uses a connect timeout plus
  `CURLOPT_LOW_SPEED_LIMIT/LOW_SPEED_TIME` (abort if < 1 KiB/s for 30 s) so a slow
  LAN never kills a legitimate big chunk.

## Host side (`src/hostglue/filerecv.{h,cpp}` + nvhttp handlers)

New module in the clipboard.h style: header free of vendored includes, one mutex,
handlers call into it. State: at most one `{id, sanitized_name, declared_size,
received, FILE*/ofstream to <dest>.part, last_activity}`.

- **Destination resolution**: `Downloads\CosmicDesk\` of the *console-session user*.
  Normal run: `SHGetKnownFolderPath(FOLDERID_Downloads)`. Elevated/service (SYSTEM)
  run: FOLDERID_Downloads would resolve to the SYSTEM profile, so reuse the
  wallpaper.cpp WTS-console-session → profile-path pattern and use
  `<profile>\Downloads`. Documented caveat: a user-redirected Downloads folder is not
  honored in service mode. Directory created on first transfer.
- **Filename safety** (host writes attacker-influenced names to disk — this is the
  security-sensitive part): take the basename only; reject/strip path separators,
  `..`, control chars, trailing dots/spaces; refuse reserved Windows device names
  (CON, PRN, AUX, NUL, COM1–9, LPT1–9, case-insensitive, with or without extension);
  empty result → `file`; cap 200 bytes. **Never overwrite**: on collision pick
  `name (2).ext`, `name (3).ext`, …
- **Atomic finish**: write to `<final>.part`, flush+close, then rename on `/end`.
  Any failure path deletes the `.part`.
- **begin checks**: declared size ≤ free disk space on the destination volume
  (else 507); a second `begin` while one is active → 409.
- **GC / teardown**: `tick()` from the main loop (same seam as
  `cosmic::clipboard::tick()`) aborts a transfer idle > 90 s and deletes the
  partial. Abort-and-delete also on: toggle switched off, last streaming session
  teardown (same stream.cpp block that clears the clipboard owner), host shutdown.
- **Flagged inherited risk**: ownership follows the last `/launch`/`/resume` cert,
  not the live RTSP session (documented clipboard limitation). For a route that
  writes to disk this is worth restating in PROTOCOL.md; the mitigations are the
  never-overwrite rule, the dedicated subfolder, and the sanitizer. Binding
  ownership to the RTSP session stays a later design item.

## Client side (`src/app/filesync.{h,cpp}` + main.cpp)

Mirrors clipsync's shape: worker thread started/stopped from the same main.cpp seam
that manages clipsync (keyed off `SessionStatus::state == Streaming`), base URL built
the same way, TLS config modeled on `ConfigureCurl` with the timeout change above.

- **Input**: `SDL_EVENT_DROP_FILE` in the single main-loop event pump (nothing
  enables drop events today; enable them and accept drops only while in viewer mode
  with an active stream). Directories rejected with a visible message; multiple
  dropped files are queued and sent sequentially.
- **Flow per file**: `begin` → loop `chunk` (4 MiB, sequential offsets, one reused
  handle) → `end`. Any HTTP error or curl failure aborts the transfer (best-effort
  `abort` POST), surfaces the error, and moves to the next queued file. `stop()`
  cancels mid-transfer via the progress callback like clipsync.
- **Progress UI**: a minimal overlay in viewer mode (PairProgress-style
  mutex-snapshot from the worker): current filename, transferred/total, queue count,
  and error/done state; visible while active and for ~3 s after finishing; no
  interaction except the implicit "dropping more files appends to the queue".
- **Settings**: one `share_files` toggle (default on, sibling of `share_clipboard`,
  all four UI touch points). It gates the **host-side routes** on the machine it is
  set on; sending by drag-and-drop is always allowed (user-initiated; the receiving
  host's toggle is what consents).

## Docs & versioning

- PROTOCOL.md: new "Cosmic extension: file transfer" section (route table above,
  gate description, sanitizer rules, the ownership caveat restated, CosmicVersion 6).
- VENDOR.md: nvhttp.cpp/stream.cpp modification notes updated.
- README: TODO line updated (file transfer: client→host shipped; host→client
  pending).

## Acceptance (owner runtime test, like clipboard's)

Two machines, active stream: drop a 100 KB file and a ~500 MB file onto the viewer →
both appear in host `Downloads\CosmicDesk\` with correct bytes (hash-compare the big
one); drop the same file twice → ` (2)` suffix, no overwrite; drop a folder →
rejected message, no request sent; toggle `share_files` off on the host → next drop
fails visibly with nothing written; kill the stream mid-transfer → `.part` removed;
stock host → clean "not supported" error.

## F2 (host→client) — designed, not scheduled

Trigger: the user copies a file on the *host* desktop (Explorer Ctrl+C → CF_HDROP).
The host clipboard watcher notices the file-list format, and instead of syncing
clipboard bytes, publishes a lightweight "file offer" (names + sizes) on a new
route; the client shows an accept toast and, on accept, pulls the bytes with chunked
GETs (`/cosmic/file/get?id=&offset=&len=`) into the client's `Downloads\CosmicDesk\`,
then optionally places a CF_HDROP on the client clipboard pointing at the downloaded
copy so "paste" works. Needs Win32 clipboard reading (SDL exposes no CF_HDROP),
offer expiry, and multi-file handling — a full wave of its own, to be scheduled
after F1 passes the owner's runtime test.
