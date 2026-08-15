#!/usr/bin/env bash
# Cross-compile Hammer-- for 64-bit Windows from Linux and pack a ready-to-run
# zip. Runs inside the hammer-windows-build container (Fedora MinGW Qt 6);
# build scripts/Containerfile.windows once first (this script does it if the
# image is missing).
#
# Usage: scripts/package-windows.sh
#   Output: dist/Hammer--<version>-win64.zip
#   Uses its own build tree (build-windows/).
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=hammer-windows-build

if ! podman image exists "$IMAGE"; then
    echo "building container image $IMAGE (one-time)..."
    podman build -t "$IMAGE" -f "$REPO_ROOT/scripts/Containerfile.windows" "$REPO_ROOT/scripts"
fi

podman run --rm --userns=keep-id -v "$REPO_ROOT:/repo:Z" -w /repo "$IMAGE" bash -euo pipefail -c '
    VERSION="$(sed -n "s/^project(.*VERSION \([0-9.]*\).*/\1/p" CMakeLists.txt | head -1)"
    SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw

    # mingw64-cmake wraps cmake with the Fedora MinGW toolchain file.
    # build.ninja, not CMakeCache.txt: a failed configure leaves a cache
    # behind, and skipping configure then would leave nothing to build.
    if [ ! -f build-windows/build.ninja ]; then
        mingw64-cmake -S . -B build-windows -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DHAMMER_BUILD_TESTS=OFF \
            -DHAMMER_ENABLE_WAYLAND_CAPTURE=OFF \
            -DHAMMER_ENABLE_VULKAN_RAY_TRACING=OFF
    fi
    cmake --build build-windows --target hammer-qt6 hammer-vmf-tool -j"$(nproc)"

    # --- stage ---------------------------------------------------------------
    # The exe resolves its resources as <exedir>/../share/hammerminusminus,
    # so the zip mirrors the Linux layout: bin/ beside share/.
    STAGE=build-windows/stage/Hammer--
    BIN="$STAGE/bin"
    rm -rf build-windows/stage
    mkdir -p "$BIN"
    cp build-windows/src/linux_qt/hammerminusminus.exe "$BIN/"
    cp build-windows/src/linux_qt/hammer-vmf-tool.exe "$BIN/" 2>/dev/null || true

    # Qt runtime plugins first, so the DLL closure below covers their
    # dependencies too (the TLS plugin drags in the OpenSSL DLLs).
    for plugin in platforms styles imageformats tls; do
        if [ -d "$SYSROOT/lib/qt6/plugins/$plugin" ]; then
            mkdir -p "$BIN/plugins/$plugin"
            cp "$SYSROOT/lib/qt6/plugins/$plugin/"*.dll "$BIN/plugins/$plugin/"
        fi
    done
    cat > "$BIN/qt.conf" <<EOF
[Paths]
Plugins = plugins
EOF

    # Runtime DLL closure: walk import tables of the exes and every staged
    # plugin, resolving only DLLs that live in the MinGW sysroot (system DLLs
    # like kernel32 stay unresolved).
    resolve() {
        x86_64-w64-mingw32-objdump -p "$1" 2>/dev/null |
            sed -n "s/^\tDLL Name: //p"
    }
    declare -A staged
    queue=("$BIN/hammerminusminus.exe")
    [ -f "$BIN/hammer-vmf-tool.exe" ] && queue+=("$BIN/hammer-vmf-tool.exe")
    while IFS= read -r plugindll; do
        queue+=("$plugindll")
    done < <(find "$BIN/plugins" -name "*.dll" 2>/dev/null)
    while [ "${#queue[@]}" -gt 0 ]; do
        current="${queue[0]}"; queue=("${queue[@]:1}")
        while IFS= read -r dll; do
            [ -n "$dll" ] || continue
            [ -n "${staged[$dll]:-}" ] && continue
            src="$SYSROOT/bin/$dll"
            if [ -f "$src" ]; then
                staged[$dll]=1
                cp "$src" "$BIN/"
                queue+=("$BIN/$dll")
            fi
        done < <(resolve "$current")
    done

    # App resources that the Linux install tree ships in share/.
    DESTDIR="$PWD/build-windows/instroot" cmake --install build-windows/src/linux_qt --prefix /
    if [ -d build-windows/instroot/share/hammerminusminus ]; then
        mkdir -p "$STAGE/share"
        cp -r build-windows/instroot/share/hammerminusminus "$STAGE/share/"
    fi

    mkdir -p dist
    rm -f "dist/Hammer--$VERSION-win64.zip"
    (cd build-windows/stage && zip -qr "/repo/dist/Hammer--$VERSION-win64.zip" Hammer--)
    echo "staged DLLs: ${!staged[*]}"
    echo "Windows zip written to dist/Hammer--$VERSION-win64.zip"
'
