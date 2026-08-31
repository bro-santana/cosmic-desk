// Cosmic Desk — clipboard implementation. See clipboard.h for the contract.

#include "hostglue/clipboard.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cosmic::clipboard {

const char *content_type(Mime mime) {
  switch (mime) {
    case Mime::Png:
      return "image/png";
    case Mime::Text:
    default:
      return "text/plain; charset=utf-8";
  }
}

std::size_t max_bytes(Mime mime) {
  return mime == Mime::Png ? kMaxImageBytes : kMaxBytes;
}

namespace {

std::atomic<bool> g_enabled {true};

// Guards both the outbound store and the inbound slot below. fetch() and
// push_incoming() are called concurrently from Sunshine's HTTP server
// threads; publish() and take_incoming() are called from the app main thread.
std::mutex g_mutex;

// Outbound: the local clipboard bytes (and their mime) last published, and a
// sequence number bumped on every distinct publish so fetch() can tell a
// client whether it already has the latest value.
std::uint64_t g_seq = 0;
std::string g_bytes;
Mime g_mime = Mime::Text;

// Outbound long-poll: a fetch_or_park() call parked because no data was
// immediately available. Resolved by whichever of publish()/tick()/
// clear_waiters() first claims it -- never while g_mutex is held, since the
// callback runs arbitrary asio code through the Response it captures.
struct Waiter {
  std::uint64_t since;
  std::chrono::steady_clock::time_point deadline;
  WaitCallback cb;
};
// Unbounded: a cap is future hardening, accepted for now because this route
// sits behind client-certificate verification (see cosmic_clipboard_get).
std::vector<Waiter> g_waiters;

// Inbound: a single pending update from a client. A clipboard has no queue
// semantics (only the latest value matters), and a single slot bounds memory
// against a client that POSTs faster than the main loop drains.
bool g_has_incoming = false;
std::string g_incoming;
Mime g_incoming_mime = Mime::Text;

// Owner: the SHA-256 fingerprint of the TLS client certificate that most
// recently started or resumed a stream session. Guarded by g_mutex; empty
// means "no owner", so is_owner() fails closed.
std::string g_owner;

}  // namespace

void set_enabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    clear_waiters();
  }
}

bool enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

// NOTE: enabled() is deliberately not checked here (or in fetch()/
// push_incoming() below) -- the /cosmic/clipboard routes in nvhttp.cpp own
// the enable/session gate. Keeping the store unconditional means toggling the
// setting back on does not resurrect a stale value with a stale sequence
// number: the store simply keeps tracking the real clipboard the whole time.
void publish(Mime mime, const std::string &bytes) {
  if (bytes.size() > max_bytes(mime)) {
    return;
  }
  std::vector<Waiter> ready;
  std::uint64_t seq_snapshot = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (mime == g_mime && bytes == g_bytes) {
      return;
    }
    g_mime = mime;
    g_bytes = bytes;
    g_seq++;
    seq_snapshot = g_seq;

    // g_seq was just incremented above, so g_seq > 0 always holds here --
    // which makes `since != g_seq` exactly fetch_or_park's availability test
    // (`g_seq > since || (g_seq < since && g_seq > 0)`) re-applied to the
    // new sequence. In particular this also wakes a waiter parked while
    // g_seq == 0 with a stale post-restart cursor (e.g. since == 5): after
    // this bump g_seq == 1, `5 < 1` would miss it, but `5 != 1` does not.
    // Do not "simplify" this back to `<` -- it would silently reintroduce
    // that starvation bug.
    auto it = g_waiters.begin();
    while (it != g_waiters.end()) {
      if (it->since != g_seq) {
        ready.push_back(std::move(*it));
        it = g_waiters.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Invoked outside g_mutex: a WaitCallback runs arbitrary asio code through
  // the Response it captures, and must never run while the lock is held.
  for (auto &waiter : ready) {
    waiter.cb(WaitResult::Changed, seq_snapshot, mime, bytes);
  }
  ready.clear();
}

bool fetch(std::uint64_t since, std::string &out_bytes, Mime &out_mime, std::uint64_t &out_seq) {
  std::lock_guard<std::mutex> lock(g_mutex);
  out_seq = g_seq;
  if (g_seq > since) {
    out_bytes = g_bytes;
    out_mime = g_mime;
    return true;
  }
  return false;
}

bool fetch_or_park(std::uint64_t since, std::uint64_t hold_ms, WaitCallback cb) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_seq > since) {
    return true;
  }
  // cosmic::clipboard's sequence counter is process-local and resets to 0
  // across a host restart. A client that carries a "since" value from
  // before the restart then has since > the (now lower) current seq, and
  // would otherwise wait forever for a change that already happened. Treat
  // a client cursor ahead of the store as stale rather than starving it:
  // report the current text as available, but only when the store has
  // actually been published to (g_seq > 0). An unpublished store (g_seq ==
  // 0) has nothing to serve, so it falls through to the park path below.
  // Mirrored by the immediate (non-wait) stale-cursor check in
  // host/sunshine/src/nvhttp.cpp's cosmic_clipboard_get -- keep both
  // predicates in sync.
  if (g_seq < since && g_seq > 0) {
    return true;
  }
  g_waiters.push_back(Waiter {
    since,
    std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms),
    std::move(cb),
  });
  return false;
}

void tick() {
  std::vector<Waiter> expired;
  std::uint64_t seq_snapshot = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    seq_snapshot = g_seq;
    auto now = std::chrono::steady_clock::now();
    auto it = g_waiters.begin();
    while (it != g_waiters.end()) {
      if (it->deadline <= now) {
        expired.push_back(std::move(*it));
        it = g_waiters.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto &waiter : expired) {
    waiter.cb(WaitResult::Timeout, seq_snapshot, Mime::Text, {});
  }
  expired.clear();
}

void clear_waiters() {
  std::vector<Waiter> waiters;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    waiters = std::move(g_waiters);
    g_waiters.clear();
  }
  for (auto &waiter : waiters) {
    waiter.cb(WaitResult::Unavailable, 0, Mime::Text, {});
  }
  waiters.clear();
}

void set_owner(const std::string &fingerprint) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_owner = fingerprint;
}

void clear_owner() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_owner.clear();
}

bool is_owner(const std::string &fingerprint) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (fingerprint.empty()) {
    return false;
  }
  return !g_owner.empty() && g_owner == fingerprint;
}

void push_incoming(Mime mime, std::string bytes) {
  if (bytes.size() > max_bytes(mime)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_incoming = std::move(bytes);
  g_incoming_mime = mime;
  g_has_incoming = true;
}

bool take_incoming(std::string &out_bytes, Mime &out_mime) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_has_incoming) {
    return false;
  }
  out_bytes = std::move(g_incoming);
  out_mime = g_incoming_mime;
  g_incoming.clear();
  g_has_incoming = false;
  return true;
}

}  // namespace cosmic::clipboard
