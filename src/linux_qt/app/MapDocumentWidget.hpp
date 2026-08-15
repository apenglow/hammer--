#pragma once

#include "FaceEditSheet.hpp"
#include "FgdDatabase.hpp"
#include "MapViewWidget.hpp"
#include "VmfEditor.hpp"
#include "VmfGroups.hpp"
#include "VmfScene.hpp"
#include "VmfSync.hpp"
#include "StudioModelSystem.hpp"

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class QCloseEvent;
class QSplitter;

class MapDocumentWidget final : public QWidget
{
    Q_OBJECT

public:
    enum class SelectionMode { Groups, Objects, Solids };
    using ProjectionMode = MapViewWidget::ProjectionMode;

    explicit MapDocumentWidget(std::shared_ptr<hammer::fgd::Database> fgd = {}, QWidget* parent = nullptr);

    // progress, when given, is called with (percent, stage text) as the load
    // advances so the caller can drive a progress dialog.
    bool loadFromFile(const QString& path, QString* error = nullptr,
                      const std::function<void(int, const QString&)>& progress = {});
    bool save(QString* error = nullptr);
    bool saveAs(const QString& path, QString* error = nullptr);
    bool maybeSave(QWidget* dialogParent = nullptr);

    // Collaborative editing (CollabSession). The session diffs this document
    // after every committed edit, so it only needs read access here...
    const hammer::vmf::EditorModel& editorModel() const { return editor_; }
    // ...plus these three write paths: the id window this peer allocates
    // object ids from, the host's map on join, and a peer's edit.
    void setCollabIdRange(int base, int span);
    // Collaborators' cameras for the presence overlay; empty clears it.
    void setCollabPeerPoses(const QList<CollabPeerPose>& poses);
    void adoptCollabDocument(hammer::vmf::Document document);
    void applyRemoteDelta(const hammer::vmf::SyncDelta& delta);

    QString filePath() const { return filePath_; }
    QString displayName() const;
    QString mapSummary() const;
    QString objectCountSummary() const;
    QString selectionSummary() const;
    QString selectionSizeSummary() const;
    bool isModified() const { return editor_.document().dirty(); }
    bool canUndo() const { return editor_.canUndo(); }
    bool canRedo() const { return editor_.canRedo(); }
    // Edit > Undo/Redo Active. Turning it off discards this document's history
    // and stops it being kept until the option is switched back on.
    bool undoRedoActive() const { return editor_.undoEnabled(); }
    void setUndoRedoActive(bool active);
    std::size_t selectionCount() const { return editor_.selection().size(); }
    const std::vector<hammer::vmf::ObjectRef>& selection() const { return editor_.selection(); }
    QString undoText() const;
    QString redoText() const;

    MapViewWidget* activeView() const { return activeView_; }
    // The document's single 3D view, which owns the renderers a cubemap bake
    // needs. Null before the views are constructed.
    MapViewWidget* perspectiveView() const;
    // View > 2D X/Y, Y/Z, X/Z and the 3D render modes retype the highlighted
    // pane, exactly as Hammer's F2/F3/F4/F5 do.
    void setActiveViewKind(MapViewWidget::Kind kind);
    std::shared_ptr<const hammer::vmf::Scene> scene() const { return scene_; }
    // The parsed VMF document backing the scene (trigger entity keyvalues and
    // other data the Scene does not carry).
    const hammer::vmf::Document& vmfDocument() const { return editor_.document(); }
    // Origin of the single selected point entity, if that is what is selected.
    std::optional<hammer::vmf::Vec3> selectedPointEntityOrigin() const;
    void setGridVisible(bool visible);
    // Pauses the perspective view's hardware rendering while the Freeman
    // window is deployed.
    void setPerspectiveRenderingPaused(bool paused);
    void setGridSnapEnabled(bool enabled);
    void setGridSpacing(int spacing);
    // Block tool primitive shape, by the Object Bar's item name ("Block",
    // "Wedge", "Cylinder", "Spike"), and the side count for round shapes.
    void setPrimitiveKindByName(const QString& name);
    void setPrimitiveFaces(int faces);
    void autosizeViews();
    void maximizeActiveView();
    void setCameraProjection(ProjectionMode mode);
    ProjectionMode cameraProjection() const;

    void undo();
    void redo();
    void deleteSelection();
    // Tools > Tie to Entity (Ctrl+T): CMapDoc::OnEditToEntity, without the
    // add-to-existing-entity prompt. Opens Object Properties on success so the
    // class can be changed from the default.
    void tieSelectionToEntity();
    // Tools > Carve (Ctrl+Shift+C): subtract the selected solids from every
    // solid they overlap. The carvers stay selected and untouched.
    void carveSelection();
    void clearSelection();
    void selectAll();
    // Every visible object that is not currently selected. Used by "Hide
    // unselected objects", which is the hide flow applied to the inverse.
    void invertSelection();

    // --- QuickHide (VisGroups > QuickHide) --------------------------------
    //
    // Session-only visibility, exactly like Hammer's quickhide: the hidden
    // objects are dropped from the Scene the views are handed, so they are not
    // drawn and not pickable in any backend, but the Document is untouched and
    // nothing about the hide reaches the saved VMF or the undo stack.
    void quickHideSelected();
    void quickHideUnselected();
    void quickHideUnhideAll();
    std::size_t quickHiddenCount() const
    {
        return quickHiddenSolids_.size() + quickHiddenEntities_.size();
    }
    // Hands the quickhidden objects over to a real VisGroup: they join it, it
    // is left unchecked so it keeps them hidden, and quickhide forgets them
    // (Unhide QuickHide Objects no longer brings them back - the VisGroup's
    // checkbox does). Returns how many objects moved, or 0 when none were
    // hidden.
    std::size_t quickHideConvertToVisGroup(const QString& name);

    // --- Groups (hammer/mapgroup.cpp) --------------------------------------
    //
    // A group is never an ObjectRef; a pick on a member expands to the member
    // list of its outermost group, so every transform path is untouched. See
    // VmfGroups.hpp.
    bool groupSelection();
    bool ungroupSelection();
    void setIgnoreGroups(bool ignore);
    bool ignoreGroups() const { return ignoreGroups_; }
    // True when group expansion is in effect (Groups selection mode with
    // Ignore Groups off).
    bool groupsActive() const;
    // Whether Group would pull objects out of an existing group or entity, so
    // the caller can raise OnToolsGroup's warning first.
    bool selectionCrossesExistingGroups() const;
    // Outermost groups all of whose members are selected.
    std::vector<int> selectedGroups() const;
    std::vector<hammer::vmf::ObjectRef> expandSelectionToGroups(
        const std::vector<hammer::vmf::ObjectRef>& objects) const;

    // --- VisGroups (hammer/visgroup.cpp, filtercontrol.cpp) ----------------
    std::vector<hammer::vmf::VisGroupDef> visGroups() const;
    hammer::vmf::VisGroupState visGroupState(int visGroupId) const;
    std::size_t visGroupMemberCount(int visGroupId) const;
    void setVisGroupVisible(int visGroupId, bool visible);
    void setShowAllVisGroups(bool showAll);
    bool showAllVisGroups() const { return showAllVisGroups_; }
    // View > Move Selection to VisGroup (CMapDoc::VisGroups_CreateNamedVisGroup
    // via NewVisGroupDlg). Returns the new visgroup's id, or 0.
    int createVisGroupFromSelection(const QString& name, bool hide,
                                    bool removeFromOtherVisGroups);
    bool addSelectionToVisGroup(int visGroupId, bool hide, bool removeFromOtherVisGroups);
    int createEmptyVisGroup(const QString& name);
    bool renameVisGroup(int visGroupId, const QString& name);
    bool setVisGroupColor(int visGroupId, const QColor& color);
    bool deleteVisGroup(int visGroupId);
    bool moveVisGroup(int visGroupId, bool up);
    // Filter Control's Mark button: select the visgroup's visible members.
    // Accepts an auto-visgroup id (negative) as well as a user one.
    std::size_t markVisGroup(int visGroupId);

    // --- Auto VisGroups (CMapDoc::AddToAutoVisGroup) -----------------------
    //
    // Derived categories, rebuilt with the object index. They are never written
    // to the VMF and never appear in visGroups(), so the Edit dialog and the
    // Object Properties VisGroup tab exclude them for free.
    struct AutoVisGroupNode
    {
        hammer::vmf::AutoVisGroup id{hammer::vmf::AutoVisGroup::None};
        hammer::vmf::AutoVisGroup parent{hammer::vmf::AutoVisGroup::None};
        QString name;
        std::size_t memberCount{0};
    };
    // Only the categories this map actually populates, parents before children.
    std::vector<AutoVisGroupNode> autoVisGroups() const;
    hammer::vmf::VisGroupState autoVisGroupState(hammer::vmf::AutoVisGroup id) const;
    // Refreshes autoVisGroupIndex_ if a remote delta left it stale.
    void ensureAutoVisGroupsFresh() const;
    void setAutoVisGroupVisible(hammer::vmf::AutoVisGroup id, bool visible);
    // Object Properties VisGroup tab: the visgroups every selected object is
    // in, with "mixed" set when the selection disagrees.
    std::vector<int> selectionVisGroups(bool* mixed = nullptr) const;
    // Solids currently hidden by a VisGroup or QuickHide. Handed to commands
    // that must not touch what the user cannot see (Replace Textures).
    const std::unordered_set<int>& hiddenSolidIds() const { return hiddenSolids_; }
    bool setSelectionVisGroupMembership(int visGroupId, bool member);
    // Map > Entity Report: one row per entity in the map, carrying everything
    // the report's filters work on.
    struct EntityReportEntry
    {
        int id{-1};
        QString classname;
        QString targetName;
        bool brushEntity{false};
        // True when every solid of a brush entity is hidden by the tool-texture
        // filter, which is the only object hiding this port has.
        bool hidden{false};
        // The raw (SmartEdit-off) keyvalues, which is what the report's
        // by-key/value search matches against.
        std::vector<std::pair<QString, QString>> properties;
    };
    std::vector<EntityReportEntry> entityReport() const;
    // Map > Entity Gallery: one point entity of every class in the loaded game
    // data, laid out in a grid. Returns how many were created.
    std::size_t createEntityGallery();
    // Selects exactly the given entities and mirrors that into the views.
    void selectEntitiesById(const std::vector<int>& ids);
    // The same for a mixed list of solids and entities, which is what a map
    // problem's "Go to" works from.
    void selectObjects(std::vector<hammer::vmf::ObjectRef> objects);

    // Map > Check for Problems (hammer/mapcheckdlg.cpp). One row of the
    // dialog's error list.
    struct MapProblem
    {
        // Only the checks CMapCheckDlg::DoCheck actually runs and that survive
        // the port; see checkForProblems() for what is deliberately dropped.
        enum class Type {
            NoPlayerStart,
            DuplicateFaceIds,
            DuplicateNodeIds,
            SolidStructure,
            InvalidTexture,
            InvalidTextureAxes,
            UnusedKeyvalue,
            EmptyEntity,
            MissingTarget,
            BadConnection,
            OverlayFaceList,
        };

        Type type{Type::NoPlayerStart};
        // The list row and the read-only description box below it, standing in
        // for the IDS_*/IDS_*_DESC string pairs, which are not in this tree.
        QString text;
        QString description;
        // What "Go to" selects and centers on. Empty for map-wide problems.
        std::vector<hammer::vmf::ObjectRef> objects;
        bool canFix{false};
        // Which part of `objects.front()` the fix acts on: a side of a solid,
        // a keyvalue of an entity, or an entity's outbound connection.
        int sideId{-1};
        QString key;
        QString value;
    };

    // Runs every check over the map. visibleOnly mirrors the dialog's "Check
    // visible parts of the map only" box.
    std::vector<MapProblem> checkForProblems(bool visibleOnly) const;
    // Fix / Fix all of type: applies the given problems' repairs as one undo
    // step. Returns how many were repaired.
    std::size_t fixProblems(const std::vector<MapProblem>& problems);

    // Edit > Find entities (Ctrl+Shift+F): select every entity whose
    // targetname is exactly `name` and bring the result into view. Returns how
    // many were found; the selection is left alone when nothing matches.
    std::size_t selectEntitiesByName(const QString& name);
    // Centers all four views on the selection without changing their zoom.
    void centerViewsOnSelection();
    // CMapDoc::OnEditSelnext / OnEditSelprev -> CSelection::SetCurrentHit.
    // Walks the hit list built by the last pick, toggling the previous entry
    // back off and the new one on.
    void selectNextHit(bool forward = true);
    // Map > Load Pointfile / Unload Pointfile (hammer/mapdoc.cpp
    // OnMapLoadpointfile / OnMapUnloadpointfile). The leak trace vbsp writes
    // beside the map when it cannot seal it.
    QString defaultPointFilePath() const;
    // Reads "x y z" per line, stopping at the first line that is not three
    // numbers, as the original does. Returns false with *error set when the
    // file cannot be read or holds no points.
    bool loadPointFile(const QString& path, QString* error = nullptr);
    void unloadPointFile();
    bool hasPointFile() const { return !pointFile_.empty(); }
    std::size_t pointFilePointCount() const { return pointFile_.size(); }
    QString pointFilePath() const { return pointFilePath_; }
    // Map > Load Portal File / Unload Portal File. vbsp writes the .prt beside
    // the bsp for vvis; loading it here shows the visleaf portals in Hammer
    // instead of Glview.
    QString defaultPortalFilePath() const;
    // Parses the PRT1/PRT2 text format: a header token, the cluster/portal
    // counts, then one portal per line as "<points> <cluster> <cluster>"
    // followed by that many "(x y z )" triples. Stops at the first line that
    // does not parse, so a truncated file still shows what is intact.
    bool loadPortalFile(const QString& path, QString* error = nullptr);
    void unloadPortalFile();
    bool hasPortalFile() const { return !portalFile_.empty(); }
    std::size_t portalCount() const { return portalFile_.size(); }
    QString portalFilePath() const { return portalFilePath_; }

    // "openVisGroupPage" opens the dialog on its VisGroup tab instead of the
    // usual first page - what a double-click on a plain world brush wants,
    // since a solid with no entity class has nothing else to edit there.
    void showObjectProperties(QWidget* dialogParent = nullptr, bool openVisGroupPage = false);
    void showMapProperties(QWidget* dialogParent = nullptr);
    void setSelectionMode(SelectionMode mode);
    void setFgdDatabase(std::shared_ptr<hammer::fgd::Database> fgd);
    void setMaterialSystem(std::shared_ptr<hammer::assets::MaterialSystem> materials);
    void setMaterialRenderingEnabled(bool enabled);
    void setWireframeOverlayEnabled(bool enabled);
    void setDisplacementSolidMaskEnabled(bool enabled);
    void setTexturedRenderMode(MapViewWidget::TexturedRenderMode mode);
    void setHdrEnabled(bool enabled);
    void setRayTracedGamma(float gamma);
    void setMaterialEffectsEnabled(bool phong, bool specular, bool bumpMaps,
                                   bool lightWarp, bool selfIllum, bool rimLight);
    void setMaterialEffectIntensities(float phong, float specular, float bumpMaps);
    QStringList toolTextureMaterials() const;
    bool toolTextureVisible(const QString& material) const;
    void setToolTextureVisible(const QString& material, bool visible);
    void setAllToolTexturesVisible(bool visible);
    void setCurrentMaterial(const QString& material);
    QString currentMaterial() const { return currentMaterial_; }
    const std::vector<hammer::vmf::FaceRef>& faceSelection() const { return faceSelection_; }
    void setEntityClass(const QString& classname);
    void setTool(MapViewWidget::Tool tool);
    void setTransformMode(MapViewWidget::TransformMode mode);
    void deleteActiveCamera();
    void cycleActiveCamera(bool forward);

    // Clipping tool (hammer/ToolClipper.cpp). The clip line, its plane, the
    // clip mode, and the clip results live on the document, exactly as the one
    // Clipper3D instance owned by CToolManager does.
    QString cycleClipMode();
    QString clipModeName() const;

    // Vertex manipulation (hammer/ToolMorph.cpp). The vertex meshes, the handle
    // selection and the handle display mode live on the document, as the one
    // Morph3D instance owned by CToolManager does.
    QString cycleMorphHandleMode();
    QString morphHandleModeName() const;
    bool morphActive() const { return !morphSolids_.empty(); }

    // Texture Application (hammer/ToolMaterial.cpp, faceeditsheet.cpp). The
    // face list, the click mode and the "Treat as one" flag live on the
    // document, as CFaceEditSheet's do on the one sheet the main frame owns.
    void setFaceClickMode(FaceEditSheet::ClickMode mode) { faceClickMode_ = mode; }
    void setTreatFacesAsOne(bool treatAsOne) { treatFacesAsOne_ = treatAsOne; }
    void setFaceSelectionMaskHidden(bool hidden);
    void clearFaceSelection();
    FaceEditValues faceEditValues() const;
    // CFaceEditMaterialPage::Apply on the whole face list.
    void applyFaceEdit(const hammer::vmf::FaceTextureEdit& edit);
    // FACE_APPLY_LIGHTMAP_SCALE: the lightmap scale only, leaving the material
    // and the mapping alone (the Lightmap page's Apply).
    void applyLightmapScale(int scale);
    void justifyFaceSelection(int justification);
    // Smoothing Groups page. Toggling a group adds the selected faces to it or
    // removes them from it (CSmoothingGroupMgr::AddFaceToGroup /
    // RemoveFaceFromGroup) in one undo step; the shown group is tinted in the
    // 3D views, and "Select Faces in Group" makes the group the face list.
    void toggleFaceSmoothingGroup(int group, bool add);
    void setShownSmoothingGroup(int group);
    void selectFacesInSmoothingGroup(int group);
    void alignFaceSelection(int alignment);
    // Displacement page (hammer/faceedit_disppage.cpp, tooldisplace.cpp). The
    // active displacement tool and the paint settings live on the document,
    // the way CToolDisplace's do on the one tool CToolManager owns.
    void createFaceDisplacements(int power);
    void destroyFaceDisplacements();
    void setDisplacementTool(DisplacementTool tool);
    void setDisplacementPaintSettings(const DisplacementPaintSettings& settings);
    // Attributes group + Apply (CFaceEditDispPage::OnButtonApply), Noise
    // (OnButtonNoise) and Sew (OnButtonSew) over the face list.
    void applyDisplacementAttributes(
        const hammer::vmf::EditorModel::DisplacementAttributeEdit& edit);
    void applyDisplacementNoise(double minimum, double maximum);
    void sewFaceDisplacements();
    // The displacement attributes shared by every face in the list, for the
    // page's Attributes fields (CFaceEditDispPage::UpdateDialogData).
    void displacementAttributeValues(std::optional<int>& power,
                                     std::optional<double>& elevation) const;

    // ID_VIEW_3DLIGHTMAP_GRID: the 3D views draw the luxel grid of every face,
    // and the Texture Application tool's right-click applies the lightmap scale
    // only while it is on (CToolMaterial::OnRMouseDown3D).
    void setLightmapGridVisible(bool visible);
    void setDetailPropsVisible(bool visible);
    bool lightmapGridVisible() const { return lightmapGridVisible_; }
    // "Apply current texture" (Shift+T): a momentary command, not a mode.
    void applyCurrentTexture();

    // Replace Textures dialog (hammer/replacetexdlg.cpp). Runs the replace/mark
    // over the requested scope, refreshes the scene when anything changed, and
    // reports the affected face count through notifyDocumentState.
    hammer::vmf::ReplaceTexturesResult replaceTextures(
        const hammer::vmf::ReplaceTexturesRequest& request,
        const std::function<bool(const std::string&, int&, int&)>& materialSize = {});

    hammer::vmf::ClipboardData copySelection() const;
    bool cutSelection(hammer::vmf::ClipboardData& clipboard);
    bool paste(const hammer::vmf::ClipboardData& clipboard);
    // Edit > Paste Special. Returns the number of objects created, 0 on
    // failure, so the caller can report the count.
    std::size_t pasteSpecial(const hammer::vmf::ClipboardData& clipboard,
                             hammer::vmf::PasteSpecialOptions options);
    // Where the 2D views are looking: the Top view supplies x and y, the Front
    // view z (the Side view standing in for either if one is missing). This is
    // the destination Paste Special uses with "start at center of original"
    // switched off.
    hammer::vmf::Vec3 viewsCenterWorld() const;
    bool duplicateSelection();
    // Shift+arrow: duplicate and move the copy by delta (Hammer's clone-nudge).
    bool duplicateSelectionBy(const hammer::vmf::Vec3& delta);

signals:
    void coordinatesChanged(const QString& text);
    void activeViewChanged(MapViewWidget* view);
    void titleChanged(const QString& title);
    void modifiedChanged(bool modified);
    void documentMessage(const QString& message);
    void editStateChanged();
    // Emitted whenever the View > Tool Textures filter changes, so anything
    // mirroring it (the Show nodraw faces button) follows without every caller
    // having to remember to refresh it.
    void toolTextureVisibilityChanged();
    void faceSelectionChanged(const FaceEditValues& values);
    // A face lift (LiftSelect / Lift click modes) made this the current
    // material. There is exactly ONE writer direction for the current
    // material: lift -> document -> the Textures bar and Face Edit sheet
    // mirror it. The sheet's own material combo stays passive under its
    // updating_ guard, which is what keeps this from looping.
    void currentMaterialLifted(const QString& material);
    void selectionChanged(const QString& summary, const QString& sizeSummary);
    // Clipper3D::OnEscape returns to the Selection tool when there is no clip
    // line left to clear; only the main frame owns the tool palette state.
    void selectionToolRequested();
    // Emitted when the handle mode changes (menu or click-cycling in a view)
    // so the main frame can keep the Ctrl+M menu text in step.
    void transformModeChanged(MapViewWidget::TransformMode mode);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void activateView(MapViewWidget* view);
    void notifyDocumentState(const QString& message = {});
    void notifySelectionState();
    void rebuildScene(bool fit = true);
    // Interactive-edit fast path: rebuild only the geometry of the objects the
    // drag/paint just changed. Falls back to rebuildScene() whenever the scene
    // cannot be updated in place.
    void rebuildSelectedObjectsInScene();
    void rebuildSceneObjects(const std::vector<hammer::vmf::ObjectRef>& changed);
    void rebuildSceneFaces(const std::vector<hammer::vmf::FaceRef>& faces);
    // "changedEntityIds", when given, restricts the FGD/model/sprite helper
    // resolution to those entities. Everything else keeps the visualization it
    // already carries, which is what makes an interactive edit independent of
    // how many entities the map has.
    void publishScene(bool fit, const std::unordered_set<int>* changedEntityIds = nullptr);
    void applyFgdEntityVisualization(const std::unordered_set<int>* changedEntityIds = nullptr);
    hammer::vmf::Bounds visualSelectionBounds() const;
    void setSelectionOnViews();
    void applyToolTextureVisibility();
    // Recomputes hiddenSolids_/hiddenEntities_ from the quickhide sets and the
    // per-object visgroupshown flags.
    void refreshHiddenObjects();
    // Reclassifies every object into the auto-visgroup categories.
    void refreshAutoVisGroups();
    // Strips the hidden objects out of scene_ and clears its lineage.
    void applySceneVisibility();
    // Drops hidden objects from the selection and the hit list, then
    // republishes the scene. Used by every quickhide command.
    void applyQuickHide();
    // CSelection::RemoveInvisibles for a visibility change that did not come
    // from quickhide.
    void dropHiddenFromSelection();
    // Every object currently in the scene, as the quickhide sets store them.
    void collectQuickHideTargets(const std::vector<hammer::vmf::ObjectRef>& objects,
                                 std::unordered_set<int>& solids,
                                 std::unordered_set<int>& entities) const;
    void beginMove();
    void moveSelection(const hammer::vmf::Vec3& delta);
    void finishMove();
    void beginResize();
    void resizeSelection(const hammer::vmf::Vec3& factors, const hammer::vmf::Vec3& pivot);
    void beginRotate();
    void rotateSelection(double radians, hammer::vmf::RotationAxis axis, const hammer::vmf::Vec3& pivot);
    void finishTransform();
    void createBlock(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                     int extrusionAxis);
    void createEntity(const hammer::vmf::Vec3& origin);
    void createEntityOnSurface(const hammer::vmf::Vec3& position, const hammer::vmf::Vec3& normal);
    void createDecal(const hammer::vmf::Vec3& position,
                     const hammer::vmf::Vec3& normal, int sideId);
    void createOverlay(const hammer::vmf::Vec3& position,
                       const hammer::vmf::Vec3& normal, int sideId);
    void editCamera(int index, const hammer::vmf::Vec3& eye, const hammer::vmf::Vec3& lookAt, bool created);
    void updateClipLine(const hammer::vmf::Vec3& first, const hammer::vmf::Vec3& second,
                        const hammer::vmf::Vec3& viewAxis);
    void applyClip();
    void clearClip();
    void pushClipToViews();
    void beginMorph();
    void endMorph();
    void refreshMorphFromScene();
    void selectMorphHandles(const QList<int>& handles, bool toggle);
    void clearMorphHandleSelection();
    void moveMorphSelection(const hammer::vmf::Vec3& delta);
    void finishMorphMove();
    void morphEscape();
    void pushMorphToViews();
public:
    // Face tool clicks (public so the harness can drive them without a mouse).
    void handleFaceSelect(MapViewWidget* view, int solidId, int sideId, bool control, bool shift);
    void handleFaceApply(MapViewWidget* view, int solidId, int sideId, bool edgeAlign, bool shift);
private:
    void toggleFaceInList(int solidId, int sideId, bool clear);
    std::optional<hammer::vmf::FaceEditTarget> faceTarget(const hammer::vmf::FaceRef& face) const;
    std::vector<hammer::vmf::FaceEditTarget> faceTargets() const;
    void validateFaceSelection();
    void pushFaceSelectionToViews();
    void pushSmoothingGroupToViews();
    void pushDisplacementPaintToViews();
    // CToolDisplace::OnLMouseDown3D / OnMouseMove3D / OnLMouseUp3D over the
    // spatial paint sphere.
    void beginDisplacementPaint(const hammer::vmf::Vec3& position,
                                const hammer::vmf::Vec3& normal, bool lower);
    void continueDisplacementPaint(const hammer::vmf::Vec3& position, bool lower);
    void endDisplacementPaint();
    hammer::vmf::SpatialPaintData paintDataFor(const hammer::vmf::Vec3& position, bool lower) const;
    void notifyFaceSelectionState();
    void liftMaterialFrom(const hammer::vmf::FaceRef& face);
    void seedFaceSelectionFromObjectSelection();
    void loadCamerasFromDocument();
    void saveCamerasToDocument();
    void pushCamerasToViews();
    std::vector<hammer::vmf::Property> entityDefaults(const std::string& classname) const;

    QSplitter* verticalSplitter_{nullptr};
    QSplitter* topSplitter_{nullptr};
    QSplitter* bottomSplitter_{nullptr};
    hammer::vmf::EditorModel::PrimitiveKind primitiveKind_{
        hammer::vmf::EditorModel::PrimitiveKind::Block};
    int primitiveFaces_{8};
    std::array<MapViewWidget*, 4> views_{};
    MapViewWidget* activeView_{nullptr};
    bool activeViewMaximized_{false};

    hammer::vmf::EditorModel editor_;
    std::shared_ptr<hammer::vmf::Scene> scene_;
    std::shared_ptr<hammer::fgd::Database> fgd_;
    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    std::shared_ptr<hammer::assets::StudioModelSystem> studioModels_;
    bool materialRenderingEnabled_{true};
    bool wireframeOverlayEnabled_{false};
    bool displacementSolidMaskEnabled_{true};
    MapViewWidget::TexturedRenderMode texturedRenderMode_{MapViewWidget::TexturedRenderMode::Unlit};
    bool phongEnabled_{true};
    bool specularEnabled_{true};
    bool bumpMapsEnabled_{true};
    bool lightWarpEnabled_{true};
    bool selfIllumEnabled_{true};
    bool hdrEnabled_{true};
    float rayTracedGamma_{2.2f};
    bool rimLightEnabled_{true};
    float phongIntensity_{1.0f};
    float specularIntensity_{1.0f};
    float bumpMapIntensity_{1.0f};
    std::unordered_set<std::string> hiddenToolTextures_;
    // QuickHide: ids of the solids and entities held out of the published
    // Scene. Session state - never serialized, never an undo step.
    std::unordered_set<int> quickHiddenSolids_;
    std::unordered_set<int> quickHiddenEntities_;
    // The union of quickhide and the document's visgroup-hidden objects: what
    // applySceneVisibility actually strips. Derived, never authoritative.
    std::unordered_set<int> hiddenSolids_;
    std::unordered_set<int> hiddenEntities_;
    // Group and visgroup state read out of the document, refreshed on every
    // full scene rebuild.
    hammer::vmf::MapObjectIndex objectIndex_;
    // Auto-visgroup membership, derived from objectIndex_ on the same rebuild.
    // Rebuilt lazily: a remote delta stream (drag-step rate) marks it stale
    // instead of reclassifying the whole map per edit, and the readers below
    // refresh it on demand. Local incremental edits already leave it stale
    // until the next full rebuild, so this only matches that behaviour.
    mutable hammer::vmf::AutoVisGroupIndex autoVisGroupIndex_;
    mutable bool autoVisGroupsStale_{false};
    // Classnames the loaded FGD declares with @NPCClass, for the NPCs category.
    std::unordered_set<std::string> npcClasses_;
    // CVisGroup::s_bShowAll: shows everything without touching the per-object
    // flags, so unchecking it restores exactly what was hidden.
    bool showAllVisGroups_{false};
    // Options.general.bIgnoreGroups / the Ignore Groups toolbar toggle.
    bool ignoreGroups_{false};
    // CSelection::m_HitList / m_iCurHit. Filled by CMapView::SelectAt on every
    // pick and walked by CSelection::SetCurrentHit.
    std::vector<hammer::vmf::ObjectRef> hitList_;
    int currentHit_{-1};
    std::vector<hammer::vmf::CameraDef> cameras_;
    int activeCamera_{-1};
    bool clipActive_{false};
    hammer::vmf::Vec3 clipPoints_[2]{};
    hammer::vmf::Vec3 clipViewAxis_{0.0, 0.0, 1.0};
    hammer::vmf::EditorModel::ClipMode clipMode_{hammer::vmf::EditorModel::ClipMode::Front};
    hammer::vmf::EditorModel::ClipPreview clipPreview_;
    std::vector<hammer::vmf::FaceRef> faceSelection_;
    FaceEditSheet::ClickMode faceClickMode_{FaceEditSheet::ClickMode::LiftSelect};
    bool treatFacesAsOne_{false};
    bool faceSelectionMaskHidden_{false};
    int shownSmoothingGroup_{0};
    bool lightmapGridVisible_{false};
    bool detailPropsVisible_{true};
    DisplacementTool displacementTool_{DisplacementTool::Select};
    DisplacementPaintSettings displacementPaintSettings_;
    bool displacementPainting_{false};
    // CToolDisplace's m_vecPaintAxis for DISPPAINT_AXIS_FACE: the normal the
    // ray traced at mouse-down, held for the whole stroke.
    hammer::vmf::Vec3 displacementPaintNormal_{0.0, 0.0, 1.0};
    // The sheet's mapping fields as last committed, so the Apply click modes
    // and the right-click apply can read them like the original dialog does.
    hammer::vmf::FaceTextureEdit pendingFaceEdit_;
    std::vector<hammer::vmf::MorphSolid> morphSolids_;
    std::vector<int> morphSolidIds_;
    std::vector<hammer::vmf::MorphHandleRef> morphSelection_;
    std::vector<hammer::vmf::MorphHandleRef> morphHandleRefs_;
    hammer::vmf::MorphHandleMode morphHandleMode_{hammer::vmf::MorphHandleMode::VerticesAndEdges};
    std::vector<hammer::vmf::Vec3> pointFile_;
    QString pointFilePath_;
    std::vector<std::vector<hammer::vmf::Vec3>> portalFile_;
    QString portalFilePath_;
    QString filePath_;
    QString currentMaterial_{QStringLiteral("tools/toolsnodraw")};
    QString entityClass_{QStringLiteral("info_player_start")};
    SelectionMode selectionMode_{SelectionMode::Groups};
    MapViewWidget::Tool tool_{MapViewWidget::Tool::Selection};
    MapViewWidget::TransformMode transformMode_{MapViewWidget::TransformMode::Scale};
};
