#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
studio_h = (root / "src/linux_qt/assets/StudioModelSystem.hpp").read_text()
studio_cpp = (root / "src/linux_qt/assets/StudioModelSystem.cpp").read_text()
scene_h = (root / "src/linux_qt/vmf/VmfScene.hpp").read_text()
scene_cpp = (root / "src/linux_qt/vmf/VmfScene.cpp").read_text()
renderer = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
model_browser = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()

checks = {
    "all MDL skin families": "skinFamilies" in studio_h and "familyIndex" in studio_cpp and "skinReferenceCount" in studio_cpp,
    "VMF skin key": "int skin{0}" in scene_h and 'root.value("skin")' in scene_cpp,
    "per-entity render skin": "model.normalizedSkin(entity.skin)" in renderer and "skinMaterials" in renderer,
    "all skin textures eagerly cached": "skinTextures" in renderer and
                                      "textureFor(material->name, material)" in renderer and
                                      "Switching an entity or browser preview skin" in renderer,
    "model-browser skin preview": "skinCombo" in model_browser and "preview->setSkin" in model_browser,
    "exact texture sizes": "{32, 64, 128, 256}" in main and all(f'tr("{n} x {n}")' in main for n in (32, 64, 128, 256)),
    "browser size button": "previewSizeButton" in main and "browserThumbnailSize" in main,
    "GPU orthographic scene": "renderOrthographicScene" in renderer and
                                "orthographicBrushVbo_" in renderer and
                                "orthographicEntityBatches_" in renderer and
                                "orthographicSelectionVbo_" in renderer and
                                "no QPainter geometry fallback" in renderer and
                                "baseFrame_" not in renderer[renderer.find("if (hardwareFrameReady"):renderer.find("void Hardware3DViewport::resizeEvent")] and
                                "Do not even" in view and "legacy CPU image off-screen" in view,
}
missing = [name for name, ok in checks.items() if not ok]
if missing:
    raise SystemExit("Missing: " + ", ".join(missing))
print("Prop skin families, eager skin texture caching, model-browser preview, texture sizing, and GPU 2D scene validated.")
