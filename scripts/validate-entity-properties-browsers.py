#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
document = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
document_h = (root / "src/linux_qt/app/MapDocumentWidget.hpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
scene = (root / "src/linux_qt/vmf/VmfScene.hpp").read_text()
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()

checks = {
    "visual selection corners": "selectionCorners" in scene and "hasSelectionCorners" in scene,
    "model-sized bounds": "model->minimum[0]" in document and "model->maximum[2]" in document,
    "rotated model bounds": "sourceTransform(entity.origin, entity.renderAngles())" in document and
                            "transform.transformPoint(local)" in document,
    "sprite-sized bounds": "material->image.width" in document and "material->image.height" in document and
                           "halfWidth" in document and "halfHeight" in document,
    "screen picking uses visual bounds": "entity.hasSelectionCorners" in view and
                                         "entity.selectionCorners" in view,
    "selection handles use visual bounds": "visualSelectionBounds()" in document and
                                           "visualSelectionBounds() const" in document_h,
    "mounted model browser": "Model Browser" in document and
                             'listFiles("models/", ".mdl")' in document and
                             "ModelPreviewWidget" in document,
    "all model property editors": document.count("makeModelPropertyEditor(smartPage") >= 2 and
                                  "PropertyType::Model" in document,
    "scrollable entity properties": "smartScroll->setWidget(smartPage)" in document and
                                    "spawnFlagsScroll->setWidget(spawnFlagsPage)" in document and
                                    'tabs->addTab(smartScroll, tr("SmartEdit"))' in document,
    "spawn flags tab": 'tabs->addTab(spawnFlagsScroll, tr("Spawn Flags"))' in document and
                       'key.compare(QStringLiteral("spawnflags")' in document,
    "mod-prefixed material paths": 'QStringLiteral("%1/materials/%2")' in main and
                                   "displayMaterialPath(materialName)" in main and
                                   "displayMaterialPath(name)" in main,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Entity/property/browser validation failed: " + ", ".join(failed))
print("Entity bounds, model browser, scrollable properties, spawn flags, and mod-prefixed material paths validated.")
