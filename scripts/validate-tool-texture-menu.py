#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
document = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
gpu = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
scene = (root / "src/linux_qt/vmf/VmfScene.cpp").read_text()

required = {
    "View hover submenu": 'view->addMenu(tr("Tool &Textures"))' in main,
    "submenu refreshes on hover": "QMenu::aboutToShow" in main and "rebuildToolTexturesMenu" in main,
    "master show-all toggle": 'tr("Show All Tool Textures")' in main and "setAllToolTexturesVisible" in main,
    "per-material checkboxes": "setToolTextureVisible(material, visible)" in main,
    "scene discovers tools materials": "toolMaterialPaths" in scene and 'rfind("tools/", 0)' in scene,
    "document stores hidden materials": "hiddenToolTextures_" in document and "applyToolTextureVisibility" in document,
    "views receive visibility state": "setHiddenToolTextures" in view,
    "GPU skips hidden tool faces": "hiddenToolTextures.find(normalizedFaceMaterial)" in gpu,
}
failed = [name for name, ok in required.items() if not ok]
if failed:
    raise SystemExit("Tool texture menu validation failed: " + ", ".join(failed))
print("Tool texture visibility menu validation passed")
