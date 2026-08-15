#pragma once

#include "VmfDisplacement.hpp"
#include "VmfDocument.hpp"
#include "VmfFaceEdit.hpp"
#include "VmfScene.hpp"
#include "VmfSolidClip.hpp"
#include "VmfSolidMorph.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hammer::vmf {

struct Property
{
    std::string key;
    std::string value;

    bool operator==(const Property&) const = default;
};

struct Bounds
{
    Vec3 minimum{};
    Vec3 maximum{};
    bool valid{false};

    Vec3 center() const
    {
        return {(minimum.x + maximum.x) * 0.5,
                (minimum.y + maximum.y) * 0.5,
                (minimum.z + maximum.z) * 0.5};
    }
};

enum class RotationAxis { X, Y, Z };

// hammer/replacetexdlg.cpp IDC_ACTION radio group: exact name match, substring
// match against the whole material name, or (SubstitutePartial) leave the rest
// of the name alone and substitute only the matched substring.
enum class TextureMatchMode { Exact, Partial, SubstitutePartial };

// CReplaceTexDlg / CMapDoc::ReplaceTextures parameters. The dialog is not in
// this source tree (only replacetexdlg.cpp/.h and the resource ids survive;
// CMapDoc::ReplaceTextures itself is missing from mapdoc.cpp here), so the
// matching/substitution semantics below are the natural reading of the
// dialog's fields rather than a line-for-line port.
struct ReplaceTexturesRequest
{
    std::string find;
    std::string replace;
    TextureMatchMode mode{TextureMatchMode::Exact};
    // IDC_INMARKED radio: true = "Replace in: Marked objects" (the current
    // selection), false = "Everything".
    bool selectionOnly{false};
    // IDC_MARKONLY: select the matching solids instead of changing them.
    bool markOnly{false};
    // IDC_RESCALETEXTURECOORDINATES: keep the texture's apparent world size
    // when the new material has different dimensions than the old one.
    bool rescaleTextureCoordinates{false};
    // IDC_HIDDEN: also replace on objects the user cannot currently see. Off
    // by default, so a replace only touches what is on screen. The caller
    // supplies the hidden solid ids, since visibility is the document widget's
    // business (VisGroups plus QuickHide), not the model's.
    bool includeHidden{false};
    std::unordered_set<int> hiddenSolidIds;
};

struct ReplaceTexturesResult
{
    int facesMatched{0};
    int facesChanged{0};
};

struct ClipboardObject
{
    ObjectType type{ObjectType::Solid};
    Block block;
};

struct ClipboardData
{
    std::vector<ClipboardObject> objects;
    Bounds bounds;

    bool empty() const { return objects.empty(); }
};

// Edit > Paste Special. Offsets and rotations are accumulative: copy N is
// placed N steps away from the original, so one clipboard rail becomes a whole
// picket fence and one stair becomes a staircase.
struct PasteSpecialOptions
{
    // IDC_COPIES. Copies to create, not counting the clipboard original.
    int copies{1};
    // IDC_STARTATORIGINAL. On (the default), the copies march away from where
    // the clipboard objects were cut from. Off, the whole run is moved so the
    // clipboard's center lands on viewCenter first.
    bool startAtOriginal{true};
    Vec3 viewCenter{};
    // IDC_GROUP. All the pasted objects land in one new group.
    bool groupCopies{false};
    // IDC_OFFSET*, in map units, applied once per copy.
    Vec3 offset{};
    // IDC_ROTATE*, degrees about each world axis, applied once per copy.
    Vec3 rotation{};
    // IDC_MAKENAMESUNIQUE. Every copy's named entities get a fresh unique
    // targetname, and I/O connections between copied entities are repointed at
    // the copy's own names.
    bool uniqueEntityNames{false};
    // IDC_PREFIX. Prepended to every named entity's targetname (and to the
    // connections that refer to it).
    std::string namePrefix;
};

class EditorModel
{
public:
    EditorModel();
    explicit EditorModel(Document document);

    const Document& document() const { return document_; }
    Document& document() { return document_; }
    void setDocument(Document document);

    const std::vector<ObjectRef>& selection() const { return selection_; }
    bool isSelected(const ObjectRef& object) const;
    void select(const ObjectRef& object, bool toggle = false, bool additive = false);
    void setSelection(std::vector<ObjectRef> selection);
    void clearSelection();
    void selectAll();
    Bounds selectionBounds() const;

    std::vector<Property> selectedProperties() const;
    bool replaceSelectedProperties(const std::vector<Property>& properties,
                                   std::string label = "Change Properties");
    std::vector<Property> worldProperties() const;
    bool replaceWorldProperties(const std::vector<Property>& properties,
                                std::string label = "Change Map Properties");

    bool translateSelection(const Vec3& delta, std::string label = "Move Objects");
    bool scaleSelection(const Vec3& factors, const Vec3& pivot, std::string label = "Resize Objects");
    bool rotateSelection(double radians, RotationAxis axis, const Vec3& pivot,
                         std::string label = "Rotate Objects");
    bool deleteSelection(std::string label = "Delete Objects");

    // Arbitrary edit of the raw document as one undo step, for commands that
    // work off their own object list rather than the selection (Map > Check
    // for Problems' fixes). The callback returns false when it changed
    // nothing, which rolls the snapshot back and leaves no undo entry.
    bool applyDocumentEdit(const std::function<bool(Document&)>& edit,
                           std::string label = "Edit Map");

    ClipboardData copySelection() const;
    bool paste(const ClipboardData& clipboard, const Vec3& offset = {16.0, 16.0, 0.0},
               std::string label = "Paste Objects");
    // Edit > Paste Special (CPasteSpecialDlg): paste the clipboard `copies`
    // times, each copy displaced and turned one more step than the last.
    bool pasteSpecial(const ClipboardData& clipboard, const PasteSpecialOptions& options,
                      std::string label = "Paste Special");
    bool duplicateSelection(const Vec3& offset = {16.0, 16.0, 0.0},
                            std::string label = "Duplicate Objects");

    std::optional<ObjectRef> createBlock(const Vec3& first, const Vec3& second,
                                         std::string material = "tools/toolsnodraw",
                                         std::string label = "Create Block");
    // The Block tool's primitive shapes (hammer's ObjectBar primitives). The
    // shape is formed in the 2D plane perpendicular to extrusionAxis (0=x,
    // 1=y, 2=z — the axis the drawing view cannot show) and extruded through
    // the bounds along it. faces is the side count for cylinder/spike.
    enum class PrimitiveKind { Block, Wedge, Cylinder, Spike };
    std::optional<ObjectRef> createPrimitive(PrimitiveKind kind, const Vec3& first,
                                             const Vec3& second, int extrusionAxis,
                                             int faces,
                                             std::string material = "tools/toolsnodraw",
                                             std::string label = "Create Primitive");
    std::optional<ObjectRef> createPointEntity(std::string classname, const Vec3& origin,
                                               const std::vector<Property>& defaults = {},
                                               std::string label = "Create Entity");
    // Map > Entity Gallery: one point entity per description, all in a single
    // undo step, all left selected. Returns how many were created.
    struct PointEntitySpec
    {
        std::string classname;
        Vec3 origin;
        std::vector<Property> defaults;
    };
    std::size_t createPointEntities(const std::vector<PointEntitySpec>& entities,
                                    std::string label = "Create Entities");

    // Clipper3D's three clip modes: keep the front half, keep the back half,
    // or keep both halves (ToolClipper.h enum { FRONT, BACK, BOTH }).
    enum class ClipMode { Front, Back, Both };

    // Clipper3D::CalcClipResults output: one entry per clipped solid, each a
    // list of face loops. "kept" is what SaveClipResults would write back,
    // "discarded" is what it would throw away.
    struct ClipPreview
    {
        std::vector<FacePolygons> kept;
        std::vector<FacePolygons> discarded;

        bool empty() const { return kept.empty() && discarded.empty(); }
    };

    ClipPreview previewClip(const ClipPlane& plane, ClipMode mode) const;
    bool clipSelection(const ClipPlane& plane, ClipMode mode, std::string label = "Clip Objects");

    // Tools > Carve: subtract the selected solids (including the solids of
    // selected brush entities) from every other non-displacement solid they
    // overlap, as one undo step. The carvers themselves are never modified and
    // stay selected. A brush entity whose solids are all carved away is
    // deleted. Returns false — with no undo entry — when nothing overlapped.
    // See VmfSolidCarve.hpp for how this deviates from Valve's fragment-happy
    // CMapSolid::Carve.
    bool carveSelection(std::string label = "Carve");

    // Morph3D::SetEmpty: commit the dragged vertex meshes back onto their
    // solids as ONE undo step (GetHistory()->MarkUndoPosition( NULL, "Morphing" )).
    // Meshes whose vertices did not move, and meshes that would no longer form a
    // solid, are left alone.
    bool applyMorph(const std::vector<MorphSolid>& solids, std::string label = "Morphing");

    // CMapDoc::OnEditToEntity (Ctrl+T): every solid in the selection - world
    // solids and the solids of selected brush entities alike - moves into one
    // new brush entity of the given class, as one undo step. A brush entity
    // that loses all of its solids is deleted. Returns the new entity, or
    // nullopt when the selection holds no eligible solids.
    std::optional<ObjectRef> tieSelectionToEntity(std::string classname,
                                                  std::string label = "To Entity");

    // Texture Application tool (hammer/ToolMaterial.cpp, faceedit_materialpage.cpp).
    // Every one of these is a single undo step, as each of the original's
    // GetHistory()->MarkUndoPosition calls is.
    std::optional<FaceTexture> faceTexture(const FaceRef& face) const;

    // CFaceEditMaterialPage::Apply. Blank fields (nullopt) are left alone on
    // every face, exactly as the NOT_INIT sentinel does in the original.
    bool applyFaceTextures(const std::vector<FaceRef>& faces, const FaceTextureEdit& edit,
                           std::string label = "Apply Face Attributes");

    // Smoothing groups (hammer/SmoothingGroupMgr.cpp AddFaceToGroup /
    // RemoveFaceFromGroup, driven by the Smoothing Groups page's group
    // buttons). Both masks are applied per face: bits in addGroups are set,
    // bits in removeGroups are cleared, sides that end up unchanged are left
    // alone, and the whole batch is one undo step.
    std::uint32_t faceSmoothingGroups(const FaceRef& face) const;
    bool applySmoothingGroups(const std::vector<FaceRef>& faces, std::uint32_t addGroups,
                              std::uint32_t removeGroups,
                              std::string label = "Set Smoothing Groups");

    // Every face of the map that belongs to the given 1-based group; the port's
    // stand-in for CSmoothingGroupMgr's per-group face bucket.
    std::vector<FaceRef> facesInSmoothingGroup(int group) const;

    // CFaceEditMaterialPage::OnJustify. bTreatManyAsOneFace merges the world
    // extents of every selected face before justifying (GetAllFaceExtents).
    bool justifyFaces(const std::vector<FaceEditTarget>& faces, TextureJustification justification,
                      bool treatAsOne, std::string label = "Justify texture");

    // CFaceEditMaterialPage::OnAlign (Align World / Align Face).
    bool alignFaceTextures(const std::vector<FaceEditTarget>& faces, TextureAlignment alignment,
                           std::string label = "Align texture");

    // CFaceEditMaterialPage::AlignToView, reached through the Align To View
    // click mode.
    bool alignFacesToView(const std::vector<FaceEditTarget>& faces, const Vec3& viewRight,
                          const Vec3& viewUp, const Vec3& viewPoint,
                          std::string label = "Apply texture");

    // FACE_APPLY_ALIGN_EDGE: copy the reference face's coordinate system onto
    // each target and rotate it about the shared edge (CopyTCoordSystem).
    bool edgeAlignFaces(const std::vector<FaceEditTarget>& faces, const FaceEditTarget& reference,
                        std::string label = "Apply texture");

    // Displacement page (hammer/faceedit_disppage.cpp).
    //
    // CFaceEditDispPage::OnButtonCreate: every stored face that is a quad and
    // does not already carry a displacement becomes one of the given power.
    // The whole batch is one undo step, as PreUndo/PostUndo make it.
    bool createDisplacements(const std::vector<FaceRef>& faces, int power,
                             std::string label = "Displacement Create");

    // CFaceEditDispPage::OnButtonDestroy, whose PreUndo label this reuses.
    bool destroyDisplacements(const std::vector<FaceRef>& faces,
                              std::string label = "Displacement Destroy");

    std::optional<DisplacementInfo> faceDisplacement(const FaceRef& face) const;

    // CFaceEditDispPage::OnButtonApply -> UpdatePower / UpdateElevation /
    // UpdateScale on every displacement in the face list, as one undo step.
    //
    // DEVIATION: the original reads its edit boxes with atoi/atof, so a blank
    // box means 0 - which clamps the power to 2 and zeroes the elevation on a
    // mixed selection. The port keeps the sheet's NOT_INIT/std::optional rule
    // instead: an unset field is left alone on every face. previousScale is the
    // scale the surfaces are currently at; CMapDisp::m_Scale is not serialized
    // to the VMF, so the sheet owns it (see scaleDisplacement).
    struct DisplacementAttributeEdit
    {
        std::optional<int> power;
        std::optional<double> elevation;
        std::optional<double> scale;
        double previousScale{1.0};

        bool empty() const { return !power && !elevation && !scale; }
    };

    bool applyDisplacementAttributes(const std::vector<FaceRef>& faces,
                                     const DisplacementAttributeEdit& edit,
                                     std::string label = "Displacement Attributes");

    // CFaceEditDispPage::OnButtonNoise -> CMapDisp::ApplyNoise over the face
    // list, as one undo step ("Displacement Noise").
    bool applyDisplacementNoise(const std::vector<FaceRef>& faces, double minimum, double maximum,
                                double rockiness = 1.0,
                                std::string label = "Displacement Noise");

    // CFaceEditDispPage::OnButtonSew -> FaceListSewEdges over the face list, as
    // one undo step ("Displacement Sewing").
    bool sewDisplacementFaces(const std::vector<FaceRef>& faces,
                              std::string label = "Displacement Sewing");

    // The displacement's world-space vertices, i.e. what the 3D view renders.
    std::vector<Vec3> displacementVertices(const FaceRef& face) const;

    // CDispPaintMgr::Paint over CToolDisplace::GetSelectedDisps' scope: only
    // the displacements of the faces currently in the face list. Alpha paints
    // the blend channel (DISPPAINT_CHANNEL_ALPHA) instead of the position.
    //
    // The sphere centre is snapped to the nearest displacement vertex first,
    // matching ApplySpatialPaintTool's GetTexelHitIndex -> GetVert.
    bool paintDisplacements(const std::vector<FaceRef>& faces, const SpatialPaintData& paint,
                            bool alphaChannel = false,
                            std::string label = "Displacement Paint");
    // The same edit inside an open transaction, so one drag is one undo entry.
    bool paintDisplacementsInTransaction(const std::vector<FaceRef>& faces,
                                         const SpatialPaintData& paint, bool alphaChannel = false);

    // CEntityConnection (hammer/entityconnection.h): one output->input wire.
    // Stored in the entity's "connections" chunk as key = output name and
    // value = "target,input,parameter,delay,timesToFire"
    // (CEditGameClass::LoadKeyCallback). timesToFire -1 is EVENT_FIRE_ALWAYS.
    struct EntityConnection
    {
        std::string output;
        std::string target;
        std::string input;
        std::string parameter;
        double delay{0.0};
        int timesToFire{-1};

        bool operator==(const EntityConnection&) const = default;
    };

    // The single selected entity's connections (empty for solids/none).
    std::vector<EntityConnection> selectedConnections() const;

    // Object Properties "OK": keyvalues and connections land as ONE undo step.
    bool replaceSelectedPropertiesAndConnections(const std::vector<Property>& properties,
                                                 const std::vector<EntityConnection>& connections,
                                                 std::string label = "Object Properties");

    // "Apply current texture" (Shift+T): the material of every face of every
    // selected solid, in one undo step.
    bool applyMaterialToSelection(const std::string& material, std::string label = "Apply texture");

    // CReplaceTexDlg::DoReplaceTextures -> CMapDoc::ReplaceTextures. Scans the
    // requested scope (selection or the whole document), applies the match
    // mode, and either replaces materials as one undo step or (markOnly)
    // selects the matching solids without touching the document. materialSize
    // resolves a material name to its texture width/height for the rescale
    // option; pass an empty callback to skip rescaling even when requested.
    ReplaceTexturesResult replaceTextures(const ReplaceTexturesRequest& request,
        const std::function<bool(const std::string&, int&, int&)>& materialSize = {},
        std::string label = "Replace Textures");

    // requireSelection is false for edits that are not driven by the object
    // selection (face painting works off the Face Edit sheet's face list).
    bool beginTransaction(std::string label, bool requireSelection = true);
    bool translateSelectionInTransaction(const Vec3& delta);
    bool scaleSelectionInTransaction(const Vec3& factors, const Vec3& pivot);
    bool rotateSelectionInTransaction(double radians, RotationAxis axis, const Vec3& pivot);
    bool commitTransaction();
    void cancelTransaction();
    bool transactionActive() const { return transaction_.has_value(); }

    bool canUndo() const { return undoEnabled_ && !undo_.empty(); }
    bool canRedo() const { return undoEnabled_ && !redo_.empty(); }
    std::string undoLabel() const;
    std::string redoLabel() const;
    bool undo();
    bool redo();

    // Edit > Undo/Redo Active (ID_EDIT_UNDOREDOACTIVE). With undo off no edit
    // history is kept in memory: both stacks are dropped now and never allowed
    // to accumulate again until it is switched back on. A command that fails
    // part-way still rolls back through the top of the undo stack, so a single
    // snapshot lives for the duration of one command; clearHistory() drops it
    // once the command has finished.
    bool undoEnabled() const { return undoEnabled_; }
    void setUndoEnabled(bool enabled);
    // Drops both stacks. An open transaction is unaffected: it snapshots into
    // transaction_, not into the history.
    void clearHistory();

    // Collaborative editing: partition the object-id space so peers minting
    // ids concurrently never collide. With a span set, allocateId only ever
    // returns ids in [base, base + span), and resetNextId scans only that
    // window (another peer's ids must not drag this peer's counter forward).
    // The range survives setDocument. span 0 restores the default behaviour.
    void setIdRange(int base, int span);

    // A remote peer's edit: applied to the live document AND to every undo/
    // redo snapshot (and any open transaction's rollback copy), so a later
    // local undo does not resurrect the pre-remote state of objects this peer
    // never touched. No undo entry is created; the edit is not undoable here.
    void applyExternalEdit(const std::function<void(Document&)>& edit);

private:
    struct Snapshot
    {
        Document document;
        std::vector<ObjectRef> selection;
        std::string label;
    };

    static bool translateObject(Document& document, const ObjectRef& object, const Vec3& delta);
    static bool scaleObject(Document& document, const ObjectRef& object, const Vec3& factors, const Vec3& pivot);
    static bool rotateObject(Document& document, const ObjectRef& object, double radians,
                             RotationAxis axis, const Vec3& pivot);
    static bool eraseObject(Document& document, const ObjectRef& object);
    static Block* findObject(Document& document, const ObjectRef& object);
    static const Block* findObject(const Document& document, const ObjectRef& object);

    std::vector<const Block*> selectedSolidBlocks() const;

    int allocateId();
    void resetNextId();
    void pushUndo(std::string label);
    void validateSelection();

    Document document_;
    std::vector<ObjectRef> selection_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    std::optional<Snapshot> transaction_;
    bool undoEnabled_{true};
    int nextId_{1};
    int idRangeBase_{0};
    int idRangeSpan_{0};
};

} // namespace hammer::vmf
