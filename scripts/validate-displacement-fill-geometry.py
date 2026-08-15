#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hw = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
rt = (root / 'src/linux_qt/app/RayTracingScene.cpp').read_text()
mat = (root / 'src/linux_qt/app/MaterialRenderer.cpp').read_text()
vmf = (root / 'src/linux_qt/vmf/VmfScene.cpp').read_text()
hpp = (root / 'src/linux_qt/vmf/VmfScene.hpp').read_text()
checks = {
    'source authoritative indices': 'if ((ndx & 1) != 0)' in vmf,
    'hardware uses index list': 'face.displacementIndices[tri + corner]' in hw,
    'rt uses index list': 'face.displacementIndices[tri + 0]' in rt,
    'software uses index list': 'face.displacementIndices[index + corner]' in mat,
    'separate displacement batches': 'batch.displacement = displacementSurface;' in hw,
    'displacement two-sided': 'const bool twoSided = batch.displacement || water ||' in hw,
    'ordinary brush culling retained': 'else glEnable(GL_CULL_FACE);' in hw,
    'no support cap suppression': 'faceIsDisplacementSupportCap' not in hpp and 'faceIsDisplacementSupportCap' not in vmf,
    'no filled grid retessellation': 'appendDisplacementTriangle(a, d, b);' not in hw,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('Displacement fill validation failed: ' + ', '.join(failed))
print('Displacement fill uses the authoritative Source/Hammer render-index list in every renderer')
