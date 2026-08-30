// Cosmic Desk — client-side clipboard sync worker (viewer half).
//
// While a viewer session is active, a single background worker polls the
// host's GET /cosmic/clipboard about once a second and POSTs whatever was
// most recently copied on this machine to it. Both routes live only on the
// host's client-cert-authenticated HTTPS port (see host/sunshine's
// /cosmic/clipboard handlers — not included from here, per src/app's rule
// against including vendored Sunshine headers).
//
// The worker never touches the system clipboard directly: the main loop is
// the only thing allowed to read/write it, so this module exchanges plain
// std::strings with it via publish_local() (main -> worker) and
// take_incoming() (worker -> main), each a single last-write-wins slot
// guarded by one mutex — a clipboard has no queue semantics, so only the
// most recent value on either side ever matters.
//
// GET responses carry an `X-Cosmic-Clipboard-Seq` header that both signals
// "the clipboard changed" (body present, 200) and "nothing changed since
// your last poll" (no body, 204). An unauthenticated or stock (non-Cosmic)
// Sunshine host can still answer 200 OK with a 404-shaped XML body — the HTTP
// status alone proves nothing (see wallcache.cpp's LooksLikeImage comment for
// the same reasoning applied to the wallpaper route) — so clipsync.cpp
// requires a well-formed seq header before it will trust a body at all.
#pragma once
#include <cstddef>
#include <string>

namespace cosmic::clipsync {

// Cap shared by both directions: a GET response body larger than this is
// discarded (never truncated) and a publish_local() text larger than this is
// dropped, mirroring the host's own 1 MiB POST body limit.
inline constexpr std::size_t kMaxBytes = 1024 * 1024;

// Spawns the worker, which polls <host_address>:<https_port>/cosmic/clipboard
// until stop() is called. No-op if the worker is already running (a second
// start() must go through stop() first). Resets the poll sequence and both
// exchange buffers, so a new session never resumes another host's sequence
// or leaks a previous session's pending text.
// Main thread only, on the session-start transition.
void start(const std::string& host_address, int https_port);

// Signals the worker to stop, aborts any in-flight HTTP transfer (so this
// does not block for the transfer's timeout budget), and joins the worker.
// Idempotent; safe to call even if start() was never called.
// Main thread only.
void stop();

// Hands text copied on this machine to the worker so it can be POSTed to the
// host on its next loop iteration; POSTing takes priority over polling. A
// text larger than kMaxBytes is dropped. Last-write-wins: a value not yet
// sent when this is called again is replaced, not queued.
// Thread-safe; called from the main thread.
void publish_local(const std::string& text);

// Moves the most recently received host clipboard text into `out_text` and
// returns true, or returns false and leaves `out_text` unchanged when there
// is nothing new since the last call. Last-write-wins: only the most recent
// value the worker accepted from the host is ever available here.
// Thread-safe; called from the main thread.
bool take_incoming(std::string& out_text);

}  // namespace cosmic::clipsync
