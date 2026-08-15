#include "MapViewWidget.hpp"
#include "Hardware3DViewport.hpp"
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
#include "EnvCubemap.hpp"
#include "VulkanRayTracedViewport.hpp"
#endif
#include "WaylandPointerLock.hpp"

#include <QColor>
#include <QCursor>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLine>
#include <QLineF>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QSettings>
#include <QTimer>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <QtMath>
#include <cmath>
#include <limits>

namespace {
// Grows a screen-space bounding box by one projected point. QRectF::united()
// cannot do this: a zero-size rectangle is null to Qt, and uniting with a null
// rectangle returns the other operand unchanged, so a box accumulated that way
// never grows past its first point.
void expandScreenBounds(QRectF& bounds, bool& hasPoint, const QPointF& point)
{
    if (!hasPoint) {
        bounds = QRectF(point, QSizeF(0.0, 0.0));
        hasPoint = true;
        return;
    }
    bounds = QRectF(QPointF(std::min(bounds.left(), point.x()), std::min(bounds.top(), point.y())),
                    QPointF(std::max(bounds.right(), point.x()), std::max(bounds.bottom(), point.y())));
}

constexpr double HalfBlockDepth = 64.0;
constexpr double Pi = 3.14159265358979323846;
const QColor ViewBackground2D(0, 0, 36);
const QColor ViewBackground3D(12, 12, 12);
const QColor MinorGrid(22, 32, 62);
const QColor MajorGrid(45, 58, 92);
const QColor BrushWire(220, 220, 220);
const QColor SelectedWire(255, 240, 32);
const QColor CreationWire(64, 224, 255);
// CMapFace::Render3D tints a selected face with SELECTED_FACE_COLOR, a flat
// red overlaid on the material.
const QColor SelectedFaceTint(200, 32, 32);
// The smoothing group tint, deliberately unlike the red face selection so both
// can be on screen at once.
const QColor SmoothingGroupFaceTint(48, 160, 220);

double pointSegmentDistance(const QPointF& point, const QPointF& a, const QPointF& b)
{
    const QPointF segment = b - a;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared < 0.0001) return QLineF(point, a).length();
    const double t = std::clamp(QPointF::dotProduct(point - a, segment) / lengthSquared, 0.0, 1.0);
    return QLineF(point, a + segment * t).length();
}

QPolygonF convexHull(std::vector<QPointF> points)
{
    if (points.size() < 3) {
        QPolygonF polygon;
        for (const QPointF& point : points) polygon << point;
        return polygon;
    }
    std::sort(points.begin(), points.end(), [](const QPointF& a, const QPointF& b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    auto cross = [](const QPointF& o, const QPointF& a, const QPointF& b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<QPointF> hull(points.size() * 2);
    std::size_t count = 0;
    for (const QPointF& point : points) {
        while (count >= 2 && cross(hull[count - 2], hull[count - 1], point) <= 0.0) --count;
        hull[count++] = point;
    }
    const std::size_t lower = count + 1;
    for (auto it = points.rbegin() + 1; it != points.rend(); ++it) {
        while (count >= lower && cross(hull[count - 2], hull[count - 1], *it) <= 0.0) --count;
        hull[count++] = *it;
    }
    if (count > 1) --count;
    hull.resize(count);
    QPolygonF polygon;
    for (const QPointF& point : hull) polygon << point;
    return polygon;
}

hammer::vmf::Vec3 add(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

hammer::vmf::Vec3 subtract(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

hammer::vmf::Vec3 scale(const hammer::vmf::Vec3& value, double amount)
{
    return {value.x * amount, value.y * amount, value.z * amount};
}

double dot(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

hammer::vmf::Vec3 cross(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

double length(const hammer::vmf::Vec3& value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

hammer::vmf::Vec3 normalized(const hammer::vmf::Vec3& value)
{
    const double magnitude = length(value);
    if (magnitude < 1e-9) return {};
    return scale(value, 1.0 / magnitude);
}

std::optional<double> rayTriangleDistance(const hammer::vmf::Vec3& origin,
                                          const hammer::vmf::Vec3& direction,
                                          const hammer::vmf::Vec3& a,
                                          const hammer::vmf::Vec3& b,
                                          const hammer::vmf::Vec3& c)
{
    // Two-sided Moller-Trumbore intersection. Point-entity helpers are editor
    // geometry, so picking must not depend on the material's culling state.
    constexpr double Epsilon = 1e-8;
    const hammer::vmf::Vec3 edge1 = subtract(b, a);
    const hammer::vmf::Vec3 edge2 = subtract(c, a);
    const hammer::vmf::Vec3 p = cross(direction, edge2);
    const double determinant = dot(edge1, p);
    if (std::abs(determinant) < Epsilon) return std::nullopt;
    const double inverse = 1.0 / determinant;
    const hammer::vmf::Vec3 t = subtract(origin, a);
    const double u = dot(t, p) * inverse;
    if (u < -Epsilon || u > 1.0 + Epsilon) return std::nullopt;
    const hammer::vmf::Vec3 q = cross(t, edge1);
    const double v = dot(direction, q) * inverse;
    if (v < -Epsilon || u + v > 1.0 + Epsilon) return std::nullopt;
    const double distance = dot(edge2, q) * inverse;
    return distance >= 0.0 ? std::optional<double>(distance) : std::nullopt;
}
}


MapViewWidget::MapViewWidget(Kind kind, QWidget* parent)
    : QWidget(parent), kind_(kind)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(96, 72);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QPalette viewPalette = palette();
    viewPalette.setColor(QPalette::Window,
                         kind_ == Kind::Perspective ? ViewBackground3D : ViewBackground2D);
    setPalette(viewPalette);
    setAutoFillBackground(true);

    hardwareViewport_ = new Hardware3DViewport(this);
    hardwareViewport_->setGeometry(rect());
    hardwareViewport_->show();
    if (kind_ == Kind::Perspective) {
        ensurePerspectiveResources();
        resetCamera();
    }
}

void MapViewWidget::ensurePerspectiveResources()
{
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (!rayTracedViewport_) {
        rayTracedViewport_ = new VulkanRayTracedViewport(this);
        rayTracedViewport_->setGeometry(rect());
        rayTracedViewport_->hide();
    }
#endif

    if (!waylandPointerLock_) waylandPointerLock_ = std::make_unique<WaylandPointerLock>(this);
    if (!flyTimer_) {
        flyTimer_ = new QTimer(this);
        flyTimer_->setTimerType(Qt::PreciseTimer);
        flyTimer_->setInterval(16);
        connect(flyTimer_, &QTimer::timeout, this, &MapViewWidget::updateFlyMovement);
    }

    // Water scroll offsets and animated texture frames are derived from a
        // clock inside the renderer, so they only advance when a frame is drawn.
        // Nothing else asks this view to redraw while idle, which left animated
        // materials frozen. Drive repaints at ~30fps; the texture browser's
        // existing animatePreviews setting turns it off.
    if (!animationTimer_ &&
        QSettings().value(QStringLiteral("textures/animatePreviews"), true).toBool()) {
        animationTimer_ = new QTimer(this);
        animationTimer_->setInterval(33);
        connect(animationTimer_, &QTimer::timeout, this, [this]() {
            if (isVisible() && materialRenderingEnabled_) requestRepaint();
        });
    }
    if (animationTimer_) animationTimer_->start();
}

void MapViewWidget::updateViewportSurface()
{
    const bool useRayTracing = kind_ == Kind::Perspective &&
        texturedRenderMode_ == TexturedRenderMode::RayTracedPreview && rayTracedViewport_;
    if (hardwareViewport_) hardwareViewport_->setVisible(!useRayTracing);
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_) {
        rayTracedViewport_->setVisible(useRayTracing);
        if (useRayTracing) rayTracedViewport_->requestUpdate(true);
    }
#endif
}

void MapViewWidget::setKind(Kind kind)
{
    if (kind_ == kind) return;
    // Flying and mouse capture belong to the 3D view this pane is about to stop
    // being; drop them before the kind changes so the release path still knows
    // what it is undoing.
    if (mouseCaptured_) setMouseCaptured(false);
    if (flyTimer_) flyTimer_->stop();
    pressedKeys_.clear();

    kind_ = kind;

    QPalette viewPalette = palette();
    viewPalette.setColor(QPalette::Window,
                         kind_ == Kind::Perspective ? ViewBackground3D : ViewBackground2D);
    setPalette(viewPalette);

    if (kind_ == Kind::Perspective) {
        ensurePerspectiveResources();
        // A pane that was showing an axis-aligned plane has no camera worth
        // keeping, so the new 3D view starts where a fresh one would, framing
        // the map if there is one.
        if (scene_) fitCameraToScene();
        else resetCamera();
    } else if (animationTimer_) {
        animationTimer_->stop();
    }

    updateViewportSurface();
    if (hardwareViewport_) hardwareViewport_->handleOwnerKindChanged();
    invalidateBaseFrame();
    update();
}

MapViewWidget::~MapViewWidget()
{
    if (mouseCaptured_) setMouseCaptured(false);
}

QString MapViewWidget::title() const
{
    switch (kind_) {
    case Kind::Perspective: return projectionMode_ == ProjectionMode::Perspective
        ? QStringLiteral("camera (perspective)") : QStringLiteral("camera (orthographic)");
    case Kind::Top: return QStringLiteral("top (x/y)");
    case Kind::Front: return QStringLiteral("front (y/z)");
    case Kind::Side: return QStringLiteral("side (x/z)");
    }
    return {};
}

void MapViewWidget::setActive(bool active)
{
    if (active_ == active) return;
    if (!active && mouseCaptured_) setMouseCaptured(false);
    active_ = active;
    requestRepaint(false);
}

void MapViewWidget::setGridVisible(bool visible)
{
    if (gridVisible_ == visible) return;
    gridVisible_ = visible;
    invalidateBaseFrame();
}

void MapViewWidget::setPointFile(std::vector<hammer::vmf::Vec3> points)
{
    pointFile_ = std::move(points);
    invalidateBaseFrame();
    update();
}

void MapViewWidget::setCollabPeers(QList<CollabPeerPose> peers)
{
    collabPeers_ = std::move(peers);
    // Plain repaint: overlays composite over the cached base frame in the
    // presentation pass, so a 4 Hz pose stream must never rebuild the scene
    // render underneath it.
    requestRepaint(false);
}

void MapViewWidget::drawCollabPeersOverlay(QPainter& painter)
{
    if (collabPeers_.isEmpty()) return;

    // The avatar model, fetched once. Missing content (stock Hammer ships
    // it, but odd game setups may not) falls back to a drawn frustum.
    if (!collabCameraModelTried_ && studioModels_) {
        collabCameraModelTried_ = true;
        collabCameraModel_ = studioModels_->model("models/editor/camera.mdl");
    }

    for (const CollabPeerPose& peer : collabPeers_) {
        const hammer::vmf::Vec3 origin{peer.x, peer.y, peer.z};
        const hammer::camera::SourceAngleBasis basis =
            hammer::camera::sourceAngleBasis({peer.pitch, peer.yaw, 0.0});
        const auto toWorld = [&](const hammer::vmf::Vec3& local) {
            const hammer::vmf::Vec3 rotated = basis.rotate(local);
            return hammer::vmf::Vec3{origin.x + rotated.x, origin.y + rotated.y,
                                     origin.z + rotated.z};
        };
        const auto drawWorldLine = [&](const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b) {
            if (kind_ == Kind::Perspective) {
                QLineF line;
                if (projectCameraLine(a, b, line)) painter.drawLine(line);
            } else {
                painter.drawLine(QLineF(toScreen(a), toScreen(b)));
            }
        };

        painter.setPen(QPen(QColor(80, 200, 255), 1));
        painter.setBrush(Qt::NoBrush);

        bool drewModel = false;
        if (kind_ == Kind::Perspective && collabCameraModel_ &&
            !collabCameraModel_->meshes.empty()) {
            // Wireframe of the reference pose, one edge per triangle rather
            // than all three: half the lines, same silhouette, and this runs
            // at the pose refresh rate over the cached frame.
            for (const hammer::assets::StudioMesh& mesh : collabCameraModel_->meshes) {
                const auto& vertices = mesh.vertices;
                for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
                    const hammer::vmf::Vec3 a =
                        toWorld({vertices[i].x, vertices[i].y, vertices[i].z});
                    const hammer::vmf::Vec3 b =
                        toWorld({vertices[i + 1].x, vertices[i + 1].y, vertices[i + 1].z});
                    const hammer::vmf::Vec3 c =
                        toWorld({vertices[i + 2].x, vertices[i + 2].y, vertices[i + 2].z});
                    drawWorldLine(a, b);
                    drawWorldLine(b, c);
                }
            }
            drewModel = true;
        }
        if (!drewModel) {
            // Fallback (and all 2D views): a small camera-shaped frustum —
            // a box body plus four lines opening along the view direction.
            constexpr double kBody = 8.0;
            const hammer::vmf::Vec3 corners[8] = {
                toWorld({-kBody, -kBody, -kBody}), toWorld({-kBody, kBody, -kBody}),
                toWorld({-kBody, kBody, kBody}),   toWorld({-kBody, -kBody, kBody}),
                toWorld({kBody, -kBody, -kBody}),  toWorld({kBody, kBody, -kBody}),
                toWorld({kBody, kBody, kBody}),    toWorld({kBody, -kBody, kBody})};
            static constexpr int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                                  {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                                  {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& edge : kEdges) drawWorldLine(corners[edge[0]], corners[edge[1]]);
            const hammer::vmf::Vec3 tip = toWorld({48.0, 0.0, 0.0});
            for (int i = 4; i < 8; ++i) drawWorldLine(corners[i], tip);
        }

        // View-direction reference line, useful at any distance.
        drawWorldLine(origin, toWorld({96.0, 0.0, 0.0}));

        // Name tag above the avatar.
        const hammer::vmf::Vec3 tagAnchor = toWorld({0.0, 0.0, 20.0});
        QPointF tagPoint;
        bool tagVisible = false;
        if (kind_ == Kind::Perspective) {
            const auto projected = hammer::camera::projectPoint(
                cameraState_, projectionMode_, tagAnchor, width(), height());
            if (projected) {
                tagPoint = QPointF(projected->x, projected->y);
                tagVisible = true;
            }
        } else {
            tagPoint = toScreen(tagAnchor);
            tagVisible = true;
        }
        if (tagVisible && !peer.name.isEmpty()) {
            painter.setPen(QColor(0, 0, 0, 180));
            painter.drawText(tagPoint + QPointF(1.0, -5.0), peer.name);
            painter.setPen(QColor(190, 235, 255));
            painter.drawText(tagPoint + QPointF(0.0, -6.0), peer.name);
        }
    }
}

void MapViewWidget::drawPointFileOverlay(QPainter& painter)
{
    // CMapView2D::DrawPointFile draws the trace in pure red. The original only
    // renders it in the 2D views; the perspective view gets it too here, which
    // is what the documentation describes and what makes the leak easy to
    // follow while flying.
    if (pointFile_.size() < 2) return;
    painter.setPen(QPen(QColor(255, 0, 0), 3));
    painter.setBrush(Qt::NoBrush);
    if (kind_ == Kind::Perspective) {
        for (std::size_t index = 1; index < pointFile_.size(); ++index) {
            QLineF line;
            if (projectCameraLine(pointFile_[index - 1], pointFile_[index], line))
                painter.drawLine(line);
        }
        return;
    }
    QPolygonF polyline;
    polyline.reserve(static_cast<int>(pointFile_.size()));
    for (const hammer::vmf::Vec3& point : pointFile_) polyline.append(toScreen(point));
    painter.drawPolyline(polyline);
}

void MapViewWidget::setPortalFile(std::vector<std::vector<hammer::vmf::Vec3>> portals)
{
    portalFile_ = std::move(portals);
    invalidateBaseFrame();
    update();
}

void MapViewWidget::drawPortalFileOverlay(QPainter& painter)
{
    // Glview draws the portal winding of every visleaf; here each portal is a
    // closed cyan outline, thin enough that a whole map's worth of them stays
    // readable, and unfilled so the geometry underneath still shows through.
    if (portalFile_.empty()) return;
    painter.setPen(QPen(QColor(0, 220, 220), 1));
    painter.setBrush(Qt::NoBrush);
    for (const std::vector<hammer::vmf::Vec3>& portal : portalFile_) {
        if (portal.size() < 3) continue;
        if (kind_ == Kind::Perspective) {
            for (std::size_t index = 0; index < portal.size(); ++index) {
                // The last edge closes the winding back onto its first point.
                const hammer::vmf::Vec3& from = portal[index];
                const hammer::vmf::Vec3& to = portal[(index + 1) % portal.size()];
                QLineF line;
                if (projectCameraLine(from, to, line)) painter.drawLine(line);
            }
            continue;
        }
        QPolygonF polygon;
        polygon.reserve(static_cast<int>(portal.size()));
        for (const hammer::vmf::Vec3& point : portal) polygon.append(toScreen(point));
        painter.drawPolygon(polygon);
    }
}

void MapViewWidget::setViewLabelVisible(bool visible)
{
    if (viewLabelVisible_ == visible) return;
    viewLabelVisible_ = visible;
    invalidateBaseFrame();
    update();
}

void MapViewWidget::setVerticalFieldOfView(double degrees)
{
    const double radians = qDegreesToRadians(std::clamp(degrees, 10.0, 170.0));
    if (std::abs(cameraState_.verticalFovRadians - radians) < 1e-9) return;
    cameraState_.verticalFovRadians = radians;
    invalidateBaseFrame();
    update();
}

void MapViewWidget::setRenderingPaused(bool paused)
{
    if (renderingPaused_ == paused) return;
    renderingPaused_ = paused;
    if (!paused) requestRepaint(true);
}

void MapViewWidget::setGridSnapEnabled(bool enabled)
{
    gridSnapEnabled_ = enabled;
}

void MapViewWidget::setGridSpacing(int spacing)
{
    spacing = std::clamp(spacing, 1, 512);
    if (gridSpacing_ == spacing) return;
    gridSpacing_ = spacing;
    invalidateBaseFrame();
}

double MapViewWidget::snap(double value) const
{
    if (!gridSnapEnabled_) return value;
    return std::round(value / gridSpacing_) * gridSpacing_;
}

QPointF MapViewWidget::snapped(const QPointF& point) const
{
    return {snap(point.x()), snap(point.y())};
}

void MapViewWidget::setScene(std::shared_ptr<const hammer::vmf::Scene> scene, bool fit)
{
    scene_ = std::move(scene);
    // No invalidateGeometryCache() here. The hardware renderer decides what to
    // rebuild from the Scene's revision lineage, so an incremental edit - a
    // drag, a nudge, a resize - re-uploads only the objects that moved instead
    // of the whole map, once per view, per mouse-move.
    if (hardwareViewport_) hardwareViewport_->requestUpdate(true);
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    // The ray tracer has no incremental path; its BVH rebuild is deferred to
    // the next paint and only happens while the ray-traced preview is visible.
    if (rayTracedViewport_) rayTracedViewport_->invalidateGeometryCache();
#endif
    if (fit) fitScene();
    else invalidateBaseFrame();
}

void MapViewWidget::setMaterialSystem(std::shared_ptr<hammer::assets::MaterialSystem> materials)
{
    if (materials_ == materials) return;
    materials_ = std::move(materials);
    collabCameraModel_.reset();
    collabCameraModelTried_ = false;
    studioModels_ = materials_ && materials_->fileSystem()
        ? std::make_shared<hammer::assets::StudioModelSystem>(materials_->fileSystem())
        : nullptr;
    if (hardwareViewport_) hardwareViewport_->invalidateMaterialCache();
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_) rayTracedViewport_->invalidateMaterialCache();
#endif
    invalidateBaseFrame();
}

void MapViewWidget::setMaterialRenderingEnabled(bool enabled)
{
    if (materialRenderingEnabled_ == enabled) return;
    materialRenderingEnabled_ = enabled;
    invalidateBaseFrame();
}

void MapViewWidget::setWireframeOverlayEnabled(bool enabled)
{
    if (wireframeOverlayEnabled_ == enabled) return;
    wireframeOverlayEnabled_ = enabled;
    requestRepaint(false);
}

void MapViewWidget::setDisplacementSolidMaskEnabled(bool enabled)
{
    if (displacementSolidMaskEnabled_ == enabled) return;
    displacementSolidMaskEnabled_ = enabled;
    // CMapDoc::OnToggleDispSolidMask only issues MAPVIEW_UPDATE_ONLY_3D, so the
    // 2D wireframe views keep drawing every side of a displacement solid.
    if (hardwareViewport_) hardwareViewport_->invalidateGeometryCache();
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_) rayTracedViewport_->invalidateGeometryCache();
#endif
    invalidateBaseFrame();
}

void MapViewWidget::setTexturedRenderMode(TexturedRenderMode mode)
{
    if (texturedRenderMode_ == mode) return;
    texturedRenderMode_ = mode;
    updateViewportSurface();
    requestRepaint();
}

void MapViewWidget::setHdrEnabled(bool enabled)
{
    if (hdrEnabled_ == enabled) return;
    hdrEnabled_ = enabled;
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    // HDR selects which light entity keys apply, so the scene's lights have to
    // be rebuilt - this is not just a post-processing switch.
    if (rayTracedViewport_) {
        rayTracedViewport_->invalidateMaterialCache();
        rayTracedViewport_->requestUpdate(true);
    }
#endif
    requestRepaint();
}

void MapViewWidget::setDetailPropsVisible(bool visible)
{
    if (detailPropsVisible_ == visible) return;
    detailPropsVisible_ = visible;
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    // Detail props are baked into the RT scene's static geometry, so toggling
    // them has to rebuild the scene. requestUpdate alone only redraws the
    // frame from the geometry that is already uploaded.
    if (rayTracedViewport_) {
        rayTracedViewport_->invalidateGeometryCache();
        rayTracedViewport_->requestUpdate(true);
    }
#endif
    requestRepaint();
}

void MapViewWidget::setRayTracedGamma(float gamma)
{
    gamma = std::clamp(gamma, 0.5f, 5.0f);
    if (qFuzzyCompare(rayTracedGamma_, gamma)) return;
    rayTracedGamma_ = gamma;
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    // Purely a composite-pass exponent, so the scene and its materials stay
    // valid - only the frame has to be redrawn.
    if (rayTracedViewport_) rayTracedViewport_->requestUpdate(true);
#endif
    requestRepaint();
}

void MapViewWidget::setMaterialEffectsEnabled(bool phong, bool specular, bool bumpMaps,
                                              bool lightWarp, bool selfIllum, bool rimLight)
{
    if (phongEnabled_ == phong && specularEnabled_ == specular &&
        bumpMapsEnabled_ == bumpMaps && lightWarpEnabled_ == lightWarp &&
        selfIllumEnabled_ == selfIllum && rimLightEnabled_ == rimLight) return;
    phongEnabled_ = phong;
    specularEnabled_ = specular;
    bumpMapsEnabled_ = bumpMaps;
    lightWarpEnabled_ = lightWarp;
    selfIllumEnabled_ = selfIllum;
    rimLightEnabled_ = rimLight;
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_) rayTracedViewport_->requestUpdate(true);
#endif
    requestRepaint();
}

void MapViewWidget::setMaterialEffectIntensities(float phong, float specular, float bumpMaps)
{
    const float clampedPhong = std::clamp(phong, 0.0f, 4.0f);
    const float clampedSpecular = std::clamp(specular, 0.0f, 4.0f);
    const float clampedBumpMaps = std::clamp(bumpMaps, 0.0f, 4.0f);
    if (std::abs(phongIntensity_ - clampedPhong) < 0.0001f &&
        std::abs(specularIntensity_ - clampedSpecular) < 0.0001f &&
        std::abs(bumpMapIntensity_ - clampedBumpMaps) < 0.0001f) return;
    phongIntensity_ = clampedPhong;
    specularIntensity_ = clampedSpecular;
    bumpMapIntensity_ = clampedBumpMaps;
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_) rayTracedViewport_->requestUpdate(true);
#endif
    requestRepaint();
}

void MapViewWidget::setHiddenToolTextures(
    const std::unordered_set<std::string>& hiddenToolTextures)
{
    if (hiddenToolTextures_ == hiddenToolTextures) return;
    hiddenToolTextures_ = hiddenToolTextures;
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    // Tool visibility is stored in per-triangle RT flags, so changing the menu
    // state requires a scene rebuild while preserving shadow-only nodraw geometry.
    if (rayTracedViewport_) rayTracedViewport_->invalidateGeometryCache();
#endif
    invalidateBaseFrame();
}

bool MapViewWidget::materialVisible(std::string_view material) const
{
    const std::string normalized = hammer::vmf::normalizeMaterialPath(material);
    return normalized.rfind("tools/", 0) != 0 ||
           hiddenToolTextures_.find(normalized) == hiddenToolTextures_.end();
}

void MapViewWidget::setSelection(const std::vector<hammer::vmf::ObjectRef>& selection,
                                 const hammer::vmf::Bounds& bounds)
{
    selection_ = selection;
    selectionBounds_ = bounds;
    if (bounds.valid) lastReferenceBounds_ = bounds;

    // Selection outlines (entity bounds boxes, displacement vertex marks) are
    // part of the GPU frame in every view, so the hardware frame must be
    // re-rendered as soon as the selection changes.
    requestRepaint(true);
}

void MapViewWidget::setFaceSelection(const std::vector<hammer::vmf::FaceRef>& faces)
{
    faceSelection_ = faces;
    // The displacement vertex marks keyed on the face selection are part of
    // the GPU frame too.
    requestRepaint(true);
}

void MapViewWidget::setSmoothingGroupFaces(const std::vector<hammer::vmf::FaceRef>& faces)
{
    smoothingGroupFaces_ = faces;
    requestRepaint(false);
}

void MapViewWidget::setFaceSelectionMaskHidden(bool hidden)
{
    if (faceSelectionMaskHidden_ == hidden) return;
    faceSelectionMaskHidden_ = hidden;
    requestRepaint(false);
}

void MapViewWidget::setDisplacementPaintActive(bool active)
{
    if (displacementPaintActive_ == active) return;
    displacementPaintActive_ = active;
    if (!active && displacementPainting_) {
        displacementPainting_ = false;
        emit displacementPaintFinished();
    }
}

hammer::vmf::Vec3 MapViewWidget::viewRight() const
{
    return hammer::camera::rightVector(cameraState_);
}

hammer::vmf::Vec3 MapViewWidget::viewUp() const
{
    return hammer::camera::upVector(cameraState_);
}

hammer::vmf::Vec3 MapViewWidget::viewPoint() const
{
    return cameraState_.position;
}

// CMapFace::Render3D's selected-face pass. The hardware renderer has no
// per-face tint, so the tint is painted over the finished 3D frame the same way
// the selection wireframe already is.
void MapViewWidget::drawFaceSelectionOverlay(QPainter& painter)
{
    if (faceSelectionMaskHidden_) return;
    // The smoothing group tint goes down first so the red face selection stays
    // readable on top of it.
    drawFaceTintOverlay(painter, smoothingGroupFaces_, SmoothingGroupFaceTint);
    drawFaceTintOverlay(painter, faceSelection_, SelectedFaceTint);
}

bool MapViewWidget::canBuildCubemaps() const
{
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    return rayTracedViewport_ != nullptr && scene_ != nullptr;
#else
    return false;
#endif
}

bool MapViewWidget::buildCubemaps(QString& error)
{
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (!rayTracedViewport_) {
        error = QStringLiteral("Building cubemaps needs the ray-traced renderer, "
                               "which is unavailable on this system");
        return false;
    }
    if (!scene_) {
        error = QStringLiteral("No map is loaded");
        return false;
    }

    const std::vector<hammer::render::CubemapProbe> probes =
        hammer::render::collectCubemapProbes(*scene_);
    if (probes.empty()) {
        error = QStringLiteral("The map contains no env_cubemap entities");
        return false;
    }

    std::vector<hammer::assets::CubeImage> cubes;
    if (!rayTracedViewport_->bakeCubemaps(probes, cubes, error)) return false;

    // Probes whose bake failed are dropped rather than kept as black cubes, so
    // a partial bake still leaves every surface on a real reflection.
    std::vector<hammer::render::BakedCubemap> baked;
    baked.reserve(cubes.size());
    for (std::size_t index = 0; index < cubes.size() && index < probes.size(); ++index) {
        if (!cubes[index].valid()) continue;
        baked.push_back({probes[index].origin, std::move(cubes[index])});
    }
    if (baked.empty()) {
        error = QStringLiteral("No env_cubemap probe could be rendered");
        return false;
    }

    if (hardwareViewport_) hardwareViewport_->setBakedCubemaps(std::move(baked));
    update();
    return true;
#else
    Q_UNUSED(error);
    error = QStringLiteral("This build has no ray-traced renderer");
    return false;
#endif
}

void MapViewWidget::setLightmapGridVisible(bool visible)
{
    if (lightmapGridVisible_ == visible) return;
    lightmapGridVisible_ = visible;
    // The grid is part of the GPU material pass now, so the 3D frame must be
    // re-rendered, not just recomposited.
    requestRepaint(true);
}

void MapViewWidget::drawFaceTintOverlay(QPainter& painter,
                                        const std::vector<hammer::vmf::FaceRef>& faces,
                                        const QColor& tint)
{
    if (!scene_ || faces.empty()) return;
    if (kind_ != Kind::Perspective) return;

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(tint.lighter(140), 1));
    QColor fill = tint;
    fill.setAlpha(110);
    painter.setBrush(fill);

    for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
        const bool listed = std::any_of(faces.begin(), faces.end(),
            [&](const hammer::vmf::FaceRef& face) { return face.solidId == brush.id; });
        if (!listed) continue;
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            if (std::none_of(faces.begin(), faces.end(),
                    [&](const hammer::vmf::FaceRef& selected) {
                        return selected.solidId == brush.id && selected.sideId == face.sideId;
                    })) {
                continue;
            }
            if (face.vertices.size() < 3) continue;
            // Back faces are invisible in the 3D view; do not paint through the solid.
            const hammer::vmf::Vec3 toCamera{cameraState_.position.x - brush.vertices[face.vertices.front()].x,
                                             cameraState_.position.y - brush.vertices[face.vertices.front()].y,
                                             cameraState_.position.z - brush.vertices[face.vertices.front()].z};
            if (face.normal.x * toCamera.x + face.normal.y * toCamera.y +
                face.normal.z * toCamera.z <= 0.0) {
                continue;
            }
            QPolygonF polygon;
            bool visible = true;
            for (const std::size_t index : face.vertices) {
                if (index >= brush.vertices.size()) { visible = false; break; }
                const auto projected = projectCameraPoint(brush.vertices[index]);
                if (!projected) { visible = false; break; }
                polygon << *projected;
            }
            if (visible && polygon.size() >= 3) painter.drawPolygon(polygon);
        }
    }
}

void MapViewWidget::setSelectionMode(SelectionMode mode)
{
    selectionMode_ = mode;
    requestRepaint(kind_ != Kind::Perspective);
}

void MapViewWidget::setTool(Tool tool)
{
    if (tool_ == tool) return;
    tool_ = tool;
    // Clipper3D::OnDeactivate calls SetEmpty; the clip line does not survive a
    // tool change.
    clipActive_ = false;
    clipDragging_ = false;
    clipPointHit_ = -1;
    clipKept_.clear();
    clipDiscarded_.clear();
    // Morph3D::OnDeactivate -> SetEmpty: the vertex meshes do not survive a
    // tool change either. The document commits them before telling the views.
    morphActive_ = false;
    morphHandles_.clear();
    morphPreview_.clear();
    if (tool_ != Tool::TextureApplication) faceSelection_.clear();
    morphDragHandle_ = -1;
    morphDragging_ = false;
    morphBoxSelecting_ = false;
    resetInteraction();
    if (!mouseCaptured_) {
        if (tool_ == Tool::Magnify) {
            // ToolMagnify::OnMouseMove2D loads Resource/magnify.cur; Qt has no
            // bundled magnifier glyph, so the closest stock cursor is used.
            setCursor(Qt::CrossCursor);
        } else {
            // CToolMaterial::OnMouseMove3D loads IDC_FACEPAINT; Qt has no
            // equivalent glyph, so the paint-ish pointing hand stands in.
            setCursor(tool_ == Tool::TextureApplication ? Qt::PointingHandCursor
                      : (tool_ == Tool::Entity || tool_ == Tool::Block ||
                         tool_ == Tool::Decal || tool_ == Tool::Overlay
                             ? Qt::CrossCursor : Qt::ArrowCursor));
        }
    }
    requestRepaint(kind_ != Kind::Perspective);
}

void MapViewWidget::setTransformMode(TransformMode mode)
{
    transformMode_ = mode;
    requestRepaint(kind_ != Kind::Perspective);
}

void MapViewWidget::setProjectionMode(ProjectionMode mode)
{
    if (kind_ != Kind::Perspective || projectionMode_ == mode) return;
    projectionMode_ = mode;
    invalidateBaseFrame();
    reportCameraStatus();
}

void MapViewWidget::resetView()
{
    if (kind_ == Kind::Perspective) {
        resetCamera();
    } else {
        pan_ = {};
        zoom_ = 1.0;
    }
    fitPending_ = false;
    invalidateBaseFrame();
}

void MapViewWidget::fitScene()
{
    fitPending_ = true;
    invalidateBaseFrame();
}

void MapViewWidget::applyPendingFit()
{
    if (!fitPending_) return;
    fitPending_ = false;

    if (kind_ == Kind::Perspective) {
        fitCameraToScene();
        return;
    }

    if (!scene_ || !scene_->hasBounds || width() < 20 || height() < 20) {
        pan_ = {};
        zoom_ = 1.0;
        return;
    }

    const hammer::vmf::Vec3 corners[] = {
        {scene_->minimum.x, scene_->minimum.y, scene_->minimum.z},
        {scene_->minimum.x, scene_->minimum.y, scene_->maximum.z},
        {scene_->minimum.x, scene_->maximum.y, scene_->minimum.z},
        {scene_->minimum.x, scene_->maximum.y, scene_->maximum.z},
        {scene_->maximum.x, scene_->minimum.y, scene_->minimum.z},
        {scene_->maximum.x, scene_->minimum.y, scene_->maximum.z},
        {scene_->maximum.x, scene_->maximum.y, scene_->minimum.z},
        {scene_->maximum.x, scene_->maximum.y, scene_->maximum.z}
    };

    double minU = std::numeric_limits<double>::max();
    double minV = std::numeric_limits<double>::max();
    double maxU = std::numeric_limits<double>::lowest();
    double maxV = std::numeric_limits<double>::lowest();
    for (const auto& corner : corners) {
        const QPointF projected = project(corner);
        minU = std::min(minU, projected.x());
        minV = std::min(minV, projected.y());
        maxU = std::max(maxU, projected.x());
        maxV = std::max(maxV, projected.y());
    }

    const double rangeU = std::max(64.0, maxU - minU);
    const double rangeV = std::max(64.0, maxV - minV);
    zoom_ = std::clamp(std::min((width() - 48.0) / rangeU, (height() - 48.0) / rangeV), 0.01, 32.0);
    const double centerU = (minU + maxU) * 0.5;
    const double centerV = (minV + maxV) * 0.5;
    pan_ = QPoint(static_cast<int>(std::lround(-centerU * zoom_)),
                  static_cast<int>(std::lround(centerV * zoom_)));
}

void MapViewWidget::paintEvent(QPaintEvent*)
{
    applyPendingFit();
    if (hardwareViewport_) {
        // The child off-screen OpenGL widget owns the cached GPU grid/3D frame.
        // The parent only maintains input and document state.
        return;
    }
    rebuildBaseFrame();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Always establish an opaque frame before drawing cached content. This is
    // intentionally redundant with the QImage fill and protects against an
    // invalid/empty cache during expose or device-pixel-ratio changes.
    painter.fillRect(rect(), kind_ == Kind::Perspective ? ViewBackground3D : ViewBackground2D);
    if (!baseFrame_.isNull()) painter.drawImage(QPoint(0, 0), baseFrame_);

    // Every overlay stage gets an isolated painter state. In v0.6.4/v0.6.5,
    // drawSelectionOverlay() left ViewBackground2D as the active brush after
    // drawing resize handles. The final drawRect() then filled the entire view,
    // exactly matching "grid disappears on select, returns on deselect".
    painter.save();
    drawFaceSelectionOverlay(painter);
    painter.restore();

    painter.save();
    drawSelectionOverlay(painter);
    painter.restore();

    painter.save();
    drawCreationPreview(painter);
    painter.restore();

    painter.save();
    drawBoxSelectOverlay(painter);
    painter.restore();

    painter.save();
    drawPortalFileOverlay(painter);
    painter.restore();

    painter.save();
    drawPointFileOverlay(painter);
    painter.restore();

    painter.save();
    drawCollabPeersOverlay(painter);
    painter.restore();

    painter.save();
    drawCameraOverlay(painter);
    painter.restore();

    painter.save();
    drawClipOverlay(painter);
    painter.restore();

    painter.save();
    drawMorphOverlay(painter);
    painter.restore();

    painter.save();
    drawViewLabel(painter);
    painter.restore();

    painter.save();
    drawZCameraCrosshair(painter);
    painter.restore();

    painter.setPen(QPen(active_ ? QColor(255, 255, 0) : QColor(112, 112, 112), active_ ? 2 : 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void MapViewWidget::invalidateBaseFrame()
{
    baseFrameDirty_ = true;
    requestRepaint();
}

void MapViewWidget::rebuildBaseFrame()
{
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(qMax(1, qCeil(width() * dpr)), qMax(1, qCeil(height() * dpr)));
    if (!baseFrameDirty_ && !baseFrame_.isNull() &&
        baseFrame_.size() == pixelSize && qFuzzyCompare(baseFrame_.devicePixelRatio(), dpr)) {
        return;
    }

    // Orthographic frames are fully owned by Hardware3DViewport. Do not even
    // build the legacy CPU image off-screen: brush/model-heavy maps previously
    // paid the entire QPainter cost despite that image no longer being shown.
    if (hardwareViewport_ && kind_ != Kind::Perspective) {
        baseFrame_ = QImage();
        baseFrameDirty_ = false;
        return;
    }

    baseFrame_ = QImage(pixelSize, QImage::Format_ARGB32_Premultiplied);
    baseFrame_.setDevicePixelRatio(dpr);
    baseFrame_.fill(kind_ == Kind::Perspective ? ViewBackground3D : ViewBackground2D);

    QPainter painter(&baseFrame_);
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (kind_ == Kind::Perspective) drawPerspective(painter);
    else drawOrthographic(painter);
    drawSceneBase(painter);
    painter.end();
    baseFrameDirty_ = false;
}

void MapViewWidget::drawOrthographic(QPainter& painter)
{
    const int spacing = std::max(4, static_cast<int>(std::lround(gridSpacing_ * zoom_)));
    const QPoint center = rect().center() + pan_;
    if (gridVisible_) {
        int startX = center.x() % spacing;
        if (startX < 0) startX += spacing;
        for (int x = startX; x < width(); x += spacing) {
            const int index = (x - center.x()) / spacing;
            painter.setPen((index % 8) == 0 ? MajorGrid : MinorGrid);
            painter.drawLine(x, 0, x, height());
        }
        int startY = center.y() % spacing;
        if (startY < 0) startY += spacing;
        for (int y = startY; y < height(); y += spacing) {
            const int index = (y - center.y()) / spacing;
            painter.setPen((index % 8) == 0 ? MajorGrid : MinorGrid);
            painter.drawLine(0, y, width(), y);
        }
    }
    painter.setPen(QPen(QColor(130, 36, 36), 1));
    painter.drawLine(0, center.y(), width(), center.y());
    painter.setPen(QPen(QColor(36, 130, 60), 1));
    painter.drawLine(center.x(), 0, center.x(), height());
}

void MapViewWidget::drawPerspective(QPainter& painter)
{
    if (!gridVisible_) return;

    // Hammer's 3D grid is a real world-space X/Y plane, not a screen-space
    // horizon fan. Keep it centered near the camera so it remains useful while
    // flying far from the origin.
    constexpr double spacing = 64.0;
    constexpr int halfLineCount = 80;
    const double centerX = std::floor(cameraState_.position.x / spacing) * spacing;
    const double centerY = std::floor(cameraState_.position.y / spacing) * spacing;
    const double extent = spacing * halfLineCount;

    for (int index = -halfLineCount; index <= halfLineCount; ++index) {
        const double x = centerX + index * spacing;
        const int absoluteIndex = static_cast<int>(std::llround(x / spacing));
        painter.setPen(QPen((absoluteIndex % 8) == 0 ? MajorGrid : MinorGrid, 1));
        QLineF line;
        if (projectCameraLine({x, centerY - extent, 0.0}, {x, centerY + extent, 0.0}, line)) {
            painter.drawLine(line);
        }
    }
    for (int index = -halfLineCount; index <= halfLineCount; ++index) {
        const double y = centerY + index * spacing;
        const int absoluteIndex = static_cast<int>(std::llround(y / spacing));
        painter.setPen(QPen((absoluteIndex % 8) == 0 ? MajorGrid : MinorGrid, 1));
        QLineF line;
        if (projectCameraLine({centerX - extent, y, 0.0}, {centerX + extent, y, 0.0}, line)) {
            painter.drawLine(line);
        }
    }

    QLineF axis;
    painter.setPen(QPen(QColor(150, 44, 44), 2));
    if (projectCameraLine({centerX - extent, 0.0, 0.0}, {centerX + extent, 0.0, 0.0}, axis)) {
        painter.drawLine(axis);
    }
    painter.setPen(QPen(QColor(44, 150, 68), 2));
    if (projectCameraLine({0.0, centerY - extent, 0.0}, {0.0, centerY + extent, 0.0}, axis)) {
        painter.drawLine(axis);
    }
}

void MapViewWidget::drawSceneBase(QPainter& painter)
{
    drawBrushWireframe(painter);
    drawEntityMarkers(painter);
}

void MapViewWidget::drawBrushWireframe(QPainter& painter)
{
    if (!scene_) return;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(BrushWire, 1));
    auto drawWorldLine = [&](const hammer::vmf::Vec3& first,
                             const hammer::vmf::Vec3& second) {
        if (kind_ == Kind::Perspective) {
            QLineF line;
            if (projectCameraLine(first, second, line)) painter.drawLine(line);
        } else {
            painter.drawLine(toScreen(first), toScreen(second));
        }
    };
    for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
        // A solid whose every face uses a hidden tool material is treated like a
        // hidden object: not drawn, and (see hitTestAll/objectsInBox) not
        // selectable in this view.
        if (brushHiddenByToolTextures(brush)) continue;
        for (const auto& edge : brush.edges) {
            if (edge.first >= brush.vertices.size() || edge.second >= brush.vertices.size()) continue;
            drawWorldLine(brush.vertices[edge.first], brush.vertices[edge.second]);
        }
        // When the optional 3D wireframe overlay is enabled, show the actual
        // displacement tessellation rather than only the flat source brush.
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            if (!face.displacement || face.displacementPower < 1) continue;
            const int gridSize = (1 << face.displacementPower) + 1;
            if (face.displacementVertices.size() !=
                static_cast<std::size_t>(gridSize * gridSize)) continue;
            for (int y = 0; y < gridSize; ++y) {
                for (int x = 0; x < gridSize; ++x) {
                    const std::size_t index = static_cast<std::size_t>(y * gridSize + x);
                    if (x + 1 < gridSize) {
                        drawWorldLine(face.displacementVertices[index].position,
                                      face.displacementVertices[index + 1].position);
                    }
                    if (y + 1 < gridSize) {
                        drawWorldLine(face.displacementVertices[index].position,
                                      face.displacementVertices[index + gridSize].position);
                    }
                }
            }
        }
    }
}

void MapViewWidget::drawEntityMarkers(QPainter& painter)
{
    if (!scene_ || !materials_) return;

    auto transformFor = [](const hammer::vmf::EntityMarker& entity) {
        const hammer::camera::SourceTransform transform =
            hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
        const auto& basis = transform.basis;
        return QMatrix4x4(
            static_cast<float>(basis.forward.x), static_cast<float>(basis.left.x),
            static_cast<float>(basis.up.x), static_cast<float>(entity.origin.x),
            static_cast<float>(basis.forward.y), static_cast<float>(basis.left.y),
            static_cast<float>(basis.up.y), static_cast<float>(entity.origin.y),
            static_cast<float>(basis.forward.z), static_cast<float>(basis.left.z),
            static_cast<float>(basis.up.z), static_cast<float>(entity.origin.z),
            0.0f, 0.0f, 0.0f, 1.0f);
    };
    auto worldPoint = [](const QVector3D& point) {
        return hammer::vmf::Vec3{point.x(), point.y(), point.z()};
    };
    auto imageFor = [](const hammer::assets::Image& source) {
        QImage image(source.width, source.height, QImage::Format_ARGB32);
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                image.setPixel(x, y, source.pixels[static_cast<std::size_t>(y * source.width + x)]);
            }
        }
        return image;
    };

    for (const hammer::vmf::EntityMarker& entity : scene_->entities) {
        const QColor entityColor(std::clamp(entity.displayColor[0], 0, 255),
                                 std::clamp(entity.displayColor[1], 0, 255),
                                 std::clamp(entity.displayColor[2], 0, 255));
        bool helperDrawn = false;
        if (!entity.projectedSurfaces.empty()) {
            helperDrawn = true;
            if (kind_ != Kind::Perspective || !materialRenderingEnabled_ ||
                isSelected(entity.object)) {
                painter.setPen(QPen(isSelected(entity.object) ? SelectedWire : entityColor,
                                    isSelected(entity.object) ? 2 : 1));
                painter.setBrush(Qt::NoBrush);
                for (const auto& surface : entity.projectedSurfaces) {
                    for (std::size_t index = 0; index + 2 < surface.triangles.size(); index += 3) {
                        const hammer::vmf::Vec3 points[3] = {
                            surface.triangles[index].position,
                            surface.triangles[index + 1].position,
                            surface.triangles[index + 2].position};
                        for (const auto edge : {std::pair{0, 1}, std::pair{1, 2}, std::pair{2, 0}}) {
                            if (kind_ == Kind::Perspective) {
                                QLineF line;
                                if (projectCameraLine(points[edge.first], points[edge.second], line))
                                    painter.drawLine(line);
                            } else {
                                painter.drawLine(toScreen(points[edge.first]), toScreen(points[edge.second]));
                            }
                        }
                    }
                }
            }
        }
        if (!helperDrawn && !entity.model.empty() && studioModels_) {
            const auto model = studioModels_->model(entity.model);
            if (model && model->valid) {
                const QMatrix4x4 transform = transformFor(entity);
                painter.setPen(QPen(entityColor, isSelected(entity.object) ? 2 : 1));
                painter.setBrush(Qt::NoBrush);
                for (const hammer::assets::StudioMesh& mesh : model->meshes) {
                    for (std::size_t index = 0; index + 2 < mesh.vertices.size(); index += 3) {
                        const auto& a = mesh.vertices[index];
                        const auto& b = mesh.vertices[index + 1];
                        const auto& c = mesh.vertices[index + 2];
                        const hammer::vmf::Vec3 points[] = {
                            worldPoint(transform * QVector3D(a.x, a.y, a.z)),
                            worldPoint(transform * QVector3D(b.x, b.y, b.z)),
                            worldPoint(transform * QVector3D(c.x, c.y, c.z))};
                        for (const auto edge : {std::pair{0, 1}, std::pair{1, 2}, std::pair{2, 0}}) {
                            if (kind_ == Kind::Perspective) {
                                QLineF line;
                                if (projectCameraLine(points[edge.first], points[edge.second], line))
                                    painter.drawLine(line);
                            } else {
                                painter.drawLine(toScreen(points[edge.first]), toScreen(points[edge.second]));
                            }
                        }
                    }
                }
                helperDrawn = true;
            }
        }
        if (!helperDrawn && !entity.sprite.empty()) {
            const auto material = materials_->material(entity.sprite);
            if (material && material->image.valid()) {
                if (const auto target = spriteScreenBounds(entity)) {
                    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    painter.drawImage(*target, imageFor(material->image));
                    if (isSelected(entity.object)) {
                        painter.setPen(QPen(SelectedWire, 1));
                        painter.setBrush(Qt::NoBrush);
                        painter.drawRect(*target);
                    }
                    helperDrawn = true;
                }
            }
        }

        if (helperDrawn && isSelected(entity.object)) {
            const std::optional<QPointF> point = kind_ == Kind::Perspective
                ? projectCameraPoint(entity.origin) : std::optional<QPointF>(toScreen(entity.origin));
            if (!point) continue;
            QString label = QString::fromStdString(entity.classname);
            if (!entity.targetName.empty()) label += QStringLiteral(" : ") + QString::fromStdString(entity.targetName);
            painter.setPen(SelectedWire);
            if (!label.isEmpty()) painter.drawText(*point + QPointF(9.0, -6.0), label);
            QString details = QString::fromStdString(entity.description).simplified();
            if (details.size() > 120) details = details.left(117) + QStringLiteral("...");
            if (!details.isEmpty()) painter.drawText(*point + QPointF(9.0, 10.0), details);
        }
    }
}

void MapViewWidget::drawSelectionOverlay(QPainter& painter)
{
    if (!scene_ || selection_.empty()) return;

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(SelectedWire, 2));
    auto drawSelectedWorldLine = [&](const hammer::vmf::Vec3& first,
                                     const hammer::vmf::Vec3& second) {
        if (kind_ == Kind::Perspective) {
            QLineF line;
            if (projectCameraLine(first, second, line)) painter.drawLine(line);
        } else {
            painter.drawLine(toScreen(first), toScreen(second));
        }
    };
    for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
        if (!isSelected(effectiveObject(brush))) continue;
        for (const auto& edge : brush.edges) {
            if (edge.first >= brush.vertices.size() || edge.second >= brush.vertices.size()) continue;
            drawSelectedWorldLine(brush.vertices[edge.first], brush.vertices[edge.second]);
        }
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            if (!face.displacement || face.displacementPower < 1) continue;
            const int gridSize = (1 << face.displacementPower) + 1;
            if (face.displacementVertices.size() !=
                static_cast<std::size_t>(gridSize * gridSize)) continue;
            for (int y = 0; y < gridSize; ++y) {
                for (int x = 0; x < gridSize; ++x) {
                    const std::size_t index = static_cast<std::size_t>(y * gridSize + x);
                    if (x + 1 < gridSize) {
                        drawSelectedWorldLine(face.displacementVertices[index].position,
                                              face.displacementVertices[index + 1].position);
                    }
                    if (y + 1 < gridSize) {
                        drawSelectedWorldLine(face.displacementVertices[index].position,
                                              face.displacementVertices[index + gridSize].position);
                    }
                }
            }
        }
    }

    // The hardware path draws a selected entity's bounds box itself (the red
    // corner box in both the perspective and orthographic renderers), so the
    // yellow QPainter rect only remains for the software fallback.
    if (!hardwareViewport_) {
        for (const hammer::vmf::EntityMarker& entity : scene_->entities) {
            if (!isSelected(entity.object)) continue;
            if (const auto bounds = entityScreenBounds(entity)) {
                painter.setPen(QPen(SelectedWire, 1, Qt::DashLine));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(*bounds);
            }
        }
    }

    const QRectF bounds = selectionScreenBounds();
    if (!bounds.isValid() || kind_ == Kind::Perspective || tool_ != Tool::Selection) return;

    // A single point entity already gets its dashed box above (or from the
    // hardware viewport); only the transform handles are added here.
    if (!isSinglePointEntitySelection()) {
        painter.setPen(QPen(SelectedWire, 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds);
    }
    const QPointF handles[] = {
        bounds.topLeft(), QPointF(bounds.center().x(), bounds.top()), bounds.topRight(),
        QPointF(bounds.right(), bounds.center().y()), bounds.bottomRight(),
        QPointF(bounds.center().x(), bounds.bottom()), bounds.bottomLeft(),
        QPointF(bounds.left(), bounds.center().y())
    };
    painter.setPen(QPen(SelectedWire, 1));
    painter.setBrush(ViewBackground2D);
    switch (effectiveTransformMode()) {
    case TransformMode::Scale:
        for (const QPointF& handle : handles) painter.drawRect(QRectF(handle.x() - 3, handle.y() - 3, 7, 7));
        break;
    case TransformMode::Translate:
        // Square handles floating just outside the middle of each side: the
        // side pair moves the selection horizontally only, the top/bottom
        // pair vertically only.
        for (const auto& [handle, position] : translateHandlePositions(bounds)) {
            Q_UNUSED(handle);
            painter.drawRect(QRectF(position.x() - 3, position.y() - 3, 7, 7));
        }
        break;
    case TransformMode::Rotate:
        for (int index : {0, 2, 4, 6}) painter.drawEllipse(handles[index], 5, 5);
        // Mark the actual rotation pivot (the selection centroid), which for
        // asymmetric shapes is not the AABB center.
        painter.drawEllipse(toScreen(selectionCentroidWorld()), 3, 3);
        break;
    }
}

void MapViewWidget::drawBoxSelectOverlay(QPainter& painter)
{
    // Box3D draws the in-progress selection box as a dashed outline in both the
    // 2D and 3D views.
    if (!boxSelecting_) return;
    painter.setBrush(Qt::NoBrush);
    QPen pen(SelectedWire, 1, Qt::DashLine);
    painter.setPen(pen);
    painter.drawRect(QRectF(boxSelectStart_, boxSelectCurrent_).normalized());
}

int MapViewWidget::missingWorldAxis() const
{
    return kind_ == Kind::Top ? 2 : kind_ == Kind::Front ? 0 : 1;
}

void MapViewWidget::planeWorldAxes(int& uAxis, int& vAxis) const
{
    // planePointToWorld's mapping, as axis indices: Top draws x/y, Front y/z,
    // Side x/z.
    switch (kind_) {
    case Kind::Top: uAxis = 0; vAxis = 1; break;
    case Kind::Front: uAxis = 1; vAxis = 2; break;
    case Kind::Side: uAxis = 0; vAxis = 2; break;
    case Kind::Perspective: uAxis = 0; vAxis = 1; break;
    }
}

namespace {
double& axisRef(hammer::vmf::Vec3& point, int axis)
{
    return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
}

double axisValue(const hammer::vmf::Vec3& point, int axis)
{
    return axis == 0 ? point.x : axis == 1 ? point.y : point.z;
}
} // namespace

hammer::vmf::Bounds MapViewWidget::pendingBoundsFromPlane(const QPointF& first,
                                                          const QPointF& second) const
{
    // The depth along the axis this view cannot draw comes from the reference
    // (last selected) brush, as Hammer sizes a new brush from the last
    // selection bounds. The other views can then resize it.
    const int missing = missingWorldAxis();
    double wMin = -HalfBlockDepth;
    double wMax = HalfBlockDepth;
    if (lastReferenceBounds_.valid) {
        wMin = axisValue(lastReferenceBounds_.minimum, missing);
        wMax = axisValue(lastReferenceBounds_.maximum, missing);
        if (wMax - wMin < 1.0) wMax = wMin + 1.0;
    }
    const hammer::vmf::Vec3 a = planePointToWorld(first, wMin);
    const hammer::vmf::Vec3 b = planePointToWorld(second, wMax);
    hammer::vmf::Bounds bounds;
    bounds.valid = true;
    bounds.minimum = {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    bounds.maximum = {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    return bounds;
}

void MapViewWidget::setPendingBlock(const hammer::vmf::Bounds& bounds, int extrusionAxis)
{
    pendingBlock_ = bounds;
    if (bounds.valid) {
        pendingExtrusionAxis_ = extrusionAxis;
    } else {
        pendingExtrusionAxis_ = -1;
        creationDragging_ = false;
        creationHandleDragging_ = false;
        pendingMoveDragging_ = false;
        creationMoveU_ = -1;
        creationMoveV_ = -1;
    }
    // The GPU 2D renderer draws the preview into the frame it caches, so the
    // frame has to be re-rendered, not just re-presented: a relayed box from
    // another view would otherwise not appear until something else redrew.
    requestRepaint(true);
}

void MapViewWidget::clearPendingBlock()
{
    if (!pendingBlock_.valid) return;
    setPendingBlock({}, -1);
    emit blockPreviewChanged({}, -1);
}

bool MapViewWidget::pendingBlockPlaneRect(QPointF& first, QPointF& second) const
{
    if (!pendingBlock_.valid || kind_ == Kind::Perspective) return false;
    int uAxis = 0;
    int vAxis = 1;
    planeWorldAxes(uAxis, vAxis);
    first = QPointF(axisValue(pendingBlock_.minimum, uAxis), axisValue(pendingBlock_.minimum, vAxis));
    second = QPointF(axisValue(pendingBlock_.maximum, uAxis), axisValue(pendingBlock_.maximum, vAxis));
    return true;
}

void MapViewWidget::drawCreationPreview(QPainter& painter)
{
    if (!pendingBlock_.valid || kind_ == Kind::Perspective) return;
    // An axis-aligned box projects to the rectangle spanned by its two extreme
    // corners in every 2D view, so each view shows its own pair of dimensions.
    const QPointF first = toScreen(pendingBlock_.minimum);
    const QPointF second = toScreen(pendingBlock_.maximum);
    painter.setPen(QPen(CreationWire, 1, Qt::DashLine));
    painter.setBrush(QColor(CreationWire.red(), CreationWire.green(), CreationWire.blue(), 28));
    painter.drawRect(QRectF(first, second).normalized());
}

QPointF MapViewWidget::project(const hammer::vmf::Vec3& point) const
{
    switch (kind_) {
    case Kind::Top: return {point.x, point.y};
    case Kind::Front: return {point.y, point.z};
    case Kind::Side: return {point.x, point.z};
    case Kind::Perspective: {
        const hammer::camera::CameraPoint cameraPoint = hammer::camera::toCamera(cameraState_, point);
        return {cameraPoint.right, cameraPoint.up};
    }
    }
    return {};
}

std::optional<QPointF> MapViewWidget::projectCameraPoint(const hammer::vmf::Vec3& point) const
{
    const auto projected = hammer::camera::projectPoint(cameraState_, projectionMode_, point,
                                                        static_cast<double>(width()),
                                                        static_cast<double>(height()));
    if (!projected) return std::nullopt;
    return QPointF(projected->x, projected->y);
}

bool MapViewWidget::projectCameraLine(const hammer::vmf::Vec3& first,
                                      const hammer::vmf::Vec3& second,
                                      QLineF& line) const
{
    hammer::camera::ScreenPoint a;
    hammer::camera::ScreenPoint b;
    if (!hammer::camera::projectLine(cameraState_, projectionMode_, first, second,
                                     static_cast<double>(width()), static_cast<double>(height()),
                                     a, b)) {
        return false;
    }
    line = QLineF(QPointF(a.x, a.y), QPointF(b.x, b.y));
    return true;
}

QPointF MapViewWidget::toScreen(const hammer::vmf::Vec3& point) const
{
    if (kind_ == Kind::Perspective) {
        const auto projected = projectCameraPoint(point);
        return projected.value_or(QPointF(-1000000.0, -1000000.0));
    }
    const QPointF projected = project(point);
    const QPoint center = rect().center() + pan_;
    return {center.x() + projected.x() * zoom_, center.y() - projected.y() * zoom_};
}

QPointF MapViewWidget::planeToScreen(const QPointF& plane) const
{
    const QPoint center = rect().center() + pan_;
    return {center.x() + plane.x() * zoom_, center.y() - plane.y() * zoom_};
}

MapViewWidget::Handle MapViewWidget::creationHandleHitTest(const QPointF& point) const
{
    if (!pendingBlock_.valid || kind_ == Kind::Perspective) return Handle::None;
    const QRectF bounds = QRectF(toScreen(pendingBlock_.minimum),
                                 toScreen(pendingBlock_.maximum)).normalized();
    const struct { Handle handle; QPointF position; } handles[] = {
        {Handle::TopLeft, bounds.topLeft()},
        {Handle::Top, {bounds.center().x(), bounds.top()}},
        {Handle::TopRight, bounds.topRight()},
        {Handle::Right, {bounds.right(), bounds.center().y()}},
        {Handle::BottomRight, bounds.bottomRight()},
        {Handle::Bottom, {bounds.center().x(), bounds.bottom()}},
        {Handle::BottomLeft, bounds.bottomLeft()},
        {Handle::Left, {bounds.left(), bounds.center().y()}},
    };
    for (const auto& candidate : handles) {
        if (QLineF(point, candidate.position).length() <= 6.0) return candidate.handle;
    }
    return Handle::None;
}

void MapViewWidget::confirmBlockCreation()
{
    if (!pendingBlock_.valid || kind_ == Kind::Perspective) return;
    const hammer::vmf::Bounds box = pendingBlock_;
    // The extrusion axis belongs to the box, not to whoever confirms it: a
    // cylinder drawn in Top must still extrude along z when Enter is pressed
    // in the Front view.
    const int extrusionAxis = pendingExtrusionAxis_ >= 0 ? pendingExtrusionAxis_ : missingWorldAxis();
    setPendingBlock({}, -1);
    emit blockPreviewChanged({}, -1);
    requestRepaint();
    const double minimumExtent = gridSnapEnabled_ ? gridSpacing_ : 1.0;
    if (box.maximum.x - box.minimum.x < minimumExtent ||
        box.maximum.y - box.minimum.y < minimumExtent ||
        box.maximum.z - box.minimum.z < minimumExtent) {
        return;
    }
    emit blockCreationRequested(box.minimum, box.maximum, extrusionAxis);
}

void MapViewWidget::centerOnWorldPoint(const hammer::vmf::Vec3& point, double radius)
{
    if (kind_ == Kind::Perspective) {
        // Keep the heading the camera already has and pull back along it, so
        // centering re-frames the target without spinning the view around.
        const double cosPitch = std::cos(cameraState_.pitchRadians);
        const hammer::vmf::Vec3 forward{cosPitch * std::cos(cameraState_.yawRadians),
                                        cosPitch * std::sin(cameraState_.yawRadians),
                                        std::sin(cameraState_.pitchRadians)};
        const double distance = std::max(128.0, radius * 2.5);
        setCameraTransform({point.x - forward.x * distance, point.y - forward.y * distance,
                            point.z - forward.z * distance},
                           point);
        return;
    }
    // pan_ is the offset of the view's middle from the world origin, in pixels,
    // with the vertical axis pointing up (the inverse of toScreen).
    const QPointF projected = project(point);
    pan_ = QPoint(static_cast<int>(std::lround(-projected.x() * zoom_)),
                  static_cast<int>(std::lround(projected.y() * zoom_)));
    // A pending fit would overwrite the pan on the next paint.
    fitPending_ = false;
    invalidateBaseFrame();
    requestRepaint(false);
}

std::optional<hammer::vmf::Vec3> MapViewWidget::viewCenterWorld() const
{
    if (kind_ == Kind::Perspective) return std::nullopt;
    return planePointToWorld(screenToPlane(QPointF(rect().center())));
}

QPointF MapViewWidget::screenToPlane(const QPointF& point) const
{
    const QPoint center = rect().center() + pan_;
    return {(point.x() - center.x()) / zoom_, (center.y() - point.y()) / zoom_};
}

hammer::vmf::Vec3 MapViewWidget::planeDeltaToWorld(const QPointF& delta) const
{
    switch (kind_) {
    case Kind::Top: return {delta.x(), delta.y(), 0.0};
    case Kind::Front: return {0.0, delta.x(), delta.y()};
    case Kind::Side: return {delta.x(), 0.0, delta.y()};
    case Kind::Perspective: return {};
    }
    return {};
}

hammer::vmf::Vec3 MapViewWidget::planePointToWorld(const QPointF& point, double missingAxis) const
{
    switch (kind_) {
    case Kind::Top: return {point.x(), point.y(), missingAxis};
    case Kind::Front: return {missingAxis, point.x(), point.y()};
    case Kind::Side: return {point.x(), missingAxis, point.y()};
    case Kind::Perspective: return {};
    }
    return {};
}

std::optional<QRectF> MapViewWidget::spriteScreenBounds(
    const hammer::vmf::EntityMarker& entity) const
{
    if (entity.sprite.empty() || !materials_) return std::nullopt;
    const auto material = materials_->material(entity.sprite);
    if (!material || !material->image.valid()) return std::nullopt;

    const double aspect = static_cast<double>(material->image.width) /
                          std::max(1, material->image.height);

    // Sized by the shared helper so the selection rectangle is derived from the
    // same camera-facing quad Hardware3DViewport::drawBillboardSprite draws,
    // rather than a fixed world-axis AABB.
    const hammer::vmf::BillboardSize billboard = hammer::vmf::billboardSpriteSize(
        entity, material->image.width, material->image.height);
    const double worldWidth = billboard.width;
    const double worldHeight = billboard.height;

    if (kind_ == Kind::Perspective) {
        const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
        const hammer::vmf::Vec3 up = hammer::camera::upVector(cameraState_);
        QRectF bounds;
        bool hasPoint = false;
        for (const auto [x, y] : {std::pair{-0.5, -0.5}, std::pair{0.5, -0.5},
                                  std::pair{0.5, 0.5}, std::pair{-0.5, 0.5}}) {
            const hammer::vmf::Vec3 corner = add(entity.origin,
                add(scale(right, worldWidth * x), scale(up, worldHeight * y)));
            const auto projected = projectCameraPoint(corner);
            if (!projected) continue;
            expandScreenBounds(bounds, hasPoint, *projected);
        }
        return hasPoint ? std::optional<QRectF>(bounds.normalized()) : std::nullopt;
    }

    const QPointF center = toScreen(entity.origin);
    double pixelHeight = std::max(12.0, worldHeight * zoom_);
    if (kind_ == Kind::Top) pixelHeight = std::max(12.0, worldWidth * zoom_);
    const double pixelWidth = pixelHeight * aspect;
    return QRectF(center.x() - pixelWidth * 0.5, center.y() - pixelHeight * 0.5,
                  pixelWidth, pixelHeight).normalized();
}

std::optional<QRectF> MapViewWidget::entityScreenBounds(const hammer::vmf::EntityMarker& entity) const
{
    if (!entity.projectedSurfaces.empty()) {
        double minimumX = std::numeric_limits<double>::infinity();
        double minimumY = std::numeric_limits<double>::infinity();
        double maximumX = -std::numeric_limits<double>::infinity();
        double maximumY = -std::numeric_limits<double>::infinity();
        for (const auto& surface : entity.projectedSurfaces) {
            for (const auto& vertex : surface.triangles) {
                const auto projected = kind_ == Kind::Perspective
                    ? projectCameraPoint(vertex.position)
                    : std::optional<QPointF>(toScreen(vertex.position));
                if (!projected) continue;
                minimumX = std::min(minimumX, projected->x());
                minimumY = std::min(minimumY, projected->y());
                maximumX = std::max(maximumX, projected->x());
                maximumY = std::max(maximumY, projected->y());
            }
        }
        if (std::isfinite(minimumX)) {
            QRectF bounds(QPointF(minimumX, minimumY), QPointF(maximumX, maximumY));
            if (bounds.width() < 8.0) bounds.adjust(-4.0, 0.0, 4.0, 0.0);
            if (bounds.height() < 8.0) bounds.adjust(0.0, -4.0, 0.0, 4.0);
            return bounds.normalized();
        }
    }

    // A prop's generated selection bounds (the transformed studio-model AABB
    // computed by the document) are the outline the user sees, so they are also
    // the hitbox. Only fall back to projecting every model vertex when the
    // corners have not been generated yet.
    if (!entity.model.empty() && !entity.hasSelectionCorners && studioModels_) {
        const auto model = studioModels_->model(entity.model);
        if (model && model->valid) {
            const hammer::camera::SourceTransform transform =
                hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
            QRectF bounds;
            bool hasPoint = false;
            for (const auto& mesh : model->meshes) {
                for (const auto& vertex : mesh.vertices) {
                    const hammer::vmf::Vec3 world = transform.transformPoint(
                        {vertex.x, vertex.y, vertex.z});
                    const auto projected = kind_ == Kind::Perspective
                        ? projectCameraPoint(world) : std::optional<QPointF>(toScreen(world));
                    if (!projected) continue;
                    expandScreenBounds(bounds, hasPoint, *projected);
                }
            }
            if (hasPoint) {
                if (bounds.width() < 10.0) bounds.adjust(-5.0, 0.0, 5.0, 0.0);
                if (bounds.height() < 10.0) bounds.adjust(0.0, -5.0, 0.0, 5.0);
                return bounds.normalized();
            }
        }
    }

    // A valid studio helper takes precedence over an FGD sprite fallback, just
    // like both the hardware renderer and the 2D helper renderer.
    //
    // The sprite quad is only the right selection box where a sprite is what is
    // actually drawn. Hardware3DViewport::appendEntityLines draws sprite-only
    // point entities in the 2D views as their FGD size box, not as the image,
    // so in those views the box below is what the user sees and must be what
    // they click. The QPainter fallback path does draw the image in 2D, so it
    // keeps the sprite rectangle.
    if (!entity.sprite.empty() &&
        (entity.model.empty() || !entity.hasSelectionCorners) &&
        (kind_ == Kind::Perspective || hardwareViewport_ == nullptr)) {
        if (const auto spriteBounds = spriteScreenBounds(entity)) return spriteBounds;
    }

    QRectF bounds;
    bool hasPoint = false;
    for (int index = 0; index < 8; ++index) {
        const hammer::vmf::Vec3 corner = entity.hasSelectionCorners
            ? entity.selectionCorners[static_cast<std::size_t>(index)]
            : hammer::vmf::Vec3{
                entity.origin.x + ((index & 1) ? entity.sizeMaximum.x : entity.sizeMinimum.x),
                entity.origin.y + ((index & 2) ? entity.sizeMaximum.y : entity.sizeMinimum.y),
                entity.origin.z + ((index & 4) ? entity.sizeMaximum.z : entity.sizeMinimum.z)};
        const std::optional<QPointF> projected = kind_ == Kind::Perspective
            ? projectCameraPoint(corner) : std::optional<QPointF>(toScreen(corner));
        if (!projected) continue;
        expandScreenBounds(bounds, hasPoint, *projected);
    }
    if (!hasPoint) return std::nullopt;
    if (bounds.width() < 10.0) bounds.adjust(-5.0, 0.0, 5.0, 0.0);
    if (bounds.height() < 10.0) bounds.adjust(0.0, -5.0, 0.0, 5.0);
    return bounds.normalized();
}

std::optional<double> MapViewWidget::projectedSurfaceHitDistance(
    const hammer::vmf::EntityMarker& entity, const QPointF& point) const
{
    if (kind_ != Kind::Perspective || entity.projectedSurfaces.empty()) return std::nullopt;
    const hammer::vmf::Vec3 forward = hammer::camera::forwardVector(cameraState_);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
    const hammer::vmf::Vec3 up = hammer::camera::upVector(cameraState_);
    hammer::vmf::Vec3 rayOrigin = cameraState_.position;
    hammer::vmf::Vec3 rayDirection = forward;
    if (projectionMode_ == ProjectionMode::Perspective) {
        const double focal = std::max(1.0, static_cast<double>(height())) /
            (2.0 * std::tan(cameraState_.verticalFovRadians * 0.5));
        const double cameraRight = (point.x() - width() * 0.5) / focal;
        const double cameraUp = (height() * 0.5 - point.y()) / focal;
        rayDirection = normalized(add(forward,
            add(scale(right, cameraRight), scale(up, cameraUp))));
    } else {
        const double unitsPerPixel = cameraState_.orthographicHeight /
                                     std::max(1.0, static_cast<double>(height()));
        rayOrigin = add(rayOrigin,
            add(scale(right, (point.x() - width() * 0.5) * unitsPerPixel),
                scale(up, (height() * 0.5 - point.y()) * unitsPerPixel)));
    }

    std::optional<double> nearest;
    for (const auto& surface : entity.projectedSurfaces) {
        for (std::size_t index = 0; index + 2 < surface.triangles.size(); index += 3) {
            const auto distance = rayTriangleDistance(rayOrigin, rayDirection,
                surface.triangles[index].position,
                surface.triangles[index + 1].position,
                surface.triangles[index + 2].position);
            if (distance && (!nearest || *distance < *nearest)) nearest = *distance;
        }
    }
    return nearest;
}

std::optional<double> MapViewWidget::modelHitDistance(
    const hammer::vmf::EntityMarker& entity, const QPointF& point) const
{
    if (kind_ != Kind::Perspective || entity.model.empty() || !studioModels_) return std::nullopt;
    const auto model = studioModels_->model(entity.model);
    if (!model || !model->valid) return std::nullopt;

    const hammer::vmf::Vec3 forward = hammer::camera::forwardVector(cameraState_);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
    const hammer::vmf::Vec3 up = hammer::camera::upVector(cameraState_);
    hammer::vmf::Vec3 rayOrigin = cameraState_.position;
    hammer::vmf::Vec3 rayDirection = forward;
    if (projectionMode_ == ProjectionMode::Perspective) {
        const double focal = std::max(1.0, static_cast<double>(height())) /
            (2.0 * std::tan(cameraState_.verticalFovRadians * 0.5));
        const double cameraRight = (point.x() - width() * 0.5) / focal;
        const double cameraUp = (height() * 0.5 - point.y()) / focal;
        rayDirection = normalized(add(forward,
            add(scale(right, cameraRight), scale(up, cameraUp))));
    } else {
        const double unitsPerPixel = cameraState_.orthographicHeight /
                                     std::max(1.0, static_cast<double>(height()));
        rayOrigin = add(rayOrigin,
            add(scale(right, (point.x() - width() * 0.5) * unitsPerPixel),
                scale(up, (height() * 0.5 - point.y()) * unitsPerPixel)));
    }

    const hammer::camera::SourceTransform transform =
        hammer::camera::sourceTransform(entity.origin, entity.renderAngles());

    // Cheap local-space slab test before walking the model triangles. This keeps
    // accurate prop picking responsive on maps containing many high-poly props.
    const auto inverseRotate = [&transform](const hammer::vmf::Vec3& worldVector) {
        return hammer::vmf::Vec3{dot(worldVector, transform.basis.forward),
                                 dot(worldVector, transform.basis.left),
                                 dot(worldVector, transform.basis.up)};
    };
    const hammer::vmf::Vec3 localOrigin = inverseRotate(subtract(rayOrigin, entity.origin));
    const hammer::vmf::Vec3 localDirection = inverseRotate(rayDirection);
    double minimumDistance = 0.0;
    double maximumDistance = std::numeric_limits<double>::infinity();
    const double minimum[3] = {model->minimum[0], model->minimum[1], model->minimum[2]};
    const double maximum[3] = {model->maximum[0], model->maximum[1], model->maximum[2]};
    const double origin[3] = {localOrigin.x, localOrigin.y, localOrigin.z};
    const double direction[3] = {localDirection.x, localDirection.y, localDirection.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-10) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return std::nullopt;
            continue;
        }
        double first = (minimum[axis] - origin[axis]) / direction[axis];
        double second = (maximum[axis] - origin[axis]) / direction[axis];
        if (first > second) std::swap(first, second);
        minimumDistance = std::max(minimumDistance, first);
        maximumDistance = std::min(maximumDistance, second);
        if (minimumDistance > maximumDistance) return std::nullopt;
    }

    std::optional<double> nearest;
    for (const auto& mesh : model->meshes) {
        for (std::size_t index = 0; index + 2 < mesh.vertices.size(); index += 3) {
            const auto world = [&](const hammer::assets::StudioVertex& vertex) {
                return transform.transformPoint({vertex.x, vertex.y, vertex.z});
            };
            const auto distance = rayTriangleDistance(rayOrigin, rayDirection,
                world(mesh.vertices[index]), world(mesh.vertices[index + 1]),
                world(mesh.vertices[index + 2]));
            if (distance && (!nearest || *distance < *nearest)) nearest = *distance;
        }
    }
    return nearest;
}

void MapViewWidget::buildPickRay(const QPointF& point, hammer::vmf::Vec3& origin,
                                 hammer::vmf::Vec3& direction) const
{
    const hammer::vmf::Vec3 forward = hammer::camera::forwardVector(cameraState_);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
    const hammer::vmf::Vec3 up = hammer::camera::upVector(cameraState_);
    origin = cameraState_.position;
    direction = forward;
    if (projectionMode_ == ProjectionMode::Perspective) {
        const double focal = std::max(1.0, static_cast<double>(height())) /
            (2.0 * std::tan(cameraState_.verticalFovRadians * 0.5));
        const double cameraRight = (point.x() - width() * 0.5) / focal;
        const double cameraUp = (height() * 0.5 - point.y()) / focal;
        direction = normalized(add(forward,
            add(scale(right, cameraRight), scale(up, cameraUp))));
    } else {
        const double unitsPerPixel = cameraState_.orthographicHeight /
                                     std::max(1.0, static_cast<double>(height()));
        origin = add(origin,
            add(scale(right, (point.x() - width() * 0.5) * unitsPerPixel),
                scale(up, (height() * 0.5 - point.y()) * unitsPerPixel)));
    }
}

std::optional<double> MapViewWidget::boxHitDistance(
    const hammer::vmf::EntityMarker& entity, const QPointF& point) const
{
    if (kind_ != Kind::Perspective) return std::nullopt;
    // Only entities the hardware renderer draws as a fallback cube: no
    // projected surfaces, no sprite, and no (valid) studio model.
    if (!entity.projectedSurfaces.empty() || !entity.sprite.empty()) return std::nullopt;
    if (!entity.model.empty() && studioModels_) {
        const auto model = studioModels_->model(entity.model);
        if (model && model->valid) return std::nullopt;
    }

    hammer::vmf::Vec3 minimum{std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::infinity()};
    hammer::vmf::Vec3 maximum{-std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()};
    for (int index = 0; index < 8; ++index) {
        const hammer::vmf::Vec3 corner = entity.hasSelectionCorners
            ? entity.selectionCorners[static_cast<std::size_t>(index)]
            : hammer::vmf::Vec3{
                entity.origin.x + ((index & 1) ? entity.sizeMaximum.x : entity.sizeMinimum.x),
                entity.origin.y + ((index & 2) ? entity.sizeMaximum.y : entity.sizeMinimum.y),
                entity.origin.z + ((index & 4) ? entity.sizeMaximum.z : entity.sizeMinimum.z)};
        minimum.x = std::min(minimum.x, corner.x);
        minimum.y = std::min(minimum.y, corner.y);
        minimum.z = std::min(minimum.z, corner.z);
        maximum.x = std::max(maximum.x, corner.x);
        maximum.y = std::max(maximum.y, corner.y);
        maximum.z = std::max(maximum.z, corner.z);
    }

    hammer::vmf::Vec3 origin{};
    hammer::vmf::Vec3 direction{};
    buildPickRay(point, origin, direction);
    // Standard slab test against the cube's AABB.
    double tNear = 0.0;
    double tFar = std::numeric_limits<double>::infinity();
    const double origins[3] = {origin.x, origin.y, origin.z};
    const double directions[3] = {direction.x, direction.y, direction.z};
    const double minimums[3] = {minimum.x, minimum.y, minimum.z};
    const double maximums[3] = {maximum.x, maximum.y, maximum.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(directions[axis]) < 1e-12) {
            if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis])
                return std::nullopt;
            continue;
        }
        double t0 = (minimums[axis] - origins[axis]) / directions[axis];
        double t1 = (maximums[axis] - origins[axis]) / directions[axis];
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) return std::nullopt;
    }
    return tNear;
}

std::optional<double> MapViewWidget::billboardHitDistance(
    const hammer::vmf::EntityMarker& entity, const QPointF& point) const
{
    if (kind_ != Kind::Perspective || entity.sprite.empty() || !materials_) return std::nullopt;
    // A studio helper takes precedence over the FGD sprite in every renderer,
    // so an entity with a valid model never draws (or picks as) a billboard.
    if (!entity.model.empty() && studioModels_) {
        const auto model = studioModels_->model(entity.model);
        if (model && model->valid) return std::nullopt;
    }
    if (!entity.projectedSurfaces.empty()) return std::nullopt;
    const auto material = materials_->material(entity.sprite);
    if (!material || !material->image.valid()) return std::nullopt;

    const hammer::vmf::BillboardSize billboard = hammer::vmf::billboardSpriteSize(
        entity, material->image.width, material->image.height);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
    const hammer::vmf::Vec3 up = hammer::camera::upVector(cameraState_);
    const double halfWidth = billboard.width * 0.5;
    const double halfHeight = billboard.height * 0.5;
    const auto corner = [&](double x, double y) {
        return add(entity.origin, add(scale(right, x), scale(up, y)));
    };
    const hammer::vmf::Vec3 bottomLeft = corner(-halfWidth, -halfHeight);
    const hammer::vmf::Vec3 bottomRight = corner(halfWidth, -halfHeight);
    const hammer::vmf::Vec3 topRight = corner(halfWidth, halfHeight);
    const hammer::vmf::Vec3 topLeft = corner(-halfWidth, halfHeight);

    hammer::vmf::Vec3 rayOrigin;
    hammer::vmf::Vec3 rayDirection;
    buildPickRay(point, rayOrigin, rayDirection);
    std::optional<double> nearest;
    for (const auto triangle : {std::array<const hammer::vmf::Vec3*, 3>{&bottomLeft, &bottomRight, &topRight},
                                std::array<const hammer::vmf::Vec3*, 3>{&bottomLeft, &topRight, &topLeft}}) {
        const auto distance = rayTriangleDistance(rayOrigin, rayDirection,
                                                  *triangle[0], *triangle[1], *triangle[2]);
        if (distance && (!nearest || *distance < *nearest)) nearest = *distance;
    }
    return nearest;
}

bool MapViewWidget::brushHiddenByToolTextures(const hammer::vmf::BrushGeometry& brush) const
{
    return hammer::vmf::isBrushHiddenByToolTextures(brush, hiddenToolTextures_);
}

std::optional<MapViewWidget::SurfaceHit> MapViewWidget::surfaceHit(const QPointF& point,
                                                                   double* distance) const
{
    if (!scene_ || kind_ != Kind::Perspective) return std::nullopt;
    hammer::vmf::Vec3 rayOrigin;
    hammer::vmf::Vec3 rayDirection;
    buildPickRay(point, rayOrigin, rayDirection);

    std::optional<double> nearest;
    SurfaceHit hit;
    for (const auto& brush : scene_->brushes) {
        for (const auto& face : brush.faces) {
            // A face whose tool material is hidden by View > Tool Textures is
            // not drawn, so - exactly like the displacement-solid mask below,
            // which CMapDoc::SelectFace also honours - it must not be pickable
            // either. Clicks pass straight through to whatever is behind it.
            if (hammer::vmf::isFaceHiddenByToolTextures(face, hiddenToolTextures_)) continue;
            // CMapDoc::SelectFace applies the same displacement-solid mask as
            // CMapSolid::Render3D, so sides hidden in the 3D view cannot be
            // picked there either.
            if (hammer::vmf::isFaceMaskedByDisplacementSolid(
                    brush, face, displacementSolidMaskEnabled_)) {
                continue;
            }
            auto testTriangle = [&](const hammer::vmf::Vec3& a,
                                    const hammer::vmf::Vec3& b,
                                    const hammer::vmf::Vec3& c) {
                const auto distance = rayTriangleDistance(rayOrigin, rayDirection, a, b, c);
                if (!distance || (nearest && *distance >= *nearest)) return;
                nearest = *distance;
                hit.position = add(rayOrigin, scale(rayDirection, *distance));
                hit.normal = normalized(face.normal);
                hit.sideId = face.sideId;
                hit.solidId = brush.id;
                hit.displacement = false;
            };
            if (face.displacement && face.displacementIndices.size() >= 3) {
                for (std::size_t index = 0; index + 2 < face.displacementIndices.size(); index += 3) {
                    const std::size_t a = face.displacementIndices[index];
                    const std::size_t b = face.displacementIndices[index + 1];
                    const std::size_t c = face.displacementIndices[index + 2];
                    if (a < face.displacementVertices.size() && b < face.displacementVertices.size() &&
                        c < face.displacementVertices.size()) {
                        const auto& pointA = face.displacementVertices[a].position;
                        const auto& pointB = face.displacementVertices[b].position;
                        const auto& pointC = face.displacementVertices[c].position;
                        const auto distance = rayTriangleDistance(rayOrigin, rayDirection,
                                                                  pointA, pointB, pointC);
                        if (!distance || (nearest && *distance >= *nearest)) continue;
                        nearest = *distance;
                        hit.position = add(rayOrigin, scale(rayDirection, *distance));
                        hit.normal = normalized(cross(subtract(pointB, pointA),
                                                      subtract(pointC, pointA)));
                        if (dot(hit.normal, face.normal) < 0.0) hit.normal = scale(hit.normal, -1.0);
                        hit.sideId = face.sideId;
                        hit.solidId = brush.id;
                        hit.displacement = true;
                    }
                }
            } else if (face.vertices.size() >= 3) {
                const std::size_t first = face.vertices.front();
                if (first >= brush.vertices.size()) continue;
                for (std::size_t index = 1; index + 1 < face.vertices.size(); ++index) {
                    const std::size_t b = face.vertices[index];
                    const std::size_t c = face.vertices[index + 1];
                    if (b < brush.vertices.size() && c < brush.vertices.size())
                        testTriangle(brush.vertices[first], brush.vertices[b], brush.vertices[c]);
                }
            }
        }
    }
    if (nearest && distance) *distance = *nearest;
    return nearest ? std::optional<SurfaceHit>(hit) : std::nullopt;
}

std::vector<hammer::vmf::ObjectRef> MapViewWidget::hitTestAll(const QPointF& point) const
{
    std::vector<hammer::vmf::ObjectRef> hits;
    if (!scene_) return hits;
    constexpr double EntityRadius = 10.0;
    constexpr double EdgeRadius = 6.0;

    // CSelection::AddHit dedupes; the caller's list order is the hit order.
    const auto addHit = [&hits](const hammer::vmf::ObjectRef& object) {
        if (object.id < 0) return;
        if (std::find(hits.begin(), hits.end(), object) == hits.end()) hits.push_back(object);
    };

    // Perspective picking is a true ray cast against the geometry that is
    // actually rendered, and everything - brush surfaces, props, sprites,
    // decals/overlays - competes in one depth-sorted list. Previously entity
    // helpers short-circuited ahead of brushes, so a prop or sprite behind a
    // wall was picked in preference to the wall.
    if (kind_ == Kind::Perspective) {
        std::vector<std::pair<double, hammer::vmf::ObjectRef>> candidates;
        for (const auto& entity : scene_->entities) {
            if (!entitySelectableInMode(entity)) continue;
            std::optional<double> nearest;
            for (const auto distance : {projectedSurfaceHitDistance(entity, point),
                                        modelHitDistance(entity, point),
                                        billboardHitDistance(entity, point),
                                        boxHitDistance(entity, point)}) {
                if (distance && (!nearest || *distance < *nearest)) nearest = *distance;
            }
            if (nearest) candidates.emplace_back(*nearest, entity.object);
        }
        double surfaceDistance = 0.0;
        if (const auto hit = surfaceHit(point, &surfaceDistance)) {
            for (const auto& brush : scene_->brushes) {
                if (brush.id != hit->solidId) continue;
                candidates.emplace_back(surfaceDistance, effectiveObject(brush));
                break;
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& candidate : candidates) addHit(candidate.second);
        if (!hits.empty()) return hits;
    }

    for (auto it = scene_->entities.rbegin(); it != scene_->entities.rend(); ++it) {
        if (!entitySelectableInMode(*it)) continue;
        const double padding = it->model.empty() ? 4.0 : 7.0;
        if (const auto bounds = entityScreenBounds(*it);
            bounds && bounds->adjusted(-padding, -padding, padding, padding).contains(point)) {
            addHit(it->object);
            continue;
        }
        const std::optional<QPointF> projected = kind_ == Kind::Perspective
            ? projectCameraPoint(it->origin) : std::optional<QPointF>(toScreen(it->origin));
        if (projected && QLineF(point, *projected).length() <= EntityRadius) addHit(it->object);
    }
    for (auto brush = scene_->brushes.rbegin(); brush != scene_->brushes.rend(); ++brush) {
        // A solid built entirely from hidden tool materials is not drawn in the
        // 2D views, so it must not be clickable there either.
        if (brushHiddenByToolTextures(*brush)) continue;
        std::vector<QPointF> projectedVertices;
        projectedVertices.reserve(brush->vertices.size());
        for (const auto& vertex : brush->vertices) {
            if (kind_ == Kind::Perspective) {
                const auto projected = projectCameraPoint(vertex);
                if (projected) projectedVertices.push_back(*projected);
            } else {
                projectedVertices.push_back(toScreen(vertex));
            }
        }
        if (projectedVertices.size() >= 3 && convexHull(projectedVertices).containsPoint(point, Qt::OddEvenFill)) {
            addHit(effectiveObject(*brush));
            continue;
        }
        for (const auto& edge : brush->edges) {
            if (edge.first >= brush->vertices.size() || edge.second >= brush->vertices.size()) continue;
            QLineF line;
            if (kind_ == Kind::Perspective) {
                if (!projectCameraLine(brush->vertices[edge.first], brush->vertices[edge.second], line)) continue;
            } else {
                line = QLineF(toScreen(brush->vertices[edge.first]), toScreen(brush->vertices[edge.second]));
            }
            if (pointSegmentDistance(point, line.p1(), line.p2()) <= EdgeRadius) {
                addHit(effectiveObject(*brush));
                break;
            }
        }
    }
    return hits;
}

std::optional<hammer::vmf::ObjectRef> MapViewWidget::hitTest(const QPointF& point) const
{
    const std::vector<hammer::vmf::ObjectRef> hits = hitTestAll(point);
    if (hits.empty()) return std::nullopt;
    return hits.front();
}

std::vector<hammer::vmf::ObjectRef> MapViewWidget::objectsInBox(const QRectF& box) const
{
    // Selection3D::SelectInBox -> CMapDoc::SelectRegion: an object joins the
    // selection when the drag rectangle touches it. Hammer projects the box
    // along the view axis, which for a 2D view means testing the object's
    // projected screen bounds against the rectangle.
    std::vector<hammer::vmf::ObjectRef> objects;
    if (!scene_) return objects;
    const auto add = [&objects](const hammer::vmf::ObjectRef& object) {
        if (object.id < 0) return;
        if (std::find(objects.begin(), objects.end(), object) == objects.end())
            objects.push_back(object);
    };
    // Touching the box is enough — a box that crosses any part of an object
    // takes it. (Hammer's SelectInBox wants the object wholly inside; this port
    // deliberately uses the more forgiving rule.) A flat rectangle is null to
    // Qt and would never intersect, so an object seen edge-on in a 2D view —
    // or a brush that projects to a bare line — is widened to a hair first.
    const auto touchesBox = [&box](QRectF bounds) {
        bounds = bounds.normalized();
        if (bounds.width() < 0.5) bounds.adjust(-0.25, 0.0, 0.25, 0.0);
        if (bounds.height() < 0.5) bounds.adjust(0.0, -0.25, 0.0, 0.25);
        return box.intersects(bounds);
    };
    for (const auto& brush : scene_->brushes) {
        if (brushHiddenByToolTextures(brush)) continue;
        if (brush.vertices.empty()) continue;
        QRectF bounds;
        bool hasPoint = false;
        bool clipped = false;
        for (const auto& vertex : brush.vertices) {
            const std::optional<QPointF> projected = kind_ == Kind::Perspective
                ? projectCameraPoint(vertex) : std::optional<QPointF>(toScreen(vertex));
            if (!projected) { clipped = true; break; }
            expandScreenBounds(bounds, hasPoint, *projected);
        }
        if (clipped || !hasPoint) continue;
        if (touchesBox(bounds)) add(effectiveObject(brush));
    }
    for (const auto& entity : scene_->entities) {
        if (!entitySelectableInMode(entity)) continue;
        const auto bounds = entityScreenBounds(entity);
        if (!bounds) continue;
        if (touchesBox(*bounds)) add(entity.object);
    }
    return objects;
}

bool MapViewWidget::isSelected(const hammer::vmf::ObjectRef& object) const
{
    return std::find(selection_.begin(), selection_.end(), object) != selection_.end();
}

// CMapSolid::PrepareSelection (hammer/mapsolid.cpp): in Groups or Objects mode a
// solid whose parent is an entity resolves to that entity; in Solids mode the
// solid itself is always selected. This port has no group container objects
// (hammer::vmf::ObjectType is Solid or Entity only), so Groups behaves as
// Objects - there is no higher ancestor to walk up to.
hammer::vmf::ObjectRef MapViewWidget::effectiveObject(const hammer::vmf::BrushGeometry& brush) const
{
    if (selectionMode_ != SelectionMode::Solids && brush.ownerEntityId >= 0) {
        return {hammer::vmf::ObjectType::Entity, brush.ownerEntityId};
    }
    return brush.object;
}

// CMapEntity::PrepareSelection: in Solids mode a solid entity is never selected;
// its solid children are selected instead. Point entities (placeholders) stay
// selectable in every mode.
bool MapViewWidget::entitySelectableInMode(const hammer::vmf::EntityMarker& entity) const
{
    if (selectionMode_ != SelectionMode::Solids) return true;
    if (!scene_) return true;
    for (const auto& brush : scene_->brushes) {
        if (brush.ownerEntityId == entity.id) return false;
    }
    return true;
}

QRectF MapViewWidget::selectionScreenBounds() const
{
    if (kind_ == Kind::Perspective) return {};
    QRectF bounds;
    bool any = false;
    if (selectionBounds_.valid) {
        const QPointF first = toScreen(selectionBounds_.minimum);
        const QPointF second = toScreen(selectionBounds_.maximum);
        bounds = QRectF(first, second).normalized();
        any = true;
    }
    // The document-side bounds of a point entity collapse to its origin, so
    // the handles used to pile up under the entity icon. Union in the box the
    // entity is actually drawn with so the handles sit on its edges.
    if (scene_) {
        std::unordered_set<int> brushOwners;
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.ownerEntityId >= 0) brushOwners.insert(brush.ownerEntityId);
        }
        for (const hammer::vmf::EntityMarker& entity : scene_->entities) {
            if (!isSelected(entity.object)) continue;
            // Brush entities are boxed by their solids; their origin marker
            // can sit far from the geometry (stray origins in decompiles) and
            // would balloon the box.
            if (brushOwners.count(entity.id)) continue;
            if (const auto entityBounds = entityScreenBounds(entity)) {
                const QRectF normalized = entityBounds->normalized();
                bounds = any ? bounds.united(normalized) : normalized;
                any = true;
            }
        }
    }
    if (!any) return {};
    if (bounds.width() < 8.0) bounds.adjust(-4.0, 0.0, 4.0, 0.0);
    if (bounds.height() < 8.0) bounds.adjust(0.0, -4.0, 0.0, 4.0);
    return bounds.adjusted(-3.0, -3.0, 3.0, 3.0);
}

std::array<std::pair<MapViewWidget::Handle, QPointF>, 4>
MapViewWidget::translateHandlePositions(const QRectF& bounds)
{
    // The move handles sit outside the box (see the reference sketch) so they
    // never cover a small selection like a point entity.
    constexpr double Offset = 14.0;
    return {{{Handle::Top, {bounds.center().x(), bounds.top() - Offset}},
             {Handle::Right, {bounds.right() + Offset, bounds.center().y()}},
             {Handle::Bottom, {bounds.center().x(), bounds.bottom() + Offset}},
             {Handle::Left, {bounds.left() - Offset, bounds.center().y()}}}};
}

MapViewWidget::Handle MapViewWidget::hitHandle(const QPointF& point) const
{
    const QRectF bounds = selectionScreenBounds();
    if (!bounds.isValid()) return Handle::None;
    const std::pair<Handle, QPointF> handles[] = {
        {Handle::TopLeft, bounds.topLeft()}, {Handle::Top, {bounds.center().x(), bounds.top()}},
        {Handle::TopRight, bounds.topRight()}, {Handle::Right, {bounds.right(), bounds.center().y()}},
        {Handle::BottomRight, bounds.bottomRight()}, {Handle::Bottom, {bounds.center().x(), bounds.bottom()}},
        {Handle::BottomLeft, bounds.bottomLeft()}, {Handle::Left, {bounds.left(), bounds.center().y()}}
    };
    const TransformMode mode = effectiveTransformMode();
    // Clicks well inside the selection belong to the body (drag to move, or
    // click to cycle the handle mode) even when the box is small enough that
    // a handle's radius would otherwise reach them — point entities are only
    // a few pixels across, so without this every click landed on a handle.
    const QRectF interior = bounds.adjusted(6.0, 6.0, -6.0, -6.0);
    if (interior.isValid() && interior.contains(point)) return Handle::None;
    if (mode == TransformMode::Translate) {
        // The move handles float outside the box; a generous radius keeps a
        // near-miss from falling through to the free body drag.
        for (const auto& [handle, position] : translateHandlePositions(bounds)) {
            if (QLineF(point, position).length() <= 8.0) return handle;
        }
        return Handle::None;
    }
    for (const auto& [handle, position] : handles) {
        const double radius = mode == TransformMode::Scale ? 6.0 : 8.0;
        if (QLineF(point, position).length() <= radius) {
            const bool corner = handle == Handle::TopLeft || handle == Handle::TopRight ||
                                handle == Handle::BottomLeft || handle == Handle::BottomRight;
            if (mode == TransformMode::Rotate && !corner) continue;
            return handle;
        }
    }
    return Handle::None;
}

hammer::vmf::Vec3 MapViewWidget::selectionCentroidWorld() const
{
    hammer::vmf::Vec3 sum{};
    std::size_t count = 0;
    if (scene_) {
        std::unordered_set<int> brushOwners;
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.ownerEntityId >= 0) brushOwners.insert(brush.ownerEntityId);
            if (!isSelected(effectiveObject(brush)) && !isSelected(brush.object)) continue;
            for (const hammer::vmf::Vec3& vertex : brush.vertices) {
                sum.x += vertex.x;
                sum.y += vertex.y;
                sum.z += vertex.z;
                ++count;
            }
        }
        for (const hammer::vmf::EntityMarker& entity : scene_->entities) {
            if (!isSelected(entity.object)) continue;
            // A brush entity's geometry already speaks for it; adding its
            // origin marker would bias the pivot toward that point.
            if (brushOwners.count(entity.id)) continue;
            sum.x += entity.origin.x;
            sum.y += entity.origin.y;
            sum.z += entity.origin.z;
            ++count;
        }
    }
    if (count == 0) return selectionBounds_.center();
    const double scale = 1.0 / static_cast<double>(count);
    return {sum.x * scale, sum.y * scale, sum.z * scale};
}

bool MapViewWidget::isSinglePointEntitySelection() const
{
    if (selection_.size() != 1 || selection_.front().type != hammer::vmf::ObjectType::Entity)
        return false;
    // An entity that owns solids is a brush entity and gets the full brush
    // treatment (dashed box, Scale/Translate/Rotate cycle); only true point
    // entities skip Scale.
    if (scene_) {
        for (const hammer::vmf::BrushGeometry& brush : scene_->brushes) {
            if (brush.ownerEntityId == selection_.front().id) return false;
        }
    }
    return true;
}

MapViewWidget::TransformMode MapViewWidget::effectiveTransformMode() const
{
    if (isSinglePointEntitySelection() && transformMode_ == TransformMode::Scale)
        return TransformMode::Translate;
    return transformMode_;
}

MapViewWidget::TransformMode MapViewWidget::nextTransformMode() const
{
    switch (effectiveTransformMode()) {
    case TransformMode::Scale: return TransformMode::Translate;
    case TransformMode::Translate: return TransformMode::Rotate;
    case TransformMode::Rotate:
        // Point entities cycle Translate <-> Rotate only; Scale is meaningless.
        return isSinglePointEntitySelection() ? TransformMode::Translate : TransformMode::Scale;
    }
    return TransformMode::Scale;
}

std::optional<MapViewWidget::CameraHit> MapViewWidget::cameraHitTest(const QPointF& screenPoint) const
{
    // Mirrors Camera3D::HitTest: hit-test the eye handle before the look-at
    // handle, most recently added camera last (matching CUtlVector order).
    const double radius = 6.0;
    for (int i = 0; i < static_cast<int>(cameras_.size()); ++i) {
        const hammer::vmf::Vec3 parts[2] = {cameras_[i].eye, cameras_[i].lookAt};
        for (int part = 0; part < 2; ++part) {
            if (QLineF(screenPoint, toScreen(parts[part])).length() <= radius) return CameraHit{i, part};
        }
    }
    return std::nullopt;
}

void MapViewWidget::setCameras(const std::vector<hammer::vmf::CameraDef>& cameras, int activeIndex)
{
    if (cameraDragging_) return; // Don't clobber an in-progress drag with a stale snapshot.
    cameras_ = cameras;
    activeCamera_ = activeIndex;
    if (kind_ != Kind::Perspective && tool_ == Tool::Camera) requestRepaint();
}

void MapViewWidget::setCameraTransform(const hammer::vmf::Vec3& eye, const hammer::vmf::Vec3& lookAt)
{
    if (kind_ != Kind::Perspective) return;
    // Camera3D::UpdateActiveCamera repoints CMapView3D at the active camera's
    // eye/look-at pair while preserving the view's own travel distance; here
    // the perspective view simply snaps to face the requested direction.
    cameraState_.position = eye;
    const hammer::vmf::Vec3 direction{lookAt.x - eye.x, lookAt.y - eye.y, lookAt.z - eye.z};
    const double horizontal = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (horizontal > 1e-6 || std::abs(direction.z) > 1e-6) {
        cameraState_.yawRadians = std::atan2(direction.y, direction.x);
        cameraState_.pitchRadians = std::atan2(direction.z, horizontal);
    }
    invalidateBaseFrame();
    reportCameraStatus();
}

void MapViewWidget::drawCameraOverlay(QPainter& painter)
{
    // Camera3D::RenderTool2D: draw every camera's eye/look-at line and eye
    // handle, with the active camera's connecting line in red instead of cyan.
    if (kind_ == Kind::Perspective || tool_ != Tool::Camera) return;
    for (int i = 0; i < static_cast<int>(cameras_.size()); ++i) {
        const hammer::vmf::CameraDef& cam = cameras_[i];
        painter.setPen(QPen(i == activeCamera_ ? QColor(255, 0, 0) : QColor(0, 255, 255), 1));
        painter.drawLine(toScreen(cam.eye), toScreen(cam.lookAt));
        painter.setPen(QPen(QColor(0, 255, 255), 1));
        painter.setBrush(QColor(0, 255, 255));
        painter.drawEllipse(toScreen(cam.eye), 4, 4);
    }
}

hammer::vmf::Vec3 MapViewWidget::viewAxis() const
{
    // CMapView2D::GetViewAxis: the axis this 2D view looks along (axThird).
    switch (kind_) {
    case Kind::Top: return {0.0, 0.0, 1.0};
    case Kind::Front: return {1.0, 0.0, 0.0};
    case Kind::Side: return {0.0, 1.0, 0.0};
    case Kind::Perspective: return {0.0, 0.0, 1.0};
    }
    return {0.0, 0.0, 1.0};
}

int MapViewWidget::clipHandleHitTest(const QPointF& screenPoint) const
{
    // Clipper3D::HitTest: hit-test the two clip points, returning their index.
    if (!clipActive_) return -1;
    for (int i = 0; i < 2; ++i) {
        if (QLineF(screenPoint, toScreen(clipPoints_[i])).length() <= 6.0) return i;
    }
    return -1;
}

void MapViewWidget::setClipState(bool active, const hammer::vmf::Vec3& first,
                                 const hammer::vmf::Vec3& second, const hammer::vmf::Vec3& axis,
                                 ClipMode mode,
                                 const std::vector<hammer::vmf::FacePolygons>& kept,
                                 const std::vector<hammer::vmf::FacePolygons>& discarded)
{
    clipMode_ = mode;
    clipKept_ = kept;
    clipDiscarded_ = discarded;
    if (clipDragging_) {
        // Mid-drag this view owns the clip points; only the clip results the
        // document just recomputed for them are taken.
        requestRepaint(false);
        return;
    }
    clipActive_ = active;
    clipViewAxis_ = axis;
    clipPoints_[0] = first;
    clipPoints_[1] = second;
    if (!active) {
        clipDragging_ = false;
        clipPointHit_ = -1;
    }
    if (kind_ != Kind::Perspective && tool_ == Tool::Clipper) requestRepaint(false);
}

void MapViewWidget::drawClipOverlay(QPainter& painter)
{
    // Clipper3D::RenderTool2D. The surviving halves are drawn as white
    // wireframe, the clip line in cyan, and its endpoints as white square
    // handles. Hammer does not render the discarded half at all; this port
    // draws it in red so the mode being applied is obvious before Enter.
    if (kind_ == Kind::Perspective || tool_ != Tool::Clipper || !clipActive_) return;

    const auto drawPolygons = [&](const std::vector<hammer::vmf::FacePolygons>& solids, const QColor& color) {
        painter.setPen(QPen(color, 1));
        painter.setBrush(Qt::NoBrush);
        for (const hammer::vmf::FacePolygons& solid : solids) {
            for (const std::vector<hammer::vmf::Vec3>& face : solid) {
                if (face.size() < 2) continue;
                QPolygonF polygon;
                polygon.reserve(static_cast<int>(face.size()));
                for (const hammer::vmf::Vec3& point : face) polygon.append(toScreen(point));
                painter.drawPolygon(polygon);
            }
        }
    };

    drawPolygons(clipDiscarded_, QColor(255, 0, 0));
    drawPolygons(clipKept_, QColor(255, 255, 255));

    painter.setPen(QPen(QColor(0, 255, 255), 1));
    painter.drawLine(toScreen(clipPoints_[0]), toScreen(clipPoints_[1]));

    painter.setPen(QPen(QColor(255, 255, 255), 1));
    painter.setBrush(QColor(255, 255, 255));
    for (const hammer::vmf::Vec3& point : clipPoints_) {
        const QPointF screen = toScreen(point);
        painter.drawRect(QRectF(screen.x() - 3.0, screen.y() - 3.0, 6.0, 6.0));
    }
    painter.setBrush(Qt::NoBrush);
}

void MapViewWidget::setMorphState(bool active, const std::vector<hammer::vmf::MorphHandle>& handles,
                                 const std::vector<hammer::vmf::FacePolygons>& preview,
                                 const std::vector<hammer::vmf::MorphDispGrid>& dispGrids)
{
    morphActive_ = active;
    morphHandles_ = handles;
    morphPreview_ = preview;
    morphDispGrids_ = dispGrids;
    if (!active) {
        morphDragHandle_ = -1;
        morphDragging_ = false;
        morphBoxSelecting_ = false;
        morphGizmoAxis_ = -1;
        morphGizmoApplied_ = 0.0;
    }
    // The mesh outlines and gizmo are drawn by the hardware renderer now, so
    // the GL frame must re-render along with the QPainter handle overlay.
    if (tool_ == Tool::Morph) requestRepaint(true);
}

void MapViewWidget::drawMorphOverlay(QPainter& painter)
{
    // Morph3D::RenderTool2D: the structured solid's edges, a square handle at
    // every vertex (white, Options.colors.clrToolHandle) and at every edge
    // midpoint (yellow), selected handles in the selection colour (red). Two
    // passes so selected handles end up on top.
    if (tool_ != Tool::Morph || !morphActive_) return;
    const bool perspective = kind_ == Kind::Perspective;

    // The mesh outlines, displacement grid edges, and mover gizmo are drawn by
    // the hardware renderer; only the handle squares and the selection box
    // remain QPainter overlays.
    const auto handleScreen = [&](const hammer::vmf::Vec3& position) -> std::optional<QPointF> {
        if (perspective) return projectCameraPoint(position);
        return toScreen(position);
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const hammer::vmf::MorphHandle& handle : morphHandles_) {
            if (handle.selected != (pass == 1)) continue;
            const QColor color = handle.selected     ? QColor(255, 0, 0)
                               : handle.displacement ? QColor(255, 160, 0)
                               : handle.edge         ? QColor(255, 255, 0)
                                                     : QColor(255, 255, 255);
            const auto screen = handleScreen(handle.position);
            if (!screen) continue;
            const double half = handle.displacement ? 2.0 : 3.0;
            painter.setPen(QPen(color, 1));
            painter.setBrush(color);
            painter.drawRect(QRectF(screen->x() - half, screen->y() - half, half * 2.0, half * 2.0));
        }
    }
    painter.setBrush(Qt::NoBrush);

    if (morphBoxSelecting_) {
        // Box3D::RenderTool2D while Morph3D::m_bBoxSelecting.
        painter.setPen(QPen(QColor(255, 255, 255), 1, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(morphBoxStart_, morphBoxCurrent_).normalized());
    }
}

int MapViewWidget::morphHandleHitTest(const QPointF& screenPoint) const
{
    // Morph3D::MorphHitTest. Vertex handles win ties over edge handles, as in
    // the original, whose vertex pass runs first. In the perspective view the
    // nearest handle to the camera wins when several stack up in screen space
    // (displacement grids do this constantly).
    int best = -1;
    double bestDistance = 5.0;
    double bestDepth = std::numeric_limits<double>::infinity();
    for (int i = 0; i < static_cast<int>(morphHandles_.size()); ++i) {
        const hammer::vmf::Vec3& position = morphHandles_[i].position;
        QPointF screen;
        if (kind_ == Kind::Perspective) {
            const auto projected = projectCameraPoint(position);
            if (!projected) continue;
            screen = *projected;
        } else {
            screen = toScreen(position);
        }
        const double distance = QLineF(screenPoint, screen).length();
        if (distance > 5.0) continue;
        if (kind_ == Kind::Perspective) {
            const double depth = length(subtract(position, cameraState_.position));
            if (best < 0 || depth < bestDepth ||
                (morphHandles_[best].edge && !morphHandles_[i].edge)) {
                bestDepth = depth;
                best = i;
            }
            continue;
        }
        const bool better = distance < bestDistance ||
                            (best >= 0 && morphHandles_[best].edge && !morphHandles_[i].edge);
        if (better) {
            bestDistance = std::min(distance, bestDistance);
            best = i;
        }
    }
    return best;
}

std::optional<hammer::vmf::Vec3> MapViewWidget::morphSelectionCentroid() const
{
    hammer::vmf::Vec3 sum{};
    int count = 0;
    for (const hammer::vmf::MorphHandle& handle : morphHandles_) {
        if (!handle.selected) continue;
        sum = add(sum, handle.position);
        ++count;
    }
    if (count == 0) return std::nullopt;
    return hammer::vmf::Vec3{sum.x / count, sum.y / count, sum.z / count};
}

double MapViewWidget::morphGizmoWorldLength(const hammer::vmf::Vec3& origin) const
{
    // Roughly constant on screen: scale the arrow with camera distance.
    const double distance = length(subtract(origin, cameraState_.position));
    return std::max(16.0, distance * 0.15);
}

std::optional<hammer::vmf::Vec3> MapViewWidget::gizmoOrigin() const
{
    if (tool_ == Tool::Morph && morphActive_) return morphSelectionCentroid();
    if (tool_ == Tool::Selection && !selection_.empty() && selectionBounds_.valid)
        return selectionBounds_.center();
    return std::nullopt;
}

int MapViewWidget::morphGizmoHitTest(const QPointF& screenPoint) const
{
    if (kind_ != Kind::Perspective) return -1;
    const auto centroid = gizmoOrigin();
    if (!centroid) return -1;
    const double armLength = morphGizmoWorldLength(*centroid);
    static const hammer::vmf::Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int best = -1;
    double bestDistance = 8.0;
    for (int axis = 0; axis < 3; ++axis) {
        const hammer::vmf::Vec3 tip = add(*centroid, scale(axes[axis], armLength));
        QLineF line;
        if (!projectCameraLine(*centroid, tip, line)) continue;
        const double distance = pointSegmentDistance(screenPoint, line.p1(), line.p2());
        if (distance < bestDistance) {
            bestDistance = distance;
            best = axis;
        }
    }
    return best;
}

QList<int> MapViewWidget::morphHandlesInRect(const QRectF& rect) const
{
    // Morph3D::SelectInBox.
    // The original loops m_nVertices only: a box drag never selects edge
    // midpoint handles. When the display mode hides the vertices there is
    // nothing else to select, so the edge handles are used instead.
    const bool hasVertexHandles = std::any_of(morphHandles_.begin(), morphHandles_.end(),
                                              [](const hammer::vmf::MorphHandle& handle) { return !handle.edge; });
    QList<int> handles;
    for (int i = 0; i < static_cast<int>(morphHandles_.size()); ++i) {
        if (hasVertexHandles && morphHandles_[i].edge) continue;
        if (rect.contains(toScreen(morphHandles_[i].position))) handles.append(i);
    }
    return handles;
}

QPointF MapViewWidget::handlePlanePoint(Handle handle) const
{
    if (!selectionBounds_.valid) return {};
    QPointF minimum = project(selectionBounds_.minimum);
    QPointF maximum = project(selectionBounds_.maximum);
    const double minU = std::min(minimum.x(), maximum.x());
    const double maxU = std::max(minimum.x(), maximum.x());
    const double minV = std::min(minimum.y(), maximum.y());
    const double maxV = std::max(minimum.y(), maximum.y());
    const double centerU = (minU + maxU) * 0.5;
    const double centerV = (minV + maxV) * 0.5;
    switch (handle) {
    case Handle::TopLeft: return {minU, maxV};
    case Handle::Top: return {centerU, maxV};
    case Handle::TopRight: return {maxU, maxV};
    case Handle::Right: return {maxU, centerV};
    case Handle::BottomRight: return {maxU, minV};
    case Handle::Bottom: return {centerU, minV};
    case Handle::BottomLeft: return {minU, minV};
    case Handle::Left: return {minU, centerV};
    case Handle::None: return {centerU, centerV};
    }
    return {};
}

QPointF MapViewWidget::oppositeHandlePlanePoint(Handle handle) const
{
    switch (handle) {
    case Handle::TopLeft: return handlePlanePoint(Handle::BottomRight);
    case Handle::Top: return handlePlanePoint(Handle::Bottom);
    case Handle::TopRight: return handlePlanePoint(Handle::BottomLeft);
    case Handle::Right: return handlePlanePoint(Handle::Left);
    case Handle::BottomRight: return handlePlanePoint(Handle::TopLeft);
    case Handle::Bottom: return handlePlanePoint(Handle::Top);
    case Handle::BottomLeft: return handlePlanePoint(Handle::TopRight);
    case Handle::Left: return handlePlanePoint(Handle::Right);
    case Handle::None: return handlePlanePoint(Handle::None);
    }
    return {};
}

hammer::vmf::Vec3 MapViewWidget::planeFactorsToWorld(const QPointF& factors) const
{
    switch (kind_) {
    case Kind::Top: return {factors.x(), factors.y(), 1.0};
    case Kind::Front: return {1.0, factors.x(), factors.y()};
    case Kind::Side: return {factors.x(), 1.0, factors.y()};
    case Kind::Perspective: return {1.0, 1.0, 1.0};
    }
    return {1.0, 1.0, 1.0};
}

hammer::vmf::RotationAxis MapViewWidget::rotationAxis() const
{
    switch (kind_) {
    case Kind::Top: return hammer::vmf::RotationAxis::Z;
    case Kind::Front: return hammer::vmf::RotationAxis::X;
    case Kind::Side: return hammer::vmf::RotationAxis::Y;
    case Kind::Perspective: return hammer::vmf::RotationAxis::Z;
    }
    return hammer::vmf::RotationAxis::Z;
}

void MapViewWidget::drawViewLabel(QPainter& painter)
{
    if (!viewLabelVisible_) return;
    QString label = title();
    if (scene_) {
        label += QStringLiteral("  [%1 solids, %2 entities]")
                     .arg(static_cast<qulonglong>(scene_->brushes.size()))
                     .arg(static_cast<qulonglong>(scene_->entities.size()));
    }
    if (kind_ == Kind::Perspective) {
        label += QStringLiteral("  speed %1").arg(QString::number(flySpeed_, 'f', 0));
        label += mouseCaptured_ ? QStringLiteral("  Z: release  WASD: fly")
                                : QStringLiteral("  Z: mouse look");
    } else if (tool_ == Tool::Block) {
        label += QStringLiteral("  BLOCK");
    } else if (tool_ == Tool::Entity) {
        label += QStringLiteral("  ENTITY");
    } else if (tool_ == Tool::Decal) {
        label += QStringLiteral("  DECAL");
    } else if (tool_ == Tool::Overlay) {
        label += QStringLiteral("  OVERLAY");
    } else if (tool_ == Tool::Magnify) {
        label += QStringLiteral("  MAGNIFY");
    } else if (tool_ == Tool::Camera) {
        label += QStringLiteral("  CAMERA");
    } else if (tool_ == Tool::Clipper) {
        label += clipMode_ == ClipMode::Front ? QStringLiteral("  CLIP FRONT")
               : clipMode_ == ClipMode::Back  ? QStringLiteral("  CLIP BACK")
                                              : QStringLiteral("  CLIP BOTH");
    } else if (!selection_.empty()) {
        switch (effectiveTransformMode()) {
        case TransformMode::Scale: label += QStringLiteral("  RESIZE"); break;
        case TransformMode::Translate: label += QStringLiteral("  MOVE"); break;
        case TransformMode::Rotate: label += QStringLiteral("  ROTATE"); break;
        }
    }
    const int labelWidth = std::max(102, QFontMetrics(painter.font()).horizontalAdvance(label) + 12);
    const QRect labelRect(4, 4, labelWidth, 18);
    painter.fillRect(labelRect, QColor(0, 0, 0, 185));
    painter.setPen(QColor(226, 226, 226));
    painter.drawText(labelRect.adjusted(5, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
}

void MapViewWidget::wheelEvent(QWheelEvent* event)
{
    if (kind_ == Kind::Perspective) {
        double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
        if (std::abs(steps) < 0.001 && !event->pixelDelta().isNull()) {
            steps = static_cast<double>(event->pixelDelta().y()) / 40.0;
        }
        if (std::abs(steps) >= 0.001) {
            flySpeed_ = std::clamp(flySpeed_ * std::pow(1.25, steps), 8.0, 16384.0);
            reportCameraStatus();
            requestRepaint(false);
        }
        event->accept();
        return;
    }

    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    zoomAboutAnchor(zoom_ * factor, event->position());
    event->accept();
}

void MapViewWidget::zoomAboutAnchor(double zoom, const QPointF& anchor)
{
    // Keep the world point under "anchor" fixed on screen: with
    // screen = center + plane * zoom (per-axis sign folded into "plane"),
    // the new projection center is the anchor plus the old center-to-anchor
    // vector rescaled by the zoom ratio.
    const double previous = zoom_;
    zoom_ = std::clamp(zoom, 0.01, 64.0);
    if (zoom_ != previous && previous > 0.0) {
        const QPointF center = QPointF(rect().center()) + QPointF(pan_);
        const QPointF rescaled = anchor - (anchor - center) * (zoom_ / previous);
        pan_ = QPoint(static_cast<int>(std::lround(rescaled.x() - rect().center().x())),
                      static_cast<int>(std::lround(rescaled.y() - rect().center().y())));
    }
    fitPending_ = false;
    invalidateBaseFrame();
}

void MapViewWidget::mousePressEvent(QMouseEvent* event)
{
    emit activated(this);
    setFocus(Qt::MouseFocusReason);
    if (kind_ == Kind::Perspective && mouseCaptured_) {
        event->accept();
        return;
    }
    // CToolMaterial::OnLMouseDown3D / OnRMouseDown3D. Face picking only exists
    // in the 3D view here; see the note in MapDocumentWidget.
    // CToolDisplace::OnLMouseDown3D / OnRMouseDown3D: with a paint tool active
    // the displacement page owns the click, and it paints instead of picking a
    // face. The right button (or CTRL) lowers, as m_bRMBDown does.
    if (kind_ == Kind::Perspective && tool_ == Tool::TextureApplication &&
        displacementPaintActive_ &&
        (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        const auto hit = surfaceHit(event->position());
        // CToolDisplace::CollideWithSelectedDisps only ever hands the paint
        // manager a displacement that is in the face list, and
        // ApplySpatialPaintTool bails when the ray missed one.
        const bool paintable =
            hit && hit->displacement &&
            std::find(faceSelection_.begin(), faceSelection_.end(),
                      hammer::vmf::FaceRef{hit->solidId, hit->sideId}) != faceSelection_.end();
        if (paintable) {
            displacementPaintLower_ = event->button() == Qt::RightButton ||
                                      event->modifiers().testFlag(Qt::ControlModifier);
            displacementPainting_ = true;
            emit displacementPaintBegin(hit->position, hit->normal, displacementPaintLower_);
        }
        event->accept();
        return;
    }

    if (kind_ == Kind::Perspective && tool_ == Tool::TextureApplication) {
        const auto hit = surfaceHit(event->position());
        if (hit && hit->solidId >= 0 && hit->sideId >= 0) {
            const bool control = event->modifiers().testFlag(Qt::ControlModifier);
            const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
            if (event->button() == Qt::RightButton) {
                emit faceApplyRequested(hit->solidId, hit->sideId,
                                        event->modifiers().testFlag(Qt::AltModifier), shift);
            } else if (event->button() == Qt::LeftButton) {
                emit faceSelectRequested(hit->solidId, hit->sideId, control, shift);
            }
        }
        event->accept();
        return;
    }

    if (kind_ == Kind::Perspective && tool_ == Tool::Camera &&
        (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        // Camera3D::OnLMouseDown3D calls CMapView3D::EnableRotating, and
        // OnRMouseDown3D calls EnableStrafing: dragging looks around or slides
        // the camera. It never selects.
        if (event->button() == Qt::LeftButton) perspectiveRotating_ = true;
        else perspectiveStrafing_ = true;
        perspectiveDragLast_ = event->position();
        event->accept();
        return;
    }
    if (kind_ == Kind::Perspective &&
        (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)) {
        event->accept();
        return;
    }
    if (kind_ != Kind::Perspective && tool_ == Tool::Magnify &&
        (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        // ToolMagnify::OnLMouseDown2D / OnRMouseDown2D double or halve the
        // view's zoom, keeping the clicked point fixed under the cursor.
        zoomAboutAnchor(zoom_ * (event->button() == Qt::LeftButton ? 2.0 : 0.5),
                        event->position());
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        dragOrigin_ = event->position().toPoint() - pan_;
        panning_ = true;
        fitPending_ = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    if (kind_ != Kind::Perspective && tool_ == Tool::Camera) {
        if (cameras_.empty() || event->modifiers().testFlag(Qt::ShiftModifier)) {
            // Camera3D::OnLMouseDown2D: with no cameras yet, or Shift held,
            // start a brand-new camera at the click point (eye == look-at
            // until the drag moves the look-at handle away).
            double thirdAxis = 0.0;
            if (!selection_.empty()) {
                thirdAxis = kind_ == Kind::Top ? selectionBounds_.center().z :
                            kind_ == Kind::Front ? selectionBounds_.center().x : selectionBounds_.center().y;
            }
            const hammer::vmf::Vec3 world = planePointToWorld(screenToPlane(event->position()), thirdAxis);
            cameraDragIndex_ = static_cast<int>(cameras_.size());
            cameras_.push_back({world, world});
            activeCamera_ = cameraDragIndex_;
            cameraDragIsNew_ = true;
            cameraDragPart_ = 1; // MoveLook: drag stretches the look-at target away from the eye.
            cameraDragMissingAxis_ = thirdAxis;
            cameraDragOtherStart_ = world;
            cameraDragMovedStart_ = world;
            cameraDragging_ = true;
        } else if (const auto hit = cameraHitTest(event->position())) {
            activeCamera_ = hit->index;
            cameraDragIndex_ = hit->index;
            cameraDragIsNew_ = false;
            cameraDragPart_ = hit->part;
            const hammer::vmf::CameraDef& cam = cameras_[hit->index];
            const hammer::vmf::Vec3& part = hit->part == 0 ? cam.eye : cam.lookAt;
            cameraDragMissingAxis_ = kind_ == Kind::Top ? part.z : kind_ == Kind::Front ? part.x : part.y;
            cameraDragOtherStart_ = hit->part == 0 ? cam.lookAt : cam.eye;
            cameraDragMovedStart_ = part;
            cameraDragging_ = true;
        }
        if (cameraDragging_) requestRepaint();
        event->accept();
        return;
    }

    if (kind_ != Kind::Perspective && tool_ == Tool::Morph && morphActive_) {
        // Morph3D::OnLMouseDown2D: remember the handle that was hit so mouse
        // up can select it (or mouse move can drag it); otherwise start a
        // selection box.
        morphDragCtrl_ = event->modifiers().testFlag(Qt::ControlModifier);
        morphPressScreen_ = event->position();
        morphDragging_ = false;
        morphDragApplied_ = {};
        morphBoxSelecting_ = false;
        morphDragHandle_ = morphHandleHitTest(event->position());
        if (morphDragHandle_ >= 0) {
            const hammer::vmf::MorphHandle& handle = morphHandles_[morphDragHandle_];
            morphDragOrigin_ = handle.position;
            morphMissingAxis_ = kind_ == Kind::Top ? handle.position.z :
                                kind_ == Kind::Front ? handle.position.x : handle.position.y;
            // Clicking an unselected handle without Ctrl selects it straight
            // away so the drag that may follow moves the right handles.
            if (!handle.selected && !morphDragCtrl_) {
                emit morphSelectRequested({morphDragHandle_}, false);
            }
        } else {
            morphBoxSelecting_ = true;
            morphBoxStart_ = morphBoxCurrent_ = event->position();
        }
        requestRepaint(false);
        event->accept();
        return;
    }

    if (kind_ == Kind::Perspective && tool_ == Tool::Morph && morphActive_) {
        // 3D vertex manipulation: a click on a mover-gizmo arrow starts an
        // axis-constrained drag of the selected handles; a click on a handle
        // selects it (Ctrl toggles); a click on empty space clears.
        const bool control = event->modifiers().testFlag(Qt::ControlModifier);
        const int axis = morphGizmoHitTest(event->position());
        if (axis >= 0) {
            if (const auto centroid = morphSelectionCentroid()) {
                morphGizmoAxis_ = axis;
                morphGizmoPressScreen_ = event->position();
                morphGizmoApplied_ = 0.0;
                morphGizmoOrigin_ = *centroid;
                gizmoSelectMode_ = false;
                requestRepaint(true);
            }
            event->accept();
            return;
        }
        const int handle = morphHandleHitTest(event->position());
        if (handle >= 0) {
            emit morphSelectRequested({handle}, control);
        } else if (!control) {
            emit morphSelectionClearRequested();
        }
        requestRepaint(false);
        event->accept();
        return;
    }

    if (kind_ != Kind::Perspective && tool_ == Tool::Clipper) {
        // Clipper3D::OnLMouseDown2D. Grab an endpoint handle if one was hit;
        // otherwise only start a NEW clip line when the existing one belongs to
        // a differently oriented view or Shift is held, exactly as the original
        // does. A plain click anywhere else is deliberately ignored.
        double thirdAxis = 0.0;
        if (selectionBounds_.valid) {
            thirdAxis = kind_ == Kind::Top ? selectionBounds_.center().z :
                        kind_ == Kind::Front ? selectionBounds_.center().x : selectionBounds_.center().y;
        }
        const hammer::vmf::Vec3 axis = viewAxis();
        const hammer::vmf::Vec3 world = planePointToWorld(snapped(screenToPlane(event->position())), thirdAxis);

        bool starting = false;
        if (clipActive_) {
            const int hit = clipHandleHitTest(event->position());
            const bool sameView = std::abs(clipViewAxis_.x - axis.x) < 1e-9 &&
                                  std::abs(clipViewAxis_.y - axis.y) < 1e-9 &&
                                  std::abs(clipViewAxis_.z - axis.z) < 1e-9;
            if (hit >= 0) {
                clipPointHit_ = hit;
                clipDragging_ = true;
                clipDragStart_[0] = clipPoints_[0];
                clipDragStart_[1] = clipPoints_[1];
                clipMissingAxis_ = kind_ == Kind::Top ? clipPoints_[hit].z :
                                   kind_ == Kind::Front ? clipPoints_[hit].x : clipPoints_[hit].y;
            } else if (!sameView || event->modifiers().testFlag(Qt::ShiftModifier)) {
                starting = true;
            } else {
                event->accept();
                return;
            }
        } else {
            starting = true;
        }

        if (starting) {
            clipActive_ = true;
            clipViewAxis_ = axis;
            clipPoints_[0] = clipPoints_[1] = world;
            clipDragStart_[0] = clipDragStart_[1] = world;
            clipPointHit_ = 0;
            clipMissingAxis_ = thirdAxis;
            clipDragging_ = true;
        }
        emit clipLineChanged(clipPoints_[0], clipPoints_[1], axis);
        requestRepaint(false);
        event->accept();
        return;
    }

    if (kind_ == Kind::Perspective && (tool_ == Tool::Decal || tool_ == Tool::Overlay)) {
        if (const auto hit = surfaceHit(event->position())) {
            if (tool_ == Tool::Decal)
                emit decalPlacementRequested(hit->position, hit->normal, hit->sideId);
            else
                emit overlayPlacementRequested(hit->position, hit->normal, hit->sideId);
        }
        event->accept();
        return;
    }

    if (kind_ != Kind::Perspective && tool_ == Tool::Block) {
        // A pending (unconfirmed) box exposes resize handles; grabbing one
        // reshapes it instead of starting over.
        if (pendingBlock_.valid && !creationDragging_) {
            const Handle handle = creationHandleHitTest(event->position());
            if (handle != Handle::None) {
                // Screen top = plane vMax, screen left = plane uMin.
                switch (handle) {
                case Handle::TopLeft: creationMoveU_ = 0; creationMoveV_ = 1; break;
                case Handle::Top: creationMoveU_ = -1; creationMoveV_ = 1; break;
                case Handle::TopRight: creationMoveU_ = 1; creationMoveV_ = 1; break;
                case Handle::Right: creationMoveU_ = 1; creationMoveV_ = -1; break;
                case Handle::BottomRight: creationMoveU_ = 1; creationMoveV_ = 0; break;
                case Handle::Bottom: creationMoveU_ = -1; creationMoveV_ = 0; break;
                case Handle::BottomLeft: creationMoveU_ = 0; creationMoveV_ = 0; break;
                case Handle::Left: creationMoveU_ = 0; creationMoveV_ = -1; break;
                case Handle::None: break;
                }
                creationHandleDragging_ = true;
                event->accept();
                return;
            }
            // Not a handle: pressing inside the box grabs it and moves it, as
            // dragging a selected object does. Only the two axes this view
            // draws move.
            const QRectF box = QRectF(toScreen(pendingBlock_.minimum),
                                      toScreen(pendingBlock_.maximum)).normalized();
            if (box.contains(event->position())) {
                pendingMoveDragging_ = true;
                pendingMovePressPlane_ = screenToPlane(event->position());
                pendingMoveStartBlock_ = pendingBlock_;
                event->accept();
                return;
            }
        }
        // Drawing a new box replaces whatever was pending, in this view or any
        // other. The axis it will be extruded along is fixed here, by the view
        // that drew it.
        creationDragging_ = true;
        creationStartPlane_ = creationCurrentPlane_ = snapped(screenToPlane(event->position()));
        pendingExtrusionAxis_ = missingWorldAxis();
        pendingBlock_ = pendingBoundsFromPlane(creationStartPlane_, creationCurrentPlane_);
        emit blockPreviewChanged(pendingBlock_, pendingExtrusionAxis_);
        requestRepaint();
        event->accept();
        return;
    }
    if (kind_ != Kind::Perspective && tool_ == Tool::Entity) {
        const QPointF point = snapped(screenToPlane(event->position()));
        emit entityCreationRequested(planePointToWorld(point));
        event->accept();
        return;
    }

    if (kind_ == Kind::Perspective && tool_ == Tool::Entity) {
        // CToolEntity::OnLMouseDown3D: pObject = NearestObjectAt(...) picks
        // whatever is nearest the ray (solid or point entity); if it isn't a
        // CMapSolid the click is swallowed ("Clicked on a point entity - do
        // nothing."). Only once the nearest hit is a solid does it trace the
        // ray against that solid's face to find the impact point. Empty space
        // (pObject == NULL) likewise does nothing - no default-distance
        // fallback placement.
        const std::vector<hammer::vmf::ObjectRef> hits = hitTestAll(event->position());
        if (!hits.empty() && hits.front().type != hammer::vmf::ObjectType::Solid) {
            event->accept();
            return;
        }
        if (const auto hit = surfaceHit(event->position())) {
            emit entityPlacementRequested(hit->position, hit->normal);
        }
        event->accept();
        return;
    }

    // CMapView3D::OnLButtonDown only ever offers the click to the active tool;
    // picking objects is CToolSelection::OnLMouseDown3D, not a fallback. Tools
    // without a 3D behaviour (Magnify) or whose 3D behaviour was handled above
    // must therefore swallow the click instead of selecting.
    if (kind_ == Kind::Perspective && tool_ != Tool::Selection) {
        event->accept();
        return;
    }

    if (kind_ != Kind::Perspective && !selection_.empty()) {
        activeHandle_ = hitHandle(event->position());
        if (activeHandle_ != Handle::None &&
            effectiveTransformMode() == TransformMode::Translate) {
            // Edge handle in Translate mode: an axis-constrained whole-selection
            // move, reusing the object-drag machinery (snapping included).
            translateAxis_ = (activeHandle_ == Handle::Left || activeHandle_ == Handle::Right) ? 0 : 1;
            activeHandle_ = Handle::None;
            objectDragPending_ = true;
            objectDragStart_ = screenToPlane(event->position());
            objectDragPressScreen_ = event->position();
            lastObjectDragDelta_ = {};
            objectDragSnapRefValid_ = selectionBounds_.valid;
            objectDragSnapRef_ = isSinglePointEntitySelection() ? selectionBounds_.center()
                                                                : selectionBounds_.minimum;
            // A clean click on the handle still counts as a click on the
            // selection: cycle the handles instead of doing nothing.
            cycleModeOnRelease_ = true;
            event->accept();
            return;
        }
        if (activeHandle_ != Handle::None) {
            transforming_ = true;
            transformMoved_ = false;
            objectDragPressScreen_ = event->position();
            transformHandlePlane_ = handlePlanePoint(activeHandle_);
            transformAnchorPlane_ = oppositeHandlePlanePoint(activeHandle_);
            transformCenterPlane_ = handlePlanePoint(Handle::None);
            transformPivotWorld_ = planePointToWorld(transformAnchorPlane_,
                                                      kind_ == Kind::Top ? selectionBounds_.center().z :
                                                      kind_ == Kind::Front ? selectionBounds_.center().x : selectionBounds_.center().y);
            lastScaleFactors_ = {1.0, 1.0};
            lastRotationRadians_ = 0.0;
            if (transformMode_ == TransformMode::Scale) {
                transformStartAngle_ = 0.0;
                emit resizeStarted();
            } else {
                // Pivot on the vertex centroid, not the AABB center: the AABB
                // of a rotated brush shifts, so an AABB pivot makes repeated
                // rotations walk the brush. The centroid is rotation-invariant.
                transformPivotWorld_ = selectionCentroidWorld();
                transformCenterPlane_ = project(transformPivotWorld_);
                // Reference the press cursor, not the grabbed handle: for a
                // point entity the handle's plane position degenerates to the
                // origin and would make the first move jump.
                const QPointF pressPlane = screenToPlane(event->position());
                transformStartAngle_ = std::atan2(pressPlane.y() - transformCenterPlane_.y(),
                                                  pressPlane.x() - transformCenterPlane_.x());
                emit rotateStarted();
            }
            event->accept();
            return;
        }
    }

    // Selection3D::OnLMouseDown2D / OnLMouseDown3D, both of which funnel into
    // CMapView::SelectAt(vPoint, bMakeFirst = !Ctrl, false):
    //   * Ctrl held  -> SelectAt(..., bMakeFirst = false): toggle the object
    //     under the cursor into/out of the selection, keeping the rest.
    //   * otherwise  -> SelectAt(..., bMakeFirst = true): SelectObject(NULL,
    //     scClear) first, so a click on empty space clears the selection and a
    //     click on an object replaces it.
    // SelectAt also fills the hit list (ClearHitList/AddHit/SetCurrentHit),
    // which is what makes Select Next/Previous Object work; publish it here.
    // The mover gizmo also serves the selection tool in 3D: grabbing an arrow
    // moves the whole selection along that axis instead of re-picking.
    if (kind_ == Kind::Perspective && tool_ == Tool::Selection && !selection_.empty()) {
        const int axis = morphGizmoHitTest(event->position());
        if (axis >= 0) {
            if (const auto origin = gizmoOrigin()) {
                morphGizmoAxis_ = axis;
                morphGizmoPressScreen_ = event->position();
                morphGizmoApplied_ = 0.0;
                morphGizmoOrigin_ = *origin;
                gizmoSelectMode_ = true;
                gizmoMoveStarted_ = false;
                requestRepaint(true);
            }
            event->accept();
            return;
        }
    }

    const bool toggle = event->modifiers().testFlag(Qt::ControlModifier);
    const bool additive = event->modifiers().testFlag(Qt::ShiftModifier);
    const std::vector<hammer::vmf::ObjectRef> hits = hitTestAll(event->position());
    emit hitListChanged(hits);

    if (hits.empty()) {
        // No hit: the original still clears here (bMakeFirst), and the drag that
        // may follow rubber-bands a selection box (Box3D / SelectInBox).
        if (!toggle) emit clearSelectionRequested();
        boxSelectPending_ = true;
        boxSelecting_ = false;
        boxSelectStart_ = boxSelectCurrent_ = event->position();
        event->accept();
        return;
    }

    // If the topmost object under the cursor is already selected, a plain
    // click grabs it for a drag instead of re-selecting. Only the front hit
    // counts: when an unselected object (e.g. a point entity inside a selected
    // brush's footprint) has hit priority, clicking it must select it rather
    // than silently grabbing the selection underneath.
    const bool grabSelected = !toggle && !additive && isSelected(hits.front());
    if (!grabSelected) emit selectionRequested(hits.front(), toggle, additive);
    if (kind_ == Kind::Perspective && !toggle) {
        // CMapView3D::BeginPick: holding the button cycles deeper into the hit
        // list every 500 ms until the button comes up (EndPick).
        if (!pickNextTimer_) {
            pickNextTimer_ = new QTimer(this);
            pickNextTimer_->setInterval(500);
            connect(pickNextTimer_, &QTimer::timeout, this, [this] {
                emit selectNextHitRequested();
            });
        }
        pickNextTimer_->start();
    }
    if (!toggle && kind_ != Kind::Perspective) {
        objectDragPending_ = true;
        objectDragStart_ = screenToPlane(event->position());
        objectDragPressScreen_ = event->position();
        lastObjectDragDelta_ = {};
        objectDragSnapRefValid_ = selectionBounds_.valid;
        objectDragSnapRef_ = isSinglePointEntitySelection() ? selectionBounds_.center()
                                                            : selectionBounds_.minimum;
        // Clicking (not dragging) an already-selected object cycles the
        // transform handles on release.
        cycleModeOnRelease_ = grabSelected;
    }
    event->accept();
}

void MapViewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    emit activated(this);
    setFocus(Qt::MouseFocusReason);
    if (event->button() != Qt::LeftButton || mouseCaptured_) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    // CMapView3D::OnLButtonDblClk hands the click to the active tool only;
    // opening object properties is the selection tool's behaviour.
    if (kind_ == Kind::Perspective && tool_ != Tool::Selection) {
        event->accept();
        return;
    }

    // Every pickable object opens its properties: point entities, brush
    // entities, and plain world solids (which open on the VisGroup page - see
    // MapDocumentWidget's handler). Only an empty click falls through.
    const auto object = hitTest(event->position());
    if (!object || !scene_) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    resetInteraction();
    emit objectPropertiesRequested(*object);
    event->accept();
}

void MapViewWidget::mouseMoveEvent(QMouseEvent* event)
{
    // CToolDisplace::OnMouseMove3D keeps painting while the button is held.
    if (displacementPainting_) {
        const auto hit = surfaceHit(event->position());
        if (hit && hit->displacement &&
            std::find(faceSelection_.begin(), faceSelection_.end(),
                      hammer::vmf::FaceRef{hit->solidId, hit->sideId}) != faceSelection_.end()) {
            emit displacementPaintMoved(hit->position, displacementPaintLower_);
        }
        event->accept();
        return;
    }
    if (kind_ == Kind::Perspective && mouseCaptured_) {
        // Native Wayland supplies unbounded relative motion directly. Ignore
        // Qt's absolute event for the same physical movement to avoid applying
        // the camera delta twice.
        if (!waylandPointerCapture_) updateCapturedMouse(event);
        event->accept();
        return;
    }
    if (kind_ == Kind::Perspective && (perspectiveRotating_ || perspectiveStrafing_)) {
        // CMapView3D::ControlCamera. The original samples the cursor's offset
        // from the view centre every frame and warps it back; dragging here
        // supplies the same relative motion without hiding the pointer.
        const QPointF delta = event->position() - perspectiveDragLast_;
        perspectiveDragLast_ = event->position();
        if (perspectiveStrafing_) {
            // Left-right slides the camera sideways. Up-down moves it forward
            // and back with Shift held (or while also rotating), otherwise up
            // and down, matching MoveForward/MoveUp/MoveRight.
            const bool forwardMode = event->modifiers().testFlag(Qt::ShiftModifier) ||
                                     perspectiveRotating_;
            const hammer::vmf::Vec3 vertical = forwardMode
                ? hammer::camera::forwardVector(cameraState_)
                : hammer::camera::upVector(cameraState_);
            const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
            cameraState_.position = add(cameraState_.position, scale(right, delta.x() * 2.0));
            cameraState_.position = add(cameraState_.position, scale(vertical, -delta.y() * 2.0));
            invalidateBaseFrame();
            reportCameraStatus();
        } else {
            applyCameraLookDelta(delta.x(), delta.y());
        }
        event->accept();
        return;
    }
    updateCoordinateText(event->position().toPoint());
    if (panning_) {
        pan_ = event->position().toPoint() - dragOrigin_;
        invalidateBaseFrame();
        event->accept();
        return;
    }
    if (pendingMoveDragging_ && pendingBlock_.valid && (event->buttons() & Qt::LeftButton)) {
        int uAxis = 0;
        int vAxis = 1;
        planeWorldAxes(uAxis, vAxis);
        const QPointF planeDelta = screenToPlane(event->position()) - pendingMovePressPlane_;
        // Snap the box's minimum corner rather than the bare delta, so a drag
        // pulls an off-grid box back onto the grid instead of sliding it in
        // grid steps forever (applyObjectDragUpdate does the same).
        const auto moveAxis = [&](int axis, double delta) {
            const double startMin = axisValue(pendingMoveStartBlock_.minimum, axis);
            const double startMax = axisValue(pendingMoveStartBlock_.maximum, axis);
            const double newMin = snap(startMin + delta);
            axisRef(pendingBlock_.minimum, axis) = newMin;
            axisRef(pendingBlock_.maximum, axis) = newMin + (startMax - startMin);
        };
        moveAxis(uAxis, planeDelta.x());
        moveAxis(vAxis, planeDelta.y());
        emit blockPreviewChanged(pendingBlock_, pendingExtrusionAxis_);
        updateAutoScroll(event->position());
        requestRepaint();
        event->accept();
        return;
    }
    if (creationHandleDragging_ && pendingBlock_.valid && (event->buttons() & Qt::LeftButton)) {
        // Only the two axes this view draws move; the third keeps whatever the
        // other views set it to.
        const QPointF plane = snapped(screenToPlane(event->position()));
        int uAxis = 0;
        int vAxis = 1;
        planeWorldAxes(uAxis, vAxis);
        if (creationMoveU_ == 0) axisRef(pendingBlock_.minimum, uAxis) = plane.x();
        else if (creationMoveU_ == 1) axisRef(pendingBlock_.maximum, uAxis) = plane.x();
        if (creationMoveV_ == 0) axisRef(pendingBlock_.minimum, vAxis) = plane.y();
        else if (creationMoveV_ == 1) axisRef(pendingBlock_.maximum, vAxis) = plane.y();
        // Dragging an edge past its opposite flips it; keep (min, max) ordered
        // so the next grab reads the edges it looks like it is grabbing.
        for (const int axis : {uAxis, vAxis}) {
            if (axisValue(pendingBlock_.minimum, axis) > axisValue(pendingBlock_.maximum, axis)) {
                std::swap(axisRef(pendingBlock_.minimum, axis), axisRef(pendingBlock_.maximum, axis));
                if (axis == uAxis && creationMoveU_ >= 0) creationMoveU_ = 1 - creationMoveU_;
                if (axis == vAxis && creationMoveV_ >= 0) creationMoveV_ = 1 - creationMoveV_;
            }
        }
        emit blockPreviewChanged(pendingBlock_, pendingExtrusionAxis_);
        updateAutoScroll(event->position());
        requestRepaint();
        event->accept();
        return;
    }
    if (creationDragging_ && (event->buttons() & Qt::LeftButton)) {
        creationCurrentPlane_ = snapped(screenToPlane(event->position()));
        pendingBlock_ = pendingBoundsFromPlane(creationStartPlane_, creationCurrentPlane_);
        emit blockPreviewChanged(pendingBlock_, pendingExtrusionAxis_);
        updateAutoScroll(event->position());
        requestRepaint();
        event->accept();
        return;
    }
    if (morphGizmoAxis_ >= 0 && (event->buttons() & Qt::LeftButton) &&
        kind_ == Kind::Perspective && (tool_ == Tool::Morph || tool_ == Tool::Selection)) {
        // Axis-constrained gizmo drag: project the mouse delta onto the arrow's
        // screen direction, convert that fraction back to world units along the
        // axis, snap, and emit the increment.
        static const hammer::vmf::Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const hammer::vmf::Vec3 axis = axes[morphGizmoAxis_];
        const double armLength = morphGizmoWorldLength(morphGizmoOrigin_);
        QLineF line;
        if (projectCameraLine(morphGizmoOrigin_, add(morphGizmoOrigin_, scale(axis, armLength)),
                              line)) {
            const QPointF screenAxis = line.p2() - line.p1();
            const double lengthSquared = screenAxis.x() * screenAxis.x() +
                                         screenAxis.y() * screenAxis.y();
            if (lengthSquared > 1e-6) {
                const QPointF mouseDelta = event->position() - morphGizmoPressScreen_;
                const double fraction = (mouseDelta.x() * screenAxis.x() +
                                         mouseDelta.y() * screenAxis.y()) / lengthSquared;
                const double total = snap(fraction * armLength);
                const double incremental = total - morphGizmoApplied_;
                if (incremental != 0.0) {
                    morphGizmoApplied_ = total;
                    if (gizmoSelectMode_) {
                        if (!gizmoMoveStarted_) {
                            gizmoMoveStarted_ = true;
                            emit moveStarted();
                        }
                        emit moveDeltaRequested(scale(axis, incremental));
                    } else {
                        emit morphMoveRequested(scale(axis, incremental));
                    }
                }
            }
        }
        requestRepaint(false);
        event->accept();
        return;
    }
    if (morphBoxSelecting_ && (event->buttons() & Qt::LeftButton)) {
        morphBoxCurrent_ = event->position();
        requestRepaint(false);
        event->accept();
        return;
    }
    if (morphDragHandle_ >= 0 && (event->buttons() & Qt::LeftButton) && tool_ == Tool::Morph) {
        // Morph3D::UpdateTranslation. Vertex handles snap to the grid; edge
        // handles deliberately do not, because an edge midpoint does not
        // necessarily lie on the grid in the first place.
        if (!morphDragging_ &&
            QLineF(event->position(), morphPressScreen_).length() < 3.0) {
            event->accept();
            return;
        }
        morphDragging_ = true;
        const bool isEdge = morphHandles_[morphDragHandle_].edge;
        QPointF plane = screenToPlane(event->position());
        if (!isEdge) plane = snapped(plane);
        const hammer::vmf::Vec3 world = planePointToWorld(plane, morphMissingAxis_);
        const hammer::vmf::Vec3 target = subtract(world, morphDragOrigin_);
        const hammer::vmf::Vec3 delta = subtract(target, morphDragApplied_);
        if (std::abs(delta.x) > 1e-6 || std::abs(delta.y) > 1e-6 || std::abs(delta.z) > 1e-6) {
            morphDragApplied_ = target;
            emit morphMoveRequested(delta);
        }
        event->accept();
        return;
    }
    if (clipDragging_ && (event->buttons() & Qt::LeftButton) && clipPointHit_ >= 0) {
        // Clipper3D::UpdateTranslation, including the Ctrl (constrainMoveAll)
        // case that drags both clip points together.
        const hammer::vmf::Vec3 world =
            planePointToWorld(snapped(screenToPlane(event->position())), clipMissingAxis_);
        hammer::vmf::Vec3& moved = clipPoints_[clipPointHit_];
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            hammer::vmf::Vec3& other = clipPoints_[(clipPointHit_ + 1) % 2];
            const hammer::vmf::Vec3 delta{world.x - moved.x, world.y - moved.y, world.z - moved.z};
            other = {other.x + delta.x, other.y + delta.y, other.z + delta.z};
        }
        moved = world;
        emit clipLineChanged(clipPoints_[0], clipPoints_[1], viewAxis());
        requestRepaint(false);
        event->accept();
        return;
    }

    if (cameraDragging_ && (event->buttons() & Qt::LeftButton)) {
        // Camera3D::UpdateTranslation: move the dragged handle, and with Ctrl
        // held (constrainMoveAll) translate both handles together.
        const hammer::vmf::Vec3 world = planePointToWorld(screenToPlane(event->position()), cameraDragMissingAxis_);
        hammer::vmf::CameraDef& cam = cameras_[cameraDragIndex_];
        hammer::vmf::Vec3& moved = cameraDragPart_ == 0 ? cam.eye : cam.lookAt;
        hammer::vmf::Vec3& other = cameraDragPart_ == 0 ? cam.lookAt : cam.eye;
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const hammer::vmf::Vec3 delta{world.x - cameraDragMovedStart_.x,
                                          world.y - cameraDragMovedStart_.y,
                                          world.z - cameraDragMovedStart_.z};
            other = {cameraDragOtherStart_.x + delta.x, cameraDragOtherStart_.y + delta.y,
                     cameraDragOtherStart_.z + delta.z};
        } else {
            other = cameraDragOtherStart_;
        }
        moved = world;
        emit cameraEdited(cameraDragIndex_, cam.eye, cam.lookAt, cameraDragIsNew_);
        requestRepaint();
        event->accept();
        return;
    }
    if (transforming_ && (event->buttons() & Qt::LeftButton)) {
        if (QLineF(event->position(), objectDragPressScreen_).length() >= 3.0)
            transformMoved_ = true;
        QPointF current = screenToPlane(event->position());
        if (transformMode_ == TransformMode::Scale) {
            // The dragged edge lands exactly on the snapped cursor position,
            // so resizing keeps (or puts) that edge on the grid.
            current = snapped(current);
            QPointF total(1.0, 1.0);
            const double oldU = transformHandlePlane_.x() - transformAnchorPlane_.x();
            const double oldV = transformHandlePlane_.y() - transformAnchorPlane_.y();
            const bool changesU = activeHandle_ != Handle::Top && activeHandle_ != Handle::Bottom;
            const bool changesV = activeHandle_ != Handle::Left && activeHandle_ != Handle::Right;
            if (changesU && std::abs(oldU) > 0.001) total.setX(std::max(0.01, (current.x() - transformAnchorPlane_.x()) / oldU));
            if (changesV && std::abs(oldV) > 0.001) total.setY(std::max(0.01, (current.y() - transformAnchorPlane_.y()) / oldV));
            const QPointF incremental(total.x() / lastScaleFactors_.x(), total.y() / lastScaleFactors_.y());
            if (std::abs(incremental.x() - 1.0) > 1e-9 || std::abs(incremental.y() - 1.0) > 1e-9) {
                transformMoved_ = true;
                emit resizeDeltaRequested(planeFactorsToWorld(incremental), transformPivotWorld_);
                lastScaleFactors_ = total;
            }
        } else {
            double total = std::atan2(current.y() - transformCenterPlane_.y(), current.x() - transformCenterPlane_.x()) - transformStartAngle_;
            if (!event->modifiers().testFlag(Qt::AltModifier)) {
                const double step = 15.0 * Pi / 180.0;
                total = std::round(total / step) * step;
            }
            const double incremental = total - lastRotationRadians_;
            if (std::abs(incremental) > 1e-9) {
                transformMoved_ = true;
                emit rotateDeltaRequested(incremental, rotationAxis(), transformPivotWorld_);
                lastRotationRadians_ = total;
            }
        }
        event->accept();
        return;
    }
    if (boxSelectPending_ && (event->buttons() & Qt::LeftButton)) {
        // Tool3D::OnMouseMove2D only begins a translation once m_bMouseDragged
        // is set, i.e. after the cursor has actually left the press point.
        if (!boxSelecting_ &&
            QLineF(event->position(), boxSelectStart_).length() < 3.0) {
            event->accept();
            return;
        }
        boxSelecting_ = true;
        boxSelectCurrent_ = event->position();
        updateAutoScroll(event->position());
        requestRepaint(false);
        event->accept();
        return;
    }
    if (!objectDragPending_ || !(event->buttons() & Qt::LeftButton)) return;
    if (!objectDragging_) {
        if (QLineF(event->position(), objectDragPressScreen_).length() < 3.0) return;
        objectDragging_ = true;
        emit moveStarted();
    }
    applyObjectDragUpdate(event->position());
    updateAutoScroll(event->position());
    event->accept();
}

void MapViewWidget::applyObjectDragUpdate(const QPointF& position)
{
    QPointF planeDelta = screenToPlane(position) - objectDragStart_;
    if (translateAxis_ == 0) planeDelta.setY(0.0);
    else if (translateAxis_ == 1) planeDelta.setX(0.0);
    const hammer::vmf::Vec3 raw = planeDeltaToWorld(planeDelta);
    // Snap the selection's reference point (bounds minimum / entity origin),
    // not the bare delta: delta snapping moves in grid steps but never pulls
    // an off-grid object back onto the grid. Axes the drag doesn't touch stay
    // untouched so a constrained drag can't shift the object sideways.
    const auto snapAxis = [&](double reference, double delta) {
        if (delta == 0.0) return 0.0;
        if (!objectDragSnapRefValid_) return snap(delta);
        return snap(reference + delta) - reference;
    };
    const hammer::vmf::Vec3 total{snapAxis(objectDragSnapRef_.x, raw.x),
                                  snapAxis(objectDragSnapRef_.y, raw.y),
                                  snapAxis(objectDragSnapRef_.z, raw.z)};
    const hammer::vmf::Vec3 incremental{total.x - lastObjectDragDelta_.x,
                                        total.y - lastObjectDragDelta_.y,
                                        total.z - lastObjectDragDelta_.z};
    if (incremental.x != 0.0 || incremental.y != 0.0 || incremental.z != 0.0) {
        emit moveDeltaRequested(incremental);
        lastObjectDragDelta_ = total;
    }
}

void MapViewWidget::updateAutoScroll(const QPointF& cursor)
{
    if (kind_ == Kind::Perspective) return;
    autoScrollCursor_ = cursor;
    constexpr double Margin = 16.0;
    const bool nearEdge = cursor.x() < Margin || cursor.y() < Margin ||
                          cursor.x() > width() - Margin || cursor.y() > height() - Margin;
    if (!nearEdge) {
        if (autoScrollTimer_) autoScrollTimer_->stop();
        return;
    }
    if (!autoScrollTimer_) {
        autoScrollTimer_ = new QTimer(this);
        autoScrollTimer_->setInterval(30);
        connect(autoScrollTimer_, &QTimer::timeout, this, &MapViewWidget::autoScrollTick);
    }
    if (!autoScrollTimer_->isActive()) autoScrollTimer_->start();
}

void MapViewWidget::stopAutoScroll()
{
    if (autoScrollTimer_) autoScrollTimer_->stop();
}

void MapViewWidget::autoScrollTick()
{
    if (!(objectDragging_ || boxSelecting_ || creationDragging_ || creationHandleDragging_ ||
          pendingMoveDragging_)) {
        stopAutoScroll();
        return;
    }
    static constexpr double Margin = 16.0;
    static constexpr double MaxStep = 32.0;
    const auto step = [](double position, double extent) {
        if (position < Margin) return -std::min(MaxStep, Margin - position + 4.0);
        if (position > extent - Margin) return std::min(MaxStep, position - (extent - Margin) + 4.0);
        return 0.0;
    };
    const QPoint delta(static_cast<int>(std::lround(step(autoScrollCursor_.x(), width()))),
                       static_cast<int>(std::lround(step(autoScrollCursor_.y(), height()))));
    if (delta.isNull()) {
        stopAutoScroll();
        return;
    }
    // Scrolling toward the edge means the world slides the other way.
    pan_ -= delta;
    // Screen-anchored drag state must track the world, not the widget.
    const QPointF panDelta(-delta.x(), -delta.y());
    boxSelectStart_ += panDelta;
    objectDragPressScreen_ += panDelta;
    invalidateBaseFrame();
    // Re-apply the drag at the frozen cursor so the dragged content keeps
    // following the scroll without another real mouse move.
    if (objectDragging_) applyObjectDragUpdate(autoScrollCursor_);
    if (creationDragging_) {
        creationCurrentPlane_ = snapped(screenToPlane(autoScrollCursor_));
        pendingBlock_ = pendingBoundsFromPlane(creationStartPlane_, creationCurrentPlane_);
        emit blockPreviewChanged(pendingBlock_, pendingExtrusionAxis_);
    }
    if (boxSelecting_) boxSelectCurrent_ = autoScrollCursor_;
    requestRepaint();
}

void MapViewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    // CMapView3D::OnLButtonUp always reaches Selection3D::OnLMouseUp3D, which
    // ends with an unconditional pView->EndPick(). Kill the cycle timer before
    // any of the early returns below (mouse capture, camera drag, ...) can
    // swallow the release and leave it running.
    if (event->button() == Qt::LeftButton && pickNextTimer_) pickNextTimer_->stop();
    if (event->button() == Qt::LeftButton) stopAutoScroll();
    // CToolDisplace::OnLMouseUp3D / OnRMouseUp3D end the stroke.
    if (displacementPainting_ &&
        (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        displacementPainting_ = false;
        emit displacementPaintFinished();
        event->accept();
        return;
    }
    if (kind_ == Kind::Perspective && mouseCaptured_) {
        event->accept();
        return;
    }
    if (kind_ == Kind::Perspective && (perspectiveRotating_ || perspectiveStrafing_)) {
        // Camera3D::OnLMouseUp3D / OnRMouseUp3D end rotating / strafing.
        if (event->button() == Qt::LeftButton) perspectiveRotating_ = false;
        if (event->button() == Qt::RightButton) perspectiveStrafing_ = false;
        event->accept();
        return;
    }
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton)) {
        panning_ = false;
        setCursor(tool_ == Tool::Selection ? Qt::ArrowCursor : Qt::CrossCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    if (morphBoxSelecting_) {
        // Morph3D::SelectInBox on button up (Options.view2d.bAutoSelect).
        morphBoxSelecting_ = false;
        const QRectF box = QRectF(morphBoxStart_, event->position()).normalized();
        if (box.width() < 2.0 && box.height() < 2.0) {
            // A plain click on empty space clears the handle selection.
            emit morphSelectionClearRequested();
        } else {
            emit morphSelectRequested(morphHandlesInRect(box), false);
        }
        requestRepaint(false);
        event->accept();
        return;
    }
    if (morphGizmoAxis_ >= 0) {
        // Mover gizmo released: commit the axis drag as one move.
        const bool moved = morphGizmoApplied_ != 0.0;
        const bool selectMode = gizmoSelectMode_;
        const bool started = gizmoMoveStarted_;
        morphGizmoAxis_ = -1;
        morphGizmoApplied_ = 0.0;
        gizmoSelectMode_ = false;
        gizmoMoveStarted_ = false;
        if (selectMode) {
            if (started) emit moveFinished();
        } else if (moved) {
            emit morphMoveFinished();
        }
        requestRepaint(true);
        event->accept();
        return;
    }
    if (morphDragHandle_ >= 0) {
        const int handle = morphDragHandle_;
        const bool dragged = morphDragging_;
        const bool ctrl = morphDragCtrl_;
        morphDragHandle_ = -1;
        morphDragging_ = false;
        if (dragged) {
            // Morph3D::FinishTranslation( true ): commit the moved vertices.
            emit morphMoveFinished();
        } else {
            // Clicked a handle without moving: Morph3D::OnLMouseUp2D selects
            // it, or toggles it when Ctrl was held on button down.
            emit morphSelectRequested({handle}, ctrl);
        }
        requestRepaint(false);
        event->accept();
        return;
    }
    if (clipDragging_) {
        // Clipper3D::FinishTranslation recomputes the clip results even when
        // the "drag" was only a click.
        clipDragging_ = false;
        emit clipLineChanged(clipPoints_[0], clipPoints_[1], viewAxis());
        requestRepaint(false);
        event->accept();
        return;
    }
    if (cameraDragging_) {
        cameraDragging_ = false;
        const hammer::vmf::CameraDef& cam = cameras_[cameraDragIndex_];
        emit cameraEdited(cameraDragIndex_, cam.eye, cam.lookAt, cameraDragIsNew_);
        requestRepaint();
        event->accept();
        return;
    }

    if (pendingMoveDragging_) {
        pendingMoveDragging_ = false;
        stopAutoScroll();
        requestRepaint();
        event->accept();
        return;
    }
    if (creationHandleDragging_) {
        creationHandleDragging_ = false;
        creationMoveU_ = -1;
        creationMoveV_ = -1;
        stopAutoScroll();
        requestRepaint();
        event->accept();
        return;
    }
    if (creationDragging_) {
        // The box is NOT committed here: it stays as a resizable preview until
        // Enter confirms it (confirmBlockCreation) or Escape/another drag
        // replaces it, exactly as Hammer's Block tool works.
        creationDragging_ = false;
        stopAutoScroll();
        const double minimumExtent = gridSnapEnabled_ ? gridSpacing_ : 1.0;
        if (std::abs(creationCurrentPlane_.x() - creationStartPlane_.x()) < minimumExtent ||
            std::abs(creationCurrentPlane_.y() - creationStartPlane_.y()) < minimumExtent) {
            clearPendingBlock();  // a stray click, not a box
        }
        requestRepaint();
        event->accept();
        return;
    }
    if (transforming_) {
        transforming_ = false;
        activeHandle_ = Handle::None;
        emit transformFinished();
        // A handle click that never turned into a drag is still a click on
        // the selection: cycle the handle mode like a body click does.
        if (!transformMoved_) emit transformModeChangeRequested(nextTransformMode());
        event->accept();
        return;
    }
    if (boxSelectPending_) {
        const bool dragged = boxSelecting_;
        const QRectF box = QRectF(boxSelectStart_, event->position()).normalized();
        // OnLMouseUp2D/OnLMouseUp3D read bShift from the RELEASE event before
        // handing it to SelectInBox, not from the press.
        const bool additive = event->modifiers().testFlag(Qt::ShiftModifier);
        boxSelectPending_ = false;
        boxSelecting_ = false;
        if (dragged) {
            // Selection3D::OnLMouseUp2D / OnLMouseUp3D -> SelectInBox(doc, bShift).
            emit boxSelectionRequested(objectsInBox(box), additive);
        }
        requestRepaint(false);
        event->accept();
        return;
    }
    if (objectDragPending_) {
        if (objectDragging_) emit moveFinished();
        else if (cycleModeOnRelease_) emit transformModeChangeRequested(nextTransformMode());
        objectDragPending_ = false;
        objectDragging_ = false;
        translateAxis_ = -1;
        cycleModeOnRelease_ = false;
        lastObjectDragDelta_ = {};
    }
    event->accept();
}

void MapViewWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (hardwareViewport_) hardwareViewport_->setGeometry(rect());
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_) rayTracedViewport_->setGeometry(rect());
#endif
    if (waylandPointerCapture_ && waylandPointerLock_) {
        waylandPointerLock_->updateRegion(this);
    } else if (mouseCaptured_) {
        centerCapturedPointer();
    }
    baseFrameDirty_ = true;
    // The child hardware viewport receives its own resize event and marks the
    // FBO dirty. Scheduling a second parent/child repaint here rendered the
    // same full frame twice during interactive window resizing.
    if (!hardwareViewport_) requestRepaint();
}

void MapViewWidget::keyPressEvent(QKeyEvent* event)
{
    // Block tool: Enter commits the pending preview box.
    if (tool_ == Tool::Block && pendingBlock_.valid && !event->isAutoRepeat() &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        confirmBlockCreation();
        event->accept();
        return;
    }
    if (tool_ == Tool::Camera && !event->isAutoRepeat()) {
        // Camera3D::OnKeyDown2D/3D: PageUp/PageDown cycle the active camera,
        // Delete removes it, while the tool is active in any view.
        if (event->key() == Qt::Key_PageDown) {
            emit cameraCycleRequested(true);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageUp) {
            emit cameraCycleRequested(false);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete) {
            emit cameraDeleteRequested();
            event->accept();
            return;
        }
    }
    if (tool_ == Tool::Morph && !event->isAutoRepeat() && event->key() == Qt::Key_Escape) {
        // Morph3D::OnEscape: end a box selection, else clear the selected
        // handles, else leave the tool (ToolManager()->SetTool( TOOL_POINTER )).
        if (morphDragging_) {
            // Morph3D::FinishTranslation( false ) puts the handles back where
            // the drag started instead of committing the partial move.
            emit morphMoveRequested({-morphDragApplied_.x, -morphDragApplied_.y, -morphDragApplied_.z});
            morphDragHandle_ = -1;
            morphDragging_ = false;
            morphDragApplied_ = {};
        } else if (morphBoxSelecting_) {
            morphBoxSelecting_ = false;
        } else if (morphActive_) {
            emit morphEscapePressed();
        } else {
            // Nothing is in morph mode, so the first Escape already leaves the
            // tool, as OnEscape does when IsEmpty().
            emit selectionToolRequested();
        }
        requestRepaint(false);
        event->accept();
        return;
    }
    if (tool_ == Tool::Clipper && !event->isAutoRepeat() &&
        !(kind_ == Kind::Perspective && mouseCaptured_)) {
        // Clipper3D::OnKeyDown2D/OnKeyDown3D: Enter applies the clip, Escape
        // clears the clip line and a second Escape returns to the Selection
        // tool (Clipper3D::OnEscape -> SetTool( TOOL_POINTER )).
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            emit clipApplyRequested();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            if (clipActive_) emit clipCancelRequested();
            else emit selectionToolRequested();
            event->accept();
            return;
        }
    }
    if (kind_ == Kind::Perspective) {
        if (event->key() == Qt::Key_End && !event->isAutoRepeat()) {
            // Jump to the water-preview harness's "overhead" shot, so a render
            // from that rig and this view can be compared frame to frame.
            // These are absolute world coordinates matched to the harness table
            // in tests/water_preview_harness.cpp - keep the two in step.
            setCameraTransform({0.0, -40.0, 500.0}, {0.0, 0.0, 0.0});
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Z && !event->isAutoRepeat()) {
            toggleMouseCapture();
            event->accept();
            return;
        }
        if (mouseCaptured_ &&
            (event->key() == Qt::Key_W || event->key() == Qt::Key_A ||
             event->key() == Qt::Key_S || event->key() == Qt::Key_D)) {
            if (!event->isAutoRepeat()) {
                const bool wasIdle = pressedKeys_.isEmpty();
                pressedKeys_.insert(event->key());
                if (wasIdle && flyTimer_) {
                    flyElapsed_.restart();
                    flyTimer_->start();
                }
            }
            event->accept();
            return;
        }
        if (mouseCaptured_ && event->key() == Qt::Key_Escape) {
            setMouseCaptured(false);
            event->accept();
            return;
        }
    }

    // Hammer arrow keys: an arrow nudges the selection by the current grid
    // size in the arrow's direction (one unit when grid snap is off, or with
    // ALT for a fine step); SHIFT duplicates the selection and moves the copy
    // by the same step - and because the copy is what ends up selected,
    // holding SHIFT and pressing again makes a new copy each time, walking a
    // row of them out at grid spacing.
    hammer::vmf::Vec3 delta{};
    const bool fineStep = event->modifiers().testFlag(Qt::AltModifier) || !gridSnapEnabled_;
    const double amount = fineStep ? 1.0 : gridSpacing_;
    if (kind_ == Kind::Top) {
        if (event->key() == Qt::Key_Left) delta.x = -amount;
        if (event->key() == Qt::Key_Right) delta.x = amount;
        if (event->key() == Qt::Key_Down) delta.y = -amount;
        if (event->key() == Qt::Key_Up) delta.y = amount;
    } else if (kind_ == Kind::Front) {
        if (event->key() == Qt::Key_Left) delta.y = -amount;
        if (event->key() == Qt::Key_Right) delta.y = amount;
        if (event->key() == Qt::Key_Down) delta.z = -amount;
        if (event->key() == Qt::Key_Up) delta.z = amount;
    } else if (kind_ == Kind::Side) {
        if (event->key() == Qt::Key_Left) delta.x = -amount;
        if (event->key() == Qt::Key_Right) delta.x = amount;
        if (event->key() == Qt::Key_Down) delta.z = -amount;
        if (event->key() == Qt::Key_Up) delta.z = amount;
    }
    if (delta.x != 0.0 || delta.y != 0.0 || delta.z != 0.0) {
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            // "A new copy every time" means every PRESS: keyboard autorepeat
            // firing this at ~30 Hz would spray dozens of copies from one
            // held key. A plain nudge keeps autorepeat, which is how nudging
            // is meant to feel.
            if (!event->isAutoRepeat()) emit nudgeDuplicateRequested(delta);
        } else {
            emit nudgeRequested(delta);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (pendingBlock_.valid || transforming_ || objectDragPending_) resetInteraction();
        else emit clearSelectionRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void MapViewWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (kind_ == Kind::Perspective && mouseCaptured_ &&
        (event->key() == Qt::Key_W || event->key() == Qt::Key_A ||
         event->key() == Qt::Key_S || event->key() == Qt::Key_D)) {
        if (!event->isAutoRepeat()) {
            pressedKeys_.remove(event->key());
            if (pressedKeys_.isEmpty() && flyTimer_) flyTimer_->stop();
        }
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void MapViewWidget::focusOutEvent(QFocusEvent* event)
{
    if (mouseCaptured_) setMouseCaptured(false);
    pressedKeys_.clear();
    QWidget::focusOutEvent(event);
}

void MapViewWidget::updateCoordinateText(const QPoint& point)
{
    const QPointF plane = screenToPlane(point);
    const int a = static_cast<int>(std::lround(plane.x()));
    const int b = static_cast<int>(std::lround(plane.y()));
    switch (kind_) {
    case Kind::Perspective: reportCameraStatus(); break;
    case Kind::Top: emit cursorPositionChanged(QStringLiteral("%1, %2, 0").arg(a).arg(b)); break;
    case Kind::Front: emit cursorPositionChanged(QStringLiteral("0, %1, %2").arg(a).arg(b)); break;
    case Kind::Side: emit cursorPositionChanged(QStringLiteral("%1, 0, %2").arg(a).arg(b)); break;
    }
}

void MapViewWidget::resetCamera()
{
    cameraState_ = hammer::camera::State{};
    cameraState_.orthographicHeight = 1024.0;
    flySpeed_ = 512.0;
    pressedKeys_.clear();
}

void MapViewWidget::fitCameraToScene()
{
    if (!scene_ || !scene_->hasBounds) {
        resetCamera();
        return;
    }

    const hammer::vmf::Vec3 center{
        (scene_->minimum.x + scene_->maximum.x) * 0.5,
        (scene_->minimum.y + scene_->maximum.y) * 0.5,
        (scene_->minimum.z + scene_->maximum.z) * 0.5
    };
    const hammer::vmf::Vec3 diagonal{
        scene_->maximum.x - scene_->minimum.x,
        scene_->maximum.y - scene_->minimum.y,
        scene_->maximum.z - scene_->minimum.z
    };
    const double radius = std::max(64.0, length(diagonal) * 0.5);
    cameraState_.orthographicHeight = std::max(128.0, radius * 2.5);
    const double distance = std::max(128.0,
        radius / std::tan(cameraState_.verticalFovRadians * 0.5) * 1.35);
    cameraState_.position = add(center,
        scale(hammer::camera::forwardVector(cameraState_), -distance));
}

void MapViewWidget::toggleMouseCapture()
{
    setMouseCaptured(!mouseCaptured_);
}

void MapViewWidget::setMouseCaptured(bool captured)
{
    if (kind_ != Kind::Perspective || mouseCaptured_ == captured) return;
    mouseCaptured_ = captured;
    pressedKeys_.clear();

    if (mouseCaptured_) {
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::BlankCursor);
        flyElapsed_.restart();
        if (flyTimer_) flyTimer_->stop();

        const bool wayland = QGuiApplication::platformName().contains(QStringLiteral("wayland"),
                                                                      Qt::CaseInsensitive);
        const QPoint center = mapToGlobal(rect().center());
        lastCapturedGlobalPosition_ = center;
        if (!wayland) {
            ignoreWarpEvent_ = true;
            QCursor::setPos(center);
        }
        waylandPointerCapture_ = wayland && waylandPointerLock_ &&
                                 waylandPointerLock_->begin(this);
        if (waylandPointerCapture_) centerCapturedPointer();
        else beginFallbackMouseCapture();
    } else {
        if (flyTimer_) flyTimer_->stop();
        if (waylandPointerLock_) waylandPointerLock_->end();
        waylandPointerCapture_ = false;
        if (QWidget::mouseGrabber() == this) releaseMouse();
        unsetCursor();
        ignoreWarpEvent_ = false;
    }

    reportCameraStatus();
    requestRepaint(false);
}

void MapViewWidget::beginFallbackMouseCapture()
{
    if (!mouseCaptured_) return;
    grabMouse(QCursor(Qt::BlankCursor));

    const bool wayland = QGuiApplication::platformName().contains(QStringLiteral("wayland"),
                                                                  Qt::CaseInsensitive);
    if (wayland) {
        // A compositor without relative-pointer support may reject warping.
        // Make the best-effort request, then track the actual position Qt
        // reports so the first fallback delta cannot jump the camera.
        QCursor::setPos(mapToGlobal(rect().center()));
        lastCapturedGlobalPosition_ = QCursor::pos();
        ignoreWarpEvent_ = false;
    } else {
        centerCapturedPointer();
    }
}

void MapViewWidget::centerCapturedPointer()
{
    if (!mouseCaptured_ || kind_ != Kind::Perspective) return;
    const QPoint center = mapToGlobal(rect().center());
    lastCapturedGlobalPosition_ = center;

    if (waylandPointerCapture_ && waylandPointerLock_) {
        waylandPointerLock_->centerCursor(this);
        ignoreWarpEvent_ = false;
        return;
    }

    const bool wayland = QGuiApplication::platformName().contains(QStringLiteral("wayland"),
                                                                  Qt::CaseInsensitive);
    QCursor::setPos(center);
    if (wayland) {
        lastCapturedGlobalPosition_ = QCursor::pos();
        ignoreWarpEvent_ = false;
    } else {
        ignoreWarpEvent_ = true;
    }
}

void MapViewWidget::handleWaylandPointerLockUnavailable()
{
    if (!mouseCaptured_ || !waylandPointerCapture_) return;
    waylandPointerCapture_ = false;
    beginFallbackMouseCapture();
    reportCameraStatus();
}

void MapViewWidget::handleWaylandPointerLockLost()
{
    if (mouseCaptured_) setMouseCaptured(false);
}

void MapViewWidget::applyCapturedMouseDelta(double deltaX, double deltaY)
{
    if (!mouseCaptured_ || kind_ != Kind::Perspective) return;
    applyCameraLookDelta(deltaX, deltaY);
}

void MapViewWidget::applyCameraLookDelta(double deltaX, double deltaY)
{
    if (kind_ != Kind::Perspective) return;
    if (std::abs(deltaX) < 0.001 && std::abs(deltaY) < 0.001) return;

    constexpr double sensitivity = 0.0032;
    // Source yaw increases toward +Y, which is a left turn. Mouse motion to
    // the right must therefore decrease yaw now that the viewport is no longer
    // horizontally mirrored.
    cameraState_.yawRadians -= deltaX * sensitivity;
    cameraState_.pitchRadians = std::clamp(cameraState_.pitchRadians - deltaY * sensitivity,
                                           -Pi * 0.495, Pi * 0.495);
    invalidateBaseFrame();
    reportCameraStatus();
}

void MapViewWidget::updateCapturedMouse(QMouseEvent* event)
{
    if (waylandPointerCapture_) return;
    const bool wayland = QGuiApplication::platformName().contains(QStringLiteral("wayland"),
                                                                  Qt::CaseInsensitive);
    QPointF delta;
    if (wayland) {
        const QPointF global = event->globalPosition();
        delta = global - lastCapturedGlobalPosition_;
        lastCapturedGlobalPosition_ = global;
    } else {
        const QPointF center = rect().center();
        if (ignoreWarpEvent_ && QLineF(event->position(), center).length() < 1.5) {
            ignoreWarpEvent_ = false;
            return;
        }
        delta = event->position() - center;
    }

    applyCapturedMouseDelta(delta.x(), delta.y());

    if (!wayland) {
        const QPoint center = mapToGlobal(rect().center());
        ignoreWarpEvent_ = true;
        lastCapturedGlobalPosition_ = center;
        QCursor::setPos(center);
    }
}

void MapViewWidget::updateFlyMovement()
{
    if (!mouseCaptured_ || kind_ != Kind::Perspective) return;
    const qint64 elapsedMilliseconds = flyElapsed_.restart();
    const double seconds = std::clamp(static_cast<double>(elapsedMilliseconds) / 1000.0,
                                      0.0, 0.1);
    if (seconds <= 0.0) return;

    hammer::vmf::Vec3 direction{};
    const hammer::vmf::Vec3 forward = hammer::camera::forwardVector(cameraState_);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(cameraState_);
    if (pressedKeys_.contains(Qt::Key_W)) direction = add(direction, forward);
    if (pressedKeys_.contains(Qt::Key_S)) direction = add(direction, scale(forward, -1.0));
    if (pressedKeys_.contains(Qt::Key_D)) direction = add(direction, right);
    if (pressedKeys_.contains(Qt::Key_A)) direction = add(direction, scale(right, -1.0));
    if (length(direction) < 1e-9) return;

    direction = normalized(direction);
    cameraState_.position = add(cameraState_.position, scale(direction, flySpeed_ * seconds));
    invalidateBaseFrame();
    reportCameraStatus();
}

void MapViewWidget::reportCameraStatus()
{
    if (kind_ != Kind::Perspective) return;
    // High-rate relative-pointer events can arrive far faster than the display
    // refresh rate. Updating the status panes for every event needlessly
    // relayouts the main window, so cap status text updates at about 30 Hz.
    if (cameraStatusElapsed_.isValid() && cameraStatusElapsed_.elapsed() < 33) return;
    cameraStatusElapsed_.restart();
    emit cursorPositionChanged(
        QStringLiteral("camera %1  pos %2 %3 %4  speed %5")
            .arg(projectionMode_ == ProjectionMode::Perspective
                     ? QStringLiteral("perspective") : QStringLiteral("orthographic"))
            .arg(QString::number(cameraState_.position.x, 'f', 0))
            .arg(QString::number(cameraState_.position.y, 'f', 0))
            .arg(QString::number(cameraState_.position.z, 'f', 0))
            .arg(QString::number(flySpeed_, 'f', 0)));
}

void MapViewWidget::requestRepaint(bool rerenderHardware)
{
#ifdef HAMMER_HAVE_VULKAN_RAY_TRACING
    if (rayTracedViewport_ && rayTracedViewport_->isVisible()) {
        rayTracedViewport_->requestUpdate(rerenderHardware);
        return;
    }
#endif
    if (hardwareViewport_) {
        hardwareViewport_->requestUpdate(rerenderHardware);
        return;
    }
    QWidget::update();
}

void MapViewWidget::paintHardwareOverlay(QPainter& painter, bool includeSceneOverlay)
{
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (kind_ == Kind::Perspective) {
        if (includeSceneOverlay) {
            // FGD model and sprite helpers are rendered by the hardware pass.
            // The advanced material-preview mode is not a wireframe mode. Keep
            // the old QPainter outline pass out of it even when Overlay 3D
            // Wireframe was enabled previously; that pass obscures normal maps.
            const bool allowOutlineOverlay =
                texturedRenderMode_ != TexturedRenderMode::ShadedMaterialPolygons &&
                texturedRenderMode_ != TexturedRenderMode::RayTracedPreview;
            if ((wireframeOverlayEnabled_ && allowOutlineOverlay) ||
                !materialRenderingEnabled_) {
                painter.save();
                drawBrushWireframe(painter);
                painter.restore();
            }
        }

        painter.save();
        drawFaceSelectionOverlay(painter);
        painter.restore();

        painter.save();
        drawSelectionOverlay(painter);
        painter.restore();

        painter.save();
        drawCreationPreview(painter);
        painter.restore();

        painter.save();
        drawZCameraCrosshair(painter);
        painter.restore();
    }

    // The in-progress Box3D selection rectangle has no GL-renderer equivalent
    // either, and applies to both the 2D and 3D views.
    painter.save();
    drawBoxSelectOverlay(painter);
    painter.restore();

    // The Camera tool's eye/look-at handles have no GL-renderer equivalent,
    // so they're drawn here via QPainter for orthographic views too.
    painter.save();
    drawPortalFileOverlay(painter);
    painter.restore();

    painter.save();
    drawPointFileOverlay(painter);
    painter.restore();

    painter.save();
    drawCollabPeersOverlay(painter);
    painter.restore();

    painter.save();
    drawCameraOverlay(painter);
    painter.restore();

    painter.save();
    drawClipOverlay(painter);
    painter.restore();

    // The Morph tool's vertex/edge handles and mesh preview are also QPainter
    // overlays; without this call the vertex tool is invisible (and therefore
    // unusable) whenever the hardware renderer is active.
    painter.save();
    drawMorphOverlay(painter);
    painter.restore();

    // Text and the active-view frame are UI chrome only. Orthographic grids,
    // brushes, entities, selection geometry, handles, and creation previews are
    // all emitted by Hardware3DViewport's OpenGL path.
    painter.save();
    drawViewLabel(painter);
    painter.restore();

    painter.setPen(QPen(active_ ? QColor(255, 255, 0) : QColor(112, 112, 112), active_ ? 2 : 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRect(0, 0, std::max(0, width() - 1), std::max(0, height() - 1)));
}

void MapViewWidget::drawZCameraCrosshair(QPainter& painter)
{
    if (kind_ != Kind::Perspective || !mouseCaptured_) return;

    const QPoint center = rect().center();
    constexpr int Gap = 4;
    constexpr int Arm = 12;
    const QLine lines[] = {
        {center.x() - Arm, center.y(), center.x() - Gap, center.y()},
        {center.x() + Gap, center.y(), center.x() + Arm, center.y()},
        {center.x(), center.y() - Arm, center.x(), center.y() - Gap},
        {center.x(), center.y() + Gap, center.x(), center.y() + Arm}
    };

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 220), 3, Qt::SolidLine, Qt::SquareCap));
    for (const QLine& line : lines) painter.drawLine(line);
    painter.setPen(QPen(QColor(245, 245, 245, 245), 1, Qt::SolidLine, Qt::SquareCap));
    for (const QLine& line : lines) painter.drawLine(line);
    painter.setPen(QColor(0, 0, 0, 230));
    painter.drawEllipse(center, 2, 2);
    painter.setPen(QColor(255, 255, 255, 255));
    painter.drawPoint(center);
}

void MapViewWidget::resetInteraction()
{
    const bool hadDocumentTransaction = objectDragging_ || transforming_;
    stopAutoScroll();
    panning_ = false;
    objectDragPending_ = false;
    objectDragging_ = false;
    translateAxis_ = -1;
    cycleModeOnRelease_ = false;
    creationDragging_ = false;
    creationHandleDragging_ = false;
    pendingMoveDragging_ = false;
    creationMoveU_ = -1;
    creationMoveV_ = -1;
    // The pending box is shared, so dropping it here must tell the other views.
    clearPendingBlock();
    transforming_ = false;
    boxSelectPending_ = false;
    boxSelecting_ = false;
    cameraDragging_ = false;
    clipDragging_ = false;
    morphGizmoAxis_ = -1;
    morphGizmoApplied_ = 0.0;
    perspectiveRotating_ = false;
    perspectiveStrafing_ = false;
    activeHandle_ = Handle::None;
    lastObjectDragDelta_ = {};
    if (hadDocumentTransaction) emit interactionCanceled();
    requestRepaint();
}
