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
// the only thing allowed to read/write it, so this module exchanges raw
// bytes plus a Mime tag with it via publish_local() (main -> worker) and
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

// Cap for a Mime::Text payload in either direction, mirroring the host's own
// 1 MiB text POST body limit. A GET response body is not yet known to be text
// or image while it is still arriving, so the transfer itself is bounded by
// the looser kMaxImageBytes below; this cap is re-checked once Content-Type
// confirms the body is text (see clipsync.cpp's PollOnce). A publish_local()
// Mime::Text payload larger than this is dropped outright.
inline constexpr std::size_t kMaxBytes = 1024 * 1024;

// Cap for an image (Mime::Png) payload in either direction, mirroring the
// host's own 8 MiB image POST body limit (src/hostglue/clipboard.h). Text
// stays capped at kMaxBytes.
inline constexpr std::size_t kMaxImageBytes = 8 * 1024 * 1024;

// The kind of bytes carried by publish_local()/take_incoming() and exchanged
// with the host over /cosmic/clipboard's Content-Type/Accept headers.
enum class Mime { Text, Png };

// Spawns the worker, which polls <host_address>:<https_port>/cosmic/clipboard
// until stop() is called. No-op if the worker is already running (a second
// start() must go through stop() first). Resets the poll sequence and both
// exchange buffers, so a new session never resumes another host's sequence
// or leaks a previous session's pending payload.
// Main thread only, on the session-start transition.
void start(const std::string& host_address, int https_port);

// Signals the worker to stop, aborts any in-flight HTTP transfer (so this
// does not block for the transfer's timeout budget), and joins the worker.
// Idempotent; safe to call even if start() was never called.
// Main thread only.
void stop();

// Hands bytes copied on this machine to the worker so they can be POSTed to
// the host on its next loop iteration; POSTing takes priority over polling.
// `bytes` larger than kMaxBytes (Mime::Text) or kMaxImageBytes (Mime::Png)
// is dropped. An image is held rather than sent while the worker has not yet
// heard from the host whether it understands image/png, and is silently
// dropped, without ever reaching the wire, once the host is confirmed not to
// (a v4 or earlier host) — such a host would otherwise store the PNG bytes
// mislabeled as text. Last-write-wins: a value not yet sent when this is
// called again is replaced, not queued. Taken by value and moved into the
// exchange slot so an 8 MiB image is never copied while the slot's mutex is
// held.
// Thread-safe; called from the main thread.
void publish_local(Mime mime, std::string bytes);

// Moves the most recently received host clipboard bytes into `out_bytes`,
// its mime into `out_mime`, and returns true, or returns false and leaves
// both unchanged when there is nothing new since the last call.
// Last-write-wins: only the most recent value the worker accepted from the
// host is ever available here.
// Thread-safe; called from the main thread.
bool take_incoming(std::string& out_bytes, Mime& out_mime);

}  // namespace cosmic::clipsync
