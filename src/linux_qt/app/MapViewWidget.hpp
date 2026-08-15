#pragma once

#include "Camera3D.hpp"
#include "MaterialSystem.hpp"
#include "StudioModelSystem.hpp"
#include "VmfEditor.hpp"
#include "VmfScene.hpp"
#include "CollabSession.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QList>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QWidget>
#include <array>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QLineF;
class QPainter;
class QTimer;
class QWheelEvent;
class Hardware3DViewport;
class VulkanRayTracedViewport;
class WaylandPointerLock;

class MapViewWidget final : public QWidget
{
    Q_OBJECT
    friend class Hardware3DViewport;
    friend class VulkanRayTracedViewport;
    friend class WaylandPointerLock;
    friend class FreemanWindow;
public:
    enum class Kind { Perspective, Top, Front, Side };
    enum class SelectionMode { Groups, Objects, Solids };
    enum class Tool { Selection, Block, Entity, Decal, Overlay, Magnify, Camera, Clipper, Morph,
                      TextureApplication };
    enum class TransformMode { Scale, Translate, Rotate };
    enum class TexturedRenderMode { Unlit, Shaded, ShadedMaterialPolygons, RayTracedPreview };
    using ProjectionMode = hammer::camera::ProjectionMode;

    explicit MapViewWidget(Kind kind, QWidget* parent = nullptr);
    ~MapViewWidget() override;

    Kind kind() const { return kind_; }
    // Hammer lets any viewport become any type (F2/F3/F4 for the 2D axes, the
    // 3D render-mode commands for the camera), so the four panes are not fixed
    // to the roles they were created with. Retyping keeps the pane's scene and
    // selection; only the camera state is reset when it becomes a 3D view.
    void setKind(Kind kind);
    // Block tool: the unconfirmed brush, shared by every 2D view so the two
    // dimensions this view cannot draw are still visible (and resizable) in the
    // others. An invalid Bounds clears it. extrusionAxis belongs to the box, not
    // to the view showing it: it is fixed by whichever view drew the box first.
    void setPendingBlock(const hammer::vmf::Bounds& bounds, int extrusionAxis);
    const hammer::vmf::Bounds& pendingBlock() const { return pendingBlock_; }
    int pendingBlockExtrusionAxis() const { return pendingExtrusionAxis_; }

    // Brings a world point to the middle of the view without changing the zoom.
    // The 3D view keeps its heading and backs off far enough to hold something
    // of the given radius in frame.
    void centerOnWorldPoint(const hammer::vmf::Vec3& point, double radius = 0.0);
    // World point under the middle of this 2D view. Only the two axes the view
    // draws are meaningful; the third comes back as 0. Empty for the 3D view.
    std::optional<hammer::vmf::Vec3> viewCenterWorld() const;
    QString title() const;
    void setActive(bool active);
    void setGridVisible(bool visible);
    // Preview panes (the Skybox Browser) turn the corner label off: there is
    // no map to report on and it only covers the picture.
    void setViewLabelVisible(bool visible);
    // Perspective vertical field of view, in degrees. Widening it is how a
    // preview zooms out without moving the camera.
    void setVerticalFieldOfView(double degrees);
    // Freezes the hardware renderer for this view (the last frame keeps
    // showing). Used while the Freeman window owns the GPU.
    void setRenderingPaused(bool paused);
    bool renderingPaused() const { return renderingPaused_; }
    // ID_MAP_SNAPTOGRID (m_bSnapToGrid) and ID_MAP_GRIDLOWER/GRIDHIGHER: the
    // snap toggle and the grid spacing shared by every editing interaction.
    void setGridSnapEnabled(bool enabled);
    bool gridSnapEnabled() const { return gridSnapEnabled_; }
    void setGridSpacing(int spacing);
    int gridSpacing() const { return gridSpacing_; }
    void setScene(std::shared_ptr<const hammer::vmf::Scene> scene, bool fit = true);
    void setMaterialSystem(std::shared_ptr<hammer::assets::MaterialSystem> materials);
    // Collaborators' cameras, drawn as an overlay (models/editor/camera.mdl
    // wireframe + name tag in 3D, marker + name in 2D). Presence only: never
    // part of the scene, never triggers a base-frame rebuild.
    void setCollabPeers(QList<CollabPeerPose> peers);
    // The live camera state, for publishing this editor's own pose.
    const hammer::camera::State& cameraState() const { return cameraState_; }
    void setMaterialRenderingEnabled(bool enabled);
    void setWireframeOverlayEnabled(bool enabled);
    // CMapDoc::IsDispSolidDrawMask - hides the non-displaced sides of a
    // displacement solid in the 3D views. Defaults on, as in Hammer.
    void setDisplacementSolidMaskEnabled(bool enabled);
    bool displacementSolidMaskEnabled() const { return displacementSolidMaskEnabled_; }
    void setTexturedRenderMode(TexturedRenderMode mode);
    // Source's HDR display path: auto-exposure, bloom and the HDR-only light
    // entity keys. Off renders the map as a non-HDR compile would light it.
    void setHdrEnabled(bool enabled);
    // View > Show detail objects.
    void setDetailPropsVisible(bool visible);
    bool detailPropsVisible() const { return detailPropsVisible_; }
    // mat_monitorgamma for the ray-traced view: the exponent composite.comp
    // converts the linear frame with. 2.2 is Source's default.
    void setRayTracedGamma(float gamma);
    void setMaterialEffectsEnabled(bool phong, bool specular, bool bumpMaps,
                                   bool lightWarp, bool selfIllum, bool rimLight);
    void setMaterialEffectIntensities(float phong, float specular, float bumpMaps);
    void setHiddenToolTextures(const std::unordered_set<std::string>& hiddenToolTextures);
    void setSelection(const std::vector<hammer::vmf::ObjectRef>& selection,
                      const hammer::vmf::Bounds& bounds = {});
    // Camera3D tool state, mirrored from the document into every 2D view so
    // each one can render the eye/look-at handles and hit-test drags.
    void setCameras(const std::vector<hammer::vmf::CameraDef>& cameras, int activeIndex);
    // Points the perspective view's live camera at an eye/look-at pair, as
    // Camera3D::UpdateActiveCamera does for CMapView3D while dragging.
    void setCameraTransform(const hammer::vmf::Vec3& eye, const hammer::vmf::Vec3& lookAt);
    // Clipper3D state, owned by the document and mirrored into every 2D view
    // so the clip line, its handles, and the clipped-solid preview appear in
    // all of them (Clipper3D::RenderTool2D via MAPVIEW_UPDATE_TOOL).
    using ClipMode = hammer::vmf::EditorModel::ClipMode;
    void setClipState(bool active, const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                      const hammer::vmf::Vec3& axis, ClipMode mode,
                      const std::vector<hammer::vmf::FacePolygons>& kept,
                      const std::vector<hammer::vmf::FacePolygons>& discarded);
    // Morph3D (vertex manipulation) state. The document owns the vertex
    // meshes and the handle selection, exactly as the single Morph3D instance
    // owned by CToolManager does; every 2D view just draws the handles and
    // reports what the mouse did to them (Morph3D::RenderTool2D).
    void setMorphState(bool active, const std::vector<hammer::vmf::MorphHandle>& handles,
                       const std::vector<hammer::vmf::FacePolygons>& preview,
                       const std::vector<hammer::vmf::MorphDispGrid>& dispGrids = {});
    // Texture Application tool (hammer/ToolMaterial.cpp). The face list lives
    // on the document, exactly as CFaceEditSheet::m_Faces does; the perspective
    // view only tints the listed faces and reports what was clicked.
    // Map > Load Pointfile (hammer/mapdoc.cpp CMapDoc::m_PFPoints): the leak
    // trace, drawn as a thick red polyline over every view.
    void setPointFile(std::vector<hammer::vmf::Vec3> points);
    // Map > Load Portal File: the .prt vbsp hands to vvis, so the visleaf
    // portals can be inspected in Hammer instead of Glview.
    void setPortalFile(std::vector<std::vector<hammer::vmf::Vec3>> portals);

    void setFaceSelection(const std::vector<hammer::vmf::FaceRef>& faces);
    // IDC_HIDEMASK / CMapFace::SetShowSelection: hides the red selection tint
    // so the material can be judged without it.
    void setFaceSelectionMaskHidden(bool hidden);
    // True while the Face Edit sheet's Displacement page has a paint tool
    // active, which is what makes the 3D view paint instead of pick faces.
    void setDisplacementPaintActive(bool active);
    // Faces of the smoothing group the face edit sheet is showing; tinted in a
    // second colour so group membership is visible while the page is up.
    void setSmoothingGroupFaces(const std::vector<hammer::vmf::FaceRef>& faces);
    // ID_VIEW_3DLIGHTMAP_GRID (VIEW3D_LIGHTMAP_GRID): draw the luxel grid of
    // every visible face in the perspective view.
    void setLightmapGridVisible(bool visible);

    // Ray-traces one cubemap per env_cubemap entity and hands the result to the
    // OpenGL viewport, which then reflects them instead of the sky. Returns
    // false with a reason in `error` when nothing could be baked.
    bool buildCubemaps(QString& error);
    bool canBuildCubemaps() const;
    // CCamera accessors used by the Align To View click mode
    // (CFaceEditMaterialPage::AlignToView).
    hammer::vmf::Vec3 viewRight() const;
    hammer::vmf::Vec3 viewUp() const;
    hammer::vmf::Vec3 viewPoint() const;
    void setSelectionMode(SelectionMode mode);
    void setTool(Tool tool);
    void setTransformMode(TransformMode mode);
    Tool tool() const { return tool_; }
    TransformMode transformMode() const { return transformMode_; }
    ProjectionMode projectionMode() const { return projectionMode_; }
    bool mouseCaptured() const { return mouseCaptured_; }
    double flySpeed() const { return flySpeed_; }
    void setProjectionMode(ProjectionMode mode);
    void resetView();
    void fitScene();

signals:
    void activated(MapViewWidget* view);
    void cursorPositionChanged(const QString& coordinates);
    void selectionRequested(const hammer::vmf::ObjectRef& object, bool toggle, bool additive);
    void clearSelectionRequested();
    // CMapView::SelectAt's hit list, published so the document can drive
    // CSelection::SetCurrentHit (Edit > Select Next/Previous Object and the 3D
    // view's 500 ms pick timer, CMapView3D::BeginPick).
    void hitListChanged(const std::vector<hammer::vmf::ObjectRef>& hits);
    // Selection3D::SelectInBox on mouse up. additive mirrors the Shift flag
    // OnLMouseUp2D/OnLMouseUp3D pass to SelectInBox.
    void boxSelectionRequested(const std::vector<hammer::vmf::ObjectRef>& objects, bool additive);
    void selectNextHitRequested();
    void moveStarted();
    void moveDeltaRequested(const hammer::vmf::Vec3& delta);
    void moveFinished();
    void nudgeRequested(const hammer::vmf::Vec3& delta);
    // Shift+arrow: duplicate the selection and move the copy by delta.
    void nudgeDuplicateRequested(const hammer::vmf::Vec3& delta);
    void resizeStarted();
    void resizeDeltaRequested(const hammer::vmf::Vec3& factors, const hammer::vmf::Vec3& pivot);
    void rotateStarted();
    void rotateDeltaRequested(double radians, hammer::vmf::RotationAxis axis,
                              const hammer::vmf::Vec3& pivot);
    void transformFinished();
    // Clicking an already-selected object in a 2D view cycles the transform
    // handles (Scale -> Translate -> Rotate); the document owns the mode and
    // pushes it back to every view.
    void transformModeChangeRequested(TransformMode mode);
    void interactionCanceled();
    // extrusionAxis is the axis this 2D view cannot show (0=x, 1=y, 2=z), the
    // one primitives are extruded along.
    void blockCreationRequested(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                                int extrusionAxis);
    // The pending (uncommitted) brush changed in this view, so the other views
    // can show and edit the same box. An invalid Bounds means it is gone.
    void blockPreviewChanged(const hammer::vmf::Bounds& bounds, int extrusionAxis);
    void entityCreationRequested(const hammer::vmf::Vec3& origin);
    // CToolEntity::OnLMouseDown3D: places the entity on the solid surface
    // under the cursor, offset along the hit normal by the class's bounds.
    void entityPlacementRequested(const hammer::vmf::Vec3& position, const hammer::vmf::Vec3& normal);
    void decalPlacementRequested(const hammer::vmf::Vec3& position,
                                 const hammer::vmf::Vec3& normal, int sideId);
    void overlayPlacementRequested(const hammer::vmf::Vec3& position,
                                   const hammer::vmf::Vec3& normal, int sideId);
    void objectPropertiesRequested(const hammer::vmf::ObjectRef& object);
    // Camera3D tool signals. Emitted live during a handle drag (and once more
    // on release) so the document can mirror the change to the other 2D
    // views and to the perspective view's live camera.
    void cameraEdited(int index, const hammer::vmf::Vec3& eye, const hammer::vmf::Vec3& lookAt, bool created);
    void cameraCycleRequested(bool forward);
    void cameraDeleteRequested();
    // Clipping tool signals. The view owns the clip line's screen interaction;
    // the document owns the plane, the mode, and the clip results.
    void clipLineChanged(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                         const hammer::vmf::Vec3& viewAxis);
    void clipApplyRequested();
    void clipCancelRequested();
    void selectionToolRequested();
    // Morph tool signals.
    void morphSelectRequested(const QList<int>& handles, bool toggle);
    void morphSelectionClearRequested();
    void morphMoveRequested(const hammer::vmf::Vec3& delta);
    void morphMoveFinished();
    void morphEscapePressed();
    // Texture Application tool signals. CToolMaterial::OnLMouseDown3D selects
    // the clicked face; OnRMouseDown3D applies the sheet's material to it.
    void faceSelectRequested(int solidId, int sideId, bool control, bool shift);
    void faceApplyRequested(int solidId, int sideId, bool edgeAlign, bool shift);
    // Displacement page's Paint Geometry / Paint Alpha tools
    // (CToolDisplace::OnLMouseDown3D / OnMouseMove3D / OnLMouseUp3D). The
    // position is the ray hit on the displacement surface, the normal is the
    // traced hit normal DISPPAINT_AXIS_FACE paints along, and "lower" is the
    // original's m_bRMBDown scalar flip.
    void displacementPaintBegin(const hammer::vmf::Vec3& position,
                                const hammer::vmf::Vec3& normal, bool lower);
    void displacementPaintMoved(const hammer::vmf::Vec3& position, bool lower);
    void displacementPaintFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Handle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

    void drawOrthographic(QPainter& painter);
    void drawPerspective(QPainter& painter);
    void drawSceneBase(QPainter& painter);
    void drawBrushWireframe(QPainter& painter);
    void drawEntityMarkers(QPainter& painter);
    void drawSelectionOverlay(QPainter& painter);
    void drawBoxSelectOverlay(QPainter& painter);
    void drawCreationPreview(QPainter& painter);
    void drawCameraOverlay(QPainter& painter);
    void drawCollabPeersOverlay(QPainter& painter);
    struct CameraHit { int index; int part; };
    void drawClipOverlay(QPainter& painter);
    void drawMorphOverlay(QPainter& painter);
    void drawFaceSelectionOverlay(QPainter& painter);
    void drawFaceTintOverlay(QPainter& painter, const std::vector<hammer::vmf::FaceRef>& faces,
                             const QColor& tint);
    int morphHandleHitTest(const QPointF& screenPoint) const;
    // The 3D mover gizmo: XYZ arrows at the selected handles' centroid in the
    // perspective view. Returns the axis (0/1/2) under the point, or -1.
    int morphGizmoHitTest(const QPointF& screenPoint) const;
    std::optional<hammer::vmf::Vec3> morphSelectionCentroid() const;
    double morphGizmoWorldLength(const hammer::vmf::Vec3& origin) const;
    // Where the mover gizmo sits: the morph handle centroid in the vertex
    // tool, the selection bounds center in the selection tool.
    std::optional<hammer::vmf::Vec3> gizmoOrigin() const;
    QList<int> morphHandlesInRect(const QRectF& rect) const;
    int clipHandleHitTest(const QPointF& screenPoint) const;
    hammer::vmf::Vec3 viewAxis() const;
    std::optional<CameraHit> cameraHitTest(const QPointF& screenPoint) const;
    void drawViewLabel(QPainter& painter);
    void drawZCameraCrosshair(QPainter& painter);
    void drawPointFileOverlay(QPainter& painter);
    void drawPortalFileOverlay(QPainter& painter);
    void updateCoordinateText(const QPoint& point);
    QPointF project(const hammer::vmf::Vec3& point) const;
    std::optional<QPointF> projectCameraPoint(const hammer::vmf::Vec3& point) const;
    bool projectCameraLine(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                           QLineF& line) const;
    QPointF toScreen(const hammer::vmf::Vec3& point) const;
    QPointF screenToPlane(const QPointF& point) const;
    hammer::vmf::Vec3 planeDeltaToWorld(const QPointF& delta) const;
    hammer::vmf::Vec3 planePointToWorld(const QPointF& point, double missingAxis = 0.0) const;
    struct SurfaceHit
    {
        hammer::vmf::Vec3 position;
        hammer::vmf::Vec3 normal;
        int sideId{-1};
        int solidId{-1};
        // True when the ray hit the displacement surface itself rather than the
        // flat brush face, which is what CToolDisplace's paint path requires
        // (ApplySpatialPaintTool bails on GetTexelHitIndex() == -1).
        bool displacement{false};
    };
    // distance, when non-null, receives the ray parameter of the returned hit so
    // callers can depth-sort brush surfaces against entity helpers.
    std::optional<SurfaceHit> surfaceHit(const QPointF& point, double* distance = nullptr) const;
    // Builds the 3D-view pick ray for a client point, exactly as surfaceHit and
    // modelHitDistance do. Only meaningful for Kind::Perspective.
    void buildPickRay(const QPointF& point, hammer::vmf::Vec3& origin,
                      hammer::vmf::Vec3& direction) const;
    // Ray distance to the camera-facing sprite quad drawn for this entity, at
    // exactly the size Hardware3DViewport::drawBillboardSprite uses.
    std::optional<double> billboardHitDistance(const hammer::vmf::EntityMarker& entity,
                                               const QPointF& point) const;
    // Ray distance to the solid fallback cube drawn for point entities with no
    // model and no sprite helper (Hardware3DViewport::drawEntityCube).
    std::optional<double> boxHitDistance(const hammer::vmf::EntityMarker& entity,
                                         const QPointF& point) const;
    std::optional<hammer::vmf::ObjectRef> hitTest(const QPointF& point) const;
    // Full ordered candidate list under the cursor, nearest/topmost first. This
    // is CMapView::SelectAt's ObjectsAt + CSelection::AddHit list, which drives
    // Select Next/Previous Object (CSelection::SetCurrentHit).
    std::vector<hammer::vmf::ObjectRef> hitTestAll(const QPointF& point) const;
    // True when this object must be ignored for drawing and picking in this
    // view because every face uses a hidden tool material.
    bool brushHiddenByToolTextures(const hammer::vmf::BrushGeometry& brush) const;
    std::vector<hammer::vmf::ObjectRef> objectsInBox(const QRectF& box) const;
    std::optional<QRectF> entityScreenBounds(const hammer::vmf::EntityMarker& entity) const;
    std::optional<QRectF> spriteScreenBounds(const hammer::vmf::EntityMarker& entity) const;
    std::optional<double> projectedSurfaceHitDistance(const hammer::vmf::EntityMarker& entity,
                                                       const QPointF& point) const;
    std::optional<double> modelHitDistance(const hammer::vmf::EntityMarker& entity,
                                           const QPointF& point) const;
    bool isSelected(const hammer::vmf::ObjectRef& object) const;
    hammer::vmf::ObjectRef effectiveObject(const hammer::vmf::BrushGeometry& brush) const;
    bool entitySelectableInMode(const hammer::vmf::EntityMarker& entity) const;
    QRectF selectionScreenBounds() const;
    Handle hitHandle(const QPointF& point) const;
    // The four move handles and where they sit relative to the selection box.
    static std::array<std::pair<Handle, QPointF>, 4> translateHandlePositions(const QRectF& bounds);
    bool isSinglePointEntitySelection() const;
    // Rotation pivot: average of the selected vertices/origins. Unlike the
    // AABB center it is invariant under rotation, so repeated rotations spin
    // the selection in place instead of walking it.
    hammer::vmf::Vec3 selectionCentroidWorld() const;
    // A single point entity has no meaningful Scale state, so the shared
    // document mode Scale renders (and hit-tests) as Translate for it.
    TransformMode effectiveTransformMode() const;
    TransformMode nextTransformMode() const;
    Handle creationHandleHitTest(const QPointF& point) const;
    // The world axis this 2D view cannot show (0=x, 1=y, 2=z), and the two it
    // can, in plane (u, v) order.
    int missingWorldAxis() const;
    void planeWorldAxes(int& uAxis, int& vAxis) const;
    // Builds the pending box from a drag in this view's plane, taking the
    // depth along the missing axis from the last selection.
    hammer::vmf::Bounds pendingBoundsFromPlane(const QPointF& first, const QPointF& second) const;
    void clearPendingBlock();
    // The pending box in this view's plane coordinates, for the GPU 2D
    // renderer which draws it from plane space. False when there is none.
    bool pendingBlockPlaneRect(QPointF& first, QPointF& second) const;
    QPointF planeToScreen(const QPointF& plane) const;
    void confirmBlockCreation();
    QPointF handlePlanePoint(Handle handle) const;
    QPointF oppositeHandlePlanePoint(Handle handle) const;
    hammer::vmf::Vec3 planeFactorsToWorld(const QPointF& factors) const;
    hammer::vmf::RotationAxis rotationAxis() const;
    void applyPendingFit();
    void invalidateBaseFrame();
    // The fly/animation timers, the pointer lock and the ray-traced viewport
    // only exist for a 3D view; a pane that is retyped into one builds them on
    // demand instead of at construction.
    void ensurePerspectiveResources();
    // Picks the hardware or the ray-traced surface for the current kind and
    // render mode. Retyping a pane changes the answer.
    void updateViewportSurface();
    // 2D views: change the zoom while keeping the world point under "anchor"
    // fixed on screen (mouse-wheel and Magnify-tool zooming).
    void zoomAboutAnchor(double zoom, const QPointF& anchor);
    void rebuildBaseFrame();
    void resetInteraction();
    void resetCamera();
    void fitCameraToScene();
    void toggleMouseCapture();
    void setMouseCaptured(bool captured);
    void updateCapturedMouse(QMouseEvent* event);
    void applyCapturedMouseDelta(double deltaX, double deltaY);
    void applyCameraLookDelta(double deltaX, double deltaY);
    void beginFallbackMouseCapture();
    void centerCapturedPointer();
    void handleWaylandPointerLockUnavailable();
    void handleWaylandPointerLockLost();
    void updateFlyMovement();
    void reportCameraStatus();
    void requestRepaint(bool rerenderHardware = true);
    double snap(double value) const;
    QPointF snapped(const QPointF& point) const;
    // 2D drag auto-scroll: while a drag sits near/over a view edge, pan the
    // view in that direction and keep applying the drag at the frozen cursor.
    void updateAutoScroll(const QPointF& cursor);
    void stopAutoScroll();
    void autoScrollTick();
    void applyObjectDragUpdate(const QPointF& position);
    void paintHardwareOverlay(QPainter& painter, bool includeSceneOverlay = true);
    bool materialVisible(std::string_view material) const;

    Kind kind_;
    QPoint pan_{};
    QPoint dragOrigin_{};
    QPointF objectDragStart_{};
    QPointF objectDragPressScreen_{};
    QPointF creationStartPlane_{};
    QPointF creationCurrentPlane_{};
    QPointF transformAnchorPlane_{};
    QPointF transformHandlePlane_{};
    QPointF transformCenterPlane_{};
    hammer::vmf::Vec3 transformPivotWorld_{};
    hammer::vmf::Vec3 lastObjectDragDelta_{};
    QPointF lastScaleFactors_{1.0, 1.0};
    double lastRotationRadians_{0.0};
    double transformStartAngle_{0.0};
    double zoom_{1.0};
    bool active_{false};
    bool gridVisible_{true};
    bool viewLabelVisible_{true};
    std::vector<hammer::vmf::Vec3> pointFile_;
    std::vector<std::vector<hammer::vmf::Vec3>> portalFile_;
    bool renderingPaused_{false};
    bool gridSnapEnabled_{true};
    int gridSpacing_{16};
    bool panning_{false};
    bool objectDragPending_{false};
    bool objectDragging_{false};
    // Selection3D box selection (Box3D). A left drag that starts on empty space
    // (or with Ctrl held) rubber-bands a selection box; releasing selects the
    // objects wholly inside it, as Options.view2d.bAutoSelect does in the
    // original.
    bool boxSelecting_{false};
    bool boxSelectPending_{false};
    QPointF boxSelectStart_{};
    QPointF boxSelectCurrent_{};
    // CMapView3D::BeginPick / EndPick: while the button stays down on a 3D
    // pick, a 500 ms timer walks the hit list (CSelection::SetCurrentHit).
    QTimer* pickNextTimer_{nullptr};
    QTimer* autoScrollTimer_{nullptr};
    QPointF autoScrollCursor_{};
    bool fitPending_{false};
    // Block tool: the pending box lives in world space so every 2D view draws
    // its own two dimensions of it. Valid means a preview exists (being dragged
    // or waiting for Enter); it is only committed when Enter confirms it.
    hammer::vmf::Bounds pendingBlock_{};
    int pendingExtrusionAxis_{-1};
    bool creationDragging_{false};
    bool creationHandleDragging_{false};
    // Dragging the pending box's body moves it, the way dragging a selected
    // object does.
    bool pendingMoveDragging_{false};
    QPointF pendingMovePressPlane_{};
    hammer::vmf::Bounds pendingMoveStartBlock_{};
    int creationMoveU_{-1};  // 0 = dragging the min edge, 1 = max, -1 = none
    int creationMoveV_{-1};
    // Bounds of the last non-empty selection; a new brush takes its extent
    // along the axis this view cannot show from here.
    hammer::vmf::Bounds lastReferenceBounds_{};
    bool transforming_{false};
    Handle activeHandle_{Handle::None};
    // Translate-handle drag: constrain the object drag to one plane axis
    // (0 = horizontal/U only, 1 = vertical/V only, -1 = unconstrained).
    int translateAxis_{-1};
    // Anchor for grid snapping during a move drag: the selection's bounds
    // minimum (origin center for a point entity) at press time. Snapping this
    // point instead of the bare delta pulls off-grid objects onto the grid.
    hammer::vmf::Vec3 objectDragSnapRef_{};
    bool objectDragSnapRefValid_{false};
    // False until the cursor actually leaves the press point of a scale or
    // rotate handle drag; a clean handle click cycles the transform mode.
    bool transformMoved_{false};
    // Set when the press grabbed an already-selected object; a release without
    // a drag then cycles the transform handles.
    bool cycleModeOnRelease_{false};
    QImage baseFrame_;
    Hardware3DViewport* hardwareViewport_{nullptr};
    VulkanRayTracedViewport* rayTracedViewport_{nullptr};
    hammer::camera::State cameraState_{};
    ProjectionMode projectionMode_{ProjectionMode::Perspective};
    double flySpeed_{512.0};
    bool mouseCaptured_{false};
    bool waylandPointerCapture_{false};
    std::unique_ptr<WaylandPointerLock> waylandPointerLock_;
    bool ignoreWarpEvent_{false};
    QPointF lastCapturedGlobalPosition_{};
    QSet<int> pressedKeys_;
    QTimer* flyTimer_{nullptr};
    QTimer* animationTimer_{nullptr};
    QElapsedTimer flyElapsed_;
    QElapsedTimer cameraStatusElapsed_;
    bool baseFrameDirty_{true};
    std::shared_ptr<const hammer::vmf::Scene> scene_;
    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    std::shared_ptr<hammer::assets::StudioModelSystem> studioModels_;
    QList<CollabPeerPose> collabPeers_;
    std::shared_ptr<const hammer::assets::StudioModel> collabCameraModel_;
    bool collabCameraModelTried_{false};
    bool materialRenderingEnabled_{true};
    bool wireframeOverlayEnabled_{false};
    bool hdrEnabled_{true};
    bool detailPropsVisible_{true};
    float rayTracedGamma_{2.2f};
    bool displacementSolidMaskEnabled_{true};
    TexturedRenderMode texturedRenderMode_{TexturedRenderMode::Unlit};
    bool phongEnabled_{true};
    bool specularEnabled_{true};
    bool bumpMapsEnabled_{true};
    bool lightWarpEnabled_{true};
    bool selfIllumEnabled_{true};
    bool rimLightEnabled_{true};
    float phongIntensity_{1.0f};
    float specularIntensity_{1.0f};
    float bumpMapIntensity_{1.0f};
    std::unordered_set<std::string> hiddenToolTextures_;
    std::vector<hammer::vmf::ObjectRef> selection_;
    hammer::vmf::Bounds selectionBounds_{};
    SelectionMode selectionMode_{SelectionMode::Groups};
    Tool tool_{Tool::Selection};
    TransformMode transformMode_{TransformMode::Scale};

    // Camera3D tool state (see ToolCamera.cpp / camera.h in the original).
    std::vector<hammer::vmf::CameraDef> cameras_;
    int activeCamera_{-1};
    bool cameraDragging_{false};
    bool cameraDragIsNew_{false};
    int cameraDragIndex_{-1};
    int cameraDragPart_{0};              // 0 = eye (MovePos), 1 = look-at (MoveLook)
    double cameraDragMissingAxis_{0.0};  // value held constant along the axis this view can't show
    hammer::vmf::Vec3 cameraDragOtherStart_{};
    hammer::vmf::Vec3 cameraDragMovedStart_{};
    // Camera3D::OnLMouseDown3D / OnRMouseDown3D map onto CMapView3D's
    // rotating/strafing modes (see CMapView3D::ControlCamera).
    bool perspectiveRotating_{false};
    bool perspectiveStrafing_{false};
    QPointF perspectiveDragLast_{};

    // Clipping tool state (see ToolClipper.cpp in the original).
    bool clipActive_{false};
    hammer::vmf::Vec3 clipPoints_[2]{};
    ClipMode clipMode_{ClipMode::Front};
    std::vector<hammer::vmf::FacePolygons> clipKept_;
    std::vector<hammer::vmf::FacePolygons> clipDiscarded_;
    bool clipDragging_{false};
    int clipPointHit_{-1};              // Clipper3D::m_ClipPointHit
    hammer::vmf::Vec3 clipDragStart_[2]{};
    double clipMissingAxis_{0.0};       // value held along the axis this view cannot show
    hammer::vmf::Vec3 clipViewAxis_{};  // Clipper3D::m_vPlaneNormal: view the clip line was drawn in

    // Morph tool state (see ToolMorph.cpp in the original).
    bool morphActive_{false};
    std::vector<hammer::vmf::MorphHandle> morphHandles_;
    std::vector<hammer::vmf::FacePolygons> morphPreview_;
    int morphDragHandle_{-1};           // Morph3D::m_DragHandle
    bool morphDragging_{false};         // moved far enough to be a drag, not a click
    bool morphDragCtrl_{false};         // Morph3D::m_bLButtonDownControlState
    QPointF morphPressScreen_{};
    hammer::vmf::Vec3 morphDragOrigin_{};   // Morph3D::m_OrigHandlePos
    hammer::vmf::Vec3 morphDragApplied_{};  // total delta already sent to the document
    double morphMissingAxis_{0.0};
    // Texture Application tool state (see ToolMaterial.cpp in the original).
    std::vector<hammer::vmf::FaceRef> faceSelection_;
    bool displacementPaintActive_{false};
    bool displacementPainting_{false};
    bool displacementPaintLower_{false};
    std::vector<hammer::vmf::FaceRef> smoothingGroupFaces_;
    bool lightmapGridVisible_{false};
    bool faceSelectionMaskHidden_{false};

    bool morphBoxSelecting_{false};     // Morph3D::m_bBoxSelecting
    QPointF morphBoxStart_{};
    QPointF morphBoxCurrent_{};

    // Perspective mover-gizmo drag state, shared between the vertex tool
    // (morph moves) and the selection tool (whole-selection moves).
    int morphGizmoAxis_{-1};
    QPointF morphGizmoPressScreen_{};
    double morphGizmoApplied_{0.0};
    hammer::vmf::Vec3 morphGizmoOrigin_{};
    bool gizmoSelectMode_{false};
    bool gizmoMoveStarted_{false};
    std::vector<hammer::vmf::MorphDispGrid> morphDispGrids_;
};
