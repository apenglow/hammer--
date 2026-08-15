#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
main = (root / 'src/linux_qt/app/MainWindow.cpp').read_text()
qrc = (root / 'src/linux_qt/hammer_resources.qrc').read_text()
icons = {
    'tool.pointer': 'select_tool.png',
    'tool.magnify': 'magnify_tool.png',
    'tool.camera': 'camera_tool.png',
    'tool.entity': 'entity_tool.png',
    'tool.block': 'brush_tool.png',
    'tool.textureApplication': 'face_tool.png',
    'tool.applyTexture': 'texture_tool.png',
    'tool.decals': 'decal_tool.png',
    'tool.overlay': 'overlay_tool.png',
    'tool.clipper': 'clip_tool.png',
    'tool.morph': 'vertex_tool.png',
}
errors = []
for tool, filename in icons.items():
    if f'{{"{tool}"' not in main or f'"{filename}"' not in main:
        errors.append(f'missing MainWindow mapping: {tool} -> {filename}')
    path = root / 'src/linux_qt/resources/tool_icons' / filename
    if not path.is_file() or path.stat().st_size == 0:
        errors.append(f'missing icon file: {filename}')
    alias = f'<file alias="tool_icons/{filename}">resources/tool_icons/{filename}</file>'
    if alias not in qrc:
        errors.append(f'missing Qt resource alias: {filename}')
if errors:
    print('\n'.join(errors), file=sys.stderr)
    raise SystemExit(1)
print('Sidebar icon mapping validation passed.')
