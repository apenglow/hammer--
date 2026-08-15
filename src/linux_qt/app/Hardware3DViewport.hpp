#pragma once

#include <QImage>
#include <QString>
#include <QWidget>
#include <memory>
#include <vector>

namespace hammer::render { struct BakedCubemap; }

class MapViewWidget;
class QOffscreenSurface;
class QOpenGLContext;
class QPaintEvent;
class QResizeEvent;
class QTimer;

// Hardware-accelerated 3D renderer that deliberately remains a plain QWidget.
// OpenGL rendering happens in an independent off-screen context and FBO. The
// completed image is then painted into the normal QWidget backing store. This
// avoids QOpenGLWidget forcing the entire MDI top-level window through Qt's
// QRhi/Wayland OpenGL compositor.
class Hardware3DViewport final : public QWidget
{
    Q_OBJECT
public:
    explicit Hardware3DViewport(MapViewWidget* owner);
    ~Hardware3DViewport() override;

    void invalidateMaterialCache();
    void invalidateGeometryCache();
    // Replaces the ray-traced env_cubemap bake this view reflects. Passing an
    // empty list reverts every surface to the sky approximation.
    void setBakedCubemaps(std::vector<hammer::render::BakedCubemap> cubemaps);
    void requestUpdate(bool rerender = true);
    // The owning pane was retyped between 2D and 3D: only a 3D view needs the
    // water/animated-material clock running, and the cached frame is drawn for
    // the old kind.
    void handleOwnerKindChanged();
    QString rendererDescription() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class Renderer;

    bool ensureContext();
    bool renderFrame();
    void releaseRenderer();
    void setContextError(const QString& error);

    MapViewWidget* owner_{nullptr};
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<QOffscreenSurface> surface_;
    // Hand-rolled instead of QOpenGLFramebufferObject: water refraction and
    // depth fog have to sample the scene depth, and Qt's FBO only ever offers a
    // depth renderbuffer whose format it chooses.
    class SceneTarget;
    std::unique_ptr<SceneTarget> framebuffer_;
    QImage frame_;
    QString contextError_;
    QTimer* waterAnimationTimer_{nullptr};
    bool frameDirty_{true};
};
