// Cosmic Desk — file receiver implementation. See filerecv.h for the contract.

#include "hostglue/filerecv.h"

#ifdef _WIN32
  #include <windows.h>
  #include <sddl.h>
  #include <shlobj.h>
  #include <wtsapi32.h>
#else
  #include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>

namespace cosmic::filerecv {
namespace {

std::atomic<bool> g_enabled {true};

// Guards g_active in full, including the open ofstream, for the duration of
// every begin/chunk/end/abort/tick call. Correctness first: state and stream
// are one unit, and a single chunk write is bounded (kMaxChunkBytes), so
// holding the lock across it does not stall other callers for meaningfully
// long. begin/chunk/end/abort run on Sunshine HTTP handler threads; tick()
// and set_enabled() run on the app main thread.
std::mutex g_mutex;

// At most one transfer in flight at a time (plan-mandated). part_path is
// where bytes land as they arrive; final_path is where end() renames it.
struct Transfer {
  std::string id;
  std::filesystem::path part_path;
  std::filesystem::path final_path;
  std::uint64_t declared_size = 0;
  std::uint64_t received = 0;
  std::ofstream out;
  std::chrono::steady_clock::time_point last_activity;
};
std::optional<Transfer> g_active;

// ---------------------------------------------------------------------------
// sanitize_filename helpers
// ---------------------------------------------------------------------------

void strip_trailing_dot_space(std::string &s) {
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) {
    s.pop_back();
  }
}

// Applies the "empty becomes file" rule, shared by the first pass and the
// post-truncation re-check in sanitize_filename. Both call sites run this
// immediately after strip_trailing_dot_space(), which already reduces any
// all-dot input to "", so there is no separate all-dots case left to check
// here.
void collapse_to_file_if_degenerate(std::string &s) {
  if (s.empty()) {
    s = "file";
  }
}

bool ascii_ieq(const std::string &a, const char *b) {
  const std::size_t len = std::char_traits<char>::length(b);
  if (a.size() != len) {
    return false;
  }
  for (std::size_t i = 0; i < len; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Reserved MS-DOS device names: writing to one of these under any extension
// (CON.txt, lpt9.tar.gz, ...) opens the device instead of a regular file.
bool is_reserved_device_stem(const std::string &stem) {
  static const char *const kReserved[] = {
      "CON", "PRN", "AUX", "NUL",
      "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
  };
  for (const char *reserved : kReserved) {
    if (ascii_ieq(stem, reserved)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Destination directory resolution
// ---------------------------------------------------------------------------

#ifdef _WIN32

// WTSQuerySessionInformationW wrapper: copies the result and frees the WTS
// buffer on every path. Returns "" if the session has no value for
// info_class. Mirrors wallpaper.cpp's helper of the same name/shape.
std::wstring wts_query_string(DWORD session_id, WTS_INFO_CLASS info_class) {
  LPWSTR buffer = nullptr;
  DWORD bytes = 0;
  if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, info_class, &buffer, &bytes) ||
      buffer == nullptr) {
    return L"";
  }
  std::wstring result(buffer);
  WTSFreeMemory(buffer);
  return result;
}

// Resolves the console session's user profile root via WTS account name ->
// LookupAccountNameW -> SID -> HKLM ProfileList -> ProfileImagePath, without
// impersonation (WTSQueryUserToken needs SeTcbPrivilege and a token handle to
// clean up). Returns an empty path on any failed step.
//
// This is a deliberate near-duplicate of resolve_fallback_path in
// src/hostglue/wallpaper.cpp:112 -- that helper is private to its TU and
// resolves TranscodedWallpaper specifically, not a directory filerecv can
// reuse. Copying and adapting it here (stop at the profile root instead of
// descending into AppData\Roaming\...\Themes) is approved; keep both copies
// of the WTS/SID/registry sequence in sync if that sequence ever needs to
// change.
std::filesystem::path resolve_console_profile_path(DWORD session_id) {
  const std::wstring user = wts_query_string(session_id, WTSUserName);
  if (user.empty()) {
    return {};
  }
  const std::wstring domain = wts_query_string(session_id, WTSDomainName);
  const std::wstring account = domain.empty() ? user : domain + L"\\" + user;

  BYTE sid[SECURITY_MAX_SID_SIZE];
  DWORD sid_size = sizeof(sid);
  wchar_t referenced_domain[256];
  DWORD referenced_domain_size = 256;
  SID_NAME_USE use;
  if (!LookupAccountNameW(nullptr, account.c_str(), sid, &sid_size, referenced_domain,
                           &referenced_domain_size, &use)) {
    return {};
  }

  LPWSTR sid_string = nullptr;
  if (!ConvertSidToStringSidW(sid, &sid_string)) {
    return {};
  }
  const std::wstring key_path =
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + std::wstring(sid_string);
  LocalFree(sid_string);

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t raw_profile[MAX_PATH] = {};
  DWORD raw_profile_bytes = sizeof(raw_profile);
  DWORD type = 0;
  const LONG status = RegQueryValueExW(key, L"ProfileImagePath", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(raw_profile), &raw_profile_bytes);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_EXPAND_SZ || raw_profile_bytes == 0 ||
      raw_profile_bytes % sizeof(wchar_t) != 0) {
    return {};
  }
  // RegQueryValueExW does not guarantee NUL-termination; force it at the
  // reported length so a value that exactly fills raw_profile cannot leave
  // ExpandEnvironmentStringsW reading past the end of the array.
  raw_profile[std::min<DWORD>(raw_profile_bytes / sizeof(wchar_t), MAX_PATH - 1)] = L'\0';

  wchar_t profile[MAX_PATH] = {};
  const DWORD expanded_chars = ExpandEnvironmentStringsW(raw_profile, profile, MAX_PATH);
  if (expanded_chars == 0 || expanded_chars > MAX_PATH) {
    // 0 is failure; greater than MAX_PATH means profile was too small and
    // its contents are unspecified -- both are unusable.
    return {};
  }
  return std::filesystem::path(profile);
}

// Resolved fresh on every begin() -- these syscalls are cheap and a
// transfer is a rare, non-hot-path event. Returns an empty path on any
// failed step; begin() maps that to BeginResult::IoError.
std::filesystem::path downloads_dir() {
  DWORD our_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &our_session)) {
    // Mirrors wallpaper.cpp's resolve_fallback_path: without our own session
    // id we cannot tell console from off-session apart, so fail closed
    // rather than falling through to SHGetKnownFolderPath -- the SYSTEM-
    // profile outcome the console-session branch below exists to prevent.
    return {};
  }
  const DWORD console_session = WTSGetActiveConsoleSessionId();
  const bool use_console_profile = console_session != 0xFFFFFFFF && console_session != our_session;

  std::filesystem::path downloads;
  if (use_console_profile) {
    // Service/SYSTEM run: FOLDERID_Downloads below would resolve to the
    // SYSTEM profile, so resolve the console user's profile instead. If that
    // fails, do NOT fall back to SHGetKnownFolderPath -- that would silently
    // write into the wrong (service) profile's Downloads.
    const std::filesystem::path profile = resolve_console_profile_path(console_session);
    if (profile.empty()) {
      return {};
    }
    downloads = profile / L"Downloads";
  } else {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, nullptr, &p))) {
      return {};
    }
    downloads = std::filesystem::path(p);
    CoTaskMemFree(p);
  }
  return downloads / L"CosmicDesk";
}

#else  // !_WIN32

std::filesystem::path downloads_dir() {
  const char *home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  return std::filesystem::path(home) / "Downloads" / "CosmicDesk";
}

#endif

// ---------------------------------------------------------------------------
// Transfer bookkeeping helpers
// ---------------------------------------------------------------------------

// 16 lowercase hex chars from a 64-bit value. Called only from begin() while
// g_mutex is already held, so the static generator needs no lock of its own.
// The high 32 bits come from the seeded RNG and the low 32 bits from a
// monotonic counter; keeping the two in disjoint bit ranges (rather than
// XORing them) is what actually delivers uniqueness within this process --
// XOR alone is not injective and mt19937_64 can repeat a 64-bit output, but
// the counter half can never repeat within a process lifetime.
std::string generate_id() {
  static std::mt19937_64 rng {std::random_device {}()};
  static std::uint64_t counter = 0;
  std::uint64_t value = (rng() & 0xFFFFFFFF00000000ull) | (++counter & 0xFFFFFFFFull);

  static const char kHexDigits[] = "0123456789abcdef";
  std::string id(16, '0');
  for (int i = 15; i >= 0; --i) {
    id[static_cast<std::size_t>(i)] = kHexDigits[value & 0xF];
    value >>= 4;
  }
  return id;
}

// Splits a sanitized name into stem/extension the way Explorer's " (2)"
// collision suffix does: the extension is the text from the last '.', when
// that '.' is not the first byte (so dotfiles like ".bashrc" have no
// extension for this purpose).
void split_stem_ext(const std::string &name, std::string &stem, std::string &ext) {
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) {
    stem = name;
    ext.clear();
    return;
  }
  stem = name.substr(0, dot);
  ext = name.substr(dot);
}

// Finds dir/name, or a " (2)"/" (3)"/... suffixed variant, such that neither
// the final file nor a same-named ".part" already exists. Checks both so a
// finished file from a previous transfer and an in-progress one never
// collide. Returns an empty path after 999 attempts.
std::filesystem::path resolve_free_path(const std::filesystem::path &dir, const std::string &name) {
  std::string stem, ext;
  split_stem_ext(name, stem, ext);

  for (int n = 1; n <= 999; ++n) {
    const std::string candidate = (n == 1) ? name : stem + " (" + std::to_string(n) + ")" + ext;
    std::filesystem::path final_path = dir / candidate;
    std::filesystem::path part_path = final_path;
    part_path += ".part";

    std::error_code ec;
    const bool final_exists = std::filesystem::exists(final_path, ec);
    ec.clear();
    const bool part_exists = std::filesystem::exists(part_path, ec);
    // A stat failure (ec set) must read as "taken", not "free" -- otherwise
    // begin() opens the candidate with std::ios::trunc, truncating the very
    // file this check exists to protect.
    if (ec || final_exists || part_exists) {
      continue;
    }
    return final_path;
  }
  return {};
}

#ifdef _WIN32

// Moves src to dst, never replacing an existing dst. On failure, sets
// *dst_exists when the failure was specifically because dst already exists
// (the caller can then re-resolve a fresh name and retry) and leaves it
// false for any other failure (caller gives up).
//
// This -- not any exists() check the caller may have already done -- is what
// actually enforces the module's never-overwrite guarantee: MOVEFILE_
// REPLACE_EXISTING is deliberately omitted, so the call itself fails
// atomically if dst is created by anything else between a check and here,
// closing the TOCTOU window that a separate exists()-then-rename cannot.
bool move_no_replace(const std::filesystem::path &src, const std::filesystem::path &dst, bool &dst_exists) {
  dst_exists = false;
  if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  const DWORD err = GetLastError();
  dst_exists = (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS);
  return false;
}

#else  // !_WIN32

// Same contract as the _WIN32 overload above. link() fails with EEXIST
// rather than replacing dst when it already exists (unlike rename()), so it
// -- not any preceding exists() check -- is what actually enforces
// never-overwrite here. src is only unlinked once the link has succeeded.
bool move_no_replace(const std::filesystem::path &src, const std::filesystem::path &dst, bool &dst_exists) {
  dst_exists = false;
  if (::link(src.c_str(), dst.c_str()) != 0) {
    dst_exists = (errno == EEXIST);
    return false;
  }
  ::unlink(src.c_str());
  return true;
}

#endif  // _WIN32

// Closes the stream (if open) and deletes the partial file, ignoring errors
// on both -- called only to tear down a transfer that is being abandoned
// anyway, on every path (success or failure).
void close_and_delete_part(Transfer &t) {
  if (t.out.is_open()) {
    t.out.close();
  }
  std::error_code ec;
  std::filesystem::remove(t.part_path, ec);
}

}  // namespace

void set_enabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    abort_all();
  }
}

bool enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

// HTTP handler threads call begin/chunk/end/abort below; std::filesystem
// calls use the std::error_code overloads throughout, but a handful of other
// operations here (string/path construction, std::random_device) can in
// theory still throw. Each public function is wrapped in try/catch(...) so
// nothing escapes to the caller; a caught exception is treated as the
// function's IoError/false/unknown-id outcome, whichever applies.
BeginResult begin(const std::string &raw_name, std::uint64_t declared_size, std::string &out_id) {
  // Tracked outside the try so a caught exception thrown after the .part is
  // opened (e.g. generate_id(), a path/string copy) can still delete it --
  // otherwise it would leak a zero-byte file, since the local Transfer that
  // owns it is already gone by the time the catch below runs. Captured
  // before t.out.open() below (not after), so a throw from the capture
  // itself never leaves a created file untracked -- if it throws, open()
  // never runs and there is nothing on disk yet to clean up.
  std::filesystem::path opened_part_path;
  try {
    const std::string name = sanitize_filename(raw_name);
    if (name.empty()) {
      return BeginResult::BadRequest;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) {
      return BeginResult::Busy;
    }

    const std::filesystem::path dir = downloads_dir();
    if (dir.empty()) {
      return BeginResult::IoError;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
      return BeginResult::IoError;
    }

    ec.clear();
    const auto space = std::filesystem::space(dir, ec);
    if (ec) {
      // NoSpace is defined as "declared_size > available"; a space() failure
      // is a different problem and must not be misreported as one.
      return BeginResult::IoError;
    }
    if (declared_size > space.available) {
      return BeginResult::NoSpace;
    }

    const std::filesystem::path final_path = resolve_free_path(dir, name);
    if (final_path.empty()) {
      return BeginResult::BadRequest;
    }
    std::filesystem::path part_path = final_path;
    part_path += ".part";

    Transfer t;
    opened_part_path = part_path;
    t.out.open(part_path, std::ios::binary | std::ios::trunc);
    if (!t.out) {
      return BeginResult::IoError;
    }
    t.id = generate_id();
    t.part_path = std::move(part_path);
    t.final_path = final_path;
    t.declared_size = declared_size;
    t.received = 0;
    t.last_activity = std::chrono::steady_clock::now();

    out_id = t.id;
    g_active = std::move(t);
    return BeginResult::Ok;
  } catch (...) {
    // g_active is assigned exactly once, in the last statement of the try
    // block above, and std::optional's assignment leaves *this disengaged if
    // constructing the new value throws -- so a throw can never leave
    // g_active half-set for this catch to clean up. What it can leave behind
    // is the zero-byte .part opened above; delete it the same way the
    // explicit IoError paths do.
    if (!opened_part_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(opened_part_path, ec);
    }
    return BeginResult::IoError;
  }
}

ChunkResult chunk(const std::string &id, std::uint64_t offset, const char *data, std::size_t len) {
  // Lock held for the whole function (not just inside the try below) so the
  // catch(...) handler can still safely tear down g_active if something
  // throws partway through.
  std::lock_guard<std::mutex> lock(g_mutex);
  try {
    if (!g_active || g_active->id != id) {
      return ChunkResult::UnknownId;
    }
    // Checked after UnknownId (and inside the lock), not before it, so an
    // oversized chunk against no active transfer reads as UnknownId (404),
    // matching the route design's documented precedence.
    if (len > kMaxChunkBytes) {
      return ChunkResult::TooLarge;
    }
    Transfer &t = *g_active;
    if (offset != t.received || t.received + len > t.declared_size) {
      return ChunkResult::BadOffset;
    }

    t.out.write(data, static_cast<std::streamsize>(len));
    if (!t.out) {
      close_and_delete_part(t);
      g_active.reset();
      return ChunkResult::IoError;
    }
    t.received += len;
    t.last_activity = std::chrono::steady_clock::now();
    return ChunkResult::Ok;
  } catch (...) {
    // Matches filerecv.h's contract that any chunk() failure aborts the
    // transfer and deletes its partial file -- without this, a throw here
    // would leave g_active set, the stream open, and the .part on disk until
    // tick() reaps it up to kIdleTimeoutMs later.
    if (g_active) {
      close_and_delete_part(*g_active);
      g_active.reset();
    }
    return ChunkResult::IoError;
  }
}

EndResult end(const std::string &id) {
  // Tracked outside the try, like begin()'s opened_part_path, so a caught
  // exception thrown after t is moved out of g_active (e.g. t.out.flush() or
  // t.out.close() below) can still delete the partial file -- otherwise it
  // would leak on disk with no active transfer left for tick() to reap. t
  // itself is gone by the time the catch runs (stack unwinding destructs it,
  // which closes t.out if still open), so only the path needs to survive.
  std::filesystem::path moved_part_path;
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active || g_active->id != id) {
      return EndResult::UnknownId;
    }
    // Captured from g_active itself, before the move+reset below, so a throw
    // from this copy (a std::filesystem::path assignment can allocate) still
    // leaves g_active fully intact for tick() to reap. This also means
    // moved_part_path is already correct even if the move or reset that
    // follow were themselves to throw -- capturing from t after the
    // move+reset, as before, would instead leave g_active already disengaged
    // and moved_part_path empty, orphaning the .part with nothing left to
    // reap it.
    moved_part_path = g_active->part_path;
    Transfer t = std::move(*g_active);
    g_active.reset();

    if (t.received != t.declared_size) {
      close_and_delete_part(t);
      return EndResult::SizeMismatch;
    }

    t.out.flush();
    t.out.close();
    if (!t.out) {
      std::error_code ec;
      std::filesystem::remove(t.part_path, ec);
      return EndResult::IoError;
    }

    // move_no_replace() below -- not an exists() check here -- is what
    // actually enforces the module's never-overwrite guarantee (see its
    // definition above): a plain exists()-then-rename leaves a TOCTOU window
    // in which anything that creates final_path between the two calls gets
    // silently clobbered by std::filesystem::rename, and exists() itself can
    // fail without that failure being noticed. Try the final_path begin()
    // originally resolved first; on a collision, re-resolve a fresh suffixed
    // name and retry, bounded the same as resolve_free_path()'s own
    // collision loop.
    std::filesystem::path final_path = t.final_path;
    for (int attempt = 1; attempt <= 999; ++attempt) {
      bool dst_exists = false;
      if (move_no_replace(t.part_path, final_path, dst_exists)) {
        return EndResult::Ok;
      }
      if (!dst_exists) {
        std::error_code ec;
        std::filesystem::remove(t.part_path, ec);
        return EndResult::IoError;
      }
      final_path = resolve_free_path(t.final_path.parent_path(), t.final_path.filename().string());
      if (final_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(t.part_path, ec);
        return EndResult::IoError;
      }
    }
    std::error_code ec;
    std::filesystem::remove(t.part_path, ec);
    return EndResult::IoError;
  } catch (...) {
    if (!moved_part_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(moved_part_path, ec);
    }
    return EndResult::IoError;
  }
}

bool abort(const std::string &id) {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active || g_active->id != id) {
      return false;
    }
    close_and_delete_part(*g_active);
    g_active.reset();
    return true;
  } catch (...) {
    return false;
  }
}

void abort_all() {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) {
      close_and_delete_part(*g_active);
      g_active.reset();
    }
  } catch (...) {
    // Best-effort: nothing more this function can report to its caller.
  }
}

void tick() {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) {
      return;
    }
    const auto idle = std::chrono::steady_clock::now() - g_active->last_activity;
    if (idle > std::chrono::milliseconds(kIdleTimeoutMs)) {
      close_and_delete_part(*g_active);
      g_active.reset();
    }
  } catch (...) {
    // Best-effort: nothing more this function can report to its caller.
  }
}

std::string sanitize_filename(const std::string &raw) {
  // A pure string-munging function has no filesystem calls to catch via
  // std::error_code, but a std::string operation can still throw bad_alloc;
  // wrapped like the other public functions here so nothing escapes. "" is
  // already this function's "refuse" outcome, so it doubles as the safe
  // fallback on a caught exception.
  try {
    // a) Basename only: drop everything up to and including the last
    // separator, so "..\\..\\evil.exe" and "a/b/c.txt" both become just the
    // last path component.
    const std::size_t slash = raw.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? raw : raw.substr(slash + 1);

    // b) Neutralize control characters and the characters NTFS/Windows treat
    // specially. ':' in particular is the alternate-data-stream separator
    // (e.g. "file.txt:zone") and must go just like the path separators.
    static const std::string kBadChars = "<>:\"/\\|?*";
    for (char &c : name) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (uc < 0x20 || uc == 0x7F || kBadChars.find(c) != std::string::npos) {
        c = '_';
      }
    }

    // c) + d) Strip trailing dots/spaces (Windows silently drops these
    // itself, and a lone "." or ".." would otherwise be a no-op or
    // traversal-shaped name); an empty or all-dot result falls back to
    // "file".
    strip_trailing_dot_space(name);
    collapse_to_file_if_degenerate(name);

    // e) Cap to 200 bytes without splitting a UTF-8 code point, keeping a
    // short trailing extension when there is one.
    constexpr std::size_t kMaxNameBytes = 200;
    constexpr std::size_t kMaxExtBytes = 20;
    if (name.size() > kMaxNameBytes) {
      std::string stem, ext;
      split_stem_ext(name, stem, ext);
      if (ext.size() > kMaxExtBytes) {
        // Not a "short extension" by this rule -- treat the whole name as
        // stem so truncation still lands on kMaxNameBytes total.
        stem = name;
        ext.clear();
      }

      const std::size_t stem_budget = kMaxNameBytes - ext.size();
      if (stem.size() > stem_budget) {
        std::size_t cut = stem_budget;
        // Back off while the byte right after the cut is a UTF-8
        // continuation byte (0b10xxxxxx) -- that means the cut lands inside
        // a multi-byte sequence, so drop the whole partial sequence rather
        // than emit invalid UTF-8.
        while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) {
          --cut;
        }
        stem = stem.substr(0, cut);
      }
      name = stem + ext;

      // Truncation can newly expose a trailing dot/space or collapse the
      // name to nothing; re-apply the same fixups.
      strip_trailing_dot_space(name);
      collapse_to_file_if_degenerate(name);
    }

    // f) LAST: refuse reserved Windows device names outright. Unlike (d)'s
    // "empty becomes file", this is the one case where sanitize_filename
    // returns "" -- meaning "refuse this upload", not "use a fallback name".
    // The device check keys on the text before the FIRST dot (so
    // "lpt9.tar.gz" is still recognized as LPT9), unlike split_stem_ext's
    // last-dot split used above for the extension-preserving truncation.
    const std::size_t first_dot = name.find('.');
    const std::string device_stem = (first_dot == std::string::npos) ? name : name.substr(0, first_dot);
    if (is_reserved_device_stem(device_stem)) {
      return "";
    }

    return name;
  } catch (...) {
    return "";
  }
}

}  // namespace cosmic::filerecv
