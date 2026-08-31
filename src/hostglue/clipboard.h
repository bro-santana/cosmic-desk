// Cosmic Desk — bidirectional clipboard sync bridge between the app main
// thread (which owns the SDL clipboard) and the vendored Sunshine host's
// GET/POST /cosmic/clipboard route (host/sunshine/src/nvhttp.cpp). The
// outbound store holds the local clipboard bytes (text or PNG image) the
// route serves to a connected client; the inbound queue holds bytes POSTed
// by a client for the main thread to apply to the local clipboard. The
// store holds either kind at a time, last-write-wins across kinds.
//
// This header is deliberately free of vendored Sunshine includes so nvhttp.cpp
// can include it without dragging the host's dependency graph into the app;
// this bridge needs none (same pattern as wallpaper.h/pin_bridge.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace cosmic::clipboard {

// Shared cap on clipboard text size, enforced by every producer (publish(),
// push_incoming()) and consumer of this store.
inline constexpr std::size_t kMaxBytes = 1024 * 1024;

// Shared cap on clipboard image (PNG) size, enforced the same way as
// kMaxBytes but for Mime::Png payloads.
inline constexpr std::size_t kMaxImageBytes = 8 * 1024 * 1024;

// Long-poll hold duration for GET /cosmic/clipboard?wait=1: how long a
// request with no immediately available data is parked before it is
// answered with WaitResult::Timeout.
inline constexpr std::uint64_t kClipboardHoldMs = 20000;

// The kind of bytes held by the store: plain text or a PNG-encoded image.
enum class Mime { Text, Png };

// The Content-Type header value to serve for a given mime.
const char *content_type(Mime mime);

// The size cap (kMaxBytes or kMaxImageBytes) that applies to a given mime.
std::size_t max_bytes(Mime mime);

// Outcome delivered to a parked fetch_or_park() caller. Changed carries the
// new sequence number, mime and bytes; Timeout and Unavailable carry neither
// (Mime::Text with empty bytes, and for Unavailable a seq of 0).
enum class WaitResult { Changed, Timeout, Unavailable };
using WaitCallback = std::function<void(WaitResult, std::uint64_t seq, Mime mime, std::string bytes)>;

// Host-side opt-out (settings' share_clipboard). Defaults to true. Thread-safe.
// NOTE: not consulted by publish()/fetch()/push_incoming() below -- the
// /cosmic/clipboard routes own the enable/session gate; see clipboard.cpp.
void set_enabled(bool enabled);
bool enabled();

// Outbound: called from the app main thread whenever the local SDL clipboard
// changes. Stores the mime and bytes for GET /cosmic/clipboard to serve.
// Rejects bytes over max_bytes(mime). Thread-safe.
void publish(Mime mime, const std::string &bytes);

// Outbound: called from Sunshine HTTP handler threads. Always writes the
// current sequence number into out_seq. When out_seq > since, also writes
// the stored mime into out_mime and bytes into out_bytes and returns true;
// otherwise leaves out_mime/out_bytes untouched and returns false.
// Thread-safe.
bool fetch(std::uint64_t since, std::string &out_bytes, Mime &out_mime, std::uint64_t &out_seq);

// Outbound long-poll: called from Sunshine HTTP handler threads for GET
// /cosmic/clipboard?wait=1. If data is already available (same rules as
// fetch(), including the stale-cursor fallback), returns true immediately
// WITHOUT invoking cb -- the caller responds inline. Otherwise parks cb
// until either publish() delivers newer data or hold_ms elapses (checked by
// tick()), and returns false. cb is invoked at most once, from whichever of
// publish()/tick()/clear_waiters() first claims this waiter, and never
// while any internal lock is held. Thread-safe.
bool fetch_or_park(std::uint64_t since, std::uint64_t hold_ms, WaitCallback cb);

// Outbound long-poll: called once per frame from the app main thread to
// time out any parked fetch_or_park() waiter whose deadline has passed.
// Thread-safe.
void tick();

// Outbound long-poll: resolves every currently parked fetch_or_park()
// waiter with WaitResult::Unavailable. Called when clipboard sharing is
// disabled or the HTTPS server is shutting down, so no waiter is left
// parked past the point it can ever be serviced. Thread-safe.
void clear_waiters();

// Owner: the SHA-256 fingerprint of the TLS client certificate that most
// recently started or resumed a stream session; empty means "no owner
// recorded". Written from Sunshine HTTP handler threads and the
// stream-teardown path, read on every clipboard request. Thread-safe.
void set_owner(const std::string &fingerprint);

// Owner: clears the recorded fingerprint (equivalent to
// set_owner("")). Thread-safe.
void clear_owner();

// Owner: returns true only when fingerprint is non-empty, an owner is
// currently recorded, and the two compare equal. Fails closed (returns
// false) when there is no owner or when fingerprint is empty. Thread-safe.
bool is_owner(const std::string &fingerprint);

// Inbound: called from Sunshine HTTP handler threads when a client POSTs a
// clipboard update. Overwrites the single pending slot (last-write-wins).
// Rejects bytes over max_bytes(mime). Thread-safe.
void push_incoming(Mime mime, std::string bytes);

// Inbound: called from the app main thread each frame to drain a pending
// update, if any, into out_bytes/out_mime. Returns false when nothing is
// pending. Thread-safe.
bool take_incoming(std::string &out_bytes, Mime &out_mime);

}  // namespace cosmic::clipboard
