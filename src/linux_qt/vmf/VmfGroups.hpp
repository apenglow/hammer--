#pragma once

#include "VmfDocument.hpp"
#include "VmfScene.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hammer::vmf {

// --- Grouping and VisGrouping -----------------------------------------------
//
// Two independent mechanisms, both of which only ever affect what Hammer shows
// and what a click selects. Neither reaches the engine.
//
// GROUPS (CMapGroup, hammer/mapgroup.cpp) glue a selection together so that
// clicking any member selects all of them. A group is a "group" block inside
// "world" carrying an id and an editor block; every member points back at it
// with editor { groupid <id> }. Groups nest, because a group block's own editor
// block can carry a groupid too.
//
// A group is never itself an ObjectRef. Nothing in this port selects, moves,
// resizes or deletes "a group" - a pick that lands on a member expands to the
// member list of its topmost group, and every existing transform then runs over
// those solids and entities unchanged. That is exactly the original's model
// ("grouping is purely for selection purposes in Hammer") and it keeps the
// group machinery out of every transform path.
//
// VISGROUPS (CVisGroup, hammer/visgroup.cpp) are named, nestable, colored sets
// whose visibility the user toggles. They live in the "visgroups" root, and an
// object joins one with a repeated editor { visgroupid <id> } key.
//
// The direction of authority here is easy to get backwards, so; per-object
// visibility is the SOURCE OF TRUTH, not a derived value. CMapDoc::
// VisGroups_ShowVisGroup *writes* editor { visgroupshown } onto every member of
// the toggled visgroup (walking nested visgroups via IsInVisGroupRecursive),
// and CMapDoc::VisGroups_UpdateForObject then *derives* each visgroup's
// shown/hidden/partial state back from its members for the tree UI. So an
// object in two visgroups follows whichever was toggled last - it is not
// "hidden if any of its visgroups is hidden".
//
// Only world-level objects carry this state: world solids, entities, and group
// blocks. The solids inside a brush entity cannot belong to a visgroup
// independently of their entity (CMapDoc::VisGroups_ObjectCanBelongToVisGroup),
// and they follow it for visibility.

enum class MapObjectKind { Solid, Entity, Group };

struct MapObjectKey
{
    MapObjectKind kind{MapObjectKind::Solid};
    int id{-1};

    bool operator==(const MapObjectKey&) const = default;
};

struct MapObjectKeyHash
{
    std::size_t operator()(const MapObjectKey& key) const
    {
        return std::hash<int>{}(key.id) * 3u + static_cast<std::size_t>(key.kind);
    }
};

// One user visgroup. Nesting is stored as a parent id rather than a child list
// so the whole tree is a flat vector that survives copying.
struct VisGroupDef
{
    int id{0};
    std::string name;
    int red{192};
    int green{192};
    int blue{192};
    int parentId{0}; // 0 = top level
    // CVisGroup::LoadKeyCallback marks a visgroup named "Auto" as an auto
    // visgroup. Auto visgroups are generated, never written to member objects.
    bool automatic{false};
};

// CVisGroup's VisGroupState_t.
enum class VisGroupState { Hidden, Shown, Partial };

// The group/visgroup state of every object that can carry it.
struct MapObjectEntry
{
    MapObjectKey key;
    // editor { groupid }, or 0 when the object is not in a group.
    int groupId{0};
    // editor { visgroupid }, repeated.
    std::vector<int> visGroupIds;
    // editor { visgroupshown } - the authoritative visibility flag.
    bool visGroupShown{true};
    // editor { visgroupautoshown } - the auto-visgroup half of visibility.
    // CMapClass::IsVisGroupShown ANDs the two flags, so either one being off
    // hides the object.
    bool visGroupAutoShown{true};
};

// A read-only snapshot of the document's grouping state. Rebuilt whenever the
// document changes; every query below is a lookup rather than a document walk.
struct MapObjectIndex
{
    std::vector<MapObjectEntry> objects;
    std::vector<VisGroupDef> visGroups;

    const MapObjectEntry* find(const MapObjectKey& key) const;
    // The outermost group containing this object, or 0 when it is in none.
    // Cycles (which a hand-edited VMF can contain) terminate the walk.
    int topLevelGroup(const MapObjectKey& key) const;
    // Every solid and entity inside this group, including through nested
    // groups. Group blocks themselves are not returned - they are not
    // selectable objects.
    std::vector<ObjectRef> groupMembers(int groupId) const;
    // Every object key directly or transitively inside this group, groups
    // included. Used by ungroup and by the empty-group purge.
    std::vector<MapObjectKey> groupContents(int groupId) const;
    // Shown/hidden/partial, derived from the members' visgroupshown flags the
    // way CMapDoc::VisGroups_UpdateForObject derives it. Nested visgroups count
    // as members of their ancestors (IsInVisGroupRecursive).
    VisGroupState visGroupState(int visGroupId) const;
    // Objects belonging to this visgroup or any of its descendants.
    std::vector<MapObjectKey> visGroupMembers(int visGroupId) const;
    const VisGroupDef* findVisGroup(int visGroupId) const;
    std::vector<int> visGroupAndDescendants(int visGroupId) const;
    // Solids and entities hidden by either half of CMapClass::IsVisGroupShown -
    // a user visgroup (visgroupshown) or an auto visgroup (visgroupautoshown).
    // Solids inside a hidden entity are not listed; they follow their entity in
    // the scene.
    void hiddenObjects(std::unordered_set<int>& solids,
                       std::unordered_set<int>& entities) const;
};

MapObjectIndex indexMapObjects(const Document& document);

// --- Auto VisGroups ---------------------------------------------------------
//
// CMapDoc::AddToAutoVisGroup: a fixed tree of categories that objects are
// sorted into by class, material and geometry. Auto visgroups are DERIVED -
// they are never written to the VMF (CMapClass::SaveVMF skips ids belonging to
// an auto visgroup) and never appear in the user visgroup list, so they cannot
// be renamed, recolored or deleted (hammer/editgroups.cpp refuses).
//
// Their visibility is a second per-object flag: editor { visgroupautoshown }.
// CMapClass::IsVisGroupShown is "m_bVisGroupShown && m_bVisGroupAutoShown", so
// an object is visible only when BOTH its user and auto visgroups allow it.
//
// Ids are negative and fixed, which keeps them out of the user id space and
// makes every "is this an auto visgroup" test a sign check.
enum class AutoVisGroup : int
{
    None = 0,

    // Top-level categories.
    Entities = -1,
    WorldGeometry = -2,
    WorldDetails = -3,
    ToolBrushes = -4,

    // Entities
    PointEntities = -10,
    Nodes = -11,
    NPCs = -12,
    Lights = -13,
    BrushEntities = -14,
    Triggers = -15,

    // World Details
    Props = -20,
    FuncDetail = -21,

    // World Geometry
    Displacements = -30,
    Water = -31,
    Nodraw = -32,
    Sky = -33,
    Black = -34,

    // Tool Brushes
    Areaportals = -40,      // func_areaportal* entities
    Occluders = -41,        // func_occluder entities
    ToolOccluder = -42,     // occluder material
    ToolAreaPortal = -43,   // areaportal material
    Skip = -44,
    ToolTrigger = -45,
    Origin = -46,
    Hint = -47,
    Fog = -48,

    // Tool Brushes > Block
    Block = -50,
    BlockLos = -51,
    BlockBullets = -52,
    BlockLight = -53,

    // Tool Brushes > Clips
    Clips = -60,
    ClipNpc = -61,
    ClipPlayer = -62,
    ClipControl = -63,
    Clip = -64,

    // Tool Brushes > Invisible
    Invisible = -70,
    InvisibleLadder = -71,
    InvisibleInvisible = -72,
};

// One node of the fixed auto-visgroup tree.
struct AutoVisGroupDef
{
    AutoVisGroup id{AutoVisGroup::None};
    const char* name{""};
    AutoVisGroup parent{AutoVisGroup::None};
};

// The whole table, parents before children.
const std::vector<AutoVisGroupDef>& autoVisGroupTable();
const AutoVisGroupDef* findAutoVisGroup(AutoVisGroup id);
inline bool isAutoVisGroupId(int id) { return id < 0; }

// Decides whether a material is water. Injected because the answer needs the
// mounted material system, which the model layer has no business owning - and
// because it must never trigger a material load (see
// MaterialSystem::residentMaterial).
using WaterMaterialPredicate = std::function<bool(std::string_view material)>;

// Every category one object belongs to. An object commonly lands in several:
// the per-face loop in AddToAutoVisGroup adds a solid to Water, Nodraw and a
// tool category independently.
std::vector<AutoVisGroup> classifyForAutoVisGroups(const Document& document,
                                                   const MapObjectKey& key,
                                                   const std::unordered_set<std::string>& npcClasses,
                                                   const WaterMaterialPredicate& isWaterMaterial);

// The auto-visgroup membership of every object in the map, plus which
// categories exist at all (a category with no members is not shown, exactly as
// the original only creates a visgroup when something lands in it).
struct AutoVisGroupIndex
{
    // Object -> the categories it is in.
    std::vector<std::pair<MapObjectKey, std::vector<AutoVisGroup>>> members;
    // Categories that exist, including the parents of any populated category.
    std::unordered_set<int> present;

    std::vector<MapObjectKey> objectsIn(AutoVisGroup id) const;
    bool contains(AutoVisGroup id) const { return present.contains(static_cast<int>(id)); }
};

AutoVisGroupIndex indexAutoVisGroups(const Document& document, const MapObjectIndex& index,
                                     const std::unordered_set<std::string>& npcClasses,
                                     const WaterMaterialPredicate& isWaterMaterial);

// CMapDoc::VisGroups_ShowVisGroup with eVisGroupType == AUTO: writes
// visgroupautoshown, and nothing else.
bool showAutoVisGroup(Document& document, const std::vector<MapObjectKey>& objects, bool show);

// --- Document mutation ------------------------------------------------------
//
// All of these return false when they changed nothing, so a caller wrapping
// them in EditorModel::applyDocumentEdit gets no empty undo entry.

// Creates a "group" block in "world" holding the given objects, and returns its
// id (0 on failure). Members are detached from whatever group they were in
// first, mirroring OnToolsGroup's warning-then-reparent.
int createGroup(Document& document, const std::vector<ObjectRef>& members, int newId);

// CMapDoc::OnToolsUngroup: strips ONE level. The group's direct children are
// re-parented to the group's own parent (which may be another group, or none)
// and the group block is removed. Nested child groups survive as groups.
bool ungroup(Document& document, const std::vector<int>& groupIds);

// Removes group blocks that no longer hold anything.
bool purgeEmptyGroups(Document& document);

// Creates a visgroup under parentId (0 = top level) and returns its id.
int createVisGroup(Document& document, const std::string& name, int newId, int parentId = 0);
bool renameVisGroup(Document& document, int visGroupId, const std::string& name);
bool setVisGroupColor(Document& document, int visGroupId, int red, int green, int blue);
// Deletes the visgroup and strips its id from every member. Children are
// re-parented to the deleted visgroup's parent. Objects are never deleted.
bool deleteVisGroup(Document& document, int visGroupId);
bool setVisGroupParent(Document& document, int visGroupId, int parentId);
// CVisGroup::MoveUp / MoveDown among its siblings.
bool moveVisGroup(Document& document, int visGroupId, bool up);

bool addObjectsToVisGroup(Document& document, const std::vector<ObjectRef>& objects,
                          int visGroupId, bool removeFromOtherVisGroups);
bool removeObjectsFromVisGroup(Document& document, const std::vector<ObjectRef>& objects,
                               int visGroupId);
// CMapDoc::VisGroups_PurgeGroups - the VDC's "a vis group with no objects tied
// to it will get removed from the list". A visgroup with child visgroups is
// kept even when empty itself.
bool purgeEmptyVisGroups(Document& document);

// CMapDoc::VisGroups_ShowVisGroup: writes visgroupshown onto every member of
// this visgroup and its descendants. This is a document change that the
// original deliberately does NOT put on the undo stack.
bool showVisGroup(Document& document, int visGroupId, bool show);
// Sets visgroupshown on specific objects (the VisGroup tab, quickhide handover).
bool setObjectsVisGroupShown(Document& document, const std::vector<ObjectRef>& objects, bool shown);
// Clears every object's hidden flag (CVisGroup::ShowAllVisGroups).
bool showAllObjects(Document& document);

// The largest id used by any solid, entity, side, group or visgroup, so a
// caller can allocate ids that collide with nothing.
int maximumObjectId(const Document& document);
int maximumVisGroupId(const Document& document);

// Finds the block for a world solid, an entity, or a group. Returns nullptr
// when there is none. Entity child solids are reachable through their entity.
Block* findObjectBlock(Document& document, const MapObjectKey& key);
const Block* findObjectBlock(const Document& document, const MapObjectKey& key);
// The object's "editor" child, created if it has none.
Block& objectEditorBlock(Block& object);

} // namespace hammer::vmf
