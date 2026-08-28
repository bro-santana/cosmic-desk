#include "app/single_instance.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#endif

namespace cosmic::single_instance {
namespace {

#ifdef _WIN32
// Session-local kernel object names: the "Local\" prefix scopes them to the
// console session, so two users logged into the same machine each get their
// own instance (cosmicsvc respawns the app per active console session).
const wchar_t* kMutexName = L"Local\\CosmicDesk.SingleInstance";
const wchar_t* kShowWindowEventName = L"Local\\CosmicDesk.ShowWindow";

// Service-created objects must be openable by unelevated users (PLAN.md M8/D8):
// cosmicsvc spawns us as SYSTEM, and a NULL lpSecurityAttributes would give the
// mutex and event SYSTEM's default DACL, which grants nothing to standard users
// (or UAC-filtered admin tokens). A normal user's second launch would then fail
// CreateMutexW with ERROR_ACCESS_DENIED (second instance -> port clash) and the
// --shortcut OpenEventW in main.cpp would fail (window never raised). The
// objects hold no secrets -- a mutex name and a signal event -- so grant
// Everyone generic-all. The descriptor is LocalFree'd by the caller after the
// create call.
SECURITY_ATTRIBUTES permissive_security_attributes() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    return sa;
}

HANDLE g_mutex = nullptr;
HANDLE g_show_window_event = nullptr;
#endif

}  // namespace

bool acquire() {
#ifdef _WIN32
    // CreateMutexW succeeds both when the mutex is created and when it already
    // exists; ERROR_ALREADY_EXISTS is how the second instance is detected.
    SECURITY_ATTRIBUTES sa = permissive_security_attributes();
    g_mutex = CreateMutexW(&sa, FALSE, kMutexName);
    if (sa.lpSecurityDescriptor != nullptr) {
        LocalFree(sa.lpSecurityDescriptor);
    }
    if (g_mutex == nullptr) {
        // No mutex means no guard; run anyway rather than fail startup.
        std::fprintf(stderr, "single_instance: CreateMutexW failed (%lu)\n",
                     GetLastError());
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is running: ask it to show its window, then exit.
        // The event may not exist yet (startup race: the owner creates it at
        // acquire() time), in which case there is nothing to signal.
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kShowWindowEventName);
        if (event != nullptr) {
            SetEvent(event);
            CloseHandle(event);
        }
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return false;
    }
    // We are the single instance: create the show-window event now so a second
    // launch always has something to signal. Auto-reset, initially
    // non-signaled; poll_show_request() consumes one signal per launch.
    sa = permissive_security_attributes();
    g_show_window_event = CreateEventW(&sa, FALSE, FALSE, kShowWindowEventName);
    if (sa.lpSecurityDescriptor != nullptr) {
        LocalFree(sa.lpSecurityDescriptor);
    }
    if (g_show_window_event == nullptr) {
        std::fprintf(stderr, "single_instance: CreateEventW failed (%lu)\n",
                     GetLastError());
    }
    return true;
#else
    // Linux double-instance prevention is a follow-up (PLAN.md M8.1): the
    // portable autostart path is the only Linux launch today, and a second
    // launch would just start a second process.
    return true;
#endif
}

bool poll_show_request() {
#ifdef _WIN32
    if (g_show_window_event == nullptr) {
        return false;
    }
    if (WaitForSingleObject(g_show_window_event, 0) == WAIT_OBJECT_0) {
        // The event is auto-reset, so the wait already cleared it; reset
        // explicitly so the exactly-once contract does not depend on that.
        ResetEvent(g_show_window_event);
        return true;
    }
    return false;
#else
    return false;
#endif
}

void release() {
#ifdef _WIN32
    if (g_show_window_event != nullptr) {
        CloseHandle(g_show_window_event);
        g_show_window_event = nullptr;
    }
    if (g_mutex != nullptr) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
#endif
}

}  // namespace cosmic::single_instance