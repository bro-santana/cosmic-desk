// Cosmic Desk — one binary that is both the host (screen sharer) and the
// viewer (client). See PLAN.md for the architecture and milestone breakdown.
//
// M0 scope: SDL window + Dear ImGui + tray icon + settings file. The host
// threads (M1) and the viewer session (M2) plug into this loop later.

#include "app/autostart.h"
#include "app/clipimage.h"
#include "app/clipsync.h"
#include "app/presence.h"
#include "app/settings.h"
#include "app/service_ctrl.h"
#include "app/single_instance.h"
#include "app/state.h"
#include "app/wallcache.h"
#include "hostglue/clipboard.h"
#include "hostglue/host.h"
#include "hostglue/pin_bridge.h"
#include "hostglue/wallpaper.h"
#include "ui/bridge/bridge.h"
#include "ui/bridge/design.h"
#include "ui/bridge/scene.h"
#include "ui/pin_dialog.h"
#include "ui/scale.h"
#include "ui/theme.h"
#include "ui/tray.h"
#include "ui/viewer_topbar.h"
#include "viewer/decoder.h"
#include "viewer/input.h"
#include "viewer/session.h"
#include "viewer/vrenderer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace {

// Default window size: 16:9-ish to match the Bridge UI's design canvas
// (docs/UI_MIGRATION.md U0).
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

// The vendored host resolves SUNSHINE_ASSETS_DIR="assets" (HLSL/GL shaders)
// relative to the CWD, and autostart/menu launches don't set it; chdir makes
// the packaged layout work. Failure is silent: keep the original CWD.
void chdir_to_executable_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        std::fprintf(stderr, "chdir: GetModuleFileNameW failed (%lu)\n",
                     GetLastError());
        return;
    }
    // Strip the filename, keep the directory.
    if (wchar_t* slash = wcsrchr(buf, L'\\'); slash != nullptr) {
        *slash = L'\0';
    }
    if (!SetCurrentDirectoryW(buf)) {
        std::fprintf(stderr, "chdir: SetCurrentDirectoryW failed (%lu)\n",
                     GetLastError());
    }
#else
    char buf[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        std::fprintf(stderr, "chdir: readlink(/proc/self/exe) failed\n");
        return;
    }
    buf[len] = '\0';
    // Strip the filename, keep the directory.
    if (char* slash = strrchr(buf, '/'); slash != nullptr) {
        *slash = '\0';
    }
    if (chdir(buf) != 0) {
        std::fprintf(stderr, "chdir: chdir(%s) failed\n", buf);
    }
#endif
}

std::string asset_path(const char* file_name) {
    std::string path;
    if (const char* base = SDL_GetBasePath()) {
        path = base;
    }
    path += "assets/";
    path += file_name;
    return path;
}

const char* tray_icon_name() {
#ifdef _WIN32
    return "icon.ico";  // ExtractIconEx needs a real .ico
#else
    return "icon.png";  // AppIndicator takes a png path
#endif
}

// Applies or releases the keyboard grab (plan M4.3). While grabbed, Alt+Tab
// and the Win key act on the remote machine; SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4
// set to "0" keeps Alt+F4 from closing the window (Windows-only hint,
// harmless elsewhere).
void apply_input_grab(SDL_Window* window, bool grabbed) {
    SDL_SetWindowKeyboardGrab(window, grabbed);
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, grabbed ? "0" : "1");
#endif
}

// 64-bit FNV-1a. Not cryptographic — just a fingerprint of clipboard text used
// to detect changes cheaply (see the clipboard-sync state below), same
// algorithm as wallcache.cpp's Fnv1a32 with 64-bit constants.
uint64_t Fnv1a64(const std::string& s) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Backs an inbound image apply (see the clipboard-in drain below): owns both
// byte strings offered to SDL_SetClipboardData for the duration SDL needs
// them. Heap-allocated and handed to SDL as userdata. Every path
// SDL_SetClipboardData can take FROM THE CALL SITE BELOW transfers ownership
// of this pointer to SDL, success or failure alike (on failure it leaves the
// callback/userdata installed and cleans up later, via
// SDL_CancelClipboardData, the next SDL_SetClipboardData/SDL_SetClipboardText
// call, or at SDL_Quit): SDL_clipboard.c has two earlier returns that leave
// a passed-in userdata unowned (uninitialized video; invalid parameters --
// null callback/mime_types or a zero count), but neither is reachable from
// our call site, since video is already initialized by the time the main
// loop is running and our arguments are always the valid non-null
// callback + mime_types + count>0 shape. The call site must therefore never
// delete this itself -- only the cleanup callback below does.
struct ClipboardImagePayload {
    std::string bmp;
    std::string png;
};

// SDL_ClipboardDataCallback: called from SDL's C frames whenever another
// application (or SDL itself, servicing SDL_GetClipboardData) asks for one of
// the mime types offered below, so no exception may escape it, and it must
// tolerate being asked for a mime type it does not carry (returns nullptr
// with *size = 0, which SDL_ClipboardDataCallback's contract treats as
// zero-length data).
const void* clipboard_image_data(void* userdata, const char* mime_type, size_t* size) {
    const auto* payload = static_cast<const ClipboardImagePayload*>(userdata);
    if (SDL_strcmp(mime_type, "image/bmp") == 0) {
        *size = payload->bmp.size();
        return payload->bmp.data();
    }
    if (SDL_strcmp(mime_type, "image/png") == 0) {
        *size = payload->png.size();
        return payload->png.data();
    }
    *size = 0;
    return nullptr;
}

// SDL_ClipboardCleanupCallback: called by SDL once this payload is no longer
// needed (cleared, superseded by a later SDL_SetClipboardData/SetClipboardText
// call, or at SDL_Quit) -- the only place that frees it.
void clipboard_image_cleanup(void* userdata) {
    delete static_cast<ClipboardImagePayload*>(userdata);
}

// Leaves the Viewing UI (plan M4.3): flush held input, release the keyboard
// grab, restore the hints Viewing changed, and tear down the video renderer.
// Used both when the session ends and when the window is hidden to the tray
// mid-stream (the session keeps running in the background). Idempotent: each
// step is guarded by the state it owns, so a second call is a no-op.
void leave_viewing_ui(SDL_Window* window, bool* input_grabbed,
                      bool* vrenderer_active) {
    // Exit fullscreen for the main window UI, keep viewer_fullscreen for next
    // session.
    SDL_SetWindowFullscreen(window, false);
    // Undo the cursor hiding the Viewing UI applies over the video.
    SDL_ShowCursor();
    // Release anything still held, then drop the grab so the host does not
    // keep stuck keys (moonlight-qt raiseAllKeys pattern). Flushing is
    // independent of the grab: keys can be held even when the grab is off.
    cosmic::viewer::input::flush_input_state();
    if (*input_grabbed) {
        apply_input_grab(window, false);
        *input_grabbed = false;
    }
    // Restore the hints Viewing changed: minimize-on-focus-loss back to "1"
    // as the baseline for the next Viewing session (the hint only takes
    // effect while fullscreen), and the Alt+F4 close hint back to "1"
    // (apply_input_grab already reset the latter when releasing the grab,
    // but restore it explicitly in case the grab was already off).
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "1");
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, "1");
#endif
    if (*vrenderer_active) {
        cosmic::viewer::vrenderer_deinit();
        *vrenderer_active = false;
    }
}

// Viewer session (plan M2.2): one session object for the whole app lifetime;
// its worker thread does all networking so the main loop never blocks. Created
// in main() once the Settings object exists (the session needs it to record
// paired hosts).
std::unique_ptr<cosmic::viewer::Session> g_session;

// COSMIC MODIFICATION (M5): converts the session's display snapshot into the
// top bar's monitor list. The UI layer (cosmic::ui) does not depend on the
// viewer session, so main.cpp bridges the two structs.
std::vector<cosmic::ui::MonitorInfo> to_monitor_info(
    const std::vector<cosmic::viewer::DisplayInfo>& displays) {
    std::vector<cosmic::ui::MonitorInfo> out;
    out.reserve(displays.size());
    for (const auto& d : displays) {
        cosmic::ui::MonitorInfo m;
        m.name = d.name;
        m.width = d.width;
        m.height = d.height;
        m.active = d.active;
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    bool start_hidden = false;
    // Set when cosmicsvc spawned us (plan M8.2): the service respawns us
    // whenever we exit, so a tray Quit must exit with
    // ERROR_SHUTDOWN_IN_PROGRESS (1115) to stop the service too.
    bool service_mode = false;
    // Set by the Start-Menu shortcut (plan M9): --shortcut signals the running
    // instance to show its window (starting the service first if needed);
    // --shortcut-admin is the elevated relaunch that starts the service and
    // exits. Windows-only in purpose; the handler block below is #ifdef'd.
    bool shortcut_launch = false;
    bool shortcut_admin = false;
    // --connect <ip> starts a viewer session as soon as the app is up, without
    // going through the window. Exists for the two-machine interop matrix
    // (PLAN.md S9), which otherwise cannot be driven from a script.
    std::string autoconnect_ip;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hidden") {
            start_hidden = true;
        } else if (arg == "--service") {
            service_mode = true;
        } else if (arg == "--shortcut") {
            shortcut_launch = true;
        } else if (arg == "--shortcut-admin") {
            shortcut_admin = true;
        } else if (arg == "--connect" && i + 1 < argc) {
            autoconnect_ip = argv[++i];
        }
    }

    // Start-Menu shortcut flow (plan M9; upstream config.cpp:1461-1500
    // semantics, implemented here because our UI is the app). Runs before the
    // single-instance acquire: a shortcut must work while an instance is
    // already running -- it only signals that instance to show its window.
#ifdef _WIN32
    if (shortcut_admin) {
        // Elevated relaunch from --shortcut: start the service and exit; never
        // run the real app (upstream returns 1 for the same reason).
        cosmic::service_ctrl::start_service();
        return 1;
    }
    if (shortcut_launch) {
        // Cheap load for port_base; the real Settings object is created later.
        cosmic::Settings settings = cosmic::Settings::load();
        if (!cosmic::service_ctrl::is_service_running()) {
            // Relaunch ourselves elevated to start the service: one UAC prompt,
            // then the elevated process starts the service and exits.
            wchar_t executable[MAX_PATH];
            if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) {
                std::fprintf(stderr, "--shortcut: GetModuleFileNameW failed (%lu)\n",
                             GetLastError());
                return 1;
            }
            SHELLEXECUTEINFOW shell_exec_info{};
            shell_exec_info.cbSize = sizeof(shell_exec_info);
            shell_exec_info.fMask =
                SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS;
            shell_exec_info.lpVerb = L"runas";
            shell_exec_info.lpFile = executable;
            shell_exec_info.lpParameters = L"--shortcut-admin";
            shell_exec_info.nShow = SW_NORMAL;
            if (!ShellExecuteExW(&shell_exec_info)) {
                std::fprintf(stderr, "--shortcut: ShellExecuteExW failed (%lu)\n",
                             GetLastError());
                return 1;
            }
            // Wait for the elevated process to finish starting the service.
            WaitForSingleObject(shell_exec_info.hProcess, INFINITE);
            CloseHandle(shell_exec_info.hProcess);
        }
        if (!cosmic::service_ctrl::is_service_running()) {
            // The service is not installed (portable install): do not stall
            // for 30 s waiting for a UI that will never come. A per-user
            // install cannot run the service safely (the LocalSystem service
            // would execute binaries from a user-writable folder), so point
            // the user at the machine-wide reinstall.
            MessageBoxW(nullptr,
                        L"Could not start the Cosmic Desk service. It requires the "
                        L"machine-wide install: reinstall Cosmic Desk and keep the "
                        L"service option checked.",
                        L"Cosmic Desk", MB_ICONWARNING | MB_OK);
            return 1;
        }
        // The service-spawned instance is starting; wait for its host port.
        // The shortcut reads port_base from the USER profile while the
        // service-spawned host reads the SYSTEM profile (PLAN.md D9), so the
        // configured port can diverge; 47989 is the default the host uses.
        // The readiness result still does not block the show-window signal.
        if (!cosmic::service_ctrl::wait_for_ui_ready(settings.port_base)) {
            cosmic::service_ctrl::wait_for_ui_ready(47989);
        }
        // Signal the running instance to show its window (plan M8.1). The
        // event may not exist yet (startup race), in which case there is
        // nothing to signal.
        HANDLE event =
            OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\CosmicDesk.ShowWindow");
        if (event != nullptr) {
            SetEvent(event);
            CloseHandle(event);
        }
        return 0;
    }
#endif

    // Single-instance guard (plan M8.1): a second launch must not create a
    // second process (the host ports would clash). The running instance shows
    // its window; we exit.
    if (!cosmic::single_instance::acquire()) {
        return 0;
    }

    // Run from the executable's directory so the vendored host's CWD-relative
    // SUNSHINE_ASSETS_DIR="assets" resolves regardless of how we were launched.
    chdir_to_executable_dir();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Per-monitor-v2 DPI awareness: crisp rendering and correct scaling on
    // HiDPI/mixed-DPI setups. SDL3 makes Windows apps per-monitor-v2 DPI
    // aware by default. SDL_WINDOW_HIGH_PIXEL_DENSITY + the ImGui backends
    // then report window sizes and mouse positions in the same (DPI-scaled)
    // coordinate space, so the UI, the top bar, and the viewer's mouse
    // mapping all line up.
    SDL_Window* window = SDL_CreateWindow(
        "Cosmic Desk", kWindowWidth, kWindowHeight,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // Vsync is a renderer property, set once after creation. Not every driver
    // supports it (SDL_render.h), so a failure here is logged but not fatal:
    // SDL3 has no accelerated-required renderer flag, and a software-renderer
    // host without vsync is a supported (if uncapped) fallback.
    if (!SDL_SetRenderVSync(renderer, 1)) {
        std::fprintf(stderr, "SDL_SetRenderVSync failed: %s\n", SDL_GetError());
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    cosmic::ui::StyleColorsDefault();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // U0 scene; draws behind the UI in MainWindow mode.
    cosmic::ui::scene::init(renderer);

    // HiDPI (see ui/scale.h): SDL3's per-monitor-v2 DPI awareness gives us the
    // raw pixel grid, so ImGui has to be told the display scale or everything it
    // draws comes out at 96-DPI sizes on a 4K panel. Must follow the backend
    // init: rebuilding the atlas drops the backend's font texture.
    cosmic::ui::apply(window);
    // The window was created at 96-DPI sizes; grow it to match, but never past
    // the display's usable area (a 2.25x scale would otherwise ask for a
    // 2250x1440 window on a 1920x1080 screen).
    {
        const float ui_scale = cosmic::ui::scale();
        SDL_Rect usable = {0, 0, 0, 0};
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable)) {
            usable.w = 0;
            usable.h = 0;
        }
        int width = static_cast<int>(kWindowWidth * ui_scale);
        int height = static_cast<int>(kWindowHeight * ui_scale);
        if (usable.w > 0 && width > usable.w) {
            width = usable.w;
        }
        if (usable.h > 0 && height > usable.h) {
            height = usable.h;
        }
        SDL_SetWindowSize(window, width, height);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        // The Bridge layout needs room.
        SDL_SetWindowMinimumSize(window, static_cast<int>(960 * ui_scale),
                                 static_cast<int>(540 * ui_scale));
    }

    cosmic::Settings settings = cosmic::Settings::load();
    // The OS owns the autostart entry; cosmic.json only mirrors it so the
    // settings toggle has a value to render. The two drift whenever the writer
    // and the reader disagree about which config store they use: an elevated
    // manual run writes the Run key under the user's hive but cosmic.json to
    // ProgramData (PLAN.md D9), so the next unelevated start reads a file that
    // never saw the write and shows the toggle off while the app does start at
    // logon. The OS is the source of truth; correct the file to match.
    //
    // Skipped in service mode: cosmicsvc spawns us as SYSTEM, so HKCU is the
    // SYSTEM profile's hive rather than the logged-on user's. Reading it would
    // report an unrelated account's autostart and overwrite the user's stored
    // value; the settings panel locks the toggle in this mode for the same
    // reason.
    bool autostart_drifted = false;
    if (!service_mode) {
        const bool os_autostart = cosmic::autostart::enabled();
        autostart_drifted = settings.autostart != os_autostart;
        settings.autostart = os_autostart;
    }
    if (autostart_drifted || !std::filesystem::exists(cosmic::Settings::config_file())) {
        // Materialize defaults on first run so the file is there to be edited,
        // and persist a corrected autostart so the drift is fixed once.
        settings.save();
    }

    g_session = std::make_unique<cosmic::viewer::Session>(settings);

    // Seed the clipboard-enabled mirror before starting the host: start()
    // below also does this (host.cpp), but it can return early (e.g. a
    // host.conf write failure) before reaching that line, and the app keeps
    // running in that case (see hosting_ok below) -- the viewer role must
    // still honour the user's share_clipboard opt-out on that path. Setting
    // it here too is idempotent with the one inside start().
    cosmic::clipboard::set_enabled(settings.share_clipboard);

    const bool hosting_ok = cosmic::hostglue::start(settings);
    if (!hosting_ok) {
        // Hosting is degraded but the app keeps running (plan M1.4): the UI and
        // viewer role still work, and the host log explains what failed.
        std::fprintf(stderr, "Hosting failed to start; continuing without it.\n");
    }

    cosmic::AppMode mode = start_hidden ? cosmic::AppMode::HiddenToTray : cosmic::AppMode::MainWindow;
    bool running = true;
    // Set on the hidden->shown transition so the Bridge replays its logo
    // splash (bridge_state is declared after the tray callbacks that need to
    // request the replay, hence the flag instead of a direct reset).
    bool replay_boot_splash = false;
    // Set when the tray Quit item is clicked, so main() can tell a tray quit
    // apart from other exits (plan M8.2: in service mode a tray quit must exit
    // with ERROR_SHUTDOWN_IN_PROGRESS so cosmicsvc stops instead of respawning
    // us).
    bool tray_quit_requested = false;

    const bool has_tray = cosmic::ui::tray_start(
        asset_path(tray_icon_name()),
        {
            [&] {
                if (mode == cosmic::AppMode::HiddenToTray) {
                    replay_boot_splash = true;
                }
                mode = cosmic::AppMode::MainWindow;
                SDL_ShowWindow(window);
                SDL_RaiseWindow(window);
            },
            [&] {
                tray_quit_requested = true;
                running = false;
            },
        });

    if (!has_tray) {
        // Without a tray there is nowhere to hide to, so never start hidden.
        std::fprintf(stderr, "Tray unavailable; keeping the window visible.\n");
        mode = cosmic::AppMode::MainWindow;
    }

    if (mode == cosmic::AppMode::MainWindow) {
        SDL_ShowWindow(window);
    }

    // Address of the machine a Connect is in flight to ("" = none). Drives the
    // Bridge card's LINKING... state; kept while the session is Connecting or
    // Streaming and cleared when it returns to Idle/Failed.
    std::string connecting_address;

    if (!autoconnect_ip.empty()) {
        // U5: the autoconnect path warps out like a card Connect. Seed the
        // connecting address so the STREAMING label can show the nickname.
        connecting_address = autoconnect_ip;
        cosmic::ui::scene::set_warp_target(1.0f);
        cosmic::ui::scene::trigger_warp_flash();
        g_session->start_connect(autoconnect_ip, settings.port_base);
    }

    // Viewer renderer is created lazily once the negotiated stream dimensions
    // are known and destroyed when leaving Viewing mode (plan M2.4).
    bool vrenderer_active = false;

    // Viewer fullscreen state (plan M4.2): kept across sessions — the user's
    // last choice sticks. Applied via SDL_SetWindowFullscreen after the frame.
    bool viewer_fullscreen = false;

    // Keyboard-grab state (plan M4.3): while true, SDL_SetWindowKeyboardGrab
    // captures Alt+Tab / Win so they act on the remote machine. Toggled by the
    // Ctrl+Alt+Shift+Z escape combo; SDL releases the grab implicitly on focus
    // loss, so the logical state and the SDL state can briefly disagree.
    bool input_grabbed = false;

    // Top-bar auto-hide state (plan M4.1): persists across frames while
    // streaming.
    cosmic::ui::TopBarState topbar_state;

    // Bridge overlay state (docs/UI_MIGRATION.md U2): persists the boot
    // sequence across frames (and across hide/show cycles).
    cosmic::ui::bridge::BridgeState bridge_state;
    // Last host set handed to the presence poller (U6): presence::start is
    // cheap, but we only call it when the address/port list actually changed
    // (pair success adds a host; Edit/Remove change entries) so the worker's
    // poll set stays in sync without a per-frame call.
    std::vector<std::pair<std::string, int>> last_poll_hosts;
    // Backdrop memo (PLAN.md D10(e)): wallcache::path_for takes a mutex shared
    // with the presence worker and scans the cache directory on a miss, so it
    // must not run every frame. Cached here and only re-queried when the
    // focused address or its advertised hash changes (see the SceneInput fill
    // below).
    struct BackdropMemo {
        std::string address;                // last address queried ("" = none)
        std::string hash;                   // that address's wallpaper hash at query time
        std::filesystem::path path;         // wallcache::path_for result
        uint64_t last_query_ms = 0;         // SDL_GetTicks() at the last query
    } backdrop_memo;
    // Settings edits save on the Settings panel's close transition, not per
    // tick (docs/UI_MIGRATION.md U4); the shutdown save() is the fallback.
    bool settings_dirty = false;
    // Pair latch: while an explicit Pair is in flight, the main loop turns the
    // session status into a PairProgress for the Bridge's Pair modal. pair_error
    // is sticky — it stays set until the next StartPair, since state==Failed is
    // true for only one frame.
    bool pair_in_flight = false;
    std::string pair_address;
    // Port override the user chose in the Pair modal, applied only once the
    // handshake succeeds. Nothing is written to the host list before then: an
    // optimistic add would set paired=false on a machine that is already in the
    // list (and an undo-on-failure would delete it, nickname and all) whenever
    // the user re-pairs an existing entry.
    int pair_port = 0;
    // Optional nickname typed in the Pair modal, applied with the port once the
    // handshake succeeds. Empty means "leave it alone", so re-pairing a machine
    // that already has a nickname never clears it.
    std::string pair_nickname;
    std::string pair_error;

    // Pending pairing state (plan M1.4): set when the host thread reports a
    // /pair request; consumed by the PIN dialog below.
    std::string g_pending_client;
    bool show_pin_dialog = false;
    bool pin_result_ok = false;

    // Clipboard sync (host->clipboard.h and viewer->clipsync.h, wired below):
    // hash of the text this machine's clipboard is last known to hold (0 =
    // none) -- updated both when we publish a local copy and when we apply an
    // incoming one, so it always reflects the current clipboard content
    // regardless of which side changed it. An SDL_EVENT_CLIPBOARD_UPDATE
    // whose text still matches this hash is an echo (of our own write or of a
    // copy we already published) and is not re-published.
    uint64_t clipboard_applied_hash = 0;
    // Tracks whether the clipsync worker is running so start/stop is driven
    // by the session state (Streaming) exactly once per transition.
    bool clipsync_running = false;
#ifdef _WIN32
    // Last-seen Windows clipboard sequence number (see the poll below).
    DWORD last_clip_seq = 0;
    // Registered once: RegisterClipboardFormatW looks up (and registers, the
    // first time any process in the session calls it) a format atom --
    // caching it avoids repeating that call on every SDL_EVENT_CLIPBOARD_UPDATE.
    // "PNG" is the exact name SDL's own Windows clipboard backend registers
    // for image/png (see third-party/SDL/src/video/windows/
    // SDL_windowsclipboard.c's GetClipboardFormatPNG), so this reads the same
    // clipboard format SDL itself would set or get.
    const UINT kPngClipboardFormat = RegisterClipboardFormatW(L"PNG");
    // Sequence number the Windows clipboard had right after our own last
    // successful image apply (0 = none), checked by the
    // SDL_EVENT_CLIPBOARD_UPDATE handler below to recognize that apply's own
    // echo -- see that handler for why this, rather than a byte hash, is
    // needed for images. This alone is sufficient: the handler compares the
    // clipboard's current sequence number against exactly this value, so
    // only the specific write this variable records can ever be suppressed.
    // The apply path deliberately does NOT also stamp last_clip_seq above
    // (an earlier version of this did, reasoning it would additionally stop
    // the poll from synthesizing a duplicate event for our own write) --
    // that would be actively harmful, not just redundant: last_clip_seq is
    // the poll's "already seen" cursor for detecting *external* changes, and
    // if some other process happens to copy something in the very same tick
    // as our apply, latching last_clip_seq to our write's sequence number
    // would make the poll treat that other, genuine external change as
    // already seen and silently drop it. image_applied_seq is only ever
    // written by the apply path.
    DWORD image_applied_seq = 0;
#endif

    while (running) {
        SDL_Event event;
        // Set by the Ctrl+Alt+Shift+Q/Enter/Z escape combos while streaming;
        // applied after the frame (fullscreen changes and session teardown
        // must not happen mid-ImGui-frame).
        cosmic::viewer::input::InputActions input_actions;
        // While a popup is open (the top bar's monitor dropdown) ImGui owns the
        // mouse for the whole window, not just the rectangle it covered last
        // frame. WantCaptureMouse is one frame stale, so on the frame the
        // cursor crosses from the bar onto the popup list it still reads false
        // and the click is forwarded to the host instead. ImGui then sees a
        // click over none of its own windows and runs its "clicking on void
        // disables focus" path, which closes the popup -- that is what made the
        // other monitor's entry vanish as soon as the list was hovered.
        // Read before NewFrame(): the open-popup stack persists across frames.
        const bool imgui_popup_open = ImGui::IsPopupOpen(
            "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
#ifdef _WIN32
        // SDL3's Windows backend only checks GetClipboardSequenceNumber on
        // focus gain (WIN_UpdateFocus -> WIN_CheckClipboardUpdate); it uses no
        // clipboard-format listener, so a tray-hidden/service-mode window never
        // sees external copies. Poll the sequence each tick and push the same
        // SDL event so the SDL_EVENT_CLIPBOARD_UPDATE handler below stays the
        // single code path (its hash echo-suppression absorbs duplicates from
        // SDL's own focus-gain and self-set events).
        {
            const DWORD clip_seq = GetClipboardSequenceNumber();
            if (clip_seq != 0 && clip_seq != last_clip_seq) {
                if (last_clip_seq != 0) {
                    SDL_Event clip_ev{};
                    clip_ev.type = SDL_EVENT_CLIPBOARD_UPDATE;
                    SDL_PushEvent(&clip_ev);
                }
                last_clip_seq = clip_seq;
            }
        }
#endif
        while (SDL_PollEvent(&event)) {
            // Clipboard-out: placed first for clarity, ahead of the viewer
            // input-forwarding block below -- ordering is not load-bearing
            // here, since cosmic::viewer::input::handle_event does not
            // consume SDL_EVENT_CLIPBOARD_UPDATE (see viewer/input.cpp's
            // default case). SDL clipboard calls only ever happen here, on
            // the main thread.
            if (event.type == SDL_EVENT_CLIPBOARD_UPDATE) {
                // Check the length before materialising the string:
                // SDL_GetClipboardText does a full UTF-16->UTF-8 conversion,
                // and copying that into a std::string is a second full copy
                // -- for a clipboard payload over kMaxBytes, doing that
                // unconditionally would stall the main loop just to discard
                // the result. SDL_free runs on every path regardless.
                char* clip = SDL_GetClipboardText();
                std::string text;
                if (clip != nullptr && clip[0] != '\0') {
                    const size_t clip_len = SDL_strlen(clip);
                    if (clip_len <= cosmic::clipboard::kMaxBytes) {
                        text.assign(clip);
                    } else {
                        // At most one line per clipboard-update event, not per tick; SDL focus-gain
                        // can duplicate it, since the echo-suppression hash below skips this branch.
                        std::fprintf(stderr,
                                     "clipboard: local copy of %zu bytes "
                                     "exceeds the %zu-byte cap; not "
                                     "shared.\n",
                                     clip_len, cosmic::clipboard::kMaxBytes);
                    }
                }
                SDL_free(clip);
                if (!text.empty()) {
                    const uint64_t hash = Fnv1a64(text);
                    if (hash != clipboard_applied_hash) {
                        // Record this as the clipboard's current content
                        // before publishing, so a genuinely new local copy is
                        // never mistaken for an echo of stale remote text
                        // (the bug this fixes: without this line, copying
                        // back to a value applied from the peer even one step
                        // earlier would be silently swallowed).
                        clipboard_applied_hash = hash;
                        // Publishing to the host bridge is unconditional by
                        // design: the /cosmic/clipboard route owns the
                        // enable/session gate (see clipboard.h), not this
                        // call site.
                        cosmic::clipboard::publish(cosmic::clipboard::Mime::Text, text);
                        // clipsync has no route of its own to gate it, unlike
                        // the host bridge above: cosmic::clipboard::enabled()
                        // is the live mirror of settings.share_clipboard and
                        // is the single per-machine toggle that deliberately
                        // governs both the host role and this viewer role, so
                        // gate the POST here.
                        if (cosmic::clipboard::enabled()) {
                            cosmic::clipsync::publish_local(cosmic::clipsync::Mime::Text, text);
                        }
                    }
                } else {
                    // No text on the clipboard: it may hold an image instead.
                    // Text always wins when both are present -- e.g. a
                    // browser copy that also places HTML/plain text alongside
                    // the image -- because this branch only runs once
                    // SDL_GetClipboardText above has already come back empty.
#ifdef _WIN32
                    // Live Win32 format queries: cheap (IsClipboardFormatAvailable
                    // opens no clipboard handle and performs no transfer) and
                    // always current, unlike SDL_GetClipboardMimeTypes (stale
                    // for a tray-hidden window -- see the sequence-poll
                    // comment above) or this event, which the poll
                    // synthesizes with no mime list at all (num_mime_types ==
                    // 0).
                    const bool clip_has_png = IsClipboardFormatAvailable(kPngClipboardFormat);
                    const bool clip_has_bmp = IsClipboardFormatAvailable(CF_DIBV5) ||
                                              IsClipboardFormatAvailable(CF_DIB);
                    // image_applied_seq is stamped by our own image apply
                    // below with the sequence number the clipboard had right
                    // after SDL_SetClipboardData returned (SDL 3.4 writes to
                    // the clipboard synchronously, so the number has already
                    // advanced by then; see image_applied_seq's declaration
                    // for why the apply path stamps only this variable and
                    // not last_clip_seq). If the sequence has not moved
                    // since, this event -- whether it is the one
                    // SDL_SetClipboardData itself queues on success, or a
                    // duplicate the poll above lets through -- is that
                    // apply's own echo, and capturing it again would bounce
                    // the image straight back to the peer forever. A byte
                    // hash, as used for text above, cannot catch this: our
                    // apply only ever puts CF_DIB on the clipboard, so
                    // re-capturing it transcodes to different PNG bytes than
                    // the ones we applied.
                    const bool is_echo = image_applied_seq != 0 &&
                                         GetClipboardSequenceNumber() == image_applied_seq;
#else
                    // Decide only from the event's own mime list:
                    // SDL_HasClipboardData would work too, but on X11 it
                    // performs a full clipboard transfer (a round-trip to the
                    // selection owner) per call, which this handler cannot
                    // afford for events it may end up ignoring entirely.
                    bool clip_has_png = false;
                    bool clip_has_bmp = false;
                    for (Sint32 i = 0; i < event.clipboard.num_mime_types; ++i) {
                        const char* mt = event.clipboard.mime_types[i];
                        if (SDL_strcmp(mt, "image/png") == 0) {
                            clip_has_png = true;
                        } else if (SDL_strcmp(mt, "image/bmp") == 0) {
                            clip_has_bmp = true;
                        }
                    }
                    // owner == true means this update is our own write (see
                    // SDL_ClipboardEvent's doc comment): the apply below just
                    // ran SDL_SetClipboardData. Unlike Windows there is no
                    // separate sequence-number channel here, but none is
                    // needed -- X11/Wayland only mark an event as owned when
                    // this process is the current selection owner, so it
                    // cannot alias a later external change the way a bare
                    // sequence number could.
                    const bool is_echo = event.clipboard.owner;
#endif
                    if (!is_echo && (clip_has_png || clip_has_bmp)) {
                        // The clipboard holds an image, not the text (if any)
                        // this hash last described -- clear it now, on
                        // detection, regardless of whether the capture below
                        // actually succeeds: an oversize or failed-transcode
                        // image still means the previously recorded text is
                        // no longer on the clipboard, and leaving the hash
                        // pointing at it would make the user's next copy of
                        // that same text look like an echo and be silently
                        // swallowed.
                        clipboard_applied_hash = 0;
                        // Prefer the native PNG -- zero-loss -- and fall back
                        // to the BMP file SDL synthesizes from CF_DIB/CF_DIBV5
                        // on Windows, transcoding it (see clipimage.h).
                        // SDL_GetClipboardData's buffer is freed on every path
                        // below.
                        std::string png;
                        bool have_png = false;
                        if (clip_has_png) {
                            size_t png_size = 0;
                            void* png_data = SDL_GetClipboardData("image/png", &png_size);
                            if (png_data != nullptr) {
                                if (png_size > 0 && png_size <= cosmic::clipboard::kMaxImageBytes) {
                                    png.assign(static_cast<const char*>(png_data), png_size);
                                    have_png = true;
                                } else if (png_size > cosmic::clipboard::kMaxImageBytes) {
                                    // At most one line per clipboard-update
                                    // event -- see the oversize-text log above
                                    // for the same reasoning.
                                    std::fprintf(stderr,
                                                 "clipboard: local image of %zu bytes "
                                                 "exceeds the %zu-byte cap; not "
                                                 "shared.\n",
                                                 png_size, cosmic::clipboard::kMaxImageBytes);
                                }
                                SDL_free(png_data);
                            }
                        }
                        if (!have_png && clip_has_bmp) {
                            size_t bmp_size = 0;
                            void* bmp_data = SDL_GetClipboardData("image/bmp", &bmp_size);
                            if (bmp_data != nullptr) {
                                if (bmp_size > 0 &&
                                    cosmic::clipimage::bmp_to_png(
                                        bmp_data, bmp_size,
                                        cosmic::clipboard::kMaxImageBytes, png)) {
                                    have_png = true;
                                } else if (bmp_size > 0) {
                                    std::fprintf(stderr,
                                                 "clipboard: local image failed to "
                                                 "transcode or exceeds the %zu-byte "
                                                 "cap; not shared.\n",
                                                 cosmic::clipboard::kMaxImageBytes);
                                }
                                SDL_free(bmp_data);
                            }
                        }
                        if (have_png) {
                            // clipboard_applied_hash was already cleared on
                            // detection above, regardless of this outcome.
                            // Same publish pattern as the text path above:
                            // unconditional to the host bridge (the route
                            // owns the enable/session gate), gated behind
                            // enabled() for clipsync. png is moved into the
                            // second call only, after the first has already
                            // read it by const reference.
                            cosmic::clipboard::publish(cosmic::clipboard::Mime::Png, png);
                            if (cosmic::clipboard::enabled()) {
                                cosmic::clipsync::publish_local(cosmic::clipsync::Mime::Png,
                                                                std::move(png));
                            }
                        }
                    }
                }
            }
            // While streaming, forward input to the host before ImGui sees it
            // (plan M2.6). Consumed events never reach ImGui; the top bar keeps
            // working because ImGui claims mouse capture when the cursor is
            // over it (WantCaptureMouse), which gates forwarding. The
            // Ctrl+Alt+Shift+Q/Enter/Z escape combos stay active regardless.
            if (mode == cosmic::AppMode::Viewing &&
                cosmic::viewer::input::handle_event(
                    event, window, ImGui::GetIO().WantCaptureKeyboard,
                    ImGui::GetIO().WantCaptureMouse || imgui_popup_open,
                    &input_actions)) {
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&event);

            // Dragged to a monitor with a different DPI: rescale the font and
            // the style so the UI keeps its physical size (see ui/scale.h).
            // Safe here — events are pumped before ImGui::NewFrame(), so the
            // font atlas is never rebuilt mid-frame.
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED &&
                event.window.windowID == SDL_GetWindowID(window)) {
                cosmic::ui::apply(window);
            }

            // Closing the window (X button or SDL_EVENT_QUIT) means "get out
            // of the way", not "stop hosting" — that is what the tray Quit
            // item is for.
            const bool close_requested =
                event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(window));
            if (close_requested) {
                if (has_tray) {
                    // Hiding to the tray mid-stream leaves the Viewing UI:
                    // release the grab, flush held input, restore the hints,
                    // and tear down the video renderer so the host does not
                    // keep stuck keys. The session is also ended so the host is
                    // not left streaming with nothing shown.
                    if (mode == cosmic::AppMode::Viewing ||
                        g_session->status().state ==
                            cosmic::viewer::ViewerState::Streaming) {
                        leave_viewing_ui(window, &input_grabbed,
                                         &vrenderer_active);
                        g_session->end_session();
                        // PLAN.md D10(e): second stream-end seam (hide to
                        // tray mid-stream) alongside the Viewing-exit
                        // transition below — stop the steady selected-card
                        // backdrop weight so it does not come back on Show.
                        bridge_state.backdrop_selection_muted = true;
                    }
                    mode = cosmic::AppMode::HiddenToTray;
                    SDL_HideWindow(window);
                } else {
                    running = false;
                }
            } else if (mode == cosmic::AppMode::Viewing) {
                // Keyboard-grab lifecycle (plan M4.3): SDL releases the grab
                // implicitly on focus loss, so re-apply it on focus gain and
                // flush held keys on focus loss so the host does not keep
                // stuck keys (moonlight-qt notifyFocusLost pattern). The
                // flush is unconditional: keys can be held even when the grab
                // is off (the user may have toggled it), and their key-ups
                // would otherwise never reach the host.
                if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
                    input_grabbed) {
                    apply_input_grab(window, true);
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    cosmic::viewer::input::flush_input_state();
                }
            }
        }

        if (has_tray && !cosmic::ui::tray_pump()) {
            running = false;
        }
        // A second launch signaled us to show the window (plan M8.1): leave
        // the tray and raise the window, same as the tray Show item.
        if (cosmic::single_instance::poll_show_request()) {
            if (mode == cosmic::AppMode::HiddenToTray) {
                replay_boot_splash = true;
            }
            mode = cosmic::AppMode::MainWindow;
            SDL_ShowWindow(window);
            SDL_RaiseWindow(window);
        }
        if (!running) {
            break;
        }

        // Surface pending pairing requests from the host thread (plan M1.4).
        // The vendored tray library cannot raise a notification, so the window
        // itself is shown and raised instead.
        std::string polled_client;
        while (cosmic::pin_bridge::poll(polled_client)) {
            g_pending_client = std::move(polled_client);
            show_pin_dialog = true;
            pin_result_ok = false;
            if (mode == cosmic::AppMode::HiddenToTray) {
                replay_boot_splash = true;
            }
            mode = cosmic::AppMode::MainWindow;
            SDL_ShowWindow(window);
            SDL_RaiseWindow(window);
        }

        // Expire any GET /cosmic/clipboard?wait=1 request parked past its
        // hold time (no timer/thread backs this -- the main loop drives it).
        cosmic::clipboard::tick();

        // Clipboard-in: drain both inbound paths every frame regardless of
        // which one is realistically active on this machine (viewing ->
        // clipsync from the host; hosting -> clipboard.h from a connected
        // client) so neither buffer is left to stagnate. At most one clipboard
        // write happens per frame: the last non-empty enabled result wins.
        {
            // cosmic::clipboard::enabled() is the live mirror of
            // settings.share_clipboard and is the single per-machine toggle
            // that deliberately governs both the host role and the viewer
            // role -- so a drained value is discarded, not applied, while
            // sharing is off.
            const bool sharing_enabled = cosmic::clipboard::enabled();
            std::string incoming;
            // Tracked in cosmic::clipboard::Mime terms regardless of which
            // side the value was drained from, so the apply logic below has
            // one mime type to switch on.
            cosmic::clipboard::Mime incoming_mime = cosmic::clipboard::Mime::Text;
            bool have_incoming = false;
            std::string from_host;
            cosmic::clipsync::Mime from_host_mime = cosmic::clipsync::Mime::Text;
            if (cosmic::clipsync::take_incoming(from_host, from_host_mime) &&
                sharing_enabled && !from_host.empty()) {
                incoming = std::move(from_host);
                incoming_mime = from_host_mime == cosmic::clipsync::Mime::Png
                                    ? cosmic::clipboard::Mime::Png
                                    : cosmic::clipboard::Mime::Text;
                have_incoming = true;
            }
            // The POST route already refuses to fill this buffer while
            // disabled, but gate defensively here too: a toggle flip between
            // the POST and this drain would otherwise let one stale value
            // through. Drained unconditionally either way so it cannot
            // stagnate. Also requires non-empty: a paired client can POST an
            // empty body (host/sunshine's route only checks the upper
            // bound), and an empty from_client here must not overwrite a
            // valid from_host value already drained above -- once drained,
            // the worker's slot is gone and the value would be lost for good.
            std::string from_client;
            cosmic::clipboard::Mime from_client_mime = cosmic::clipboard::Mime::Text;
            if (cosmic::clipboard::take_incoming(from_client, from_client_mime) &&
                sharing_enabled && !from_client.empty()) {
                incoming = std::move(from_client);
                incoming_mime = from_client_mime;
                have_incoming = true;
            }
            if (have_incoming && !incoming.empty() &&
                incoming_mime == cosmic::clipboard::Mime::Png) {
                // Image apply: transcode to BMP and, on success, offer BOTH
                // representations, fail closed on transcode failure (leave
                // the clipboard untouched rather than apply anything
                // partial/garbage).
                std::string bmp;
                if (cosmic::clipimage::png_to_bmp(incoming.data(), incoming.size(), bmp)) {
                    // BMP must be listed first: Windows' WIN_SetClipboardData
                    // (third-party/SDL/src/video/windows/SDL_windowsclipboard.c)
                    // sets only the first image mime type it recognises in
                    // this list, and CF_DIB (from image/bmp) is what pastes
                    // universally there; X11 advertises both regardless of
                    // order.
                    static const char* const kImageMimeTypes[] = {"image/bmp", "image/png"};
                    // Heap-allocated and hand it to SDL as userdata: every
                    // path SDL_SetClipboardData can take from this call site
                    // transfers ownership to SDL, success or failure alike
                    // (see ClipboardImagePayload's comment for why the two
                    // paths that would leave it unowned cannot be taken
                    // here) -- this call site must never delete it itself.
                    auto* payload = new ClipboardImagePayload{std::move(bmp), std::move(incoming)};
                    const bool applied =
                        SDL_SetClipboardData(clipboard_image_data, clipboard_image_cleanup, payload,
                                              kImageMimeTypes, SDL_arraysize(kImageMimeTypes));
                    // Whether SDL_SetClipboardData succeeded or not, payload
                    // is now SDL's to free -- never delete it here.
                    // The clipboard no longer holds the text this hash
                    // described (if any), success or failure: a failed
                    // SDL_SetClipboardData has still called
                    // SDL_CancelClipboardData and let the backend clear the
                    // real clipboard before it could fail, so the previously
                    // recorded text is gone either way. Reset unconditionally
                    // so a later genuine local copy of that same text is not
                    // mistaken for an echo of the stale hash and silently
                    // swallowed.
                    clipboard_applied_hash = 0;
#ifdef _WIN32
                    if (applied) {
                        // SDL 3.4 writes to the clipboard synchronously, so
                        // the sequence number has already advanced by the
                        // time SDL_SetClipboardData returns. Stamp
                        // image_applied_seq with it so the
                        // SDL_EVENT_CLIPBOARD_UPDATE handler recognizes the
                        // real event this call queues (or any duplicate the
                        // poll above lets through) as this apply's own echo.
                        //
                        // Deliberately NOT also stamping last_clip_seq (an
                        // earlier version of this did, as a "belt and braces"
                        // pair): image_applied_seq alone already fully
                        // suppresses the echo in the handler below, so
                        // touching last_clip_seq buys nothing, and it is
                        // actively harmful -- in the race where some other
                        // process copies something onto the clipboard in the
                        // very same tick as this apply, latching last_clip_seq
                        // to this write's sequence number would make the poll
                        // above treat that other, genuine external change as
                        // already seen and silently drop it.
                        image_applied_seq = GetClipboardSequenceNumber();
                    }
#endif
                }
            } else if (have_incoming && !incoming.empty()) {
                // SDL_SetClipboardText takes a C string, so an embedded NUL
                // would truncate what it actually writes. Truncate here,
                // before hashing, so the hash describes what the clipboard
                // will really hold -- hashing the untruncated text would
                // make the synchronous echo (see below) look like a fresh
                // local copy instead of this write, and the truncated text
                // would be re-published to the peer, overwriting its
                // clipboard.
                if (auto n = incoming.find('\0'); n != std::string::npos) {
                    incoming.resize(n);
                }
                const uint64_t hash = Fnv1a64(incoming);
                if (hash != clipboard_applied_hash) {
                    // Store the hash BEFORE writing the clipboard, updating
                    // the same "clipboard's last known content" record the
                    // out-handler above maintains: SDL_SetClipboardText can
                    // synchronously post SDL_EVENT_CLIPBOARD_UPDATE, and that
                    // handler compares against this hash to recognize its own
                    // echo and suppress it. Storing after would let the echo
                    // through and bounce the text straight back to the peer.
                    const uint64_t prev_hash = clipboard_applied_hash;
                    clipboard_applied_hash = hash;
                    if (!SDL_SetClipboardText(incoming.c_str())) {
                        // Write failed (e.g. another process holds the
                        // clipboard open): the value is already drained and
                        // gone, but the hash must not claim the clipboard
                        // holds it, or a later resend of the same text would
                        // be suppressed as an echo.
                        clipboard_applied_hash = prev_hash;
                    }
                }
            }
        }

        // Drive the window mode from the viewer session (plan M2): while
        // streaming, the window shows the viewer placeholder instead of the
        // main UI. Hiding to the tray still wins so tray behavior is kept.
        cosmic::viewer::SessionStatus session_status = g_session->status();
        // Clipsync lifecycle: driven by the session state directly rather
        // than the AppMode::Viewing transition, which is a deliberate
        // deviation from mirroring share_wallpaper's mode-based seam --
        // AppMode::Viewing only becomes true once the warp finishes and is
        // also left early when hiding to the tray mid-stream (see the
        // close_requested handling above), so a state-machine flag is the one
        // piece of code that covers every path in and out of Streaming.
        // Also gated on cosmic::clipboard::enabled(): it is the live mirror
        // of settings.share_clipboard and, per the single-toggle contract,
        // deliberately governs the viewer role here as well as the host
        // role inside the /cosmic/clipboard routes -- so the worker neither
        // starts nor keeps running (issuing a poll every second) while
        // sharing is off, and starts/stops live as the user flips the
        // toggle mid-session.
        const bool clipboard_sharing_enabled = cosmic::clipboard::enabled();
        if (session_status.state == cosmic::viewer::ViewerState::Streaming &&
            clipboard_sharing_enabled && !clipsync_running) {
            const int https_port = session_status.port_used - 5;
            if (!connecting_address.empty() && https_port > 0) {
                cosmic::clipsync::start(connecting_address, https_port);
                clipsync_running = true;
            }
        } else if (clipsync_running &&
                   (session_status.state != cosmic::viewer::ViewerState::Streaming ||
                    !clipboard_sharing_enabled)) {
            cosmic::clipsync::stop();
            clipsync_running = false;
        }
        // Once the session settles (Idle/Failed), the LINKING... card clears:
        // the connect either finished (Streaming keeps it) or gave up.
        if (session_status.state == cosmic::viewer::ViewerState::Idle ||
            session_status.state == cosmic::viewer::ViewerState::Failed) {
            connecting_address.clear();
            // U5: an ended/failed session warps the scene back to the Bridge.
            // Idempotent — the target is already 0 when no session is active.
            cosmic::ui::scene::set_warp_target(0.0f);
        }
        // Build the Bridge Pair modal's live feedback from the session status,
        // and resolve the pair latch. This is only correct because begin_worker
        // publishes Connecting synchronously, so the latch never sees a stale
        // previous terminal state on the frame after Pair is clicked.
        cosmic::ui::PairProgress pairing;
        if (pair_in_flight) {
            switch (session_status.state) {
            case cosmic::viewer::ViewerState::Connecting:
            case cosmic::viewer::ViewerState::PairingNeedPin:
            case cosmic::viewer::ViewerState::PairingInProgress:
                pairing.active = true;
                pairing.show_pin =
                    session_status.state != cosmic::viewer::ViewerState::Connecting;
                pairing.pin = session_status.pin;
                pairing.message = session_status.message;
                break;
            case cosmic::viewer::ViewerState::Failed:
                // Clear the latch and make the error sticky. Nothing to undo:
                // the pair worker only records a machine once the handshake has
                // actually succeeded, so a failed pair leaves the list untouched
                // (and an existing entry keeps its nickname and port override).
                pair_in_flight = false;
                pair_error = session_status.message;
                break;
            case cosmic::viewer::ViewerState::Idle:
                // Handshake succeeded: the worker has added the machine with
                // paired=true, so apply the port override the user chose and
                // close the Bridge's Pair modal.
                pair_in_flight = false;
                bridge_state.pair_modal_open = false;  // the Bridge's own modal
                if (!pair_address.empty()) {
                    settings.set_host_port(pair_address, pair_port);
                    if (!pair_nickname.empty()) {
                        settings.set_host_nickname(pair_address, pair_nickname);
                    }
                }
                break;
            case cosmic::viewer::ViewerState::Streaming:
                break;  // Unreachable while pairing; pair-while-streaming is
                        // excluded by busy() and AppMode::Viewing.
            }
        }
        // U5: enter Viewing only once the warp has carried the scene out (warp
        // progress >= 0.95). While Streaming with the warp still rising, stay
        // in MainWindow — the scene keeps warping and the bridge keeps drawing.
        if (session_status.state == cosmic::viewer::ViewerState::Streaming &&
            cosmic::ui::scene::warp_progress() >= 0.95f) {
            if (mode != cosmic::AppMode::HiddenToTray) {
                if (mode != cosmic::AppMode::Viewing) {
                    // Entering Viewing: keep the fullscreen video visible when
                    // the app loses focus (moonlight-qt pattern) — pinning
                    // the hint to "0" guarantees a focus loss mid-stream
                    // never minimizes the window, regardless of the hint's
                    // prior value. Restored to "1" when leaving Viewing
                    // below.
                    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
                    // Grab the keyboard so Alt+Tab / Win act on the remote
                    // machine (plan M4.3).
                    input_grabbed = true;
                    apply_input_grab(window, true);
                }
                mode = cosmic::AppMode::Viewing;
            }
        } else if (mode == cosmic::AppMode::Viewing) {
            mode = cosmic::AppMode::MainWindow;
            // Leaving Viewing: flush held input, release the grab, restore
            // the hints, and tear down the video renderer.
            leave_viewing_ui(window, &input_grabbed, &vrenderer_active);
            SDL_ShowWindow(window);
            // U5: warp back to the Bridge — the scene reassembles while the
            // bridge shows.
            cosmic::ui::scene::set_warp_target(0.0f);
            // PLAN.md D10(e): stop the steady selected-card backdrop weight
            // now that the stream has ended, so it fades out with the scene.
            bridge_state.backdrop_selection_muted = true;
        }

        if (mode == cosmic::AppMode::HiddenToTray) {
            // Nothing to draw; stay responsive to tray clicks without burning a core.
            SDL_Delay(50);
            continue;
        }

        // While minimized, SDL3's ImGui backend reports a 0x0 DisplaySize, and
        // the Bridge's layout math (CardOrbitCenter's viewport clamps,
        // bridge.cpp:254) must never run against a zero viewport — the bounds
        // invert and Debug builds abort on the libstdc++ clamp assertion.
        // Skip the frame; SDL_GetWindowFlags is polled fresh each iteration,
        // so drawing resumes on its own once the window is restored.
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_Delay(50);
            continue;
        }

        if (mode == cosmic::AppMode::Viewing) {
            // Lazy renderer init once the negotiated stream dimensions are
            // known (the video setup callback stores them in SessionStatus).
            if (!vrenderer_active && session_status.stream_width > 0 &&
                session_status.stream_height > 0) {
                // Input forwarding needs the negotiated geometry for the
                // window->stream mouse coordinate mapping (plan M2.6).
                cosmic::viewer::input::init(session_status.stream_width,
                                            session_status.stream_height);
                if (cosmic::viewer::vrenderer_init(
                        renderer, session_status.stream_width,
                        session_status.stream_height) == 0) {
                    vrenderer_active = true;
                }
            }

            // Video area: the whole renderer output minus the top bar strip,
            // which now owns its own band instead of overlaying the stream.
            // The strip height is derived from the scaled ImGui style, so it
            // tracks DPI changes; the renderer output is in the same pixel
            // space the ImGui viewport uses (per-monitor-v2 DPI awareness).
            int out_w = 0;
            int out_h = 0;
            SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
            const float bar_h = cosmic::ui::topbar_height();
            const int strip_h = std::min(static_cast<int>(bar_h), out_h);
            const SDL_Rect video_area{0, strip_h, out_w, out_h - strip_h};
            cosmic::viewer::input::set_topbar_height(bar_h);

            // Latest-frame exchange (plan D2): grab the newest decoded frame
            // without blocking, upload it, and hand it back to the decoder.
            AVFrame* frame =
                vrenderer_active ? cosmic::viewer::decoder_acquire_frame() : nullptr;
            if (frame != nullptr) {
                cosmic::viewer::vrenderer_render(renderer, frame, video_area);
                cosmic::viewer::decoder_release_frame(frame);
            } else {
                cosmic::viewer::vrenderer_present_no_frame(renderer, video_area);
            }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            // Top bar (plan M4.1): drawn after the video so it sits above it,
            // before the centered placeholder overlay. The returned action is
            // applied after the ImGui frame below — SDL_SetWindowFullscreen
            // and end_session() must not run mid-frame. M5: the monitor
            // dropdown is fed from the session's display snapshot.
            const std::vector<cosmic::ui::MonitorInfo> monitors =
                to_monitor_info(session_status.displays);
            const cosmic::ui::TopBarAction topbar_action =
                cosmic::ui::draw_topbar(&topbar_state, viewer_fullscreen,
                                        monitors, session_status.active_display);

            // No centred overlay: it sat on top of the remote desktop for the
            // whole session and swallowed mouse input wherever it covered
            // (WantCaptureMouse gates forwarding). Ending the session lives on
            // the top bar's Exit button and on Ctrl+Alt+Shift+Q.

            // Hide the local pointer over the stream: the host composites its
            // own cursor into the video, so drawing ours too shows two. This
            // must go through ImGui, not SDL_HideCursor() directly --
            // ImGui_ImplSDL3_NewFrame() calls SDL_ShowCursor()/SDL_HideCursor()
            // every frame based on ImGui's own cursor, so a direct hide is
            // undone before the next frame is drawn. Keep the pointer while
            // the top bar is up (its buttons have to be aimed at) and while
            // the window is unfocused, so it is never lost on the way out.
            const bool focused =
                (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
            if (focused && !topbar_state.visible) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            }

            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);

            // Apply top-bar and escape-combo actions after the ImGui frame:
            // toggling fullscreen resizes the window, ending the session
            // tears down the stream, and toggling the grab changes SDL input
            // state, all unsafe mid-frame.
            if (input_actions.fullscreen ||
                topbar_action.kind == cosmic::ui::TopBarAction::ToggleFullscreen) {
                viewer_fullscreen = !viewer_fullscreen;
                SDL_SetWindowFullscreen(window, viewer_fullscreen);
            }
            if (input_actions.quit ||
                topbar_action.kind == cosmic::ui::TopBarAction::Exit) {
                g_session->end_session();
            }
            if (input_actions.toggle_grab) {
                input_grabbed = !input_grabbed;
                if (input_grabbed) {
                    apply_input_grab(window, true);
                } else {
                    // Release anything still held before ungrab so the host
                    // does not keep stuck keys (moonlight-qt
                    // KeyComboUngrabInput pattern).
                    cosmic::viewer::input::flush_input_state();
                    apply_input_grab(window, false);
                }
            }
            // COSMIC MODIFICATION (M5): monitor dropdown actions. Opening the
            // combo re-fetches /serverinfo (hotplug); selecting a different
            // monitor synthesizes Ctrl+Alt+Shift+F(1+i) on the host and marks
            // it active locally so the dropdown's [active] marker stays in
            // sync without another round-trip.
            if (topbar_action.kind == cosmic::ui::TopBarAction::RefreshDisplays) {
                g_session->refresh_displays();
            }
            if (topbar_action.kind == cosmic::ui::TopBarAction::SwitchMonitor) {
                cosmic::viewer::input::send_monitor_switch(
                    topbar_action.monitor_index);
                g_session->set_active_display(topbar_action.monitor_index);
            }
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Bridge overlay (docs/UI_MIGRATION.md U2-U3): fullscreen window with
        // the in-scene monitor UI. The only UI in MainWindow mode; the host-side
        // PIN dialog is drawn after it so it stays on top.
        cosmic::ui::bridge::BridgeInput bridge_input;
        bridge_input.hosting_ok = hosting_ok;
        bridge_input.port_base = settings.port_base;
        bridge_input.resolution_mode = settings.resolution_mode;
        bridge_input.fps = settings.fps;
        bridge_input.bitrate_kbps = settings.bitrate_kbps;
        bridge_input.autostart = settings.autostart;
        bridge_input.share_wallpaper = settings.share_wallpaper;
        bridge_input.share_clipboard = settings.share_clipboard;
        bridge_input.service_mode = service_mode;
        bridge_input.paired_count = cosmic::hostglue::paired_client_count();
        bridge_input.time_s = static_cast<double>(SDL_GetTicks()) / 1000.0;
        bridge_input.hosts = settings.hosts_snapshot();
        // U6: keep the presence poller's target set in sync with the current
        // hosts (address + resolved port). Only call start() when the set
        // actually changed — pair success adds a host, Edit/Remove change
        // entries — so the worker's poll set stays current without a per-frame
        // call (start() just swaps targets when the worker is already alive).
        {
            std::vector<std::pair<std::string, int>> poll_hosts;
            poll_hosts.reserve(bridge_input.hosts.size());
            for (const cosmic::SavedHost& host : bridge_input.hosts) {
                poll_hosts.emplace_back(host.address, settings.port_for(host.address));
            }
            if (poll_hosts != last_poll_hosts) {
                cosmic::presence::start(poll_hosts);
                last_poll_hosts = std::move(poll_hosts);
            }
        }
        // The Bridge only needs reachability today; the wallpaper hash in the
        // snapshot feeds the backdrop memo in the SceneInput fill below.
        const std::map<std::string, cosmic::presence::HostPresence> presence_snapshot =
            cosmic::presence::snapshot();
        for (const auto& [address, presence] : presence_snapshot) {
            bridge_input.presence[address] = presence.reachable;
        }
        bridge_input.warp = cosmic::ui::scene::warp_progress();
        switch (session_status.state) {
        case cosmic::viewer::ViewerState::Idle:
        case cosmic::viewer::ViewerState::Failed:
            bridge_input.session_label = "IDLE";
            break;
        case cosmic::viewer::ViewerState::PairingNeedPin:
        case cosmic::viewer::ViewerState::PairingInProgress:
            bridge_input.session_label = "PAIRING";
            break;
        case cosmic::viewer::ViewerState::Connecting:
            bridge_input.session_label = "CONNECTING";
            break;
        case cosmic::viewer::ViewerState::Streaming: {
            // U5: "SESSION · STREAMING · <NICKNAME>" when the connected host
            // has a nickname (nicknames are stored uppercase).
            std::string label = "STREAMING";
            if (!connecting_address.empty()) {
                for (const cosmic::SavedHost& host : bridge_input.hosts) {
                    if (host.address == connecting_address &&
                        !host.nickname.empty()) {
                        label += " · " + host.nickname;
                        break;
                    }
                }
            }
            bridge_input.session_label = label;
            break;
        }
        }
        bridge_input.session_busy = g_session->busy();
        bridge_input.connected_or_connecting =
            session_status.state == cosmic::viewer::ViewerState::Streaming ||
            session_status.state == cosmic::viewer::ViewerState::Connecting;
        bridge_input.connecting_address = connecting_address;
        // Pairing feedback (U4): the in-scene PIN panel and the Pair modal's
        // pairing state come from the pair latch, exactly as before. The
        // auto-pair PIN on the Connect path is produced by the session worker
        // itself (ViewerState PairingNeedPin/PairingInProgress with
        // session_status.pin) WITHOUT the latch, so feed the bridge's pairing
        // fields from BOTH sources.
        const bool auto_pair_pin =
            (session_status.state == cosmic::viewer::ViewerState::PairingNeedPin ||
             session_status.state == cosmic::viewer::ViewerState::PairingInProgress) &&
            !session_status.pin.empty() && !pair_in_flight;
        bridge_input.pairing_active =
            (pair_in_flight && pairing.active) || auto_pair_pin;
        bridge_input.pairing_show_pin =
            (pair_in_flight && pairing.show_pin) || auto_pair_pin;
        bridge_input.pairing_pin =
            auto_pair_pin ? session_status.pin : pairing.pin;
        // Only the explicit latch path has a sticky error.
        bridge_input.pairing_error = pair_error;
        // Unhidden from the tray this frame: replay the monitor logo splash.
        if (replay_boot_splash) {
            bridge_state.boot_start_s = -1.0;
            replay_boot_splash = false;
        }
        const cosmic::ui::bridge::BridgeDrawResult bridge_result =
            cosmic::ui::bridge::draw_bridge(bridge_input, &bridge_state);

        if (show_pin_dialog) {
            cosmic::ui::draw_pin_dialog(g_pending_client, show_pin_dialog, pin_result_ok);
            // When pin_result_ok is set, nvhttp::pin() completed the handshake
            // server-side and the client is paired.
        }

        ImGui::Render();
        // Clear to the design's deep indigo (#101226) and draw the parallax
        // scene underneath the ImGui UI (docs/UI_MIGRATION.md U0). The scene
        // draws in renderer-output pixels; out_w/out_h are in the same space
        // the ImGui viewport uses (per-monitor-v2 DPI awareness).
        const SDL_Color clear_color = cosmic::ui::SdlColor(cosmic::ui::kBg);
        SDL_SetRenderDrawColor(renderer, clear_color.r, clear_color.g,
                               clear_color.b, clear_color.a);
        SDL_RenderClear(renderer);
        int out_w = 0;
        int out_h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
        if (out_w > 0 && out_h > 0) {
            cosmic::ui::scene::SceneInput scene_input;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            scene_input.mouse_x = mouse.x;
            scene_input.mouse_y = mouse.y;
            scene_input.time_s = static_cast<float>(SDL_GetTicks()) / 1000.0f;
            scene_input.motion = 1.0f;
            scene_input.screen_logo_alpha = bridge_result.screen_logo_alpha;
            // PLAN.md D10(e): resolve the focused host's cached wallpaper into
            // the scene backdrop. path_for is memoized (backdrop_memo) to keep
            // it off the per-frame path, and refreshed at 1 Hz because the
            // advertised hash changes one poll pass BEFORE wallcache::sync
            // finishes downloading the new file — so both "no file yet" and
            // "still the previous file" resolve themselves within a second.
            scene_input.backdrop_alpha = bridge_result.backdrop_weight;
            if (!bridge_result.backdrop_address.empty()) {
                std::string hash;
                const auto presence_it = presence_snapshot.find(bridge_result.backdrop_address);
                if (presence_it != presence_snapshot.end()) {
                    hash = presence_it->second.wallpaper_hash;
                }
                const uint64_t now_ms = SDL_GetTicks();
                const bool stale =
                    now_ms - backdrop_memo.last_query_ms >= 1000;
                if (bridge_result.backdrop_address != backdrop_memo.address ||
                    hash != backdrop_memo.hash || stale) {
                    backdrop_memo.address = bridge_result.backdrop_address;
                    backdrop_memo.hash = hash;
                    backdrop_memo.path = cosmic::wallcache::path_for(backdrop_memo.address);
                    backdrop_memo.last_query_ms = now_ms;
                }
                scene_input.backdrop_path = backdrop_memo.path.string();
            }
            cosmic::ui::scene::draw(renderer, out_w, out_h, scene_input);
        }
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        // Apply Bridge actions after the frame (docs/UI_MIGRATION.md U3): the
        // main-window branch falls off the end of the loop body, so this runs
        // only for MainWindow mode. g_session->busy() was read mid-frame and
        // can flip immediately after; worst case a button is enabled for one
        // frame whose call then no-ops. Benign.
        if (bridge_result.action.kind == cosmic::ui::bridge::BridgeAction::Connect) {
            connecting_address = bridge_result.action.address;
            // U5: warp out to the stream — the sky zooms/fades and the cards
            // exit while the session connects; Viewing starts once the warp
            // reaches 0.95.
            cosmic::ui::scene::set_warp_target(1.0f);
            cosmic::ui::scene::trigger_warp_flash();
            g_session->start_connect(bridge_result.action.address,
                                     settings.port_for(bridge_result.action.address));
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::StartPair) {
            // Same latch discipline as the Pair modal: nothing is persisted
            // until the handshake succeeds (see pair_port above).
            pair_in_flight = true;
            pair_address = bridge_result.action.address;
            pair_port = bridge_result.action.port;
            pair_nickname = bridge_result.action.nickname;
            pair_error.clear();
            g_session->start_pair(
                bridge_result.action.address,
                bridge_result.action.port > 0 ? bridge_result.action.port
                                              : settings.port_base);
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::CancelPair) {
            // The worker may stay parked in gs_pair for minutes; the modal
            // keeps showing the handshake line until it exits.
            g_session->end_session();
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::ClosePair) {
            bridge_state.pair_modal_open = false;
            pair_error.clear();  // Reopening the modal starts clean.
        } else if (bridge_result.action.kind == cosmic::ui::bridge::BridgeAction::Edit) {
            settings.set_host_nickname(bridge_result.action.address,
                                       bridge_result.action.nickname);
        } else if (bridge_result.action.kind == cosmic::ui::bridge::BridgeAction::Remove) {
            // No settings_dirty here: remove_host() persists itself and drops
            // the wallpaper cache (settings.cpp:296-312), same contract as
            // set_host_nickname() above. The presence poller picks the change
            // up from the next frame's hosts_snapshot() (main.cpp:880-896).
            settings.remove_host(bridge_result.action.address);
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetResolution) {
            settings.resolution_mode = bridge_result.action.resolution;
            settings_dirty = true;
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetFps) {
            // The panel already clamps to 10-240 in steps of 10.
            settings.fps = bridge_result.action.value;
            settings_dirty = true;
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetBitrate) {
            settings.bitrate_kbps = bridge_result.action.value;
            settings_dirty = true;
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetPortBase) {
            settings.port_base = bridge_result.action.value;
            settings_dirty = true;
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetAutostart) {
            settings.autostart = bridge_result.action.on;
            settings_dirty = true;
            if (!cosmic::autostart::set_enabled(bridge_result.action.on)) {
                std::fprintf(stderr, "Failed to update autostart (see log).\n");
            }
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetShareWallpaper) {
            settings.share_wallpaper = bridge_result.action.on;
            settings_dirty = true;
            // Apply immediately so the running host's /serverinfo and
            // /cosmic/wallpaper handlers honor the change without a restart
            // (PLAN.md D10 / W1.3's "on settings change" seam).
            cosmic::wallpaper::set_enabled(bridge_result.action.on);
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::SetShareClipboard) {
            settings.share_clipboard = bridge_result.action.on;
            settings_dirty = true;
            // Apply immediately so the running host's /cosmic/clipboard route
            // honors the change without a restart (mirrors SetShareWallpaper).
            cosmic::clipboard::set_enabled(bridge_result.action.on);
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::CloseSettings) {
            bridge_state.settings_open = false;
            // Settings edits save on the close transition (docs/UI_MIGRATION.md
            // U4): save() is a full-file rewrite, so steppers/sliders must not
            // hit disk per tick. The shutdown save below is the fallback.
            if (settings_dirty) {
                settings.save();
                settings_dirty = false;
            }
        } else if (bridge_result.action.kind ==
                   cosmic::ui::bridge::BridgeAction::Disconnect) {
            g_session->end_session();
        }
    }

    settings.save();

    // Stop the viewer session before tearing down SDL (plan M2.7): unblocks
    // the worker thread; the Session destructor joins it.
    g_session->end_session();
    // Destroy the session (and join its worker) here, while `settings` is still
    // alive: the worker calls settings_.add_or_update_host(), and g_session is a
    // namespace-scope static whose destructor would otherwise run at static
    // destruction, after main()'s local `settings` is already gone.
    g_session.reset();

    cosmic::ui::tray_stop();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    cosmic::viewer::vrenderer_deinit();
    // Tear down the scene's layer textures before the renderer goes away.
    cosmic::ui::scene::shutdown(renderer);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    cosmic::hostglue::stop();
    // Stop the presence poller (U6) and join its worker before SDL_Quit.
    cosmic::presence::stop();
    // Stop the clipsync worker (idempotent; safe if never started) so it
    // cannot outlive SDL.
    cosmic::clipsync::stop();
    cosmic::single_instance::release();
    SDL_Quit();

#ifdef _WIN32
    // Quitting from the tray while service-spawned: exit 1115 so cosmicsvc stops
    // instead of respawning us (PLAN.md M8.2; upstream pattern in Sunshine's
    // system_tray.cpp tray_quit_cb). Upstream detects service mode via
    // GetConsoleWindow()==nullptr, but our GUI build never has a console, so the
    // explicit --service flag is the equivalent.
    if (tray_quit_requested && service_mode) { return ERROR_SHUTDOWN_IN_PROGRESS; }
#endif
    return 0;
}
