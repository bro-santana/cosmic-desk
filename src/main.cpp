// Cosmic Desk — one binary that is both the host (screen sharer) and the
// viewer (client). See PLAN.md for the architecture and milestone breakdown.
//
// M0 scope: SDL window + Dear ImGui + tray icon + settings file. The host
// threads (M1) and the viewer session (M2) plug into this loop later.

#include "app/settings.h"
#include "app/state.h"
#include "ui/tray.h"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cstdio>
#include <filesystem>
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

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
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

        if (mode == cosmic::AppMode::HiddenToTray) {
            // Nothing to draw; stay responsive to tray clicks without burning a core.
            SDL_Delay(50);
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
        ImGui::TextUnformatted("Milestone 0 - scaffold is alive.");
        ImGui::Separator();
        ImGui::Text("Config file: %s", cosmic::Settings::config_file().string().c_str());
        ImGui::Text("Host port base: %d", settings.port_base);
        ImGui::Text("Resolution mode: %s", cosmic::to_string(settings.resolution_mode));
        ImGui::Text("Bitrate: %d kbps", settings.bitrate_kbps);
        ImGui::Text("Tray: %s", has_tray ? "active" : "unavailable");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Closing this window hides Cosmic Desk to the tray; hosting keeps running. "
            "Use the tray menu to show it again or to quit.");
        ImGui::Checkbox("Show Dear ImGui demo window", &show_imgui_demo);
        ImGui::End();

        if (show_imgui_demo) {
            ImGui::ShowDemoWindow(&show_imgui_demo);
        }

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 18, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    settings.save();

    cosmic::ui::tray_stop();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
