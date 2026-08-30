// Cosmic Desk — clipboard implementation. See clipboard.h for the contract.

#include "hostglue/clipboard.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace cosmic::clipboard {
namespace {

std::atomic<bool> g_enabled {true};

// Guards both the outbound store and the inbound slot below. fetch() and
// push_incoming() are called concurrently from Sunshine's HTTP server
// threads; publish() and take_incoming() are called from the app main thread.
std::mutex g_mutex;

// Outbound: the local clipboard text last published, and a sequence number
// bumped on every distinct publish so fetch() can tell a client whether it
// already has the latest value.
std::uint64_t g_seq = 0;
std::string g_text;

// Inbound: a single pending update from a client. A clipboard has no queue
// semantics (only the latest value matters), and a single slot bounds memory
// against a client that POSTs faster than the main loop drains.
bool g_has_incoming = false;
std::string g_incoming;

}  // namespace

void set_enabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

bool enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

// NOTE: enabled() is deliberately not checked here (or in fetch()/
// push_incoming() below) -- the /cosmic/clipboard routes in nvhttp.cpp own
// the enable/session gate. Keeping the store unconditional means toggling the
// setting back on does not resurrect a stale value with a stale sequence
// number: the store simply keeps tracking the real clipboard the whole time.
void publish(const std::string &text) {
  if (text.size() > kMaxBytes) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (text == g_text) {
    return;
  }
  g_text = text;
  g_seq++;
}

bool fetch(std::uint64_t since, std::string &out_text, std::uint64_t &out_seq) {
  std::lock_guard<std::mutex> lock(g_mutex);
  out_seq = g_seq;
  if (g_seq > since) {
    out_text = g_text;
    return true;
  }
  return false;
}

void push_incoming(std::string text) {
  if (text.size() > kMaxBytes) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_incoming = std::move(text);
  g_has_incoming = true;
}

bool take_incoming(std::string &out_text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_has_incoming) {
    return false;
  }
  out_text = std::move(g_incoming);
  g_incoming.clear();
  g_has_incoming = false;
  return true;
}

}  // namespace cosmic::clipboard
