#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / 'src/linux_qt/app/MainWindow.cpp').read_text()
document = (root / 'src/linux_qt/app/MapDocumentWidget.cpp').read_text()
document_h = (root / 'src/linux_qt/app/MapDocumentWidget.hpp').read_text()
editor = (root / 'src/linux_qt/vmf/VmfEditor.cpp').read_text()
editor_h = (root / 'src/linux_qt/vmf/VmfEditor.hpp').read_text()
scene = (root / 'src/linux_qt/vmf/VmfScene.cpp').read_text()
scene_h = (root / 'src/linux_qt/vmf/VmfScene.hpp').read_text()
gpu = (root / 'src/linux_qt/app/Hardware3DViewport.cpp').read_text()
tests = (root / 'src/linux_qt/tests/vmf_document_tests.cpp').read_text()

checks = {
    'map menu action is implemented':
        'QAction* mapProperties = addMenuCommand' in main and
        'mapProperties->setProperty("implemented", true)' in main and
        'document->showMapProperties(this)' in main,
    'worldspawn properties API':
        'worldProperties() const' in editor_h and
        'replaceWorldProperties' in editor_h and
        'EditorModel::worldProperties() const' in editor and
        'EditorModel::replaceWorldProperties' in editor,
    'map properties dialog':
        'void MapDocumentWidget::showMapProperties' in document and
        'Map Properties — worldspawn' in document and
        'editor_.replaceWorldProperties(updated)' in document and
        'showMapProperties' in document_h,
    'skyname reaches scene':
        'std::string skyName;' in scene_h and
        'scene.skyName = *skyName' in scene,
    'skybox renderer':
        'void drawSkybox' in gpu and
        '"skybox/" + skyName + face.suffix' in gpu and
        'drawSkybox(scene->skyName' in gpu,
    'horizontal face mapping':
        'BK -> LF -> FT -> RT' in gpu and
        '{"bk", point(-1,  1,  1)' in gpu and
        '{"lf", point(-1, -1,  1)' in gpu and
        '{"ft", point( 1, -1,  1)' in gpu and
        '{"rt", point( 1,  1,  1)' in gpu,
    'vertical faces':
        '{"up",' in gpu and '{"dn",' in gpu,
    'map property regressions':
        'worldspawn properties can be replaced' in tests and
        'changed map skyname reaches the scene' in tests and
        'changing map properties preserves world geometry' in tests,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit('Map properties / skybox validation failed: ' + ', '.join(failed))
print('Map properties and corrected BK -> LF -> FT -> RT skybox preview validation passed.')
