#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
camera_h = (root / "src/linux_qt/camera/Camera3D.hpp").read_text()
camera_cpp = (root / "src/linux_qt/camera/Camera3D.cpp").read_text()
scene_h = (root / "src/linux_qt/vmf/VmfScene.hpp").read_text()
scene_cpp = (root / "src/linux_qt/vmf/VmfScene.cpp").read_text()
gpu = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
checks = {
    "shared affine type": "struct SourceTransform" in camera_h and "transformPoint" in camera_cpp,
    "exact QAngle API": "sourceAngleBasis(const vmf::Vec3& pitchYawRollDegrees)" in camera_h,
    "raw angle storage": "hasPitchOverride" in scene_h and "renderAngles() const" in scene_h,
    "no single-angle transform hack": 'root.value("angle")' not in scene_cpp,
    "3D shared transform": "sourceTransform(entity.origin, entity.renderAngles())" in gpu,
    "2D shared transform": "sourceTransform(entity.origin, entity.renderAngles())" in view,
}
failed=[name for name,ok in checks.items() if not ok]
if failed: raise SystemExit("Source entity transform validation failed: " + ", ".join(failed))
print("Source entity transform validation passed")
