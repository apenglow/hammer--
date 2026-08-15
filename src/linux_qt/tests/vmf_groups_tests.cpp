// Grouping and VisGrouping model tests (VmfGroups.hpp).
//
// The release build compiles asserts out, so every check here goes through
// require() and a non-zero exit code.

#include "VmfDocument.hpp"
#include "VmfGroups.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>

using hammer::vmf::Block;
using hammer::vmf::Document;
using hammer::vmf::MapObjectKey;
using hammer::vmf::MapObjectKind;
using hammer::vmf::ObjectRef;
using hammer::vmf::ObjectType;
using hammer::vmf::VisGroupState;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// A map with: two world solids in a group, a third loose world solid, a point
// entity, a brush entity, nested visgroups, and one solid in two visgroups.
const char* GroupedVmf =
    "versioninfo\n"
    "{\n"
    "\t\"editorversion\" \"400\"\n"
    "}\n"
    "visgroups\n"
    "{\n"
    "\tvisgroup\n"
    "\t{\n"
    "\t\t\"name\" \"Furniture\"\n"
    "\t\t\"visgroupid\" \"1\"\n"
    "\t\t\"color\" \"255 0 0\"\n"
    "\t\tvisgroup\n"
    "\t\t{\n"
    "\t\t\t\"name\" \"Chairs\"\n"
    "\t\t\t\"visgroupid\" \"2\"\n"
    "\t\t\t\"color\" \"0 255 0\"\n"
    "\t\t}\n"
    "\t}\n"
    "\tvisgroup\n"
    "\t{\n"
    "\t\t\"name\" \"Lights\"\n"
    "\t\t\"visgroupid\" \"3\"\n"
    "\t\t\"color\" \"0 0 255\"\n"
    "\t}\n"
    "}\n"
    "world\n"
    "{\n"
    "\t\"id\" \"1\"\n"
    "\t\"classname\" \"worldspawn\"\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"10\"\n"
    "\t\teditor\n"
    "\t\t{\n"
    "\t\t\t\"color\" \"0 180 0\"\n"
    "\t\t\t\"groupid\" \"50\"\n"
    "\t\t\t\"visgroupid\" \"2\"\n"
    "\t\t\t\"visgroupid\" \"3\"\n"
    "\t\t\t\"visgroupshown\" \"1\"\n"
    "\t\t\t\"visgroupautoshown\" \"1\"\n"
    "\t\t}\n"
    "\t}\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"11\"\n"
    "\t\teditor\n"
    "\t\t{\n"
    "\t\t\t\"color\" \"0 180 0\"\n"
    "\t\t\t\"groupid\" \"50\"\n"
    "\t\t\t\"visgroupshown\" \"1\"\n"
    "\t\t}\n"
    "\t}\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"12\"\n"
    "\t\teditor\n"
    "\t\t{\n"
    "\t\t\t\"color\" \"0 180 0\"\n"
    "\t\t\t\"visgroupshown\" \"1\"\n"
    "\t\t}\n"
    "\t}\n"
    "\tgroup\n"
    "\t{\n"
    "\t\t\"id\" \"50\"\n"
    "\t\teditor\n"
    "\t\t{\n"
    "\t\t\t\"color\" \"0 180 0\"\n"
    "\t\t\t\"visgroupshown\" \"1\"\n"
    "\t\t}\n"
    "\t}\n"
    "}\n"
    "entity\n"
    "{\n"
    "\t\"id\" \"20\"\n"
    "\t\"classname\" \"light\"\n"
    "\t\"origin\" \"0 0 64\"\n"
    "\teditor\n"
    "\t{\n"
    "\t\t\"color\" \"220 30 220\"\n"
    "\t\t\"visgroupid\" \"3\"\n"
    "\t\t\"visgroupshown\" \"1\"\n"
    "\t}\n"
    "}\n"
    "entity\n"
    "{\n"
    "\t\"id\" \"21\"\n"
    "\t\"classname\" \"func_detail\"\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"13\"\n"
    "\t\teditor\n"
    "\t\t{\n"
    "\t\t\t\"color\" \"0 180 0\"\n"
    "\t\t}\n"
    "\t}\n"
    "\teditor\n"
    "\t{\n"
    "\t\t\"color\" \"0 180 0\"\n"
    "\t\t\"visgroupshown\" \"1\"\n"
    "\t}\n"
    "}\n";

Document parseOrDie(const char* text)
{
    hammer::vmf::ParseError error;
    auto document = Document::parse(text, &error);
    require(document.has_value(), "the fixture VMF parses");
    return std::move(*document);
}

const ObjectRef Solid10{ObjectType::Solid, 10};
const ObjectRef Solid11{ObjectType::Solid, 11};
const ObjectRef Solid12{ObjectType::Solid, 12};
const ObjectRef Light20{ObjectType::Entity, 20};

bool contains(const std::vector<ObjectRef>& objects, const ObjectRef& wanted)
{
    return std::find(objects.begin(), objects.end(), wanted) != objects.end();
}

void testIndexing()
{
    const Document document = parseOrDie(GroupedVmf);
    const auto index = hammer::vmf::indexMapObjects(document);

    // World solids, the group block and both entities are indexed; the solid
    // inside the brush entity is not - it cannot carry visgroup state.
    require(index.objects.size() == 6, "world solids, the group and both entities are indexed");
    require(index.find({MapObjectKind::Solid, 13}) == nullptr,
            "a brush entity's child solid is not a visgroup-capable object");
    require(index.find({MapObjectKind::Group, 50}) != nullptr, "the group block is indexed");

    const auto* solid10 = index.find({MapObjectKind::Solid, 10});
    require(solid10 != nullptr, "solid 10 is indexed");
    require(solid10->groupId == 50, "groupid is read from the editor block");
    require(solid10->visGroupIds.size() == 2,
            "a repeated visgroupid key yields multiple memberships");
    require(solid10->visGroupShown, "visgroupshown defaults on");

    require(index.visGroups.size() == 3, "all three visgroups are read");
    const auto* chairs = index.findVisGroup(2);
    require(chairs && chairs->parentId == 1, "a nested visgroup records its parent");
    require(chairs && chairs->name == "Chairs" && chairs->green == 255,
            "visgroup name and color are read");

    // Grouping
    require(index.topLevelGroup({MapObjectKind::Solid, 10}) == 50, "a member resolves to its group");
    require(index.topLevelGroup({MapObjectKind::Solid, 12}) == 0, "a loose solid is in no group");
    const std::vector<ObjectRef> members = index.groupMembers(50);
    require(members.size() == 2 && contains(members, Solid10) && contains(members, Solid11),
            "the group holds exactly its two solids");

    // IsInVisGroupRecursive: solid 10 is in "Chairs", so it is a member of
    // "Furniture" too.
    const auto furnitureMembers = index.visGroupMembers(1);
    require(furnitureMembers.size() == 1 && furnitureMembers.front().id == 10,
            "a nested visgroup's members belong to the ancestor as well");
    require(index.visGroupMembers(3).size() == 2, "Lights holds the solid and the light entity");
}

void testNestedGroups()
{
    Document document = parseOrDie(GroupedVmf);
    // Put group 50 inside a new group 51 that also holds the loose solid.
    require(hammer::vmf::createGroup(document, {Solid12}, 51) == 51, "a group is created");
    Block* outer = hammer::vmf::findObjectBlock(document, {MapObjectKind::Group, 51});
    require(outer != nullptr, "the new group block exists");
    Block* inner = hammer::vmf::findObjectBlock(document, {MapObjectKind::Group, 50});
    require(inner != nullptr, "the original group survives");
    hammer::vmf::objectEditorBlock(*inner).setValue("groupid", "51");

    const auto index = hammer::vmf::indexMapObjects(document);
    require(index.topLevelGroup({MapObjectKind::Solid, 10}) == 51,
            "a nested member resolves to the OUTERMOST group");
    const std::vector<ObjectRef> members = index.groupMembers(51);
    require(members.size() == 3, "the outer group reaches through the nested group");

    // Ungroup strips one level only: 50 survives, its members stay in it.
    require(hammer::vmf::ungroup(document, {51}), "ungroup reports a change");
    const auto after = hammer::vmf::indexMapObjects(document);
    require(after.find({MapObjectKind::Group, 51}) == nullptr, "the ungrouped block is gone");
    require(after.find({MapObjectKind::Group, 50}) != nullptr,
            "ungroup strips one level and leaves nested groups intact");
    require(after.topLevelGroup({MapObjectKind::Solid, 10}) == 50,
            "the inner group's members are still grouped");
    require(after.topLevelGroup({MapObjectKind::Solid, 12}) == 0,
            "a direct member of the ungrouped group is now loose");
}

void testVisibilityDirection()
{
    Document document = parseOrDie(GroupedVmf);
    const auto before = hammer::vmf::indexMapObjects(document);
    require(before.visGroupState(3) == VisGroupState::Shown, "everything starts shown");

    // Hiding "Lights" writes the flag onto its two members.
    require(hammer::vmf::showVisGroup(document, 3, false), "hiding a visgroup changes the document");
    auto index = hammer::vmf::indexMapObjects(document);
    require(!index.find({MapObjectKind::Solid, 10})->visGroupShown,
            "the member's own visgroupshown flag is what gets written");
    require(!index.find({MapObjectKind::Entity, 20})->visGroupShown, "both members are hidden");
    require(index.visGroupState(3) == VisGroupState::Hidden, "the visgroup reads back hidden");

    // Solid 10 is also in "Chairs"/"Furniture", which now read partial-or-
    // hidden purely from their members - the object flag is the source of
    // truth, not the visgroup state.
    require(index.visGroupState(2) == VisGroupState::Hidden,
            "Chairs derives its state from its one hidden member");

    std::unordered_set<int> hiddenSolids;
    std::unordered_set<int> hiddenEntities;
    index.hiddenObjects(hiddenSolids, hiddenEntities);
    require(hiddenSolids.count(10) == 1 && hiddenEntities.count(20) == 1,
            "hidden objects are reported for the scene filter");
    require(hiddenSolids.count(11) == 0, "an unaffected member of the same group stays visible");

    // Showing "Chairs" un-hides solid 10 even though "Lights" was the group
    // that hid it: last toggle wins, per member flag.
    require(hammer::vmf::showVisGroup(document, 2, true), "showing a visgroup changes the document");
    index = hammer::vmf::indexMapObjects(document);
    require(index.find({MapObjectKind::Solid, 10})->visGroupShown,
            "an object in two visgroups follows whichever was toggled last");
    require(index.visGroupState(3) == VisGroupState::Partial,
            "a visgroup with one shown and one hidden member reads partial");

    require(hammer::vmf::showAllObjects(document), "Show All clears every hidden flag");
    index = hammer::vmf::indexMapObjects(document);
    require(index.visGroupState(3) == VisGroupState::Shown, "Show All shows everything");
}

void testMembershipEditing()
{
    Document document = parseOrDie(GroupedVmf);
    require(hammer::vmf::addObjectsToVisGroup(document, {Solid12}, 1, false),
            "an object can be added to a visgroup");
    require(!hammer::vmf::addObjectsToVisGroup(document, {Solid12}, 1, false),
            "adding the same membership twice changes nothing");
    auto index = hammer::vmf::indexMapObjects(document);
    require(index.find({MapObjectKind::Solid, 12})->visGroupIds.size() == 1, "one membership added");

    // "Remove from other groups" leaves exactly one id behind.
    require(hammer::vmf::addObjectsToVisGroup(document, {Solid10}, 1, true),
            "exclusive add rewrites the membership list");
    index = hammer::vmf::indexMapObjects(document);
    const auto& ids = index.find({MapObjectKind::Solid, 10})->visGroupIds;
    require(ids.size() == 1 && ids.front() == 1, "the object is now only in the target visgroup");

    require(hammer::vmf::removeObjectsFromVisGroup(document, {Solid10}, 1),
            "a membership can be removed");
    index = hammer::vmf::indexMapObjects(document);
    require(index.find({MapObjectKind::Solid, 10})->visGroupIds.empty(),
            "removing the last membership leaves none");

    // Deleting a visgroup re-parents its children and strips member ids, but
    // deletes no objects.
    require(hammer::vmf::deleteVisGroup(document, 1), "a visgroup can be deleted");
    index = hammer::vmf::indexMapObjects(document);
    require(index.findVisGroup(1) == nullptr, "the visgroup is gone");
    require(index.findVisGroup(2) != nullptr && index.findVisGroup(2)->parentId == 0,
            "its child visgroup moves up to the deleted group's parent");
    require(index.find({MapObjectKind::Solid, 12}) != nullptr, "no object was deleted");
    require(index.find({MapObjectKind::Solid, 12})->visGroupIds.empty(),
            "members lose the deleted visgroup's id");
}

void testPurgeAndIds()
{
    Document document = parseOrDie(GroupedVmf);
    // "Furniture" has a child visgroup, so it survives an empty purge;
    // "Chairs" keeps solid 10, "Lights" keeps two members - nothing to purge.
    require(!hammer::vmf::purgeEmptyVisGroups(document), "nothing is purged while all are in use");

    const int newId = hammer::vmf::maximumVisGroupId(document) + 1;
    require(newId == 4, "the next visgroup id follows the largest in the file");
    require(hammer::vmf::createVisGroup(document, "Empty", newId) == newId,
            "an empty visgroup can be created");
    require(hammer::vmf::purgeEmptyVisGroups(document), "an empty visgroup is purged");
    require(hammer::vmf::indexMapObjects(document).findVisGroup(newId) == nullptr,
            "the purged visgroup is gone");

    require(hammer::vmf::maximumObjectId(document) >= 50,
            "object ids account for group blocks");

    // Emptying a group purges the group block.
    require(hammer::vmf::removeObjectsFromVisGroup(document, {Solid10}, 2), "membership removed");
    Document grouped = parseOrDie(GroupedVmf);
    require(hammer::vmf::ungroup(grouped, {50}), "the group is dissolved");
    require(!hammer::vmf::purgeEmptyGroups(grouped), "ungroup already removed the block");
}

void testRoundTrip()
{
    Document document = parseOrDie(GroupedVmf);
    require(hammer::vmf::showVisGroup(document, 1, false), "hide a visgroup");
    require(hammer::vmf::createGroup(document, {Solid12}, 51) == 51, "make a second group");

    const std::string serialized = document.serialize();
    hammer::vmf::ParseError error;
    auto reloaded = Document::parse(serialized, &error);
    require(reloaded.has_value(), "the written VMF parses again");

    const auto before = hammer::vmf::indexMapObjects(document);
    const auto after = hammer::vmf::indexMapObjects(*reloaded);
    require(before.objects.size() == after.objects.size(), "the same objects come back");
    require(before.visGroups.size() == after.visGroups.size(), "the visgroup tree survives");
    require(after.findVisGroup(2) && after.findVisGroup(2)->parentId == 1,
            "visgroup nesting survives a round trip");
    require(after.find({MapObjectKind::Solid, 10})->visGroupIds.size() == 2,
            "multi-visgroup membership survives a round trip");
    require(!after.find({MapObjectKind::Solid, 10})->visGroupShown,
            "the hidden flag survives a round trip");
    require(after.topLevelGroup({MapObjectKind::Solid, 12}) == 51,
            "group membership survives a round trip");
    require(after.find({MapObjectKind::Solid, 10})->visGroupAutoShown,
            "visgroupautoshown round-trips");
}

// --- Auto VisGroups ---------------------------------------------------------

// One solid whose six sides all carry the same material, so the per-face
// classification can be driven from a single string.
std::string toolSolid(int id, const char* material)
{
    std::string text = "\tsolid\n\t{\n\t\t\"id\" \"" + std::to_string(id) + "\"\n";
    for (int side = 0; side < 6; ++side) {
        text += "\t\tside\n\t\t{\n\t\t\t\"id\" \"" + std::to_string(id * 10 + side) +
                "\"\n\t\t\t\"material\" \"" + material + "\"\n\t\t}\n";
    }
    text += "\t\teditor\n\t\t{\n\t\t\t\"visgroupshown\" \"1\"\n\t\t}\n\t}\n";
    return text;
}

std::string autoVisGroupMap()
{
    std::string text = "world\n{\n\t\"id\" \"1\"\n\t\"classname\" \"worldspawn\"\n";
    text += toolSolid(10, "brick/brickwall001a");        // plain World Geometry
    text += toolSolid(11, "TOOLS/TOOLSNODRAW");          // Nodraw
    text += toolSolid(12, "tools/toolsskybox");          // Sky
    text += toolSolid(13, "tools/toolsplayerclip");      // Clips > Player
    text += toolSolid(14, "tools/toolsblock_los");       // Block > LOS
    text += toolSolid(15, "tools/toolsinvisibleladder"); // Invisible > Ladder
    text += toolSolid(16, "tools/toolshint");            // Hint
    text += toolSolid(17, "water/water_canals");         // Water, via the predicate
    text += "}\n";
    // Entities: a point entity, a light, a prop, an NPC, a node, a brush
    // entity, a func_detail and a trigger.
    const auto pointEntity = [](int id, const char* classname) {
        return std::string("entity\n{\n\t\"id\" \"") + std::to_string(id) +
               "\"\n\t\"classname\" \"" + classname + "\"\n\t\"origin\" \"0 0 0\"\n}\n";
    };
    text += pointEntity(20, "info_player_start");
    text += pointEntity(21, "light_spot");
    text += pointEntity(22, "prop_static");
    text += pointEntity(23, "npc_zombie");
    text += pointEntity(24, "info_node");
    text += pointEntity(25, "info_node_link");
    const auto brushEntity = [](int id, const char* classname) {
        return std::string("entity\n{\n\t\"id\" \"") + std::to_string(id) +
               "\"\n\t\"classname\" \"" + classname + "\"\n" +
               "\tsolid\n\t{\n\t\t\"id\" \"" + std::to_string(id * 10) +
               "\"\n\t\tside\n\t\t{\n\t\t\t\"id\" \"1\"\n"
               "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n\t\t}\n\t}\n}\n";
    };
    text += brushEntity(30, "func_wall");
    text += brushEntity(31, "func_detail");
    text += brushEntity(32, "trigger_multiple");
    text += brushEntity(33, "func_areaportal");
    return text;
}

bool inCategory(const hammer::vmf::AutoVisGroupIndex& index, hammer::vmf::AutoVisGroup category,
                MapObjectKind kind, int id)
{
    for (const auto& key : index.objectsIn(category)) {
        if (key.kind == kind && key.id == id) return true;
    }
    return false;
}

void testAutoVisGroupClassification()
{
    using AVG = hammer::vmf::AutoVisGroup;
    const Document document = parseOrDie(autoVisGroupMap().c_str());
    const auto index = hammer::vmf::indexMapObjects(document);
    const std::unordered_set<std::string> npcClasses{"npc_zombie"};
    // Stands in for the material system: only the water material is water.
    const auto isWater = [](std::string_view material) {
        return material.find("water/") == 0;
    };
    const auto autoIndex =
        hammer::vmf::indexAutoVisGroups(document, index, npcClasses, isWater);

    // World geometry
    require(inCategory(autoIndex, AVG::WorldGeometry, MapObjectKind::Solid, 10),
            "a plain world solid is World Geometry itself");
    require(inCategory(autoIndex, AVG::Nodraw, MapObjectKind::Solid, 11), "nodraw is classified");
    require(inCategory(autoIndex, AVG::Sky, MapObjectKind::Solid, 12), "sky is classified");
    require(inCategory(autoIndex, AVG::Water, MapObjectKind::Solid, 17),
            "water comes from the injected material predicate");

    // Nested tool categories, and their parents.
    require(inCategory(autoIndex, AVG::ClipPlayer, MapObjectKind::Solid, 13),
            "a playerclip lands in Clips > Player");
    require(inCategory(autoIndex, AVG::Clips, MapObjectKind::Solid, 13),
            "and so counts as a member of the parent Clips category");
    require(inCategory(autoIndex, AVG::ToolBrushes, MapObjectKind::Solid, 13),
            "and of Tool Brushes above that");
    require(inCategory(autoIndex, AVG::BlockLos, MapObjectKind::Solid, 14),
            "toolsblock_los lands in Block > LOS");
    require(inCategory(autoIndex, AVG::InvisibleLadder, MapObjectKind::Solid, 15),
            "invisibleladder lands in Invisible > Ladder");
    require(inCategory(autoIndex, AVG::Hint, MapObjectKind::Solid, 16), "hint is classified");
    require(!inCategory(autoIndex, AVG::Clip, MapObjectKind::Solid, 13),
            "a playerclip is not ALSO in the generic Clip category");

    // Entities
    require(inCategory(autoIndex, AVG::PointEntities, MapObjectKind::Entity, 20),
            "a point entity is a Point Entity");
    require(inCategory(autoIndex, AVG::Lights, MapObjectKind::Entity, 21),
            "light_* lands in Lights");
    require(inCategory(autoIndex, AVG::Props, MapObjectKind::Entity, 22),
            "prop_* lands in Props");
    require(inCategory(autoIndex, AVG::WorldDetails, MapObjectKind::Entity, 22),
            "Props sits under World Details, not Entities");
    // AddToAutoVisGroup adds "Point Entities" first and then Props, so a prop
    // is in both trees at once - the categories are not exclusive.
    require(inCategory(autoIndex, AVG::PointEntities, MapObjectKind::Entity, 22),
            "a prop is still a Point Entity as well");
    require(inCategory(autoIndex, AVG::NPCs, MapObjectKind::Entity, 23),
            "an @NPCClass entity lands in NPCs");
    require(inCategory(autoIndex, AVG::Nodes, MapObjectKind::Entity, 24),
            "info_node lands in Nodes");
    require(inCategory(autoIndex, AVG::PointEntities, MapObjectKind::Entity, 25),
            "info_node_link is explicitly NOT a node class");
    require(inCategory(autoIndex, AVG::BrushEntities, MapObjectKind::Entity, 30),
            "a brush entity lands in Brush Entities");
    require(inCategory(autoIndex, AVG::FuncDetail, MapObjectKind::Entity, 31),
            "func_detail lands in Func Detail, not Brush Entities");
    require(!inCategory(autoIndex, AVG::BrushEntities, MapObjectKind::Entity, 31),
            "and only there");
    require(inCategory(autoIndex, AVG::Triggers, MapObjectKind::Entity, 32),
            "trigger_* lands in Triggers");
    require(inCategory(autoIndex, AVG::Areaportals, MapObjectKind::Entity, 33),
            "func_areaportal lands in Areaportals");

    // A brush entity's own solids are not classified separately: they cannot
    // belong to a visgroup independently of their entity.
    require(!inCategory(autoIndex, AVG::Nodraw, MapObjectKind::Solid, 300),
            "a brush entity's child solid is not classified on its own");

    // Only populated categories (and their ancestors) exist.
    require(autoIndex.contains(AVG::Clips), "a populated category is present");
    require(autoIndex.contains(AVG::ToolBrushes), "its ancestors are present too");
    require(!autoIndex.contains(AVG::Fog), "an unpopulated category is absent");
}

void testAutoVisGroupVisibility()
{
    using AVG = hammer::vmf::AutoVisGroup;
    Document document = parseOrDie(autoVisGroupMap().c_str());
    const auto isWater = [](std::string_view) { return false; };
    const std::unordered_set<std::string> npcClasses;

    auto index = hammer::vmf::indexMapObjects(document);
    auto autoIndex = hammer::vmf::indexAutoVisGroups(document, index, npcClasses, isWater);
    const std::vector<MapObjectKey> nodraw = autoIndex.objectsIn(AVG::Nodraw);
    require(!nodraw.empty(), "there is something to hide");

    require(hammer::vmf::showAutoVisGroup(document, nodraw, false),
            "hiding an auto visgroup changes the document");
    index = hammer::vmf::indexMapObjects(document);
    const auto* solid11 = index.find({MapObjectKind::Solid, 11});
    require(solid11 && !solid11->visGroupAutoShown,
            "hiding an auto visgroup writes visgroupautoshown, not visgroupshown");
    require(solid11 && solid11->visGroupShown, "the user flag is left alone");

    std::unordered_set<int> hiddenSolids;
    std::unordered_set<int> hiddenEntities;
    index.hiddenObjects(hiddenSolids, hiddenEntities);
    require(hiddenSolids.count(11) == 1,
            "an auto-hidden object counts as hidden (IsVisGroupShown ANDs both flags)");

    // CMapClass::VisGroupShow: showing through a USER visgroup has precedence
    // and clears the auto-hidden flag as well.
    const int userGroup = hammer::vmf::createVisGroup(document, "User", 1);
    require(hammer::vmf::addObjectsToVisGroup(document, {Solid11}, userGroup, false),
            "the auto-hidden solid joins a user visgroup");
    require(hammer::vmf::showVisGroup(document, userGroup, true),
            "showing that user visgroup changes the document");
    index = hammer::vmf::indexMapObjects(document);
    const auto* after = index.find({MapObjectKind::Solid, 11});
    require(after && after->visGroupAutoShown,
            "a USER show clears the auto-hidden flag too (user has precedence)");
    hiddenSolids.clear();
    hiddenEntities.clear();
    index.hiddenObjects(hiddenSolids, hiddenEntities);
    require(hiddenSolids.count(11) == 0, "so the object is visible again");
}

} // namespace

int main()
{
    testIndexing();
    testNestedGroups();
    testVisibilityDirection();
    testMembershipEditing();
    testPurgeAndIds();
    testRoundTrip();
    testAutoVisGroupClassification();
    testAutoVisGroupVisibility();
    std::cout << "VMF grouping/visgrouping tests passed\n";
    return EXIT_SUCCESS;
}
