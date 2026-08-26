// Cosmic Desk — one binary that is both the host (screen sharer) and the
// viewer (client). See PLAN.md for the architecture and milestone breakdown.
//
// M0 scope: SDL window + Dear ImGui + tray icon + settings file. The host
// threads (M1) and the viewer session (M2) plug into this loop later.

#include "app/settings.h"
#include "app/state.h"
#include "hostglue/host.h"
#include "hostglue/pin_bridge.h"
#include "ui/pin_dialog.h"
#include "ui/scale.h"
#include "ui/settings_window.h"
#include "ui/tray.h"
#include "ui/viewer_topbar.h"
#include "viewer/decoder.h"
#include "viewer/input.h"
#include "viewer/session.h"
#include "viewer/vrenderer.h"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cstdio>
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

constexpr int kWindowWidth = 1000;
constexpr int kWindowHeight = 640;

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
    if (char* base = SDL_GetBasePath()) {
        path = base;
        SDL_free(base);
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
// and the Win key act on the remote machine; SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4
// keeps Alt+F4 from closing the window (Windows-only hint, harmless elsewhere).
void apply_input_grab(SDL_Window* window, bool grabbed) {
    SDL_SetWindowKeyboardGrab(window, grabbed ? SDL_TRUE : SDL_FALSE);
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, grabbed ? "1" : "0");
#endif
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
    SDL_SetWindowFullscreen(window, 0);
    // Release anything still held, then drop the grab so the host does not
    // keep stuck keys (moonlight-qt raiseAllKeys pattern). Flushing is
    // independent of the grab: keys can be held even when the grab is off.
    cosmic::viewer::input::flush_input_state();
    if (*input_grabbed) {
        apply_input_grab(window, false);
        *input_grabbed = false;
    }
    // Restore the hints Viewing changed: minimize-on-focus-loss back to the
    // default "1", and the Alt+F4 close hint back to "0" (apply_input_grab
    // already reset the latter when releasing the grab, but restore it
    // explicitly in case the grab was already off).
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "1");
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "0");
#endif
    if (*vrenderer_active) {
        cosmic::viewer::vrenderer_deinit();
        *vrenderer_active = false;
    }
}

// Viewer session (plan M2.2): one session object for the whole app lifetime;
// its worker thread does all networking so the main loop never blocks. Created
// in main() once the Settings object exists (the session needs it to record
// recent hosts).
std::unique_ptr<cosmic::viewer::Session> g_session;
// ASCII-only host IP input: the default ImGui font has no other glyphs, and
// the vendored imgui has no std::string InputText overload (imgui_stdlib.h is
// not vendored), so this is a fixed buffer.
char g_host_ip_input[64] = {};

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
    // --connect <ip> starts a viewer session as soon as the app is up, without
    // going through the window. Exists for the two-machine interop matrix
    // (PLAN.md S9), which otherwise cannot be driven from a script.
    std::string autoconnect_ip;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hidden") {
            start_hidden = true;
        } else if (arg == "--connect" && i + 1 < argc) {
            autoconnect_ip = argv[++i];
        }
    }

    // Run from the executable's directory so the vendored host's CWD-relative
    // SUNSHINE_ASSETS_DIR="assets" resolves regardless of how we were launched.
    chdir_to_executable_dir();

#ifdef _WIN32
    // Per-monitor-v2 DPI awareness: crisp rendering and correct scaling on
    // HiDPI/mixed-DPI setups. Must be set before SDL_Init creates the video
    // driver. SDL_WINDOW_ALLOW_HIGHDPI + the ImGui backends then report window
    // sizes and mouse positions in the same (DPI-scaled) coordinate space, so
    // the UI, the top bar, and the viewer's mouse mapping all line up.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Cosmic Desk", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowWidth,
        kWindowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    // HiDPI (see ui/scale.h): the DPI-awareness hint above gives us the raw
    // pixel grid, so ImGui has to be told the display scale or everything it
    // draws comes out at 96-DPI sizes on a 4K panel. Must follow the backend
    // init: rebuilding the atlas drops the backend's font texture.
    cosmic::ui::apply(window);
    // The window was created at 96-DPI sizes; grow it to match, but never past
    // the display's usable area (a 2.25x scale would otherwise ask for a
    // 2250x1440 window on a 1920x1080 screen).
    {
        const float ui_scale = cosmic::ui::scale();
        SDL_Rect usable = {0, 0, 0, 0};
        const int display = SDL_GetWindowDisplayIndex(window);
        if (display < 0 || SDL_GetDisplayUsableBounds(display, &usable) != 0) {
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
    }

    cosmic::Settings settings = cosmic::Settings::load();
    if (!std::filesystem::exists(cosmic::Settings::config_file())) {
        // Materialize defaults on first run so the file is there to be edited.
        settings.save();
    }

    g_session = std::make_unique<cosmic::viewer::Session>(settings);

    const bool hosting_ok = cosmic::hostglue::start(settings);
    if (!hosting_ok) {
        // Hosting is degraded but the app keeps running (plan M1.4): the UI and
        // viewer role still work, and the host log explains what failed.
        std::fprintf(stderr, "Hosting failed to start; continuing without it.\n");
    }

    cosmic::AppMode mode = start_hidden ? cosmic::AppMode::HiddenToTray : cosmic::AppMode::MainWindow;
    bool running = true;

    const bool has_tray = cosmic::ui::tray_start(
        asset_path(tray_icon_name()),
        {
            [&] {
                mode = cosmic::AppMode::MainWindow;
                SDL_ShowWindow(window);
                SDL_RaiseWindow(window);
            },
            [&] { running = false; },
        });

    if (!has_tray) {
        // Without a tray there is nowhere to hide to, so never start hidden.
        std::fprintf(stderr, "Tray unavailable; keeping the window visible.\n");
        mode = cosmic::AppMode::MainWindow;
    }

    if (mode == cosmic::AppMode::MainWindow) {
        SDL_ShowWindow(window);
    }

    if (!autoconnect_ip.empty()) {
        std::snprintf(g_host_ip_input, sizeof(g_host_ip_input), "%s",
                      autoconnect_ip.c_str());
        g_session->start_connect(autoconnect_ip, settings.port_base);
    }

    bool show_imgui_demo = false;
    bool show_settings = false;

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

    // Pending pairing state (plan M1.4): set when the host thread reports a
    // /pair request; consumed by the PIN dialog below.
    std::string g_pending_client;
    bool show_pin_dialog = false;
    bool pin_result_ok = false;

    while (running) {
        SDL_Event event;
        // Set by the Ctrl+Alt+Shift+Q/Enter/Z escape combos while streaming;
        // applied after the frame (fullscreen changes and session teardown
        // must not happen mid-ImGui-frame).
        cosmic::viewer::input::InputActions input_actions;
        while (SDL_PollEvent(&event)) {
            // While streaming, forward input to the host before ImGui sees it
            // (plan M2.6). Consumed events never reach ImGui. The overlay's
            // "End session" button still works because ImGui claims mouse
            // capture when the cursor is over it (WantCaptureMouse), which
            // gates forwarding; the Ctrl+Alt+Shift+Q/Enter/Z escape combos
            // stay active regardless of the capture state.
            if (mode == cosmic::AppMode::Viewing &&
                cosmic::viewer::input::handle_event(
                    event, window, ImGui::GetIO().WantCaptureKeyboard,
                    ImGui::GetIO().WantCaptureMouse, &input_actions)) {
                continue;
            }
            ImGui_ImplSDL2_ProcessEvent(&event);

            // Dragged to a monitor with a different DPI: rescale the font and
            // the style so the UI keeps its physical size (see ui/scale.h).
            // Safe here — events are pumped before ImGui::NewFrame(), so the
            // font atlas is never rebuilt mid-frame.
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED &&
                event.window.windowID == SDL_GetWindowID(window)) {
                cosmic::ui::apply(window);
            }

            // Closing the window (X button or SDL_QUIT) means "get out of the
            // way", not "stop hosting" — that is what the tray Quit item is
            // for.
            const bool close_requested =
                event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE &&
                 event.window.windowID == SDL_GetWindowID(window));
            if (close_requested) {
                if (has_tray) {
                    // Hiding to the tray mid-stream leaves the Viewing UI:
                    // release the grab, flush held input, restore the hints,
                    // and tear down the video renderer so the host does not
                    // keep stuck keys. The session keeps running in the
                    // background (close to tray = get out of the way).
                    if (mode == cosmic::AppMode::Viewing ||
                        g_session->status().state ==
                            cosmic::viewer::ViewerState::Streaming) {
                        leave_viewing_ui(window, &input_grabbed,
                                         &vrenderer_active);
                    }
                    mode = cosmic::AppMode::HiddenToTray;
                    SDL_HideWindow(window);
                } else {
                    running = false;
                }
            } else if (event.type == SDL_WINDOWEVENT &&
                       mode == cosmic::AppMode::Viewing) {
                // Keyboard-grab lifecycle (plan M4.3): SDL releases the grab
                // implicitly on focus loss, so re-apply it on focus gain and
                // flush held keys on focus loss so the host does not keep
                // stuck keys (moonlight-qt notifyFocusLost pattern). The
                // flush is unconditional: keys can be held even when the grab
                // is off (the user may have toggled it), and their key-ups
                // would otherwise never reach the host.
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
                    input_grabbed) {
                    apply_input_grab(window, true);
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    cosmic::viewer::input::flush_input_state();
                }
            }
        }

        if (has_tray && !cosmic::ui::tray_pump()) {
            running = false;
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
            mode = cosmic::AppMode::MainWindow;
            SDL_ShowWindow(window);
            SDL_RaiseWindow(window);
        }

        // Drive the window mode from the viewer session (plan M2): while
        // streaming, the window shows the viewer placeholder instead of the
        // main UI. Hiding to the tray still wins so tray behavior is kept.
        cosmic::viewer::SessionStatus session_status = g_session->status();
        if (session_status.state == cosmic::viewer::ViewerState::Streaming) {
            if (mode != cosmic::AppMode::HiddenToTray) {
                if (mode != cosmic::AppMode::Viewing) {
                    // Entering Viewing: keep the fullscreen video visible when
                    // the app loses focus (moonlight-qt pattern) — SDL would
                    // otherwise minimize the window mid-stream. Restored to
                    // the default "1" when leaving Viewing below.
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
        }

        if (mode == cosmic::AppMode::HiddenToTray) {
            // Nothing to draw; stay responsive to tray clicks without burning a core.
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

            // Latest-frame exchange (plan D2): grab the newest decoded frame
            // without blocking, upload it, and hand it back to the decoder.
            AVFrame* frame =
                vrenderer_active ? cosmic::viewer::decoder_acquire_frame() : nullptr;
            if (frame != nullptr) {
                cosmic::viewer::vrenderer_render(renderer, frame);
                cosmic::viewer::decoder_release_frame(frame);
            } else {
                cosmic::viewer::vrenderer_present_no_frame(renderer);
            }

            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
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
            ImGui::Render();
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);

            // Apply top-bar and escape-combo actions after the ImGui frame:
            // toggling fullscreen resizes the window, ending the session
            // tears down the stream, and toggling the grab changes SDL input
            // state, all unsafe mid-frame.
            if (input_actions.fullscreen ||
                topbar_action.kind == cosmic::ui::TopBarAction::ToggleFullscreen) {
                viewer_fullscreen = !viewer_fullscreen;
                SDL_SetWindowFullscreen(
                    window, viewer_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
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

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const float ui_scale = cosmic::ui::scale();
        ImGui::SetNextWindowPos(ImVec2(20 * ui_scale, 20 * ui_scale),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560 * ui_scale, 320 * ui_scale),
                                 ImGuiCond_FirstUseEver);
        ImGui::Begin("Cosmic Desk");
        // Keep UI strings ASCII-only: the default ImGui font has no glyphs
        // beyond Basic Latin, so anything else renders as '?'.
        ImGui::TextUnformatted("Cosmic Desk is running.");
        ImGui::Separator();
        ImGui::Text("Config file: %s", cosmic::Settings::config_file().string().c_str());
        // Hosting status line (plan M3.3): paired_client_count() is cached
        // internally, so polling it every frame costs no disk I/O.
        if (hosting_ok) {
            ImGui::Text("Hosting on :%d - %d client(s) paired", settings.port_base,
                        cosmic::hostglue::paired_client_count());
        } else {
            ImGui::TextUnformatted("Hosting unavailable (see log)");
        }
        ImGui::Text("Resolution mode: %s", cosmic::to_string(settings.resolution_mode));
        ImGui::Text("Bitrate: %d kbps", settings.bitrate_kbps);
        ImGui::Text("Tray: %s", has_tray ? "active" : "unavailable");
        ImGui::Separator();
        ImGui::TextUnformatted("Connect to host");
        ImGui::InputText("Host IP", g_host_ip_input, sizeof(g_host_ip_input));
        if (ImGui::Button("Connect")) {
            if (g_host_ip_input[0] != '\0') {
                g_session->start_connect(g_host_ip_input, settings.port_base);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Settings")) {
            show_settings = !show_settings;
        }
        // Recent hosts (plan M3.3): clicking one fills the IP field; the
        // Connect button starts the session as usual.
        const std::vector<std::string> recent_hosts = settings.recent_hosts_snapshot();
        if (!recent_hosts.empty()) {
            ImGui::TextUnformatted("Recent:");
            for (const auto& host : recent_hosts) {
                if (ImGui::Selectable(host.c_str())) {
                    std::snprintf(g_host_ip_input, sizeof(g_host_ip_input), "%s",
                                  host.c_str());
                }
            }
        }
        ImGui::Text("Session: %s", cosmic::viewer::to_string(session_status.state));
        ImGui::TextWrapped("%s", session_status.message.c_str());
        if ((session_status.state == cosmic::viewer::ViewerState::PairingNeedPin ||
             session_status.state == cosmic::viewer::ViewerState::PairingInProgress) &&
            !session_status.pin.empty()) {
            if (session_status.state == cosmic::viewer::ViewerState::PairingNeedPin) {
                ImGui::TextUnformatted("Enter this PIN on the host:");
            }
            ImGui::SetWindowFontScale(2.0f);
            ImGui::TextUnformatted(session_status.pin.c_str());
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Separator();
        ImGui::TextWrapped(
            "Closing this window hides Cosmic Desk to the tray; hosting keeps running. "
            "Use the tray menu to show it again or to quit.");
        ImGui::Checkbox("Show Dear ImGui demo window", &show_imgui_demo);
        ImGui::End();

        if (show_imgui_demo) {
            ImGui::ShowDemoWindow(&show_imgui_demo);
        }

        if (show_pin_dialog) {
            cosmic::ui::draw_pin_dialog(g_pending_client, show_pin_dialog, pin_result_ok);
            // When pin_result_ok is set, nvhttp::pin() completed the handshake
            // server-side and the client is paired.
        }

        // Called every frame, not just while open: draw_settings_window
        // early-returns when the window is closed and saves pending edits on
        // that transition (plan M4.4).
        cosmic::ui::draw_settings_window(settings, show_settings);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 18, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    settings.save();

    // Stop the viewer session before tearing down SDL (plan M2.7): unblocks
    // the worker thread; the Session destructor joins it.
    g_session->end_session();
    // Destroy the session (and join its worker) here, while `settings` is still
    // alive: the worker calls settings_.add_recent_host(), and g_session is a
    // namespace-scope static whose destructor would otherwise run at static
    // destruction, after main()'s local `settings` is already gone.
    g_session.reset();

    cosmic::ui::tray_stop();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    cosmic::viewer::vrenderer_deinit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    cosmic::hostglue::stop();
    SDL_Quit();
    return 0;
}
