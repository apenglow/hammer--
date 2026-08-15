#!/usr/bin/env bash
# Package a PORTABLE Hammer-- AppImage by running scripts/package-appimage.sh
# inside an ubuntu:22.04 container (glibc 2.35 floor + official Qt 6.5.3).
# This is the packaging path for builds that leave this machine; the bare
# script bakes in the host's glibc and the result only runs on distros at
# least that new.
#
# Usage: scripts/package-appimage-container.sh
#   Output: dist/Hammer--<version>-x86_64.AppImage (overwrites)
#   Uses its own build tree (build-container/) so the host build/ cache is
#   never mixed with the container's compiler and Qt.
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=hammer-appimage-build

if ! podman image exists "$IMAGE"; then
    echo "building container image $IMAGE (one-time, downloads Qt 6.5.3)..."
    podman build -t "$IMAGE" -f "$REPO_ROOT/scripts/Containerfile.appimage" "$REPO_ROOT/scripts"
fi

# :Z relabels the mount for SELinux; --userns=keep-id keeps the outputs owned
# by the invoking user instead of root.
podman run --rm \
    --userns=keep-id \
    -v "$REPO_ROOT:/repo:Z" \
    -w /repo \
    "$IMAGE" \
    bash /repo/scripts/package-appimage.sh /repo/build-container /repo/dist

# The whole point: refuse to ship a binary whose glibc floor crept past the
# container's 2.35 (or that references the 2.42+ TLS ABI marker at all).
APPDIR="$REPO_ROOT/build-container/AppDir"
# "|| true": objdump exits non-zero on the non-ELF files find feeds it, which
# would silently kill the script under set -e/pipefail before the checks run.
FLOOR="$( { find "$APPDIR" \( -name '*.so*' -o -path '*/bin/*' \) -type f \
    -exec objdump -T {} + 2>/dev/null || true; } | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)"
echo "bundled glibc floor: ${FLOOR:-unknown}"
if { find "$APPDIR" -type f \( -name '*.so*' -o -path '*/bin/*' \) \
    -exec objdump -T {} + 2>/dev/null || true; } | grep -q GLIBC_ABI_GNU2_TLS; then
    echo "error: GLIBC_ABI_GNU2_TLS leaked into the bundle — not portable" >&2
    exit 1
fi
case "${FLOOR#GLIBC_}" in
    2.3[0-5]|2.[12]?|2.[0-9]) ;;
    *) echo "error: glibc floor $FLOOR is newer than ubuntu 22.04's 2.35" >&2; exit 1 ;;
esac
echo "portable AppImage OK"
