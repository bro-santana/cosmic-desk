// Cosmic Desk — host glue implementation. Boots the vendored Sunshine host
// inside our process, mirroring upstream Sunshine main.cpp (lines ~155-400)
// minus the parts Cosmic Desk replaces (web UI, tray, service, signal handlers).

#include "hostglue/host.h"
#include "hostglue/clipboard.h"
#include "hostglue/wallpaper.h"

// Vendored Sunshine headers. Only this TU depends on them; host.h stays
// self-contained. The include root is host/sunshine (see top-level CMakeLists).
#include "src/config.h"
#include "src/display_device.h"
#include "src/globals.h"
#include "src/httpcommon.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/video.h"

extern "C" {
#include "src/rswrapper.h"
}

#ifdef _WIN32
  #include <windows.h>
  #include <shlobj.h>
#endif

#include <clocale>
#include <chrono>
#include <codecvt>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <locale>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace cosmic::hostglue {
namespace {

// Deinit guards are held in static storage and released in stop() (reverse
// init order) so their deinit logging still has a live logger.
std::unique_ptr<logging::deinit_t> g_log_guard;
std::unique_ptr<platf::deinit_t> g_display_device_guard;
std::unique_ptr<platf::deinit_t> g_platf_guard;
std::unique_ptr<platf::deinit_t> g_proc_guard;
std::unique_ptr<platf::deinit_t> g_input_guard;

// Server threads are held in static storage so stop() can join them.
// nvhttp::start and rtsp_stream::start both observe mail::shutdown and return
// (nvhttp waits on shutdown_event->view(), rtsp loops on shutdown_event->peek()),
// so joining is safe.
std::thread g_nvhttp_thread;
std::thread g_rtsp_thread;

// stop() is idempotent: the first call tears the host down, later calls no-op.
static bool g_stopped;

// Writes host.conf (the config file Sunshine's config.cpp reads) from the app
// settings. Only the port key is managed: every other line is preserved, so
// user edits to other keys survive relaunches, and a manual port_base override
// in host.conf stays durable even when cosmic.json differs.
bool write_host_conf(const Settings &settings) {
  const auto conf_path = platf::appdata() / "host.conf";
  const std::string port_line = "port = " + std::to_string(settings.port_base);

  std::error_code ec;
  std::filesystem::create_directories(platf::appdata(), ec);
  if (ec) {
    std::fprintf(stderr, "hostglue: failed to create %s: %s\n",
                 platf::appdata().string().c_str(), ec.message().c_str());
    return false;
  }

  // Read the existing file line-by-line, keeping every line. Replace the line
  // whose key (before '=') is "port"; append one if no port line exists.
  std::vector<std::string> lines;
  {
    std::ifstream in(conf_path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
    }
  }

  bool changed = false;
  bool found_port = false;
  for (auto &line : lines) {
    // Sunshine's config.cpp parse_option splits on '=', so match the key the
    // same way: "port=47989" and "port = 47989" are both valid port lines.
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    const auto key_begin = key.find_first_not_of(" \t");
    if (key_begin == std::string::npos) {
      continue;
    }
    const auto key_end = key.find_last_not_of(" \t");
    key = key.substr(key_begin, key_end - key_begin + 1);
    if (key == "port") {
      // Ignore a trailing '\r' so an existing CRLF file is not rewritten.
      std::string trimmed = line;
      if (!trimmed.empty() && trimmed.back() == '\r') {
        trimmed.pop_back();
      }
      if (trimmed != port_line) {
        line = port_line;
        changed = true;
      }
      found_port = true;
    }
  }
  if (!found_port) {
    lines.push_back(port_line);
    changed = true;
  }

  if (!changed) {
    return true;
  }

  std::ofstream out(conf_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "hostglue: failed to write %s\n", conf_path.string().c_str());
    return false;
  }
  for (const auto &line : lines) {
    out << line << '\n';
  }
  return out.good();
}

}  // namespace

// One-time migration to the machine-wide config location (Windows only).
// Before the ProgramData change, elevated runs stored everything under
// %APPDATA%\CosmicDesk of whoever ran the app (for the service that is
// C:\Windows\System32\config\systemprofile\AppData\Roaming\CosmicDesk). If
// the new location has no config yet and the legacy one does, copy the whole
// folder over so pairing and settings survive the upgrade. Called before
// anything writes to platf::appdata().
#ifdef _WIN32
void migrate_legacy_appdata() {
  const auto new_dir = platf::appdata();

  PWSTR roaming = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY,
                                  nullptr, &roaming))) {
    return;
  }
  std::filesystem::path legacy {roaming};
  CoTaskMemFree(roaming);
  legacy = legacy / L"CosmicDesk";

  std::error_code ec;
  if (legacy == new_dir || !std::filesystem::exists(legacy, ec)) {
    return;
  }
  if (std::filesystem::exists(new_dir / "host.conf", ec) ||
      std::filesystem::exists(new_dir / "cosmic.json", ec)) {
    return;
  }

  std::filesystem::create_directories(new_dir, ec);
  if (ec) {
    return;
  }
  std::filesystem::directory_iterator it(legacy, ec);
  if (ec) {
    return;
  }
  for (auto &entry : it) {
    ec.clear();
    std::filesystem::copy(entry.path(), new_dir / entry.path().filename(),
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
      BOOST_LOG(warning) << "Failed to migrate " << entry.path();
    }
  }
  BOOST_LOG(info) << "Migrated config from " << legacy << " to " << new_dir;
}
#endif

int paired_client_count() {
  // Sunshine's state file format (host/sunshine/src/nvhttp.cpp save_state/
  // load_state): root.named_devices is an array of paired client certs. Read it
  // from disk rather than touching the vendored in-memory client_root, which
  // the nvhttp thread owns. Cached for 2 s so the UI can poll per frame.
  static std::chrono::steady_clock::time_point last_check;
  static int cached_count = 0;

  const auto now = std::chrono::steady_clock::now();
  if (now - last_check < std::chrono::seconds(2)) {
    return cached_count;
  }
  last_check = now;

  std::ifstream file(platf::appdata() / "sunshine_state.json");
  if (!file.is_open()) {
    cached_count = 0;
    return 0;
  }

  nlohmann::json json =
      nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded() || !json.is_object()) {
    cached_count = 0;
    return 0;
  }

  const auto &root = json["root"];
  if (!root.is_object()) {
    cached_count = 0;
    return 0;
  }
  const auto &devices = root["named_devices"];
  cached_count = devices.is_array() ? static_cast<int>(devices.size()) : 0;
  return cached_count;
}

bool start(const Settings &settings) {
  // Locale setup mirrors upstream main.cpp (lines ~152-159): the C locale keeps
  // parsing locale-independent, and the UTF-8 codecvt keeps boost::log happy.
#ifdef _WIN32
  // Use the C locale so parsing is locale-independent
  std::setlocale(LC_ALL, "C");
#endif
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));

#ifdef _WIN32
  // Bring config/state from the legacy per-profile location over to the
  // machine-wide one (ProgramData) before anything touches appdata().
  migrate_legacy_appdata();
#endif

  if (!write_host_conf(settings)) {
    return false;
  }

  // Must be set before nvhttp's /serverinfo, /cosmic/wallpaper and
  // /cosmic/clipboard handlers can be reached, since all three consult the
  // corresponding provider.
  cosmic::wallpaper::set_enabled(settings.share_wallpaper);
  cosmic::clipboard::set_enabled(settings.share_clipboard);

  mail::man = std::make_shared<safe::mail_raw_t>();

  // config::parse() with a synthetic argv: no command-line options. It reads
  // host.conf (written above) and creates it if missing.
  int argc = 1;
  char arg0[] = "cosmicdesk";
  char *argv[] = {arg0, nullptr};
  if (config::parse(argc, argv)) {
    std::fprintf(stderr, "hostglue: config::parse failed\n");
    return false;
  }

  g_log_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!g_log_guard) {
    BOOST_LOG(error) << "Logging failed to initialize";
  }

  BOOST_LOG(info) << "Cosmic Desk host starting";

  g_display_device_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!g_display_device_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize";
  }

#ifdef _WIN32
  // Modify relevant NVIDIA control panel settings if the system has a
  // corresponding GPU (mirrors upstream main.cpp).
  if (nvprefs_instance.load()) {
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    nvprefs_instance.modify_application_profile();
    nvprefs_instance.modify_global_profile();
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate during logoff/shutdown.
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);
#endif

  // The task pool runs delayed tasks (input key repeat, stream force-kill);
  // upstream starts it with one worker thread.
  task_pool.start(1);

  proc::refresh(config::stream.file_apps);

  g_platf_guard = platf::init();
  if (!g_platf_guard) {
    BOOST_LOG(error) << "Platform failed to initialize";
  }

  g_proc_guard = proc::init();
  if (!g_proc_guard) {
    BOOST_LOG(error) << "Proc failed to initialize";
  }

  reed_solomon_init();

  g_input_guard = input::init();
  if (!g_input_guard) {
    BOOST_LOG(error) << "Input failed to initialize";
  }

  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder";
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize";
    return false;
  }

  g_nvhttp_thread = std::thread {nvhttp::start};
  g_rtsp_thread = std::thread {rtsp_stream::start};

  BOOST_LOG(info) << "Cosmic Desk hosting on port :" << config::sunshine.port;
  return true;
}

void stop() {
  // Idempotent: only the first call tears the host down; the caller may stop
  // the host more than once (e.g. on shutdown after a failed start).
  if (g_stopped) {
    return;
  }
  g_stopped = true;

  // Graceful stop: raise mail::shutdown so the server threads return (nvhttp
  // waits on shutdown_event->view(), rtsp loops on shutdown_event->peek()),
  // then join them. The deinit guards are released in reverse init order so
  // their deinit logging still has a live logger.
  if (g_nvhttp_thread.joinable()) {
    mail::man->event<bool>(mail::shutdown)->raise(true);
    g_nvhttp_thread.join();
  }
  if (g_rtsp_thread.joinable()) {
    g_rtsp_thread.join();
  }

  // Stop and join the task pool so delayed tasks (input key repeat, stream
  // force-kill) finish before the deinit guards are released (mirrors upstream
  // main.cpp). ThreadPool::join() on a never-started pool is a no-op.
  task_pool.stop();
  task_pool.join();

  BOOST_LOG(info) << "Cosmic Desk host stopping";
  g_input_guard.reset();
  g_proc_guard.reset();
  g_platf_guard.reset();
  g_display_device_guard.reset();
  g_log_guard.reset();
}

}  // namespace cosmic::hostglue