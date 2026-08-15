#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()
entry = (root / "src/linux_qt/app/main.cpp").read_text()
window = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
document = (root / "src/linux_qt/app/MapDocumentWidget.cpp").read_text()
desktop = (root / "src/linux_qt/resources/hammerplusplus-testing.desktop").read_text()

checks = {
    "display identity": 'HAMMER_APP_DISPLAY_NAME=\\"Hammer++ (Testing)\\"' in cmake,
    "application id": 'HAMMER_APP_ID=\\"hammerplusplus-testing\\"' in cmake,
    "renamed executable": 'OUTPUT_NAME "hammerplusplus-testing"' in cmake,
    "runtime display name": 'setApplicationDisplayName(QStringLiteral(HAMMER_APP_DISPLAY_NAME))' in entry,
    "legacy settings migration": 'migrateLegacyHammerSettings()' in entry and 'QStringLiteral("Valve"), QStringLiteral("Hammer")' in entry,
    "window branding": '%1 - Hammer++ (Testing)' in window and 'About Hammer++ (Testing)' in window,
    "save prompt branding": 'tr("Hammer++ (Testing)")' in document,
    "desktop entry": 'Name=Hammer++ (Testing)' in desktop and 'Exec=hammerplusplus-testing %F' in desktop,
    "renamed install tree": 'share/hammerplusplus-testing' in cmake,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Testing-brand validation failed: " + ", ".join(failed))
print("Hammer++ (Testing) application branding validation passed")
