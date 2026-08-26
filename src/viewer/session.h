// Cosmic Desk — viewer session layer (plan M2.2). Owns the pairing and
// streaming lifecycle for one connection to a host. All network/streaming work
// runs on a worker thread; the main loop only polls status() and never blocks.
//
// Threading model:
//   - start_connect() spawns the worker thread and returns immediately.
//   - The worker runs gs_init/gs_pair/gs_applist/gs_start_app, then calls
//     LiStartConnection(). That call does NOT block: it brings every stage up
//     and returns, leaving the stream running on moonlight-common-c's own
//     threads. The worker therefore parks on state_cv_ for the rest of the
//     session and only then calls LiStopConnection().
//   - end_session() may be called from any thread: it sets a flag and wakes
//     the worker, which owns the LiStopConnection() call (Limelight documents
//     LiStartConnection/LiStopConnection as not thread-safe, so they must be
//     paired on one thread). The worker then quits the app on the host and
//     returns to Idle.
//   - status() returns a mutex-protected snapshot for the UI.

#pragma once

#include "app/settings.h"

#include <Limelight.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// libgamestream's headers are plain C (no extern "C" guards); the Session owns
// a SERVER_DATA (server_) that the monitor-refresh thread re-fetches against.
extern "C" {
#include <client.h>
}

namespace cosmic::viewer {

enum class ViewerState {
    Idle,
    Connecting,
    PairingNeedPin,
    PairingInProgress,
    Streaming,
    Failed,
};

const char* to_string(ViewerState state);

// Snapshot of the stream-affecting settings taken on the main thread when a
// connection starts (plan M4.4). The worker thread gets a copy by value so it
// never reads the live Settings scalars while the user edits them mid-connect.
struct StreamPrefs {
    // COSMIC MODIFICATION (M5): carries the resolution mode instead of a
    // pre-resolved WxH. "Host native" is resolved from the just-fetched
    // CosmicDisplays snapshot at the point STREAM_CONFIGURATION is filled in
    // (the worker has the displays by then); the fixed modes keep their own
    // WxH. custom_w/custom_h hold the Custom mode's dimensions.
    ResolutionMode mode = ResolutionMode::HostNative;
    int custom_w = 1920;
    int custom_h = 1080;
    int fps = 60;
    int bitrate_kbps = 20000;
};

// COSMIC MODIFICATION (M5): one host display from the <CosmicDisplays> block
// (docs/PROTOCOL.md). Mirrors COSMIC_DISPLAY in libgamestream/xml.h; stored in
// the status snapshot for the top-bar monitor dropdown.
struct DisplayInfo {
    std::string name;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool primary = false;
    bool active = false;
};

struct SessionStatus {
    ViewerState state = ViewerState::Idle;
    std::string message;
    std::string pin;     // 4-digit PIN shown while PairingNeedPin / PairingInProgress
    int port_used = 0;   // host HTTP port this session talks to
    int stream_width = 0;   // negotiated video width (0 until streaming)
    int stream_height = 0;  // negotiated video height (0 until streaming)
    // COSMIC MODIFICATION (M5): host display list + the currently captured
    // (active) display index, refreshed when the monitor dropdown opens.
    std::vector<DisplayInfo> displays;
    int active_display = 0;
};

class Session {
public:
    // settings is the app-wide Settings object (owned by main.cpp); the worker
    // thread uses it to record successful connections as recent hosts.
    explicit Session(Settings& settings);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Async: spawns the worker thread. No-op if a session is already active
    // or the address is empty. port is the host HTTP port (settings.port_base;
    // 0 or negative falls back to 47989). The stream preferences are snapshotted
    // here, on the main thread, and passed to the worker by value.
    void start_connect(const std::string& host_ip, int port);

    // Async-safe request to stop: sets the end flag and calls LiStopConnection()
    // to unblock the worker. Does not join the worker thread.
    void end_session();

    // Thread-safe snapshot for the UI.
    SessionStatus status();

    bool is_streaming() const;

    // COSMIC MODIFICATION (M5): re-fetches /serverinfo on a short-lived thread
    // and updates the displays snapshot + active_display under the status
    // mutex. No-op if a refresh is already running. Safe to call from the main
    // thread while streaming; the refresh thread is joined in the worker's
    // teardown so it never outlives the session worker that owns server_.
    void refresh_displays();

    // COSMIC MODIFICATION (M5): records a monitor switch locally (the host has
    // already switched via the synthesized hotkey); updates active_display in
    // the status snapshot under the mutex. Main thread only.
    void set_active_display(int index);

private:
    void worker(std::string host_ip, int port, StreamPrefs prefs);
    void refresh_worker();
    bool session_ended() const;
    void set_status(ViewerState state, const std::string& message,
                    const std::string& pin = "", int port_used = 0);
    void set_message(const std::string& message);
    void set_stage_failed(const std::string& message);
    void set_terminated(int error_code);

    // Connection/video/audio callbacks (C function pointers, see Limelight.h).
    // They run on moonlight-common-c internal threads and reach this Session
    // through the file-static s_active pointer in session.cpp.
    static void on_stage_starting(int stage);
    static void on_stage_complete(int stage);
    static void on_stage_failed(int stage, int error_code);
    static void on_connection_terminated(int error_code);
    static void on_log_message(const char* format, ...);
    static int on_video_setup(int videoFormat, int width, int height,
                              int redrawRate, void* context, int drFlags);
    static void on_video_start();
    static void on_video_stop();
    static void on_video_cleanup();
    static int on_video_submit(PDECODE_UNIT decodeUnit);
    static int on_audio_init(int audioConfiguration,
                             const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                             void* context, int arFlags);
    static void on_audio_start();
    static void on_audio_stop();
    static void on_audio_cleanup();
    static void on_audio_decode(char* sampleData, int sampleLength);

    mutable std::mutex mutex_;
    // Signals the streaming worker that the session should come down, either
    // because the user ended it or because the connection terminated itself.
    std::condition_variable state_cv_;
    SessionStatus status_;
    std::thread worker_thread_;
    bool worker_running_ = false;
    bool session_ended_ = false;
    bool stage_failed_ = false;
    // Set by the connectionTerminated callback; one of the two wake conditions
    // the streaming worker waits on (session_ended_ is the other).
    bool connection_terminated_ = false;
    std::string stage_failed_message_;
    int termination_error_ = 0;
    std::string termination_message_;
    Settings& settings_;
    // COSMIC MODIFICATION (M5): the refresh thread spawned by refresh_displays().
    // Guarded by mutex_ (refresh_running_); joined in the worker's tail teardown
    // so it never outlives the worker that owns server_.
    std::thread refresh_thread_;
    bool refresh_running_ = false;
    // COSMIC MODIFICATION (M5): true only while the worker is blocked in
    // LiStartConnection() (streaming). The refresh thread shares libgamestream's
    // static curl handle with the worker, so it must never run while the worker
    // is doing HTTP (gs_init/gs_pair/gs_applist/gs_quit_app). Guarded by mutex_.
    bool refresh_allowed_ = false;
    // COSMIC MODIFICATION (M5): the SERVER_DATA for the current connection,
    // owned by the session worker (populated by gs_init, re-fetched by the
    // refresh thread via gs_load_serverinfo). server_.serverInfo.address points
    // into the worker's local address buffer, so the refresh thread must be
    // joined before the worker returns (done in the worker's tail teardown).
    SERVER_DATA server_ = {};
};

}  // namespace cosmic::viewer
