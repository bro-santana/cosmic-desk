#include "app/settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#endif

namespace cosmic {
namespace {

const char* kResolutionNames[] = {"host", "1080p", "1440p", "2160p", "custom"};

// Lifts the trim + case-insensitive compare that add_recent_host inlined into
// file-local helpers; every SavedHost mutator and port_for need them.
std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

std::vector<SavedHost>::iterator find_host(std::vector<SavedHost>& hosts,
                                           const std::string& address) {
    return std::find_if(hosts.begin(), hosts.end(),
                        [&](const SavedHost& h) {
                            return iequals(h.address, address);
                        });
}

std::vector<SavedHost>::const_iterator find_host(
    const std::vector<SavedHost>& hosts, const std::string& address) {
    return std::find_if(hosts.begin(), hosts.end(),
                        [&](const SavedHost& h) {
                            return iequals(h.address, address);
                        });
}

// nlohmann's json::value() throws type_error when a key exists with the wrong
// type. load() deliberately parses with allow_exceptions=false so a corrupt or
// hand-edited cosmic.json yields defaults rather than an error, but the scalar
// reads used to defeat that: one mistyped value (say "fps": "60") threw out of
// load() and terminated the app at startup with no message. Read every scalar
// through a type check instead, the same way the hosts array is read below.
int json_int(const nlohmann::json& json, const char* key, int fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_number()) ? it->get<int>() : fallback;
}

bool json_bool(const nlohmann::json& json, const char* key, bool fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

std::string json_string(const nlohmann::json& json, const char* key,
                        const std::string& fallback) {
    const auto it = json.find(key);
    return (it != json.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

ResolutionMode resolution_from_string(const std::string& value) {
    for (int i = 0; i < 5; ++i) {
        if (value == kResolutionNames[i]) {
            return static_cast<ResolutionMode>(i);
        }
    }
    return ResolutionMode::HostNative;
}

std::filesystem::path home_dir() {
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE")) {
        return std::filesystem::path(profile);
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home);
    }
#endif
    return std::filesystem::current_path();
}

#ifdef _WIN32
// True when the process token is elevated (elevated admin) or LocalSystem.
// Token elevation *type* alone is ambiguous for LocalSystem (it reports
// TokenElevationTypeDefault like a standard user), so an explicit SYSTEM SID
// check backs it up. Deliberately NOT a write probe: the first unelevated run
// can create C:\ProgramData\CosmicDesk (Windows lets Users create subfolders
// there) and become its creator-owner, which made a probe pass and wrongly
// adopted the machine-wide location.
bool is_elevated_or_system() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    bool elevated = false;
    TOKEN_ELEVATION_TYPE type {};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size) &&
        type == TokenElevationTypeFull) {
        elevated = true;
    }
    if (!elevated) {
        BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        DWORD needed = 0;
        if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &needed)) {
            auto* token_user = reinterpret_cast<TOKEN_USER*>(buffer);
            BYTE system_sid[SECURITY_MAX_SID_SIZE];
            DWORD sid_size = sizeof(system_sid);
            if (CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid, &sid_size) &&
                EqualSid(token_user->User.Sid, system_sid)) {
                elevated = true;
            }
        }
    }
    CloseHandle(token);
    return elevated;
}
#endif

}  // namespace

const char* to_string(ResolutionMode mode) {
    return kResolutionNames[static_cast<int>(mode)];
}

std::filesystem::path Settings::config_dir() {
#ifdef _WIN32
    // Machine-wide location for elevated runs (the service spawns the app as
    // SYSTEM, or a manual run-as-admin), per-user for portable unelevated
    // runs. The decision is by token identity (see is_elevated_or_system
    // above), not by write capability: a write probe is fooled by the
    // creator-owner right an unelevated run gets when it is the first to
    // create C:\ProgramData\CosmicDesk. Mirrors
    // host/sunshine/src/platform/windows/misc.cpp appdata() — the two must
    // agree so the UI and the host share one config store.
    if (is_elevated_or_system()) {
        if (const char* programdata = std::getenv("PROGRAMDATA")) {
            return std::filesystem::path(programdata) / "CosmicDesk";
        }
        return std::filesystem::path("C:\\ProgramData") / "CosmicDesk";
    }
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "CosmicDesk";
    }
    return home_dir() / "AppData" / "Roaming" / "CosmicDesk";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg != '\0') {
            return std::filesystem::path(xdg) / "cosmicdesk";
        }
    }
    return home_dir() / ".config" / "cosmicdesk";
#endif
}

std::filesystem::path Settings::config_file() {
    return config_dir() / "cosmic.json";
}

Settings Settings::load() {
    Settings settings;

    std::ifstream file(config_file());
    if (!file.is_open()) {
        return settings;
    }

    nlohmann::json json = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object()) {
        return settings;
    }

    settings.port_base = json_int(json, "port_base", settings.port_base);
    settings.resolution_mode =
        resolution_from_string(json_string(json, "resolution_mode", "host"));
    settings.custom_width = json_int(json, "custom_width", settings.custom_width);
    settings.custom_height = json_int(json, "custom_height", settings.custom_height);
    settings.fps = json_int(json, "fps", settings.fps);
    settings.bitrate_kbps = json_int(json, "bitrate_kbps", settings.bitrate_kbps);
    settings.autostart = json_bool(json, "autostart", settings.autostart);

    // hosts is read by hand with contains()/is_*() guards: json.value() throws
    // nlohmann::type_error when a key exists with the wrong type, even on a
    // codepath parsed with allow_exceptions=false, and a hand-edited cosmic.json
    // must never crash startup. Gate the legacy migration on contains("hosts"),
    // not on hosts.empty(), so a user who deliberately removed every machine is
    // not resurrected from an old recent_hosts list. Migration lands on disk at
    // the first save() (main.cpp does that at shutdown); downgrading to an older
    // build after migrating loses the list.
    if (json.contains("hosts")) {
        const auto& arr = json["hosts"];
        if (arr.is_array()) {
            for (const auto& entry : arr) {
                if (!entry.is_object()) {
                    continue;
                }
                SavedHost h;
                if (entry.contains("address") && entry["address"].is_string()) {
                    h.address = trim(entry["address"].get<std::string>());
                }
                if (entry.contains("nickname") && entry["nickname"].is_string()) {
                    h.nickname = entry["nickname"].get<std::string>();
                }
                if (entry.contains("port") && entry["port"].is_number_integer()) {
                    h.port = entry["port"].get<int>();
                }
                if (entry.contains("paired") && entry["paired"].is_boolean()) {
                    h.paired = entry["paired"].get<bool>();
                }
                if (!h.address.empty()) {
                    settings.hosts.push_back(std::move(h));
                }
            }
        }
    } else if (json.contains("recent_hosts") && json["recent_hosts"].is_array()) {
        // Legacy: a flat string list becomes {address, "", 0, false} entries.
        for (const auto& entry : json["recent_hosts"]) {
            if (entry.is_string()) {
                const std::string addr = trim(entry.get<std::string>());
                if (!addr.empty()) {
                    SavedHost h;
                    h.address = addr;
                    settings.hosts.push_back(std::move(h));
                }
            }
        }
    }

    return settings;
}

Settings::Settings(Settings&& other) noexcept
    : port_base(other.port_base),
      resolution_mode(other.resolution_mode),
      custom_width(other.custom_width),
      custom_height(other.custom_height),
      fps(other.fps),
      bitrate_kbps(other.bitrate_kbps),
      autostart(other.autostart),
      hosts(std::move(other.hosts)) {}

void Settings::add_or_update_host(const std::string& address, bool paired) {
    {
        std::lock_guard lock(mutex_);
        const std::string addr = trim(address);
        if (addr.empty()) {
            return;
        }
        auto it = find_host(hosts, addr);
        if (it != hosts.end()) {
            // Update only touches `paired`, preserving nickname and port so a
            // connect can never clobber the user's edits.
            it->paired = paired;
        } else {
            // Insert appends; do not move-to-front — in a managed list that
            // makes rows jump under the cursor. No cap: deliberately paired
            // machines must not be silently evicted.
            SavedHost h;
            h.address = addr;
            h.paired = paired;
            hosts.push_back(std::move(h));
        }
    }
    save();
}

bool Settings::remove_host(const std::string& address) {
    {
        std::lock_guard lock(mutex_);
        const std::string addr = trim(address);
        auto it = find_host(hosts, addr);
        if (it == hosts.end()) {
            return false;  // Nothing changed; skip the save().
        }
        hosts.erase(it);
    }
    save();
    return true;
}

bool Settings::set_host_nickname(const std::string& address,
                                 const std::string& nickname) {
    {
        std::lock_guard lock(mutex_);
        const std::string addr = trim(address);
        auto it = find_host(hosts, addr);
        if (it == hosts.end()) {
            return false;
        }
        it->nickname = nickname;  // Empty nickname clears it.
    }
    save();
    return true;
}

bool Settings::set_host_port(const std::string& address, int port) {
    {
        std::lock_guard lock(mutex_);
        const std::string addr = trim(address);
        auto it = find_host(hosts, addr);
        if (it == hosts.end()) {
            return false;
        }
        it->port = port;  // 0 = follow the global port_base.
    }
    save();
    return true;
}

std::vector<SavedHost> Settings::hosts_snapshot() const {
    std::lock_guard lock(mutex_);
    return hosts;
}

int Settings::port_for(const std::string& address) const {
    std::lock_guard lock(mutex_);
    auto it = find_host(hosts, trim(address));
    if (it != hosts.end() && it->port > 0) {
        return it->port;
    }
    return port_base;
}

bool Settings::save() const {
    std::lock_guard lock(mutex_);

    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);
    if (ec) {
        return false;
    }

    std::vector<nlohmann::json> hosts_json;
    hosts_json.reserve(hosts.size());
    for (const SavedHost& h : hosts) {
        hosts_json.push_back({
            {"address", h.address},
            {"nickname", h.nickname},
            {"port", h.port},
            {"paired", h.paired},
        });
    }

    const nlohmann::json json = {
        {"port_base", port_base},
        {"resolution_mode", to_string(resolution_mode)},
        {"custom_width", custom_width},
        {"custom_height", custom_height},
        {"fps", fps},
        {"bitrate_kbps", bitrate_kbps},
        {"autostart", autostart},
        {"hosts", hosts_json},
    };

    std::ofstream file(config_file(), std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << json.dump(2) << '\n';
    return file.good();
}

}  // namespace cosmic
