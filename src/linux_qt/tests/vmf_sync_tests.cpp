#include "VmfDocument.hpp"
#include "VmfSync.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using hammer::vmf::Block;
using hammer::vmf::Document;
using hammer::vmf::SyncDelta;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

const char* SampleVmf =
    "versioninfo\n"
    "{\n"
    "\t\"mapversion\" \"7\"\n"
    "}\n"
    "world\n"
    "{\n"
    "\t\"id\" \"1\"\n"
    "\t\"classname\" \"worldspawn\"\n"
    "\t\"skyname\" \"sky_day01_01\"\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"2\"\n"
    "\t\tside\n"
    "\t\t{\n"
    "\t\t\t\"id\" \"3\"\n"
    "\t\t\t\"material\" \"BRICK/BRICKWALL001A\"\n"
    "\t\t}\n"
    "\t}\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"10\"\n"
    "\t\tside\n"
    "\t\t{\n"
    "\t\t\t\"id\" \"11\"\n"
    "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
    "\t\t}\n"
    "\t}\n"
    "}\n"
    "entity\n"
    "{\n"
    "\t\"id\" \"20\"\n"
    "\t\"classname\" \"info_player_start\"\n"
    "\t\"origin\" \"0 0 0\"\n"
    "}\n";

Document parseSample()
{
    auto document = Document::parse(SampleVmf);
    require(document.has_value(), "sample parses");
    return *document;
}

Block* worldSolid(Document& document, const std::string& id)
{
    Block* world = document.firstRoot("world");
    require(world != nullptr, "world exists");
    for (Block* solid : world->children("solid")) {
        const std::string* value = solid->value("id");
        if (value && *value == id) return solid;
    }
    return nullptr;
}

void testNoChanges()
{
    const Document a = parseSample();
    const Document b = parseSample();
    require(hammer::vmf::diffDocuments(a, b).empty(), "identical documents diff empty");
}

void testSolidEditAndConvergence()
{
    const Document before = parseSample();
    Document after = before;
    Block* solid = worldSolid(after, "2");
    require(solid != nullptr, "solid 2 found");
    solid->children("side")[0]->setValue("material", "CONCRETE/CONCRETEFLOOR001");

    const SyncDelta delta = hammer::vmf::diffDocuments(before, after);
    require(delta.upserts.size() == 1 && delta.removals.empty(), "one upsert for one solid edit");
    require(delta.upserts[0].kind == "solid" && delta.upserts[0].key == "2", "solid keyed by id");

    Document patched = before;
    hammer::vmf::applyDelta(patched, delta);
    require(hammer::vmf::diffDocuments(patched, after).empty(), "patched converges to after");
}

void testEntityAddRemove()
{
    const Document before = parseSample();
    Document after = before;
    Block& added = after.appendRoot("entity");
    added.setValue("id", "30");
    added.setValue("classname", "light");

    SyncDelta delta = hammer::vmf::diffDocuments(before, after);
    require(delta.upserts.size() == 1 && delta.upserts[0].kind == "entity" &&
                delta.upserts[0].key == "30",
            "added entity upserts");

    Document patched = before;
    hammer::vmf::applyDelta(patched, delta);
    require(hammer::vmf::diffDocuments(patched, after).empty(), "entity add converges");

    // And the reverse direction is a removal.
    delta = hammer::vmf::diffDocuments(after, before);
    require(delta.removals.size() == 1 && delta.removals[0].kind == "entity" &&
                delta.removals[0].key == "30",
            "removed entity produces removal");
    hammer::vmf::applyDelta(patched, delta);
    require(hammer::vmf::diffDocuments(patched, before).empty(), "entity remove converges");
}

void testSolidRemoval()
{
    const Document before = parseSample();
    Document after = before;
    Block* world = after.firstRoot("world");
    for (auto it = world->entries.begin(); it != world->entries.end(); ++it) {
        if (it->kind == hammer::vmf::Entry::Kind::ChildBlock && it->child &&
            it->child->name == "solid" && it->child->value("id") && *it->child->value("id") == "10") {
            world->entries.erase(it);
            break;
        }
    }
    const SyncDelta delta = hammer::vmf::diffDocuments(before, after);
    require(delta.removals.size() == 1 && delta.removals[0].kind == "solid" &&
                delta.removals[0].key == "10",
            "deleted solid produces removal");
    Document patched = before;
    hammer::vmf::applyDelta(patched, delta);
    require(hammer::vmf::diffDocuments(patched, after).empty(), "solid removal converges");
    // Removals are idempotent.
    hammer::vmf::applyDelta(patched, delta);
    require(hammer::vmf::diffDocuments(patched, after).empty(), "reapplying delta is harmless");
}

void testWorldKeyvaluesKeepSolids()
{
    const Document before = parseSample();
    Document after = before;
    after.firstRoot("world")->setValue("skyname", "sky_day02_01");

    const SyncDelta delta = hammer::vmf::diffDocuments(before, after);
    require(delta.upserts.size() == 1 && delta.upserts[0].kind == "world",
            "sky change is one world upsert");

    Document patched = before;
    hammer::vmf::applyDelta(patched, delta);
    require(patched.firstRoot("world")->children("solid").size() == 2,
            "world upsert keeps existing solids");
    require(hammer::vmf::diffDocuments(patched, after).empty(), "world edit converges");
}

void testOtherRoots()
{
    const Document before = parseSample();
    Document after = before;
    after.firstRoot("versioninfo")->setValue("mapversion", "8");

    const SyncDelta delta = hammer::vmf::diffDocuments(before, after);
    require(delta.upserts.size() == 1 && delta.upserts[0].kind == "root" &&
                delta.upserts[0].key == "versioninfo#0",
            "versioninfo edit is a root upsert");
    Document patched = before;
    hammer::vmf::applyDelta(patched, delta);
    require(hammer::vmf::diffDocuments(patched, after).empty(), "root edit converges");
}

void testWireRoundTrip()
{
    const Document before = parseSample();
    Document after = before;
    worldSolid(after, "2")->children("side")[0]->setValue("material", "METAL/METALWALL001");
    Block& added = after.appendRoot("entity");
    added.setValue("id", "31");
    added.setValue("classname", "light_spot");

    const SyncDelta delta = hammer::vmf::diffDocuments(before, after);
    const std::string wire = hammer::vmf::serializeDelta(delta);
    const auto parsed = hammer::vmf::parseDelta(wire);
    require(parsed.has_value(), "wire delta parses");
    require(parsed->upserts.size() == delta.upserts.size() &&
                parsed->removals.size() == delta.removals.size(),
            "wire delta keeps every operation");

    Document patched = before;
    hammer::vmf::applyDelta(patched, *parsed);
    require(hammer::vmf::diffDocuments(patched, after).empty(), "wire round trip converges");
}

void testConcurrentDisjointEdits()
{
    // Peer A moves an entity while peer B edits a solid: applying both deltas
    // to the shared base, in either order, converges to the same map.
    const Document base = parseSample();

    Document byA = base;
    for (Block& root : byA.roots()) {
        if (root.name == "entity") root.setValue("origin", "64 0 0");
    }
    Document byB = base;
    worldSolid(byB, "10")->children("side")[0]->setValue("material", "METAL/METALGRATE001");

    const SyncDelta deltaA = hammer::vmf::diffDocuments(base, byA);
    const SyncDelta deltaB = hammer::vmf::diffDocuments(base, byB);

    Document mergedAB = base;
    hammer::vmf::applyDelta(mergedAB, deltaA);
    hammer::vmf::applyDelta(mergedAB, deltaB);
    Document mergedBA = base;
    hammer::vmf::applyDelta(mergedBA, deltaB);
    hammer::vmf::applyDelta(mergedBA, deltaA);

    require(hammer::vmf::diffDocuments(mergedAB, mergedBA).empty(),
            "disjoint concurrent edits commute");
    const std::string* origin = mergedAB.firstRoot("entity")->value("origin");
    require(origin && *origin == "64 0 0", "merge kept A's entity move");
    require(*worldSolid(mergedAB, "10")->children("side")[0]->value("material") ==
                "METAL/METALGRATE001",
            "merge kept B's material edit");
}

void testSyncHash()
{
    const Document base = parseSample();
    require(hammer::vmf::documentSyncHash(base) == hammer::vmf::documentSyncHash(parseSample()),
            "equal documents hash equal");

    // Same objects, different order: concurrent creates arrive in different
    // order on each peer, and that must NOT read as a desync.
    Document reordered = parseSample();
    std::rotate(reordered.roots().begin(), reordered.roots().begin() + 1,
                reordered.roots().end());
    Block* world = reordered.firstRoot("world");
    for (auto it = world->entries.begin(); it != world->entries.end(); ++it) {
        if (it->kind == hammer::vmf::Entry::Kind::ChildBlock && it->child &&
            it->child->name == "solid") {
            hammer::vmf::Entry moved = std::move(*it);
            world->entries.erase(it);
            world->entries.push_back(std::move(moved));
            break;
        }
    }
    require(hammer::vmf::documentSyncHash(base) == hammer::vmf::documentSyncHash(reordered),
            "object order does not change the hash");

    Document changed = parseSample();
    worldSolid(changed, "2")->children("side")[0]->setValue("material", "METAL/METALDOOR001");
    require(hammer::vmf::documentSyncHash(base) != hammer::vmf::documentSyncHash(changed),
            "a property edit changes the hash");

    Document removed = parseSample();
    removed.roots().pop_back();  // drop the entity
    require(hammer::vmf::documentSyncHash(base) != hammer::vmf::documentSyncHash(removed),
            "a removed object changes the hash");
}

} // namespace

int main()
{
    testSyncHash();
    testNoChanges();
    testSolidEditAndConvergence();
    testEntityAddRemove();
    testSolidRemoval();
    testWorldKeyvaluesKeepSolids();
    testOtherRoots();
    testWireRoundTrip();
    testConcurrentDisjointEdits();
    std::cout << "vmf sync tests passed\n";
    return 0;
}
