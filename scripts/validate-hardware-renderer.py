#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cmake = (root / "src/linux_qt/CMakeLists.txt").read_text()
main = (root / "src/linux_qt/app/main.cpp").read_text()
view = (root / "src/linux_qt/app/MapViewWidget.cpp").read_text()
gpu = (root / "src/linux_qt/app/Hardware3DViewport.cpp").read_text()
header = (root / "src/linux_qt/app/Hardware3DViewport.hpp").read_text()

required = {
    "Qt OpenGL target": "Qt6::OpenGL" in cmake,
    "No OpenGLWidgets dependency": "OpenGLWidgets" not in cmake,
    "Raster QWidget camera surface": "public QWidget" in header,
    "Offscreen surface": "QOffscreenSurface" in gpu,
    "Offscreen context": "QOpenGLContext" in gpu,
    "Framebuffer object": "QOpenGLFramebufferObject" in gpu,
    "No QOpenGLWidget class": "#include <QOpenGLWidget>" not in gpu and "public QOpenGLWidget" not in header,
    "No native window container": "createWindowContainer" not in gpu,
    "Raster backing store requested before QApplication": main.find('qputenv("QT_WIDGETS_RHI"') < main.find("QApplication application"),
    "No global surface format override": "setDefaultFormat" not in main,
    "Legacy GLES2 shader": "#version 100" in gpu,
    "Legacy desktop shader": "#version 120" in gpu,
    "vertex shader": "#version 330 core" in gpu and "gl_Position" in gpu,
    "texture shader": "texture(uTexture" in gpu,
    "depth testing": "glEnable(GL_DEPTH_TEST)" in gpu,
    "ordinary brush backface culling": "glEnable(GL_CULL_FACE)" in gpu and "glCullFace(GL_BACK)" in gpu,
    "Source clockwise front-face winding": "glFrontFace(GL_CW)" in gpu and "glFrontFace(GL_CCW)" not in gpu,
    "displacements, water, and compiletrigger stay two-sided":
        "twoSided = batch.displacement || water" in gpu and "compileTrigger" in gpu,
    "Source-style water normals": gpu.count('rotated45') == 8 and
                                   gpu.count('rotated90') == 8 and
                                   gpu.count('reflectionSurfaceNormal = normalize(') == 4 and
                                   gpu.count('refractionSurfaceNormal = normalize(') == 4,
    "single-layer refractively bent skybox water":
        gpu.count('environmentDirection = normalize(') == 4 and
        gpu.count('refractedDirection = refract(') == 4 and
        gpu.count('visibleSkyHemisphere = geometricNormal * interfaceSide') == 4 and
        gpu.count('refractiveBend = clamp(') == 4 and
        gpu.count('baseUv, -2.0') == 4 and
        'environmentRefraction = uHasEnvironmentMap' not in gpu,
    "bounded water animation phases": "wrappedUnitPhase" in gpu and "uTime" not in gpu,
    "water normal slope preservation": "record.id = uploadTexture(sourceImage, !material->water)" in gpu,
    "GPU vertex buffer": "glGenBuffers" in gpu and "glBufferData" in gpu,
    "GPU texture upload": "glTexImage2D" in gpu and "glGenerateMipmap" in gpu,
    "hardware viewport child": "new Hardware3DViewport" in view,
    "software renderer removed from target": "app/MaterialRenderer.cpp" not in cmake,
}
failed = [name for name, ok in required.items() if not ok]
if failed:
    raise SystemExit("Hardware renderer validation failed: " + ", ".join(failed))
print("Hardware renderer structural validation passed")
