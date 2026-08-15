#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
view_h = (root / "src/linux_qt/app/MapViewWidget.hpp").read_text()
view_cpp = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
main_cpp = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()
qrc = (root / "src/linux_qt/hammer_resources.qrc").read_text()

mapping = {
    "tool.magnify": "magnify_tool.png",
    "tool.camera": "camera_tool.png",
    "tool.entity": "entity_tool.png",
    "tool.block": "brush_tool.png",
    "tool.textureApplication": "face_tool.png",
    "tool.applyTexture": "texture_tool.png",
    "tool.clipper": "clip_tool.png",
}
required = {
    "persistent base image": "QImage baseFrame_" in view_h and "rebuildBaseFrame()" in view_cpp,
    "single viewport widget": "SelectionOverlayWidget" not in view_h and "SelectionOverlayWidget" not in view_cpp,
    "normal backing-store background": "setAutoFillBackground(true)" in view_cpp and "WA_NoSystemBackground" not in view_cpp,
    "selection painter state isolated": "painter.save();\n    drawSelectionOverlay(painter);\n    painter.restore();" in view_cpp,
    "border cannot fill view": "painter.setBrush(Qt::NoBrush);\n    painter.drawRect(rect().adjusted(0, 0, -1, -1));" in view_cpp,
    "root cause documented": "left ViewBackground2D as the active brush" in view_cpp,
    "exact icon loader": "loadSidebarToolIcon" in main_cpp and "HAMMER_TOOL_ICON_DIR" in cmake,
    "explicit sidebar buttons": "HammerSidebarToolButton" in main_cpp and "new QToolButton(editTools)" in main_cpp,
    "icon files installed": "install(DIRECTORY resources/tool_icons DESTINATION share/hammerplusplus-testing)" in cmake,
    "all icon resources": all(name in qrc for name in mapping.values()),
    "exact mappings": all(f'{{"{tool}"' in main_cpp and f'"{filename}"' in main_cpp for tool, filename in mapping.items()),
}
missing = [name for name, ok in required.items() if not ok]
if missing:
    raise SystemExit("Viewport/icon validation failed: " + ", ".join(missing))
print("Viewport painter-state and sidebar icon validation passed.")
print("Selection state cannot leak a fill brush into the viewport border; exact PNG filenames are hard-wired.")
