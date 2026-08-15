#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
files = {
    "camera_hpp": root / "src/linux_qt/camera/Camera3D.hpp",
    "camera_cpp": root / "src/linux_qt/camera/Camera3D.cpp",
    "view_hpp": root / "src/linux_qt/app/MapViewWidget.hpp",
    "view_cpp": root / "src/linux_qt/app/MapViewWidget.cpp",
    "document": root / "src/linux_qt/app/MapDocumentWidget.cpp",
    "main": root / "src/linux_qt/app/MainWindow.cpp",
    "tests": root / "src/linux_qt/tests/vmf_document_tests.cpp",
    "cmake": root / "src/linux_qt/CMakeLists.txt",
    "doc": root / "CAMERA_3D.md",
}
errors = []
for label, path in files.items():
    if not path.exists():
        errors.append(f"missing {label}: {path}")

required = {
    "camera_hpp": ["enum class ProjectionMode", "projectPoint", "projectLine", "forwardVector"],
    "camera_cpp": [
        "nearPlane", "verticalFovRadians", "orthographicHeight", "clip",
        "Source/Hammer coordinates use +X forward, +Y left, and +Z up",
        "return {std::sin(state.yawRadians), -std::cos(state.yawRadians), 0.0}",
        "right x forward preserves +Z",
    ],
    "view_hpp": ["setProjectionMode", "toggleMouseCapture", "updateFlyMovement", "keyReleaseEvent"],
    "view_cpp": [
        "Qt::Key_Z", "Qt::Key_W", "Qt::Key_A", "Qt::Key_S", "Qt::Key_D",
        "grabMouse(QCursor(Qt::BlankCursor))", "flySpeed_ * std::pow(1.25", "projectCameraLine",
        "world-space X/Y plane", "QTimer::timeout", "drawZCameraCrosshair",
        "centerCapturedPointer",
        "cameraState_.yawRadians -= deltaX * sensitivity",
    ],
    "document": ["setCameraProjection", "cameraProjection"],
    "main": ["3D &Projection", "&Perspective Projection", "&Orthographic Projection"],
    "tests": [
        "perspective camera projects its forward axis", "near plane", "orthographic 3D projection",
        "positive world Z toward the top",
        "Source negative Y as screen-right",
        "right-handed instead of horizontally reflected",
        "Source +Y on the left and -Y on the right",
    ],
    "cmake": ["add_library(hammer_camera", "hammer_camera hammer_fgd"],
}
for label, tokens in required.items():
    path = files[label]
    if not path.exists():
        continue
    text = path.read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            errors.append(f"{path.name} missing token: {token}")

if errors:
    print("3D camera validation failed:")
    for error in errors:
        print(f" - {error}")
    sys.exit(1)

print("3D camera structure validation passed.")
print("Projection switching, non-mirrored Source handedness, world grid, Z capture, WASD flight, wheel speed, and portable math tests are present.")
