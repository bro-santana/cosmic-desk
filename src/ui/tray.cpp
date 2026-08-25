#include "ui/tray.h"

// Pick the backend inside the vendored single-header tray library. It defines
// static functions, so it must be included from exactly this translation unit.
#ifdef _WIN32
#define TRAY_WINAPI 1
#else
#define TRAY_APPINDICATOR 1
#endif

#include <cstring>

#include <tray.h>

namespace cosmic::ui {
namespace {

// tray_menu holds non-const char*, so the label storage has to outlive the
// tray and stay writable.
std::string g_icon_path;
std::string g_show_text = "Show Cosmic Desk";
std::string g_separator = "-";
std::string g_quit_text = "Quit";

TrayCallbacks g_callbacks;
tray_menu g_menu[4];
tray g_tray;
bool g_active = false;

void on_show_clicked(tray_menu*) {
    if (g_callbacks.on_show) {
        g_callbacks.on_show();
    }
}

void on_quit_clicked(tray_menu*) {
    if (g_callbacks.on_quit) {
        g_callbacks.on_quit();
    }
}

}  // namespace

bool tray_start(const std::string& icon_path, TrayCallbacks callbacks) {
    if (g_active) {
        return true;
    }

    g_callbacks = std::move(callbacks);
    g_icon_path = icon_path;

    g_menu[0] = {g_show_text.data(), 0, 0, on_show_clicked, nullptr, nullptr};
    g_menu[1] = {g_separator.data(), 0, 0, nullptr, nullptr, nullptr};
    g_menu[2] = {g_quit_text.data(), 0, 0, on_quit_clicked, nullptr, nullptr};
    g_menu[3] = {nullptr, 0, 0, nullptr, nullptr, nullptr};

    g_tray.icon = g_icon_path.data();
    g_tray.menu = g_menu;

    if (tray_init(&g_tray) != 0) {
        return false;
    }

    g_active = true;
    return true;
}

bool tray_pump() {
    if (!g_active) {
        return false;
    }
    if (tray_loop(0) != 0) {
        g_active = false;
        return false;
    }
    return true;
}

void tray_stop() {
    if (!g_active) {
        return;
    }
    g_active = false;
    tray_exit();
}

bool tray_available() {
    return g_active;
}

}  // namespace cosmic::ui
