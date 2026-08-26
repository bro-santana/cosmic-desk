// Cosmic Desk — viewer session implementation (plan M2.2). Mirrors the
// moonlight-embedded main.c stream path: gs_init -> (pair) -> gs_applist ->
// gs_start_app -> LiStartConnection, all on a worker thread so the main loop
// never blocks.

#include "viewer/session.h"

#include "app/settings.h"
#include "viewer/audio.h"
#include "viewer/decoder.h"

// libgamestream's headers are plain C (no extern "C" guards), so they must be
// wrapped here or the gs_* symbols get C++-mangled names and fail to link.
extern "C" {
#include <client.h>
#include <errors.h>
}
#include <Limelight.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

namespace cosmic::viewer {
namespace {

// LiStartConnection's connection callbacks take no context pointer, so the
// callbacks reach the owning Session through this file-static. Only one
// Session exists (owned by main.cpp) and only one connection runs at a time;
// it is set right before LiStartConnection() and cleared when it returns.
Session* s_active = nullptr;

// Host HTTP port for the viewer (plan M2.2). The port_base setting is passed
// through start_connect(); this constant is the fallback when it is absent.
constexpr int kHostHttpPort = 47989;

std::string generate_pin() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> digit(0, 9);
    std::string pin;
    pin.reserve(4);
    for (int i = 0; i < 4; ++i) {
        pin += static_cast<char>('0' + digit(gen));
    }
    return pin;
}

}  // namespace

const char* to_string(ViewerState state) {
    switch (state) {
    case ViewerState::Idle:
        return "Idle";
    case ViewerState::Connecting:
        return "Connecting";
    case ViewerState::PairingNeedPin:
        return "Pairing (PIN needed)";
    case ViewerState::PairingInProgress:
        return "Pairing";
    case ViewerState::Streaming:
        return "Streaming";
    case ViewerState::Failed:
        return "Failed";
    }
    return "Unknown";
}

Session::~Session() {
    end_session();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void Session::start_connect(const std::string& host_ip, int port) {
    {
        std::lock_guard lock(mutex_);
        if (worker_running_ || host_ip.empty()) {
            return;  // A session is already active, or there is nothing to do.
        }
        session_ended_ = false;
        stage_failed_ = false;
        stage_failed_message_.clear();
        termination_error_ = 0;
        termination_message_.clear();
        worker_running_ = true;
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();  // Previous worker already finished.
    }
    worker_thread_ = std::thread(&Session::worker, this, host_ip, port);
}

void Session::end_session() {
    {
        std::lock_guard lock(mutex_);
        session_ended_ = true;
    }
    // LiStopConnection() is safe to call from any thread (moonlight-embedded
    // does this from signal handlers) and unblocks the worker thread blocked
    // in LiStartConnection(). gs_quit_app() runs on the worker after
    // LiStartConnection() returns. If the worker is mid-pairing (blocked in
    // gs_pair), this is a no-op and the worker notices the end flag when
    // gs_pair returns.
    LiStopConnection();
}

SessionStatus Session::status() {
    std::lock_guard lock(mutex_);
    return status_;
}

bool Session::is_streaming() const {
    std::lock_guard lock(mutex_);
    return status_.state == ViewerState::Streaming;
}

void Session::worker(std::string host_ip, int port) {
    // server.serverInfo.address points into this buffer for the whole session,
    // so it must outlive gs_init and stay put.
    std::vector<char> address(host_ip.begin(), host_ip.end());
    address.push_back('\0');

    const int http_port = port > 0 ? port : kHostHttpPort;

    set_status(ViewerState::Connecting, "Connecting to " + host_ip + "...",
               "", http_port);

    // Client cert + key live in <config dir>/client (plan D6). gs_init's
    // mkdirtree() creates the tree itself, but create it explicitly so the
    // directory exists before the cert is generated into it.
    const std::filesystem::path key_dir = cosmic::Settings::config_dir() / "client";
    std::error_code ec;
    std::filesystem::create_directories(key_dir, ec);

    SERVER_DATA server = {};
    const int init_ret = gs_init(&server, address.data(), http_port,
                                 key_dir.string().c_str(), 1 /*logLevel*/,
                                 true /*unsupported*/);
    if (init_ret != GS_OK) {
        set_status(ViewerState::Failed,
                   gs_error != nullptr ? gs_error : "Failed to reach host",
                   "", http_port);
        return;
    }
    if (session_ended()) {
        set_status(ViewerState::Idle, "Session ended", "", http_port);
        return;
    }

    if (!server.paired) {
        const std::string pin = generate_pin();
        set_status(ViewerState::PairingNeedPin, "Enter this PIN on the host",
                   pin, http_port);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (session_ended()) {
            set_status(ViewerState::Idle, "Session ended", "", http_port);
            return;
        }
        set_status(ViewerState::PairingInProgress,
                   "Waiting for the host to accept the PIN", pin, http_port);
        char pin_buf[5];
        std::snprintf(pin_buf, sizeof(pin_buf), "%s", pin.c_str());
        const int pair_ret = gs_pair(&server, pin_buf);
        if (pair_ret != GS_OK) {
            set_status(ViewerState::Failed,
                       gs_error != nullptr ? gs_error : "Pairing failed",
                       "", http_port);
            return;
        }
        if (session_ended()) {
            set_status(ViewerState::Idle, "Session ended", "", http_port);
            return;
        }
    }

    // Find the "Desktop" app. Sunshine's Desktop app is the one with an empty
    // command; the app list only exposes its name, so match on that.
    PAPP_LIST app_list = nullptr;
    if (gs_applist(&server, &app_list) != GS_OK) {
        set_status(ViewerState::Failed,
                   gs_error != nullptr ? gs_error : "Failed to get app list",
                   "", http_port);
        return;
    }
    int app_id = -1;
    for (PAPP_LIST app = app_list; app != nullptr; app = app->next) {
        if (std::strcmp(app->name, "Desktop") == 0) {
            app_id = app->id;
            break;
        }
    }
    while (app_list != nullptr) {
        PAPP_LIST next = app_list->next;
        free(app_list->name);
        free(app_list);
        app_list = next;
    }
    if (app_id < 0) {
        set_status(ViewerState::Failed, "No 'Desktop' app on this host",
                   "", http_port);
        return;
    }
    if (session_ended()) {
        set_status(ViewerState::Idle, "Session ended", "", http_port);
        return;
    }

    // Hardcoded M2 stream config (plan M2.2): 1920x1080@60, 20 Mbps, H.264.
    // packetSize/streamingRemotely/encryptionFlags mirror moonlight-embedded's
    // config.c defaults (1392 bytes, STREAM_CFG_AUTO, ENCFLG_ALL for CPUs with
    // AES-NI, which every x86-64 CPU since ~2011 has).
    STREAM_CONFIGURATION stream_config;
    LiInitializeStreamConfiguration(&stream_config);
    stream_config.width = 1920;
    stream_config.height = 1080;
    stream_config.fps = 60;
    stream_config.bitrate = 20000;
    stream_config.packetSize = 1392;
    stream_config.streamingRemotely = STREAM_CFG_AUTO;
    stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    stream_config.supportedVideoFormats = VIDEO_FORMAT_H264;
    stream_config.clientRefreshRateX100 = 6000;  // 60 Hz display
    stream_config.encryptionFlags = ENCFLG_ALL;

    const int start_ret = gs_start_app(&server, &stream_config, app_id,
                                       false /*sops*/, false /*localaudio*/,
                                       0 /*gamepad_mask*/);
    if (start_ret != GS_OK) {
        set_status(ViewerState::Failed,
                   gs_error != nullptr ? gs_error : "Failed to start app",
                   "", http_port);
        return;
    }
    if (session_ended()) {
        // The app was launched on the host but the connection never started;
        // quit it so the host does not keep the session running.
        gs_quit_app(&server);
        set_status(ViewerState::Idle, "Session ended", "", http_port);
        return;
    }

    CONNECTION_LISTENER_CALLBACKS conn;
    LiInitializeConnectionCallbacks(&conn);
    conn.stageStarting = &Session::on_stage_starting;
    conn.stageComplete = &Session::on_stage_complete;
    conn.stageFailed = &Session::on_stage_failed;
    conn.connectionStarted = nullptr;
    conn.connectionTerminated = &Session::on_connection_terminated;
    conn.logMessage = &Session::on_log_message;
    conn.rumble = nullptr;
    conn.connectionStatusUpdate = nullptr;
    conn.setHdrMode = nullptr;
    conn.rumbleTriggers = nullptr;
    conn.setMotionEventState = nullptr;
    conn.setControllerLED = nullptr;

    DECODER_RENDERER_CALLBACKS video;
    LiInitializeVideoCallbacks(&video);
    video.setup = &Session::on_video_setup;
    video.start = &Session::on_video_start;
    video.stop = &Session::on_video_stop;
    video.cleanup = &Session::on_video_cleanup;
    video.submitDecodeUnit = &Session::on_video_submit;
    video.capabilities = 0;

    AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeAudioCallbacks(&audio);
    audio.init = &Session::on_audio_init;
    audio.start = &Session::on_audio_start;
    audio.stop = &Session::on_audio_stop;
    audio.cleanup = &Session::on_audio_cleanup;
    audio.decodeAndPlaySample = &Session::on_audio_decode;
    audio.capabilities = 0;

    // LiStartConnection() blocks on this thread for the whole session; the
    // moonlight-common-c internal threads do the networking. The main thread
    // polls status() and calls end_session() to unblock us.
    s_active = this;
    set_status(ViewerState::Streaming, "Streaming", "", http_port);
    LiStartConnection(&server.serverInfo, &stream_config, &conn, &video, &audio,
                      nullptr, 0, nullptr, 0);
    s_active = nullptr;

    LiStopConnection();
    gs_quit_app(&server);

    // No SDL teardown here: the video cleanup callback already ran
    // decoder_deinit() (its only call site), and the renderer texture is
    // owned entirely by the main thread — main.cpp deinits it when leaving
    // Viewing mode and at shutdown. SDL renderer APIs are not thread-safe,
    // so the worker must never touch them.
    {
        std::lock_guard lock(mutex_);
        status_.pin.clear();
        if (session_ended_) {
            status_.state = ViewerState::Idle;
            status_.message = "Session ended";
        } else if (stage_failed_) {
            status_.state = ViewerState::Failed;
            status_.message = stage_failed_message_;
        } else if (termination_error_ != 0) {
            status_.state = ViewerState::Failed;
            status_.message = termination_message_;
        } else {
            status_.state = ViewerState::Idle;
            status_.message = "Connection closed";
        }
        worker_running_ = false;
    }
}

bool Session::session_ended() const {
    std::lock_guard lock(mutex_);
    return session_ended_;
}

void Session::set_status(ViewerState state, const std::string& message,
                         const std::string& pin, int port_used) {
    std::lock_guard lock(mutex_);
    status_.state = state;
    status_.message = message;
    status_.pin = pin;
    status_.port_used = port_used;
}

void Session::set_message(const std::string& message) {
    std::lock_guard lock(mutex_);
    status_.message = message;
}

void Session::set_stage_failed(const std::string& message) {
    std::lock_guard lock(mutex_);
    stage_failed_ = true;
    stage_failed_message_ = message;
    status_.message = message;
}

void Session::set_terminated(int error_code) {
    std::lock_guard lock(mutex_);
    termination_error_ = error_code;
    switch (error_code) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        termination_message_ = "The host ended the session";
        break;
    case ML_ERROR_NO_VIDEO_TRAFFIC:
        termination_message_ =
            "No video received from the host; check firewall and port forwarding";
        break;
    case ML_ERROR_NO_VIDEO_FRAME:
        termination_message_ =
            "Unstable connection; reduce the bitrate or use a faster connection";
        break;
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        termination_message_ =
            "The host ended the stream unexpectedly (video capture error)";
        break;
    case ML_ERROR_PROTECTED_CONTENT:
        termination_message_ = "The host ended the stream due to protected content";
        break;
    default:
        termination_message_ =
            "Connection terminated (error " + std::to_string(error_code) + ")";
        break;
    }
    status_.message = termination_message_;
}

// --- connection listener callbacks (run on moonlight-common-c threads) ---

void Session::on_stage_starting(int stage) {
    if (Session* s = s_active) {
        s->set_message(std::string("Starting: ") + LiGetStageName(stage));
    }
}

void Session::on_stage_complete(int stage) {
    if (Session* s = s_active) {
        s->set_message(std::string("Started: ") + LiGetStageName(stage));
    }
}

void Session::on_stage_failed(int stage, int error_code) {
    if (Session* s = s_active) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Stage failed: %s (error %d)",
                      LiGetStageName(stage), error_code);
        s->set_stage_failed(buf);
    }
}

void Session::on_connection_terminated(int error_code) {
    if (Session* s = s_active) {
        s->set_terminated(error_code);
    }
}

void Session::on_log_message(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

// --- video renderer callbacks ---

int Session::on_video_setup(int videoFormat, int width, int height,
                            int redrawRate, void* context, int drFlags) {
    (void)context;
    (void)drFlags;
    STREAM_CONFIGURATION cfg;
    LiInitializeStreamConfiguration(&cfg);
    cfg.width = width;
    cfg.height = height;
    cfg.fps = redrawRate;
    cfg.supportedVideoFormats = videoFormat;
    // Publish the negotiated geometry so main.cpp can lazily init the
    // renderer when it enters Viewing mode.
    if (Session* s = s_active) {
        std::lock_guard lock(s->mutex_);
        s->status_.stream_width = width;
        s->status_.stream_height = height;
    }
    return decoder_init(cfg);
}

void Session::on_video_start() {}

void Session::on_video_stop() {}

void Session::on_video_cleanup() {
    decoder_deinit();
}

int Session::on_video_submit(PDECODE_UNIT decodeUnit) {
    return decoder_submit(decodeUnit);
}

// --- audio renderer callbacks ---

int Session::on_audio_init(int audioConfiguration,
                           const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                           void* context, int arFlags) {
    (void)audioConfiguration;
    (void)context;
    (void)arFlags;
    if (opusConfig == nullptr) {
        return -1;
    }
    return audio_init(*opusConfig);
}

void Session::on_audio_start() {}

void Session::on_audio_stop() {}

void Session::on_audio_cleanup() {
    audio_deinit();
}

void Session::on_audio_decode(char* sampleData, int sampleLength) {
    audio_decode_sample(sampleData, sampleLength);
}

}  // namespace cosmic::viewer