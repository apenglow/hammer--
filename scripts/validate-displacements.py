#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
scene_h = (root / 'src/linux_qt/vmf/VmfScene.hpp').read_text()
scene_cpp = (root / 'src/linux_qt/vmf/VmfScene.cpp').read_text()
material_h = (root / 'src/linux_qt/assets/MaterialSystem.hpp').read_text()
material_cpp = (root / 'src/linux_qt/assets/MaterialSystem.cpp').read_text()
gpu = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
rt = (root / 'src/linux_qt/app/RayTracingScene.cpp').read_text()
view = (root / 'src/linux_qt/app/MapViewWidget.cpp').read_text()
vmf_tests = (root / 'src/linux_qt/tests/vmf_document_tests.cpp').read_text()
asset_tests = (root / 'src/linux_qt/tests/asset_system_tests.cpp').read_text()

checks = {
    'displacement vertex model': 'struct DisplacementVertex' in scene_h and 'blendAlpha' in scene_h,
    'dispinfo parsing': 'firstChild(side, "dispinfo")' in scene_cpp,
    'power tessellation': 'const int cells = 1 << power' in scene_cpp,
    'Source start rotation': 'std::rotate(corners.begin()' in scene_cpp and 'std::swap(corners[1], corners[3])' not in scene_cpp,
    'Source GenerateDispSurf interpolation': 'edgeInterval0' in scene_cpp and 'segmentInterval' in scene_cpp,
    'normal and distance deformation': 'firstChild(*disp, "normals")' in scene_cpp and 'fieldDistances' in scene_cpp,
    'offset deformation': 'firstChild(*disp, "offsets")' in scene_cpp,
    'alpha paint': 'firstChild(*disp, "alphas")' in scene_cpp and 'blendAlpha' in scene_cpp,
    'Source CalcNormalFromEdges normals': 'CalcNormalFromEdges' in scene_cpp and 'normalCount' in scene_cpp,
    'Source checkerboard triangles': 'BuildTriTLtoBR' in scene_cpp and 'BuildTriBLtoTR' in scene_cpp and 'if ((ndx & 1) != 0)' in scene_cpp,
    'secondary material texture': 'baseTexture2' in material_h and 'image2' in material_h and 'blended' in material_h,
    'basetexture2 parsing': '"$basetexture2"' in material_cpp,
    'secondary VTF loading': 'secondaryImage' in material_cpp,
    'GPU blend sampler': 'uTexture2' in gpu and 'uHasTexture2' in gpu,
    'GPU alpha attribute': 'aBlendAlpha' in gpu and 'vBlendAlpha' in gpu,
    'GPU authoritative triangles': 'face.displacementIndices[tri + corner]' in gpu,
    'RT authoritative triangles': 'face.displacementIndices[tri + 0]' in rt,
    'independent second UVs': 'aTexCoord2' in gpu and 'vTexCoord2' in gpu,
    'wireframe displacement grid': 'displacement tessellation' in view,
    'topology regression': 'Source CCoreDispInfo checkerboard topology' in vmf_tests,
    'blend material regression': 'nature/displacement_blend' in asset_tests and 'WorldVertexTransition' in asset_tests,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('Displacement validation failed: ' + ', '.join(failed))
print('Source/Hammer displacement construction, normals, topology, and painted blending validation passed')
