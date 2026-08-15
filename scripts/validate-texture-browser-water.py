#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
material_h = (root / "src/linux_qt/assets/MaterialSystem.hpp").read_text()
material_cpp = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()
main_cpp = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
hardware_cpp = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()

checks = {
    "water material flag": "bool water{false};" in material_h,
    "animated preview flag": "bool previewAnimated{false};" in material_h,
    "water shader recognition": "isWaterShader" in material_cpp,
    "water normal map": '"$normalmap"' in material_cpp,
    "water procedural preview": "makeWaterPreview" in material_cpp,
    "browser excludes models": 'name.rfind("models/", 0) == 0' in material_cpp,
    "browser excludes vgui": 'name.rfind("vgui/", 0) == 0' in material_cpp,
    "browser excludes backpack": 'name.rfind("backpack/", 0) == 0' in material_cpp,
    "water normal image": "waterNormalImage" in material_h and "makeProceduralWaterNormal" in material_cpp,
    "water flow image": "waterFlowImage" in material_h and '"$flowmap"' in material_cpp,
    "flow map kept separate": 'normalName = nestedValueCi(root, "$flowmap")' not in material_cpp,
    "flow texture sampler": "uWaterFlowMap" in hardware_cpp and "GL_TEXTURE1" in hardware_cpp,
    "flow red direction": "1.0 - encodedFlow.r * 2.0" in hardware_cpp,
    "flow green direction": "encodedFlow.g * 2.0 - 1.0" in hardware_cpp,
    "two-phase flow": "phaseA" in hardware_cpp and "phaseB" in hardware_cpp and "phaseBlend" in hardware_cpp,
    "flow-aware browser preview": "flowMap->sampleWrapped" in main_cpp and "flowX" in main_cpp and "flowY" in main_cpp,
    "single-layer skybox water":
        hardware_cpp.count("environmentDirection = normalize(") == 4 and
        hardware_cpp.count("refractedDirection = refract(") == 4 and
        hardware_cpp.count("if (uHasEnvironmentMap == 0)") == 4 and
        "environmentRefraction = uHasEnvironmentMap" not in hardware_cpp and
        "refractedColor" not in hardware_cpp and
        "fakeReflection" not in hardware_cpp,
    "Source water controls": all(token in material_cpp for token in (
        '"$reflectamount"', '"$refractamount"', '"$reflectblendfactor"',
        '"$nofresnel"', '"$scroll1"', '"$scroll2"', '"$fogstart"', '"$fogend"')),
    "Source multi-layer normals": hardware_cpp.count('rotated45') == 8 and
                                  hardware_cpp.count('rotated90') == 8 and
                                  hardware_cpp.count('detailOffsetA') == 8,
    "water fog is not fabricated from view distance":
        'length(uCameraPosition - vWorldPosition) - uWaterFogStart' not in hardware_cpp and
        'fogTintWeight' not in hardware_cpp,
    "independent Source distortion amounts":
        hardware_cpp.count('reflectionDistortion = clamp(abs(uWaterReflectAmount)') == 4 and
        hardware_cpp.count('refractionDistortion = clamp(abs(uWaterRefractAmount)') == 4,
    "refractive single-ray bend":
        hardware_cpp.count('relativeIor = interfaceSide > 0.0') == 4 and
        hardware_cpp.count('visibleSkyHemisphere = geometricNormal * interfaceSide') == 4 and
        hardware_cpp.count('refractiveBend = clamp(') == 4 and
        hardware_cpp.count('baseUv, -2.0') == 4 and
        'stableFresnel' not in hardware_cpp and 'reflectionRipple' not in hardware_cpp,
    "water owns depth and is not blended":
        'const bool translucent = texture.translucent && !water;' in hardware_cpp and
        'return vec4(clamp(color, 0.0, 1.0), 1.0);' in hardware_cpp,
    "tangent-space water normals":
        hardware_cpp.count('reflectionSurfaceNormal = normalize(') == 4 and
        hardware_cpp.count('refractionSurfaceNormal = normalize(') == 4,
    "bounded animation phases": "wrappedUnitPhase" in hardware_cpp and "uTime" not in hardware_cpp,
    "adaptive water updates": "desiredInterval" in hardware_cpp and "? 16 : 33" in hardware_cpp,
    "compile trigger material flag": "compileTrigger" in material_h and '"%compiletrigger"' in material_cpp,
    "visual icon mode": "QListView::IconMode" in main_cpp,
    "thumbnail size setting": 'textures/browserThumbnailSize' in main_cpp,
    "animate setting": 'textures/animatePreviews' in main_cpp,
    "batched thumbnails": "batchSize = 6" in main_cpp,
    "visible thumbnail priority": "rebuildThumbnailPriority" in main_cpp and "visualItemRect" in main_cpp and "onScreen ? 0 : 1" in main_cpp,
    "scroll reprioritization": "requestThumbnailReprioritization" in main_cpp and "QScrollBar::valueChanged" in main_cpp,
    "resize reprioritization": "TextureBrowserViewportWatcher" in main_cpp and "QEvent::Resize" in main_cpp,
    "loaded thumbnail tracking": "ThumbnailLoadedRole" in main_cpp and "loadedThumbnailCount" in main_cpp,
    "water regression": 'nature/water_coast' in tests,
    "flow water regression": 'nature/water_flowing' in tests and 'waterHasFlowMap' in tests,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("validation failed: " + ", ".join(failed))
print("texture browser and water validation passed")
