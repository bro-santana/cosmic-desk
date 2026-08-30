// Cosmic Desk — bidirectional text clipboard sync bridge between the app main
// thread (which owns the SDL clipboard) and the vendored Sunshine host's
// GET/POST /cosmic/clipboard route (host/sunshine/src/nvhttp.cpp). The
// outbound store holds the local clipboard text the route serves to a
// connected client; the inbound queue holds text POSTed by a client for the
// main thread to apply to the local clipboard.
//
// This header is deliberately free of vendored Sunshine includes so nvhttp.cpp
// can include it without dragging the host's dependency graph into the app;
// this bridge needs none (same pattern as wallpaper.h/pin_bridge.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cosmic::clipboard {

// Shared cap on clipboard text size, enforced by every producer (publish(),
// push_incoming()) and consumer of this store.
inline constexpr std::size_t kMaxBytes = 1024 * 1024;

// Host-side opt-out (settings' share_clipboard). Defaults to true. Thread-safe.
// NOTE: not consulted by publish()/fetch()/push_incoming() below -- the
// /cosmic/clipboard routes own the enable/session gate; see clipboard.cpp.
void set_enabled(bool enabled);
bool enabled();

// Outbound: called from the app main thread whenever the local SDL clipboard
// changes. Stores the text for GET /cosmic/clipboard to serve. Thread-safe.
void publish(const std::string &text);

// Outbound: called from Sunshine HTTP handler threads. Always writes the
// current sequence number into out_seq. When out_seq > since, also writes the
// stored text into out_text and returns true; otherwise leaves out_text
// untouched and returns false. Thread-safe.
bool fetch(std::uint64_t since, std::string &out_text, std::uint64_t &out_seq);

// Inbound: called from Sunshine HTTP handler threads when a client POSTs a
// clipboard update. Overwrites the single pending slot (last-write-wins).
// Thread-safe.
void push_incoming(std::string text);

// Inbound: called from the app main thread each frame to drain a pending
// update, if any, into out_text. Returns false when nothing is pending.
// Thread-safe.
bool take_incoming(std::string &out_text);

}  // namespace cosmic::clipboard
