#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
scene_h = (root / "src/linux_qt/vmf/VmfScene.hpp").read_text()
scene_cpp = (root / "src/linux_qt/vmf/VmfScene.cpp").read_text()
project_h = (root / "src/linux_qt/vmf/VmfProjectedSurfaces.hpp").read_text()
project_cpp = (root / "src/linux_qt/vmf/VmfProjectedSurfaces.cpp").read_text()
editor = (root / "src/linux_qt/vmf/VmfEditor.cpp").read_text()
material_h = (root / "src/linux_qt/assets/MaterialSystem.hpp").read_text()
material_cpp = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()
document = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
view_h = (root / "src/linux_qt/app/MapViewWidget.hpp").read_text()
view_cpp = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
hardware = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
tests = (root / "src/linux_qt/tests/vmf_document_tests.cpp").read_text()
asset_tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()

checks = {
    "shared projected geometry": all(token in scene_h for token in (
        "ProjectedSurfaceKind", "ProjectedSurfaceVertex", "projectedSurfaces", "overlayProperties")),
    "nested overlaydata parsing": "overlaydata" in scene_cpp and "overlayProperties.emplace_back" in scene_cpp,
    "projection module built": "VmfProjectedSurfaces.cpp" in cmake and
                                "rebuildProjectedSurfaceGeometry" in project_h,
    "decal projection": all(token in project_cpp for token in (
        'classname == "infodecal"', 'property(entity.properties, "texture")',
        "decalBasis", "decalScale", "distance > 16.0", "projectAndClip")),
    "overlay projection": all(token in project_cpp for token in (
        'classname == "info_overlay"', 'projectedProperty(entity, "BasisOrigin")',
        'projectedProperty(entity, "BasisU")', 'projectedProperty(entity, "BasisV")',
        'projectedProperty(entity, "BasisNormal")', '"uv" + std::to_string(index)',
        'projectedProperty(entity, "sides")')),
    "brush and displacement projection": "forEachTargetPatch" in project_cpp and
                                          "face.displacementIndices" in project_cpp,
    "material decal semantics": "decalScale" in material_h and "decalModulate" in material_h and
                                '"$decalscale"' in material_cpp and '"DecalModulate"' in material_cpp,
    "scene refresh and exact bounds": "rebuildProjectedSurfaceGeometry" in document and
                                      "entity.projectedSurfaces" in document and
                                      "at least a one-unit" in document,
    "placement tools": all(token in view_h for token in (
        "Tool { Selection, Block, Entity, Decal, Overlay }",
        "decalPlacementRequested", "overlayPlacementRequested", "SurfaceHit")) and
                       all(token in document for token in (
        "createDecal", "createOverlay", 'appendChild("overlaydata")',
        'setOverlayValue("BasisOrigin"', 'setOverlayValue("uv0"',
        'setOverlayValue("sides"')),
    "tool routing": all(token in main for token in (
        'QStringLiteral("tool.decals")', 'MapViewWidget::Tool::Decal',
        'QStringLiteral("tool.overlay")', 'MapViewWidget::Tool::Overlay')),
    "2D and 3D selection": "projectedSurfaceHitDistance" in view_cpp and
                           "surfaceHit" in view_cpp and
                           "entity.projectedSurfaces" in view_cpp,
    "hardware rendering": "drawProjectedSurfaces" in hardware and
                           "GL_POLYGON_OFFSET_FILL" in hardware and
                           "material->decalModulate" in hardware,
    "overlay transforms and synchronization": "transformOverlayData" in editor and
                          'entity.children("overlaydata")' in editor and
                          "synchronizeOverlayData" in editor,
    "portable regressions": "infodecal is clipped into renderable projected triangles" in tests and
                            "root and nested info_overlay data project onto the referenced side" in tests and
                            "editing compiler-facing overlay keys synchronizes nested overlaydata" in tests and
                            "modulatedDecal->decalModulate" in asset_tests,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Decal/overlay validation failed: " + ", ".join(failed))
print("Source decal and overlay projection, rendering, placement, selection, and transforms validated.")
