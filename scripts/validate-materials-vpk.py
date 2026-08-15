#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
required = {
    "src/linux_qt/assets/GameFileSystem.cpp": ["|appid_", "libraryfolders.vdf", "_dir.vpk", "VpkSignature"],
    "src/linux_qt/assets/MaterialSystem.cpp": ["$basetexture", "decodeDxt1", "decodeDxt5", "Invalid VTF signature"],
    "src/linux_qt/app/Hardware3DViewport.cpp": ["glTexImage2D", "glEnable(GL_DEPTH_TEST)", "glDrawArrays"],
    "src/linux_qt/app/MainWindow.cpp": ["Configure Game Directory", "loadGameInfoPath"],
    "src/linux_qt/vmf/VmfScene.hpp": ["FaceGeometry", "TextureAxis"],
}
for name, markers in required.items():
    text = (root / name).read_text(errors="replace")
    for marker in markers:
        if marker not in text:
            raise SystemExit(f"missing {marker!r} in {name}")
print("material/VPK structural validation passed")
