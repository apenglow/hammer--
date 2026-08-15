#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
vmf_h=(root/'src/linux_qt/vmf/VmfScene.hpp').read_text()
vmf=(root/'src/linux_qt/vmf/VmfScene.cpp').read_text()
scene_h=(root/'src/linux_qt/app/RayTracingScene.hpp').read_text()
scene=(root/'src/linux_qt/app/RayTracingScene.cpp').read_text()
view=(root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
shader=(root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
checks={
 'tonemap scene state':'ToneMapSettings' in vmf_h and 'autoExposureMin{0.5}' in vmf_h and 'autoExposureMax{2.0}' in vmf_h,
 'tonemap controller startup':'env_tonemap_controller' in vmf and 'setautoexposuremin' in vmf and 'setautoexposuremax' in vmf and 'settonemaprate' in vmf,
 'logic auto outputs':'onmapspawn' in vmf and 'blendtonemapscale' in vmf,
 'point color correction':'color_correction' in vmf and 'minfalloff' in vmf and 'maxfalloff' in vmf,
 'volume color correction':'color_correction_volume' in vmf and 'correction.minimum' in vmf and 'correction.maximum' in vmf,
 '32 cube LUT':'LutSide = 32' in scene and 'LutBytes = LutTexels * 3' in scene,
 'LUT dedupe':'loadedLuts' in scene,
 'gamma correction':'applySourceColorCorrection' in shader and 'sourceLinearToGamma(color)' in shader,
 'up to four LUTs':'for (int i = 0; i < 4; ++i)' in shader and 'colorCorrectionWeights' in shader,
 'Source LUT coordinates':'* 31.0' in shader and 'sourceColorCorrectionTexel' in shader,
 'no ACES':'2.51 * color + 0.03' not in shader,
 '16-bin histogram':'sourceHistogramBoundary' in view and 'std::array<std::uint32_t, 16>' in view,
 'Source nonlinear bins':'std::pow(normalized, 1.5f)' in view and 'pow(normalizedLuminance, 2.0 / 3.0)' in shader,
 'Source bright target':'findHistogramLocation(bins, 2.0f, 60.0f)' in view and '0.60f / brightLocation' in view,
 'Source median floor':'findHistogramLocation(bins, 50.0f)' in view and '0.03f / medianLocation' in view,
 'Source center region':'centered.x <= 0.45' in shader and 'centered.y <= 0.425' in shader,
 'authored exposure clamp':'scene_.toneMap.autoExposureMin' in view and 'scene_.toneMap.autoExposureMax' in view,
 'faster dark adaptation':'goal < currentToneScale_ ? 3.0f : 1.0f' in view,
 'low overhead buffer':'17u * sizeof(std::uint32_t)' in view and 'VK_BUFFER_USAGE_TRANSFER_DST_BIT' in view,
 'descriptor binding':'binding = 10' in shader and 'std::array<VkDescriptorSetLayoutBinding, 11>' in view,
}
failed=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(f'{k}: {"ok" if v else "FAIL"}')
if failed: raise SystemExit('Source postprocess validation failed: '+', '.join(failed))
print('Source tonemapping and color-correction RT validation passed')
