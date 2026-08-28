// Cosmic Desk - single-instance guard (plan M8.1). Exactly one app per console
// session: a second launch must never create a second process (the host ports
// would clash). The first instance owns a session-local named mutex; a second
// launch fails to acquire it, signals the running instance to show its window,
// and exits. Non-Windows builds are no-ops: Linux double-instance prevention
// is a follow-up.

#pragma once

namespace cosmic::single_instance {

// Attempts to become the single instance. Returns true when this process is
// the owner; false when another instance is already running (that instance was
// signaled to show its window).
bool acquire();

// Non-blocking: returns true exactly once per external "show window" request
// (a second launch while we are running).
bool poll_show_request();

// Releases the single-instance handles. Call during cleanup.
void release();

}  // namespace cosmic::single_instance