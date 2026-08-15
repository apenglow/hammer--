from pathlib import Path
root=Path(__file__).resolve().parents[1]
shader=(root/'src/linux_qt/shaders/raytraced_preview.comp').read_text()
checks={
 'Source gamma 2.2 decode':'sourceGammaToLinear' in shader and 'vec3(2.2)' in shader,
 'Source gamma 2.2 encode':'sourceLinearToGamma' in shader and '1.0 / 2.2' in shader,
 'finite HDR guard':'finiteColor(color * max(cameraData.toneMapControls.x' in shader,
 'ordinary highlight compression':'materialHighlight = materialHighlight / (vec3(1.0) + materialHighlight)' in shader,
 'highlight headroom':'max(vec3(1.0) - color, vec3(0.12))' in shader,
 'high energy preserved':'color += min(materialHighlight, vec3(4.0))' in shader,
 'no ACES':'2.51 * color + 0.03' not in shader,
}
failed=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(f'{k}: {"ok" if v else "FAIL"}')
if failed: raise SystemExit('Vulkan RT color range validation failed: '+', '.join(failed))
print('Vulkan RT color range validation passed')
