// Cosmic Desk — wallpaper implementation. See wallpaper.h for the contract.

#include "hostglue/wallpaper.h"

// Vendored Sunshine header; only this TU needs it (wallpaper.h stays clean).
#include "src/crypto.h"

#ifdef _WIN32
  #include <windows.h>
  #include <sddl.h>
  #include <wtsapi32.h>
#endif

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace cosmic::wallpaper {
namespace {

// Files larger than this are not the kind of wallpaper Sunshine should be
// hashing/streaming over the LAN; treat them as unavailable.
constexpr std::uintmax_t kMaxBytes = 8 * 1024 * 1024;

std::atomic<bool> g_enabled {true};

// Guards both the resolve-and-read path below and the hash cache: current_hash()
// and read_bytes() are called concurrently from Sunshine's HTTP server threads.
std::mutex g_mutex;
std::filesystem::path g_cached_path;
std::filesystem::file_time_type g_cached_mtime;
std::uintmax_t g_cached_size = 0;
std::string g_cached_hash;

// 64-char lowercase hex encoding of a SHA-256 digest.
std::string to_hex_lower(const crypto::sha256_t &digest) {
  static const char digits[] = "0123456789abcdef";
  std::string hex;
  hex.resize(digest.size() * 2);
  for (std::size_t i = 0; i < digest.size(); ++i) {
    hex[i * 2] = digits[digest[i] >> 4];
    hex[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  return hex;
}

// Signature contract (plan D10a/b): current_hash() and read_bytes() only ever
// report a file starting with one of these three magic-byte sequences
// (JPEG/PNG/BMP). The identical check is duplicated in cosmic_wallpaper
// (host/sunshine/src/nvhttp.cpp), which uses it to pick the Content-Type for
// GET /cosmic/wallpaper; the two lists must change together. Never relax this
// list here without also updating nvhttp.cpp -- /serverinfo must not
// advertise a wallpaper hash that route cannot actually serve.
bool has_known_signature(const std::string &bytes) {
  if (bytes.size() >= 3 && (unsigned char) bytes[0] == 0xFF && (unsigned char) bytes[1] == 0xD8 &&
      (unsigned char) bytes[2] == 0xFF) {
    return true;  // JPEG
  }
  if (bytes.size() >= 4 && (unsigned char) bytes[0] == 0x89 && (unsigned char) bytes[1] == 0x50 &&
      (unsigned char) bytes[2] == 0x4E && (unsigned char) bytes[3] == 0x47) {
    return true;  // PNG
  }
  if (bytes.size() >= 2 && (unsigned char) bytes[0] == 0x42 && (unsigned char) bytes[1] == 0x4D) {
    return true;  // BMP
  }
  return false;
}

// Reads the whole file into memory. Returns nullopt on any failure (missing
// file, permission error, or the file shrinking between the size check and
// this read) rather than throwing -- callers are HTTP handler threads.
std::optional<std::string> read_file_bytes(const std::filesystem::path &path, std::uintmax_t size) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::string data;
  data.resize(static_cast<std::size_t>(size));
  file.read(data.data(), static_cast<std::streamsize>(data.size()));
  if (static_cast<std::uintmax_t>(file.gcount()) != size) {
    return std::nullopt;
  }
  return data;
}

#ifdef _WIN32

// WTSQuerySessionInformationW wrapper: copies the result and frees the WTS
// buffer on every path, including failure. Returns "" if the session has no
// value for info_class (e.g. no interactive user, or the query itself fails).
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

// Locates TranscodedWallpaper for the interactive console session without
// impersonation: WTSQueryUserToken needs SeTcbPrivilege at call time and
// leaves a token handle to clean up, so instead resolve the session's account
// name, look up its SID, and read the profile path Windows itself recorded
// for that SID at logon. Returns an empty path on any failed step.
std::filesystem::path resolve_fallback_path(DWORD session_id) {
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
    // 0 is failure; a value greater than MAX_PATH means profile was too
    // small and its contents are unspecified -- both are unusable.
    return {};
  }

  const std::filesystem::path path = std::filesystem::path(profile) / L"AppData" / L"Roaming" /
                                      L"Microsoft" / L"Windows" / L"Themes" / L"TranscodedWallpaper";
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return {};
  }
  return path;
}

// Resolved fresh on every call: the syscalls involved are cheap, and
// current_hash()'s (path, mtime, size) cache already avoids re-reading the
// wallpaper file itself when nothing has changed.
std::filesystem::path resolve_path() {
  wchar_t buf[MAX_PATH] = {};
  if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buf, 0) && buf[0] != L'\0') {
    const std::filesystem::path path(buf);
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
      return path;
    }
  }

  // SPI_GETDESKWALLPAPER came back empty. On the interactive desktop that
  // means a solid-colour background -- not a resolution failure -- so do not
  // fall back to guessing a file. Off the interactive desktop (e.g. running
  // as a service in session 0 while a user is logged on elsewhere), SPI
  // always reports empty because there is no desktop to query; that is the
  // only case worth falling back for.
  DWORD our_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &our_session)) {
    return {};
  }
  const DWORD console_session = WTSGetActiveConsoleSessionId();
  if (console_session == 0xFFFFFFFF || console_session == our_session) {
    return {};
  }
  return resolve_fallback_path(console_session);
}

#else  // !_WIN32

// DE-specific (X11/Wayland compositors each own wallpaper state differently);
// revisited with the Wayland work.
std::filesystem::path resolve_path() {
  return {};
}

#endif

}  // namespace

void set_enabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

std::string current_hash() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return "";
  }

  const std::filesystem::path path = resolve_path();
  if (path.empty()) {
    return "";
  }

  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > kMaxBytes) {
    return "";
  }
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return "";
  }

  if (path == g_cached_path && mtime == g_cached_mtime && size == g_cached_size) {
    // Cached decision, including "" for a file whose signature this provider
    // (and GET /cosmic/wallpaper) does not recognise -- an unsupported file
    // is not re-read and re-sniffed on every /serverinfo poll.
    return g_cached_hash;
  }

  const auto bytes = read_file_bytes(path, size);
  if (!bytes) {
    return "";
  }
  const std::string hash = has_known_signature(*bytes) ? to_hex_lower(crypto::hash(*bytes)) : std::string();

  g_cached_path = path;
  g_cached_mtime = mtime;
  g_cached_size = size;
  g_cached_hash = hash;
  return hash;
}

std::string read_bytes() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return "";
  }

  const std::filesystem::path path = resolve_path();
  if (path.empty()) {
    return "";
  }

  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > kMaxBytes) {
    return "";
  }

  // Not cached: read_bytes() serves the current file every call.
  const auto bytes = read_file_bytes(path, size);
  if (!bytes || !has_known_signature(*bytes)) {
    return "";
  }
  return *bytes;
}

}  // namespace cosmic::wallpaper
