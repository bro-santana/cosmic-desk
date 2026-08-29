// Cosmic Desk — host wallpaper cache (PLAN.md D10(d)(e), milestone W2 item 2).
//
// See wallcache.h for the cache layout and the one-download-per-pass rule.

#include "app/wallcache.h"

#include "app/settings.h"

#include <curl/curl.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>

namespace cosmic::wallcache {

namespace {

// A hostile or misbehaving host must not make sync() buffer an unbounded
// response; Windows' TranscodedWallpaper is a composited JPEG well under this
// (PLAN.md D10b caps the host's own response the same way).
constexpr size_t kMaxImageBytes = 8 * 1024 * 1024;
constexpr size_t kHashLen = 64;

// Guards g_index and g_forget_seq, which are written by sync() (presence
// worker thread) and read/written by path_for()/forget() (UI thread).
std::mutex g_mutex;
std::map<std::string, std::filesystem::path> g_index;  // address -> cached path ("" = known none)

// Bumped by forget() so an in-flight sync() download can detect that its
// target address was removed mid-transfer and discard the write instead of
// resurrecting a file forget() already ran (and found nothing to delete, since
// the write hadn't landed yet). See sync()'s use below.
size_t g_forget_seq = 0;

// Set by cancel(); cleared only by reset(), before a fresh worker is
// spawned — never by sync() (see reset()'s doc comment for why). Checked by
// the curl progress callback below so a shutdown can abort an in-flight
// download instead of waiting out its ~35 s connect+transfer timeout budget.
std::atomic<bool> g_cancel_requested{false};

// Remembers the address of the last download attempt (success or failure) so
// sync() can round-robin its scan's starting point next pass, instead of
// always starting from the front — otherwise one persistently failing host
// early in the target list would starve every host after it. Only ever
// touched from sync(), which per wallcache.h runs on the presence worker's
// single thread — no lock needed.
std::string g_last_attempted_address;

// Only ever touched from sync(), which per wallcache.h is only ever called
// from the presence worker thread — no lock needed for the handle itself.
// curl_global_init is presence.cpp's responsibility (called once before its
// worker thread starts, which is always before sync() can run); this module
// must not call it again.
CURL* g_curl = nullptr;

std::filesystem::path WallpapersDir() {
    return cosmic::Settings::config_dir() / "wallpapers";
}

// The hash comes from a remote host's /serverinfo and lands in a filename;
// presence.cpp already validates it, but a filename-bound value is
// re-validated here rather than trusted from the caller.
bool ValidHash(const std::string& hash) {
    if (hash.size() != kHashLen) {
        return false;
    }
    for (char c : hash) {
        const bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) {
            return false;
        }
    }
    return true;
}

// Replaces every character outside [A-Za-z0-9._-] with '_' — covers ':',
// '/', '\\', '[', ']', all invalid or dangerous in a Windows filename.
std::string SanitizeAddress(const std::string& address) {
    std::string out = address;
    for (char& c : out) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            c = '_';
        }
    }
    return out;
}

// 32-bit FNV-1a. Not cryptographic — just a short, deterministic fingerprint
// of the raw address to fold into the cache filename (see CacheStem).
uint32_t Fnv1a32(const std::string& s) {
    uint32_t hash = 0x811c9dc5u;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 0x01000193u;
    }
    return hash;
}

// The one function that produces the filesystem-safe identifier for
// `address` used by every cache filename: write, prune, and both scans
// (ScanForCached, forget) all derive it from here so they can never disagree.
//
// <sanitized-address>-<8 lowercase hex fnv1a32(address)>: the hash covers the
// raw, pre-sanitization address, so two addresses that sanitize to the same
// string (e.g. "a:b" and "a_b" both becoming "a_b") get different stems
// instead of cross-serving/cross-deleting each other's wallpaper. The
// "-XXXXXXXX" suffix also guarantees the stem can never exactly equal a
// Win32 reserved device basename (CON, NUL, COM1, ...), which would
// otherwise open a device instead of creating a file.
std::string CacheStem(const std::string& address) {
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", Fnv1a32(address));
    return SanitizeAddress(address) + "-" + hex;
}

std::filesystem::path ExpectedPath(const std::string& stem, const std::string& hash) {
    return WallpapersDir() / (stem + "." + hash + ".img");
}

// Appends the response body, aborting the transfer once it exceeds
// kMaxImageBytes by returning fewer bytes than were provided (libcurl treats
// a short count as a write error and aborts with CURLE_WRITE_ERROR). Runs
// inside libcurl's C frames, so no exception may escape this frame.
size_t WriteImageBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
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

// JPEG (FF D8), PNG (89 50 4E 47) or BMP (42 4D) magic bytes. An unauthorized
// host still answers 200 OK — Sunshine's not-found/verify-failed paths write
// a 200 status line with a 404-shaped XML body — so CURLOPT_FAILONERROR would
// not catch it; this magic-byte check is the real gate against that body.
bool LooksLikeImage(const std::string& body) {
    if (body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0xFF &&
        static_cast<unsigned char>(body[1]) == 0xD8) {
        return true;
    }
    if (body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0x89 &&
        body[1] == 'P' && body[2] == 'N' && body[3] == 'G') {
        return true;
    }
    if (body.size() >= 2 && body[0] == 'B' && body[1] == 'M') {
        return true;
    }
    return false;
}

// Checked by curl during a transfer (CURLOPT_XFERINFOFUNCTION below); a
// non-zero return aborts the transfer with CURLE_ABORTED_BY_CALLBACK, which
// DownloadOnce's CURLE_OK check then turns into an ordinary failed download
// (nothing written, nothing logged) — see cancel().
int ProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_cancel_requested.load(std::memory_order_relaxed) ? 1 : 0;
}

// Creates and configures the shared easy handle on first use.
CURL* EnsureCurl() {
    if (g_curl != nullptr) {
        return g_curl;
    }

    g_curl = curl_easy_init();
    if (g_curl == nullptr) {
        std::fprintf(stderr, "wallcache: curl_easy_init failed; downloads disabled.\n");
        return nullptr;
    }

    // Same self-signed Moonlight trust model as
    // third-party/libgamestream/http.c:76-94, duplicated (not shared) because
    // that handle is a single global owned by the viewer session worker
    // (pairing/applist/launch) — a fetch from the presence thread would race it.
    const std::filesystem::path client_dir = cosmic::Settings::config_dir() / "client";
    // libcurl has copied string options like CURLOPT_SSLCERT/CURLOPT_SSLKEY
    // since 7.17.0, so these locals do not need to outlive this call (same
    // reason the local `url` in DownloadOnce is fine to pass by c_str()).
    const std::string cert_path = (client_dir / "client.pem").string();
    const std::string key_path = (client_dir / "key.pem").string();
    curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(g_curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(g_curl, CURLOPT_SSLKEYTYPE, "PEM");
    curl_easy_setopt(g_curl, CURLOPT_SSLCERT, cert_path.c_str());
    curl_easy_setopt(g_curl, CURLOPT_SSLKEY, key_path.c_str());
    curl_easy_setopt(g_curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
    // Lets cancel() (the shutdown path) abort an in-flight transfer instead of
    // presence::stop() blocking on its full timeout budget while joining.
    curl_easy_setopt(g_curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(g_curl, CURLOPT_NOPROGRESS, 0L);

    return g_curl;
}

// Writes `body` to `dest` atomically: full write to a .tmp sibling, then
// rename over `dest` (replaces an existing file per the standard). Killing
// the host mid-download must never leave a partial .img; any failure cleans
// up the temp file.
bool WriteAtomic(const std::filesystem::path& dest, const std::string& body) {
    std::filesystem::path tmp = dest;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

// Downloads the wallpaper for `target` over its authenticated HTTPS port and
// atomically writes it to `dest`. A failed or rejected download writes
// nothing and logs nothing per attempt — an unpaired host polled every ~10 s
// must not produce error spam.
bool DownloadOnce(const Target& target, const std::filesystem::path& dest) {
    CURL* curl = EnsureCurl();
    if (curl == nullptr) {
        return false;
    }

    // IPv6 literals must be bracketed in a URL; IPv4/hostnames are not (mirrors
    // presence.cpp's poll loop). A user-supplied bracketed form is kept as-is.
    const bool ipv6 = target.address.find(':') != std::string::npos &&
                      target.address.front() != '[';
    const std::string host = ipv6 ? "[" + target.address + "]" : target.address;
    const std::string https_port = std::to_string(target.http_port - 5);
    const std::string url = "https://" + host + ":" + https_port + "/cosmic/wallpaper";

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteImageBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK || http_code != 200 || !LooksLikeImage(body)) {
        return false;
    }

    return WriteAtomic(dest, body);
}

// Deletes every <stem>.* file in the wallpapers directory except `keep` (if
// given). Used both to prune stale hashes/leftover .tmp files after a
// successful write, and by forget() to drop everything.
void PruneMatching(const std::string& stem, const std::filesystem::path& keep) {
    const std::string prefix = stem + ".";
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(WallpapersDir(), ec);
    if (ec) {
        return;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const std::filesystem::path entry_path = it->path();
        const std::string filename = entry_path.filename().string();
        if (filename.rfind(prefix, 0) != 0) {
            continue;
        }
        if (!keep.empty() && entry_path == keep) {
            continue;
        }
        std::error_code rm_ec;
        std::filesystem::remove(entry_path, rm_ec);
    }
}

// Scans the wallpapers directory once for a file whose name starts with
// `<stem>.` and ends with `.img`. The `.` after the stem is required so
// "10.0.0.1" never matches "10.0.0.11"'s files (their stems already differ
// via CacheStem's hash suffix, but the separator is kept as a second guard).
std::filesystem::path ScanForCached(const std::string& stem) {
    const std::string prefix = stem + ".";
    static constexpr char kSuffix[] = ".img";
    static constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;

    std::error_code ec;
    auto it = std::filesystem::directory_iterator(WallpapersDir(), ec);
    if (ec) {
        return {};
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const std::string filename = it->path().filename().string();
        if (filename.rfind(prefix, 0) != 0) {
            continue;
        }
        if (filename.size() >= kSuffixLen &&
            filename.compare(filename.size() - kSuffixLen, kSuffixLen, kSuffix) == 0) {
            return it->path();
        }
    }
    return {};
}

}  // namespace

void sync(const std::vector<Target>& targets) {
    // Filesystem calls below all use std::error_code overloads, but this is a
    // hard backstop: sync() runs inside the presence worker's poll loop and
    // must never throw out of it.
    try {
        // Deliberately does NOT clear g_cancel_requested here: a cancel() can
        // land between the worker reading g_running and this call (see
        // presence.cpp's guard), and clearing it on entry would erase that
        // cancel and make the in-flight download uncancellable again. Only
        // reset() (called by start(), before a fresh worker is spawned) may
        // clear it.
        if (targets.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(WallpapersDir(), ec);

        // Round-robin start point: resume just after the last address that
        // triggered a download attempt, wrapping around, so a persistently
        // failing host near the front of the list cannot starve every host
        // after it. A host whose *write* keeps failing still only retries on
        // its turn — bounded to one attempt per (up to) targets.size() passes,
        // which is the accepted behaviour for now.
        size_t start_index = 0;
        if (!g_last_attempted_address.empty()) {
            for (size_t i = 0; i < targets.size(); ++i) {
                if (targets[i].address == g_last_attempted_address) {
                    start_index = (i + 1) % targets.size();
                    break;
                }
            }
        }

        for (size_t offset = 0; offset < targets.size(); ++offset) {
            const Target& target = targets[(start_index + offset) % targets.size()];
            if (!ValidHash(target.wallpaper_hash)) {
                continue;
            }
            if (target.http_port - 5 <= 0) {
                continue;
            }

            const std::string stem = CacheStem(target.address);
            const std::filesystem::path expected = ExpectedPath(stem, target.wallpaper_hash);

            std::error_code exists_ec;
            if (std::filesystem::exists(expected, exists_ec) && !exists_ec) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_index[target.address] = expected;
                continue;
            }

            // Otherwise download it, and stop after this attempt (whether it
            // succeeds or fails) so at most one transfer happens per pass.
            g_last_attempted_address = target.address;

            // Snapshotted before the (possibly slow) download so a forget()
            // landing anywhere while it is in flight is detected below.
            size_t forget_seq_before;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                forget_seq_before = g_forget_seq;
            }

            if (DownloadOnce(target, expected)) {
                // The lock only guards the g_forget_seq check and index
                // update; PruneMatching's directory scan + unlinks run after
                // it is released (below) so a UI-thread path_for() cannot
                // block on them. Idempotent either way: a forget() racing
                // this can only delete files this call would have kept.
                bool discarded = false;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (g_forget_seq == forget_seq_before) {
                        g_index[target.address] = expected;
                    } else {
                        // Some address was forgotten while this download was
                        // in flight: forget() may have already scanned and
                        // found nothing (the write hadn't landed yet), so
                        // this write would otherwise persist forever
                        // uncleaned. Conservative and simple: any removal
                        // during a transfer discards that transfer, even if
                        // it was for a different address.
                        discarded = true;
                    }
                }
                if (discarded) {
                    std::error_code rm_ec;
                    std::filesystem::remove(expected, rm_ec);
                } else {
                    PruneMatching(stem, expected);
                }
            }
            return;
        }
    } catch (...) {
    }
}

std::filesystem::path path_for(const std::string& address) {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_index.find(address);
        if (it != g_index.end()) {
            return it->second;
        }
        const std::filesystem::path found = ScanForCached(CacheStem(address));
        g_index[address] = found;  // caches the negative too
        return found;
    } catch (...) {
        return {};
    }
}

void forget(const std::string& address) {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_forget_seq;
        PruneMatching(CacheStem(address), std::filesystem::path());
        g_index.erase(address);
    } catch (...) {
    }
}

void cancel() {
    g_cancel_requested.store(true, std::memory_order_relaxed);
}

void reset() {
    g_cancel_requested.store(false, std::memory_order_relaxed);
}

}  // namespace cosmic::wallcache
