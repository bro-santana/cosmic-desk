#pragma once

#include <cstdint>
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

// One machine the user has paired with. Replaces the flat recent_hosts string
// list: this one is managed by the user, not accumulated as history.
struct SavedHost {
    std::string address;   // IP or hostname, as typed (trimmed). Identity key.
    std::string nickname;  // Optional display name; ASCII-only (see ui/scale.h).
    int port = 0;          // 0 = follow Settings::port_base.
    bool paired = false;   // Last known pairing state; refreshed by the session worker.
    int64_t last_connected = 0;  // Unix seconds of the last successful stream
                                 // start; 0 = never (UI migration U3).
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
    std::vector<SavedHost> hosts;

    // Adds the host if unknown, else updates it. Insert appends (nickname="",
    // port=0); update only touches `paired` so a connect can never clobber the
    // user's nickname or port override. Trims whitespace and ignores empty
    // input; matches `address` case-insensitively. Persists via save().
    void add_or_update_host(const std::string& address, bool paired);

    // Erases the host; returns whether anything changed. Local forget only —
    // the host keeps listing this client as paired until it is removed there
    // (there is no /unpair route; do not add one).
    bool remove_host(const std::string& address);

    // Sets the display nickname; an empty nickname clears it.
    bool set_host_nickname(const std::string& address, const std::string& nickname);

    // Sets the per-machine port override; 0 = follow the global port_base.
    bool set_host_port(const std::string& address, int port);

    // Records the last successful stream start (unix seconds). Persists via
    // save(); 0 clears it. Called by the viewer session (UI migration U3).
    void set_host_last_connected(const std::string& address, int64_t unix_seconds);

    // Thread-safe copy of hosts for the UI (main thread).
    std::vector<SavedHost> hosts_snapshot() const;

    // The one place the per-machine port override is resolved: entry.port if
    // set (>0), else port_base.
    int port_for(const std::string& address) const;

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
