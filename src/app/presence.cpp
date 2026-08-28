// Cosmic Desk — host presence polling (docs/UI_MIGRATION.md U6).
//
// A single background worker polls GET http://<address>:<port>/serverinfo for
// every saved host every ~10 s (2 s timeout) so the Bridge cards can show the
// design's LINK READY state. Results are cached in a mutex-protected map and
// read from the main thread; the worker never touches ImGui/SDL.

#include "app/presence.h"

#include <curl/curl.h>

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
std::map<std::string, int> g_poll_targets;   // address -> port
std::map<std::string, bool> g_results;       // address -> reachable

// Discards the response body (the host answers /serverinfo with an XML body;
// only the HTTP result matters). Returns the size so curl keeps reading.
size_t DiscardBody(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/) {
    return size * nmemb;
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
    // discards the body.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardBody);

    while (g_running.load(std::memory_order_relaxed)) {
        try {
            // Copy the target set under the lock so the main thread can swap it
            // freely while we poll.
            std::map<std::string, int> targets;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                targets = g_poll_targets;
            }

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
                const CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                // Reachable = the request completed and the host answered 200.
                // Timeouts and connection failures (res != CURLE_OK) are offline.
                const bool online = (res == CURLE_OK && http_code == 200);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_results[address] = online;
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
        g_running.store(true, std::memory_order_relaxed);
        g_worker = std::thread(WorkerLoop);
    }
}

void stop() {
    g_running.store(false, std::memory_order_relaxed);
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

std::map<std::string, bool> snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_results;
}

}  // namespace cosmic::presence
