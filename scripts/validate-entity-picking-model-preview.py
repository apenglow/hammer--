#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
view_h = (root / "src/linux_qt/app/MapViewWidget.hpp").read_text()
document = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()

checks = {
    "sprite renderer-sized bounds":
        "spriteScreenBounds" in view_h and
        "Match Hardware3DViewport::drawBillboardSprite exactly" in view and
        "camera::rightVector(cameraState_)" in view and
        "camera::upVector(cameraState_)" in view,
    "sprite drawing shares bounds":
        "if (const auto target = spriteScreenBounds(entity))" in view and
        "painter.drawImage(*target" in view,
    "triangle prop picking":
        "rayTriangleDistance" in view and "modelHitDistance" in view and
        "world(mesh.vertices[index])" in view,
    "prop picking broad phase":
        "Cheap local-space slab test" in view and
        "model->minimum[0]" in view and "model->maximum[2]" in view,
    "nearest rendered entity":
        "nearestEntity" in view and "nearestDistance" in view and
        "projectedSurfaceHitDistance" in view,
    "mesh-projected selection bounds":
        "Project actual model vertices when possible" in view and
        "for (const auto& vertex : mesh.vertices)" in view,
    "browser uses actual map viewport":
        "new MapViewWidget(MapViewWidget::Kind::Perspective" in document and
        "viewport_->setMaterialSystem(materials_)" in document and
        "viewport_->setTexturedRenderMode(texturedRenderMode)" in document and
        "viewport_->setScene(scene_, true)" in document,
    "old wireframe preview removed":
        "Stable Hammer-style isometric preview" not in document and
        "MaxPreviewTriangles" not in document,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Entity picking/model preview validation failed: " + ", ".join(failed))
print("Sprite bounds, prop picking, and hardware model-browser preview validated.")
