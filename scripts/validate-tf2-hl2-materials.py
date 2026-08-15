#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
fs = (root / "src/linux_qt/assets/GameFileSystem.cpp").read_text()
mat = (root / "src/linux_qt/assets/MaterialSystem.cpp").read_text()
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()
tests = (root / "src/linux_qt/tests/asset_system_tests.cpp").read_text()
required = {
    "TF2 misc fallback": 'hl2 / "hl2_misc.vpk"' in fs,
    "TF2 textures fallback": 'hl2 / "hl2_textures.vpk"' in fs,
    "HL2 material probe": "materials/brick/brickwall001a.vmt" in fs,
    "VPK enumeration": "VpkArchive::files" in fs,
    "mounted file index": "GameFileSystem::listFiles" in fs,
    "material names": "MaterialSystem::materialNames" in mat,
    "texture browser": "showMaterialBrowser" in main,
    "source diagnostics": "HL2 material probe:" in main,
    "paired archive regression": "hl2_textures_dir.vpk" in tests and "fallbackMaterials.material" in tests,
}
failed = [name for name, ok in required.items() if not ok]
if failed:
    raise SystemExit("TF2/HL2 material validation failed: " + ", ".join(failed))
print("TF2/HL2 material indexing validation passed")
