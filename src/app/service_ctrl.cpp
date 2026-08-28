#include "app/service_ctrl.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
// winsock2.h must precede windows.h/iphlpapi.h: windows.h would otherwise pull
// in the legacy winsock.h and the two headers clash over the same symbols.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#endif

namespace cosmic::service_ctrl {

#ifdef _WIN32
namespace {

// RAII wrapper for the service-manager and service handles (upstream
// entry_handler.cpp service_controller). Both handles are closed on
// destruction; a failed open leaves the handle null and every operation
// reports failure.
class service_controller {
public:
    explicit service_controller(DWORD service_desired_access) {
        scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm_handle == nullptr) {
            std::fprintf(stderr, "service_ctrl: OpenSCManagerA failed (%lu)\n",
                         GetLastError());
            return;
        }
        service_handle =
            OpenServiceA(scm_handle, kServiceName, service_desired_access);
        if (service_handle == nullptr) {
            std::fprintf(stderr, "service_ctrl: OpenServiceA(%s) failed (%lu)\n",
                         kServiceName, GetLastError());
        }
    }

    ~service_controller() {
        if (service_handle != nullptr) {
            CloseServiceHandle(service_handle);
        }
        if (scm_handle != nullptr) {
            CloseServiceHandle(scm_handle);
        }
    }

    bool start_service() {
        if (service_handle == nullptr) {
            return false;
        }
        if (!StartServiceA(service_handle, 0, nullptr)) {
            const DWORD winerr = GetLastError();
            if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
                std::fprintf(stderr, "service_ctrl: StartServiceA failed (%lu)\n",
                             winerr);
                return false;
            }
        }
        return true;
    }

    bool query_service_status(SERVICE_STATUS& status) {
        if (service_handle == nullptr) {
            return false;
        }
        if (!QueryServiceStatus(service_handle, &status)) {
            std::fprintf(stderr, "service_ctrl: QueryServiceStatus failed (%lu)\n",
                         GetLastError());
            return false;
        }
        return true;
    }

private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
};

}  // namespace

bool is_service_running() {
    service_controller sc(SERVICE_QUERY_STATUS);

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
        return false;
    }
    return status.dwCurrentState == SERVICE_RUNNING;
}

bool start_service() {
    service_controller sc(SERVICE_QUERY_STATUS | SERVICE_START);

    std::cout << "Starting Cosmic Desk service...";
    if (!sc.start_service()) {
        return false;
    }

    // StartServiceA is asynchronous: poll once per second until the service
    // leaves SERVICE_START_PENDING (upstream entry_handler.cpp pattern).
    SERVICE_STATUS status{};
    do {
        Sleep(1000);
        std::cout << '.';
    } while (sc.query_service_status(status) &&
             status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
        std::fprintf(stderr, "service_ctrl: %s failed to start (exit code %lu)\n",
                     kServiceName, status.dwWin32ExitCode);
        return false;
    }

    std::cout << std::endl;
    return true;
}

bool wait_for_ui_ready(uint16_t port) {
    std::cout << "Waiting for Cosmic Desk to be ready...";

    // Wait up to 30 seconds for the host to start listening on `port`
    // (upstream entry_handler.cpp pattern).
    for (int i = 0; i < 30; ++i) {
        PMIB_TCPTABLE tcp_table = nullptr;
        ULONG table_size = 0;
        ULONG err;

        // GetTcpTable needs a growing buffer; reallocate until it fits.
        do {
            err = GetTcpTable(tcp_table, &table_size, false);
            if (err == ERROR_INSUFFICIENT_BUFFER) {
                free(tcp_table);
                tcp_table = static_cast<PMIB_TCPTABLE>(malloc(table_size));
            }
        } while (err == ERROR_INSUFFICIENT_BUFFER);

        if (err != NO_ERROR) {
            free(tcp_table);
            std::fprintf(stderr, "service_ctrl: GetTcpTable failed (%lu)\n", err);
            return false;
        }

        const uint16_t port_nbo = htons(port);
        for (DWORD j = 0; j < tcp_table->dwNumEntries; ++j) {
            const MIB_TCPROW& entry = tcp_table->table[j];
            if (entry.dwLocalPort == port_nbo &&
                entry.dwState == MIB_TCP_STATE_LISTEN) {
                free(tcp_table);
                std::cout << std::endl;
                return true;
            }
        }

        free(tcp_table);
        Sleep(1000);
        std::cout << '.';
    }

    std::cout << "timed out" << std::endl;
    return false;
}

#else
// Non-Windows stub: the service model is Windows-only (plan D7); Linux uses
// the M6 autostart path instead.
bool is_service_running() {
    return false;
}

bool start_service() {
    return false;
}

bool wait_for_ui_ready(uint16_t) {
    return false;
}
#endif

}  // namespace cosmic::service_ctrl