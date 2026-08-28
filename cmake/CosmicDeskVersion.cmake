# Cosmic Desk - the single source of truth for the release version (plan M6.4).
#
# Derives the version from `git describe --tags --long --always` once, and
# exposes it in the four shapes the packaging formats need:
#   COSMICDESK_VERSION       X.Y.Z          numeric triple, feeds project(VERSION)
#   COSMICDESK_VERSION_FULL  X.Y.Z-N-gSHA   human-facing string
#   COSMICDESK_VERSION_INFO  X.Y.Z.N        Windows VERSIONINFO (four numeric fields)
#   COSMICDESK_VERSION_DEB   X.Y.Z+N+gSHA   Debian-policy-safe package version
# N is the commit count since the tag, so untagged nightlies order correctly
# against each other instead of every one of them claiming 0.0.0.0.
#
# The packaging scripts never re-derive any of this - that is how the deb and
# the Windows installer would drift apart. The values are emitted into the
# build tree for them to read:
#   build/packaging/version.env         -> packaging/linux/make-deb.sh
#   build/packaging/windows/version.iss -> packaging/windows/installer.iss
#
# This file runs in two modes:
#
#   included (from CMakeLists.txt, BEFORE project() so the derived triple can be
#     passed to it - so it must not depend on anything project() sets up):
#     derives the variables and defines cosmicdesk_write_version_files().
#
#   script (`cmake -DCOSMICDESK_SOURCE_DIR=... -DCOSMICDESK_BINARY_DIR=... -P
#     cmake/CosmicDeskVersion.cmake`): derives and rewrites the two generated
#     files, nothing else. The cosmicdesk_version target runs this on EVERY
#     build.
#
# That build-time refresh is not belt-and-braces, it is the whole guarantee.
# `git tag v1.2.3` changes what `git describe` reports while touching no file
# CMake can watch - not HEAD, not the branch ref. Deriving only at configure
# time therefore stamps the version of the commit *before* the tag onto the
# release artifact, and tag-then-package is exactly the release workflow.
# Re-deriving on every build closes that window, since packaging follows a build.

if(DEFINED COSMICDESK_SOURCE_DIR)
    set(_cd_root "${COSMICDESK_SOURCE_DIR}")
else()
    get_filename_component(_cd_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

find_package(Git QUIET)

set(_cd_describe "")
if(Git_FOUND AND EXISTS "${_cd_root}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --long --always
        WORKING_DIRECTORY "${_cd_root}"
        OUTPUT_VARIABLE _cd_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

# --long prints TAG-N-gSHA even when HEAD is exactly on a tag, so one pattern
# covers both cases. With no reachable tag, --always prints the bare short SHA
# (this is also what a shallow clone looks like - CI checks out with
# fetch-depth: 0 to avoid it).
if(_cd_describe MATCHES "^(.+)-([0-9]+)-g([0-9a-fA-F]+)$")
    set(_cd_tag "${CMAKE_MATCH_1}")
    set(_cd_count "${CMAKE_MATCH_2}")
    set(_cd_sha "g${CMAKE_MATCH_3}")
elseif(_cd_describe MATCHES "^[0-9a-fA-F]+$")
    set(_cd_tag "")
    set(_cd_count 0)
    set(_cd_sha "g${_cd_describe}")
else()
    # No git, no repo, or an unrecognised describe output.
    set(_cd_tag "")
    set(_cd_count 0)
    set(_cd_sha "gunknown")
endif()

string(REGEX REPLACE "^v" "" _cd_tag "${_cd_tag}")
if(_cd_tag MATCHES "^([0-9]+)\.([0-9]+)\.([0-9]+)")
    set(COSMICDESK_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
else()
    set(COSMICDESK_VERSION "0.0.0")
endif()

# dpkg reads the LAST hyphen as the debian_revision separator, so no hyphen may
# survive into the upstream version. A pre-release tag's own hyphen becomes `~`,
# which is the one character that sorts BEFORE the empty string - so
# 2.0.0~rc1 < 2.0.0, the ordering a tag like v2.0.0-rc1 is asking for.
string(REPLACE "-" "~" _cd_tag_deb "${_cd_tag}")
if(_cd_tag STREQUAL "")
    set(COSMICDESK_VERSION_FULL "${COSMICDESK_VERSION}+${_cd_sha}")
    set(COSMICDESK_VERSION_DEB "${COSMICDESK_VERSION}+${_cd_sha}")
elseif(_cd_count EQUAL 0)
    set(COSMICDESK_VERSION_FULL "${_cd_tag}")
    set(COSMICDESK_VERSION_DEB "${_cd_tag_deb}")
else()
    set(COSMICDESK_VERSION_FULL "${_cd_tag}-${_cd_count}-${_cd_sha}")
    set(COSMICDESK_VERSION_DEB "${_cd_tag_deb}+${_cd_count}+${_cd_sha}")
endif()

# Debian upstream versions allow [0-9A-Za-z.+~-] and must start with a digit.
string(REGEX REPLACE "[^0-9A-Za-z.+~-]" "" COSMICDESK_VERSION_DEB "${COSMICDESK_VERSION_DEB}")
if(NOT COSMICDESK_VERSION_DEB MATCHES "^[0-9]")
    set(COSMICDESK_VERSION_DEB "0.0.0+${COSMICDESK_VERSION_DEB}")
endif()

# VERSIONINFO fields are 16-bit; clamp rather than let ISCC reject the build.
set(_cd_tweak "${_cd_count}")
if(_cd_tweak GREATER 65535)
    set(_cd_tweak 65535)
endif()
set(COSMICDESK_VERSION_INFO "${COSMICDESK_VERSION}.${_cd_tweak}")

# configure_file leaves the output untouched when the content is unchanged, so
# running this on every build costs nothing and causes no rebuild churn.
function(_cosmicdesk_generate_version_files _bindir)
    configure_file(${_cd_root}/packaging/version.env.in
                   ${_bindir}/packaging/version.env @ONLY)
    configure_file(${_cd_root}/packaging/windows/version.iss.in
                   ${_bindir}/packaging/windows/version.iss @ONLY)
endfunction()

# Script mode (`cmake -P`): regenerate and stop. Nothing below applies.
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED COSMICDESK_BINARY_DIR)
        message(FATAL_ERROR
                "COSMICDESK_BINARY_DIR must be set when running this file with -P")
    endif()
    _cosmicdesk_generate_version_files("${COSMICDESK_BINARY_DIR}")
    return()
endif()

# Re-run CMake when HEAD moves. On a branch HEAD is a symref and only the ref
# file it points at changes per commit; refs/tags catches `git tag` via the
# directory mtime. None of this is load-bearing - a new loose ref does not
# reliably bump the directory mtime on every filesystem - which is why
# cosmicdesk_version re-derives at build time regardless.
foreach(_cd_watch "${_cd_root}/.git/HEAD"
                  "${_cd_root}/.git/packed-refs"
                  "${_cd_root}/.git/refs/tags")
    if(EXISTS "${_cd_watch}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_cd_watch}")
    endif()
endforeach()
if(EXISTS "${_cd_root}/.git/HEAD")
    file(READ "${_cd_root}/.git/HEAD" _cd_head)
    if(_cd_head MATCHES "^ref: *([^\r\n]+)" AND EXISTS "${_cd_root}/.git/${CMAKE_MATCH_1}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${_cd_root}/.git/${CMAKE_MATCH_1}")
    endif()
endif()

# Emit the derived version into the build tree, and add the target that keeps
# it honest. Call after project().
function(cosmicdesk_write_version_files)
    _cosmicdesk_generate_version_files("${CMAKE_BINARY_DIR}")
    # ALL, so any `cmake --build` refreshes the files from live git state.
    # Packaging always follows a build, so this is what actually guarantees the
    # installer and the deb carry the version of the commit being packaged.
    add_custom_target(cosmicdesk_version ALL
        COMMAND ${CMAKE_COMMAND}
                -DCOSMICDESK_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DCOSMICDESK_BINARY_DIR=${CMAKE_BINARY_DIR}
                -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CosmicDeskVersion.cmake
        COMMENT "Refreshing the derived version from git"
        VERBATIM)
    message(STATUS "Cosmic Desk version: ${COSMICDESK_VERSION_FULL} "
                   "(info ${COSMICDESK_VERSION_INFO}, deb ${COSMICDESK_VERSION_DEB})")
endfunction()
