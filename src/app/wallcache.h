// Cosmic Desk — host wallpaper cache (PLAN.md D10(d)(e), milestone W2 item 2).
//
// Turns a changed /serverinfo wallpaper hash (see app/presence.h) into a
// download over the host's authenticated HTTPS port (http_port - 5) and
// caches the result on disk, so the Bridge backdrop (later phase) can render
// it even while the host is offline.
//
// Cache layout: <Settings::config_dir()>/wallpapers/<host-key>.<hash>.img
// where <host-key> is <sanitized-address>-<8 lowercase hex fnv1a32(address)>:
// <sanitized-address> replaces every character outside [A-Za-z0-9._-] with
// '_', and the hash suffix (of the raw, pre-sanitization address) keeps two
// addresses that sanitize to the same string (e.g. "a:b" and "a_b" both
// becoming "a_b") from colliding, and keeps <host-key> from ever exactly
// matching a Win32 reserved device name (CON, NUL, COM1, ...). The
// wallpaper-hash-in-filename doubles as the sidecar: a matching file means
// the cache is already current for that hash, and any other <host-key>.*.img
// is stale and pruned once a fresh one lands. Files written by a pre-<host-key>
// development build (stem = bare sanitized address, no hash suffix) are not
// recognised or pruned by this format; accepted because that format never
// shipped.
//
// sync() downloads at most ONE wallpaper per call, so it is meant to be
// called once per presence poll pass (~10 s) — that is the "one download in
// flight at a time" budget from the design. It owns its own curl easy handle
// rather than reusing third-party/libgamestream/http.c's, because that handle
// is global and already owned by the viewer session worker (pairing,
// applist, launch); sharing it would race a fetch from the presence thread.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace cosmic::wallcache {

// One host to consider for a wallpaper download.
struct Target {
    std::string address;         // as saved (identity key), unbracketed
    int http_port = 0;           // the host's HTTP port; HTTPS is http_port - 5
    std::string wallpaper_hash;  // as advertised; "" = nothing to fetch
};

// Downloads at most ONE wallpaper per call. Call once per presence poll pass,
// from the presence worker only (not thread-safe against a concurrent sync).
void sync(const std::vector<Target>& targets);

// Cached image for `address`, or an empty path when there is none.
// Thread-safe; called from the UI thread.
std::filesystem::path path_for(const std::string& address);

// Drops every cached file for `address`. Thread-safe.
void forget(const std::string& address);

// Shutdown path only: aborts an in-flight download (if any) so
// presence::stop() does not block for a stalled host's curl timeout budget
// while joining the worker. Call once, after the presence worker has been
// told to stop but before joining it. An aborted transfer leaves no .img —
// its temp file is removed like any other failed download.
void cancel();

// Clears a pending cancel() before a fresh worker starts. The shutdown flag
// must never be cleared by the worker itself (sync() does not clear it) — a
// cancel() racing the start of a poll pass would otherwise be lost and that
// pass's download would become uncancellable again. Call once, from
// presence::start(), only on the path that actually spawns the worker.
void reset();

}  // namespace cosmic::wallcache
