#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text(errors="replace")
main_h = (root / "src/linux_qt/app/MainWindow.hpp").read_text(errors="replace")
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text(errors="replace")
view_h = (root / "src/linux_qt/app/MapViewWidget.hpp").read_text(errors="replace")
doc = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text(errors="replace")
hardware = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text(errors="replace")

markers = {
    "settings game tab": "Game Configurations",
    "directory chooser": "getExistingDirectory",
    "gameinfo validation": "gameinfo.txt",
    "persist directory": "game/gameDirectory",
    "persist renderer": "render/materials3d",
    "renderer checkbox": "Render materials in 3D views",
    "view action": "view.textured",
    "renderer setter": "setMaterialRenderingEnabled",
}
for label, marker in markers.items():
    if marker not in main and marker not in main_h and marker not in view and marker not in view_h and marker not in doc and marker not in hardware:
        raise SystemExit(f"missing {label}: {marker}")
if "materialRenderingEnabled_" not in view_h:
    raise SystemExit("MapViewWidget does not retain material-renderer state")
if "owner_->materialRenderingEnabled_" not in hardware or "materialsEnabled" not in hardware:
    raise SystemExit("3D material renderer is not gated by its setting")
print("game settings/material renderer structural validation passed")
