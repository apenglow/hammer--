#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hardware = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "version": "VERSION 0.15.0" in cmake,
    "reused framebuffer image": "QImage::Format_RGBX8888" in hardware and
        "glReadPixels(0, 0, pixelSize.width(), pixelSize.height()" in hardware and
        "framebuffer_->toImage()" not in hardware,
    "presentation flip only": "painter.scale(1.0, -1.0);" in hardware and
        "QPainter::CompositionMode_Source" in hardware,
    "prop backface culling": "Studio models use the same clockwise Source winding" in hardware and
        "glEnable(GL_CULL_FACE);" in hardware[hardware.find("void drawStudioModel"):hardware.find("void drawBillboardSprite")],
    "frustum rejection before model lookup": "entityVisibleInClip(entity, viewProjectionMatrix_)" in hardware and
        hardware.find("entityVisibleInClip(entity, viewProjectionMatrix_)") < hardware.find("studioModelFor(entity.model)"),
    "model lookup cache": "studioModelLookupCache_" in hardware and "studioModelFor(" in hardware,
    "cached projected geometry": all(token in hardware for token in (
        "projectedVbo_", "projectedVao_", "projectedBatches_",
        "Decals and overlays used to rebuild tangents", "GL_STATIC_DRAW")),
    "projected frustum culling": "boundsCorners(batch.minimum, batch.maximum)" in hardware,
    "texture records by reference": "const TextureRecord& textureFor(" in hardware and
        "const TextureRecord texture =" not in hardware,
    "no texture-unit teardown": "textures do not need to be" in hardware and
        "unbound from all eight units" in hardware,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Viewport optimization validation failed: " + ", ".join(failed))
print("Viewport readback, prop culling, projected-surface caching, and texture-state validation passed")
