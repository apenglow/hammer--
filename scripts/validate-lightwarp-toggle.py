#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_cpp = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
main_hpp = (root / "src/linux_qt/app/MainWindow.hpp").read_text()
doc_cpp = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
doc_hpp = (root / "src/linux_qt/app/MapDocumentWidget.hpp").read_text()
view_cpp = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
view_hpp = (root / "src/linux_qt/app/MapViewWidget.hpp").read_text()
hardware = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "version": "VERSION 0.15.0" in cmake,
    "menu command": "view.materialLightWarp" in main_cpp and 'tr("&Lightwarp")' in main_cpp,
    "menu action state": "QAction* lightWarpAction_{nullptr};" in main_hpp,
    "default enabled": all("bool lightWarpEnabled_{true};" in text for text in
                           (main_hpp, doc_hpp, view_hpp, hardware)),
    "persistent setting": 'render/materialLightWarp' in main_cpp,
    "toggle callback": "lightWarpAction_ && lightWarpAction_->isChecked()" in main_cpp and
                       "connect(lightWarpAction_, &QAction::toggled" in main_cpp,
    "main to document": "lightWarpEnabled_, selfIllumEnabled_, rimLightEnabled_" in main_cpp,
    "document to views": "lightWarp, selfIllum, rimLight" in doc_cpp and
                         "lightWarpEnabled_ = lightWarp;" in doc_cpp,
    "view state": "lightWarpEnabled_ == lightWarp" in view_cpp and
                  "lightWarpEnabled_ = lightWarp;" in view_cpp,
    "model browser state": doc_cpp.count("lightWarpEnabled, selfIllumEnabled, rimLightEnabled") >= 3 and
                           doc_cpp.count("lightWarpEnabled_, selfIllumEnabled_, rimLightEnabled_") == 2,
    "advanced mode gate": "advancedMaterialPreview && owner_->lightWarpEnabled_" in hardware,
    "renderer state": "lightWarpEnabled_ = lightWarpEnabled;" in hardware,
    "material gate": "const bool useLightWarp = usable && lightWarpEnabled_ &&" in hardware and
                     "material->hasLightWarpTexture" in hardware,
    "ordinary materials supported": "lightWarpEnabled_ && material->uberEffect" not in hardware,
    "shader uniform remains independent": hardware.count('setUniformValue("uUseLightWarp", useLightWarp ? 1 : 0)') == 1,
    "status reports toggle": "Lightwarp %4" in main_cpp,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    for name in failed:
        print(f"FAIL: {name}")
    raise SystemExit(1)

print("Independent persisted Lightwarp material toggle validation passed")
