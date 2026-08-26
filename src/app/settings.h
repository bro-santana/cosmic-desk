#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cosmic {

// How the viewer picks the stream resolution. "Host native" is the default and
// resolves to the host's current desktop resolution at connect time (M5).
enum class ResolutionMode {
    HostNative,
    R1080p,
    R1440p,
    R2160p,
    Custom,
};

// Persistent user settings, stored as cosmic.json in config_dir().
struct Settings {
    int port_base = 47989;  // all six ports derive from this; see PLAN.md 3.2

    ResolutionMode resolution_mode = ResolutionMode::HostNative;
    int custom_width = 1920;
    int custom_height = 1080;
    int fps = 60;
    int bitrate_kbps = 20000;

    bool autostart = false;
    std::vector<std::string> recent_hosts;

    // Records a successful host connection: trims whitespace, dedupes
    // case-insensitively, moves the entry to the front, caps the list at 10,
    // and persists via save(). Called from the viewer worker thread while the
    // main thread renders recent_hosts, so the list is mutex-protected.
    void add_recent_host(const std::string& host);

    // Thread-safe copy of recent_hosts for the UI (main thread).
    std::vector<std::string> recent_hosts_snapshot() const;

    // %APPDATA%\CosmicDesk on Windows, $XDG_CONFIG_HOME/cosmicdesk (or
    // ~/.config/cosmicdesk) elsewhere. Created on demand.
    static std::filesystem::path config_dir();
    static std::filesystem::path config_file();

    // Missing or unreadable file yields defaults rather than an error: a fresh
    // install must always start.
    static Settings load();
    bool save() const;

    // std::mutex is neither copyable nor movable, so the implicit move
    // constructor is deleted; provide one for load()'s return-by-value.
    Settings() = default;
    Settings(Settings&& other) noexcept;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

private:
    mutable std::mutex mutex_;
};

const char* to_string(ResolutionMode mode);

}  // namespace cosmic
