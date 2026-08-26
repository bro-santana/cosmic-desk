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
# the Windows installer would drift apart. cosmicdesk_write_version_files()
# emits the values into the build tree for them to read:
#   build/packaging/version.env         -> packaging/linux/make-deb.sh
#   build/packaging/windows/version.iss -> packaging/windows/installer.iss
#
# This module is included BEFORE project() so the triple can be passed to it,
# so it must not depend on anything project() sets up.

set(_cd_root "${CMAKE_CURRENT_LIST_DIR}/..")

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

# Re-run CMake when HEAD moves, or the generated files go stale. On a branch
# HEAD is a symref and only the ref file it points at changes per commit.
foreach(_cd_watch "${_cd_root}/.git/HEAD" "${_cd_root}/.git/packed-refs")
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

# Emit the derived version into the build tree for the packaging scripts.
# Call after project().
function(cosmicdesk_write_version_files)
    configure_file(${CMAKE_SOURCE_DIR}/packaging/version.env.in
                   ${CMAKE_BINARY_DIR}/packaging/version.env @ONLY)
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/version.iss.in
                   ${CMAKE_BINARY_DIR}/packaging/windows/version.iss @ONLY)
    message(STATUS "Cosmic Desk version: ${COSMICDESK_VERSION_FULL} "
                   "(info ${COSMICDESK_VERSION_INFO}, deb ${COSMICDESK_VERSION_DEB})")
endfunction()
