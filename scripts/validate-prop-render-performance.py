#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
model = (root / "src/linux_qt/assets/StudioModelSystem.cpp").read_text()
camera = (root / "src/linux_qt/camera/Camera3D.cpp").read_text()
fgd = (root / "src/linux_qt/fgd/FgdDatabase.cpp").read_text()
main = (root / "src/linux_qt/app/MainWindow.cpp").read_text()

checks = {
    "bind-pose bone transforms": "StudioHeaderBoneCount" in model and
                                 "quaternionMatrix" in model and
                                 "concatenate(bone.boneToPose, bone.poseToBone)" in model and
                                 "StudioHeader2LinearBoneIndex" in model and
                                 "transformPoint(matrix, sourcePosition)" in model and
                                 "rotateVector(matrix, sourceNormal)" in model,
    "source QAngle transform": "sourceTransform(entity.origin, entity.renderAngles())" in gpu and
                               "sourceTransform(entity.origin, entity.renderAngles())" in view and
                               "Source mathlib AngleMatrix" in camera,
    "FGD model helper transforms": 'modifier == "studioprop"' in fgd and
                                   'modifier == "lightprop"' in fgd,
    "no arbitrary model yaw": "vertex.x = -sourceY;" not in model and
                              "transform.rotate(90.0f, 0.0f, 0.0f, 1.0f)" not in gpu and
                              "transform.rotate(90.0f, 0.0f, 0.0f, 1.0f)" not in view,
    "opaque prop blend state": "if (translucent) glEnable(GL_BLEND);" in gpu and "else glDisable(GL_BLEND);" in gpu,
    "opaque prop depth writes": "glDepthMask(translucent ? GL_FALSE : GL_TRUE);" in gpu,
    "opaque framebuffer alpha": gpu.count("uniform int uForceOpaque;") == 4 and
                                  gpu.count("if (uForceOpaque != 0) fragmentColor.a = 1.0;\n                else if (fragmentColor.a < 0.01) discard;") == 2 and
                                  gpu.count("if (uForceOpaque != 0) gl_FragColor.a = 1.0;\n                else if (gl_FragColor.a < 0.01) discard;") == 2,
    "world geometry cache": "rebuildWorldCache" in gpu and "worldVbo_" in gpu and "GL_STATIC_DRAW" in gpu,
    "studio GPU cache": "studioGpuModel" in gpu and "studioGpuModels_" in gpu,
    "GPU studio animation skinning": all(token in gpu for token in (
        "uGpuSkinning", "uBoneRow0[32]", "paletteBones",
        "sampleAnimationMatrices", "uploadStudioPalette")) and
        "uploadStudioPose" not in gpu and "GL_DYNAMIC_DRAW" not in
        gpu[gpu.find("StudioGpuModel& studioGpuModel"):gpu.find("void bindStudioMesh")],
    "shader model transform": gpu.count("uniform mat4 uModel;") == 4 and gpu.count("vec4 worldPosition = uModel") == 4,
    "overlay frame reuse": "requestUpdate(bool rerender)" in gpu and "requestRepaint(false)" in view,
    "movement timer on demand": "const bool wasIdle = pressedKeys_.isEmpty();" in view and "if (pressedKeys_.isEmpty() && flyTimer_) flyTimer_->stop();" in view,
    "no fly-tick region commit": "void MapViewWidget::updateFlyMovement()" in view and "waylandPointerLock_->updateRegion(this);\n    const qint64 elapsedMilliseconds" not in view,
    "settled resize correction": "layoutRefreshTimer_->setInterval(120)" in main and "mdiArea_->updateGeometry();" not in main,
    "adaptive water cadence": "desiredInterval" in gpu and "setInterval(33)" in gpu,
    "camera status throttling": "cameraStatusElapsed_.elapsed() < 33" in view,
    "single resize render": "if (!hardwareViewport_) requestRepaint();" in view,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("Prop/performance validation failed: " + ", ".join(failed))
print("Prop rendering and viewport performance validation passed")
