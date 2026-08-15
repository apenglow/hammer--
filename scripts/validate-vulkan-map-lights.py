#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parents[1]
a=(r/'src/linux_qt/app/RayTracingScene.cpp').read_text()
s=(r/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
v=(r/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
checks={
    'lights': all(x in a for x in ['light_spot','light_environment','_ambient']),
    'Source attenuation': 'q * 10000.0f + l * 100.0f + c' in a,
    'Source direction props': 'entity.angles.y' in a and 'propertyNumber(entity, "pitch", 0.0)' in a,
    'nodraw shadows': 'normalizedMaterial == "tools/toolsnodraw"' in a,
    'other tools no shadow': 'shadowFlagsForMaterial' in a and 'TriangleNoShadow' in a,
    'hidden tool primary flag': 'TriangleNoPrimary' in a and 'TRIANGLE_NO_PRIMARY' in s,
    'shadow skip': 'TRIANGLE_NO_SHADOW) != 0u' in s,
    'two-sided shadow blockers': 'LightBoundary closestLightBoundary' in s and 'float lightTransmission' in s,
    'gpu light buffer': 'lightBuffer_' in v and 'binding = 8' in s,
}
f=[k for k,x in checks.items() if not x]
if f: raise SystemExit('validation failed: '+', '.join(f))
print('Vulkan VMF lights and tool-shadow validation passed')
