// Cosmic Desk — viewer session layer (plan M2.2). Owns the pairing and
// streaming lifecycle for one connection to a host. All network/streaming work
// runs on a worker thread; the main loop only polls status() and never blocks.
//
// Threading model:
//   - start_connect() spawns the worker thread and returns immediately.
//   - The worker runs gs_init/gs_pair/gs_applist/gs_start_app and then blocks
//     in LiStartConnection() for the whole session.
//   - end_session() may be called from any thread: it sets a flag and calls
//     LiStopConnection() (safe from any thread; moonlight-embedded does this
//     from signal handlers) to unblock the worker. The worker then quits the
//     app on the host and returns to Idle.
//   - status() returns a mutex-protected snapshot for the UI.

#pragma once

#include <Limelight.h>

#include <mutex>
#include <string>
#include <thread>

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

struct SessionStatus {
    ViewerState state = ViewerState::Idle;
    std::string message;
    std::string pin;     // 4-digit PIN shown while PairingNeedPin
    int port_used = 0;   // host HTTP port this session talks to
    int stream_width = 0;   // negotiated video width (0 until streaming)
    int stream_height = 0;  // negotiated video height (0 until streaming)
};

class Session {
public:
    Session() = default;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Async: spawns the worker thread. No-op if a session is already active
    // or the address is empty. port is the host HTTP port (settings.port_base;
    // 0 or negative falls back to 47989).
    void start_connect(const std::string& host_ip, int port);

    // Async-safe request to stop: sets the end flag and calls LiStopConnection()
    // to unblock the worker. Does not join the worker thread.
    void end_session();

    // Thread-safe snapshot for the UI.
    SessionStatus status();

    bool is_streaming() const;

private:
    void worker(std::string host_ip, int port);
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
    SessionStatus status_;
    std::thread worker_thread_;
    bool worker_running_ = false;
    bool session_ended_ = false;
    bool stage_failed_ = false;
    std::string stage_failed_message_;
    int termination_error_ = 0;
    std::string termination_message_;
};

}  // namespace cosmic::viewer