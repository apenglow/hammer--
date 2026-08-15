#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
checks = {
    root / 'src/linux_qt/app/MainWindow.cpp': [
        'FGD file:', 'FgdPath', 'loadFgdPath(requestedFgd, true)',
        'FGD colors, sizes, descriptions, models, sprites',
    ],
    root / 'src/linux_qt/app/MapViewWidget.hpp': [
        'objectPropertiesRequested', 'mouseDoubleClickEvent', 'entityScreenBounds',
    ],
    root / 'src/linux_qt/app/MapViewWidget.cpp': [
        'void MapViewWidget::mouseDoubleClickEvent', 'emit objectPropertiesRequested',
        'entity.displayColor', 'entity.sizeMinimum', 'entity.sprite',
    ],
    root / 'src/linux_qt/app/MapDocumentWidget.cpp': [
        'applyFgdEntityVisualization', 'effectiveVisualization',
        'showObjectProperties(window())',
    ],
    root / 'src/linux_qt/fgd/FgdDatabase.cpp': [
        'modifier == "color"', 'modifier == "size"', 'modifier == "studio"',
        'modifier == "iconsprite"', 'effectiveVisualization',
    ],
    root / 'src/linux_qt/tests/vmf_document_tests.cpp': [
        'FGD point-class color and size helpers are retained',
        'FGD model, sprite, helper kind, and description are retained',
    ],
    root / 'FGD_ENTITY_EDITING.md': [
        'double-clicked', 'color(r g b)', 'size(minx miny minz',
    ],
}

missing = []
for path, needles in checks.items():
    if not path.exists():
        missing.append(f'missing file: {path.relative_to(root)}')
        continue
    text = path.read_text(errors='replace')
    for needle in needles:
        if needle not in text:
            missing.append(f'{path.relative_to(root)} missing {needle!r}')

if missing:
    print('FGD entity editing validation failed:')
    for item in missing:
        print(' -', item)
    sys.exit(1)
print('FGD selection, point-entity double-click editing, and FGD helper visualization validation passed.')
