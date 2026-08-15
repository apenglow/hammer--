#include "VmfGroups.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace hammer::vmf {
namespace {

bool equalsIgnoreCaseAscii(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool isWorldBlock(const Block& block) { return equalsIgnoreCaseAscii(block.name, "world"); }
bool isEntityBlock(const Block& block) { return equalsIgnoreCaseAscii(block.name, "entity"); }

int parseId(const std::string* text)
{
    if (!text) return -1;
    try {
        return std::stoi(*text);
    } catch (...) {
        return -1;
    }
}

int blockId(const Block& block) { return parseId(block.value("id")); }

// VMF booleans are "0"/"1"; anything unparsable keeps the caller's default,
// matching CChunkFile::ReadKeyValueBool leaving its output alone on failure.
bool parseBool(const std::string* text, bool fallback)
{
    if (!text || text->empty()) return fallback;
    try {
        return std::stoi(*text) != 0;
    } catch (...) {
        return fallback;
    }
}

const Block* editorOf(const Block& object)
{
    for (const Block* child : object.children("editor")) return child;
    return nullptr;
}

// Reads one visgroup subtree into the flat list.
void readVisGroupTree(const Block& block, int parentId, std::vector<VisGroupDef>& out)
{
    VisGroupDef group;
    group.parentId = parentId;
    if (const std::string* name = block.value("name")) group.name = *name;
    group.id = parseId(block.value("visgroupid"));
    if (group.id < 0) group.id = 0;
    if (const std::string* color = block.value("color")) {
        int rgb[3] = {192, 192, 192};
        std::size_t index = 0;
        std::size_t cursor = 0;
        while (index < 3 && cursor < color->size()) {
            while (cursor < color->size() &&
                   std::isspace(static_cast<unsigned char>((*color)[cursor]))) {
                ++cursor;
            }
            const std::size_t start = cursor;
            while (cursor < color->size() &&
                   !std::isspace(static_cast<unsigned char>((*color)[cursor]))) {
                ++cursor;
            }
            if (cursor > start) {
                try {
                    rgb[index] = std::stoi(color->substr(start, cursor - start));
                } catch (...) {
                }
                ++index;
            }
        }
        group.red = rgb[0];
        group.green = rgb[1];
        group.blue = rgb[2];
    }
    // CVisGroup::LoadKeyCallback: a visgroup literally named "Auto" is the
    // generated auto-visgroup root.
    group.automatic = equalsIgnoreCaseAscii(group.name, "Auto");
    out.push_back(group);
    for (const Block* child : block.children("visgroup")) readVisGroupTree(*child, group.id, out);
}

MapObjectEntry readObjectEntry(const Block& object, MapObjectKind kind, int id)
{
    MapObjectEntry entry;
    entry.key = {kind, id};
    if (const Block* editor = editorOf(object)) {
        const int groupId = parseId(editor->value("groupid"));
        entry.groupId = groupId > 0 ? groupId : 0;
        for (const std::string& text : editor->values("visgroupid")) {
            try {
                const int visGroupId = std::stoi(text);
                if (visGroupId > 0) entry.visGroupIds.push_back(visGroupId);
            } catch (...) {
            }
        }
        entry.visGroupShown = parseBool(editor->value("visgroupshown"), true);
        entry.visGroupAutoShown = parseBool(editor->value("visgroupautoshown"), true);
    }
    return entry;
}

// Every "group" block, wherever it sits. Hammer writes them into "world", but a
// group's own editor block can name a parent group, so nesting is by id and not
// by block position.
std::vector<Block*> groupBlocks(Document& document)
{
    std::vector<Block*> result;
    for (Block& root : document.roots()) {
        if (!isWorldBlock(root)) continue;
        for (Block* child : root.children("group")) result.push_back(child);
    }
    return result;
}

std::vector<const Block*> groupBlocks(const Document& document)
{
    std::vector<const Block*> result;
    for (const Block& root : document.roots()) {
        if (!isWorldBlock(root)) continue;
        for (const Block* child : root.children("group")) result.push_back(child);
    }
    return result;
}

Block* visGroupsRoot(Document& document, bool create)
{
    if (Block* existing = document.firstRoot("visgroups")) return existing;
    return create ? &document.appendRoot("visgroups") : nullptr;
}

// Depth-first search for a visgroup block by id, with its parent block.
Block* findVisGroupBlock(Block& parent, int visGroupId, Block** owner)
{
    for (Block* child : parent.children("visgroup")) {
        if (parseId(child->value("visgroupid")) == visGroupId) {
            if (owner) *owner = &parent;
            return child;
        }
        if (Block* nested = findVisGroupBlock(*child, visGroupId, owner)) return nested;
    }
    return nullptr;
}

// Detaches a child block from its parent, returning it by value.
bool takeChild(Block& parent, const Block* child, Block& out)
{
    for (auto it = parent.entries.begin(); it != parent.entries.end(); ++it) {
        if (it->kind != Entry::Kind::ChildBlock || it->child.get() != child) continue;
        out = *it->child;
        parent.entries.erase(it);
        return true;
    }
    return false;
}

bool removeChild(Block& parent, const Block* child)
{
    for (auto it = parent.entries.begin(); it != parent.entries.end(); ++it) {
        if (it->kind != Entry::Kind::ChildBlock || it->child.get() != child) continue;
        parent.entries.erase(it);
        return true;
    }
    return false;
}

// Walks every world solid, entity and group block, calling visit with the
// block, its kind and its id. Entity child solids are deliberately skipped:
// they cannot carry group or visgroup state of their own.
template <typename Document_, typename Block_, typename Visit>
void forEachMapObject(Document_& document, Visit visit)
{
    for (Block_& root : document.roots()) {
        if (isWorldBlock(root)) {
            for (Block_* solid : root.children("solid")) {
                const int id = blockId(*solid);
                if (id >= 0) visit(*solid, MapObjectKind::Solid, id);
            }
            for (Block_* group : root.children("group")) {
                const int id = blockId(*group);
                if (id >= 0) visit(*group, MapObjectKind::Group, id);
            }
        } else if (isEntityBlock(root)) {
            const int id = blockId(root);
            if (id >= 0) visit(root, MapObjectKind::Entity, id);
        }
    }
}

// The ObjectRef a selectable map object maps to, if it is one. Group blocks
// are not selectable and have no ObjectRef.
bool toObjectRef(const MapObjectKey& key, ObjectRef& out)
{
    if (key.kind == MapObjectKind::Group) return false;
    out = {key.kind == MapObjectKind::Solid ? ObjectType::Solid : ObjectType::Entity, key.id};
    return true;
}

MapObjectKey toObjectKey(const ObjectRef& object)
{
    return {object.type == ObjectType::Solid ? MapObjectKind::Solid : MapObjectKind::Entity,
            object.id};
}

// Every id already spoken for anywhere in the document, so a new one collides
// with nothing. Hammer draws group, solid, entity and side ids from one pool.
void collectIds(const Block& block, int& maximum)
{
    if (const std::string* text = block.value("id")) {
        const int id = parseId(text);
        if (id > maximum) maximum = id;
    }
    for (const Block* child : block.children()) collectIds(*child, maximum);
}

void collectVisGroupIds(const Block& block, int& maximum)
{
    if (const std::string* text = block.value("visgroupid")) {
        const int id = parseId(text);
        if (id > maximum) maximum = id;
    }
    for (const Block* child : block.children()) collectVisGroupIds(*child, maximum);
}

} // namespace

// --- MapObjectIndex ---------------------------------------------------------

const MapObjectEntry* MapObjectIndex::find(const MapObjectKey& key) const
{
    for (const MapObjectEntry& entry : objects) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

int MapObjectIndex::topLevelGroup(const MapObjectKey& key) const
{
    const MapObjectEntry* entry = find(key);
    if (!entry || entry->groupId == 0) return 0;
    int groupId = entry->groupId;
    // A hand-edited VMF can point a group at itself; bound the walk by the
    // number of objects so a cycle cannot hang the editor.
    for (std::size_t step = 0; step <= objects.size(); ++step) {
        const MapObjectEntry* group = find({MapObjectKind::Group, groupId});
        if (!group || group->groupId == 0) return groupId;
        groupId = group->groupId;
    }
    return groupId;
}

std::vector<MapObjectKey> MapObjectIndex::groupContents(int groupId) const
{
    std::vector<MapObjectKey> result;
    if (groupId == 0) return result;
    std::vector<int> pending{groupId};
    std::vector<int> visited;
    while (!pending.empty()) {
        const int current = pending.back();
        pending.pop_back();
        if (std::find(visited.begin(), visited.end(), current) != visited.end()) continue;
        visited.push_back(current);
        for (const MapObjectEntry& entry : objects) {
            if (entry.groupId != current) continue;
            result.push_back(entry.key);
            if (entry.key.kind == MapObjectKind::Group) pending.push_back(entry.key.id);
        }
    }
    return result;
}

std::vector<ObjectRef> MapObjectIndex::groupMembers(int groupId) const
{
    std::vector<ObjectRef> result;
    for (const MapObjectKey& key : groupContents(groupId)) {
        ObjectRef object;
        if (toObjectRef(key, object)) result.push_back(object);
    }
    return result;
}

const VisGroupDef* MapObjectIndex::findVisGroup(int visGroupId) const
{
    for (const VisGroupDef& group : visGroups) {
        if (group.id == visGroupId) return &group;
    }
    return nullptr;
}

std::vector<int> MapObjectIndex::visGroupAndDescendants(int visGroupId) const
{
    std::vector<int> result;
    if (visGroupId == 0) return result;
    std::vector<int> pending{visGroupId};
    while (!pending.empty()) {
        const int current = pending.back();
        pending.pop_back();
        if (std::find(result.begin(), result.end(), current) != result.end()) continue;
        result.push_back(current);
        for (const VisGroupDef& group : visGroups) {
            if (group.parentId == current) pending.push_back(group.id);
        }
    }
    return result;
}

std::vector<MapObjectKey> MapObjectIndex::visGroupMembers(int visGroupId) const
{
    // IsInVisGroupRecursive: an object in a nested visgroup counts as a member
    // of every ancestor visgroup.
    const std::vector<int> ids = visGroupAndDescendants(visGroupId);
    std::vector<MapObjectKey> result;
    for (const MapObjectEntry& entry : objects) {
        for (const int id : entry.visGroupIds) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
            result.push_back(entry.key);
            break;
        }
    }
    return result;
}

VisGroupState MapObjectIndex::visGroupState(int visGroupId) const
{
    const std::vector<MapObjectKey> members = visGroupMembers(visGroupId);
    // An empty visgroup reads as shown, the state CVisGroup starts in.
    if (members.empty()) return VisGroupState::Shown;
    bool anyShown = false;
    bool anyHidden = false;
    for (const MapObjectKey& key : members) {
        const MapObjectEntry* entry = find(key);
        if (!entry) continue;
        if (entry->visGroupShown) anyShown = true;
        else anyHidden = true;
    }
    if (anyShown && anyHidden) return VisGroupState::Partial;
    return anyHidden ? VisGroupState::Hidden : VisGroupState::Shown;
}

void MapObjectIndex::hiddenObjects(std::unordered_set<int>& solids,
                                   std::unordered_set<int>& entities) const
{
    for (const MapObjectEntry& entry : objects) {
        // CMapClass::IsVisGroupShown: both halves must allow it.
        if (entry.visGroupShown && entry.visGroupAutoShown) continue;
        if (entry.key.kind == MapObjectKind::Solid) solids.insert(entry.key.id);
        else if (entry.key.kind == MapObjectKind::Entity) entities.insert(entry.key.id);
        else {
            // A hidden group hides everything inside it. Hammer reaches the
            // same place by writing the flag onto the members too, but a group
            // block whose members were edited by hand should still hide them.
            for (const MapObjectKey& key : groupContents(entry.key.id)) {
                if (key.kind == MapObjectKind::Solid) solids.insert(key.id);
                else if (key.kind == MapObjectKind::Entity) entities.insert(key.id);
            }
        }
    }
}

MapObjectIndex indexMapObjects(const Document& document)
{
    MapObjectIndex index;
    forEachMapObject<const Document, const Block>(
        document, [&index](const Block& block, MapObjectKind kind, int id) {
            index.objects.push_back(readObjectEntry(block, kind, id));
        });
    if (const Block* root = document.firstRoot("visgroups")) {
        for (const Block* child : root->children("visgroup")) readVisGroupTree(*child, 0, index.visGroups);
    }
    return index;
}

// --- Block lookup -----------------------------------------------------------

const Block* findObjectBlock(const Document& document, const MapObjectKey& key)
{
    const Block* found = nullptr;
    forEachMapObject<const Document, const Block>(
        document, [&](const Block& block, MapObjectKind kind, int id) {
            if (!found && kind == key.kind && id == key.id) found = &block;
        });
    return found;
}

Block* findObjectBlock(Document& document, const MapObjectKey& key)
{
    Block* found = nullptr;
    forEachMapObject<Document, Block>(document, [&](Block& block, MapObjectKind kind, int id) {
        if (!found && kind == key.kind && id == key.id) found = &block;
    });
    return found;
}

Block& objectEditorBlock(Block& object)
{
    for (Block* child : object.children("editor")) return *child;
    return object.appendChild("editor");
}

int maximumObjectId(const Document& document)
{
    int maximum = 0;
    for (const Block& root : document.roots()) collectIds(root, maximum);
    return maximum;
}

int maximumVisGroupId(const Document& document)
{
    int maximum = 0;
    for (const Block& root : document.roots()) collectVisGroupIds(root, maximum);
    return maximum;
}

// --- Groups -----------------------------------------------------------------

int createGroup(Document& document, const std::vector<ObjectRef>& members, int newId)
{
    if (members.empty() || newId <= 0) return 0;
    Block* world = document.firstRoot("world");
    if (!world) return 0;

    // Stamp the members first: if none of them exists there is no group to make.
    int stamped = 0;
    const std::string groupIdText = std::to_string(newId);
    for (const ObjectRef& member : members) {
        Block* block = findObjectBlock(document, toObjectKey(member));
        if (!block) continue;
        // OnToolsGroup detaches members from whatever group they were in.
        objectEditorBlock(*block).setValue("groupid", groupIdText);
        ++stamped;
    }
    if (stamped == 0) return 0;

    Block& group = world->appendChild("group");
    group.setValue("id", groupIdText);
    Block& editor = group.appendChild("editor");
    // CMapGroup gets a random olive-ish render color in OnToolsGroup; a fixed
    // one keeps the write deterministic.
    editor.setValue("color", "0 180 0");
    editor.setValue("visgroupshown", "1");
    editor.setValue("visgroupautoshown", "1");
    document.markDirty();
    return newId;
}

bool ungroup(Document& document, const std::vector<int>& groupIds)
{
    if (groupIds.empty()) return false;
    const MapObjectIndex index = indexMapObjects(document);
    bool changed = false;
    for (const int groupId : groupIds) {
        const MapObjectEntry* group = index.find({MapObjectKind::Group, groupId});
        if (!group) continue;
        // One level only: the direct children move up to the group's own
        // parent group, which is 0 when the group was top level.
        const std::string parentText = std::to_string(group->groupId);
        for (const MapObjectEntry& entry : index.objects) {
            if (entry.groupId != groupId) continue;
            Block* block = findObjectBlock(document, entry.key);
            if (!block) continue;
            Block& editor = objectEditorBlock(*block);
            if (group->groupId == 0) editor.removeValues("groupid");
            else editor.setValue("groupid", parentText);
            changed = true;
        }
        Block* world = document.firstRoot("world");
        const Block* block = findObjectBlock(document, {MapObjectKind::Group, groupId});
        if (world && block && removeChild(*world, block)) changed = true;
    }
    if (changed) document.markDirty();
    return changed;
}

bool purgeEmptyGroups(Document& document)
{
    bool changed = false;
    // Removing one group can empty its parent, so repeat until stable.
    for (;;) {
        const MapObjectIndex index = indexMapObjects(document);
        int emptyId = 0;
        for (const MapObjectEntry& entry : index.objects) {
            if (entry.key.kind != MapObjectKind::Group) continue;
            if (!index.groupContents(entry.key.id).empty()) continue;
            emptyId = entry.key.id;
            break;
        }
        if (emptyId == 0) break;
        Block* world = document.firstRoot("world");
        const Block* block = findObjectBlock(document, {MapObjectKind::Group, emptyId});
        if (!world || !block || !removeChild(*world, block)) break;
        changed = true;
    }
    if (changed) document.markDirty();
    return changed;
}

// --- VisGroups --------------------------------------------------------------

int createVisGroup(Document& document, const std::string& name, int newId, int parentId)
{
    if (newId <= 0) return 0;
    Block* root = visGroupsRoot(document, true);
    if (!root) return 0;
    Block* parent = root;
    if (parentId > 0) {
        if (Block* found = findVisGroupBlock(*root, parentId, nullptr)) parent = found;
    }
    Block& group = parent->appendChild("visgroup");
    group.setValue("name", name);
    group.setValue("visgroupid", std::to_string(newId));
    group.setValue("color", "192 192 192");
    document.markDirty();
    return newId;
}

bool renameVisGroup(Document& document, int visGroupId, const std::string& name)
{
    Block* root = visGroupsRoot(document, false);
    if (!root) return false;
    Block* group = findVisGroupBlock(*root, visGroupId, nullptr);
    if (!group) return false;
    const std::string* existing = group->value("name");
    if (existing && *existing == name) return false;
    group->setValue("name", name);
    document.markDirty();
    return true;
}

bool setVisGroupColor(Document& document, int visGroupId, int red, int green, int blue)
{
    Block* root = visGroupsRoot(document, false);
    if (!root) return false;
    Block* group = findVisGroupBlock(*root, visGroupId, nullptr);
    if (!group) return false;
    const std::string color = std::to_string(std::clamp(red, 0, 255)) + " " +
                              std::to_string(std::clamp(green, 0, 255)) + " " +
                              std::to_string(std::clamp(blue, 0, 255));
    const std::string* existing = group->value("color");
    if (existing && *existing == color) return false;
    group->setValue("color", color);
    document.markDirty();
    return true;
}

bool setVisGroupParent(Document& document, int visGroupId, int parentId)
{
    if (visGroupId == parentId) return false;
    Block* root = visGroupsRoot(document, false);
    if (!root) return false;
    // Re-parenting a visgroup under its own descendant would detach the subtree
    // from the tree entirely.
    const MapObjectIndex index = indexMapObjects(document);
    if (parentId > 0) {
        const std::vector<int> descendants = index.visGroupAndDescendants(visGroupId);
        if (std::find(descendants.begin(), descendants.end(), parentId) != descendants.end())
            return false;
    }
    Block* owner = nullptr;
    Block* group = findVisGroupBlock(*root, visGroupId, &owner);
    if (!group || !owner) return false;
    Block detached;
    if (!takeChild(*owner, group, detached)) return false;
    Block* parent = root;
    if (parentId > 0) {
        if (Block* found = findVisGroupBlock(*root, parentId, nullptr)) parent = found;
    }
    parent->entries.emplace_back(std::move(detached));
    document.markDirty();
    return true;
}

bool moveVisGroup(Document& document, int visGroupId, bool up)
{
    Block* root = visGroupsRoot(document, false);
    if (!root) return false;
    Block* owner = nullptr;
    Block* group = findVisGroupBlock(*root, visGroupId, &owner);
    if (!group || !owner) return false;
    // CVisGroup::MoveUp/MoveDown reorder among siblings only, so the swap is
    // between visgroup entries and must skip any keyvalues in between.
    std::vector<std::size_t> siblings;
    for (std::size_t index = 0; index < owner->entries.size(); ++index) {
        const Entry& entry = owner->entries[index];
        if (entry.kind == Entry::Kind::ChildBlock && entry.child &&
            equalsIgnoreCaseAscii(entry.child->name, "visgroup")) {
            siblings.push_back(index);
        }
    }
    const auto position = std::find_if(siblings.begin(), siblings.end(),
                                       [&](std::size_t index) {
                                           return owner->entries[index].child.get() == group;
                                       });
    if (position == siblings.end()) return false;
    const std::size_t slot = static_cast<std::size_t>(std::distance(siblings.begin(), position));
    if (up && slot == 0) return false;
    if (!up && slot + 1 >= siblings.size()) return false;
    std::swap(owner->entries[siblings[slot]], owner->entries[siblings[up ? slot - 1 : slot + 1]]);
    document.markDirty();
    return true;
}

bool deleteVisGroup(Document& document, int visGroupId)
{
    Block* root = visGroupsRoot(document, false);
    if (!root) return false;
    const MapObjectIndex index = indexMapObjects(document);
    const VisGroupDef* def = index.findVisGroup(visGroupId);
    if (!def) return false;
    // Children move up to the deleted visgroup's own parent rather than
    // disappearing with it - deleting a visgroup never deletes anything.
    for (const VisGroupDef& child : index.visGroups) {
        if (child.parentId == visGroupId) setVisGroupParent(document, child.id, def->parentId);
    }
    Block* owner = nullptr;
    Block* group = findVisGroupBlock(*root, visGroupId, &owner);
    if (group && owner) removeChild(*owner, group);

    const std::string idText = std::to_string(visGroupId);
    forEachMapObject<Document, Block>(document, [&](Block& block, MapObjectKind, int) {
        for (Block* editor : block.children("editor")) {
            const std::vector<std::string> ids = editor->values("visgroupid");
            if (std::find(ids.begin(), ids.end(), idText) == ids.end()) continue;
            editor->removeValues("visgroupid");
            for (const std::string& id : ids) {
                if (id != idText) editor->appendValue("visgroupid", id);
            }
        }
    });
    document.markDirty();
    return true;
}

bool addObjectsToVisGroup(Document& document, const std::vector<ObjectRef>& objects,
                          int visGroupId, bool removeFromOtherVisGroups)
{
    if (objects.empty() || visGroupId <= 0) return false;
    const std::string idText = std::to_string(visGroupId);
    bool changed = false;
    for (const ObjectRef& object : objects) {
        Block* block = findObjectBlock(document, toObjectKey(object));
        if (!block) continue;
        Block& editor = objectEditorBlock(*block);
        std::vector<std::string> ids = editor.values("visgroupid");
        if (removeFromOtherVisGroups) {
            if (ids.size() == 1 && ids.front() == idText) continue;
            editor.removeValues("visgroupid");
            editor.appendValue("visgroupid", idText);
            changed = true;
            continue;
        }
        if (std::find(ids.begin(), ids.end(), idText) != ids.end()) continue;
        editor.appendValue("visgroupid", idText);
        changed = true;
    }
    if (changed) document.markDirty();
    return changed;
}

bool removeObjectsFromVisGroup(Document& document, const std::vector<ObjectRef>& objects,
                               int visGroupId)
{
    if (objects.empty() || visGroupId <= 0) return false;
    const std::string idText = std::to_string(visGroupId);
    bool changed = false;
    for (const ObjectRef& object : objects) {
        Block* block = findObjectBlock(document, toObjectKey(object));
        if (!block) continue;
        for (Block* editor : block->children("editor")) {
            const std::vector<std::string> ids = editor->values("visgroupid");
            if (std::find(ids.begin(), ids.end(), idText) == ids.end()) continue;
            editor->removeValues("visgroupid");
            for (const std::string& id : ids) {
                if (id != idText) editor->appendValue("visgroupid", id);
            }
            changed = true;
        }
    }
    if (changed) document.markDirty();
    return changed;
}

bool purgeEmptyVisGroups(Document& document)
{
    bool changed = false;
    for (;;) {
        const MapObjectIndex index = indexMapObjects(document);
        int emptyId = 0;
        for (const VisGroupDef& group : index.visGroups) {
            if (group.automatic) continue;
            // A visgroup that only parents other visgroups is still useful.
            if (!index.visGroupMembers(group.id).empty()) continue;
            const bool hasChildren =
                std::any_of(index.visGroups.begin(), index.visGroups.end(),
                            [&group](const VisGroupDef& other) { return other.parentId == group.id; });
            if (hasChildren) continue;
            emptyId = group.id;
            break;
        }
        if (emptyId == 0) break;
        if (!deleteVisGroup(document, emptyId)) break;
        changed = true;
    }
    return changed;
}

// --- Visibility -------------------------------------------------------------

namespace {

// CMapClass::VisGroupShow with eVisGroup == USER. Showing through a user
// visgroup ALSO clears the auto-hidden flag ("since user visgroup visibility
// has precedence over auto, it is possible to change an object's auto
// visibility through an action in a user visgroup"); hiding leaves it alone.
bool writeShownFlag(Block& object, bool shown)
{
    Block& editor = objectEditorBlock(object);
    bool changed = false;
    const std::string text = shown ? "1" : "0";
    const std::string* existing = editor.value("visgroupshown");
    if (!existing || *existing != text) {
        editor.setValue("visgroupshown", text);
        changed = true;
    }
    if (shown) {
        const std::string* autoShown = editor.value("visgroupautoshown");
        if (!autoShown || *autoShown != "1") {
            editor.setValue("visgroupautoshown", "1");
            changed = true;
        }
    }
    return changed;
}

} // namespace

bool showVisGroup(Document& document, int visGroupId, bool show)
{
    const MapObjectIndex index = indexMapObjects(document);
    const std::vector<MapObjectKey> members = index.visGroupMembers(visGroupId);
    bool changed = false;
    for (const MapObjectKey& key : members) {
        Block* block = findObjectBlock(document, key);
        if (!block) continue;
        if (writeShownFlag(*block, show)) changed = true;
        // A group carries its members with it, exactly as CMapGroup forwards
        // visibility to its children.
        if (key.kind != MapObjectKind::Group) continue;
        for (const MapObjectKey& child : index.groupContents(key.id)) {
            if (Block* childBlock = findObjectBlock(document, child)) {
                if (writeShownFlag(*childBlock, show)) changed = true;
            }
        }
    }
    if (changed) document.markDirty();
    return changed;
}

bool setObjectsVisGroupShown(Document& document, const std::vector<ObjectRef>& objects, bool shown)
{
    if (objects.empty()) return false;
    const MapObjectIndex index = indexMapObjects(document);
    bool changed = false;
    for (const ObjectRef& object : objects) {
        const MapObjectKey key = toObjectKey(object);
        if (Block* block = findObjectBlock(document, key)) {
            if (writeShownFlag(*block, shown)) changed = true;
        }
    }
    if (changed) document.markDirty();
    return changed;
}

// --- Auto VisGroups ---------------------------------------------------------

const std::vector<AutoVisGroupDef>& autoVisGroupTable()
{
    // Parents before children, so a single pass can build the tree.
    static const std::vector<AutoVisGroupDef> table = {
        {AutoVisGroup::Entities, "Entities", AutoVisGroup::None},
        {AutoVisGroup::WorldGeometry, "World Geometry", AutoVisGroup::None},
        {AutoVisGroup::WorldDetails, "World Details", AutoVisGroup::None},
        {AutoVisGroup::ToolBrushes, "Tool Brushes", AutoVisGroup::None},

        {AutoVisGroup::PointEntities, "Point Entities", AutoVisGroup::Entities},
        {AutoVisGroup::Nodes, "Nodes", AutoVisGroup::Entities},
        {AutoVisGroup::NPCs, "NPCs", AutoVisGroup::Entities},
        {AutoVisGroup::Lights, "Lights", AutoVisGroup::Entities},
        {AutoVisGroup::BrushEntities, "Brush Entities", AutoVisGroup::Entities},
        {AutoVisGroup::Triggers, "Triggers", AutoVisGroup::Entities},

        {AutoVisGroup::Props, "Props", AutoVisGroup::WorldDetails},
        {AutoVisGroup::FuncDetail, "Func Detail", AutoVisGroup::WorldDetails},

        {AutoVisGroup::Displacements, "Displacements", AutoVisGroup::WorldGeometry},
        {AutoVisGroup::Water, "Water", AutoVisGroup::WorldGeometry},
        {AutoVisGroup::Nodraw, "Nodraw", AutoVisGroup::WorldGeometry},
        {AutoVisGroup::Sky, "Sky", AutoVisGroup::WorldGeometry},
        {AutoVisGroup::Black, "Black", AutoVisGroup::WorldGeometry},

        {AutoVisGroup::Areaportals, "Areaportals", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::Occluders, "Occluders", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::ToolOccluder, "Occluder", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::ToolAreaPortal, "Area Portal", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::Skip, "Skip", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::ToolTrigger, "Trigger", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::Origin, "Origin", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::Hint, "Hint", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::Fog, "Fog", AutoVisGroup::ToolBrushes},

        {AutoVisGroup::Block, "Block", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::BlockLos, "LOS", AutoVisGroup::Block},
        {AutoVisGroup::BlockBullets, "Bullets", AutoVisGroup::Block},
        {AutoVisGroup::BlockLight, "Light", AutoVisGroup::Block},

        {AutoVisGroup::Clips, "Clips", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::ClipNpc, "NPC", AutoVisGroup::Clips},
        {AutoVisGroup::ClipPlayer, "Player", AutoVisGroup::Clips},
        {AutoVisGroup::ClipControl, "Control", AutoVisGroup::Clips},
        {AutoVisGroup::Clip, "Clip", AutoVisGroup::Clips},

        {AutoVisGroup::Invisible, "Invisible", AutoVisGroup::ToolBrushes},
        {AutoVisGroup::InvisibleLadder, "Ladder", AutoVisGroup::Invisible},
        {AutoVisGroup::InvisibleInvisible, "Invisible", AutoVisGroup::Invisible},
    };
    return table;
}

const AutoVisGroupDef* findAutoVisGroup(AutoVisGroup id)
{
    for (const AutoVisGroupDef& def : autoVisGroupTable()) {
        if (def.id == id) return &def;
    }
    return nullptr;
}

namespace {

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size()) return false;
    return equalsIgnoreCaseAscii(text.substr(0, prefix.size()), prefix);
}

std::string lowerCopy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool containsText(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

void addCategory(std::vector<AutoVisGroup>& out, AutoVisGroup id)
{
    if (id == AutoVisGroup::None) return;
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
}

// GDclass::IsNodeClass: info_node* but not info_node_link.
bool isNodeClass(std::string_view classname)
{
    return startsWithIgnoreCase(classname, "info_node") &&
           !equalsIgnoreCaseAscii(classname, "info_node_link");
}

// The per-face half of AddToAutoVisGroup for one world solid. The reference
// walks the faces and uses "continue" between the tool-material tests, so the
// ORDER here is load-bearing: Block is tested before the generic clip check,
// and the nested Block/Clips/Invisible parents are added even when no child
// matches.
void classifySolidFaces(const Block& solid, std::vector<AutoVisGroup>& out,
                        const WaterMaterialPredicate& isWaterMaterial)
{
    bool waterAdded = false;
    for (const Block* side : solid.children("side")) {
        const std::string* material = side->value("material");
        if (!material) continue;
        const std::string name = lowerCopy(*material);

        // A solid with water on several faces is only added once.
        if (!waterAdded && isWaterMaterial && isWaterMaterial(*material)) {
            waterAdded = true;
            addCategory(out, AutoVisGroup::Water);
        }

        if (!containsText(name, "tools/tools")) continue;

        if (containsText(name, "tools/toolsnodraw")) {
            addCategory(out, AutoVisGroup::Nodraw);
            continue;
        }
        if (containsText(name, "tools/toolssky")) {
            addCategory(out, AutoVisGroup::Sky);
            continue;
        }
        if (containsText(name, "tools/toolsblock")) {
            // The parent appears as soon as the branch is entered, matching
            // AddChildGroupToAutoVisGroup(NULL, "Block", "Tool Brushes").
            addCategory(out, AutoVisGroup::Block);
            if (containsText(name, "block_los")) {
                addCategory(out, AutoVisGroup::BlockLos);
                continue;
            }
            if (containsText(name, "blockbullets")) {
                addCategory(out, AutoVisGroup::BlockBullets);
                continue;
            }
            if (containsText(name, "blocklight")) {
                addCategory(out, AutoVisGroup::BlockLight);
                continue;
            }
        }
        if (containsText(name, "clip")) {
            addCategory(out, AutoVisGroup::Clips);
            if (containsText(name, "npcclip")) {
                addCategory(out, AutoVisGroup::ClipNpc);
                continue;
            }
            if (containsText(name, "playerclip")) {
                addCategory(out, AutoVisGroup::ClipPlayer);
                continue;
            }
            if (containsText(name, "controlclip")) {
                addCategory(out, AutoVisGroup::ClipControl);
                continue;
            }
            addCategory(out, AutoVisGroup::Clip);
            continue;
        }
        if (containsText(name, "occluder")) {
            addCategory(out, AutoVisGroup::ToolOccluder);
            continue;
        }
        if (containsText(name, "areaportal")) {
            addCategory(out, AutoVisGroup::ToolAreaPortal);
            continue;
        }
        if (containsText(name, "invisible")) {
            addCategory(out, AutoVisGroup::Invisible);
            if (containsText(name, "invisibleladder")) {
                addCategory(out, AutoVisGroup::InvisibleLadder);
                continue;
            }
            addCategory(out, AutoVisGroup::InvisibleInvisible);
            continue;
        }
        if (containsText(name, "skip")) {
            addCategory(out, AutoVisGroup::Skip);
            continue;
        }
        if (containsText(name, "trigger")) {
            addCategory(out, AutoVisGroup::ToolTrigger);
            continue;
        }
        if (containsText(name, "origin")) {
            addCategory(out, AutoVisGroup::Origin);
            continue;
        }
        if (containsText(name, "hint")) {
            addCategory(out, AutoVisGroup::Hint);
            continue;
        }
        if (containsText(name, "fog")) {
            addCategory(out, AutoVisGroup::Fog);
            continue;
        }
        if (containsText(name, "black")) {
            addCategory(out, AutoVisGroup::Black);
            continue;
        }
    }
}

bool solidHasDisplacement(const Block& solid)
{
    for (const Block* side : solid.children("side")) {
        if (!side->children("dispinfo").empty()) return true;
    }
    return false;
}

} // namespace

namespace {

std::vector<AutoVisGroup> classifyBlockForAutoVisGroups(
    const Block* block, const MapObjectKey& key,
    const std::unordered_set<std::string>& npcClasses,
    const WaterMaterialPredicate& isWaterMaterial)
{
    std::vector<AutoVisGroup> out;
    if (!block) return out;

    if (key.kind == MapObjectKind::Entity) {
        const std::string* classnameValue = block->value("classname");
        const std::string classname = classnameValue ? *classnameValue : std::string{};
        // CMapEntity::IsPointClass is "has no solid children" as far as the VMF
        // is concerned, which is also how the Entity Report decides it.
        const bool pointClass = block->children("solid").empty();

        if (pointClass) {
            if (!isNodeClass(classname)) addCategory(out, AutoVisGroup::PointEntities);
            else addCategory(out, AutoVisGroup::Nodes);

            if (npcClasses.contains(lowerCopy(classname))) addCategory(out, AutoVisGroup::NPCs);
            if (startsWithIgnoreCase(classname, "light_")) addCategory(out, AutoVisGroup::Lights);
            // Props land under World Details, not Entities.
            if (startsWithIgnoreCase(classname, "prop_")) addCategory(out, AutoVisGroup::Props);
        } else {
            if (!equalsIgnoreCaseAscii(classname, "func_detail"))
                addCategory(out, AutoVisGroup::BrushEntities);
            else addCategory(out, AutoVisGroup::FuncDetail);

            if (startsWithIgnoreCase(classname, "trigger_"))
                addCategory(out, AutoVisGroup::Triggers);
            if (startsWithIgnoreCase(classname, "func_areaportal"))
                addCategory(out, AutoVisGroup::Areaportals);
            if (equalsIgnoreCaseAscii(classname, "func_occluder"))
                addCategory(out, AutoVisGroup::Occluders);
        }
        return out;
    }

    if (key.kind != MapObjectKind::Solid) return out; // groups classify their members

    if (solidHasDisplacement(*block)) {
        addCategory(out, AutoVisGroup::Displacements);
    } else {
        // Only a solid with no entity parent is plain World Geometry. Solids
        // inside a brush entity never reach here - indexMapObjects does not
        // list them (VisGroups_ObjectCanBelongToVisGroup).
        addCategory(out, AutoVisGroup::WorldGeometry);
    }
    classifySolidFaces(*block, out, isWaterMaterial);
    return out;
}

} // namespace

std::vector<AutoVisGroup> classifyForAutoVisGroups(
    const Document& document, const MapObjectKey& key,
    const std::unordered_set<std::string>& npcClasses,
    const WaterMaterialPredicate& isWaterMaterial)
{
    return classifyBlockForAutoVisGroups(findObjectBlock(document, key), key, npcClasses,
                                         isWaterMaterial);
}

std::vector<MapObjectKey> AutoVisGroupIndex::objectsIn(AutoVisGroup id) const
{
    // A category holds everything in it and in its descendants, the same way a
    // nested user visgroup's members belong to its ancestors.
    std::vector<AutoVisGroup> wanted{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const AutoVisGroupDef& def : autoVisGroupTable()) {
            if (std::find(wanted.begin(), wanted.end(), def.parent) == wanted.end()) continue;
            if (std::find(wanted.begin(), wanted.end(), def.id) != wanted.end()) continue;
            wanted.push_back(def.id);
            grew = true;
        }
    }
    std::vector<MapObjectKey> result;
    for (const auto& [key, categories] : members) {
        for (const AutoVisGroup category : categories) {
            if (std::find(wanted.begin(), wanted.end(), category) == wanted.end()) continue;
            result.push_back(key);
            break;
        }
    }
    return result;
}

AutoVisGroupIndex indexAutoVisGroups(const Document& document, const MapObjectIndex& index,
                                     const std::unordered_set<std::string>& npcClasses,
                                     const WaterMaterialPredicate& isWaterMaterial)
{
    AutoVisGroupIndex result;
    // One document walk builds the key -> block map; the per-object classify
    // is then a lookup. Looking each object up with findObjectBlock (a full
    // document walk per object) made this quadratic: ~220 ms at 2000 solids
    // and ~2 s at 6000, on every full scene rebuild - map load, undo, every
    // visgroup toggle. First occurrence wins on a duplicate id, exactly as
    // findObjectBlock's "if (!found)" resolved it.
    std::unordered_map<MapObjectKey, const Block*, MapObjectKeyHash> blocks;
    blocks.reserve(index.objects.size());
    forEachMapObject<const Document, const Block>(
        document, [&blocks](const Block& block, MapObjectKind kind, int id) {
            blocks.try_emplace(MapObjectKey{kind, id}, &block);
        });
    for (const MapObjectEntry& entry : index.objects) {
        if (entry.key.kind == MapObjectKind::Group) continue;
        const auto found = blocks.find(entry.key);
        std::vector<AutoVisGroup> categories = classifyBlockForAutoVisGroups(
            found == blocks.end() ? nullptr : found->second, entry.key, npcClasses,
            isWaterMaterial);
        if (categories.empty()) continue;
        for (const AutoVisGroup category : categories) {
            // A populated category makes its whole ancestor chain visible in
            // the tree, so "Nodraw" brings "World Geometry" with it.
            for (AutoVisGroup walk = category; walk != AutoVisGroup::None;) {
                result.present.insert(static_cast<int>(walk));
                const AutoVisGroupDef* def = findAutoVisGroup(walk);
                walk = def ? def->parent : AutoVisGroup::None;
            }
        }
        result.members.emplace_back(entry.key, std::move(categories));
    }
    return result;
}

bool showAutoVisGroup(Document& document, const std::vector<MapObjectKey>& objects, bool show)
{
    bool changed = false;
    // Same one-walk lookup map as indexAutoVisGroups: a per-key findObjectBlock
    // made toggling a populated category quadratic in the map size.
    std::unordered_map<MapObjectKey, Block*, MapObjectKeyHash> blocks;
    forEachMapObject<Document, Block>(document, [&blocks](Block& block, MapObjectKind kind, int id) {
        blocks.try_emplace(MapObjectKey{kind, id}, &block);
    });
    for (const MapObjectKey& key : objects) {
        const auto found = blocks.find(key);
        Block* block = found == blocks.end() ? nullptr : found->second;
        if (!block) continue;
        Block& editor = objectEditorBlock(*block);
        const std::string text = show ? "1" : "0";
        const std::string* existing = editor.value("visgroupautoshown");
        if (existing && *existing == text) continue;
        editor.setValue("visgroupautoshown", text);
        changed = true;
    }
    if (changed) document.markDirty();
    return changed;
}

bool showAllObjects(Document& document)
{
    bool changed = false;
    forEachMapObject<Document, Block>(document, [&](Block& block, MapObjectKind, int) {
        if (writeShownFlag(block, true)) changed = true;
    });
    if (changed) document.markDirty();
    return changed;
}

} // namespace hammer::vmf
