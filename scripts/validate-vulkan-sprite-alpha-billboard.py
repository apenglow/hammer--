#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
scene=(root/'src/linux_qt/app/RayTracingScene.cpp').read_text()
hdr=(root/'src/linux_qt/app/RayTracingScene.hpp').read_text()
view=(root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
shader=(root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
checks={
 'sprite triangle tag':'TriangleSprite = 1u << 5' in scene and 'TRIANGLE_SPRITE' in shader,
 'sprite never shadows':'TriangleNoShadow' in scene and 'TriangleSprite | shadowFlags' in scene,
 'sprite alpha always blended':'if ((triangleFlags & TRIANGLE_SPRITE) != 0u)' in shader and 'return coverage >= 0.005' in shader,
 'sprite unlit parity':'Point-entity sprites are unlit editor helpers' in shader and 'return base.rgb;' in shader,
 'dedicated sprite updater':'updateSpriteGeometry' in hdr and 'RayTracingSceneBuilder::updateSpriteGeometry' in scene,
 'settled billboard refit':'spriteBillboardRefreshPending_' in view and 'const bool refreshSprites = !interactive && spriteBillboardRefreshPending_' in view and 'uploadDynamicVertices(error)' in view,
 'camera queues billboard refresh':'if (scene_.hasCameraFacingSprites) spriteBillboardRefreshPending_ = true;' in view,
 'Source postprocess':'sourceLinearToGamma(color)' in shader and 'applySourceColorCorrection(clamp(color, 0.0, 1.0))' in shader,
 'reused refit scratch':'updateScratchBuffer_' in view and 'requiredScratch' in view,
}
failed=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(f'{k}: {"ok" if v else "FAIL"}')
if failed: raise SystemExit('Sprite/alpha/billboard validation failed: '+', '.join(failed))
print('Vulkan RT sprite alpha and settled billboard validation passed')
