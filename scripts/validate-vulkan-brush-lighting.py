#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
shader = (root / "src/linux_qt/shaders/raytraced_preview.comp").read_text()
checks = {
    "geometric hit normal": "vec3 geometricNormal;" in shader,
    "normal independent of frontface bit": "dot(result.normal, result.geometricNormal) < 0.0" in shader,
    "no frontface-driven shading flip": "if (!result.frontFace)" not in shader,
    "geometric secondary offset": "vec3 surfaceRayOrigin(HitRecord hit, vec3 rayDirection)" in shader,
    "direct light uses safe origin": "lightTransmission(surfaceRayOrigin(hit, selectedDirections[slot])" in shader,
    "secondary direct uses safe origin": "lightTransmission(surfaceRayOrigin(hit, selectedDirections[slot])" in shader,
    "ao uses safe origin": "lightTransmission(surfaceRayOrigin(hit, aoDirection)" in shader,
    "sky ambient uses safe origin": "lightTransmission(surfaceRayOrigin(hit, skyProbeDirection)" in shader,
}
failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(f"{name}: {'ok' if ok else 'FAILED'}")
if failed:
    raise SystemExit("validation failed: " + ", ".join(failed))
print("Vulkan RT brush-lighting validation passed")
