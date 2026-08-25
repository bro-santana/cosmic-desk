#pragma once

namespace cosmic {

// What the single SDL window is currently doing. The host role runs on its own
// threads regardless of this mode; only the window changes.
enum class AppMode {
    HiddenToTray,
    MainWindow,
    Viewing,
};

}  // namespace cosmic
