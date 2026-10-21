#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds the Debian/Ubuntu package of OpenMine Suite from a Release build.
#
#   packaging/linux/make_deb.sh <build-dir> [output-dir]
#
# Layout of the package:
#   /opt/openmine-suite/bin/openmine-desktop      the application (RUNPATH $ORIGIN/../lib)
#   /opt/openmine-suite/lib/                      bundled libraries not packaged by the distribution
#                                                 (PDAL + plugins, Open3D, OR-Tools, Clipper2)
#   /opt/openmine-suite/lib/openmine/plugins/     sample native and Python plugins
#   /opt/openmine-suite/lib/openmine/python/      the openmine Python package (built for python3.12)
#   /opt/openmine-suite/share/doc/                licence texts and documentation
#   /usr/bin/openmine-suite                       launcher
#   /usr/bin/openmine-python                      python3.12 with the openmine package
#   desktop entry, icon, MIME type for .openmine files
# Every other library comes from distribution packages, listed by dpkg-shlibdeps in Depends.
set -euo pipefail

BUILD=$(realpath "${1:?usage: make_deb.sh <build-dir> [output-dir]}")
OUT=$(realpath "${2:-$PWD}")
SRC=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=$(sed -n 's/^project(OpenMineSuite VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
ARCH=$(dpkg --print-architecture)
PKG=openmine-suite
STAGE=$(mktemp -d)/${PKG}_${VERSION}_${ARCH}
PREFIX=$STAGE/opt/openmine-suite
trap 'rm -rf "$(dirname "$STAGE")"' EXIT

[ -x "$BUILD/bin/openmine-desktop" ] || { echo "no $BUILD/bin/openmine-desktop" >&2; exit 1; }
command -v patchelf >/dev/null || { echo "patchelf is required (pip install patchelf)" >&2; exit 1; }
grep -q "CMAKE_BUILD_TYPE:STRING=Release" "$BUILD/CMakeCache.txt" || echo "warning: $BUILD is not a Release build" >&2

mkdir -p "$PREFIX/bin" "$PREFIX/lib/openmine/plugins" "$PREFIX/lib/openmine/python" "$PREFIX/share/doc/licenses" \
         "$STAGE/usr/bin" "$STAGE/usr/share/applications" "$STAGE/usr/share/icons/hicolor/scalable/apps" \
         "$STAGE/usr/share/mime/packages" "$STAGE/usr/share/doc/$PKG" "$STAGE/DEBIAN"

# ---- application, Python package, plugins ---------------------------------------------------------
install -m 755 "$BUILD/bin/openmine-desktop" "$PREFIX/bin/"
cp -r "$BUILD/python/openmine" "$PREFIX/lib/openmine/python/"
find "$PREFIX/lib/openmine/python" -name "__pycache__" -prune -exec rm -rf {} +
cp -r "$BUILD/plugins/." "$PREFIX/lib/openmine/plugins/"
find "$PREFIX/lib/openmine/plugins" -name "__pycache__" -prune -exec rm -rf {} +

# ---- bundled libraries: everything an ELF file needs from outside the distribution ----------------
bundle_from() { # copy libraries resolved under /opt (not packaged by Ubuntu) into lib/
    ldd "$1" | awk '/=> \/opt\// {print $3}' | while read -r lib; do
        name=$(basename "$lib")
        case "$name" in libz.so.*) continue ;; esac # OR-Tools ships its own zlib; the system's is used
        [ -e "$PREFIX/lib/$name" ] || cp -L "$lib" "$PREFIX/lib/$name"
    done
}
ELVES=("$PREFIX/bin/openmine-desktop" $(find "$PREFIX/lib/openmine" -name "*.so*" -type f))
for f in "${ELVES[@]}"; do bundle_from "$f"; done
# PDAL loads its plugins (E57, pgpointcloud) at run time from PDAL_DRIVER_PATH.
mkdir -p "$PREFIX/lib/pdal-plugins"
for p in /opt/pdal/lib/libpdal_plugin_*.so; do cp -L "$p" "$PREFIX/lib/pdal-plugins/"; bundle_from "$p"; done
# Second pass: dependencies of the bundled libraries themselves.
for f in "$PREFIX"/lib/*.so*; do bundle_from "$f"; done

# Relative run paths, so nothing depends on /opt/<dependency> or LD_LIBRARY_PATH.
patchelf --set-rpath '$ORIGIN/../lib' "$PREFIX/bin/openmine-desktop"
for f in "$PREFIX"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done
for f in "$PREFIX"/lib/pdal-plugins/*.so; do patchelf --set-rpath '$ORIGIN/..' "$f"; done
for f in $(find "$PREFIX/lib/openmine" -name "*.so*" -type f); do
    rel=$(realpath --relative-to="$(dirname "$f")" "$PREFIX/lib")
    patchelf --set-rpath "\$ORIGIN/$rel" "$f"
done
strip --strip-unneeded "$PREFIX/bin/openmine-desktop" $(find "$PREFIX/lib/openmine" -name "*.so*" -type f) 2>/dev/null || true

# ---- launchers --------------------------------------------------------------------------------------
cat > "$STAGE/usr/bin/openmine-suite" <<'LAUNCH'
#!/bin/sh
# OpenMine Suite launcher.
export PDAL_DRIVER_PATH=/opt/openmine-suite/lib/pdal-plugins${PDAL_DRIVER_PATH:+:$PDAL_DRIVER_PATH}
exec /opt/openmine-suite/bin/openmine-desktop "$@"
LAUNCH
cat > "$STAGE/usr/bin/openmine-python" <<'LAUNCH'
#!/bin/sh
# python3.12 with the openmine package of OpenMine Suite on its path: openmine-python script.py
export PDAL_DRIVER_PATH=/opt/openmine-suite/lib/pdal-plugins${PDAL_DRIVER_PATH:+:$PDAL_DRIVER_PATH}
export PYTHONPATH=/opt/openmine-suite/lib/openmine/python${PYTHONPATH:+:$PYTHONPATH}
exec /usr/bin/python3.12 "$@"
LAUNCH
chmod 755 "$STAGE/usr/bin/openmine-suite" "$STAGE/usr/bin/openmine-python"

# ---- desktop integration ----------------------------------------------------------------------------
install -m 644 "$SRC/packaging/linux/openmine-suite.desktop" "$STAGE/usr/share/applications/"
install -m 644 "$SRC/packaging/linux/openmine-suite.svg" "$STAGE/usr/share/icons/hicolor/scalable/apps/"
install -m 644 "$SRC/packaging/linux/openmine-suite-mime.xml" "$STAGE/usr/share/mime/packages/openmine-suite.xml"

# ---- licences and documentation ---------------------------------------------------------------------
cp "$SRC/LICENSE" "$SRC/NOTICE" "$SRC/THIRD_PARTY_NOTICES.md" "$SRC/README.md" "$PREFIX/share/doc/"
cp -r "$SRC/docs/user-guide.md" "$SRC/docs/python.md" "$SRC/docs/plugins.md" "$SRC/docs/console-commands.md" "$PREFIX/share/doc/"
copy_licence() { [ -f "$2" ] && cp "$2" "$PREFIX/share/doc/licenses/$1" || echo "warning: no licence file $2" >&2; }
copy_licence PDAL.txt /opt/src/pdal-src/LICENSE.txt
copy_licence Open3D.txt "$SRC/packaging/linux/licenses/Open3D.txt"
copy_licence OR-Tools.txt /opt/ortools/share/doc/ortools/LICENSE
copy_licence Clipper2.txt /opt/src/clipper2-src/LICENSE
{
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: OpenMine Suite"
    echo
    echo "Files: *"
    echo "License: Apache-2.0"
    echo " See /opt/openmine-suite/share/doc/LICENSE and NOTICE. Third-party components and their"
    echo " licences: /opt/openmine-suite/share/doc/THIRD_PARTY_NOTICES.md and share/doc/licenses/."
} > "$STAGE/usr/share/doc/$PKG/copyright"

# ---- control ----------------------------------------------------------------------------------------
# Distribution libraries the ELF files need, as package dependencies.
SHLIB_TMP=$(mktemp -d)
mkdir -p "$SHLIB_TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" > "$SHLIB_TMP/debian/control"
ALL_ELF=$(find "$PREFIX" -type f \( -name "*.so*" -o -perm -u+x \) -exec sh -c 'file -b "$1" | grep -q ELF && echo "$1"' _ {} \;)
(cd "$SHLIB_TMP" && dpkg-shlibdeps -O -l"$PREFIX/lib" --ignore-missing-info $ALL_ELF 2>/dev/null) | sed -n 's/^shlibs:Depends=//p' > "$SHLIB_TMP/deps"
rm -rf "$SHLIB_TMP/debian"
DEPENDS="$(cat "$SHLIB_TMP/deps"), qt6-qpa-plugins, python3.12, python3-numpy, shared-mime-info, desktop-file-utils"
rm -rf "$SHLIB_TMP"
SIZE=$(du -sk "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<CTRL
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: OpenMine Suite contributors <maintainers@openmine.invalid>
Installed-Size: $SIZE
Depends: $DEPENDS
Section: science
Priority: optional
Homepage: https://openmine.invalid
Description: Integrated mine survey, CAD/mine design and scheduling
 OpenMine Suite is an open-source desktop application covering point-cloud
 survey (registration, as-built surfaces, overbreak/underbreak), mine CAD and
 design (development headings, declines, stopes, solids) and scheduling
 (calendar-time CPM, resources, levelling, OR-Tools optimisation, 4D) in one
 project, with a Python API, command console and plugins.
CTRL
cat > "$STAGE/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
command -v update-mime-database >/dev/null && update-mime-database /usr/share/mime || true
command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
exit 0
POST
cp "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"
cat > "$STAGE/DEBIAN/prerm" <<'PRERM'
#!/bin/sh
# Byte-code caches Python wrote at run time are not part of the package: remove them first.
set -e
find /opt/openmine-suite -name "__pycache__" -type d -prune -exec rm -rf {} + 2>/dev/null || true
exit 0
PRERM
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm" "$STAGE/DEBIAN/prerm"

mkdir -p "$OUT"
dpkg-deb --build --root-owner-group -Zxz "$STAGE" "$OUT/${PKG}_${VERSION}_${ARCH}.deb" >/dev/null
echo "$OUT/${PKG}_${VERSION}_${ARCH}.deb"
