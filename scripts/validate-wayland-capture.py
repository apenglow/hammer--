#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
paths = {
    "root_cmake": root / "CMakeLists.txt",
    "qt_cmake": root / "src/linux_qt/CMakeLists.txt",
    "lock_h": root / "src/linux_qt/app/WaylandPointerLock.hpp",
    "lock_cpp": root / "src/linux_qt/app/WaylandPointerLock.cpp",
    "view_h": root / "src/linux_qt/app/MapViewWidget.hpp",
    "view_cpp": root / "src/linux_qt/app/MapViewWidget.cpp",
    "build": root / "scripts/build-fedora.sh",
}
texts = {}
errors = []
for name, path in paths.items():
    if not path.exists():
        errors.append(f"missing {path}")
    else:
        texts[name] = path.read_text(encoding="utf-8")

checks = {
    "C language enabled for generated protocol code": "LANGUAGES C CXX" in texts.get("root_cmake", ""),
    "Wayland build option": "HAMMER_ENABLE_WAYLAND_CAPTURE" in texts.get("root_cmake", ""),
    "pointer constraints XML": "pointer-constraints-unstable-v1.xml" in texts.get("qt_cmake", ""),
    "relative pointer XML": "relative-pointer-unstable-v1.xml" in texts.get("qt_cmake", ""),
    "protocol generation": "wayland-scanner" in texts.get("qt_cmake", "").lower(),
    "public Qt 6.9 surface path": "HAMMER_WAYLAND_SURFACE_FROM_WINID" in texts.get("lock_cpp", ""),
    "persistent pointer lock": "ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT" in texts.get("lock_cpp", ""),
    "relative motion listener": "zwp_relative_pointer_v1_add_listener" in texts.get("lock_cpp", ""),
    "unaccelerated delta": "dxUnaccelerated" in texts.get("lock_cpp", "") and "dyUnaccelerated" in texts.get("lock_cpp", ""),
    "locked confirmation": "pointerLocked" in texts.get("lock_cpp", "") and "!locked_" in texts.get("lock_cpp", ""),
    "surface-local region": "mapTo(topLevel" in texts.get("lock_cpp", ""),
    "center cursor hint": "zwp_locked_pointer_v1_set_cursor_position_hint" in texts.get("lock_cpp", ""),
    "cursor hint commit": "wl_surface_commit(surface_)" in texts.get("lock_cpp", ""),
    "double motion suppression": "if (!waylandPointerCapture_) updateCapturedMouse(event);" in texts.get("view_cpp", ""),
    "fallback grab": "handleWaylandPointerLockUnavailable" in texts.get("view_cpp", "") and "beginFallbackMouseCapture" in texts.get("view_cpp", ""),
    "region refresh": "waylandPointerLock_->updateRegion(this)" in texts.get("view_cpp", ""),
    "Fedora dependency check": "wayland-protocols-devel" in texts.get("build", ""),
}
errors.extend(name for name, ok in checks.items() if not ok)
if errors:
    print("Wayland capture validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)
print("Native Wayland Z-camera capture validation passed.")
