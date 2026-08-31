// Cosmic Desk — client-side file transfer worker (see filesync.h).
//
// Modeled closely on clipsync.cpp: its own curl easy handle (created and torn
// down on the worker thread, never the vendored libgamestream global), the
// same client-cert trust model, and the same XFERINFOFUNCTION progress guard
// so stop() can abort an in-flight transfer instead of blocking on it while
// joining. The deliberate difference is the timeout policy — see
// ConfigureCurl.

#include "app/filesync.h"

#include "app/settings.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cosmic::filesync {

namespace {

// Longest X-Cosmic-File-Id kept from a response header. The host mints short
// opaque ids; this bounds what an untrusted header can make the worker carry
// around. A longer value is truncated, which simply makes the next request
// fail with an unknown id — this client never invents an id of its own.
constexpr std::size_t kMaxFileIdBytes = 64;

// Ceiling on the response body DiscardBody keeps. No route here returns a
// body this client reads; the callback exists only so libcurl does not dump
// an unauthorized host's XML error body to stdout, and the cap only keeps a
// chatty host from making the worker buffer a large one.
constexpr std::size_t kMaxDiscardBytes = 4096;

// User-facing failure text for Progress::error, picked from the failing
// request's status (see ErrorForStatus). Deliberately unprefixed: the overlay
// adds the context ("<filename>: ...").
constexpr char kErrorUnsupported[] = "Host does not support file transfer";
constexpr char kErrorBusy[] = "Host is busy with another transfer";
constexpr char kErrorNoSpace[] = "Not enough space on the host";
constexpr char kErrorBadName[] = "Host rejected the file name";
constexpr char kErrorChunkTooLarge[] = "Chunk too large";
constexpr char kErrorFailed[] = "Transfer failed";
constexpr char kErrorQueueFull[] = "Too many files queued";
constexpr char kErrorNotStreaming[] = "No active stream";

std::thread g_worker;

// Set by stop(), cleared by start() before a fresh worker is spawned. Serves
// two purposes: the worker's wait/continue condition, and (via
// ProgressCallback below) the signal an in-flight curl transfer checks to
// abort early instead of running out its low-speed budget.
std::atomic<bool> g_cancel_requested{false};

// Guards all three pieces of shared state below. Held only for snapshot-sized
// work — never across a request — so progress() and enqueue() on the main
// thread cannot end up waiting on the network.
std::mutex g_mutex;
std::condition_variable g_cv;

// Paths still waiting, oldest first; the file being uploaded right now has
// already been popped off. Bounded by kMaxQueued (see enqueue).
std::deque<std::string> g_queue;

Progress g_progress;

// Whether a worker is running and therefore whether enqueue() has anywhere to
// put a path. Distinct from g_cancel_requested so that a drop arriving in the
// window between stop() and the next start() is refused with a visible reason
// instead of being silently swallowed by a queue nobody drains.
bool g_running = false;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The four publishers below are the only writers of g_progress. Each one
// refreshes updated_ms and re-reads the queue depth, so the overlay's
// "changed recently" test and its queue count can never drift from reality.
void PublishFileStart(const std::string& filename, std::uint64_t total) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_progress.active = true;
    g_progress.filename = filename;
    g_progress.transferred = 0;
    g_progress.total = total;
    g_progress.queued = static_cast<int>(g_queue.size());
    g_progress.done = false;
    g_progress.error.clear();  // the sticky error describes the last file only
    g_progress.updated_ms = NowMs();
}

void PublishTransferred(std::uint64_t transferred) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_progress.transferred = transferred;
    g_progress.queued = static_cast<int>(g_queue.size());
    g_progress.updated_ms = NowMs();
}

void PublishFileDone() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_progress.active = false;
    g_progress.transferred = g_progress.total;
    g_progress.queued = static_cast<int>(g_queue.size());
    g_progress.done = true;
    g_progress.error.clear();
    g_progress.updated_ms = NowMs();
}

void PublishError(const char* message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_progress.active = false;
    g_progress.queued = static_cast<int>(g_queue.size());
    g_progress.done = false;
    g_progress.error = message;
    g_progress.updated_ms = NowMs();
}

// Maps the failing request's HTTP status onto the user-facing text above. A
// curl-level failure has no status (0) and lands on the generic message, as
// does any status the routes do not document.
const char* ErrorForStatus(long http_code) {
    switch (http_code) {
        case 404:
            return kErrorUnsupported;  // stock/old host, or sharing switched off
        case 409:
            return kErrorBusy;
        case 507:
            return kErrorNoSpace;
        case 400:
            return kErrorBadName;
        case 413:
            return kErrorChunkTooLarge;
        default:
            return kErrorFailed;
    }
}

// Checked by curl during a transfer (CURLOPT_XFERINFOFUNCTION below); a
// non-zero return aborts the transfer with CURLE_ABORTED_BY_CALLBACK, so
// stop() does not have to wait out a 4 MiB chunk's low-speed budget while
// joining the worker.
int ProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_cancel_requested.load(std::memory_order_relaxed) ? 1 : 0;
}

// Swallows the response body, keeping at most kMaxDiscardBytes of it (nothing
// ever reads what it kept). Returning the full count on overflow rather than
// short — unlike clipsync's WriteCappedBody — keeps an over-chatty response
// from turning an otherwise successful request into a curl write error; the
// status and the id header are the only things that decide anything here.
// Runs inside libcurl's C frames, so no exception may escape this function.
size_t DiscardBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    std::string* body = static_cast<std::string*>(userdata);
    try {
        if (body->size() < kMaxDiscardBytes) {
            body->append(ptr, std::min(bytes, kMaxDiscardBytes - body->size()));
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

// Strips leading/trailing spaces, tabs and (for the trailing end) \r\n from a
// header value substring. Returns an empty string when the value is entirely
// whitespace, which the caller treats as "no id present".
std::string TrimHeaderValue(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t");
    const size_t end = value.find_last_not_of(" \t\r\n");
    if (start == std::string::npos || end == std::string::npos || start > end) {
        return {};
    }
    return value.substr(start, end - start + 1);
}

// Scans one /begin response header line for X-Cosmic-File-Id, recording it
// into *userdata (a std::string) when present. This header — not the status —
// is what proves the host actually understood the route: an unauthorized or
// stock Sunshine host answers 200 with a 404-shaped XML body (see
// filesync.h). Runs inside libcurl's C frames, so no exception may escape
// this function.
size_t CaptureFileId(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t bytes = size * nitems;
    std::string* file_id = static_cast<std::string*>(userdata);
    try {
        const std::string line(buffer, bytes);
        static constexpr char kIdName[] = "X-Cosmic-File-Id:";
        static constexpr size_t kIdNameLen = sizeof(kIdName) - 1;
        if (StartsWithHeaderName(line, kIdName, kIdNameLen)) {
            std::string value = TrimHeaderValue(line.substr(kIdNameLen));
            if (value.size() > kMaxFileIdBytes) {
                value.resize(kMaxFileIdBytes);
            }
            *file_id = std::move(value);
        }
    } catch (...) {
        return 0;
    }
    return bytes;
}

// Configures the easy handle exactly like clipsync.cpp's ConfigureCurl
// (clipsync.cpp:254-276): same self-signed-host trust model (client cert +
// key from Settings::config_dir()/"client", no peer/host verification — this
// mirrors third-party/libgamestream/http.c's Moonlight pairing trust model),
// and the same progress-callback cancel hook. Does NOT call curl_global_init:
// presence.cpp:189 owns that call, it is reference-counted, and it always
// runs before any session (and therefore this worker) can start.
//
// One deliberate difference from clipsync: no CURLOPT_TIMEOUT. A wall-clock
// cap that is right for a 1 MiB clipboard POST would kill a legitimate 4 MiB
// chunk on a slow LAN, and the chunk would then be retried forever. The
// transfer is bounded by progress instead — connect promptly, and give up
// only if throughput actually stalls below 1 KiB/s for 30 s.
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
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Lets stop() abort an in-flight transfer instead of blocking on its full
    // budget while joining the worker.
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

// Clears everything a request below sets, so nothing leaks from one request
// into the next on this reused handle: a stale CURLOPT_POSTFIELDS would point
// at a freed chunk buffer, a stale CURLOPT_HEADERDATA at a dead local, and a
// stale header list at a body type this request does not send. clipsync.cpp
// documents the same libcurl trap at clipsync.cpp:341-346. The write callback
// is reset too, but PerformRequest always installs one again before
// performing — libcurl's default writer dumps the body to stdout, which this
// module must never do.
void ResetRequestState(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, -1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(-1));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
}

// Outcome of one request on the shared handle.
struct RequestResult {
    bool ok = false;         // curl succeeded and the host answered 200
    bool cancelled = false;  // ProgressCallback aborted it: stop() is joining
    long http_code = 0;      // 0 when curl itself failed
    // Set when curl_easy_perform returned CURLE_GOT_NOTHING: the TLS
    // handshake and connection succeeded but the peer closed without writing
    // any HTTP response at all. Left false for every other curl-level
    // failure (CURLE_COULDNT_CONNECT, a TLS failure, CURLE_OPERATION_TIMEDOUT,
    // ...), which stay generic failures — only UploadFile's /begin call gives
    // this flag any meaning (see the comment there for why).
    bool got_nothing = false;
};

// POSTs `body_size` bytes from `body` to `url` and discards the response
// body. `headers` may be null (no extra headers) and `out_file_id` may be
// null (no interest in the id header). `body` must stay alive for the call;
// it may be empty (an empty POST body, which /begin, /end and /abort want).
RequestResult PerformRequest(CURL* curl, const std::string& url, const char* body,
                             std::uint64_t body_size, curl_slist* headers,
                             std::string* out_file_id) {
    std::string discard;
    ResetRequestState(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    // CURLOPT_POSTFIELDS alone makes libcurl strlen() the data when it builds
    // the request (during curl_easy_perform), not when the pointer is set —
    // but a binary chunk (and a zero-byte begin/end/abort body) needs an
    // explicit size regardless of setopt order, since strlen() would stop at
    // the first zero byte or return non-zero for an intentionally empty
    // body. (CURLOPT_COPYPOSTFIELDS is the one with a set-size-first
    // requirement; this code does not use it.)
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body_size));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
    if (out_file_id != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CaptureFileId);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, out_file_id);
    }

    RequestResult result;
    const CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_ABORTED_BY_CALLBACK) {
        result.cancelled = true;
        return result;
    }
    if (res != CURLE_OK) {
        result.got_nothing = res == CURLE_GOT_NOTHING;
        return result;  // curl-level failure: no status, generic error text
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);
    result.ok = result.http_code == 200;
    return result;
}

using EscapedPtr = std::unique_ptr<char, decltype(&curl_free)>;

// Percent-encodes `value` for use in a query string. Null on allocation
// failure, which every caller treats as a failed transfer.
EscapedPtr Escape(CURL* curl, const std::string& value) {
    return EscapedPtr(curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size())),
                      &curl_free);
}

enum class UploadResult { Ok, Failed, Cancelled };

// Best-effort POST /cosmic/file/abort so the host drops its partial file
// immediately instead of waiting for its idle GC. The result is deliberately
// ignored: the transfer has already failed and the user has already been told
// why. Never called on the cancellation path — see UploadFile.
void SendAbort(CURL* curl, const std::string& base_url, const std::string& escaped_id) {
    const std::string url = base_url + "/abort?id=" + escaped_id;
    PerformRequest(curl, url, "", 0, nullptr, nullptr);
}

// Uploads one file: begin -> N x chunk -> end. Publishes its own progress and,
// on failure, its own sticky error, so the caller only has to decide whether
// to keep going. `chunk_headers` is the caller-owned "Content-Type:
// application/octet-stream" list (see WorkerLoop).
UploadResult UploadFile(CURL* curl, const std::string& base_url, const std::string& utf8_path,
                        curl_slist* chunk_headers) {
    // SDL delivers dropped paths as UTF-8. Constructing the path from
    // char8_t* is what makes libstdc++ decode it as UTF-8 and convert it to
    // the native wide encoding, instead of running it through the (typically
    // ACP) narrow locale and mangling every non-ASCII name on Windows.
    const std::filesystem::path path(reinterpret_cast<const char8_t*>(utf8_path.c_str()));
    const std::u8string u8_name = path.filename().u8string();
    const std::string name(reinterpret_cast<const char*>(u8_name.c_str()), u8_name.size());
    // Published as soon as the name is known, with total = 0 as a
    // placeholder (corrected below once the real size is known), so every
    // failure from here on reports against *this* file. Without this, the
    // three checks below would publish an error while g_progress.filename,
    // .total and .transferred still held the PREVIOUS file's values — a
    // successful upload followed by a failed drop would then show the error
    // against the file that actually succeeded.
    PublishFileStart(name, 0);
    if (name.empty()) {
        PublishError(kErrorFailed);  // no basename to send (e.g. a trailing separator)
        return UploadResult::Failed;
    }

    // Taken once, before /begin, because it is what the host is told to
    // expect: every chunk offset and the final size check are derived from
    // this number, so a file that shrinks underneath us fails on the short
    // read below rather than looping or sending a size the host will reject.
    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        PublishError(kErrorFailed);
        return UploadResult::Failed;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(file_size);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        PublishError(kErrorFailed);
        return UploadResult::Failed;
    }

    PublishFileStart(name, total);  // corrects total now that it is known

    const EscapedPtr escaped_name = Escape(curl, name);
    if (escaped_name == nullptr) {
        PublishError(kErrorFailed);
        return UploadResult::Failed;
    }

    std::string file_id;
    const std::string begin_url = base_url + "/begin?name=" + escaped_name.get() +
                                  "&size=" + std::to_string(total);
    const RequestResult begin = PerformRequest(curl, begin_url, "", 0, nullptr, &file_id);
    // Cancellation (here and at every other .cancelled below) means stop() is
    // joining: return without POSTing /abort and without a sticky error. The
    // connection is going away with the session, so an abort would only block
    // the join on one more request — and the host aborts the transfer and
    // deletes its .part file on stream teardown regardless. The user did not
    // fail at anything either, so nothing is published to the overlay, which
    // stop() clears immediately afterwards.
    if (begin.cancelled) {
        return UploadResult::Cancelled;
    }
    // The trust rule (filesync.h): 200 alone proves nothing, because a stock
    // or unauthorized host answers 200 with an XML error body. Only a 200
    // that also carries a non-empty X-Cosmic-File-Id means the transfer
    // exists — and no id means there is nothing to abort either.
    //
    // begin.got_nothing is the OTHER way a stock or pre-v6 host reveals
    // itself here, and it takes a curl-level failure rather than an HTTP
    // status to see it: Sunshine registers only
    // https_server.default_resource["GET"] (host/sunshine/src/nvhttp.cpp), and
    // Simple-Web-Server's find_resource (server_http.hpp:790-792) returns
    // WITHOUT writing anything when a request's method has no default
    // resource — so a POST here gets no HTTP response at all, just a closed
    // connection, which libcurl reports as CURLE_GOT_NOTHING with the
    // response code left at 0. Do NOT fold this into the ErrorForStatus(0)
    // catch-all below: an unreachable host, a TLS failure or a timeout are
    // different curl-level failures and must keep reporting kErrorFailed, so
    // this is checked before falling back to ErrorForStatus.
    if (!begin.ok || file_id.empty()) {
        const char* error;
        if (begin.ok) {
            error = kErrorUnsupported;
        } else if (begin.got_nothing) {
            error = kErrorUnsupported;
        } else {
            error = ErrorForStatus(begin.http_code);
        }
        PublishError(error);
        return UploadResult::Failed;
    }

    // The id is host-supplied text going into a query string; escaping it
    // keeps a surprising value from splicing extra parameters into the URL.
    // A well-formed opaque id passes through unchanged.
    const EscapedPtr escaped_id = Escape(curl, file_id);
    if (escaped_id == nullptr) {
        PublishError(kErrorFailed);
        return UploadResult::Failed;
    }

    // One buffer for every chunk of this file, sized to the file so a small
    // drop does not allocate 4 MiB, and released with the file rather than
    // held for the whole idle session.
    std::vector<char> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, total)));

    std::uint64_t sent = 0;
    while (sent < total) {
        if (g_cancel_requested.load(std::memory_order_relaxed)) {
            return UploadResult::Cancelled;
        }
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, total - sent));
        file.read(buffer.data(), static_cast<std::streamsize>(want));
        if (file.gcount() != static_cast<std::streamsize>(want)) {
            // Short read: the file was truncated or vanished mid-upload. Fail
            // it cleanly instead of re-reading the same offset forever.
            SendAbort(curl, base_url, escaped_id.get());
            PublishError(kErrorFailed);
            return UploadResult::Failed;
        }

        const std::string chunk_url = base_url + "/chunk?id=" + escaped_id.get() +
                                      "&offset=" + std::to_string(sent);
        const RequestResult chunk =
            PerformRequest(curl, chunk_url, buffer.data(), want, chunk_headers, nullptr);
        if (chunk.cancelled) {
            return UploadResult::Cancelled;
        }
        if (!chunk.ok) {
            SendAbort(curl, base_url, escaped_id.get());
            PublishError(ErrorForStatus(chunk.http_code));
            return UploadResult::Failed;
        }
        sent += want;
        PublishTransferred(sent);
    }

    const std::string end_url = base_url + "/end?id=" + escaped_id.get();
    const RequestResult end = PerformRequest(curl, end_url, "", 0, nullptr, nullptr);
    if (end.cancelled) {
        return UploadResult::Cancelled;
    }
    if (!end.ok) {
        SendAbort(curl, base_url, escaped_id.get());
        PublishError(ErrorForStatus(end.http_code));
        return UploadResult::Failed;
    }

    PublishFileDone();
    return UploadResult::Ok;
}

// Clears g_running under g_mutex, no matter which of WorkerLoop's exit paths
// runs: falling off the end of the try block, an early return (the
// curl_easy_init() == nullptr check below), or the outer catch(...) unwinding
// past this object. Declared first inside the try block so its destructor
// fires on every one of those paths, per the C++ rule that objects
// constructed in a try block are destroyed while unwinding to a matching
// catch. Without this, a worker that dies on an unexpected exception (or a
// curl init failure) leaves g_running == true forever, and enqueue() keeps
// accepting paths for a worker that will never drain them (contradicts
// g_running's contract at its declaration above).
struct ClearRunningOnExit {
    ~ClearRunningOnExit() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_running = false;
    }
};

// The worker thread body. `host_address` and `https_port` are captured by
// value at spawn time (see start()), so no shared global is needed for them.
void WorkerLoop(std::string host_address, int https_port) {
    // The worker must never throw out of the thread; a transient failure
    // (e.g. a temporary curl init hiccup) must not crash the process.
    try {
        const ClearRunningOnExit clear_running_on_exit;

        // One handle for the whole worker lifetime, so every chunk of every
        // file rides the same keep-alive connection instead of paying for a
        // TLS handshake each. Owned here so an exception thrown anywhere
        // below still releases it plus its socket and TLS session.
        const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl_owner(
            curl_easy_init(), &curl_easy_cleanup);
        CURL* curl = curl_owner.get();
        if (curl == nullptr) {
            return;  // silent: this module never writes to the log
        }
        ConfigureCurl(curl);

        // libcurl does not copy a curl_slist passed to CURLOPT_HTTPHEADER;
        // the list must outlive every transfer that uses it. Built once here
        // and freed on every exit path, including an exception, via the same
        // unique_ptr RAII idiom as curl_owner above. It overrides libcurl's
        // default application/x-www-form-urlencoded so a chunk is labelled
        // for what it is: raw bytes.
        //
        // The empty "Expect:" entry suppresses "Expect: 100-continue", which
        // libcurl otherwise adds on its own to any HTTP/1.1 POST body over
        // 1024 bytes (every chunk qualifies at kChunkBytes), then waits up to
        // CURLOPT_EXPECT_100_TIMEOUT_MS (1000 ms, not overridden here) for an
        // interim 100 response before sending the body. The host,
        // Simple-Web-Server (third-party/Simple-Web-Server/server_http.hpp),
        // has no Expect/100-continue handling at all, so it never sends that
        // interim response and every chunk would stall the full second
        // regardless of LAN speed — not a minor inefficiency but enough to
        // cap a multi-hundred-MB transfer's throughput near kChunkBytes/s.
        // The empty-body begin/end/abort requests below stay under the
        // 1024-byte threshold, so libcurl never adds the header to them and
        // they need no equivalent entry.
        curl_slist* chunk_headers_raw =
            curl_slist_append(nullptr, "Content-Type: application/octet-stream");
        if (chunk_headers_raw != nullptr) {
            // curl_slist_append leaves the original list untouched and
            // returns null on OOM, so only adopt the new pointer on success
            // — otherwise this still uploads with the Content-Type-only list
            // (and pays the 100-continue stall) rather than losing it too.
            curl_slist* with_expect = curl_slist_append(chunk_headers_raw, "Expect:");
            if (with_expect != nullptr) {
                chunk_headers_raw = with_expect;
            }
        }
        const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> chunk_headers(
            chunk_headers_raw, &curl_slist_free_all);

        // IPv6 literals must be bracketed in a URL; IPv4/hostnames are not
        // (mirrors clipsync.cpp:507-514). A user-supplied bracketed form is
        // kept as-is.
        const bool ipv6 = host_address.find(':') != std::string::npos &&
                          host_address.front() != '[';
        const std::string host = ipv6 ? "[" + host_address + "]" : host_address;
        const std::string base_url =
            "https://" + host + ":" + std::to_string(https_port) + "/cosmic/file";

        while (true) {
            std::string path;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait(lock, [] {
                    return g_cancel_requested.load(std::memory_order_relaxed) ||
                           !g_queue.empty();
                });
                if (g_cancel_requested.load(std::memory_order_relaxed)) {
                    break;
                }
                path = std::move(g_queue.front());
                g_queue.pop_front();
            }
            // Uploaded with the mutex released: it guards a snapshot, never a
            // request, so progress() and enqueue() stay non-blocking.
            //
            // Wrapped per-file rather than letting an exception reach the
            // outer catch: std::filesystem::path's char8_t constructor
            // throws filesystem_error for a byte sequence that is not valid
            // UTF-8, and the chunk buffer's std::vector can throw bad_alloc.
            // Either one must fail just this file, not end the worker (which
            // would otherwise silently strand every path queued after it —
            // see ClearRunningOnExit and g_running above for what used to
            // happen instead).
            UploadResult result;
            try {
                result = UploadFile(curl, base_url, path, chunk_headers.get());
            } catch (...) {
                PublishError(kErrorFailed);
                result = UploadResult::Failed;
            }
            if (result == UploadResult::Cancelled) {
                break;
            }
        }

        // curl_owner's and chunk_headers' destructors release the handle and
        // the header list here (and on every other exit path from this try
        // block, including an exception).
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
        g_queue.clear();
        g_progress = Progress{};
        g_running = true;
    }
    g_cancel_requested.store(false, std::memory_order_relaxed);
    g_worker = std::thread(WorkerLoop, host_address, https_port);
}

void stop() {
    {
        // The flag is raised under the mutex, unlike clipsync's plain store,
        // because the worker sleeps on a condition variable: a store between
        // the worker's predicate check and its wait would otherwise be
        // notified into an empty room and the join would hang until the next
        // enqueue.
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cancel_requested.store(true, std::memory_order_relaxed);
        g_running = false;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
    // Cleared after join(), not before: the worker is provably gone by this
    // point, so there is no race with an in-flight pop. A cancelled transfer
    // leaves no sticky error to clear here anyway (the host deletes its own
    // partial file when the stream tears down), but a finished one's snapshot
    // must not survive into the next session's overlay.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
    g_progress = Progress{};
}

bool enqueue(const std::string& utf8_path) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) {
            g_progress.error = kErrorNotStreaming;
            g_progress.updated_ms = NowMs();
            return false;
        }
        if (g_queue.size() >= static_cast<std::size_t>(kMaxQueued)) {
            g_progress.error = kErrorQueueFull;
            g_progress.updated_ms = NowMs();
            return false;
        }
        g_queue.push_back(utf8_path);
        // The file being uploaded right now was already popped, so the queue
        // depth is exactly the "still waiting" count the overlay wants.
        g_progress.queued = static_cast<int>(g_queue.size());
        g_progress.updated_ms = NowMs();
    }
    g_cv.notify_one();
    return true;
}

Progress progress() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_progress;
}

}  // namespace cosmic::filesync
