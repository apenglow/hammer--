#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
scene=(root/'src/linux_qt/app/RayTracingScene.cpp').read_text()
viewport=(root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
checks={
 'winding reversal':'output.indices.push_back(first + 2u);' in scene and 'output.indices.push_back(first + 1u);' in scene,
 'edge-aware denoise':'edgeAwareDenoise(frame);' in viewport,
 'temporal accumulation':'temporalAccumulate(frame, historyFrame_, 0.78f);' in viewport,
 'camera reset':'if (cameraChanged) {\n            historyFrame_ = QImage{};' in viewport,
 'scene reset':'if (rebuildScene || !scene_.valid()) {\n            historyFrame_ = QImage{};' in viewport,
 'interactive reset':'historyFrame_ = QImage{};' in viewport and 'if (!interactive)' in viewport,
}
failed=[k for k,v in checks.items() if not v]
if failed: raise SystemExit('validation failed: '+', '.join(failed))
print('Vulkan winding and denoising validation passed')
