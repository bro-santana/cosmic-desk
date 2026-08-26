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
#include "ui/tray.h"
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

namespace {

constexpr int kWindowWidth = 1000;
constexpr int kWindowHeight = 640;

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

// Viewer session (plan M2.2): one session object for the whole app lifetime;
// its worker thread does all networking so the main loop never blocks.
std::unique_ptr<cosmic::viewer::Session> g_session =
    std::make_unique<cosmic::viewer::Session>();
// ASCII-only host IP input: the default ImGui font has no other glyphs, and
// the vendored imgui has no std::string InputText overload (imgui_stdlib.h is
// not vendored), so this is a fixed buffer.
char g_host_ip_input[64] = {};

}  // namespace

int main(int argc, char** argv) {
    bool start_hidden = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--hidden") {
            start_hidden = true;
        }
    }

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

    cosmic::Settings settings = cosmic::Settings::load();
    if (!std::filesystem::exists(cosmic::Settings::config_file())) {
        // Materialize defaults on first run so the file is there to be edited.
        settings.save();
    }

    if (!cosmic::hostglue::start(settings)) {
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

    bool show_imgui_demo = false;

    // Viewer renderer is created lazily once the negotiated stream dimensions
    // are known and destroyed when leaving Viewing mode (plan M2.4).
    bool vrenderer_active = false;

    // Pending pairing state (plan M1.4): set when the host thread reports a
    // /pair request; consumed by the PIN dialog below.
    std::string g_pending_client;
    bool show_pin_dialog = false;
    bool pin_result_ok = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // While streaming, forward input to the host before ImGui sees it
            // (plan M2.6). Consumed events never reach ImGui. The overlay's
            // "End session" button still works because ImGui claims mouse
            // capture when the cursor is over it (WantCaptureMouse), which
            // gates forwarding; the Ctrl+Alt+Shift+Q escape combo stays active
            // regardless of the capture state.
            if (mode == cosmic::AppMode::Viewing &&
                cosmic::viewer::input::handle_event(
                    event, *g_session, window, ImGui::GetIO().WantCaptureKeyboard,
                    ImGui::GetIO().WantCaptureMouse)) {
                continue;
            }
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                // Closing the last window means "get out of the way", not
                // "stop hosting" — that is what the tray Quit item is for.
                if (has_tray) {
                    mode = cosmic::AppMode::HiddenToTray;
                    SDL_HideWindow(window);
                } else {
                    running = false;
                }
            } else if (event.type == SDL_WINDOWEVENT &&
                       event.window.event == SDL_WINDOWEVENT_CLOSE &&
                       event.window.windowID == SDL_GetWindowID(window)) {
                if (has_tray) {
                    mode = cosmic::AppMode::HiddenToTray;
                    SDL_HideWindow(window);
                } else {
                    running = false;
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
                mode = cosmic::AppMode::Viewing;
            }
        } else if (mode == cosmic::AppMode::Viewing) {
            mode = cosmic::AppMode::MainWindow;
            cosmic::viewer::vrenderer_deinit();
            vrenderer_active = false;
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

            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::Begin("Viewer", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoSavedSettings);
            ImGui::Text("Streaming %dx%d", session_status.stream_width,
                        session_status.stream_height);
            ImGui::Separator();
            if (ImGui::Button("End session")) {
                g_session->end_session();
            }
            ImGui::End();

            ImGui::Render();
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            continue;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_FirstUseEver);
        ImGui::Begin("Cosmic Desk");
        // Keep UI strings ASCII-only: the default ImGui font has no glyphs
        // beyond Basic Latin, so anything else renders as '?'.
        ImGui::TextUnformatted("Cosmic Desk is running.");
        ImGui::Separator();
        ImGui::Text("Config file: %s", cosmic::Settings::config_file().string().c_str());
        ImGui::Text("Host port base: %d", settings.port_base);
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
        ImGui::Text("Session: %s", cosmic::viewer::to_string(session_status.state));
        ImGui::TextWrapped("%s", session_status.message.c_str());
        if (session_status.state == cosmic::viewer::ViewerState::PairingNeedPin) {
            ImGui::TextUnformatted("Enter this PIN on the host:");
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
