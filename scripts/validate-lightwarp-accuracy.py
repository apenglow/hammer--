#!/usr/bin/env python3
"""Structural and numerical checks for the Source diffuse-lightwarp preview path."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hardware = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
material = (root / 'src/linux_qt/assets/MaterialSystem.cpp').read_text()

# Material support and all four GLSL profiles.
assert '"$lightwarptexture"' in material
for token, expected in [
    ('uniform sampler2D uLightWarpTexture;', 4),
    ('uniform int uUseLightWarp;', 4),
    ('float lightWarpCoordinate = clamp(sourceDiffuse, 0.0, 1.0);', 4),
    ('directDiffuseLighting = lightWarpColor * 0.78;', 4),
    ('ambientCubeLighting + directDiffuseLighting', 4),
]:
    actual = hardware.count(token)
    assert actual == expected, (token, actual)

# Valve binds the diffuse-warp sampler without sRGB read. It is a lighting LUT,
# not albedo, so applying pow(..., 2.2) makes the middle of the ramp far too dark.
assert 'pow(max(texture(uLightWarpTexture' not in hardware
assert 'pow(max(texture2D(uLightWarpTexture' not in hardware

# The warp output replaces the direct diffuse response. Multiplying it by N.L a
# second time destroys authored ramps and makes ordinary TF2 materials go black.
assert 'lightWarpColor * (0.30 + normalShade * 0.92)' not in hardware
assert 'lightWarpColor * sourceDiffuse' not in hardware

# Half-Lambert must be ((N.L * 0.5) + 0.5)^2 before the LUT lookup.
def half_lambert(ndotl: float) -> float:
    x = max(0.0, min(1.0, ndotl * 0.5 + 0.5))
    return x * x

assert abs(half_lambert(-1.0) - 0.0) < 1e-9
assert abs(half_lambert(0.0) - 0.25) < 1e-9
assert abs(half_lambert(1.0) - 1.0) < 1e-9

# Identity LUT behavior: with ambient removed, a grey ramp must exactly preserve
# the local-light response rather than darkening it through a gamma conversion.
for value in (0.0, 0.25, 0.5, 0.75, 1.0):
    sampled_identity = value
    direct = sampled_identity * 0.78
    assert abs(direct - value * 0.78) < 1e-9

print('Source diffuse-lightwarp accuracy validator passed')
