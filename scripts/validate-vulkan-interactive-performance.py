#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
cpp=(root/'src/linux_qt/app/VulkanRayTracedViewport.cpp').read_text()
shader=(root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
checks={
 'camera updates do not dirty scene':'if (rerender) frameDirty_ = true;' in cpp and 'sceneDirty_ = true;\n    }\n    update();' not in cpp,
 'adaptive resolution':'(width + 1) / 2' in cpp and ': width;' in cpp,
 'interactive control':'ControlInteractive' in cpp and 'CONTROL_INTERACTIVE' in shader,
 'interactive two / settled four lights':'MAX_SHADED_LIGHTS = 4' in shader and 'interactive ? 2 : MAX_SHADED_LIGHTS' in shader,
 'interactive AO skip':'if (!interactive) {' in shader,
 'interactive reflection skip':'if (!interactive) {\n            reflectedHit = traceScene' in shader,
}
failed=[k for k,v in checks.items() if not v]
if failed: raise SystemExit('validation failed: '+', '.join(failed))
print('Vulkan interactive performance validation passed')
