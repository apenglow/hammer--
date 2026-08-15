#!/usr/bin/env bash
# Package hammer-- as an AppImage.
#
# Builds (or reuses) the Qt6 editor, stages the CMake install tree into an
# AppDir, and runs linuxdeploy + its Qt plugin to bundle Qt and all non-system
# shared libraries. The linuxdeploy tools are downloaded once into
# scripts/.appimage-tools/.
#
# Usage: scripts/package-appimage.sh [build-dir] [output-dir]
#   build-dir   CMake build tree (default: <repo>/build)
#   output-dir  where the .AppImage lands (default: <repo>/dist)
#
# Note: an AppImage does not bundle glibc — the result runs on distros with a
# glibc at least as new as this machine's. For builds that leave this machine,
# use scripts/package-appimage-container.sh instead: it runs this script inside
# an ubuntu:22.04 container (glibc 2.35 floor, official Qt 6.5.3) and verifies
# the bundle's glibc floor before declaring success.
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
OUTPUT_DIR="${2:-$REPO_ROOT/dist}"
TOOLS_DIR="$REPO_ROOT/scripts/.appimage-tools"
APPDIR="$BUILD_DIR/AppDir"

# Version for the AppImage filename, from the top-level project() line.
VERSION="$(sed -n 's/^project(.*VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -1)"
export VERSION="${VERSION:-0.0.0}"

# --- build -------------------------------------------------------------------
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi
# hammer-vmf-tool ships in the install tree alongside the editor.
cmake --build "$BUILD_DIR" --target hammer-qt6 hammer-vmf-tool -j"$(nproc)"

# --- stage the install tree into the AppDir ----------------------------------
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR/src/linux_qt" --prefix /usr

DESKTOP_FILE="$APPDIR/usr/share/applications/hammerminusminus.desktop"
ICON_FILE="$APPDIR/usr/share/icons/hicolor/256x256/apps/hammerminusminus.png"
for staged in "$DESKTOP_FILE" "$ICON_FILE" "$APPDIR/usr/bin/hammerminusminus"; do
    [ -e "$staged" ] || { echo "error: expected install output missing: $staged" >&2; exit 1; }
done

# The source logo is 1024x1024, but it installs into the 256x256 hicolor slot
# and appimagetool rejects the mismatch — resize the staged copy in place.
if command -v magick >/dev/null; then
    magick "$ICON_FILE" -resize 256x256 "$ICON_FILE"
elif command -v convert >/dev/null; then  # ImageMagick 6 (ubuntu container)
    convert "$ICON_FILE" -resize 256x256 "$ICON_FILE"
else
    echo "error: ImageMagick not found; cannot resize icon and linuxdeploy will reject it" >&2
    exit 1
fi

# --- fetch linuxdeploy (once) ------------------------------------------------
mkdir -p "$TOOLS_DIR"
fetch() {
    local url="$1" target="$TOOLS_DIR/$2"
    if [ ! -x "$target" ]; then
        echo "downloading $2 ..."
        curl -fL --retry 3 -o "$target" "$url"
        chmod +x "$target"
    fi
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
      linuxdeploy-x86_64.AppImage
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
      linuxdeploy-plugin-qt-x86_64.AppImage
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
      appimagetool-x86_64.AppImage

# --- bundle ------------------------------------------------------------------
QMAKE_BIN="$(command -v qmake6 || command -v qmake-qt6 || command -v qmake)" \
    || { echo "error: qmake6 not found in PATH" >&2; exit 1; }
export QMAKE="$QMAKE_BIN"
# Lets the tools run on kernels/setups without FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
# linuxdeploy's bundled strip predates the .relr.dyn sections modern binutils
# emits and errors out on them; skip stripping (costs some size, breaks nothing).
export NO_STRIP=1
# Bundle wayland + xcb so one AppImage covers either session type. The
# wayland plugin's filename varies across Qt builds; probe what this one has.
QT_PLUGIN_ROOT="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
EXTRA_PLATFORM_PLUGINS="libqxcb.so"
for candidate in libqwayland.so libqwayland-generic.so libqwayland-egl.so; do
    [ -f "$QT_PLUGIN_ROOT/platforms/$candidate" ] &&
        EXTRA_PLATFORM_PLUGINS="$EXTRA_PLATFORM_PLUGINS;$candidate"
done
export EXTRA_PLATFORM_PLUGINS

# Populate the AppDir (libraries, Qt plugins, root .desktop/.DirIcon). The
# AppImage itself is packed separately below, after the library repair.
"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --desktop-file "$DESKTOP_FILE" \
    --icon-file "$ICON_FILE" \
    --plugin qt

# --- repair patchelf damage --------------------------------------------------
# linuxdeploy rewrites every bundled library's rpath with its bundled patchelf,
# which is too old for the RELR relocations modern distro builds use and
# corrupts them (segfault in _init on dlopen). Overwrite each bundled copy
# with the pristine system library; the AppRun wrapper below supplies the
# search path via LD_LIBRARY_PATH, so no rpath is needed at all.
declare -A SYSTEM_LIB
while IFS= read -r line; do
    # ldconfig -p: "\tlibfoo.so.1 (libc6,x86-64) => /usr/lib64/libfoo.so.1"
    name="${line%% (*}"; name="${name#"${name%%[![:space:]]*}"}"
    path="${line##*=> }"
    [[ "$line" == *x86-64* ]] && SYSTEM_LIB["$name"]="$path"
done < <(ldconfig -p)
repaired=0
while IFS= read -r bundled; do
    base="$(basename "$bundled")"
    source_path="${SYSTEM_LIB[$base]:-}"
    if [ -z "$source_path" ]; then
        # Qt plugins are not in the ldconfig cache; mirror their path under
        # the Qt plugin root instead.
        relative="${bundled#"$APPDIR"/usr/plugins/}"
        [ "$relative" != "$bundled" ] && [ -f "$QT_PLUGIN_ROOT/$relative" ] &&
            source_path="$QT_PLUGIN_ROOT/$relative"
    fi
    if [ -n "$source_path" ] && [ -f "$source_path" ]; then
        cp --remove-destination "$(readlink -f "$source_path")" "$bundled"
        repaired=$((repaired + 1))
    fi
done < <(find "$APPDIR/usr/lib" "$APPDIR/usr/plugins" -name '*.so*' -type f 2>/dev/null)
echo "restored $repaired pristine libraries over patchelf-modified copies"

# linuxdeploy-plugin-qt skips the wayland-graphics-integration-client plugins
# entirely; without them Qt on Wayland has no EGL hardware integration and
# every GL viewport silently degrades/breaks. Copy the pristine set (its
# library deps are either already bundled or deliberately host-side: EGL,
# OpenGL, wayland-egl are driver-tied).
if [ -d "$QT_PLUGIN_ROOT/wayland-graphics-integration-client" ]; then
    mkdir -p "$APPDIR/usr/plugins/wayland-graphics-integration-client"
    cp "$QT_PLUGIN_ROOT/wayland-graphics-integration-client/"*.so \
       "$APPDIR/usr/plugins/wayland-graphics-integration-client/"
fi

# AppRun wrapper: the pristine libraries carry no \$ORIGIN rpath, so the
# bundled tree is put on LD_LIBRARY_PATH here instead.
rm -f "$APPDIR/AppRun"
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/hammerminusminus" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# Sanity: every top-level bundled library must dlopen cleanly.
LD_LIBRARY_PATH="$APPDIR/usr/lib" python3 - "$APPDIR/usr/lib" <<'PY'
import ctypes, glob, sys
bad = []
for lib in sorted(glob.glob(sys.argv[1] + "/*.so*")):
    try:
        ctypes.CDLL(lib)
    except OSError as error:
        bad.append(f"{lib}: {error}")
if bad:
    print("\n".join(bad), file=sys.stderr)
    sys.exit("error: bundled libraries fail to load")
PY

# --- pack --------------------------------------------------------------------
mkdir -p "$OUTPUT_DIR"
# Pack to a temp name and rename into place: overwriting the AppImage in
# place fails with "Text file busy" whenever an instance of it is running.
ARCH=x86_64 "$TOOLS_DIR/appimagetool-x86_64.AppImage" "$APPDIR" \
    "$OUTPUT_DIR/.Hammer--$VERSION-x86_64.AppImage.new"
mv -f "$OUTPUT_DIR/.Hammer--$VERSION-x86_64.AppImage.new" \
      "$OUTPUT_DIR/Hammer--$VERSION-x86_64.AppImage"

echo
echo "AppImage written to:"
ls -1 "$OUTPUT_DIR/Hammer--$VERSION-x86_64.AppImage"
