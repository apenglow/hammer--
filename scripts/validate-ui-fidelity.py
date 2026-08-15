#!/usr/bin/env python3
from pathlib import Path
from PIL import Image
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
hammer = root / "src" / "hammer"
qt = root / "src" / "linux_qt"
errors: list[str] = []

expected_images = {
    "forgemap.bmp": (96, 15),
    "mapedit.bmp": (440, 32),
    "mapedit256.bmp": (480, 32),
    "toolbar1.bmp": (432, 15),
    "undoredo.bmp": (32, 15),
    "newsplash.bmp": (489, 182),
}
for name, expected in expected_images.items():
    path = hammer / "res" / name
    if not path.exists():
        errors.append(f"missing original resource: {path}")
        continue
    actual = Image.open(path).size
    if actual != expected:
        errors.append(f"unexpected dimensions for {name}: {actual}, expected {expected}")

rc_text = (hammer / "hammer.rc").read_text(encoding="latin-1")
for token in [
    "IDR_FORGEMAPTYPE MENU",
    "IDR_MAPDOC_VALVE TOOLBAR",
    "IDR_MAPEDITTOOLS_VALVE TOOLBAR",
    "IDR_MAPOPERATIONS_VALVE TOOLBAR",
    "IDD_FILTERCONTROL DIALOGEX",
    "IDD_OBJECTBAR DIALOGEX",
    "IDD_TEXTUREBAR DIALOG",
    "IDD_SELECT_MODE_BAR DIALOG",
]:
    if token not in rc_text:
        errors.append(f"original resource definition missing: {token}")

qrc = ET.parse(qt / "hammer_resources.qrc")
for entry in qrc.findall(".//file"):
    relative = (qt / (entry.text or "")).resolve()
    if not relative.exists():
        errors.append(f"qrc entry does not exist: {entry.text}")

main_window = (qt / "app" / "MainWindow.cpp").read_text(encoding="utf-8")
for token in [
    'tr("&File")', 'tr("&Edit")', 'tr("&Map")', 'tr("&View")',
    'tr("&Tools")', 'tr("&Window")', 'tr("&Help")',
    'tr("Selection Mode")', 'tr("Textures")', 'tr("Filter Control")',
    'tr("New Objects")', 'MapEditStrip', 'MapOperationsStrip',
    'QKeySequence(QStringLiteral("Shift+Z"))',
]:
    if token not in main_window:
        errors.append(f"Qt fidelity shell missing token: {token}")

if errors:
    print("UI fidelity validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)

print("UI fidelity resources validated successfully.")
print("Original menus, toolbar strips, control-bar dialogs, and Qt resource aliases are present.")
