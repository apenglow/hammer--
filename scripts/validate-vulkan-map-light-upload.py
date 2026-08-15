#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
cpp = (root / "src/linux_qt/app/VulkanRayTracedViewport.cpp").read_text()
checks = {
    "map-light SSBO uploaded": "createBuffer(scene_.lights.size() * sizeof(scene_.lights[0])" in cpp and "lightBuffer_, error, scene_.lights.data()" in cpp,
    "descriptor pool reserves all RT storage buffers": "{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}" in cpp,
    "light buffer bound at shader binding 8": "writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER" in cpp and "writes[8].pBufferInfo = &bufferInfos[5]" in cpp,
    "light buffer destroyed with scene": "destroyBuffer(lightBuffer_)" in cpp,
}
failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(f"{name}: {'ok' if ok else 'FAILED'}")
if failed:
    raise SystemExit("validation failed: " + ", ".join(failed))
print("Vulkan RT map-light upload validation passed")
