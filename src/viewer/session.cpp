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

// COSMIC MODIFICATION (M5): converts the libgamestream CosmicDisplays linked
// list (owned by the caller) into the status snapshot's vector. The list is
// in document order, which is also the host's platf::display_names() ordering
// (the index contract, docs/PROTOCOL.md).
std::vector<DisplayInfo> displays_to_vector(PCOSMIC_DISPLAY displays) {
    std::vector<DisplayInfo> out;
    for (PCOSMIC_DISPLAY d = displays; d != nullptr; d = d->next) {
        DisplayInfo info;
        info.name = d->name != nullptr ? d->name : "";
        info.width = d->width;
        info.height = d->height;
        info.fps = d->fps;
        info.primary = d->primary;
        info.active = d->active;
        out.push_back(std::move(info));
    }
    return out;
}

// COSMIC MODIFICATION (M5): index of the first active display in the snapshot,
// or 0 when none is marked active (stock Sunshine host, or a host that never
// sets the flag).
int active_display_index(const std::vector<DisplayInfo>& displays) {
    for (size_t i = 0; i < displays.size(); ++i) {
        if (displays[i].active) {
            return static_cast<int>(i);
        }
    }
    return 0;
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

Session::Session(Settings& settings) : settings_(settings) {}

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
        connection_terminated_ = false;
        termination_error_ = 0;
        termination_message_.clear();
        worker_running_ = true;
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();  // Previous worker already finished.
    }
    // Snapshot the stream settings on the main thread before the worker starts
    // (plan M4.4): the user can keep editing sliders mid-connect, and reading
    // the plain scalar members from the worker would race those writes. The
    // worker gets this copy by value. M5: carry the resolution mode; "Host
    // native" is resolved from the just-fetched CosmicDisplays in the worker.
    StreamPrefs prefs;
    prefs.mode = settings_.resolution_mode;
    prefs.custom_w = settings_.custom_width;
    prefs.custom_h = settings_.custom_height;
    prefs.fps = settings_.fps;
    prefs.bitrate_kbps = settings_.bitrate_kbps;

    worker_thread_ = std::thread(&Session::worker, this, host_ip, port, prefs);
}

void Session::end_session() {
    {
        std::lock_guard lock(mutex_);
        session_ended_ = true;
    }
    // Wake the streaming worker, which owns the LiStopConnection() call:
    // Limelight documents LiStartConnection/LiStopConnection as not
    // thread-safe, so stopping from here would race the worker. If the worker
    // is instead mid-pairing (blocked in gs_pair), it picks the flag up when
    // gs_pair returns.
    state_cv_.notify_all();
}

SessionStatus Session::status() {
    std::lock_guard lock(mutex_);
    return status_;
}

bool Session::is_streaming() const {
    std::lock_guard lock(mutex_);
    return status_.state == ViewerState::Streaming;
}

// COSMIC MODIFICATION (M5): re-fetches /serverinfo on a short-lived thread and
// publishes the fresh display list + active index into the status snapshot.
// Guarded against re-entry: a second call while one refresh is in flight is a
// no-op. The refresh thread only runs while the session worker is alive
// (streaming): it reads server_, which the worker owns, and the worker joins
// the refresh thread in its tail teardown before server_ goes away.
void Session::refresh_displays() {
    // Hold the mutex across the spawn so it is atomic with the
    // refresh_allowed_ check: the worker cannot set refresh_allowed_ = false
    // and join between our check and the thread assignment.
    std::lock_guard lock(mutex_);
    // Only refresh while the worker is blocked in LiStartConnection():
    // the refresh thread shares libgamestream's static curl handle with
    // the worker, which must not be doing HTTP at the same time.
    if (refresh_running_ || !refresh_allowed_) {
        return;  // A refresh is already in flight, or not streaming.
    }
    refresh_running_ = true;
    // Reuse the thread slot. A previous refresh has finished (refresh_running_
    // was false), so this join returns immediately; without it, assigning a new
    // thread to a joinable std::thread would call std::terminate.
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
    refresh_thread_ = std::thread(&Session::refresh_worker, this);
}

void Session::refresh_worker() {
    // gs_load_serverinfo re-fetches /serverinfo (HTTPS first, then HTTP) and
    // re-parses the CosmicDisplays block into server_.displays. It shares the
    // libgamestream static curl handle with the session worker, but the worker
    // is blocked in LiStartConnection() while streaming and does no HTTP, so
    // the handle is never used concurrently. curl_easy_init() (http_init) is
    // also safe to have been called once from the worker before we get here.
    const int ret = gs_load_serverinfo(&server_);
    if (ret != GS_OK) {
        // Keep the previous snapshot; a failed refresh must not blank the
        // dropdown. gs_error is a shared global, so read it before any other
        // libgamestream call could overwrite it.
        std::string error = gs_error != nullptr ? gs_error : "refresh failed";
        std::lock_guard lock(mutex_);
        refresh_running_ = false;
        status_.message = "Monitor refresh failed: " + error;
        return;
    }
    std::lock_guard lock(mutex_);
    status_.displays = displays_to_vector(server_.displays);
    status_.active_display = active_display_index(status_.displays);
    refresh_running_ = false;
}

// COSMIC MODIFICATION (M5): records a monitor switch locally. The host has
// already switched capture via the synthesized Ctrl+Alt+Shift+F(1+i) hotkey;
// this keeps the dropdown's [active] marker in sync without another
// /serverinfo round-trip.
void Session::set_active_display(int index) {
    std::lock_guard lock(mutex_);
    if (index >= 0 && index < static_cast<int>(status_.displays.size())) {
        status_.active_display = index;
    }
}

void Session::worker(std::string host_ip, int port, StreamPrefs prefs) {
    // RAII: guarantee worker_running_ is cleared on every exit path, including
    // the early returns below, so start_connect() can retry after a failed
    // connect. This destructor is the sole writer that clears the flag (the
    // tail only updates status_ under the mutex), so a concurrent
    // start_connect() that saw false under the mutex keeps its true.
    struct WorkerDone {
        Session* s;
        ~WorkerDone() {
            std::lock_guard<std::mutex> lock(s->mutex_);
            s->worker_running_ = false;
        }
    } worker_done{this};

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

    // COSMIC MODIFICATION (M5): server_ is a member so the monitor-refresh
    // thread can re-fetch /serverinfo against it mid-stream. Zero it at the
    // start of each connection; gs_init repopulates it. (The previous
    // connection's malloc'd fields are leaked here, matching the pre-M5
    // behavior of the local SERVER_DATA going out of scope.)
    server_ = {};
    SERVER_DATA& server = server_;
    const int init_ret = gs_init(&server, address.data(), http_port,
                                 key_dir.string().c_str(), 1 /*logLevel*/,
                                 true /*unsupported*/);
    if (init_ret != GS_OK) {
        set_status(ViewerState::Failed,
                   gs_error != nullptr ? gs_error : "Failed to reach host",
                   "", http_port);
        return;
    }
    // COSMIC MODIFICATION (M5): publish the host's display list + active index
    // into the status snapshot. The worker owns server.displays for the whole
    // session; the snapshot copies the strings so the refresh thread can
    // replace the list later without invalidating UI-held copies.
    {
        std::lock_guard lock(mutex_);
        status_.displays = displays_to_vector(server.displays);
        status_.active_display = active_display_index(status_.displays);
    }
    // Host reachable: record it as a recent host (plan M3.3). Runs on the
    // worker thread; add_recent_host is mutex-protected against the main
    // thread's UI reads of the same Settings object.
    settings_.add_recent_host(host_ip);
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

    // Stream config from the settings snapshot taken at connect start
    // (plan M4.4). packetSize/streamingRemotely/encryptionFlags mirror
    // moonlight-embedded's config.c defaults (1392 bytes, STREAM_CFG_AUTO,
    // ENCFLG_ALL for CPUs with AES-NI, which every x86-64 CPU since ~2011 has).
    //
    // COSMIC MODIFICATION (M5): "Host native" resolves to the active display's
    // WxH from the just-fetched CosmicDisplays snapshot. When the list is empty
    // or the active entry has width==0 the host is stock Sunshine (no
    // CosmicDisplays block) and we fall back to 1920x1080, as documented in
    // docs/PROTOCOL.md.
    int stream_width = 0;
    int stream_height = 0;
    switch (prefs.mode) {
    case ResolutionMode::HostNative: {
        std::lock_guard lock(mutex_);
        const std::vector<DisplayInfo>& displays = status_.displays;
        const int active = active_display_index(displays);
        if (active < static_cast<int>(displays.size()) &&
            displays[active].width > 0 && displays[active].height > 0) {
            stream_width = displays[active].width;
            stream_height = displays[active].height;
        } else {
            // Stock Sunshine host: no CosmicDisplays block.
            stream_width = 1920;
            stream_height = 1080;
        }
        break;
    }
    case ResolutionMode::R1080p:
        stream_width = 1920;
        stream_height = 1080;
        break;
    case ResolutionMode::R1440p:
        stream_width = 2560;
        stream_height = 1440;
        break;
    case ResolutionMode::R2160p:
        stream_width = 3840;
        stream_height = 2160;
        break;
    case ResolutionMode::Custom:
        stream_width = prefs.custom_w;
        stream_height = prefs.custom_h;
        break;
    }
    STREAM_CONFIGURATION stream_config;
    LiInitializeStreamConfiguration(&stream_config);
    stream_config.width = stream_width;
    stream_config.height = stream_height;
    stream_config.fps = prefs.fps;
    stream_config.bitrate = prefs.bitrate_kbps;
    stream_config.packetSize = 1392;
    stream_config.streamingRemotely = STREAM_CFG_AUTO;
    stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    stream_config.supportedVideoFormats = VIDEO_FORMAT_H264;
    stream_config.clientRefreshRateX100 = prefs.fps * 100;  // display refresh x100
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
    {
        std::lock_guard lock(mutex_);
        // COSMIC MODIFICATION (M5): from here until LiStartConnection returns
        // the worker does no libgamestream HTTP, so the monitor-refresh thread
        // may safely share the static curl handle.
        refresh_allowed_ = true;
    }
    set_status(ViewerState::Streaming, "Streaming", "", http_port);
    // LiStartConnection() is NOT a blocking call: it brings every stage up,
    // fires connectionStarted() and returns, leaving the stream running on
    // moonlight-common-c's own threads. Falling straight through would reach
    // the LiStopConnection() below a few hundred milliseconds later and tear
    // down the session that just came up, so park here until the session
    // really ends: the user stopped it, or the connection terminated itself.
    const int conn_ret = LiStartConnection(&server.serverInfo, &stream_config,
                                           &conn, &video, &audio, nullptr, 0,
                                           nullptr, 0);
    if (conn_ret == 0) {
        std::unique_lock lock(mutex_);
        state_cv_.wait(lock, [this] {
            return session_ended_ || connection_terminated_;
        });
    }
    // A non-zero return means a stage failed and LiStartConnection() already
    // unwound it (the stageFailed callback recorded the reason). Every stage
    // is back at STAGE_NONE, so the LiStopConnection() below is a no-op on
    // that path, which is what makes it correct for both.
    s_active = nullptr;
    {
        std::lock_guard lock(mutex_);
        refresh_allowed_ = false;
    }

    // COSMIC MODIFICATION (M5): join the refresh thread BEFORE gs_quit_app:
    // the refresh thread shares libgamestream's static curl handle with the
    // worker, and gs_quit_app does HTTP on it. Joining first guarantees the
    // handle is never used concurrently. (If a refresh is in flight this can
    // wait up to the refresh's 30 s curl timeout; the session is ending
    // anyway.) Not holding the mutex here: the refresh thread takes it when
    // publishing its snapshot, and joining while holding it would deadlock.
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
    {
        std::lock_guard lock(mutex_);
        refresh_running_ = false;
    }

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
    // Also to stderr: the UI shows this, but a session that fails while the
    // window is hidden to the tray would otherwise leave no trace anywhere.
    std::fprintf(stderr, "[session] %s\n", message.c_str());
    std::fflush(stderr);
    std::lock_guard lock(mutex_);
    stage_failed_ = true;
    stage_failed_message_ = message;
    status_.message = message;
}

void Session::set_terminated(int error_code) {
    std::string message;
    {
    std::lock_guard lock(mutex_);
    termination_error_ = error_code;
    connection_terminated_ = true;
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
    message = termination_message_;
    }
    // Wakes the worker parked after LiStartConnection().
    state_cv_.notify_all();
    std::fprintf(stderr, "[session] terminated: %s (code %d)\n",
                 message.c_str(), error_code);
    std::fflush(stderr);
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
