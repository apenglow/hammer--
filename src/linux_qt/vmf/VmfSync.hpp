#pragma once

#include "VmfDocument.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hammer::vmf {

// Object-level document diffing for collaborative editing. A delta carries
// whole top-level objects (a world solid, an entity with its solids, the
// worldspawn keyvalues, or any other root block) rather than fine-grained
// edits: peers exchange deltas and conflicts collapse to last-writer-wins per
// object, which is the granularity two mappers actually collide at.
struct SyncDelta
{
    // kind values:
    //   "solid"  - a world solid, key is its "id"
    //   "entity" - a top-level entity block (including its solids), key is its "id"
    //   "world"  - the world block minus its solid children (keyvalues, group
    //              blocks...), key is empty
    //   "root"   - any other top-level block (versioninfo, visgroups, cameras,
    //              viewsettings...), key is "<name>#<occurrence index>"
    // A solid or entity without an "id" keyvalue is keyed "@<occurrence index>".
    struct Upsert
    {
        std::string kind;
        std::string key;
        Block block;
    };

    struct Removal
    {
        std::string kind;
        std::string key;
    };

    std::vector<Upsert> upserts;
    std::vector<Removal> removals;

    bool empty() const { return upserts.empty() && removals.empty(); }
};

// Object-level structural comparison of two documents. Order changes among
// unchanged objects are not reported; the delta captures added, removed and
// modified objects only.
SyncDelta diffDocuments(const Document& before, const Document& after);

// Applies a delta produced by diffDocuments. Unknown keys upsert as new
// objects; removals of already-absent objects are ignored, so applying the
// same delta twice is harmless.
void applyDelta(Document& document, const SyncDelta& delta);

// Wire format: the delta rendered as VMF text (a "sync_delta" root block), so
// the existing parser and serializer carry it and it stays human-debuggable.
std::string serializeDelta(const SyncDelta& delta);
std::optional<SyncDelta> parseDelta(const std::string& text);

// Content hash of every property in the document, at the same object
// granularity the deltas use, for desync detection between peers. The
// combination is ORDER-INDEPENDENT across objects: concurrent creates append
// in arrival order, so two peers legitimately hold the same objects in
// different order, and an order-sensitive hash would flag every such session
// as desynced. (The occurrence-based keys for id-less objects are still
// order-sensitive; in practice everything carries an id.)
std::uint64_t documentSyncHash(const Document& document);

} // namespace hammer::vmf
