#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_cpp = (root / 'src/linux_qt/app/MainWindow.cpp').read_text()
main_hpp = (root / 'src/linux_qt/app/MainWindow.hpp').read_text()
view_cpp = (root / 'src/linux_qt/app/MapViewWidget.cpp').read_text()
view_hpp = (root / 'src/linux_qt/app/MapViewWidget.hpp').read_text()
hardware = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
document_cpp = (root / 'src/linux_qt/app/MapDocumentWidget.cpp').read_text()

checks = {
    'wireframe overlay action': 'view.wireframeOverlay' in main_cpp and 'Overlay 3D &Wireframe' in main_cpp,
    'wireframe disabled by default': 'wireframeOverlayEnabled_{false}' in main_hpp and 'render/wireframeOverlay3d"), false' in main_cpp,
    'exclusive textured actions': 'texturedModeGroup->setExclusive(true)' in main_cpp and 'shadedTexturedViewAction_' in main_cpp and 'shadedMaterialPolygonsViewAction_' in main_cpp,
    'unlit default': 'TexturedRenderMode::Unlit' in main_hpp and 'QStringLiteral("unlit")' in main_cpp,
    'materials polygon mode': '3D Textured Shaded + &Materials Polygons' in main_cpp and 'shaded-material-polygons' in main_cpp,
    'state reaches documents': 'document->setWireframeOverlayEnabled' in main_cpp and 'document->setTexturedRenderMode' in main_cpp,
    'state reaches viewports': 'view->setWireframeOverlayEnabled' in document_cpp and 'view->setTexturedRenderMode' in document_cpp,
    'advanced mode suppresses outline pass': 'allowOutlineOverlay' in view_cpp and
                                               'texturedRenderMode_ != TexturedRenderMode::ShadedMaterialPolygons' in view_cpp and
                                               '(wireframeOverlayEnabled_ && allowOutlineOverlay)' in view_cpp,
    'advanced mode gates material effects': 'advancedMaterialPreview && owner_->phongEnabled_' in hardware and
                                             'advancedMaterialPreview && owner_->specularEnabled_' in hardware and
                                             'advancedMaterialPreview && owner_->bumpMapsEnabled_' in hardware,
    'normal vertex attribute': 'layout(location = 2) in vec3 aNormal' in hardware and 'bindAttributeLocation("aNormal", 2)' in hardware,
    'legacy normal attribute': hardware.count('attribute vec3 aNormal;') == 2,
    'all fragment shaders shade normals': hardware.count('uniform int uShaded;') == 4 and hardware.count('vec3 normal = geometricNormal;') == 4,
    'face normals uploaded': 'static_cast<float>(normal.x)' in hardware and 'face.normal' in hardware and 'offsetof(GpuVertex, nx)' in hardware,
    'shaded mode reaches renderer': 'texturedRenderMode_ != MapViewWidget::TexturedRenderMode::Unlit' in hardware,
    'gamma-correct shaded modulation': hardware.count('vec3 linearAlbedo = pow') == 4 and hardware.count('vec3 linearTexture = linearAlbedo * diffuseLightingColor') == 4 and hardware.count('vec3(1.0 / 2.2)') == 4,
    'brighter contrast-lifted shaded output': hardware.count('vec3 diffuseLightingColor = clamp(') == 4 and hardware.count('(shadedColor - vec3(0.5)) * 1.06 +') == 4,
    'optional Source diffuse controls': hardware.count('halfLambertDiffuse *= halfLambertDiffuse;') == 4 and hardware.count('? halfLambertDiffuse : lambertDiffuse;') == 4 and hardware.count('if (uUseLightWarp != 0)') == 4,
    'water remains separate': hardware.count('if (uWater != 0)') >= 4,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    for name in failed:
        print(f'FAIL: {name}')
    raise SystemExit(1)
print('Textured render-mode and wireframe-overlay validation passed.')
