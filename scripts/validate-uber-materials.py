#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
material_h = (root / "src/linux_qt/assets/MaterialSystem.hpp").read_text()
material_cpp = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()
renderer = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()

for field in ["color2", "color2Active", "modelGlowProxy", "itemTintProxy",
              "invulnLevelProxy", "uberEffect", "highEnergyEffect",
              "selfIllumFresnel", "selfIllumFresnelMinMaxExp",
              "bumpFrames", "bumpAnimationFrameRate", "lightWarpTexture",
              "lightWarpImage", "hasLightWarpTexture", "halfLambert"]:
    assert field in material_h, field

for token in ["modelglowcolor", "itemtintcolor", "invulnlevel",
              "selectfirstifnonzero", "lessorequal", "type == \"sine\"",
              "type == \"equals\"", "type == \"multiply\"",
              "animatedtexture", "proxyTarget", "target.component",
              "$selfillumfresnel", "$selfillumfresnelminmaxexp",
              "$lightwarptexture", "$halflambert", "decodeVtfFrames"]:
    assert token in material_cpp, token

for token, count in [
    ("uniform vec3 uColor2;", 4),
    ("uniform int uHighEnergyEffect;", 4),
    ("uniform int uUseSelfIllumFresnel;", 4),
    ("uniform vec3 uSelfIllumFresnelMinMaxExp;", 4),
    ("uniform sampler2D uLightWarpTexture;", 4),
    ("uniform int uUseLightWarp;", 4),
    ("uniform int uHalfLambert;", 4),
    ("if (uUseSelfIllumFresnel != 0)", 4),
    ("selfIllumMask = vec3(baseAlphaMask * selfIllumFresnel);", 4),
    ("selfIllumColor *= max(selfIllumMaximum, 0.0);", 4),
    ("if (uUseLightWarp != 0)", 4),
    ("halfLambertDiffuse *= halfLambertDiffuse;", 4),
    ("if (uHighEnergyEffect == 0)", 4),
    ("linearTexture += materialHighlight;", 4),
]:
    assert renderer.count(token) == count, (token, renderer.count(token))

assert "uniform int uUberEffect;" not in renderer

# The old global shell override caused the texture/white-material regressions.
for forbidden in [
    "if (uUberEffect != 0)",
    "linearAlbedo = linearColor2;",
    "selfIllumMask = vec3(1.0);",
    "materialHighlight *= mix(vec3(1.0), linearColor2, 0.72);",
    "max(selfIllumMask, vec3(0.82))",
    "max(uSelfIllumTint, vec3(1.0))",
]:
    assert forbidden not in renderer, forbidden

for token in ["invulnfx_stock_red", "ModelGlowColor", "AnimatedTexture",
              "InvulnLevel", "LessOrEqual", "$selfIllumFresnel",
              "$selfIllumFresnelMinMaxExp", "bumpFrames.size() == 2",
              "bumpAnimationFrameRate - 70.0f", "hasLightWarpTexture",
              "hasEnvMapCube", "character_runtime_tint", "robot_invulnerability"]:
    assert token in tests, token

assert "isTf2InvulnerabilityMaterial" in material_cpp
assert 'name.find("uber")' not in material_cpp
assert "previewUberColor" in material_h and "previewUberColor" in material_cpp
assert "HAMMER_MATERIAL_PROXY_ONLY" in tests
assert "hammer-material-proxy-tests" in cmake
assert 'ENVIRONMENT "HAMMER_MATERIAL_PROXY_ONLY=1"' in cmake

print("parameter-driven TF2 invulnerability material validation passed")

# Forced invulnerability classification remains explicit, while Lightwarp is
# independently controlled for every material declaring $lightwarptexture.
assert "material.uberEffect = isTf2InvulnerabilityMaterial(materialName);" in material_cpp
assert "material.invulnLevelProxy || modelGlowInvulnerability" not in material_cpp
assert "const bool useLightWarp = usable && lightWarpEnabled_ &&" in renderer
assert "lightWarpEnabled_ && material->uberEffect" not in renderer
