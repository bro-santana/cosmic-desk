// Cosmic Desk — host glue: start/stop the vendored Sunshine host inside our
// process. See PLAN.md M1.4 and D6. This header is deliberately self-contained:
// it must not pull in vendored Sunshine headers (host.cpp owns those).

#pragma once

#include "app/settings.h"

namespace cosmic::hostglue {

// Boots the full host (config, logging, crypto, capture, input, HTTP, RTSP).
// Returns false if a fatal step failed; the caller keeps running either way
// (hosting degraded, plan M1.4).
bool start(const Settings &settings);

// Graceful stop: raises mail::shutdown so the server threads return, joins
// them, then releases the deinit guards in reverse init order. Idempotent:
// subsequent calls are no-ops.
void stop();

// Number of paired clients, read from <config dir>/sunshine_state.json
// (Sunshine's format: root.named_devices array; see nvhttp.cpp save_state/
// load_state). Returns 0 if the file is missing or unparseable. The result is
// cached for 2 seconds, so the UI can poll this every frame without disk I/O.
int paired_client_count();

}  // namespace cosmic::hostglue