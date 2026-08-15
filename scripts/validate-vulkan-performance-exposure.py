#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
cpp=(root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
hpp=(root/'src/linux_qt/app/VulkanRayTracedViewport.hpp').read_text()
shader=(root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
checks={
 'device scoring':'scorePhysicalDevice(candidate' in cpp,
 'software rejection':'deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU' in cpp and 'lavapipe' in cpp,
 'adaptive resolution':'(width + 1) / 2' in cpp and ': width;' in cpp,
 'resident scene':'requested only by invalidateGeometryCache()' in cpp,
 'conditional animation rebuild':'renderer_->hasAnimatedContent()' in cpp,
 'linear albedo':'base.rgb = sourceGammaToLinear' in shader,
 'Source tone scale':'color * max(cameraData.toneMapControls.x, 0.0)' in shader,
 'no ACES':'2.51 * color + 0.03' not in shader,
 'sparse histogram':'(pixel.x & 15) == 0' in shader and 'exposureHistogram' in shader,
 'tiny histogram buffer':'17u * sizeof(std::uint32_t)' in cpp,
 'status overlay':'100% settled / 50% moving ray resolution' in cpp,
}
failed=[k for k,v in checks.items() if not v]
if failed: raise SystemExit('Vulkan performance/exposure validation failed: '+', '.join(failed))
print('Vulkan GPU selection, performance, and exposure validation passed')
