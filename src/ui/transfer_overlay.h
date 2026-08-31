// Cosmic Desk — client-side file transfer progress overlay (Wave F1 of
// docs/FILE_TRANSFER_PLAN.md). Small window pinned to the bottom-left of the
// viewer window while a dropped file is uploading, lingering briefly after.

#pragma once

#include <cstdint>
#include <string>

namespace cosmic::ui {

// Per-frame input, filled by main.cpp every frame from
// cosmic::filesync::progress() plus a local, main.cpp-owned notice for a drop
// that never reaches the worker (a rejected folder or a vanished path). The
// ui layer must not include app/filesync.h (same bridging PairProgress
// (src/ui/bridge/bridge.h:17-23) and MonitorInfo (src/ui/viewer_topbar.h:
// 29-34) already do for other layers), so this is a plain mirror of
// cosmic::filesync::Progress rather than that type itself.
struct TransferStatus {
  bool active = false;         // a file is uploading right now
  std::string filename;        // basename of the current/last file
  std::uint64_t transferred = 0;
  std::uint64_t total = 0;
  int queued = 0;               // files still waiting, excluding the current one
  bool done = false;            // the last file finished successfully
  std::string error;            // non-empty = the last file failed
  std::string message;          // local notice, e.g. a rejected folder
};

// Draws the overlay unconditionally. The caller decides whether to draw at
// all -- and for how long a finished/failed transfer should keep showing --
// by only calling this while its own visibility window (built from
// Progress::updated_ms) says so.
void draw_transfer_overlay(const TransferStatus& status);

}  // namespace cosmic::ui
