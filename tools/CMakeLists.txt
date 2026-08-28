# ---------------------------------------------------------------------------
# cosmicsvc: the Windows service launcher (M7). A LocalSystem service that
# spawns cosmicdesk.exe as SYSTEM in the active console session; see
# tools/cosmicsvc.cpp and PLAN.md M7 / D7-D9.
#
# LAYOUT CONTRACT: cosmicsvc.exe must sit in a tools\ subfolder next to
# cosmicdesk.exe. main() sets its CWD by stripping 2 path components (the file
# name and the "tools" folder), so the binary must build to build\tools\ and
# later be packaged to dist\CosmicDesk\tools\ next to the app.
# ---------------------------------------------------------------------------
add_executable(cosmicsvc cosmicsvc.cpp)

# wtsapi32 provides WTSGetActiveConsoleSessionId().
target_link_libraries(cosmicsvc wtsapi32)

# The service binary must be self-contained. The SCM starts it in session 0
# with no MSYS2 bin dir on PATH, so a dynamically linked MinGW runtime
# (libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll) makes the
# process die at startup and the service fails with error 1053 ("did not
# respond in a timely fashion"). -static covers all three; system DLLs
# (kernel32/advapi32/ucrtbase) are always available.
target_link_options(cosmicsvc PRIVATE -static)
