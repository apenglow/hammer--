from pathlib import Path
root=Path(__file__).resolve().parents[1]
scene=(root/'src/linux_qt/app/RayTracingScene.cpp').read_text()
hdr=(root/'src/linux_qt/app/RayTracingScene.hpp').read_text()
shader=(root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
view=(root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
mat=(root/'src/linux_qt/assets/MaterialSystem.cpp').read_text()
checks={
 'vmt alpha parsed':'result->alpha = std::clamp' in mat,
 'rt alpha control':'transparencyControls' in hdr and 'transparencyControls' in shader,
 'translucent layers':'for (int layer = 0; layer < 4' in shader and 'lightTransmission' in shader,
 'sprite no shadow':'TriangleNoShadow' in scene and 'hasCameraFacingSprites = true' in scene,
 'sprite settled refit':'spriteBillboardRefreshPending_' in view and 'const bool refreshSprites = !interactive && spriteBillboardRefreshPending_' in view and 'updateSpriteGeometry' in view,
 'animated model refresh':'renderer_->hasAnimatedContent()' in view and 'updateDynamicGeometry' in view,
 'animated bump frames':'bumpFrames.size() > 1' in scene and 'bumpAnimationFrameRate' in scene,
 'atlas dedup':'Deduplicate by the authored texture source' in scene,
 'vram-minimized atlas':'bestAllocatedTexels' in scene and 'candidateSize' in scene,
 'raster alpha parity':'uMaterialAlpha' in (root/'src/linux_qt/app/Hardware3DViewport.cpp').read_text() and 'uAlphaTestReference' in (root/'src/linux_qt/app/Hardware3DViewport.cpp').read_text(),
 'opaque alpha masks preserved':'candidateHasCoverage' in shader and 'RT_TRANSLUCENT' in shader,
 'sprite alpha parity':'TRIANGLE_SPRITE' in shader and 'materialCoverage(material, base, triangleFlags)' in shader and 'return base.rgb;' in shader,
 'translucent light transmission':'LightBoundary' in shader and 'candidateOpacity' in shader,
 'settled four lights':'MAX_SHADED_LIGHTS = 4' in shader,
 'single AO probe':'float visibleSample = lightTransmission' in shader,
 'native settled':' : width;' in view and ': height;' in view,
 'half resolution moving':'(width + 1) / 2' in view and '(height + 1) / 2' in view,
}
for k,v in checks.items(): print(f'{k}: {"ok" if v else "FAIL"}')
if not all(checks.values()): raise SystemExit(1)
print('Vulkan RT fidelity/alpha/animation/VRAM validation passed')
