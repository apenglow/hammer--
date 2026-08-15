#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
files = {
    "main": root / "src/linux_qt/app/main.cpp",
    "main_window_h": root / "src/linux_qt/app/MainWindow.hpp",
    "main_window_cpp": root / "src/linux_qt/app/MainWindow.cpp",
    "view_h": root / "src/linux_qt/app/MapViewWidget.hpp",
    "view_cpp": root / "src/linux_qt/app/MapViewWidget.cpp",
    "lock_h": root / "src/linux_qt/app/WaylandPointerLock.hpp",
    "lock_cpp": root / "src/linux_qt/app/WaylandPointerLock.cpp",
    "document": root / "src/linux_qt/app/MapDocumentWidget.cpp",
    "qrc": root / "src/linux_qt/hammer_resources.qrc",
    "cmake": root / "src/linux_qt/CMakeLists.txt",
    "icon": root / "src/linux_qt/resources/window_icon.png",
}
errors = []
texts = {}
for name, path in files.items():
    if not path.exists():
        errors.append(f"missing {path}")
    elif path.suffix != ".png":
        texts[name] = path.read_text(encoding="utf-8")

checks = {
    "dedicated window icon resource": "window_icon.png" in texts.get("qrc", "") and files["icon"].exists(),
    "multi-size native icon": "constexpr std::array<int, 7> sizes" in texts.get("main", ""),
    "flexible status minimum": "HammerStatusPaneLabel" in texts.get("main_window_cpp", "") and "minimumSizeHint" in texts.get("main_window_cpp", ""),
    "main resize hook": "normalizeResizableLayout" in texts.get("main_window_cpp", "") and "resizeEvent(QResizeEvent" in texts.get("main_window_h", ""),
    "shrinkable docks": "QLayout::SetNoConstraint" in texts.get("main_window_cpp", ""),
    "splitter stretch": texts.get("document", "").count("setStretchFactor") >= 6,
    "zcam crosshair": "drawZCameraCrosshair" in texts.get("view_h", "") and "mouseCaptured_" in texts.get("view_cpp", ""),
    "capture centers pointer": "centerCapturedPointer" in texts.get("view_cpp", ""),
    "Wayland cursor hint": "zwp_locked_pointer_v1_set_cursor_position_hint" in texts.get("lock_cpp", "") and "wl_surface_commit(surface_)" in texts.get("lock_cpp", "") and "centerCursor" in texts.get("lock_h", ""),
    "window icon install": "resources/window_icon.png" in texts.get("cmake", ""),
    "aligned MDI hammer button": "HammerMdiSystemButton" in texts.get("main_window_cpp", "") and "setCornerWidget" in texts.get("main_window_cpp", "") and "setFixedSize(buttonExtent, buttonExtent)" in texts.get("main_window_cpp", ""),
    "default MDI icon suppressed": "Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint" in texts.get("main_window_cpp", "") and "addSubWindow(document, documentFlags)" in texts.get("main_window_cpp", ""),
}
errors.extend(name for name, ok in checks.items() if not ok)
if errors:
    print("Window/Z-camera UI validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)
print("Window resizing, aligned MDI Hammer button, Z-camera centering, and crosshair validation passed.")
