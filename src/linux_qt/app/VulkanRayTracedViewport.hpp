#pragma once

#include <QImage>
#include <QString>
#include <QWidget>
#include <memory>
#include <vector>

namespace hammer::assets { struct CubeImage; }
namespace hammer::render { struct CubemapProbe; }

class MapViewWidget;
class QPaintEvent;
class QResizeEvent;
class QTimer;

// Vulkan 1.2 off-screen ray-query renderer used by the dedicated
// "Ray-Traced Preview" mode. The normal OpenGL viewport remains available as
// a fallback on systems without VK_KHR_acceleration_structure +
// VK_KHR_ray_query support.
class VulkanRayTracedViewport final : public QWidget
{
    Q_OBJECT
public:
    explicit VulkanRayTracedViewport(MapViewWidget* owner);
    ~VulkanRayTracedViewport() override;

    void invalidateMaterialCache();
    void invalidateGeometryCache();
    void requestUpdate(bool rerender = true);
    QString rendererDescription() const;
    bool hardwareRayTracingAvailable() const;

    // Ray-traces six faces per probe for the OpenGL viewport to reflect in place
    // of the sky approximation. Synchronous: a bake is a user-initiated command,
    // not part of a paint.
    bool bakeCubemaps(const std::vector<hammer::render::CubemapProbe>& probes,
                      std::vector<hammer::assets::CubeImage>& output, QString& error);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class Renderer;

    bool ensureRenderer();
    bool renderFrame();
    // True while a modal dialog owns input: the preview then reuses its last
    // frame rather than competing with the dialog for the GUI thread.
    static bool renderingSuspended();
    void releaseRenderer();
    void setRendererError(const QString& error);

    MapViewWidget* owner_{nullptr};
    std::unique_ptr<Renderer> renderer_;
    QImage frame_;
    QString rendererError_;
    QTimer* animationTimer_{nullptr};
    bool sceneDirty_{true};
    bool frameDirty_{true};
};
