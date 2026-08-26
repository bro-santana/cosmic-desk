#!/bin/sh
# Cosmic Desk - Debian package for Ubuntu 24.04+ (plan M6.4).
#
# Usage (from anywhere, after `cmake -B build && cmake --build build`):
#   packaging/linux/make-deb.sh [VERSION]
#
# Stages the CMake install rules into a Debian tree under build/dist-deb and
# builds cosmicdesk_<version>_amd64.deb with dpkg-deb. Differences from the
# tarball (make-tarball.sh):
#   - installs into /usr (the CI build configures -DCMAKE_INSTALL_PREFIX=/usr,
#     so the .desktop Exec path matches)
#   - the udev rule goes to usr/lib/udev/rules.d/ so input injection works out
#     of the box
#   - a postinst refreshes the hicolor icon cache
# Dependencies (Depends:) are computed with dpkg-shlibdeps from the build
# machine's package set, so the package targets the same distribution it is
# built on (CI builds on ubuntu-24.04). VERSION defaults to the value derived
# by cmake/CosmicDeskVersion.cmake at configure time, so the deb and the
# Windows installer always agree; pass an argument to override it.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/cosmicdesk"
if [ ! -x "$BIN" ]; then
    echo "error: build/cosmicdesk not found. Run 'cmake -B build && cmake --build build' first." >&2
    exit 1
fi

# The version is derived once, by CMake, and written into the build tree;
# re-running `git describe` here is how this package and the Windows installer
# would drift apart. An explicit argument still wins.
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    VERSION_ENV="$BUILD/packaging/version.env"
    if [ ! -f "$VERSION_ENV" ]; then
        echo "error: $VERSION_ENV not found. Run 'cmake -B build' first." >&2
        exit 1
    fi
    . "$VERSION_ENV"
    VERSION="$COSMICDESK_VERSION_DEB"
fi
# Sanitise anyway: the CMake-derived value already conforms, an argument may not.
VERSION="$(printf '%s' "$VERSION" | sed 's/^v//' | tr -cd '0-9A-Za-z.+~-')"
case "$VERSION" in
    [0-9]*) ;;
    *) VERSION="0.0.0+$VERSION" ;;
esac

DEBROOT="$BUILD/dist-deb"
STAGE="$DEBROOT/cosmicdesk_${VERSION}_amd64"
DEB="$DEBROOT/cosmicdesk_${VERSION}_amd64.deb"

# 1. Stage the CMake install rules into $STAGE/usr.
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
cmake --install "$BUILD" --prefix "$STAGE/usr"

# The .desktop Exec path is baked in at configure time; the deb installs into
# /usr, so a build configured for another prefix would ship a broken launcher.
if grep -q '/usr/local/' "$STAGE/usr/share/applications/cosmicdesk.desktop"; then
    echo "error: the .desktop Exec points at /usr/local. Re-run cmake with" >&2
    echo "       -DCMAKE_INSTALL_PREFIX=/usr (the CI job does this)." >&2
    exit 1
fi

# 2. Activate the udev rule (vendored location for .deb, unlike the tarball).
mkdir -p "$STAGE/usr/lib/udev/rules.d"
mv "$STAGE/usr/share/cosmicdesk/60-cosmicdesk-input.rules" "$STAGE/usr/lib/udev/rules.d/"
rmdir "$STAGE/usr/share/cosmicdesk" 2>/dev/null || true

# 3. Compute runtime dependencies from the binary's DT_NEEDED entries.
# The libdir is GNUInstallDirs' (lib or lib/<multiarch>), so locate the staged
# binary instead of assuming a path.
BIN_STAGE="$(find "$STAGE/usr" -path '*/cosmicdesk/cosmicdesk' -type f -executable | head -n 1)"
if [ -z "$BIN_STAGE" ]; then
    echo "error: staged binary not found under $STAGE/usr; aborting." >&2
    exit 1
fi
if ! command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "error: dpkg-shlibdeps not found. Install the dpkg-dev package." >&2
    exit 1
fi
# dpkg-shlibdeps reads debian/control from its working directory to learn the
# package name; give it a scratch copy (its Depends output does not come from
# there).
SHLIBDIR="$DEBROOT/shlibdeps"
rm -rf "$SHLIBDIR"
mkdir -p "$SHLIBDIR/debian"
cat > "$SHLIBDIR/debian/control" <<EOF
Source: cosmicdesk
Section: net
Priority: optional
Maintainer: Cosmic Desk maintainers
Standards-Version: 4.6.2

Package: cosmicdesk
Architecture: amd64
Description: Remote desktop host and viewer (Moonlight protocol)
EOF
DEPS="$(cd "$SHLIBDIR" && dpkg-shlibdeps -O "$BIN_STAGE" 2>/dev/null | sed 's/^shlibs:Depends=//')"
if [ -z "$DEPS" ]; then
    echo "error: dpkg-shlibdeps produced no dependencies; aborting." >&2
    exit 1
fi

# 4. Debian control file.
cat > "$STAGE/DEBIAN/control" <<EOF
Package: cosmicdesk
Version: $VERSION
Section: net
Priority: optional
Architecture: amd64
Maintainer: Cosmic Desk maintainers
Depends: $DEPS
Homepage: https://github.com/bro-santana/cosmic-desk
Description: Remote desktop host and viewer (Moonlight protocol)
 Cosmic Desk streams your desktop to Moonlight clients and hosts a
 viewer for other Cosmic Desk and Sunshine hosts.
EOF

# 5. postinst: refresh the hicolor icon cache if the tool is present.
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

# 6. Build the package.
rm -f "$DEB"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB"

# 7. Sanity check: metadata and contents are listed before publishing.
dpkg-deb --info "$DEB"
dpkg-deb --contents "$DEB"
echo ""
echo "Deb: $DEB"
echo "Input injection works out of the box (udev rule installed to /usr/lib/udev/rules.d/)."
