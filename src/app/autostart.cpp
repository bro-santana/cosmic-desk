// Cosmic Desk — autostart at logon (plan M6.1). Windows: HKCU\...\Run value
// "CosmicDesk" = "\"<exe>\" --hidden". Linux: $XDG_CONFIG_HOME/autostart/
// cosmicdesk.desktop (or ~/.config/autostart). Both are per-user logon
// autostarts, NOT a Windows service: a service runs in session 0 and cannot
// capture the interactive desktop or inject input (plan 3.1 session-0
// limitation). Errors go to stderr with fprintf, app-side style.

#include "app/autostart.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cosmic::autostart {
namespace {

#ifdef _WIN32
// The HKCU Run key that Windows checks at logon.
const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValueName = L"CosmicDesk";

// Returns the full path of the running executable, or an empty string on
// failure. Grows the buffer until the path fits (GetModuleFileNameW returns
// the buffer size when the path was truncated).
std::wstring exe_path() {
    std::wstring path(256, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, &path[0],
                                             static_cast<DWORD>(path.size()));
        if (len == 0) {
            std::fprintf(stderr, "autostart: GetModuleFileNameW failed (%lu)\n",
                         GetLastError());
            return std::wstring();
        }
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}
#else
// Returns the full path of the running executable, or an empty string on
// failure. Grows the buffer until the path fits (readlink truncates to the
// buffer size when the path is longer).
std::string exe_path() {
    std::string path(256, '\0');
    for (;;) {
        const ssize_t len = readlink("/proc/self/exe", &path[0], path.size());
        if (len < 0) {
            std::fprintf(stderr, "autostart: readlink(/proc/self/exe) failed: %s\n",
                         std::strerror(errno));
            return std::string();
        }
        if (static_cast<size_t>(len) < path.size()) {
            path.resize(static_cast<size_t>(len));
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// The autostart directory: $XDG_CONFIG_HOME/autostart if XDG_CONFIG_HOME is
// set, else ~/.config/autostart.
std::filesystem::path autostart_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg != '\0') {
            return std::filesystem::path(xdg) / "autostart";
        }
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::filesystem::path();
    }
    return std::filesystem::path(home) / ".config" / "autostart";
}
#endif

}  // namespace

bool enabled() {
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, kRunValueName, nullptr, &type,
                                         nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ;
#else
    const std::filesystem::path dir = autostart_dir();
    if (dir.empty()) {
        return false;
    }
    return std::filesystem::exists(dir / "cosmicdesk.desktop");
#endif
}

bool set_enabled(bool enable) {
#ifdef _WIN32
    if (enable) {
        const std::wstring path = exe_path();
        if (path.empty()) {
            return false;
        }
        // Value = "\"<full exe path>\" --hidden" (quoted so a path with
        // spaces survives command-line parsing).
        const std::wstring value = L"\"" + path + L"\" --hidden";
        const LONG status = RegSetKeyValueW(
            HKEY_CURRENT_USER, kRunKey, kRunValueName, REG_SZ, value.c_str(),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        if (status != ERROR_SUCCESS) {
            std::fprintf(stderr, "autostart: RegSetKeyValueW failed (%ld)\n",
                         status);
            return false;
        }
        return true;
    }
    const LONG status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey,
                                           kRunValueName);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        std::fprintf(stderr, "autostart: RegDeleteKeyValueW failed (%ld)\n",
                     status);
        return false;
    }
    return true;
#else
    const std::filesystem::path dir = autostart_dir();
    if (dir.empty()) {
        std::fprintf(stderr, "autostart: no HOME/XDG_CONFIG_HOME to write to\n");
        return false;
    }
    const std::filesystem::path file = dir / "cosmicdesk.desktop";
    if (enable) {
        const std::string path = exe_path();
        if (path.empty()) {
            return false;
        }
        // Exec = "\"<full exe path>\" --hidden" (quoted so a path with
        // spaces survives command-line parsing).
        const std::string content =
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Cosmic Desk\n"
            "Comment=Cosmic Desk remote desktop (host + viewer)\n"
            "Exec=\"" + path + "\" --hidden\n"
            "X-GNOME-Autostart-enabled=true\n";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "autostart: create_directories failed: %s\n",
                         ec.message().c_str());
            return false;
        }
        std::ofstream out(file, std::ios::trunc);
        if (!out.is_open()) {
            std::fprintf(stderr, "autostart: cannot write %s\n",
                         file.string().c_str());
            return false;
        }
        out << content;
        if (!out.good()) {
            std::fprintf(stderr, "autostart: write failed for %s\n",
                         file.string().c_str());
            return false;
        }
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::remove(file, ec) && ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            // Already gone: removing an autostart that isn't set is a success
            // (mirrors the Windows ERROR_FILE_NOT_FOUND handling).
            return true;
        }
        std::fprintf(stderr, "autostart: remove failed: %s\n",
                     ec.message().c_str());
        return false;
    }
    return true;
#endif
}

}  // namespace cosmic::autostart
