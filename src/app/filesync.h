// Cosmic Desk — client-side file transfer worker (viewer half).
//
// While a viewer session is active, a single background worker drains a queue
// of local file paths (filled by the main thread when the user drops files on
// the viewer window) and uploads them one at a time to the host's
// POST /cosmic/file/{begin,chunk,end,abort} routes. Those routes live only on
// the host's client-cert-authenticated HTTPS port (see host/sunshine's
// /cosmic/file handlers — not included from here, per src/app's rule against
// including vendored Sunshine headers, and docs/FILE_TRANSFER_PLAN.md for the
// route table this module implements).
//
// The host's HTTP library buffers a whole POST body before its handler runs,
// so a file is chunked at the application layer — begin -> N x chunk -> end,
// bounded kChunkBytes POSTs on one reused keep-alive connection — and nothing
// large is ever buffered on either side.
//
// /begin answers 200 plus an `X-Cosmic-File-Id` header naming the transfer.
// An unauthenticated or stock (non-Cosmic) Sunshine host can still answer 200
// OK with a 404-shaped XML body — the HTTP status alone proves nothing (the
// same lesson clipsync.h records for the clipboard seq header, and
// wallcache.cpp's LooksLikeImage for the wallpaper route) — so this module
// refuses to upload anything until it has seen a non-empty id header.
//
// The worker never draws and never logs: it publishes a Progress snapshot
// that the viewer overlay copies once a frame, and stays silent on every
// failure path so a host with file sharing off cannot spam the log.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace cosmic::filesync {

// Bytes per /cosmic/file/chunk POST. Comfortably under the host's 8 MiB
// per-chunk limit, and large enough that a big file is not thousands of
// round trips.
inline constexpr std::size_t kChunkBytes = 4 * 1024 * 1024;

// Most files that may wait in the queue at once. A drop of a whole folder's
// worth of files is rejected past this point rather than letting the queue
// grow without bound.
inline constexpr int kMaxQueued = 64;

// Snapshot of worker state for the viewer overlay, in the style of
// cosmic::ui::PairProgress (src/ui/bridge/bridge.h:17-23). Copied out by
// progress() under the worker's mutex; the overlay never sees a half-updated
// value.
//
// `updated_ms` is a std::chrono::steady_clock timestamp in milliseconds, from
// the same clock the overlay's own std::chrono::steady_clock::now() reads, so
// "this snapshot changed less than 3 s ago" is a plain subtraction. It is
// refreshed on every change to any other field, including the transition to
// done/error, which is what lets the overlay linger for a few seconds after a
// transfer finishes and then fade out.
struct Progress {
    bool active = false;              // a file is being uploaded right now
    std::string filename;             // basename of the current/last file
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    int queued = 0;                   // files still waiting, excluding the current one
    bool done = false;                // the last file finished successfully
    std::string error;                // sticky; non-empty = the last file failed
    std::uint64_t updated_ms = 0;     // steady_clock ms when this snapshot last changed
};

// Spawns the worker, which uploads whatever enqueue() hands it to
// <host_address>:<https_port>/cosmic/file until stop() is called. No-op if
// the worker is already running (a second start() must go through stop()
// first). Clears the queue and the progress snapshot, so a new session never
// resumes another host's transfer or shows the previous session's result.
// Main thread only, on the session-start transition.
void start(const std::string& host_address, int https_port);

// Signals the worker to stop, aborts any in-flight HTTP transfer (so this
// does not block for the transfer's low-speed budget), joins the worker, and
// clears the queue and the progress snapshot.
// Idempotent; safe to call even if start() was never called.
// Main thread only.
void stop();

// Queues one local file for upload and returns true, or returns false and
// records a sticky Progress::error when the worker is not running or the
// queue already holds kMaxQueued paths. `utf8_path` is a UTF-8 absolute path
// (what SDL drop events deliver); it is opened, sized and named on the worker
// thread, so a path that has gone away by then simply fails that one file.
// Never blocks on the network — only on the snapshot mutex.
// Thread-safe; called from the main thread.
bool enqueue(const std::string& utf8_path);

// Copy of the current worker state for the overlay. Never blocks on the
// network — only on the snapshot mutex.
// Thread-safe; called from the main thread, once a frame.
Progress progress();

}  // namespace cosmic::filesync
