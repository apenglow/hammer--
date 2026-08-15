from pathlib import Path

root = Path(__file__).resolve().parents[1]
scene = (root / "src/linux_qt/app/RayTracingScene.cpp").read_text()
shader = (root / "src/linux_qt/shaders/raytraced_preview.comp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
viewport = (root / "src/linux_qt/app/VulkanRayTracedViewport.cpp").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "version bump": "VERSION 0.15.26" in cmake,
    "nodraw casts shadows": 'normalizedMaterial == "tools/toolsnodraw"' in scene,
    "noclip no longer special": 'normalizedMaterial == "tools/toolsnoclip"' not in scene,
    "other tools no-shadow": "shadowFlagsForMaterial" in scene and "TriangleNoShadow" in scene,
    "hidden tools primary-only": "TriangleNoPrimary" in scene and "TRIANGLE_NO_PRIMARY" in shader,
    "hidden nodraw kept for shadows": "same shadow it would cast in the compiled Source map" in scene,
    "tool menu invalidates RT flags": "rayTracedViewport_->invalidateGeometryCache()" in view,
    "Source attenuation scale": "q * 10000.0f + l * 100.0f + c" in scene,
    "Source light direction": 'propertyNumber(entity, "pitch", 0.0)' in scene and "entity.angles.y" in scene,
    "unbounded standard lights": 'propertyNumber(entity,"_distance",0.0)' in scene and "light.directionRange.w>0.0" in shader,
    "two-sided shadow rays": "closestLightBoundary" in shader and
        "rayQueryGetIntersectionFrontFaceEXT(query, false)" not in shader[shader.index("LightBoundary closestLightBoundary"):shader.index("float lightTransmission")],
    "linear albedo": "base.rgb = sourceGammaToLinear" in shader,
    "integrated environment ambient": "return ambient * 0.25" in shader,
    "Source display transform": "color = sourceLinearToGamma(color);" in shader and "2.51 * color + 0.03" not in shader,
    "sky portal triangle flag": "TriangleSky" in scene and "TRIANGLE_SKY" in shader,
    "environment light is sky-gated": "environment?4.0f" in scene and "requireSkyPortal=(type==4)" in shader,
    "sky-gated ambient": "environmentTransmission" in shader and "131072.0, true" in shader,
    "stable-frame RGBA channel order": "rgbaChannelDistance" in viewport and
        "image.format() != QImage::Format_RGBA8888" in viewport and
        "reinterpret_cast<QRgb*>" not in viewport,
}

failed = [name for name, passed in checks.items() if not passed]
for name, passed in checks.items():
    print(f"{name}: {'ok' if passed else 'FAILED'}")
if failed:
    raise SystemExit("Validation failed: " + ", ".join(failed))
print("Vulkan RT lighting/tool visibility validation passed")
