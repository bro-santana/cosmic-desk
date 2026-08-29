// Cosmic Desk — host presence polling (docs/UI_MIGRATION.md U6).
//
// A single background worker polls GET http://<address>:<port>/serverinfo for
// every saved host every ~10 s (2 s timeout) so the Bridge cards can show the
// design's LINK READY state. Results (reachability plus the host's advertised
// wallpaper hash, if any) are cached in a mutex-protected map and read from
// the main thread; the worker never touches ImGui/SDL.

#include "app/presence.h"

#include "app/wallcache.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace cosmic::presence {

namespace {

// Poll cadence and per-request timeouts (docs/UI_MIGRATION.md U6).
constexpr int kPollIntervalMs = 10000;  // ~10 s between full passes
constexpr int kConnectTimeoutMs = 1500;
constexpr int kTimeoutMs = 2000;
// Sleep granularity while waiting out the poll interval, so stop() can join
// promptly instead of blocking for the whole interval.
constexpr int kSleepStepMs = 100;

// curl_global_init is reference-counted, so a second call is safe even if the
// vendored libgamestream already inited curl; still guard with a flag so we
// only ever call it once from here.
std::atomic<bool> g_curl_inited{false};

std::thread g_worker;
std::atomic<bool> g_running{false};

// Guarded by g_mutex.
std::mutex g_mutex;
std::map<std::string, int> g_poll_targets;         // address -> port
std::map<std::string, HostPresence> g_results;     // address -> presence

// /serverinfo XML is a couple of KB; cap the accumulated body so a hostile or
// broken host can't make the worker buffer an unbounded response.
constexpr size_t kMaxBodyBytes = 64 * 1024;

// Appends the response body into the std::string passed via CURLOPT_WRITEDATA,
// stopping once kMaxBodyBytes is reached. Always returns the full byte count
// (even once capped) so curl does not treat the truncation as a write error.
size_t AppendBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    std::string* body = static_cast<std::string*>(userdata);
    // This callback runs inside libcurl (prebuilt C, no unwinding contract),
    // so a thrown bad_alloc from append() must never escape this frame;
    // swallow it and fall back to an empty/partial body (hash == "").
    try {
        if (body->size() < kMaxBodyBytes) {
            const size_t room = kMaxBodyBytes - body->size();
            body->append(ptr, std::min(bytes, room));
        }
    } catch (...) {
    }
    return bytes;
}

// Extracts the wallpaper hash from a /serverinfo body by a plain string scan
// (no XML parser, no regex — the body is untrusted and this keeps parsing
// trivially bounded). The value ends up in a filename on the client, so
// anything that is not exactly 64 lowercase hex characters is rejected.
std::string ExtractWallpaperHash(const std::string& body) {
    static constexpr char kOpenTag[] = "<CosmicWallpaperHash>";
    static constexpr char kCloseTag[] = "</CosmicWallpaperHash>";
    const size_t open = body.find(kOpenTag);
    if (open == std::string::npos) {
        return "";
    }
    const size_t value_start = open + sizeof(kOpenTag) - 1;
    const size_t close = body.find(kCloseTag, value_start);
    if (close == std::string::npos) {
        return "";
    }
    const std::string value = body.substr(value_start, close - value_start);
    constexpr size_t kHashLen = 64;
    if (value.size() != kHashLen) {
        return "";
    }
    for (char c : value) {
        const bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) {
            return "";
        }
    }
    return value;
}

void WorkerLoop() {
    // The easy handle is created once and reused across polls.
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fprintf(stderr, "presence: curl_easy_init failed; polling disabled.\n");
        return;
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // GET (not HEAD — the host may reject HEAD) with a write callback that
    // captures the body so the advertised wallpaper hash can be extracted.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendBody);

    while (g_running.load(std::memory_order_relaxed)) {
        try {
            // Copy the target set under the lock so the main thread can swap it
            // freely while we poll.
            std::map<std::string, int> targets;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                targets = g_poll_targets;
            }

            // Fed to wallcache::sync() once this pass finishes; built here
            // rather than re-read from g_results so it reflects exactly this
            // pass's addresses/ports/hashes, not the accumulated snapshot.
            std::vector<wallcache::Target> wallpaper_targets;
            wallpaper_targets.reserve(targets.size());

            for (const auto& [address, port] : targets) {
                if (!g_running.load(std::memory_order_relaxed)) {
                    break;
                }
                // IPv6 literals must be bracketed in a URL; IPv4/hostnames are
                // not. A user-supplied bracketed form is kept as-is.
                const bool ipv6 = address.find(':') != std::string::npos &&
                                  address.front() != '[';
                const std::string host = ipv6 ? "[" + address + "]" : address;
                const std::string url = "http://" + host + ":" + std::to_string(port) +
                                        "/serverinfo";
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                // A fresh buffer per target guarantees a failed or partial
                // poll can never leak the previous host's body into this
                // host's result.
                std::string body;
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
                const CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                // Reachable = the request completed and the host answered 200.
                // Timeouts and connection failures (res != CURLE_OK) are offline.
                const bool online = (res == CURLE_OK && http_code == 200);
                const std::string hash = online ? ExtractWallpaperHash(body) : "";
                wallpaper_targets.push_back(wallcache::Target{address, port, hash});
                std::lock_guard<std::mutex> lock(g_mutex);
                g_results[address] = HostPresence{online, hash};
            }

            // One wallcache download attempt per pass (wallcache.h), on the
            // presence worker thread only — see that header for why it is
            // not safe to call from anywhere else. Skipped once stop() has
            // already been requested: sync() can block for its ~35 s curl
            // timeout budget, and there is no point starting a download that
            // stop()'s wallcache::cancel() call would immediately abort.
            if (g_running.load(std::memory_order_relaxed)) {
                wallcache::sync(wallpaper_targets);
            }

            // Sleep out the poll interval in small increments so stop() can join
            // promptly rather than blocking for the whole interval.
            for (int waited = 0;
                 waited < kPollIntervalMs && g_running.load(std::memory_order_relaxed);
                 waited += kSleepStepMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
            }
        } catch (...) {
            // The worker must never throw out of the thread; log once and keep
            // polling (a transient failure must not kill presence updates).
            std::fprintf(stderr, "presence: worker iteration failed; continuing.\n");
        }
    }

    curl_easy_cleanup(curl);
}

}  // namespace

void start(const std::vector<std::pair<std::string, int>>& hosts) {
    if (!g_curl_inited.exchange(true)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_poll_targets.clear();
        for (const auto& [address, port] : hosts) {
            g_poll_targets[address] = port;
        }
        // Prune results for hosts no longer polled, so a re-added address
        // never reports a stale reachability for up to a poll pass.
        for (auto it = g_results.begin(); it != g_results.end();) {
            if (g_poll_targets.find(it->first) == g_poll_targets.end()) {
                it = g_results.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Spawn the worker once; if it is already alive, the target swap above is
    // all that is needed (the loop picks up the new set next pass).
    if (!g_worker.joinable()) {
        // Clears a cancel() left over from a previous stop(), only on the
        // path that actually spawns a fresh worker — see wallcache.h.
        wallcache::reset();
        g_running.store(true, std::memory_order_relaxed);
        g_worker = std::thread(WorkerLoop);
    }
}

void stop() {
    g_running.store(false, std::memory_order_relaxed);
    // Unblocks a wallcache download that may already be in flight so the
    // join() below cannot stall for its ~35 s curl timeout budget — without
    // this, an immediate relaunch (main.cpp calls stop() after destroying the
    // window and before releasing the single-instance lock) could fail.
    wallcache::cancel();
    if (g_worker.joinable()) {
        g_worker.join();
    }
    // Reset so a later start() can spawn a fresh worker, and clear the cached
    // state so a restart does not report stale results.
    g_running.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_poll_targets.clear();
    g_results.clear();
}

std::map<std::string, HostPresence> snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_results;
}

}  // namespace cosmic::presence
