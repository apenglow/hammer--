#include "VmfSync.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <map>
#include <utility>

namespace hammer::vmf {

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] - 'A' + 'a') : a[i];
        const char cb = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) return false;
    }
    return true;
}

bool blockEquals(const Block& a, const Block& b)
{
    if (a.name != b.name || a.entries.size() != b.entries.size()) return false;
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        const Entry& ea = a.entries[i];
        const Entry& eb = b.entries[i];
        if (ea.kind != eb.kind) return false;
        if (ea.kind == Entry::Kind::KeyValue) {
            if (ea.key != eb.key || ea.value != eb.value) return false;
        } else {
            if (!ea.child != !eb.child) return false;
            if (ea.child && !blockEquals(*ea.child, *eb.child)) return false;
        }
    }
    return true;
}

// The world block with its solid children stripped: worldspawn keyvalues plus
// the group blocks, diffed as one "world" unit while each solid is its own.
Block strippedWorld(const Block& world)
{
    Block stripped(world.name);
    for (const Entry& entry : world.entries) {
        if (entry.kind == Entry::Kind::ChildBlock && entry.child &&
            equalsIgnoreCase(entry.child->name, "solid")) {
            continue;
        }
        stripped.entries.push_back(entry);
    }
    return stripped;
}

std::string idKey(const Block& block, std::size_t occurrence)
{
    if (const std::string* id = block.value("id")) return *id;
    return "@" + std::to_string(occurrence);
}

// Keyed views over a document's top-level objects. Key -> block pointer;
// std::map so diff output order is deterministic.
struct DocumentIndex
{
    std::map<std::string, const Block*> solids;    // world solids by id
    std::map<std::string, const Block*> entities;  // top-level entities by id
    std::map<std::string, const Block*> roots;     // other roots by "name#index"
    std::optional<Block> world;                    // stripped world block
};

DocumentIndex indexDocument(const Document& document)
{
    DocumentIndex index;
    std::map<std::string, std::size_t> rootCounts;
    std::size_t entityOccurrence = 0;
    for (const Block& root : document.roots()) {
        if (equalsIgnoreCase(root.name, "world")) {
            index.world = strippedWorld(root);
            std::size_t solidOccurrence = 0;
            for (const Block* solid : root.children("solid"))
                index.solids.emplace(idKey(*solid, solidOccurrence++), solid);
        } else if (equalsIgnoreCase(root.name, "entity")) {
            index.entities.emplace(idKey(root, entityOccurrence++), &root);
        } else {
            const std::size_t occurrence = rootCounts[root.name]++;
            index.roots.emplace(root.name + "#" + std::to_string(occurrence), &root);
        }
    }
    return index;
}

template <typename Map>
void diffKeyed(const Map& before, const Map& after, const char* kind, SyncDelta& delta)
{
    for (const auto& [key, block] : after) {
        const auto it = before.find(key);
        if (it == before.end() || !blockEquals(*it->second, *block))
            delta.upserts.push_back({kind, key, *block});
    }
    for (const auto& [key, block] : before) {
        (void)block;
        if (!after.count(key)) delta.removals.push_back({kind, key});
    }
}

Block* findWorld(Document& document)
{
    for (Block& root : document.roots())
        if (equalsIgnoreCase(root.name, "world")) return &root;
    return nullptr;
}

bool solidKeyMatches(const Block& solid, const std::string& key, std::size_t occurrence)
{
    return idKey(solid, occurrence) == key;
}

void upsertWorld(Document& document, const Block& stripped)
{
    Block* world = findWorld(document);
    if (!world) {
        world = &document.appendRoot("world");
    }
    // Replace everything but the solids, keeping the solids where the new
    // keyvalue/group layout puts them: non-solid entries first, solids after.
    std::vector<Entry> merged = stripped.entries;
    for (Entry& entry : world->entries) {
        if (entry.kind == Entry::Kind::ChildBlock && entry.child &&
            equalsIgnoreCase(entry.child->name, "solid")) {
            merged.push_back(std::move(entry));
        }
    }
    world->name = stripped.name;
    world->entries = std::move(merged);
}

void upsertSolid(Document& document, const std::string& key, const Block& block)
{
    Block* world = findWorld(document);
    if (!world) world = &document.appendRoot("world");
    std::size_t occurrence = 0;
    for (Entry& entry : world->entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child ||
            !equalsIgnoreCase(entry.child->name, "solid")) {
            continue;
        }
        if (solidKeyMatches(*entry.child, key, occurrence++)) {
            *entry.child = block;
            return;
        }
    }
    world->entries.emplace_back(Block(block));
}

void removeSolid(Document& document, const std::string& key)
{
    Block* world = findWorld(document);
    if (!world) return;
    std::size_t occurrence = 0;
    for (auto it = world->entries.begin(); it != world->entries.end(); ++it) {
        if (it->kind != Entry::Kind::ChildBlock || !it->child ||
            !equalsIgnoreCase(it->child->name, "solid")) {
            continue;
        }
        if (solidKeyMatches(*it->child, key, occurrence++)) {
            world->entries.erase(it);
            return;
        }
    }
}

void upsertEntity(Document& document, const std::string& key, const Block& block)
{
    std::size_t occurrence = 0;
    for (Block& root : document.roots()) {
        if (!equalsIgnoreCase(root.name, "entity")) continue;
        if (idKey(root, occurrence++) == key) {
            root = block;
            return;
        }
    }
    document.roots().push_back(block);
}

void removeEntity(Document& document, const std::string& key)
{
    std::vector<Block>& roots = document.roots();
    std::size_t occurrence = 0;
    for (auto it = roots.begin(); it != roots.end(); ++it) {
        if (!equalsIgnoreCase(it->name, "entity")) continue;
        if (idKey(*it, occurrence++) == key) {
            roots.erase(it);
            return;
        }
    }
}

// "name#index" -> name, index. Returns false on a malformed key.
bool splitRootKey(const std::string& key, std::string& name, std::size_t& index)
{
    const std::size_t hash = key.rfind('#');
    if (hash == std::string::npos) return false;
    name = key.substr(0, hash);
    try {
        index = std::stoul(key.substr(hash + 1));
    } catch (...) {
        return false;
    }
    return true;
}

void upsertRoot(Document& document, const std::string& key, const Block& block)
{
    std::string name;
    std::size_t index = 0;
    if (!splitRootKey(key, name, index)) return;
    std::size_t occurrence = 0;
    for (Block& root : document.roots()) {
        if (root.name != name) continue;
        if (occurrence++ == index) {
            root = block;
            return;
        }
    }
    document.roots().push_back(block);
}

void removeRoot(Document& document, const std::string& key)
{
    std::string name;
    std::size_t index = 0;
    if (!splitRootKey(key, name, index)) return;
    std::vector<Block>& roots = document.roots();
    std::size_t occurrence = 0;
    for (auto it = roots.begin(); it != roots.end(); ++it) {
        if (it->name != name) continue;
        if (occurrence++ == index) {
            roots.erase(it);
            return;
        }
    }
}

void serializeBlockText(const Block& block, std::string& out)
{
    out += block.name;
    out += '{';
    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::KeyValue) {
            out += '"';
            out += entry.key;
            out += "\" \"";
            out += entry.value;
            out += '"';
        } else if (entry.child) {
            serializeBlockText(*entry.child, out);
        }
    }
    out += '}';
}

std::uint64_t fnv1a(std::string_view text, std::uint64_t hash = 14695981039346656037ull)
{
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t objectHash(const char* kind, const std::string& key, const Block& block)
{
    std::string text;
    serializeBlockText(block, text);
    std::uint64_t hash = fnv1a(kind);
    hash = fnv1a("\x1f", hash);
    hash = fnv1a(key, hash);
    hash = fnv1a("\x1f", hash);
    return fnv1a(text, hash);
}

// Order-independent accumulation: sum and xor of a mixed per-object hash.
// Either alone is weak (sum cancels pairs, xor cancels duplicates); combined
// and mixed they are plenty for detecting divergence.
void accumulate(std::uint64_t& sum, std::uint64_t& xored, std::uint64_t hash)
{
    hash *= 0x9E3779B97F4A7C15ull;
    hash ^= hash >> 29;
    sum += hash;
    xored ^= hash;
}

} // namespace

std::uint64_t documentSyncHash(const Document& document)
{
    const DocumentIndex index = indexDocument(document);
    std::uint64_t sum = 0;
    std::uint64_t xored = 0;
    if (index.world) accumulate(sum, xored, objectHash("world", std::string(), *index.world));
    for (const auto& [key, block] : index.solids)
        accumulate(sum, xored, objectHash("solid", key, *block));
    for (const auto& [key, block] : index.entities)
        accumulate(sum, xored, objectHash("entity", key, *block));
    for (const auto& [key, block] : index.roots)
        accumulate(sum, xored, objectHash("root", key, *block));
    return sum ^ (xored * 0xFF51AFD7ED558CCDull);
}

SyncDelta diffDocuments(const Document& before, const Document& after)
{
    const DocumentIndex a = indexDocument(before);
    const DocumentIndex b = indexDocument(after);

    SyncDelta delta;
    if (b.world && (!a.world || !blockEquals(*a.world, *b.world)))
        delta.upserts.push_back({"world", "", *b.world});
    diffKeyed(a.solids, b.solids, "solid", delta);
    diffKeyed(a.entities, b.entities, "entity", delta);
    diffKeyed(a.roots, b.roots, "root", delta);
    return delta;
}

void applyDelta(Document& document, const SyncDelta& delta)
{
    for (const SyncDelta::Upsert& upsert : delta.upserts) {
        if (upsert.kind == "world") upsertWorld(document, upsert.block);
        else if (upsert.kind == "solid") upsertSolid(document, upsert.key, upsert.block);
        else if (upsert.kind == "entity") upsertEntity(document, upsert.key, upsert.block);
        else if (upsert.kind == "root") upsertRoot(document, upsert.key, upsert.block);
    }
    for (const SyncDelta::Removal& removal : delta.removals) {
        if (removal.kind == "solid") removeSolid(document, removal.key);
        else if (removal.kind == "entity") removeEntity(document, removal.key);
        else if (removal.kind == "root") removeRoot(document, removal.key);
    }
    document.markDirty();
}

std::string serializeDelta(const SyncDelta& delta)
{
    // Rendered through a throwaway Document so the delta rides the existing
    // VMF serializer/parser instead of a second wire format.
    Document carrier;
    Block& root = carrier.appendRoot("sync_delta");
    root.setValue("format", "1");
    for (const SyncDelta::Upsert& upsert : delta.upserts) {
        Block& entry = root.appendChild("upsert");
        entry.setValue("kind", upsert.kind);
        entry.setValue("key", upsert.key);
        entry.entries.emplace_back(Block(upsert.block));
    }
    for (const SyncDelta::Removal& removal : delta.removals) {
        Block& entry = root.appendChild("remove");
        entry.setValue("kind", removal.kind);
        entry.setValue("key", removal.key);
    }
    return carrier.serialize(false);
}

std::optional<SyncDelta> parseDelta(const std::string& text)
{
    const std::optional<Document> carrier = Document::parse(text);
    if (!carrier) return std::nullopt;
    const Block* root = carrier->firstRoot("sync_delta");
    if (!root) return std::nullopt;

    SyncDelta delta;
    for (const Block* entry : root->children("upsert")) {
        const std::string* kind = entry->value("kind");
        const std::string* key = entry->value("key");
        const Block* payload = nullptr;
        for (const Entry& child : entry->entries) {
            if (child.kind == Entry::Kind::ChildBlock && child.child) {
                payload = child.child.get();
                break;
            }
        }
        if (!kind || !key || !payload) return std::nullopt;
        delta.upserts.push_back({*kind, *key, *payload});
    }
    for (const Block* entry : root->children("remove")) {
        const std::string* kind = entry->value("kind");
        const std::string* key = entry->value("key");
        if (!kind || !key) return std::nullopt;
        delta.removals.push_back({*kind, *key});
    }
    return delta;
}

} // namespace hammer::vmf
