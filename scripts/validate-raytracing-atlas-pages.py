#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
scene_h = (root/'src/linux_qt/app/RayTracingScene.hpp').read_text()
scene_cpp = (root/'src/linux_qt/app/RayTracingScene.cpp').read_text()
viewport = (root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
shader = (root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
checks = {
    'atlas layer metadata': 'int layers{0};' in scene_h and 'maximumAtlasLayers' in scene_h,
    'multi-page packing': 'std::vector<ShelfPage> pages(1);' in scene_cpp and 'itemLayers' in scene_cpp,
    'encoded layer rectangles': 'static_cast<float>(layer) +' in scene_cpp,
    'device limits': 'maxImageDimension2D' in viewport and 'maxImageArrayLayers' in viewport,
    'array image': 'VK_IMAGE_VIEW_TYPE_2D_ARRAY' in viewport and 'scene_.atlas.layers' in viewport,
    'per-layer copies': 'baseArrayLayer = layer' in viewport and 'copies.data()' in viewport,
    'array shader': 'uniform sampler2DArray materialAtlas;' in shader and 'floor(rect.x' in shader,
}
failed=[name for name, ok in checks.items() if not ok]
if failed: raise SystemExit('Paged ray-tracing atlas validation failed: '+', '.join(failed))
print('Paged Vulkan ray-tracing material atlas validated.')
