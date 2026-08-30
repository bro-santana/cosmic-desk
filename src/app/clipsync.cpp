// Cosmic Desk — client-side clipboard sync worker (see clipsync.h).
//
// Modeled closely on wallcache.cpp: its own curl easy handle (created and
// torn down on the worker thread, never the vendored libgamestream global),
// the same client-cert trust model and timeouts, and the same
// XFERINFOFUNCTION progress guard so stop() can abort an in-flight transfer
// instead of blocking for its timeout budget while joining.

#include "app/clipsync.h"

#include "app/settings.h"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cosmic::clipsync {

namespace {

// Poll cadence, and the slice size the loop sleeps in so stop() can join
// promptly instead of blocking for up to a full pass.
constexpr int kPollIntervalMs = 1000;
constexpr int kSleepStepMs = 50;

std::thread g_worker;

// Set by stop(), cleared by start() before a fresh worker is spawned. Serves
// two purposes: the worker loop's continue condition, and (via
// ProgressCallback below) the signal an in-flight curl transfer checks to
// abort early instead of running out its connect/timeout budget.
std::atomic<bool> g_cancel_requested{false};

// Guards g_outgoing and g_incoming, the two last-write-wins exchange slots
// with the main thread (see clipsync.h). g_outgoing is written by
// publish_local() and drained by the worker; g_incoming is written by the
// worker and drained by take_incoming().
std::mutex g_mutex;

struct PendingText {
    std::string text;
    bool pending = false;
};
PendingText g_outgoing;
PendingText g_incoming;

// Result of scanning GET response headers for X-Cosmic-Clipboard-Seq. A body
// is never trusted without a well-formed seq header — see clipsync.h.
struct HeaderState {
    bool valid = false;
    unsigned long long seq = 0;
};

// Checked by curl during a transfer (CURLOPT_XFERINFOFUNCTION below); a
// non-zero return aborts the transfer with CURLE_ABORTED_BY_CALLBACK, so
// stop() does not have to wait out the connect/transfer timeout budget.
int ProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_cancel_requested.load(std::memory_order_relaxed) ? 1 : 0;
}

// Appends the response body, aborting the transfer once it exceeds kMaxBytes
// by returning fewer bytes than were provided (libcurl treats a short count
// as a write error and aborts with CURLE_WRITE_ERROR) — an overrun body is
// discarded entirely, never truncated, since the caller only looks at `body`
// after confirming curl_easy_perform succeeded. Runs inside libcurl's C
// frames, so no exception may escape this function.
size_t WriteCappedBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    std::string* body = static_cast<std::string*>(userdata);
    try {
        body->append(ptr, bytes);
        if (body->size() > kMaxBytes) {
            return 0;
        }
    } catch (...) {
        return 0;
    }
    return bytes;
}

// Case-insensitive match of `line`'s leading bytes against `name`.
bool StartsWithHeaderName(const std::string& line, const char* name, size_t name_len) {
    if (line.size() < name_len) {
        return false;
    }
    for (size_t i = 0; i < name_len; ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

// Scans one response header line for X-Cosmic-Clipboard-Seq and, if found and
// its value parses cleanly as an unsigned integer, records it in *state. An
// unauthorized or stock Sunshine host answers with a 200-status,
// 404-shaped XML body (same trick wallcache.cpp's LooksLikeImage guards
// against for the wallpaper route), so this header is the actual gate — the
// caller must not trust a GET body without state->valid being true. Runs
// inside libcurl's C frames, so no exception may escape this function.
size_t CaptureSeqHeader(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t bytes = size * nitems;
    HeaderState* state = static_cast<HeaderState*>(userdata);
    try {
        static constexpr char kHeaderName[] = "X-Cosmic-Clipboard-Seq:";
        static constexpr size_t kHeaderNameLen = sizeof(kHeaderName) - 1;

        const std::string line(buffer, bytes);
        if (!StartsWithHeaderName(line, kHeaderName, kHeaderNameLen)) {
            return bytes;
        }

        std::string value = line.substr(kHeaderNameLen);
        const size_t start = value.find_first_not_of(" \t");
        const size_t end = value.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos || start > end) {
            return bytes;  // no non-whitespace value: leave state untouched
        }
        value = value.substr(start, end - start + 1);
        for (char c : value) {
            if (c < '0' || c > '9') {
                return bytes;  // not a well-formed unsigned integer
            }
        }

        errno = 0;
        char* end_ptr = nullptr;
        const unsigned long long parsed = std::strtoull(value.c_str(), &end_ptr, 10);
        if (errno == ERANGE || end_ptr == value.c_str()) {
            return bytes;
        }
        state->seq = parsed;
        state->valid = true;
    } catch (...) {
        return 0;
    }
    return bytes;
}

// Configures the easy handle exactly like wallcache.cpp's EnsureCurl
// (wallcache.cpp:172-209): same self-signed-host trust model (client cert +
// key from Settings::config_dir()/"client", no peer/host verification — this
// mirrors third-party/libgamestream/http.c's Moonlight pairing trust model),
// same timeouts, and the same progress-callback cancel hook. Does NOT call
// curl_global_init: presence.cpp:189 owns that call, it is reference-counted,
// and it always runs before any session (and therefore this worker) can
// start.
void ConfigureCurl(CURL* curl) {
    const std::filesystem::path client_dir = cosmic::Settings::config_dir() / "client";
    // libcurl copies string options like CURLOPT_SSLCERT/CURLOPT_SSLKEY since
    // 7.17.0, so these locals do not need to outlive this call.
    const std::string cert_path = (client_dir / "client.pem").string();
    const std::string key_path = (client_dir / "key.pem").string();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLCERT, cert_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLKEY, key_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Lets stop() abort an in-flight transfer instead of blocking on its full
    // timeout budget while joining the worker.
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

// POSTs `text` as the request body of /cosmic/clipboard. The response body is
// irrelevant (host -> client text only ever arrives via the GET poll below);
// the write callback and cap here only exist so curl does not dump the
// response to stdout. A failed POST is dropped silently and not retried
// until the next locally-copied text arrives — POSTing takes priority over
// polling, so a persistently failing host is retried at most once a second.
void PostText(CURL* curl, const std::string& post_url, const std::string& text) {
    std::string discard;
    curl_easy_setopt(curl, CURLOPT_URL, post_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, text.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(text.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCappedBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);
    curl_easy_perform(curl);
}

// GETs /cosmic/clipboard?since=<last_seq> and, if the response passes every
// check described in clipsync.h, updates `last_seq` and/or the incoming
// buffer. A failed request, a missing/malformed seq header, a 404, or any
// status other than 200/204 changes nothing and logs nothing — a host with
// clipboard sharing off answers 404 every poll and must not produce log spam.
// CURLOPT_HTTPGET/POSTFIELDS/POSTFIELDSIZE are reset explicitly so a
// preceding PostText() call on this same handle cannot leak into this GET.
// CURLOPT_POSTFIELDS switches the handle's method to POST even when set to
// nullptr, so CURLOPT_HTTPGET must be set last among these three calls to
// actually select GET (per curl's docs, switching back to GET after
// CURLOPT_POST requires re-enabling CURLOPT_HTTPGET).
void PollOnce(CURL* curl, const std::string& get_base_url, unsigned long long& last_seq) {
    const std::string get_url = get_base_url + "?since=" + std::to_string(last_seq);

    std::string body;
    HeaderState header_state;
    curl_easy_setopt(curl, CURLOPT_URL, get_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCappedBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CaptureSeqHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_state);

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK || !header_state.valid) {
        return;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code == 200 && header_state.seq > last_seq) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_incoming.text = std::move(body);
        g_incoming.pending = true;
        last_seq = header_state.seq;
    } else if (http_code == 204) {
        last_seq = header_state.seq;
    }
    // 200 with a non-advancing seq, 404, and any other status: change nothing.
}

// The worker thread body. `host_address` and `https_port` are captured by
// value at spawn time (see start()), so no shared global is needed for them.
void WorkerLoop(std::string host_address, int https_port) {
    // The worker must never throw out of the thread; a transient failure
    // (e.g. a temporary curl init hiccup) must not crash the process.
    try {
        // Owns the handle for the rest of this scope: an exception thrown
        // anywhere below (e.g. std::bad_alloc building a URL string) must
        // still release it instead of leaking it plus its socket and TLS
        // session. Unlike wallcache.cpp's process-lifetime global handle,
        // this one is created and torn down on every session start/stop
        // cycle, so a leak here accumulates across sessions.
        const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl_owner(
            curl_easy_init(), &curl_easy_cleanup);
        CURL* curl = curl_owner.get();
        if (curl == nullptr) {
            std::fprintf(stderr, "clipsync: curl_easy_init failed; sync disabled.\n");
            return;
        }
        ConfigureCurl(curl);

        // IPv6 literals must be bracketed in a URL; IPv4/hostnames are not
        // (mirrors wallcache.cpp:256-258). A user-supplied bracketed form is
        // kept as-is.
        const bool ipv6 = host_address.find(':') != std::string::npos &&
                          host_address.front() != '[';
        const std::string host = ipv6 ? "[" + host_address + "]" : host_address;
        const std::string base_url =
            "https://" + host + ":" + std::to_string(https_port) + "/cosmic/clipboard";

        // Local to this call, so every fresh worker (every start()) begins
        // at 0 — a new session never resumes another host's sequence.
        unsigned long long last_seq = 0;

        while (!g_cancel_requested.load(std::memory_order_relaxed)) {
            std::string outgoing_text;
            bool has_outgoing = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_outgoing.pending) {
                    outgoing_text = std::move(g_outgoing.text);
                    g_outgoing.text.clear();
                    g_outgoing.pending = false;
                    has_outgoing = true;
                }
            }
            // A pending POST is sent before this pass's poll, so a fresh
            // local copy reaches the host without waiting behind a GET.
            if (has_outgoing) {
                PostText(curl, base_url, outgoing_text);
            }

            if (!g_cancel_requested.load(std::memory_order_relaxed)) {
                PollOnce(curl, base_url, last_seq);
            }

            for (int waited = 0;
                 waited < kPollIntervalMs && !g_cancel_requested.load(std::memory_order_relaxed);
                 waited += kSleepStepMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
            }
        }

        // curl_owner's destructor releases the handle here (and on every
        // other exit path from this try block, including an exception).
    } catch (...) {
    }
}

}  // namespace

void start(const std::string& host_address, int https_port) {
    if (g_worker.joinable()) {
        return;  // already running
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_outgoing = PendingText{};
        g_incoming = PendingText{};
    }
    g_cancel_requested.store(false, std::memory_order_relaxed);
    g_worker = std::thread(WorkerLoop, host_address, https_port);
}

void stop() {
    g_cancel_requested.store(true, std::memory_order_relaxed);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

void publish_local(const std::string& text) {
    if (text.size() > kMaxBytes) {
        return;  // refuse oversize text, matching the host's own POST cap
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_outgoing.text = text;
    g_outgoing.pending = true;
}

bool take_incoming(std::string& out_text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_incoming.pending) {
        return false;
    }
    out_text = std::move(g_incoming.text);
    g_incoming.text.clear();
    g_incoming.pending = false;
    return true;
}

}  // namespace cosmic::clipsync
