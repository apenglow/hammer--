#include "VmfEditor.hpp"

#include "VmfGroups.hpp"

#include "VmfSolidCarve.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace hammer::vmf {
namespace {

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        return a == b;
    });
}

const std::string* valueIgnoreCase(const Block& block, std::string_view key);

int parseInt(const std::string* text)
{
    if (!text) return -1;
    int value = -1;
    const auto result = std::from_chars(text->data(), text->data() + text->size(), value);
    return result.ec == std::errc{} && result.ptr == text->data() + text->size() ? value : -1;
}

bool parseNumbers(std::string_view text, double* output, std::size_t count)
{
    std::string cleaned(text);
    for (char& ch : cleaned) {
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',') ch = ' ';
    }
    std::istringstream stream(cleaned);
    for (std::size_t i = 0; i < count; ++i) {
        if (!(stream >> output[i]) || !std::isfinite(output[i])) return false;
    }
    return true;
}

std::string formatNumber(double value)
{
    if (std::abs(value) < 0.0000005) value = 0.0;
    const double rounded = std::round(value);
    if (std::abs(value - rounded) < 0.0000005) return std::to_string(static_cast<long long>(rounded));

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    std::string result = stream.str();
    while (!result.empty() && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result.empty() || result == "-0" ? "0" : result;
}

std::string formatVec3(const Vec3& value)
{
    return formatNumber(value.x) + " " + formatNumber(value.y) + " " + formatNumber(value.z);
}

std::string formatPlane(const double* values)
{
    return "(" + formatNumber(values[0]) + " " + formatNumber(values[1]) + " " + formatNumber(values[2]) + ") "
           "(" + formatNumber(values[3]) + " " + formatNumber(values[4]) + " " + formatNumber(values[5]) + ") "
           "(" + formatNumber(values[6]) + " " + formatNumber(values[7]) + " " + formatNumber(values[8]) + ")";
}

// CMapFace::CalcPlane's GetNormalFromPoints(p0, p1, p2) = (p0 - p1) x (p2 - p1).
// For the winding Hammer writes this is the OUTWARD normal.
Vec3 planeNormal(const Vec3& p0, const Vec3& p1, const Vec3& p2)
{
    const Vec3 edge1{p0.x - p1.x, p0.y - p1.y, p0.z - p1.z};
    const Vec3 edge2{p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
    return {edge1.y * edge2.z - edge1.z * edge2.y,
            edge1.z * edge2.x - edge1.x * edge2.z,
            edge1.x * edge2.y - edge1.y * edge2.x};
}

// Every face of a newly created brush is World-aligned, which is
// CMapFace::InitializeTextureAxes( TEXTURE_ALIGN_WORLD ): the texture axes
// come from the dominant world axis of the face normal.
//
// The fixed "[1 0 0] / [0 -1 0]" pair this replaces was only right on the two
// faces whose normal is +/-Z. On the +/-X faces it put the U axis PARALLEL to
// the normal, so the projection was degenerate and the material smeared; the
// slanted faces of wedges, cylinders and spikes had the same problem.
void applyWorldAlignedTexture(Block& side, const Vec3& normal, const std::string& material)
{
    FaceTexture texture;
    texture.material = material;
    initializeTextureAxes(texture, normal, TextureAlignment::World);
    writeFaceTexture(side, texture);
}

Vec3 scalePoint(const Vec3& point, const Vec3& factors, const Vec3& pivot)
{
    return {pivot.x + (point.x - pivot.x) * factors.x,
            pivot.y + (point.y - pivot.y) * factors.y,
            pivot.z + (point.z - pivot.z) * factors.z};
}

Vec3 rotatePoint(const Vec3& point, double radians, RotationAxis axis, const Vec3& pivot)
{
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    Vec3 local{point.x - pivot.x, point.y - pivot.y, point.z - pivot.z};
    Vec3 rotated = local;
    switch (axis) {
    case RotationAxis::X:
        rotated.y = local.y * cosine - local.z * sine;
        rotated.z = local.y * sine + local.z * cosine;
        break;
    case RotationAxis::Y:
        rotated.x = local.x * cosine + local.z * sine;
        rotated.z = -local.x * sine + local.z * cosine;
        break;
    case RotationAxis::Z:
        rotated.x = local.x * cosine - local.y * sine;
        rotated.y = local.x * sine + local.y * cosine;
        break;
    }
    return {rotated.x + pivot.x, rotated.y + pivot.y, rotated.z + pivot.z};
}

// Rotates an entity's "angles" keyvalue (Source "pitch yaw roll", degrees) by
// composing the world-axis rotation with the entity's orientation matrix.
// Rotating a point entity about its own origin moves no points at all, so the
// orientation key is the only thing that records the rotation.
bool rotateEntityAngles(Block& entity, double radians, RotationAxis axis)
{
    std::string* angles = entity.value("angles");
    // Entities without an angles key that own brushes (brush entities) keep
    // their geometry-only rotation; point entities gain the key, as Hammer's
    // CMapEntity::DoTransform does.
    if (!angles && !entity.children("solid").empty()) return false;
    double values[3] = {0.0, 0.0, 0.0};
    if (angles && !parseNumbers(*angles, values, 3)) return false;

    constexpr double DegToRad = 3.14159265358979323846 / 180.0;
    const double pitch = values[0] * DegToRad;
    const double yaw = values[1] * DegToRad;
    const double roll = values[2] * DegToRad;
    const double sp = std::sin(pitch), cp = std::cos(pitch);
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    const double sr = std::sin(roll), cr = std::cos(roll);
    // Source AngleMatrix: columns are forward/left/up.
    double m[3][3] = {
        {cp * cy, sr * sp * cy - cr * sy, cr * sp * cy + sr * sy},
        {cp * sy, sr * sp * sy + cr * cy, cr * sp * sy - sr * cy},
        {-sp, sr * cp, cr * cp}
    };
    // Rotate every basis column through the same world-axis rotation the
    // vertices go through.
    for (int column = 0; column < 3; ++column) {
        const Vec3 rotated = rotatePoint({m[0][column], m[1][column], m[2][column]},
                                         radians, axis, {0.0, 0.0, 0.0});
        m[0][column] = rotated.x;
        m[1][column] = rotated.y;
        m[2][column] = rotated.z;
    }
    // Source MatrixAngles.
    const double xyLength = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
    double newPitch, newYaw, newRoll;
    if (xyLength > 0.001) {
        newYaw = std::atan2(m[1][0], m[0][0]);
        newPitch = std::atan2(-m[2][0], xyLength);
        newRoll = std::atan2(m[2][1], m[2][2]);
    } else {
        newYaw = std::atan2(-m[0][1], m[1][1]);
        newPitch = std::atan2(-m[2][0], xyLength);
        newRoll = 0.0;
    }
    entity.setValue("angles", formatVec3({newPitch / DegToRad, newYaw / DegToRad, newRoll / DegToRad}));
    return true;
}

template <typename Transform>
bool transformOrigin(Block& block, Transform&& transform)
{
    std::string* origin = block.value("origin");
    if (!origin) return false;
    double values[3];
    if (!parseNumbers(*origin, values, 3)) return false;
    *origin = formatVec3(transform(Vec3{values[0], values[1], values[2]}));
    return true;
}

template <typename Transform>
bool transformPlane(Block& side, Transform&& transform)
{
    std::string* plane = side.value("plane");
    if (!plane) return false;
    double values[9];
    if (!parseNumbers(*plane, values, 9)) return false;
    for (int point = 0; point < 3; ++point) {
        const Vec3 transformed = transform(Vec3{values[point * 3], values[point * 3 + 1], values[point * 3 + 2]});
        values[point * 3] = transformed.x;
        values[point * 3 + 1] = transformed.y;
        values[point * 3 + 2] = transformed.z;
    }
    *plane = formatPlane(values);
    return true;
}

template <typename Transform>
bool transformSolid(Block& solid, Transform&& transform)
{
    bool changed = false;
    for (Block* side : solid.children("side")) changed = transformPlane(*side, transform) || changed;
    return changed;
}

Vec3 vectorAdd(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 vectorSubtract(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double vectorDot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 vectorNormalize(const Vec3& value, const Vec3& fallback)
{
    const double magnitude = std::sqrt(vectorDot(value, value));
    if (magnitude < 1e-9) return fallback;
    return {value.x / magnitude, value.y / magnitude, value.z / magnitude};
}

template <typename Transform>
bool transformOverlayData(Block& data, Transform&& transform)
{
    double originValues[3], uValues[3], vValues[3], normalValues[3];
    if (!data.value("BasisOrigin") ||
        !parseNumbers(*data.value("BasisOrigin"), originValues, 3) ||
        !data.value("BasisU") || !parseNumbers(*data.value("BasisU"), uValues, 3) ||
        !data.value("BasisV") || !parseNumbers(*data.value("BasisV"), vValues, 3)) {
        return false;
    }
    const Vec3 origin{originValues[0], originValues[1], originValues[2]};
    const Vec3 axisU{uValues[0], uValues[1], uValues[2]};
    const Vec3 axisV{vValues[0], vValues[1], vValues[2]};
    Vec3 axisNormal{};
    if (data.value("BasisNormal") && parseNumbers(*data.value("BasisNormal"), normalValues, 3))
        axisNormal = {normalValues[0], normalValues[1], normalValues[2]};
    else
        axisNormal = {axisU.y * axisV.z - axisU.z * axisV.y,
                      axisU.z * axisV.x - axisU.x * axisV.z,
                      axisU.x * axisV.y - axisU.y * axisV.x};

    std::array<Vec3, 4> coordinates{};
    std::array<double, 4> flags{};
    for (int index = 0; index < 4; ++index) {
        const std::string key = "uv" + std::to_string(index);
        double values[3];
        if (!data.value(key) || !parseNumbers(*data.value(key), values, 3)) return false;
        coordinates[static_cast<std::size_t>(index)] = {values[0], values[1], 0.0};
        flags[static_cast<std::size_t>(index)] = values[2];
    }

    const Vec3 transformedOrigin = transform(origin);
    const Vec3 transformedUEnd = transform(vectorAdd(origin, axisU));
    const Vec3 transformedVEnd = transform(vectorAdd(origin, axisV));
    const Vec3 transformedNormalEnd = transform(vectorAdd(origin, axisNormal));
    const Vec3 transformedU = vectorNormalize(vectorSubtract(transformedUEnd, transformedOrigin), axisU);
    const Vec3 transformedV = vectorNormalize(vectorSubtract(transformedVEnd, transformedOrigin), axisV);
    const Vec3 transformedNormal = vectorNormalize(
        vectorSubtract(transformedNormalEnd, transformedOrigin), axisNormal);

    for (int index = 0; index < 4; ++index) {
        const Vec3 world = vectorAdd(origin,
            vectorAdd({axisU.x * coordinates[static_cast<std::size_t>(index)].x,
                       axisU.y * coordinates[static_cast<std::size_t>(index)].x,
                       axisU.z * coordinates[static_cast<std::size_t>(index)].x},
                      {axisV.x * coordinates[static_cast<std::size_t>(index)].y,
                       axisV.y * coordinates[static_cast<std::size_t>(index)].y,
                       axisV.z * coordinates[static_cast<std::size_t>(index)].y}));
        const Vec3 transformedWorld = transform(world);
        const Vec3 delta = vectorSubtract(transformedWorld, transformedOrigin);
        const Vec3 updated{vectorDot(delta, transformedU), vectorDot(delta, transformedV),
                           flags[static_cast<std::size_t>(index)]};
        data.setValue("uv" + std::to_string(index), formatVec3(updated));
    }
    data.setValue("BasisOrigin", formatVec3(transformedOrigin));
    data.setValue("BasisU", formatVec3(transformedU));
    data.setValue("BasisV", formatVec3(transformedV));
    data.setValue("BasisNormal", formatVec3(transformedNormal));
    return true;
}

template <typename Transform>
bool transformEntity(Block& entity, Transform&& transform)
{
    bool changed = transformOrigin(entity, transform);
    for (Block* solid : entity.children("solid")) changed = transformSolid(*solid, transform) || changed;
    // Transform both the compiler-facing entity keys and Hammer's nested
    // helper data. They describe the same overlay and must remain in lockstep.
    if (valueIgnoreCase(entity, "BasisOrigin"))
        changed = transformOverlayData(entity, transform) || changed;
    for (Block* overlay : entity.children("overlaydata"))
        changed = transformOverlayData(*overlay, transform) || changed;
    return changed;
}

bool translateSolid(Block& solid, const Vec3& delta)
{
    return transformSolid(solid, [&](const Vec3& point) {
        return Vec3{point.x + delta.x, point.y + delta.y, point.z + delta.z};
    });
}

bool translateEntity(Block& entity, const Vec3& delta)
{
    return transformEntity(entity, [&](const Vec3& point) {
        return Vec3{point.x + delta.x, point.y + delta.y, point.z + delta.z};
    });
}

Block* findSolid(Block& block, int id)
{
    for (Entry& entry : block.entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child) continue;
        if (equalsIgnoreCase(entry.child->name, "solid") && parseInt(entry.child->value("id")) == id) return entry.child.get();
        if (Block* nested = findSolid(*entry.child, id)) return nested;
    }
    return nullptr;
}

const Block* findSolid(const Block& block, int id)
{
    for (const Entry& entry : block.entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child) continue;
        if (equalsIgnoreCase(entry.child->name, "solid") && parseInt(entry.child->value("id")) == id) return entry.child.get();
        if (const Block* nested = findSolid(*entry.child, id)) return nested;
    }
    return nullptr;
}

bool eraseSolid(Block& block, int id)
{
    for (auto it = block.entries.begin(); it != block.entries.end(); ++it) {
        if (it->kind != Entry::Kind::ChildBlock || !it->child) continue;
        if (equalsIgnoreCase(it->child->name, "solid") && parseInt(it->child->value("id")) == id) {
            block.entries.erase(it);
            return true;
        }
        if (eraseSolid(*it->child, id)) return true;
    }
    return false;
}

void collectSelectable(const Block& block, std::vector<ObjectRef>& result)
{
    for (const Entry& entry : block.entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child) continue;
        if (equalsIgnoreCase(entry.child->name, "solid")) {
            const int id = parseInt(entry.child->value("id"));
            if (id >= 0) result.push_back({ObjectType::Solid, id});
        } else {
            collectSelectable(*entry.child, result);
        }
    }
}

int solidOwnerEntityId(const Block& block, int solidId, int currentEntityId = -1)
{
    int owner = currentEntityId;
    if (equalsIgnoreCase(block.name, "entity")) owner = parseInt(block.value("id"));
    for (const Entry& entry : block.entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child) continue;
        if (equalsIgnoreCase(entry.child->name, "solid") && parseInt(entry.child->value("id")) == solidId) return owner;
        const int nested = solidOwnerEntityId(*entry.child, solidId, owner);
        if (nested >= 0) return nested;
    }
    return -1;
}

int solidOwnerEntityId(const Document& document, int solidId)
{
    for (const Block& root : document.roots()) {
        const int owner = solidOwnerEntityId(root, solidId);
        if (owner >= 0) return owner;
    }
    return -1;
}

std::vector<Property> blockProperties(const Block& block)
{
    std::vector<Property> result;
    for (const Entry& entry : block.entries) if (entry.kind == Entry::Kind::KeyValue) result.push_back({entry.key, entry.value});
    return result;
}

void replaceBlockProperties(Block& block, const std::vector<Property>& properties)
{
    std::vector<Entry> replacement;
    replacement.reserve(block.entries.size() + properties.size());
    for (const Property& property : properties) replacement.emplace_back(property.key, property.value);
    for (Entry& entry : block.entries) if (entry.kind == Entry::Kind::ChildBlock) replacement.push_back(std::move(entry));
    block.entries = std::move(replacement);
}

const std::string* valueIgnoreCase(const Block& block, std::string_view key)
{
    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::KeyValue && equalsIgnoreCase(entry.key, key))
            return &entry.value;
    }
    return nullptr;
}

void synchronizeOverlayData(Block& entity)
{
    const std::string* classname = valueIgnoreCase(entity, "classname");
    if (!classname || !equalsIgnoreCase(*classname, "info_overlay")) return;

    auto blocks = entity.children("overlaydata");
    Block* data = blocks.empty() ? &entity.appendChild("overlaydata") : blocks.front();
    static constexpr std::string_view Keys[] = {
        "material", "StartU", "EndU", "StartV", "EndV",
        "BasisOrigin", "BasisU", "BasisV", "BasisNormal",
        "uv0", "uv1", "uv2", "uv3", "sides"
    };
    for (std::string_view key : Keys) {
        if (const std::string* value = valueIgnoreCase(entity, key))
            data->setValue(std::string(key), *value);
    }
}

void expand(Bounds& bounds, const Vec3& point)
{
    if (!bounds.valid) {
        bounds.minimum = bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

int maximumId(const Block& block)
{
    int result = parseInt(block.value("id"));
    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::ChildBlock && entry.child) result = std::max(result, maximumId(*entry.child));
    }
    return result;
}

// Paste Special turns each copy about a fixed pivot. Solids move their
// vertices; entities move their origin and their "angles" together, exactly as
// EditorModel::rotateObject does for an interactive rotate.
bool rotateBlockAboutPivot(Block& block, ObjectType type, const Vec3& degrees, const Vec3& pivot)
{
    constexpr double DegToRad = 3.14159265358979323846 / 180.0;
    const std::pair<double, RotationAxis> steps[] = {
        {degrees.x, RotationAxis::X}, {degrees.y, RotationAxis::Y}, {degrees.z, RotationAxis::Z}};
    bool changed = false;
    for (const auto& [angle, axis] : steps) {
        if (angle == 0.0) continue;
        const double radians = angle * DegToRad;
        const auto transform = [&](const Vec3& point) { return rotatePoint(point, radians, axis, pivot); };
        if (type == ObjectType::Solid) {
            changed = transformSolid(block, transform) || changed;
        } else {
            changed = transformEntity(block, transform) || changed;
            changed = rotateEntityAngles(block, radians, axis) || changed;
        }
    }
    return changed;
}

// Every targetname in the map, so a generated one can be guaranteed not to
// collide with a name the map already uses.
void collectTargetNames(const Block& block, std::unordered_set<std::string>& names)
{
    if (const std::string* name = valueIgnoreCase(block, "targetname"); name && !name->empty()) {
        names.insert(*name);
    }
    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::ChildBlock && entry.child) collectTargetNames(*entry.child, names);
    }
}

// "crate" -> "crate1", "crate2"...; a name that already ends in digits has them
// replaced rather than extended, so copying "crate1" gives "crate2", not
// "crate11". Names taken by the map or by an earlier copy are skipped.
std::string uniqueTargetName(const std::string& base, std::unordered_set<std::string>& taken)
{
    std::string stem = base;
    while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') stem.pop_back();
    if (stem.empty()) stem = base.empty() ? std::string{"entity"} : base;
    for (int suffix = 1; suffix < 1000000; ++suffix) {
        std::string candidate = stem + std::to_string(suffix);
        if (taken.insert(candidate).second) return candidate;
    }
    return base;
}

// An output's value is "target<sep>input<sep>parameter<sep>delay<sep>times".
// VMF has used both ',' and ESC as the separator; only the target — the part
// before the first of either — is ours to rewrite, and only when this paste
// renamed the entity it points at. Connections aimed outside the pasted set
// keep pointing where they did.
void rewriteConnectionTargets(Block& entity, const std::unordered_map<std::string, std::string>& renamed)
{
    for (Block* connections : entity.children("connections")) {
        for (Entry& entry : connections->entries) {
            if (entry.kind != Entry::Kind::KeyValue) continue;
            const std::size_t separator = entry.value.find_first_of(",\x1b");
            const std::string target = entry.value.substr(0, separator);
            const auto replacement = renamed.find(target);
            if (replacement == renamed.end()) continue;
            entry.value = separator == std::string::npos
                              ? replacement->second
                              : replacement->second + entry.value.substr(separator);
        }
    }
}

// Hammer's groups live in the file as a "group" block in the world plus a
// "groupid" in each member's editor data. The port writes that faithfully so
// the result round-trips, but does not yet treat a group as a selection unit —
// Tools > Group is still unimplemented.
void setGroupId(Block& object, int groupId)
{
    const std::vector<Block*> editors = object.children("editor");
    Block& editor = editors.empty() ? object.appendChild("editor") : *editors.front();
    editor.setValue("groupid", std::to_string(groupId));
}

// The group an object's editor block points at, or -1 for none.
int groupIdOf(const Block& object)
{
    for (const Block* editor : object.children("editor")) {
        if (const std::string* text = editor->value("groupid")) return parseInt(text);
    }
    return -1;
}

void setEditorMetadata(Block& owner, std::string color)
{
    Block& editor = owner.appendChild("editor");
    editor.setValue("color", std::move(color));
    editor.setValue("visgroupshown", "1");
    editor.setValue("visgroupautoshown", "1");
}

} // namespace

EditorModel::EditorModel() : document_(Document::createDefault()) { resetNextId(); }
EditorModel::EditorModel(Document document) : document_(std::move(document)) { resetNextId(); }

void EditorModel::setDocument(Document document)
{
    document_ = std::move(document);
    selection_.clear();
    undo_.clear();
    redo_.clear();
    transaction_.reset();
    resetNextId();
}

bool EditorModel::isSelected(const ObjectRef& object) const
{
    return std::find(selection_.begin(), selection_.end(), object) != selection_.end();
}

void EditorModel::select(const ObjectRef& object, bool toggle, bool additive)
{
    if (!findObject(document_, object)) return;
    const auto existing = std::find(selection_.begin(), selection_.end(), object);
    if (toggle) {
        if (existing != selection_.end()) selection_.erase(existing);
        else selection_.push_back(object);
        return;
    }
    if (!additive) selection_.clear();
    if (std::find(selection_.begin(), selection_.end(), object) == selection_.end()) selection_.push_back(object);
}

void EditorModel::setSelection(std::vector<ObjectRef> selection)
{
    selection_ = std::move(selection);
    validateSelection();
    std::sort(selection_.begin(), selection_.end(), [](const ObjectRef& a, const ObjectRef& b) {
        if (a.type != b.type) return a.type < b.type;
        return a.id < b.id;
    });
    selection_.erase(std::unique(selection_.begin(), selection_.end()), selection_.end());
}

void EditorModel::clearSelection() { selection_.clear(); }

void EditorModel::selectAll()
{
    selection_.clear();
    for (const Block& root : document_.roots()) {
        if (equalsIgnoreCase(root.name, "entity")) {
            const bool hasSolids = !root.children("solid").empty();
            const int id = parseInt(root.value("id"));
            if (!hasSolids && id >= 0) selection_.push_back({ObjectType::Entity, id});
        }
        collectSelectable(root, selection_);
    }
}

Bounds EditorModel::selectionBounds() const
{
    Bounds bounds;
    if (selection_.empty()) return bounds;
    // Only the selected solids are intersected into geometry. Building the
    // whole scene here made every selection change cost a full map rebuild.
    const auto accumulate = [&](const Block& owner, int ownerEntityId) {
        const bool ownerSelected = ownerEntityId >= 0 && isSelected({ObjectType::Entity, ownerEntityId});
        for (const Block* solid : owner.children("solid")) {
            const int id = parseInt(solid->value("id"));
            if (!ownerSelected && !isSelected({ObjectType::Solid, id})) continue;
            const BrushGeometry brush = buildSolidGeometry(*solid, ownerEntityId);
            for (const Vec3& vertex : brush.vertices) expand(bounds, vertex);
        }
    };
    for (const Block& root : document_.roots()) {
        if (root.name == "world" || root.name == "WORLD") {
            accumulate(root, -1);
            continue;
        }
        if (!equalsIgnoreCase(root.name, "entity")) continue;
        const int entityId = parseInt(root.value("id"));
        accumulate(root, entityId);
        // buildScene only creates a marker for entities that carry an origin.
        const std::string* origin = root.value("origin");
        double values[3];
        if (isSelected({ObjectType::Entity, entityId}) && origin && parseNumbers(*origin, values, 3))
            expand(bounds, Vec3{values[0], values[1], values[2]});
    }
    return bounds;
}

std::vector<Property> EditorModel::selectedProperties() const
{
    if (selection_.size() != 1) return {};
    const Block* block = findObject(document_, selection_.front());
    return block ? blockProperties(*block) : std::vector<Property>{};
}

bool EditorModel::replaceSelectedProperties(const std::vector<Property>& properties, std::string label)
{
    if (selection_.size() != 1) return false;
    Block* block = findObject(document_, selection_.front());
    if (!block || blockProperties(*block) == properties) return false;
    pushUndo(std::move(label));
    replaceBlockProperties(*block, properties);
    synchronizeOverlayData(*block);
    document_.markDirty();
    validateSelection();
    resetNextId();
    return true;
}

namespace {

// "target,input,parameter,delay,timesToFire". Modern Hammer writes the 0x1B
// ESC separator when a field contains a comma; both are accepted on read and
// ESC is only written when needed.
constexpr char ConnectionEscapeDelimiter = '\x1b';

std::vector<std::string> splitConnectionValue(const std::string& value)
{
    const char delimiter = value.find(ConnectionEscapeDelimiter) != std::string::npos
        ? ConnectionEscapeDelimiter : ',';
    std::vector<std::string> fields;
    std::string::size_type start = 0;
    while (fields.size() < 5) {
        const auto end = value.find(delimiter, start);
        if (end == std::string::npos) {
            fields.push_back(value.substr(start));
            break;
        }
        fields.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    fields.resize(5);
    return fields;
}

std::string formatConnectionDelay(double delay)
{
    std::string text = std::to_string(delay);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text.empty() ? "0" : text;
}

std::vector<EditorModel::EntityConnection> blockConnections(const Block& block)
{
    std::vector<EditorModel::EntityConnection> result;
    for (const Block* connections : block.children("connections")) {
        for (const Entry& entry : connections->entries) {
            if (entry.kind != Entry::Kind::KeyValue) continue;
            const std::vector<std::string> fields = splitConnectionValue(entry.value);
            EditorModel::EntityConnection connection;
            connection.output = entry.key;
            connection.target = fields[0];
            connection.input = fields[1];
            connection.parameter = fields[2];
            try { connection.delay = fields[3].empty() ? 0.0 : std::stod(fields[3]); }
            catch (...) { connection.delay = 0.0; }
            try { connection.timesToFire = fields[4].empty() ? -1 : std::stoi(fields[4]); }
            catch (...) { connection.timesToFire = -1; }
            result.push_back(std::move(connection));
        }
    }
    return result;
}

void replaceBlockConnections(Block& block,
                             const std::vector<EditorModel::EntityConnection>& connections)
{
    block.entries.erase(std::remove_if(block.entries.begin(), block.entries.end(),
                                       [](const Entry& entry) {
                                           return entry.kind == Entry::Kind::ChildBlock &&
                                                  entry.child &&
                                                  equalsIgnoreCase(entry.child->name,
                                                                   "connections");
                                       }),
                        block.entries.end());
    if (connections.empty()) return;
    Block& chunk = block.appendChild("connections");
    for (const EditorModel::EntityConnection& connection : connections) {
        const std::string fields[5] = {connection.target, connection.input,
                                       connection.parameter,
                                       formatConnectionDelay(connection.delay),
                                       std::to_string(connection.timesToFire)};
        char delimiter = ',';
        for (const std::string& field : fields) {
            if (field.find(',') != std::string::npos) delimiter = ConnectionEscapeDelimiter;
        }
        std::string value;
        for (int index = 0; index < 5; ++index) {
            if (index != 0) value += delimiter;
            value += fields[index];
        }
        chunk.entries.emplace_back(connection.output, value);
    }
}

} // namespace

std::vector<EditorModel::EntityConnection> EditorModel::selectedConnections() const
{
    if (selection_.size() != 1 || selection_.front().type != ObjectType::Entity) return {};
    const Block* block = findObject(document_, selection_.front());
    return block ? blockConnections(*block) : std::vector<EntityConnection>{};
}

bool EditorModel::replaceSelectedPropertiesAndConnections(
    const std::vector<Property>& properties, const std::vector<EntityConnection>& connections,
    std::string label)
{
    if (selection_.size() != 1) return false;
    Block* block = findObject(document_, selection_.front());
    if (!block) return false;
    const bool propertiesChanged = blockProperties(*block) != properties;
    const bool connectionsChanged = blockConnections(*block) != connections;
    if (!propertiesChanged && !connectionsChanged) return false;
    pushUndo(std::move(label));
    if (propertiesChanged) {
        replaceBlockProperties(*block, properties);
        synchronizeOverlayData(*block);
    }
    if (connectionsChanged) replaceBlockConnections(*block, connections);
    document_.markDirty();
    validateSelection();
    resetNextId();
    return true;
}

std::vector<Property> EditorModel::worldProperties() const
{
    const Block* world = document_.firstRoot("world");
    return world ? blockProperties(*world) : std::vector<Property>{};
}

bool EditorModel::replaceWorldProperties(const std::vector<Property>& properties, std::string label)
{
    Block* world = document_.firstRoot("world");
    if (!world) return false;
    if (blockProperties(*world) == properties) return false;
    pushUndo(std::move(label));
    replaceBlockProperties(*world, properties);
    document_.markDirty();
    resetNextId();
    return true;
}

bool EditorModel::translateSelection(const Vec3& delta, std::string label)
{
    if (selection_.empty() || (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0)) return false;
    pushUndo(std::move(label));
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = translateObject(document_, object, delta) || changed;
    }
    if (!changed) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return false;
    }
    document_.markDirty();
    validateSelection();
    return true;
}

bool EditorModel::scaleSelection(const Vec3& factors, const Vec3& pivot, std::string label)
{
    if (selection_.empty() || factors.x <= 0.0001 || factors.y <= 0.0001 || factors.z <= 0.0001 ||
        !std::isfinite(factors.x) || !std::isfinite(factors.y) || !std::isfinite(factors.z)) return false;
    if (factors.x == 1.0 && factors.y == 1.0 && factors.z == 1.0) return false;
    pushUndo(std::move(label));
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = scaleObject(document_, object, factors, pivot) || changed;
    }
    if (!changed) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return false;
    }
    document_.markDirty();
    return true;
}

bool EditorModel::rotateSelection(double radians, RotationAxis axis, const Vec3& pivot, std::string label)
{
    if (selection_.empty() || !std::isfinite(radians) || std::abs(radians) < 1e-12) return false;
    pushUndo(std::move(label));
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = rotateObject(document_, object, radians, axis, pivot) || changed;
    }
    if (!changed) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return false;
    }
    document_.markDirty();
    return true;
}

bool EditorModel::deleteSelection(std::string label)
{
    if (selection_.empty()) return false;
    pushUndo(std::move(label));
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = eraseObject(document_, object) || changed;
    }
    if (!changed) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return false;
    }
    selection_.clear();
    // Deleting a group's last member leaves an orphan "group" block, and the
    // VDC's "a vis group with no objects tied to it will get removed from the
    // list" says the same about visgroups (CMapDoc::VisGroups_PurgeGroups).
    // Both purges belong inside this undo step, so one undo brings the group
    // back along with its objects.
    purgeEmptyGroups(document_);
    purgeEmptyVisGroups(document_);
    document_.markDirty();
    resetNextId();
    return true;
}

bool EditorModel::applyDocumentEdit(const std::function<bool(Document&)>& edit, std::string label)
{
    if (!edit) return false;
    pushUndo(std::move(label));
    if (!edit(document_)) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return false;
    }
    document_.markDirty();
    // A fix may have deleted an object that was selected, and ids of anything
    // the edit created have to be accounted for before the next allocation.
    validateSelection();
    resetNextId();
    return true;
}

ClipboardData EditorModel::copySelection() const
{
    ClipboardData clipboard;
    clipboard.bounds = selectionBounds();
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        if (const Block* block = findObject(document_, object)) clipboard.objects.push_back({object.type, *block});
    }
    return clipboard;
}

bool EditorModel::paste(const ClipboardData& clipboard, const Vec3& offset, std::string label)
{
    // A plain paste is one Paste Special copy, nudged clear of the original.
    PasteSpecialOptions options;
    options.copies = 1;
    options.offset = offset;
    return pasteSpecial(clipboard, options, std::move(label));
}

bool EditorModel::pasteSpecial(const ClipboardData& clipboard, const PasteSpecialOptions& options,
                               std::string label)
{
    if (clipboard.empty()) return false;
    const int copies = std::max(1, options.copies);
    pushUndo(std::move(label));
    std::vector<ObjectRef> pasted;

    auto remap = [&](auto&& self, Block& block) -> void {
        for (Entry& entry : block.entries) {
            if (entry.kind == Entry::Kind::KeyValue && equalsIgnoreCase(entry.key, "id") && parseInt(&entry.value) >= 0) {
                entry.value = std::to_string(allocateId());
            } else if (entry.kind == Entry::Kind::ChildBlock && entry.child) {
                self(self, *entry.child);
            }
        }
    };

    Block* world = document_.firstRoot("world");
    if (!world) {
        Block& createdWorld = document_.appendRoot("world");
        createdWorld.setValue("id", std::to_string(allocateId()));
        createdWorld.setValue("mapversion", "1");
        createdWorld.setValue("classname", "worldspawn");
        world = &createdWorld;
    }

    // Rotation turns about the center of the clipboard contents as they were
    // cut — the "location and rotation of the original" the dialog's
    // start-at-original option refers to. Deliberate reading of a dialog whose
    // original code the port does not have; the alternative (each copy turning
    // about its own displaced center) makes a staircase spiral.
    Vec3 pivot{};
    if (clipboard.bounds.valid) {
        pivot = {(clipboard.bounds.minimum.x + clipboard.bounds.maximum.x) * 0.5,
                 (clipboard.bounds.minimum.y + clipboard.bounds.maximum.y) * 0.5,
                 (clipboard.bounds.minimum.z + clipboard.bounds.maximum.z) * 0.5};
    }
    // With start-at-original off the whole run is shifted so the clipboard's
    // center lands where the 2D views are looking.
    Vec3 base{};
    if (!options.startAtOriginal && clipboard.bounds.valid) {
        base = {options.viewCenter.x - pivot.x, options.viewCenter.y - pivot.y,
                options.viewCenter.z - pivot.z};
    }

    std::unordered_set<std::string> takenNames;
    if (options.uniqueEntityNames) {
        for (const Block& root : document_.roots()) collectTargetNames(root, takenNames);
    }

    int groupId = -1;
    if (options.groupCopies) {
        groupId = allocateId();
        Block& group = world->appendChild("group");
        group.setValue("id", std::to_string(groupId));
        setEditorMetadata(group, "0 180 0");
    }

    for (int copy = 1; copy <= copies; ++copy) {
        // Every copy is built from the clipboard rather than from the previous
        // copy, so N steps is exactly N times one step with no drift.
        const Vec3 delta{base.x + options.offset.x * copy,
                         base.y + options.offset.y * copy,
                         base.z + options.offset.z * copy};
        const Vec3 degrees{options.rotation.x * copy, options.rotation.y * copy,
                           options.rotation.z * copy};

        std::vector<std::pair<ObjectType, Block>> clones;
        clones.reserve(clipboard.objects.size());
        // Source group id -> this copy's replacement for it.
        std::unordered_map<int, int> clonedGroups;
        // Renames are per copy: copy 3's connections must point at copy 3's
        // entities, not at copy 1's.
        std::unordered_map<std::string, std::string> renamed;

        for (const ClipboardObject& item : clipboard.objects) {
            Block clone = item.block;
            remap(remap, clone);
            rotateBlockAboutPivot(clone, item.type, degrees, pivot);
            if (item.type == ObjectType::Entity) {
                translateEntity(clone, delta);
                if (std::string* name = clone.value("targetname"); name && !name->empty()) {
                    // Prefix first, then the uniqueness suffix, so a prefixed
                    // run stays readable: "west_crate", "west_crate1"...
                    std::string updated = options.namePrefix + *name;
                    if (options.uniqueEntityNames) updated = uniqueTargetName(updated, takenNames);
                    if (updated != *name) renamed.emplace(*name, updated);
                    *name = std::move(updated);
                }
            } else {
                translateSolid(clone, delta);
            }
            if (groupId >= 0) {
                setGroupId(clone, groupId);
            } else if (const int sourceGroup = groupIdOf(clone); sourceGroup >= 0) {
                // The clipboard block still names the group the ORIGINAL is in.
                // Left alone, every paste would silently join that group. Each
                // copy instead gets its own group per source group, so a copied
                // group stays grouped without swallowing the original.
                auto [entry, inserted] = clonedGroups.try_emplace(sourceGroup, 0);
                if (inserted) entry->second = allocateId();
                setGroupId(clone, entry->second);
            }
            clones.emplace_back(item.type, std::move(clone));
        }

        // Every group a copy needs must exist as a block in the world, or the
        // members point at nothing and read as ungrouped.
        for (const auto& [sourceGroup, newGroup] : clonedGroups) {
            Block* destinationWorld = document_.firstRoot("world");
            if (!destinationWorld) break;
            Block& group = destinationWorld->appendChild("group");
            group.setValue("id", std::to_string(newGroup));
            setEditorMetadata(group, "0 180 0");
        }
        clonedGroups.clear();

        if (!renamed.empty()) {
            for (auto& [type, clone] : clones) {
                if (type == ObjectType::Entity) rewriteConnectionTargets(clone, renamed);
            }
        }

        for (auto& [type, clone] : clones) {
            const int topId = parseInt(clone.value("id"));
            if (type == ObjectType::Entity) {
                document_.roots().push_back(std::move(clone));
                pasted.push_back({ObjectType::Entity, topId});
            } else {
                // Re-looked up every time: pushing a root can move the world.
                Block* destinationWorld = document_.firstRoot("world");
                if (!destinationWorld) continue;
                destinationWorld->entries.emplace_back(std::move(clone));
                pasted.push_back({ObjectType::Solid, topId});
            }
        }
    }

    if (pasted.empty()) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return false;
    }
    selection_ = std::move(pasted);
    document_.markDirty();
    return true;
}

namespace {

// Clipper3D::SetClipObjects walks the selection and adds every CMapSolid it
// finds, including the solids owned by selected brush entities
// (EnumChildren( AddToClipList, MAPCLASS_TYPE( CMapSolid ) )).
void collectSolidIds(const Block& block, std::vector<int>& result)
{
    for (const Entry& entry : block.entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child) continue;
        if (equalsIgnoreCase(entry.child->name, "solid")) {
            const int id = parseInt(entry.child->value("id"));
            if (id >= 0) result.push_back(id);
        } else {
            collectSolidIds(*entry.child, result);
        }
    }
}

} // namespace

std::vector<const Block*> EditorModel::selectedSolidBlocks() const
{
    std::vector<const Block*> solids;
    for (const ObjectRef& object : selection_) {
        const Block* block = findObject(document_, object);
        if (!block) continue;
        if (object.type == ObjectType::Solid) {
            solids.push_back(block);
            continue;
        }
        std::vector<int> ids;
        collectSolidIds(*block, ids);
        for (const int id : ids) {
            if (const Block* child = findObject(document_, {ObjectType::Solid, id})) solids.push_back(child);
        }
    }
    return solids;
}

EditorModel::ClipPreview EditorModel::previewClip(const ClipPlane& plane, ClipMode mode) const
{
    ClipPreview preview;
    if (!plane.valid()) return preview;

    int nextId = 0;
    const std::function<int()> allocate = [&nextId] { return nextId++; };

    for (const Block* solid : selectedSolidBlocks()) {
        if (solidHasDisplacement(*solid)) continue;
        const SolidPlaneRelation relation = classifySolid(*solid, plane);
        const bool keepFrontHalf = mode != ClipMode::Back;
        const bool keepBackHalf = mode != ClipMode::Front;

        if (relation != SolidPlaneRelation::Split) {
            // Solids that do not straddle the plane survive whole or vanish
            // whole, exactly as CMapSolid::Split's single-sided path decides.
            const bool keep = relation == SolidPlaneRelation::Front ? keepFrontHalf : keepBackHalf;
            FacePolygons polygons = solidFacePolygons(*solid);
            if (polygons.empty()) continue;
            (keep ? preview.kept : preview.discarded).push_back(std::move(polygons));
            continue;
        }

        for (int side = 0; side < 2; ++side) {
            const bool front = side == 0;
            const std::optional<Block> piece = clipSolid(*solid, plane, front, allocate);
            if (!piece) continue;
            FacePolygons polygons = solidFacePolygons(*piece);
            if (polygons.empty()) continue;
            const bool keep = front ? keepFrontHalf : keepBackHalf;
            (keep ? preview.kept : preview.discarded).push_back(std::move(polygons));
        }
    }
    return preview;
}

bool EditorModel::clipSelection(const ClipPlane& plane, ClipMode mode, std::string label)
{
    if (!plane.valid()) return false;

    std::vector<int> targets;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            targets.push_back(object.id);
        } else if (const Block* entity = findObject(document_, object)) {
            collectSolidIds(*entity, targets);
        }
    }
    if (targets.empty()) return false;

    const Snapshot snapshot{document_, selection_, label};

    const bool keepFrontHalf = mode != ClipMode::Back;
    const bool keepBackHalf = mode != ClipMode::Front;
    const std::function<int()> allocate = [this] { return allocateId(); };

    bool changed = false;
    std::vector<ObjectRef> newSelection;
    const std::function<void(Block&)> visit = [&](Block& block) {
        std::vector<Entry> replacement;
        replacement.reserve(block.entries.size());
        for (Entry& entry : block.entries) {
            if (entry.kind != Entry::Kind::ChildBlock || !entry.child) {
                replacement.push_back(std::move(entry));
                continue;
            }
            if (!equalsIgnoreCase(entry.child->name, "solid")) {
                visit(*entry.child);
                replacement.push_back(std::move(entry));
                continue;
            }
            const int id = parseInt(entry.child->value("id"));
            if (std::find(targets.begin(), targets.end(), id) == targets.end() ||
                solidHasDisplacement(*entry.child)) {
                replacement.push_back(std::move(entry));
                continue;
            }

            const SolidPlaneRelation relation = classifySolid(*entry.child, plane);
            if (relation != SolidPlaneRelation::Split) {
                const bool keep = relation == SolidPlaneRelation::Front ? keepFrontHalf : keepBackHalf;
                if (keep) {
                    newSelection.push_back({ObjectType::Solid, id});
                    replacement.push_back(std::move(entry));
                } else {
                    // Clipper3D::RemoveOrigSolid
                    changed = true;
                }
                continue;
            }

            // Clipper3D::SaveClipResults: add the surviving halves to the
            // original solid's parent, then remove the original. The original
            // never survives this branch, so the document changed even if no
            // half could be built (RemoveOrigSolid runs unconditionally there).
            changed = true;
            for (int side = 0; side < 2; ++side) {
                const bool front = side == 0;
                if (!(front ? keepFrontHalf : keepBackHalf)) continue;
                std::optional<Block> piece = clipSolid(*entry.child, plane, front, allocate);
                if (!piece) continue;
                const int pieceId = parseInt(piece->value("id"));
                if (pieceId >= 0) newSelection.push_back({ObjectType::Solid, pieceId});
                replacement.emplace_back(std::move(*piece));
            }
        }
        block.entries = std::move(replacement);
    };

    for (Block& root : document_.roots()) visit(root);
    if (!changed) return false;

    undo_.push_back(snapshot);
    redo_.clear();
    // Selected brush entities stay selected; their solids were replaced in place.
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Entity) newSelection.push_back(object);
    }
    selection_ = std::move(newSelection);
    validateSelection();
    document_.markDirty();
    return true;
}

bool EditorModel::carveSelection(std::string label)
{
    // Working copies of the carvers: the document mutates underneath us while
    // targets are replaced, so the Block pointers from selectedSolidBlocks()
    // must not be held across the visit.
    std::vector<Block> carvers;
    std::vector<int> carverIds;
    for (const Block* solid : selectedSolidBlocks()) {
        if (solidHasDisplacement(*solid)) continue;
        carvers.push_back(*solid);
        carverIds.push_back(parseInt(solid->value("id")));
    }
    if (carvers.empty()) return false;

    const Snapshot snapshot{document_, selection_, std::move(label)};
    const std::function<int()> allocate = [this] { return allocateId(); };
    bool changed = false;

    const std::function<void(Block&)> visit = [&](Block& block) {
        std::vector<Entry> replacement;
        replacement.reserve(block.entries.size());
        for (Entry& entry : block.entries) {
            if (entry.kind != Entry::Kind::ChildBlock || !entry.child) {
                replacement.push_back(std::move(entry));
                continue;
            }
            if (!equalsIgnoreCase(entry.child->name, "solid")) {
                visit(*entry.child);
                replacement.push_back(std::move(entry));
                continue;
            }
            const int id = parseInt(entry.child->value("id"));
            if (std::find(carverIds.begin(), carverIds.end(), id) != carverIds.end() ||
                solidHasDisplacement(*entry.child)) {
                replacement.push_back(std::move(entry));
                continue;
            }

            // Later carvers cut the pieces the earlier ones produced, so a
            // multi-solid carver behaves as the union of its volumes.
            bool touched = false;
            std::vector<Block> pieces;
            pieces.push_back(std::move(*entry.child));
            for (const Block& carver : carvers) {
                std::vector<Block> next;
                for (Block& piece : pieces) {
                    std::optional<std::vector<Block>> carved = carveSolid(piece, carver, allocate);
                    if (!carved) {
                        next.push_back(std::move(piece));
                        continue;
                    }
                    touched = true;
                    for (Block& fragment : *carved) next.push_back(std::move(fragment));
                }
                pieces = std::move(next);
            }
            if (touched) changed = true;
            for (Block& piece : pieces) replacement.emplace_back(std::move(piece));
        }
        block.entries = std::move(replacement);
    };

    // Brush entities that lose every solid must not linger as invisible,
    // unselectable point-entity impostors; note who owned solids beforehand.
    std::unordered_set<int> hadSolids;
    for (const Block& root : document_.roots()) {
        if (equalsIgnoreCase(root.name, "entity") && !root.children("solid").empty()) {
            hadSolids.insert(parseInt(root.value("id")));
        }
    }

    for (Block& root : document_.roots()) visit(root);
    if (!changed) return false;

    std::vector<Block>& roots = document_.roots();
    roots.erase(std::remove_if(roots.begin(), roots.end(), [&](const Block& root) {
        return equalsIgnoreCase(root.name, "entity") &&
               hadSolids.contains(parseInt(root.value("id"))) &&
               root.children("solid").empty();
    }), roots.end());

    undo_.push_back(snapshot);
    redo_.clear();
    // The carvers are untouched and stay selected.
    validateSelection();
    document_.markDirty();
    return true;
}

std::optional<ObjectRef> EditorModel::tieSelectionToEntity(std::string classname,
                                                           std::string label)
{
    if (classname.empty() || selection_.empty()) return std::nullopt;

    // The solids to move: selected world solids plus every solid of a selected
    // brush entity (OnEditToEntity gathers CMapSolid children of entities too).
    std::unordered_set<int> solidIds;
    std::unordered_set<int> donorEntityIds;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            solidIds.insert(object.id);
        } else {
            donorEntityIds.insert(object.id);
        }
    }
    const auto blockId = [](const Block& block) { return parseInt(block.value("id")); };
    // Only entities that actually own solids take part; selected point
    // entities are left untouched (Hammer skips placeholders).
    std::unordered_set<int> brushDonorIds;
    for (Block& root : document_.roots()) {
        if (!equalsIgnoreCase(root.name, "entity")) continue;
        const int id = blockId(root);
        if (!donorEntityIds.contains(id)) continue;
        const auto solids = root.children("solid");
        if (solids.empty()) continue;
        brushDonorIds.insert(id);
        for (const Block* solid : solids) solidIds.insert(blockId(*solid));
    }
    if (solidIds.empty()) return std::nullopt;

    pushUndo(std::move(label));

    // Detach the solid blocks from their current owners.
    std::vector<Entry> movedSolids;
    const auto detachFrom = [&](Block& owner) {
        for (auto it = owner.entries.begin(); it != owner.entries.end();) {
            if (it->kind == Entry::Kind::ChildBlock && it->child &&
                equalsIgnoreCase(it->child->name, "solid") &&
                solidIds.contains(blockId(*it->child))) {
                movedSolids.push_back(std::move(*it));
                it = owner.entries.erase(it);
            } else {
                ++it;
            }
        }
    };
    for (Block& root : document_.roots()) {
        if (equalsIgnoreCase(root.name, "world") || equalsIgnoreCase(root.name, "entity")) {
            detachFrom(root);
        }
    }
    if (movedSolids.empty()) {
        // Nothing detached; the undo entry would be a no-op, drop it.
        undo_.pop_back();
        return std::nullopt;
    }

    // A brush entity whose every solid moved away is deleted, as Hammer's
    // OnEditToEntity does when re-tying.
    std::erase_if(document_.roots(), [&](const Block& root) {
        return equalsIgnoreCase(root.name, "entity") &&
               brushDonorIds.contains(blockId(root)) && root.children("solid").empty();
    });

    Block entity("entity");
    const int entityId = allocateId();
    entity.setValue("id", std::to_string(entityId));
    entity.setValue("classname", std::move(classname));
    for (Entry& solid : movedSolids) entity.entries.push_back(std::move(solid));
    setEditorMetadata(entity, "220 30 220");
    document_.roots().push_back(std::move(entity));

    selection_ = {{ObjectType::Entity, entityId}};
    document_.markDirty();
    return selection_.front();
}

bool EditorModel::applyMorph(const std::vector<MorphSolid>& solids, std::string label)
{
    // Morph3D::SetEmpty walks the structured solids, saves each one for undo
    // and converts it back to a CMapSolid. Here the whole batch is one undo
    // entry, matching the single MarkUndoPosition the original takes.
    std::vector<std::pair<int, Block>> updates;
    for (const MorphSolid& morph : solids) {
        if (!morph.moved()) continue;
        const Block* original = nullptr;
        for (const Block& root : document_.roots()) {
            if (equalsIgnoreCase(root.name, "solid") && parseInt(root.value("id")) == morph.solidId) {
                original = &root;
                break;
            }
            if ((original = findSolid(root, morph.solidId)) != nullptr) break;
        }
        if (!original) continue;
        std::optional<Block> updated = morphSolid(*original, morph);
        if (!updated) continue;  // collapsed solid: keep the original geometry
        updates.emplace_back(morph.solidId, std::move(*updated));
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (auto& [id, block] : updates) {
        for (Block& root : document_.roots()) {
            if (Block* target = findSolid(root, id)) {
                *target = std::move(block);
                break;
            }
        }
    }
    document_.markDirty();
    return true;
}

namespace {

// The VMF side chunk holding one face of one solid.
Block* findSide(Document& document, int solidId, int sideId)
{
    for (Block& root : document.roots()) {
        Block* solid = equalsIgnoreCase(root.name, "solid") && parseInt(root.value("id")) == solidId
                           ? &root : findSolid(root, solidId);
        if (!solid) continue;
        for (Block* side : solid->children("side")) {
            if (parseInt(side->value("id")) == sideId) return side;
        }
    }
    return nullptr;
}

const Block* findSide(const Document& document, int solidId, int sideId)
{
    for (const Block& root : document.roots()) {
        const Block* solid = equalsIgnoreCase(root.name, "solid") && parseInt(root.value("id")) == solidId
                                 ? &root : findSolid(root, solidId);
        if (!solid) continue;
        for (const Block* side : solid->children("side")) {
            if (parseInt(side->value("id")) == sideId) return side;
        }
    }
    return nullptr;
}

bool sameVector(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool sameFaceTexture(const FaceTexture& a, const FaceTexture& b)
{
    return a.material == b.material && sameVector(a.uAxis, b.uAxis) && sameVector(a.vAxis, b.vAxis) &&
           a.uShift == b.uShift && a.vShift == b.vShift && a.uScale == b.uScale &&
           a.vScale == b.vScale && a.rotation == b.rotation && a.lightmapScale == b.lightmapScale;
}

// CFaceEditMaterialPage::Apply's per-field NOT_INIT test. The rotation is a
// DELTA applied to the texture axes, not a plain assignment.
void applyEdit(FaceTexture& texture, const FaceTextureEdit& edit)
{
    if (edit.shiftX) texture.uShift = *edit.shiftX;
    if (edit.shiftY) texture.vShift = *edit.shiftY;
    if (edit.scaleX) texture.uScale = *edit.scaleX;
    if (edit.scaleY) texture.vScale = *edit.scaleY;
    if (edit.rotation) {
        rotateTextureAxes(texture, *edit.rotation - texture.rotation);
        texture.rotation = *edit.rotation;
    }
    if (edit.lightmapScale) texture.lightmapScale = std::max(1, *edit.lightmapScale);
    if (edit.material) texture.material = *edit.material;
}

double planeDistance(const Vec3& normal, const std::vector<Vec3>& points)
{
    if (points.empty()) return 0.0;
    return normal.x * points.front().x + normal.y * points.front().y + normal.z * points.front().z;
}

} // namespace

std::optional<FaceTexture> EditorModel::faceTexture(const FaceRef& face) const
{
    const Block* side = findSide(document_, face.solidId, face.sideId);
    if (!side) return std::nullopt;
    return readFaceTexture(*side);
}

// Applies the whole batch through one undo entry, the way Apply()'s single
// MarkUndoPosition( NULL, "Apply Face Attributes" ) covers every stored face.
bool EditorModel::applyFaceTextures(const std::vector<FaceRef>& faces, const FaceTextureEdit& edit,
                                    std::string label)
{
    if (faces.empty() || edit.empty()) return false;

    std::vector<std::pair<FaceRef, FaceTexture>> updates;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        if (!side) continue;
        const FaceTexture original = readFaceTexture(*side);
        FaceTexture updated = original;
        applyEdit(updated, edit);
        if (!sameFaceTexture(original, updated)) updates.emplace_back(face, updated);
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, texture] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) writeFaceTexture(*side, texture);
    }
    document_.markDirty();
    return true;
}

std::uint32_t EditorModel::faceSmoothingGroups(const FaceRef& face) const
{
    const Block* side = findSide(document_, face.solidId, face.sideId);
    return side ? readSmoothingGroups(*side) : 0u;
}

// CSmoothingGroupMgr::AddFaceToGroup / RemoveFaceFromGroup applied to the whole
// face list under a single undo entry, the way the material page's Apply covers
// every stored face at once.
bool EditorModel::applySmoothingGroups(const std::vector<FaceRef>& faces, std::uint32_t addGroups,
                                       std::uint32_t removeGroups, std::string label)
{
    if (faces.empty() || (addGroups == 0u && removeGroups == 0u)) return false;

    std::vector<std::pair<FaceRef, std::uint32_t>> updates;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        if (!side) continue;
        const std::uint32_t original = readSmoothingGroups(*side);
        const std::uint32_t updated = (original | addGroups) & ~removeGroups;
        if (updated != original) updates.emplace_back(face, updated);
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, groups] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) {
            writeSmoothingGroups(*side, groups);
        }
    }
    document_.markDirty();
    return true;
}

namespace {

void collectSolidBlocks(const Block& block, std::vector<const Block*>& result)
{
    for (const Entry& entry : block.entries) {
        if (entry.kind != Entry::Kind::ChildBlock || !entry.child) continue;
        if (equalsIgnoreCase(entry.child->name, "solid")) result.push_back(entry.child.get());
        else collectSolidBlocks(*entry.child, result);
    }
}

} // namespace

std::vector<FaceRef> EditorModel::facesInSmoothingGroup(int group) const
{
    std::vector<FaceRef> faces;
    const std::uint32_t bit = smoothingGroupBit(group);
    if (bit == 0u) return faces;

    std::vector<const Block*> solids;
    for (const Block& root : document_.roots()) {
        if (equalsIgnoreCase(root.name, "solid")) solids.push_back(&root);
        else collectSolidBlocks(root, solids);
    }
    for (const Block* solid : solids) {
        const int solidId = parseInt(solid->value("id"));
        for (const Block* side : solid->children("side")) {
            if ((readSmoothingGroups(*side) & bit) != 0u) {
                faces.push_back(FaceRef{solidId, parseInt(side->value("id"))});
            }
        }
    }
    return faces;
}

namespace {

// The four world corners plus the normal of one face, the geometry CMapDisp
// reads off its parent CMapFace (InitDispSurfaceData) and the VMF side chunk
// does not carry. Only quads can be displacements, which is exactly the
// GetPointCount() != 4 test OnButtonCreate makes.
struct DisplacementFaceGeometry
{
    std::array<Vec3, 4> corners{};
    Vec3 normal{};
};

std::optional<DisplacementFaceGeometry> displacementFaceGeometry(const Scene& scene,
                                                                 const FaceRef& face)
{
    for (const BrushGeometry& brush : scene.brushes) {
        if (brush.id != face.solidId) continue;
        for (const FaceGeometry& geometry : brush.faces) {
            if (geometry.sideId != face.sideId) continue;
            if (geometry.vertices.size() != 4) return std::nullopt;
            DisplacementFaceGeometry result;
            for (std::size_t index = 0; index < 4; ++index) {
                const std::size_t vertex = geometry.vertices[index];
                if (vertex >= brush.vertices.size()) return std::nullopt;
                result.corners[index] = brush.vertices[vertex];
            }
            result.normal = geometry.normal;
            return result;
        }
    }
    return std::nullopt;
}

} // namespace

namespace {

// The solids named by a face list, so the geometry helpers below can build
// only those brushes instead of the whole map.
std::unordered_set<int> solidIdsOf(const std::vector<FaceRef>& faces)
{
    std::unordered_set<int> ids;
    for (const FaceRef& face : faces) ids.insert(face.solidId);
    return ids;
}

} // namespace

bool EditorModel::createDisplacements(const std::vector<FaceRef>& faces, int power,
                                      std::string label)
{
    if (faces.empty()) return false;
    power = std::clamp(power, MinDisplacementPower, MaxDisplacementPower);

    const Scene scene = buildSceneForSolids(document_, solidIdsOf(faces));
    std::vector<std::pair<FaceRef, DisplacementInfo>> updates;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        // OnButtonCreate skips faces that are not quads and faces that already
        // have a displacement.
        if (!side || hasDisplacement(*side)) continue;
        const auto geometry = displacementFaceGeometry(scene, face);
        if (!geometry) continue;
        // CMapDisp::InitDispSurfaceData( pFace, true ) generates the start
        // point from the face's first corner.
        updates.emplace_back(face, makeDisplacement(power, geometry->corners[0], geometry->normal));
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, info] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) {
            writeDisplacement(*side, info);
        }
    }
    document_.markDirty();
    return true;
}

bool EditorModel::destroyDisplacements(const std::vector<FaceRef>& faces, std::string label)
{
    if (faces.empty()) return false;

    std::vector<FaceRef> targets;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        if (side && hasDisplacement(*side)) targets.push_back(face);
    }
    if (targets.empty()) return false;

    pushUndo(std::move(label));
    for (const FaceRef& face : targets) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) removeDisplacement(*side);
    }
    document_.markDirty();
    return true;
}

std::optional<DisplacementInfo> EditorModel::faceDisplacement(const FaceRef& face) const
{
    const Block* side = findSide(document_, face.solidId, face.sideId);
    if (!side) return std::nullopt;
    return readDisplacement(*side);
}

// CFaceEditDispPage::OnButtonApply: UpdatePower / UpdateElevation / UpdateScale
// on every displacement in the face list.
bool EditorModel::applyDisplacementAttributes(const std::vector<FaceRef>& faces,
                                              const DisplacementAttributeEdit& edit,
                                              std::string label)
{
    if (faces.empty() || edit.empty()) return false;

    std::vector<std::pair<FaceRef, DisplacementInfo>> updates;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        if (!side) continue;
        auto info = readDisplacement(*side);
        if (!info) continue;
        bool changed = false;
        // The original's order: power, then elevation, then scale.
        if (edit.power) {
            changed |= resampleDisplacement(*info,
                                            std::clamp(*edit.power, MinDisplacementPower,
                                                       MaxDisplacementPower));
        }
        if (edit.elevation) changed |= elevateDisplacement(*info, *edit.elevation);
        if (edit.scale) changed |= scaleDisplacement(*info, edit.previousScale, *edit.scale);
        if (changed) updates.emplace_back(face, std::move(*info));
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, info] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) {
            writeDisplacement(*side, info);
        }
    }
    document_.markDirty();
    return true;
}

bool EditorModel::applyDisplacementNoise(const std::vector<FaceRef>& faces, double minimum,
                                         double maximum, double rockiness, std::string label)
{
    if (faces.empty() || minimum == maximum) return false;

    const Scene scene = buildSceneForSolids(document_, solidIdsOf(faces));
    std::vector<std::pair<FaceRef, DisplacementInfo>> updates;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        if (!side) continue;
        auto info = readDisplacement(*side);
        if (!info) continue;
        const auto geometry = displacementFaceGeometry(scene, face);
        if (!geometry) continue;
        if (hammer::vmf::applyDisplacementNoise(*info, geometry->corners, geometry->normal, minimum,
                                                maximum, rockiness)) {
            updates.emplace_back(face, std::move(*info));
        }
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, info] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) {
            writeDisplacement(*side, info);
        }
    }
    document_.markDirty();
    return true;
}

// CFaceEditDispPage::OnButtonSew -> FaceListSewEdges.
bool EditorModel::sewDisplacementFaces(const std::vector<FaceRef>& faces, std::string label)
{
    if (faces.size() < 2) return false;

    const Scene scene = buildSceneForSolids(document_, solidIdsOf(faces));
    std::vector<FaceRef> refs;
    std::vector<DisplacementInfo> infos;
    std::vector<SewSurface> surfaces;
    refs.reserve(faces.size());
    infos.reserve(faces.size());
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document_, face.solidId, face.sideId);
        if (!side) continue;
        auto info = readDisplacement(*side);
        if (!info) continue;
        const auto geometry = displacementFaceGeometry(scene, face);
        if (!geometry) continue;
        refs.push_back(face);
        infos.push_back(std::move(*info));
        SewSurface surface;
        // The corner / edge point indices assume the start-position ordering.
        surface.corners = orientDisplacementCorners(geometry->corners, infos.back().startPosition);
        surface.material = readFaceTexture(*side).material;
        surfaces.push_back(std::move(surface));
    }
    if (surfaces.size() < 2) return false;
    // infos is stable now, so the surfaces can point into it.
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        surfaces[index].info = &infos[index];
    }

    if (!sewDisplacements(surfaces)) return false;

    pushUndo(std::move(label));
    for (std::size_t index = 0; index < refs.size(); ++index) {
        if (Block* side = findSide(document_, refs[index].solidId, refs[index].sideId)) {
            writeDisplacement(*side, infos[index]);
        }
    }
    document_.markDirty();
    return true;
}

std::vector<Vec3> EditorModel::displacementVertices(const FaceRef& face) const
{
    const auto info = faceDisplacement(face);
    if (!info) return {};
    const Scene scene = buildSceneForSolids(document_, {face.solidId});
    const auto geometry = displacementFaceGeometry(scene, face);
    if (!geometry) return {};
    return displacementVertexPositions(*info, geometry->corners, geometry->normal);
}

namespace {

// The shared body of the two paint entry points: everything except how the
// result is folded into the undo stack.
bool paintDisplacementFaces(Document& document, const std::vector<FaceRef>& faces,
                            SpatialPaintData paint, bool alphaChannel,
                            std::vector<std::pair<FaceRef, DisplacementInfo>>& updates)
{
    const Scene scene = buildSceneForSolids(document, solidIdsOf(faces));

    struct Target
    {
        FaceRef face;
        DisplacementInfo info;
        DisplacementFaceGeometry geometry;
    };
    std::vector<Target> targets;
    for (const FaceRef& face : faces) {
        const Block* side = findSide(document, face.solidId, face.sideId);
        if (!side) continue;
        auto info = readDisplacement(*side);
        if (!info) continue;
        const auto geometry = displacementFaceGeometry(scene, face);
        if (!geometry) continue;
        targets.push_back({face, std::move(*info), *geometry});
    }
    if (targets.empty()) return false;

    // ApplySpatialPaintTool centres the sphere on the displacement vertex the
    // ray hit (GetTexelHitIndex -> GetVert), not on the raw intersection.
    // Distances are measured from the fixed hit point; writing the winner into
    // paint.center inside the loop would move the reference point mid-search
    // and snap to a vertex nowhere near the cursor.
    const Vec3 hitPoint = paint.center;
    double best = std::numeric_limits<double>::infinity();
    for (const Target& target : targets) {
        for (const Vec3& vertex : displacementVertexPositions(target.info, target.geometry.corners,
                                                              target.geometry.normal)) {
            const double dx = vertex.x - hitPoint.x;
            const double dy = vertex.y - hitPoint.y;
            const double dz = vertex.z - hitPoint.z;
            const double distance = dx * dx + dy * dy + dz * dz;
            if (distance < best) {
                best = distance;
                paint.center = vertex;
            }
        }
    }

    for (Target& target : targets) {
        const bool changed =
            alphaChannel ? paintDisplacementAlpha(target.info, target.geometry.corners,
                                                  target.geometry.normal, paint)
                         : paintDisplacement(target.info, target.geometry.corners,
                                             target.geometry.normal, paint);
        if (changed) updates.emplace_back(target.face, std::move(target.info));
    }
    return !updates.empty();
}

} // namespace

bool EditorModel::paintDisplacements(const std::vector<FaceRef>& faces,
                                     const SpatialPaintData& paint, bool alphaChannel,
                                     std::string label)
{
    if (faces.empty()) return false;
    std::vector<std::pair<FaceRef, DisplacementInfo>> updates;
    if (!paintDisplacementFaces(document_, faces, paint, alphaChannel, updates)) return false;

    pushUndo(std::move(label));
    for (const auto& [face, info] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) {
            writeDisplacement(*side, info);
        }
    }
    document_.markDirty();
    return true;
}

bool EditorModel::paintDisplacementsInTransaction(const std::vector<FaceRef>& faces,
                                                  const SpatialPaintData& paint, bool alphaChannel)
{
    if (!transaction_ || faces.empty()) return false;
    std::vector<std::pair<FaceRef, DisplacementInfo>> updates;
    if (!paintDisplacementFaces(document_, faces, paint, alphaChannel, updates)) return false;

    for (const auto& [face, info] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) {
            writeDisplacement(*side, info);
        }
    }
    document_.markDirty();
    return true;
}

bool EditorModel::justifyFaces(const std::vector<FaceEditTarget>& faces,
                               TextureJustification justification, bool treatAsOne, std::string label)
{
    if (faces.empty()) return false;

    // OnJustify only honours "Treat as one" when more than one face is selected.
    const bool merge = treatAsOne && faces.size() > 1;
    FaceExtents merged{};
    if (merge) {
        bool first = true;
        for (const FaceEditTarget& target : faces) {
            FaceExtents extents{};
            if (!faceExtents(target.points, extents)) continue;
            mergeFaceExtents(merged, extents, first);
            first = false;
        }
        if (first) return false;
    }

    std::vector<std::pair<FaceRef, FaceTexture>> updates;
    for (const FaceEditTarget& target : faces) {
        const Block* side = findSide(document_, target.face.solidId, target.face.sideId);
        if (!side) continue;
        FaceExtents extents = merged;
        if (!merge && !faceExtents(target.points, extents)) continue;
        const FaceTexture original = readFaceTexture(*side);
        FaceTexture updated = original;
        justifyTextureUsingExtents(updated, justification, extents,
                                   target.textureWidth, target.textureHeight);
        if (!sameFaceTexture(original, updated)) updates.emplace_back(target.face, updated);
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, texture] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) writeFaceTexture(*side, texture);
    }
    document_.markDirty();
    return true;
}

bool EditorModel::alignFaceTextures(const std::vector<FaceEditTarget>& faces,
                                    TextureAlignment alignment, std::string label)
{
    if (faces.empty()) return false;

    std::vector<std::pair<FaceRef, FaceTexture>> updates;
    for (const FaceEditTarget& target : faces) {
        const Block* side = findSide(document_, target.face.solidId, target.face.sideId);
        if (!side) continue;
        const FaceTexture original = readFaceTexture(*side);
        FaceTexture updated = original;
        initializeTextureAxes(updated, target.normal, alignment);
        if (!sameFaceTexture(original, updated)) updates.emplace_back(target.face, updated);
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, texture] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) writeFaceTexture(*side, texture);
    }
    document_.markDirty();
    return true;
}

bool EditorModel::alignFacesToView(const std::vector<FaceEditTarget>& faces, const Vec3& viewRight,
                                   const Vec3& viewUp, const Vec3& viewPoint, std::string label)
{
    if (faces.empty()) return false;

    std::vector<std::pair<FaceRef, FaceTexture>> updates;
    for (const FaceEditTarget& target : faces) {
        const Block* side = findSide(document_, target.face.solidId, target.face.sideId);
        if (!side) continue;
        const FaceTexture original = readFaceTexture(*side);
        FaceTexture updated = original;
        alignTextureToView(updated, viewRight, viewUp, viewPoint,
                           target.textureWidth, target.textureHeight);
        if (!sameFaceTexture(original, updated)) updates.emplace_back(target.face, updated);
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, texture] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) writeFaceTexture(*side, texture);
    }
    document_.markDirty();
    return true;
}

bool EditorModel::edgeAlignFaces(const std::vector<FaceEditTarget>& faces,
                                 const FaceEditTarget& reference, std::string label)
{
    if (faces.empty()) return false;
    const Block* referenceSide = findSide(document_, reference.face.solidId, reference.face.sideId);
    if (!referenceSide) return false;
    const FaceTexture from = readFaceTexture(*referenceSide);
    const double fromDistance = planeDistance(reference.normal, reference.points);

    std::vector<std::pair<FaceRef, FaceTexture>> updates;
    for (const FaceEditTarget& target : faces) {
        if (target.face == reference.face) continue;
        const Block* side = findSide(document_, target.face.solidId, target.face.sideId);
        if (!side) continue;
        const FaceTexture original = readFaceTexture(*side);
        FaceTexture updated = original;
        copyTextureCoordinateSystem(from, reference.normal, fromDistance, updated, target.normal,
                                    planeDistance(target.normal, target.points),
                                    target.textureWidth, target.textureHeight);
        if (!sameFaceTexture(original, updated)) updates.emplace_back(target.face, updated);
    }
    if (updates.empty()) return false;

    pushUndo(std::move(label));
    for (const auto& [face, texture] : updates) {
        if (Block* side = findSide(document_, face.solidId, face.sideId)) writeFaceTexture(*side, texture);
    }
    document_.markDirty();
    return true;
}

// "Apply current texture" (Shift+T). CMapDoc::OnApplyTexture / CToolMaterial's
// right-click path both end in "Apply texture" on the history stack.
bool EditorModel::applyMaterialToSelection(const std::string& material, std::string label)
{
    if (material.empty()) return false;

    std::vector<int> targets;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            targets.push_back(object.id);
        } else if (const Block* entity = findObject(document_, object)) {
            collectSolidIds(*entity, targets);
        }
    }
    if (targets.empty()) return false;

    // Do not disturb the document unless at least one side actually changes.
    bool changed = false;
    for (const int id : targets) {
        const Block* solid = findObject(document_, {ObjectType::Solid, id});
        if (!solid) continue;
        for (const Block* side : solid->children("side")) {
            const std::string* current = side->value("material");
            if (!current || *current != material) {
                changed = true;
                break;
            }
        }
        if (changed) break;
    }
    if (!changed) return false;

    pushUndo(std::move(label));
    for (const int id : targets) {
        Block* solid = nullptr;
        for (Block& root : document_.roots()) {
            if (equalsIgnoreCase(root.name, "solid") && parseInt(root.value("id")) == id) {
                solid = &root;
                break;
            }
            if ((solid = findSolid(root, id)) != nullptr) break;
        }
        if (!solid) continue;
        for (Block* side : solid->children("side")) side->setValue("material", material);
    }
    document_.markDirty();
    return true;
}

namespace {

char toLowerAscii(char c)
{
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

std::string toLowerAscii(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](char c) { return toLowerAscii(c); });
    return result;
}

bool textureMatches(std::string_view material, std::string_view find, TextureMatchMode mode)
{
    if (find.empty()) return false;
    if (mode == TextureMatchMode::Exact) return equalsIgnoreCase(material, find);
    return toLowerAscii(material).find(toLowerAscii(find)) != std::string::npos;
}

// SubstitutePartial keeps the rest of the material name and only substitutes
// the matched substring; Exact/Partial replace the whole name, matching what
// the dialog's "Partial match" and "Exact match" radio buttons read as.
std::string replacementMaterial(const std::string& material, const std::string& find,
                                const std::string& replace, TextureMatchMode mode)
{
    if (mode != TextureMatchMode::SubstitutePartial) return replace;
    const std::string hayLower = toLowerAscii(material);
    const std::string needleLower = toLowerAscii(find);
    const std::size_t pos = hayLower.find(needleLower);
    if (pos == std::string::npos) return material;
    return material.substr(0, pos) + replace + material.substr(pos + find.size());
}

} // namespace

ReplaceTexturesResult EditorModel::replaceTextures(
    const ReplaceTexturesRequest& request,
    const std::function<bool(const std::string&, int&, int&)>& materialSize,
    std::string label)
{
    ReplaceTexturesResult result;
    if (request.find.empty()) return result;

    std::vector<const Block*> solids;
    if (request.selectionOnly) {
        solids = selectedSolidBlocks();
    } else {
        for (const Block& root : document_.roots()) {
            if (equalsIgnoreCase(root.name, "solid")) solids.push_back(&root);
            else collectSolidBlocks(root, solids);
        }
    }

    struct Match { int solidId; int sideId; std::string newMaterial; };
    std::vector<Match> matches;
    std::vector<int> matchedSolidIds;

    for (const Block* solid : solids) {
        const int solidId = parseInt(solid->value("id"));
        // IDC_HIDDEN: a hidden solid is skipped unless the box is ticked, so a
        // replace does not silently change geometry the user cannot see.
        if (!request.includeHidden && request.hiddenSolidIds.contains(solidId)) continue;
        bool solidMatched = false;
        for (const Block* side : solid->children("side")) {
            const std::string* material = side->value("material");
            if (!material || !textureMatches(*material, request.find, request.mode)) continue;
            ++result.facesMatched;
            solidMatched = true;
            const std::string updated =
                replacementMaterial(*material, request.find, request.replace, request.mode);
            if (updated != *material) {
                matches.push_back({solidId, parseInt(side->value("id")), updated});
            }
        }
        if (solidMatched) matchedSolidIds.push_back(solidId);
    }

    if (request.markOnly) {
        std::vector<ObjectRef> newSelection;
        newSelection.reserve(matchedSolidIds.size());
        for (const int id : matchedSolidIds) newSelection.push_back(ObjectRef{ObjectType::Solid, id});
        selection_ = std::move(newSelection);
        validateSelection();
        return result;
    }

    if (matches.empty()) return result;

    pushUndo(std::move(label));
    for (const Match& match : matches) {
        Block* side = findSide(document_, match.solidId, match.sideId);
        if (!side) continue;
        FaceTexture texture = readFaceTexture(*side);
        int oldWidth = 0, oldHeight = 0, newWidth = 0, newHeight = 0;
        const bool haveOld = materialSize && materialSize(texture.material, oldWidth, oldHeight);
        texture.material = match.newMaterial;
        if (request.rescaleTextureCoordinates && materialSize) {
            const bool haveNew = materialSize(match.newMaterial, newWidth, newHeight);
            if (haveOld && haveNew && oldWidth > 0 && oldHeight > 0 && newWidth > 0 && newHeight > 0) {
                // Keep the texture's apparent world-space size: scale is
                // world-units per texel, so a wider replacement texture needs
                // a proportionally smaller scale to cover the same area.
                texture.uScale *= static_cast<double>(oldWidth) / static_cast<double>(newWidth);
                texture.vScale *= static_cast<double>(oldHeight) / static_cast<double>(newHeight);
            }
        }
        writeFaceTexture(*side, texture);
        ++result.facesChanged;
    }
    document_.markDirty();
    return result;
}

bool EditorModel::duplicateSelection(const Vec3& offset, std::string label)
{
    return paste(copySelection(), offset, std::move(label));
}

std::optional<ObjectRef> EditorModel::createBlock(const Vec3& first, const Vec3& second,
                                                   std::string material, std::string label)
{
    Vec3 minimum{std::min(first.x, second.x), std::min(first.y, second.y), std::min(first.z, second.z)};
    Vec3 maximum{std::max(first.x, second.x), std::max(first.y, second.y), std::max(first.z, second.z)};
    if (maximum.x - minimum.x < 1.0) maximum.x = minimum.x + 16.0;
    if (maximum.y - minimum.y < 1.0) maximum.y = minimum.y + 16.0;
    if (maximum.z - minimum.z < 1.0) maximum.z = minimum.z + 16.0;

    Block* world = document_.firstRoot("world");
    if (!world) return std::nullopt;
    pushUndo(std::move(label));

    Block solid("solid");
    const int solidId = allocateId();
    solid.setValue("id", std::to_string(solidId));
    const Vec3 a{maximum.x, maximum.y, maximum.z};
    const Vec3 b{minimum.x, maximum.y, maximum.z};
    const Vec3 c{minimum.x, minimum.y, maximum.z};
    const Vec3 d{minimum.x, minimum.y, minimum.z};
    const Vec3 e{minimum.x, maximum.y, minimum.z};
    const Vec3 f{maximum.x, maximum.y, minimum.z};
    const Vec3 g{maximum.x, minimum.y, maximum.z};
    const Vec3 h{maximum.x, minimum.y, minimum.z};
    // Each triple is wound so that CMapFace::CalcPlane's
    // GetNormalFromPoints( p0, p1, p2 ) = (p0 - p1) x (p2 - p1) yields the
    // OUTWARD normal, which is the ordering Hammer itself writes. Reversing a
    // triple negates that normal, so brushes saved with the opposite winding
    // load inside-out in Hammer and VBSP.
    const Vec3 planes[6][3] = {{c,b,a}, {f,e,d}, {h,g,a}, {e,b,c}, {f,a,b}, {d,c,g}};
    for (const auto& points : planes) {
        Block& side = solid.appendChild("side");
        side.setValue("id", std::to_string(allocateId()));
        double values[9] = {points[0].x, points[0].y, points[0].z,
                            points[1].x, points[1].y, points[1].z,
                            points[2].x, points[2].y, points[2].z};
        side.setValue("plane", formatPlane(values));
        applyWorldAlignedTexture(side, planeNormal(points[0], points[1], points[2]), material);
        side.setValue("smoothing_groups", "0");
    }
    setEditorMetadata(solid, "0 255 255");
    world->entries.emplace_back(std::move(solid));
    selection_ = {{ObjectType::Solid, solidId}};
    document_.markDirty();
    return selection_.front();
}

std::optional<ObjectRef> EditorModel::createPrimitive(PrimitiveKind kind, const Vec3& first,
                                                       const Vec3& second, int extrusionAxis,
                                                       int faces, std::string material,
                                                       std::string label)
{
    if (kind == PrimitiveKind::Block)
        return createBlock(first, second, std::move(material), std::move(label));
    extrusionAxis = std::clamp(extrusionAxis, 0, 2);
    faces = std::clamp(faces, 3, 64);

    Vec3 minimum{std::min(first.x, second.x), std::min(first.y, second.y), std::min(first.z, second.z)};
    Vec3 maximum{std::max(first.x, second.x), std::max(first.y, second.y), std::max(first.z, second.z)};
    if (maximum.x - minimum.x < 1.0) maximum.x = minimum.x + 16.0;
    if (maximum.y - minimum.y < 1.0) maximum.y = minimum.y + 16.0;
    if (maximum.z - minimum.z < 1.0) maximum.z = minimum.z + 16.0;

    // The shape lives in the (u, v) plane; w is the extrusion axis.
    const int axisU = extrusionAxis == 0 ? 1 : 0;
    const int axisV = extrusionAxis == 2 ? 1 : 2;
    const int axisW = extrusionAxis;
    const auto component = [](const Vec3& value, int axis) {
        return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
    };
    const auto makePoint = [&](double u, double v, double w) {
        Vec3 result{};
        double* fields[3] = {&result.x, &result.y, &result.z};
        *fields[axisU] = std::round(u);
        *fields[axisV] = std::round(v);
        *fields[axisW] = std::round(w);
        return result;
    };
    const double uMin = component(minimum, axisU);
    const double uMax = component(maximum, axisU);
    const double vMin = component(minimum, axisV);
    const double vMax = component(maximum, axisV);
    const double wMin = component(minimum, axisW);
    const double wMax = component(maximum, axisW);
    const double uCenter = (uMin + uMax) * 0.5;
    const double vCenter = (vMin + vMax) * 0.5;

    // The base outline in the (u, v) plane, counter-clockwise.
    std::vector<std::pair<double, double>> outline;
    if (kind == PrimitiveKind::Wedge) {
        outline = {{uMin, vMin}, {uMax, vMin}, {uMin, vMax}};
    } else {
        const double radiusU = (uMax - uMin) * 0.5;
        const double radiusV = (vMax - vMin) * 0.5;
        for (int i = 0; i < faces; ++i) {
            const double angle = 2.0 * 3.14159265358979323846 * i / faces;
            outline.emplace_back(uCenter + radiusU * std::cos(angle),
                                 vCenter + radiusV * std::sin(angle));
        }
    }

    // Rounding to integers can merge neighbouring outline points on a small
    // box; drop the duplicates and fall back to a plain block if the outline
    // degenerates.
    std::vector<std::pair<double, double>> deduped;
    for (const auto& point : outline) {
        const std::pair<double, double> rounded{std::round(point.first), std::round(point.second)};
        if (deduped.empty() || deduped.back() != rounded) deduped.push_back(rounded);
    }
    if (deduped.size() > 1 && deduped.front() == deduped.back()) deduped.pop_back();
    outline = std::move(deduped);
    if (outline.size() < 3)
        return createBlock(first, second, std::move(material), std::move(label));

    // Interior reference point for outward plane orientation. The outline's
    // average, not the box center: the box center of a wedge lies exactly on
    // its hypotenuse, which degenerates the orientation test.
    double uSum = 0.0;
    double vSum = 0.0;
    for (const auto& point : outline) {
        uSum += point.first;
        vSum += point.second;
    }
    Vec3 centroid{};
    {
        double* fields[3] = {&centroid.x, &centroid.y, &centroid.z};
        *fields[axisU] = uSum / static_cast<double>(outline.size());
        *fields[axisV] = vSum / static_cast<double>(outline.size());
        *fields[axisW] = (wMin + wMax) * 0.5;
    }

    Block* world = document_.firstRoot("world");
    if (!world) return std::nullopt;
    pushUndo(std::move(label));

    Block solid("solid");
    const int solidId = allocateId();
    solid.setValue("id", std::to_string(solidId));

    const auto appendSide = [&](Vec3 a, Vec3 b, Vec3 c) {
        // GetNormalFromPoints(p0, p1, p2) = (p0 - p1) x (p2 - p1) must point
        // away from the interior; swap the winding when it does not.
        const Vec3 edge1{a.x - b.x, a.y - b.y, a.z - b.z};
        const Vec3 edge2{c.x - b.x, c.y - b.y, c.z - b.z};
        const Vec3 normal{edge1.y * edge2.z - edge1.z * edge2.y,
                          edge1.z * edge2.x - edge1.x * edge2.z,
                          edge1.x * edge2.y - edge1.y * edge2.x};
        const Vec3 toPlane{b.x - centroid.x, b.y - centroid.y, b.z - centroid.z};
        if (normal.x * toPlane.x + normal.y * toPlane.y + normal.z * toPlane.z < 0.0)
            std::swap(a, c);
        Block& side = solid.appendChild("side");
        side.setValue("id", std::to_string(allocateId()));
        double values[9] = {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z};
        side.setValue("plane", formatPlane(values));
        // Recomputed from the final winding: the swap above flips the normal,
        // and the alignment has to follow the face the brush actually gets.
        applyWorldAlignedTexture(side, planeNormal(a, b, c), material);
        side.setValue("smoothing_groups", "0");
    };

    const std::size_t count = outline.size();
    if (kind == PrimitiveKind::Spike) {
        const Vec3 apex = makePoint(uCenter, vCenter, wMax);
        // Base cap plus one plane through the apex per base edge.
        appendSide(makePoint(outline[0].first, outline[0].second, wMin),
                   makePoint(outline[1].first, outline[1].second, wMin),
                   makePoint(outline[2].first, outline[2].second, wMin));
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = outline[i];
            const auto& b = outline[(i + 1) % count];
            appendSide(makePoint(a.first, a.second, wMin),
                       makePoint(b.first, b.second, wMin), apex);
        }
    } else {
        // Wedge and cylinder: a prism over the outline.
        appendSide(makePoint(outline[0].first, outline[0].second, wMin),
                   makePoint(outline[1].first, outline[1].second, wMin),
                   makePoint(outline[2].first, outline[2].second, wMin));
        appendSide(makePoint(outline[0].first, outline[0].second, wMax),
                   makePoint(outline[1].first, outline[1].second, wMax),
                   makePoint(outline[2].first, outline[2].second, wMax));
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = outline[i];
            const auto& b = outline[(i + 1) % count];
            appendSide(makePoint(a.first, a.second, wMin),
                       makePoint(b.first, b.second, wMin),
                       makePoint(b.first, b.second, wMax));
        }
    }

    setEditorMetadata(solid, "0 255 255");
    world->entries.emplace_back(std::move(solid));
    selection_ = {{ObjectType::Solid, solidId}};
    document_.markDirty();
    return selection_.front();
}

std::optional<ObjectRef> EditorModel::createPointEntity(std::string classname, const Vec3& origin,
                                                         const std::vector<Property>& defaults,
                                                         std::string label)
{
    if (classname.empty()) return std::nullopt;
    pushUndo(std::move(label));
    Block entity("entity");
    const int entityId = allocateId();
    entity.setValue("id", std::to_string(entityId));
    entity.setValue("classname", std::move(classname));
    entity.setValue("origin", formatVec3(origin));
    for (const Property& property : defaults) {
        if (property.key.empty() || equalsIgnoreCase(property.key, "id") || equalsIgnoreCase(property.key, "classname") ||
            equalsIgnoreCase(property.key, "origin")) continue;
        entity.setValue(property.key, property.value);
    }
    setEditorMetadata(entity, "220 30 220");
    document_.roots().push_back(std::move(entity));
    selection_ = {{ObjectType::Entity, entityId}};
    document_.markDirty();
    return selection_.front();
}

std::size_t EditorModel::createPointEntities(const std::vector<PointEntitySpec>& entities,
                                            std::string label)
{
    if (entities.empty()) return 0;
    pushUndo(std::move(label));
    std::vector<ObjectRef> created;
    for (const PointEntitySpec& spec : entities) {
        if (spec.classname.empty()) continue;
        Block entity("entity");
        const int entityId = allocateId();
        entity.setValue("id", std::to_string(entityId));
        entity.setValue("classname", spec.classname);
        entity.setValue("origin", formatVec3(spec.origin));
        for (const Property& property : spec.defaults) {
            if (property.key.empty() || equalsIgnoreCase(property.key, "id") ||
                equalsIgnoreCase(property.key, "classname") ||
                equalsIgnoreCase(property.key, "origin")) continue;
            entity.setValue(property.key, property.value);
        }
        setEditorMetadata(entity, "220 30 220");
        document_.roots().push_back(std::move(entity));
        created.push_back({ObjectType::Entity, entityId});
    }
    if (created.empty()) {
        document_ = std::move(undo_.back().document);
        selection_ = std::move(undo_.back().selection);
        undo_.pop_back();
        return 0;
    }
    const std::size_t count = created.size();
    selection_ = std::move(created);
    document_.markDirty();
    return count;
}

bool EditorModel::beginTransaction(std::string label, bool requireSelection)
{
    if (transaction_ || (requireSelection && selection_.empty())) return false;
    transaction_ = Snapshot{document_, selection_, std::move(label)};
    return true;
}

bool EditorModel::translateSelectionInTransaction(const Vec3& delta)
{
    if (!transaction_ || (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0)) return false;
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = translateObject(document_, object, delta) || changed;
    }
    if (changed) document_.markDirty();
    return changed;
}

bool EditorModel::scaleSelectionInTransaction(const Vec3& factors, const Vec3& pivot)
{
    if (!transaction_ || factors.x <= 0.0001 || factors.y <= 0.0001 || factors.z <= 0.0001 ||
        !std::isfinite(factors.x) || !std::isfinite(factors.y) || !std::isfinite(factors.z)) return false;
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = scaleObject(document_, object, factors, pivot) || changed;
    }
    if (changed) document_.markDirty();
    return changed;
}

bool EditorModel::rotateSelectionInTransaction(double radians, RotationAxis axis, const Vec3& pivot)
{
    if (!transaction_ || !std::isfinite(radians) || std::abs(radians) < 1e-12) return false;
    bool changed = false;
    for (const ObjectRef& object : selection_) {
        if (object.type == ObjectType::Solid) {
            const int owner = solidOwnerEntityId(document_, object.id);
            if (owner >= 0 && isSelected({ObjectType::Entity, owner})) continue;
        }
        changed = rotateObject(document_, object, radians, axis, pivot) || changed;
    }
    if (changed) document_.markDirty();
    return changed;
}

bool EditorModel::commitTransaction()
{
    if (!transaction_) return false;
    if (document_.serialize(false) == transaction_->document.serialize(false)) {
        transaction_.reset();
        return false;
    }
    undo_.push_back(std::move(*transaction_));
    transaction_.reset();
    redo_.clear();
    validateSelection();
    resetNextId();
    return true;
}

void EditorModel::cancelTransaction()
{
    if (!transaction_) return;
    document_ = std::move(transaction_->document);
    selection_ = std::move(transaction_->selection);
    transaction_.reset();
    resetNextId();
}

std::string EditorModel::undoLabel() const { return canUndo() ? undo_.back().label : std::string{}; }
std::string EditorModel::redoLabel() const { return canRedo() ? redo_.back().label : std::string{}; }

bool EditorModel::undo()
{
    if (!canUndo() || transaction_) return false;
    Snapshot previous = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({document_, selection_, previous.label});
    document_ = std::move(previous.document);
    selection_ = std::move(previous.selection);
    validateSelection();
    resetNextId();
    return true;
}

bool EditorModel::redo()
{
    if (!canRedo() || transaction_) return false;
    Snapshot next = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({document_, selection_, next.label});
    document_ = std::move(next.document);
    selection_ = std::move(next.selection);
    validateSelection();
    resetNextId();
    return true;
}

bool EditorModel::translateObject(Document& document, const ObjectRef& object, const Vec3& delta)
{
    Block* block = findObject(document, object);
    if (!block) return false;
    return object.type == ObjectType::Solid ? translateSolid(*block, delta) : translateEntity(*block, delta);
}

bool EditorModel::scaleObject(Document& document, const ObjectRef& object, const Vec3& factors, const Vec3& pivot)
{
    Block* block = findObject(document, object);
    if (!block) return false;
    const auto transform = [&](const Vec3& point) { return scalePoint(point, factors, pivot); };
    return object.type == ObjectType::Solid ? transformSolid(*block, transform) : transformEntity(*block, transform);
}

bool EditorModel::rotateObject(Document& document, const ObjectRef& object, double radians,
                               RotationAxis axis, const Vec3& pivot)
{
    Block* block = findObject(document, object);
    if (!block) return false;
    const auto transform = [&](const Vec3& point) { return rotatePoint(point, radians, axis, pivot); };
    if (object.type == ObjectType::Solid) return transformSolid(*block, transform);
    bool changed = transformEntity(*block, transform);
    changed = rotateEntityAngles(*block, radians, axis) || changed;
    return changed;
}

bool EditorModel::eraseObject(Document& document, const ObjectRef& object)
{
    if (object.type == ObjectType::Entity) {
        auto& roots = document.roots();
        const auto it = std::find_if(roots.begin(), roots.end(), [&](const Block& root) {
            return equalsIgnoreCase(root.name, "entity") && parseInt(root.value("id")) == object.id;
        });
        if (it == roots.end()) return false;
        roots.erase(it);
        return true;
    }
    for (Block& root : document.roots()) if (eraseSolid(root, object.id)) return true;
    return false;
}

Block* EditorModel::findObject(Document& document, const ObjectRef& object)
{
    if (object.type == ObjectType::Entity) {
        for (Block& root : document.roots()) {
            if (equalsIgnoreCase(root.name, "entity") && parseInt(root.value("id")) == object.id) return &root;
        }
        return nullptr;
    }
    for (Block& root : document.roots()) if (Block* solid = findSolid(root, object.id)) return solid;
    return nullptr;
}

const Block* EditorModel::findObject(const Document& document, const ObjectRef& object)
{
    if (object.type == ObjectType::Entity) {
        for (const Block& root : document.roots()) {
            if (equalsIgnoreCase(root.name, "entity") && parseInt(root.value("id")) == object.id) return &root;
        }
        return nullptr;
    }
    for (const Block& root : document.roots()) if (const Block* solid = findSolid(root, object.id)) return solid;
    return nullptr;
}

int EditorModel::allocateId() { return nextId_++; }

void EditorModel::resetNextId()
{
    if (idRangeSpan_ > 0) {
        // Only this peer's window counts: another collaborator's ids sit in a
        // different window and must not drag this counter into it.
        const std::function<int(const Block&)> maximumInRange = [&](const Block& block) {
            const int id = parseInt(block.value("id"));
            int result = id >= idRangeBase_ && id < idRangeBase_ + idRangeSpan_ ? id : 0;
            for (const Entry& entry : block.entries) {
                if (entry.kind == Entry::Kind::ChildBlock && entry.child)
                    result = std::max(result, maximumInRange(*entry.child));
            }
            return result;
        };
        int maximum = 0;
        for (const Block& root : document_.roots())
            maximum = std::max(maximum, maximumInRange(root));
        nextId_ = std::max(idRangeBase_ + 1, maximum + 1);
        return;
    }
    int maximum = 0;
    for (const Block& root : document_.roots()) maximum = std::max(maximum, maximumId(root));
    nextId_ = maximum + 1;
}

void EditorModel::setIdRange(int base, int span)
{
    idRangeBase_ = base;
    idRangeSpan_ = span;
    resetNextId();
}

void EditorModel::applyExternalEdit(const std::function<void(Document&)>& edit)
{
    edit(document_);
    for (Snapshot& snapshot : undo_) edit(snapshot.document);
    for (Snapshot& snapshot : redo_) edit(snapshot.document);
    if (transaction_) edit(transaction_->document);
    document_.markDirty();
    validateSelection();
    resetNextId();
}

void EditorModel::pushUndo(std::string label)
{
    // With undo off the entry is still pushed — a command that bails out
    // restores itself from undo_.back() — but nothing older is kept, so at
    // most one snapshot is in memory while the command runs.
    if (!undoEnabled_) undo_.clear();
    undo_.push_back({document_, selection_, std::move(label)});
    redo_.clear();
}

void EditorModel::setUndoEnabled(bool enabled)
{
    undoEnabled_ = enabled;
    // Cleared on both transitions: switching back on must not resurrect a
    // rollback snapshot left over from an edit made while undo was off.
    clearHistory();
}

void EditorModel::clearHistory()
{
    undo_.clear();
    redo_.clear();
}

void EditorModel::validateSelection()
{
    std::erase_if(selection_, [&](const ObjectRef& object) { return !findObject(document_, object); });
}

} // namespace hammer::vmf
