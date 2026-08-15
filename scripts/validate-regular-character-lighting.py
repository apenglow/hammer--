#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hardware = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
material = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()

required = [
    "material.uberEffect = isTf2InvulnerabilityMaterial(materialName);",
    "const bool useLightWarp = usable && lightWarpEnabled_ &&",
    "material->hasLightWarpTexture &&",
    "texture.hasLightWarp && texture.lightWarpId;",
    "variables[target.name] = {};",
    "else invalidateResult(target);",
    "assert(!heavyPlayerMaterial->color2Active",
]
missing = [token for token in required if token not in material + hardware + tests]
if missing:
    raise SystemExit("missing regular-character lighting safeguard: " + repr(missing))

for forbidden in (
    "material.invulnLevelProxy || modelGlowInvulnerability",
    "lightWarpEnabled_ && material->uberEffect",
    "const bool useLightWarp = usable && material->highEnergyEffect &&",
):
    if forbidden in material + hardware:
        raise SystemExit("regressed character path remains: " + forbidden)

print("Regular character proxy fallback and independent Lightwarp toggle passed")
