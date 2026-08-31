// Cosmic Desk — client-side file transfer progress overlay implementation
// (see transfer_overlay.h). ASCII-only strings: the default ImGui font has no
// glyphs beyond Basic Latin, so anything else renders as '?' -- a dropped
// file with a non-ASCII name may show missing glyphs here; that is a
// font-atlas limitation, not something this overlay works around.

#include "ui/transfer_overlay.h"

#include <cstdio>

#include <imgui.h>

namespace cosmic::ui {
namespace {

// Renders `bytes` as a short human-readable size, matching the precision the
// tiny overlay has room for.
std::string FormatBytes(std::uint64_t bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  constexpr double kGiB = kMiB * 1024.0;
  char buf[32];
  if (bytes >= static_cast<std::uint64_t>(kGiB)) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / kGiB);
  } else if (bytes >= static_cast<std::uint64_t>(kMiB)) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / kMiB);
  } else if (bytes >= static_cast<std::uint64_t>(kKiB)) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / kKiB);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

// " +N more" when files are still waiting behind the current/last one; empty
// otherwise.
std::string QueuedSuffix(int queued) {
  if (queued <= 0) {
    return {};
  }
  return " +" + std::to_string(queued) + " more";
}

}  // namespace

void draw_transfer_overlay(const TransferStatus& status) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  // Bottom-left corner: SetNextWindowPos's pivot puts the window's own
  // bottom-left at the viewport's bottom-left, mirroring how the top bar pins
  // itself to viewport->Pos for the top edge.
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y),
                          ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  // ImGuiWindowFlags_NoInputs is REQUIRED, not cosmetic: main.cpp gates input
  // forwarding to the host on ImGui::GetIO().WantCaptureMouse (see the
  // viewer::input::handle_event call in the main loop), so an overlay that
  // accepted mouse input would silently swallow the user's clicks over this
  // corner of the remote desktop.
  ImGui::Begin("TransferOverlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);

  if (!status.message.empty()) {
    ImGui::TextUnformatted(status.message.c_str());
  } else if (!status.error.empty()) {
    std::string line;
    if (!status.filename.empty()) {
      line = status.filename + ": ";
    }
    line += status.error;
    line += QueuedSuffix(status.queued);
    ImGui::TextUnformatted(line.c_str());
  } else if (status.active) {
    std::string line = status.filename + ": " + FormatBytes(status.transferred);
    if (status.total > 0) {
      // Guarded above: no division by zero for a file whose size is not
      // known yet (the worker publishes total = 0 as a placeholder until the
      // real size is read -- see filesync.cpp's UploadFile).
      line += " / " + FormatBytes(status.total);
      const int percent = static_cast<int>(
          (static_cast<double>(status.transferred) / static_cast<double>(status.total)) *
          100.0);
      line += " (" + std::to_string(percent) + "%)";
    }
    line += QueuedSuffix(status.queued);
    ImGui::TextUnformatted(line.c_str());
  } else if (status.done) {
    std::string line = status.filename + " sent";
    line += QueuedSuffix(status.queued);
    ImGui::TextUnformatted(line.c_str());
  }

  ImGui::End();
}

}  // namespace cosmic::ui
