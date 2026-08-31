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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// Ceiling for the exponential backoff WorkerLoop applies after consecutive
// PollOnce failures (unreachable host, TLS error, sharing off, etc.) — keeps
// a persistently failing host from being hammered every second forever while
// still checking back within half a minute.
constexpr int kMaxPollIntervalMs = 30000;
constexpr int kSleepStepMs = 50;

std::thread g_worker;

// Set by stop(), cleared by start() before a fresh worker is spawned. Serves
// two purposes: the worker loop's continue condition, and (via
// ProgressCallback below) the signal an in-flight curl transfer checks to
// abort early instead of running out its connect/timeout budget.
std::atomic<bool> g_cancel_requested{false};

// Set by publish_local() whenever the outgoing slot just became non-empty,
// cleared once per WorkerLoop iteration (before the slot is drained — see the
// comment there). Means "abandon any parked GET": with wait=1 the GET can
// hold for up to 20 s, and without this a fresh local copy would otherwise
// wait behind that hold instead of reaching the host promptly.
std::atomic<bool> g_wake_requested{false};

// Guards g_outgoing and g_incoming, the two last-write-wins exchange slots
// with the main thread (see clipsync.h). g_outgoing is written by
// publish_local() and drained by the worker; g_incoming is written by the
// worker and drained by take_incoming().
std::mutex g_mutex;

struct PendingPayload {
    std::string bytes;
    Mime mime = Mime::Text;
    bool pending = false;
};
PendingPayload g_outgoing;
PendingPayload g_incoming;

// Result of scanning GET response headers for X-Cosmic-Clipboard-Seq,
// Content-Type and X-Cosmic-Clipboard-Version. A body is never trusted
// without a well-formed seq header — see clipsync.h.
struct HeaderState {
    bool valid = false;
    unsigned long long seq = 0;
    // Content-Type, classified by media type only (parameters such as
    // "; charset=utf-8" and case are ignored, mirroring the host's own
    // classification in nvhttp.cpp's cosmic_clipboard_post). Defaults to,
    // and an unrecognised value is recorded as, ContentType::Unknown rather
    // than defaulting to text, so PollOnce can fail closed instead of
    // storing a body under the wrong (or a missing) mime.
    enum class ContentType { Text, Png, Unknown } content_type = ContentType::Unknown;
    // True once a well-formed X-Cosmic-Clipboard-Version header carrying an
    // integer >= 5 has been seen on this response.
    bool version_at_least_5 = false;
};

// Checked by curl during a transfer (CURLOPT_XFERINFOFUNCTION below); a
// non-zero return aborts the transfer with CURLE_ABORTED_BY_CALLBACK, so
// stop() does not have to wait out the connect/transfer timeout budget. Also
// aborts a parked wait=1 GET as soon as a local copy needs to go out, so it
// does not sit behind up to 20 s of long-poll hold (see PollOnce).
int ProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return (g_cancel_requested.load(std::memory_order_relaxed) ||
            g_wake_requested.load(std::memory_order_relaxed))
               ? 1
               : 0;
}

// Appends the response body, aborting the transfer once it exceeds
// kMaxImageBytes (the larger of the two caps: a GET body may legitimately be
// a PNG now) by returning fewer bytes than were provided (libcurl treats a
// short count as a write error and aborts with CURLE_WRITE_ERROR) — an
// overrun body is discarded entirely, never truncated, since the caller only
// looks at `body` after confirming curl_easy_perform succeeded. This is
// shared by the GET and the POST's (discarded) response body, so a
// Mime::Text body still needs its own kMaxBytes re-check once the mime is
// known — see PollOnce. Runs inside libcurl's C frames, so no exception may
// escape this function.
size_t WriteCappedBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    std::string* body = static_cast<std::string*>(userdata);
    try {
        body->append(ptr, bytes);
        if (body->size() > kMaxImageBytes) {
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

// Case-insensitive exact match, used to classify a Content-Type header's
// media type once any ";..." parameter and surrounding whitespace have
// already been stripped by the caller.
bool EqualsIgnoreCase(const std::string& value, const char* other) {
    const size_t other_len = std::strlen(other);
    if (value.size() != other_len) {
        return false;
    }
    return StartsWithHeaderName(value, other, other_len);
}

// Strips leading/trailing spaces, tabs and (for the trailing end) \r\n from a
// header value substring. Returns an empty string when the value is entirely
// whitespace, which every caller below treats as "no value present".
std::string TrimHeaderValue(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t");
    const size_t end = value.find_last_not_of(" \t\r\n");
    if (start == std::string::npos || end == std::string::npos || start > end) {
        return {};
    }
    return value.substr(start, end - start + 1);
}

// Parses `value` as a well-formed (digits-only, in-range) unsigned integer
// into *out, leaving *out untouched and returning false otherwise.
bool ParseUnsignedInteger(const std::string& value, unsigned long long* out) {
    if (value.empty()) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;  // not a well-formed unsigned integer
        }
    }
    errno = 0;
    char* end_ptr = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end_ptr, 10);
    if (errno == ERANGE || end_ptr == value.c_str()) {
        return false;
    }
    *out = parsed;
    return true;
}

// Scans one response header line for X-Cosmic-Clipboard-Seq,
// X-Cosmic-Clipboard-Version and Content-Type, recording each into *state
// when well-formed. An unauthorized or stock Sunshine host answers with a
// 200-status, 404-shaped XML body (same trick wallcache.cpp's LooksLikeImage
// guards against for the wallpaper route), so the seq header remains the
// actual gate — the caller must not trust a GET body without state->valid
// being true, regardless of what this function fills in for the other two.
// Runs inside libcurl's C frames, so no exception may escape this function.
size_t CaptureHeaders(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t bytes = size * nitems;
    HeaderState* state = static_cast<HeaderState*>(userdata);
    try {
        const std::string line(buffer, bytes);

        static constexpr char kSeqName[] = "X-Cosmic-Clipboard-Seq:";
        static constexpr size_t kSeqNameLen = sizeof(kSeqName) - 1;
        if (StartsWithHeaderName(line, kSeqName, kSeqNameLen)) {
            unsigned long long parsed = 0;
            if (ParseUnsignedInteger(TrimHeaderValue(line.substr(kSeqNameLen)), &parsed)) {
                state->seq = parsed;
                state->valid = true;
            }
            return bytes;
        }

        static constexpr char kVersionName[] = "X-Cosmic-Clipboard-Version:";
        static constexpr size_t kVersionNameLen = sizeof(kVersionName) - 1;
        if (StartsWithHeaderName(line, kVersionName, kVersionNameLen)) {
            unsigned long long parsed = 0;
            if (ParseUnsignedInteger(TrimHeaderValue(line.substr(kVersionNameLen)), &parsed) &&
                parsed >= 5) {
                state->version_at_least_5 = true;
            }
            return bytes;
        }

        static constexpr char kContentTypeName[] = "Content-Type:";
        static constexpr size_t kContentTypeNameLen = sizeof(kContentTypeName) - 1;
        if (StartsWithHeaderName(line, kContentTypeName, kContentTypeNameLen)) {
            std::string media = TrimHeaderValue(line.substr(kContentTypeNameLen));
            // Media type only, ignoring any "; charset=..." parameter,
            // mirroring the host's own classification in nvhttp.cpp's
            // cosmic_clipboard_post.
            const size_t semi = media.find(';');
            if (semi != std::string::npos) {
                media = TrimHeaderValue(media.substr(0, semi));
            }
            if (EqualsIgnoreCase(media, "text/plain")) {
                state->content_type = HeaderState::ContentType::Text;
            } else if (EqualsIgnoreCase(media, "image/png")) {
                state->content_type = HeaderState::ContentType::Png;
            } else {
                state->content_type = HeaderState::ContentType::Unknown;
            }
            return bytes;
        }
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
    // 20 s server-side long-poll hold (wait=1 on the GET, see PollOnce) plus
    // TLS handshake and network margin.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Lets stop() abort an in-flight transfer instead of blocking on its full
    // timeout budget while joining the worker.
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

// POSTs `bytes` as the request body of /cosmic/clipboard, with
// `content_type_headers` (one of WorkerLoop's kept-alive lists — see there)
// supplying an explicit Content-Type that overrides libcurl's default
// application/x-www-form-urlencoded, so the host can classify the body
// instead of assuming text. The response body is irrelevant (host -> client
// data only ever arrives via the GET poll below); the write callback and cap
// here only exist so curl does not dump the response to stdout. A failed
// POST is dropped silently and not retried until the next locally-copied
// value arrives — POSTing takes priority over polling, and a fresh local
// copy also wakes a parked wait=1 GET (see g_wake_requested), so a
// persistently failing host is retried promptly rather than only once a poll
// interval.
void PostPayload(CURL* curl, const std::string& post_url, const std::string& bytes,
                 curl_slist* content_type_headers) {
    std::string discard;
    curl_easy_setopt(curl, CURLOPT_URL, post_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bytes.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, content_type_headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCappedBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);
    curl_easy_perform(curl);
}

// Outcome of one PollOnce call, used by WorkerLoop both to decide whether to
// skip the pacing sleep and to drive the failure backoff:
//   Failed     — the request failed outright, the seq header was missing or
//                malformed, or the response was a 200 with a non-advancing
//                seq / a 404 / any other status. WorkerLoop backs off.
//   Ok         — a trustworthy 200/204 that did not block on the host's
//                wait=1 hold (a v1/stock host answers instantly). The normal
//                pacing sleep still applies, and any backoff is reset.
//   OkNoSleep  — either a benign wake (a parked GET aborted because
//                publish_local() just filled the outgoing slot) or a
//                trustworthy 200/204 that did block on the wait=1 hold. The
//                caller should re-poll immediately, and any backoff is reset.
enum class PollResult { Failed, Ok, OkNoSleep };

// Tri-state latch for whether the connected host understands image/png over
// /cosmic/clipboard, threaded through PollOnce by reference exactly like
// last_seq. Local to WorkerLoop and reset to Unknown by every start() (a new
// host starts the latch over), never regressing once resolved: a host does
// not un-learn a protocol version mid-session.
//   Unknown   — nothing trustworthy has been heard from the host yet. An
//               untrustworthy response (curl failure, missing/malformed seq
//               header, 404, any other status) leaves this unchanged rather
//               than resolving it, since a failure must never be allowed to
//               prove a host incapable.
//   Capable   — the first trustworthy (well-formed seq header, HTTP 200 or
//               204) response also carried X-Cosmic-Clipboard-Version >= 5.
//   Incapable — the first trustworthy 200/204 response did not.
enum class HostImages { Unknown, Capable, Incapable };

// GETs /cosmic/clipboard?since=<last_seq>&wait=1, advertising image support
// via `accept_headers` (WorkerLoop's kept-alive "Accept: text/plain,
// image/png" list — see there), and, if the response passes every check
// described in clipsync.h, updates `last_seq`, `host_images` and/or the
// incoming buffer. A failed request, a missing/malformed seq header, a 404,
// or any status other than 200/204 changes nothing and logs nothing — a host
// with clipboard sharing off answers 404 every poll and must not produce log
// spam. CURLOPT_HTTPHEADER/HTTPGET/POSTFIELDS/POSTFIELDSIZE are reset
// explicitly so a preceding PostPayload() call on this same handle cannot
// leak into this GET. CURLOPT_POSTFIELDS switches the handle's method to
// POST even when set to nullptr, so CURLOPT_HTTPGET must be set last among
// these three calls to actually select GET (per curl's docs, switching back
// to GET after CURLOPT_POST requires re-enabling CURLOPT_HTTPGET).
//
// `host_images` resolves out of Unknown on the first trustworthy
// (well-formed seq header, HTTP 200 or 204) response: to Capable if it
// carries X-Cosmic-Clipboard-Version >= 5, to Incapable otherwise — this is
// the only signal the client has that the connected host understands
// image/png at all, since a v1-v4 host silently ignores an unrecognised
// Accept value instead of erroring. Left unchanged once resolved (see
// HostImages above) and on every untrustworthy response, and never touched
// on the OkNoSleep-benign-wake path below, since that path never receives
// real header data.
//
// Returns OkNoSleep when the caller should re-poll immediately instead of
// running the normal inter-poll sleep. This happens in two cases: the
// request actually blocked on the host's wait=1 long-poll hold (request took
// more than 2 s) and produced a trustworthy, successful response, or the GET
// was aborted by a benign wake — in which case the re-poll is preceded by
// draining and POSTing the outgoing slot, not by an idle re-poll of the same
// request. A v1/stock host that ignores wait=1 and answers instantly must
// never see an immediate re-poll, or the worker would hot-loop it.
PollResult PollOnce(CURL* curl, const std::string& get_base_url, unsigned long long& last_seq,
                    HostImages& host_images, curl_slist* accept_headers) {
    const std::string get_url =
        get_base_url + "?since=" + std::to_string(last_seq) + "&wait=1";

    std::string body;
    HeaderState header_state;
    curl_easy_setopt(curl, CURLOPT_URL, get_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, accept_headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCappedBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CaptureHeaders);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_state);

    const auto start = std::chrono::steady_clock::now();
    const CURLcode res = curl_easy_perform(curl);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool held = elapsed > std::chrono::seconds(2);

    // A parked GET aborted by ProgressCallback because publish_local() just
    // filled the outgoing slot is a benign wake, not a failure: skip the
    // sleep so the loop immediately drains and POSTs the fresh text, but
    // discard the partial body/headers exactly like any other error path —
    // do not touch last_seq or the incoming buffer. A real cancel still
    // falls through to the res != CURLE_OK return below.
    if (res == CURLE_ABORTED_BY_CALLBACK &&
        !g_cancel_requested.load(std::memory_order_relaxed)) {
        return PollResult::OkNoSleep;
    }

    if (res != CURLE_OK || !header_state.valid) {
        return PollResult::Failed;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code == 200 && header_state.seq > last_seq) {
        if (header_state.content_type == HeaderState::ContentType::Unknown) {
            // Fail closed: never store a body of unrecognised or absent
            // Content-Type, since there is then no way to know how to treat
            // it later (as clipboard text or an image).
            return PollResult::Failed;
        }
        const Mime mime = header_state.content_type == HeaderState::ContentType::Png
                               ? Mime::Png
                               : Mime::Text;
        if (mime == Mime::Text && body.size() > kMaxBytes) {
            // WriteCappedBody only enforces kMaxImageBytes, the looser of
            // the two caps, since a GET body may legitimately be a PNG now;
            // a body now known to be text must be re-checked against the
            // tighter kMaxBytes before it is trusted.
            return PollResult::Failed;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_incoming.bytes = std::move(body);
        g_incoming.mime = mime;
        g_incoming.pending = true;
        last_seq = header_state.seq;
    } else if (http_code == 204) {
        last_seq = header_state.seq;
    } else {
        // 200 with a non-advancing seq, 404, and any other status: change
        // nothing, and never skip the sleep for an outcome that is not a
        // clean 200/204.
        return PollResult::Failed;
    }

    // Resolved here, after every Failed return above, rather than
    // immediately after http_code is read: a 200 is not actually
    // trustworthy until it has also cleared the Content-Type and
    // (for text) the kMaxBytes checks above, both of which return Failed on
    // the paths that fail them. Resolving the latch any earlier would let a
    // 200 with an unrecognised Content-Type — untrustworthy by that same
    // definition — decide Capable/Incapable before its untrustworthiness is
    // known. Do not hoist this back up next to http_code.
    if (host_images == HostImages::Unknown && (http_code == 200 || http_code == 204)) {
        if (header_state.version_at_least_5) {
            host_images = HostImages::Capable;
        } else {
            host_images = HostImages::Incapable;
        }
    }

    return held ? PollResult::OkNoSleep : PollResult::Ok;
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

        // libcurl does not copy a curl_slist passed to CURLOPT_HTTPHEADER;
        // each list must outlive every transfer that uses it. These three
        // are built once and kept alive for the whole loop below (freed by
        // curl_slist_free_all on every exit path, including an exception,
        // via the same unique_ptr RAII idiom as curl_owner above), and the
        // correct one is set immediately before each transfer so a
        // preceding POST's Content-Type list can never leak into the
        // following GET (the same POSTFIELDS hazard PollOnce's comment
        // documents).
        using SlistPtr = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
        SlistPtr get_accept_headers(curl_slist_append(nullptr, "Accept: text/plain, image/png"),
                                    &curl_slist_free_all);
        SlistPtr post_text_headers(
            curl_slist_append(nullptr, "Content-Type: text/plain; charset=utf-8"),
            &curl_slist_free_all);
        // If curl_slist_append fails here (OOM), post_png_headers is left
        // null and the outgoing-slot drain below drops any Png payload
        // instead of POSTing it: a null CURLOPT_HTTPHEADER falls back to
        // libcurl's default Content-Type, and the host would then classify
        // the unlabelled PNG bytes as text and hand raw PNG bytes to the
        // peer's paste — a fail-open path this feature's bar (fail-closed)
        // does not allow. A null post_text_headers needs no equivalent
        // guard: libcurl's default Content-Type for CURLOPT_POSTFIELDS is
        // application/x-www-form-urlencoded, which host/sunshine's
        // cosmic_clipboard_post already classifies as text (see nvhttp.cpp),
        // the correct outcome for a dropped text list — do not "fix" this
        // asymmetry by adding a guard here too.
        SlistPtr post_png_headers(curl_slist_append(nullptr, "Content-Type: image/png"),
                                  &curl_slist_free_all);

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

        // Whether the connected host has advertised (via
        // X-Cosmic-Clipboard-Version >= 5, see PollOnce) that it understands
        // image/png. Local to this call, exactly like last_seq above and for
        // the same reason: a client that roams from a v5 host to a v4 host
        // across a start()/stop() cycle must begin again at Unknown rather
        // than inheriting the previous host's capability. Starting at
        // Unknown rather than Incapable is what lets a Png copied before the
        // first trustworthy response arrives still be held (see the drain
        // below) instead of being dropped before the host ever gets a
        // chance to prove itself capable.
        HostImages host_images = HostImages::Unknown;

        // Current inter-poll sleep, doubled (capped at kMaxPollIntervalMs) on
        // every consecutive PollOnce failure and snapped back to
        // kPollIntervalMs on the first success, so a persistently failing
        // host (unreachable, TLS error, sharing off, etc.) is not retried
        // every second forever.
        int poll_interval_ms = kPollIntervalMs;

        while (!g_cancel_requested.load(std::memory_order_relaxed)) {
            // Cleared before the outgoing slot is drained below, not after:
            // a wake arriving between this clear and the drain costs at most
            // one immediately-aborted GET on the next pass (self-correcting,
            // since that next iteration clears it again), whereas clearing
            // after the drain could discard a wake whose payload had not yet
            // been drained and strand that payload for a full 20 s hold.
            g_wake_requested.store(false, std::memory_order_relaxed);

            std::string outgoing_bytes;
            Mime outgoing_mime = Mime::Text;
            bool has_outgoing = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                // A Png payload is held rather than drained here while host
                // support is still Unknown, instead of the more obvious
                // "drain now, decide whether to POST it below": draining it
                // now would discard it before host_images ever gets a
                // chance to resolve to Capable, losing the very first image
                // of a session against a capable host. Holding it here is
                // bounded and safe: the slot is last-write-wins, so at most
                // one payload (<= kMaxImageBytes) is ever held; a newer
                // local copy overwrites it in place; and start() clears both
                // slots, so nothing survives into a different host's
                // session. It also cannot spin: g_wake_requested is cleared
                // at the top of this iteration, above, before this drain
                // runs, so holding a payload here costs at most one extra
                // poll cycle before host_images resolves out of Unknown.
                // Text is never held.
                const bool hold =
                    g_outgoing.mime == Mime::Png && host_images == HostImages::Unknown;
                if (g_outgoing.pending && !hold) {
                    outgoing_bytes = std::move(g_outgoing.bytes);
                    outgoing_mime = g_outgoing.mime;
                    g_outgoing.bytes.clear();
                    g_outgoing.pending = false;
                    has_outgoing = true;
                }
            }
            // A pending POST is sent before this pass's poll, so a fresh
            // local copy reaches the host without waiting behind a GET.
            if (has_outgoing) {
                // Post text unconditionally, and an image only once the host
                // has advertised image support (host_images == Capable) and
                // this worker can actually label it as image/png (the
                // post_png_headers OOM guard below). Otherwise drop it here,
                // without POSTing and without retrying: this worker
                // deliberately produces no output for a host that does not
                // support a feature (a v4 host would otherwise log a line
                // per copy via the unsupported-media-type rejection path),
                // and never POSTs a PNG it cannot label, which the host
                // would otherwise misclassify as text.
                const bool can_post_png =
                    host_images == HostImages::Capable && post_png_headers != nullptr;
                if (outgoing_mime != Mime::Png || can_post_png) {
                    curl_slist* content_type_headers = outgoing_mime == Mime::Png
                                                            ? post_png_headers.get()
                                                            : post_text_headers.get();
                    PostPayload(curl, base_url, outgoing_bytes, content_type_headers);
                }
            }

            bool skip_sleep = false;
            // This pass's sleep. Defaults to the backoff already in effect;
            // poll_interval_ms itself is only raised *after* this pass's
            // sleep on Failed, so a run of failures sleeps 1, 2, 4, ...
            // rather than jumping straight to the doubled value.
            int sleep_ms = poll_interval_ms;
            if (!g_cancel_requested.load(std::memory_order_relaxed)) {
                switch (PollOnce(curl, base_url, last_seq, host_images,
                                 get_accept_headers.get())) {
                    case PollResult::Failed:
                        poll_interval_ms =
                            std::min(poll_interval_ms * 2, kMaxPollIntervalMs);
                        break;
                    case PollResult::Ok:
                        poll_interval_ms = kPollIntervalMs;
                        sleep_ms = kPollIntervalMs;
                        break;
                    case PollResult::OkNoSleep:
                        poll_interval_ms = kPollIntervalMs;
                        skip_sleep = true;
                        break;
                }
            }

            // Only skip the pacing sleep when PollOnce reports either that
            // the request actually blocked on the host's wait=1 hold and
            // returned a trustworthy 200/204, or that the GET was aborted by
            // a benign wake, whose payload the next iteration drains and
            // POSTs before re-polling — a v1/stock host ignores wait=1 and
            // answers instantly forever, so an unconditional skip here would
            // hot-loop that host.
            //
            // Also breaks on g_wake_requested so a fresh local copy is not
            // stranded behind a maxed-out (up to 30 s) backoff sleep. This
            // cannot spin: WorkerLoop clears g_wake_requested at the top of
            // the next iteration, before it drains the outgoing slot, so the
            // flag is never seen still set once that iteration re-checks it.
            if (!skip_sleep) {
                for (int waited = 0;
                     waited < sleep_ms &&
                     !g_cancel_requested.load(std::memory_order_relaxed) &&
                     !g_wake_requested.load(std::memory_order_relaxed);
                     waited += kSleepStepMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
                }
            }
        }

        // curl_owner's and the three SlistPtr's destructors release the
        // handle and the header lists here (and on every other exit path
        // from this try block, including an exception).
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
        g_outgoing = PendingPayload{};
        g_incoming = PendingPayload{};
    }
    g_cancel_requested.store(false, std::memory_order_relaxed);
    g_worker = std::thread(WorkerLoop, host_address, https_port);
}

void stop() {
    g_cancel_requested.store(true, std::memory_order_relaxed);
    if (g_worker.joinable()) {
        g_worker.join();
    }
    // Cleared after join(), not before: the worker is provably gone by this
    // point, so there is no race with an in-flight drain of g_outgoing. This
    // must run here rather than relying on the next start() to do it,
    // because the hold-while-Unknown drain (see WorkerLoop) can leave a Png
    // payload (up to kMaxImageBytes) parked in g_outgoing for the entire
    // idle period between sessions if the host's capability was never
    // resolved before this call.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_outgoing = PendingPayload{};
    g_incoming = PendingPayload{};
}

void publish_local(Mime mime, std::string bytes) {
    const std::size_t cap = mime == Mime::Png ? kMaxImageBytes : kMaxBytes;
    if (bytes.size() > cap) {
        return;  // refuse oversize bytes, matching the host's own POST cap
    }
    {
        // Taken by value above and moved in here rather than copied, so an
        // 8 MiB image is not memcpy'd while g_mutex is held on the
        // main/render thread — the copy (if the caller passes an lvalue)
        // happens constructing the by-value parameter, before this lock is
        // ever taken.
        std::lock_guard<std::mutex> lock(g_mutex);
        g_outgoing.bytes = std::move(bytes);
        g_outgoing.mime = mime;
        g_outgoing.pending = true;
    }
    // Wake a parked wait=1 GET so this copy does not wait behind its hold.
    g_wake_requested.store(true, std::memory_order_relaxed);
}

bool take_incoming(std::string& out_bytes, Mime& out_mime) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_incoming.pending) {
        return false;
    }
    out_bytes = std::move(g_incoming.bytes);
    out_mime = g_incoming.mime;
    g_incoming.bytes.clear();
    g_incoming.pending = false;
    return true;
}

}  // namespace cosmic::clipsync
