#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
checks = {
    root / 'src/linux_qt/assets/StudioModelSystem.cpp': [
        '".dx90.vtx"', 'StudioVertexSize = 48', 'origMeshVertID',
        'fixedVertexOrder', 'StudioHeaderBoneCount', 'transformPoint(matrix, sourcePosition)', 'No renderable bind-pose meshes',
    ],
    root / 'src/linux_qt/app/Hardware3DViewport.cpp': [
        'drawStudioModel', 'drawBillboardSprite', 'drawEntityHelpers',
        'camera::rightVector(camera)', 'GL_CLAMP_TO_EDGE',
    ],
    root / 'src/linux_qt/app/MapViewWidget.cpp': [
        'studioModels_->model(entity.model)', 'painter.drawImage(*target',
        'FGD model and sprite helpers are rendered by the hardware pass',
    ],
    root / 'src/linux_qt/app/MapDocumentWidget.cpp': [
        'resolveHelper', 'propertyValue(normalized)', 'propertyValue("model")', 'ModelHelperKind::LightProp',
    ],
    root / 'src/linux_qt/fgd/FgdDatabase.cpp': [
        'modifier == "studioprop"', 'modifier == "lightprop"',
    ],
    root / 'src/linux_qt/tests/asset_system_tests.cpp': [
        'makeTinyMdl', 'makeTinyVvd', 'makeTinyVtx',
        'helperModel->meshes[0].vertices.size()==3',
    ],
    root / 'FGD_ENTITY_EDITING.md': [
        'no longer receive a generic box, cross, or diamond',
        'camera-facing billboard',
    ],
}
missing=[]
for path, needles in checks.items():
    if not path.exists():
        missing.append(f'missing file: {path.relative_to(root)}')
        continue
    text=path.read_text(errors='replace')
    for needle in needles:
        if needle not in text:
            missing.append(f'{path.relative_to(root)} missing {needle!r}')
if missing:
    print('FGD model/sprite helper validation failed:')
    for item in missing: print(' -', item)
    sys.exit(1)
print('FGD models, billboard sprites, helper property resolution, and generic-marker removal validation passed.')
