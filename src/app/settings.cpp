#include "app/settings.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace cosmic {
namespace {

const char* kResolutionNames[] = {"host", "1080p", "1440p", "2160p", "custom"};

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

}  // namespace

const char* to_string(ResolutionMode mode) {
    return kResolutionNames[static_cast<int>(mode)];
}

std::filesystem::path Settings::config_dir() {
#ifdef _WIN32
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

    settings.port_base = json.value("port_base", settings.port_base);
    settings.resolution_mode =
        resolution_from_string(json.value("resolution_mode", std::string("host")));
    settings.custom_width = json.value("custom_width", settings.custom_width);
    settings.custom_height = json.value("custom_height", settings.custom_height);
    settings.fps = json.value("fps", settings.fps);
    settings.bitrate_kbps = json.value("bitrate_kbps", settings.bitrate_kbps);
    settings.autostart = json.value("autostart", settings.autostart);
    settings.recent_hosts = json.value("recent_hosts", settings.recent_hosts);

    return settings;
}

bool Settings::save() const {
    std::error_code ec;
    std::filesystem::create_directories(config_dir(), ec);
    if (ec) {
        return false;
    }

    const nlohmann::json json = {
        {"port_base", port_base},
        {"resolution_mode", to_string(resolution_mode)},
        {"custom_width", custom_width},
        {"custom_height", custom_height},
        {"fps", fps},
        {"bitrate_kbps", bitrate_kbps},
        {"autostart", autostart},
        {"recent_hosts", recent_hosts},
    };

    std::ofstream file(config_file(), std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << json.dump(2) << '\n';
    return file.good();
}

}  // namespace cosmic
