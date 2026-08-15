#include "DetailObjects.hpp"

#include "VmfDocument.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

namespace hammer::assets {

namespace {

using hammer::vmf::Vec3;

bool equalsCi(std::string_view left, std::string_view right)
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

const std::string* valueCi(const hammer::vmf::Block& block, std::string_view key)
{
    for (const hammer::vmf::Entry& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::KeyValue && equalsCi(entry.key, key))
            return &entry.value;
    }
    return nullptr;
}

float floatValue(const hammer::vmf::Block& block, std::string_view key, float fallback)
{
    const std::string* value = valueCi(block, key);
    // KeyValues::GetFloat is atof, which never throws and stops at the first
    // character it cannot use.
    return value ? static_cast<float>(std::atof(value->c_str())) : fallback;
}

int intValue(const hammer::vmf::Block& block, std::string_view key, int fallback)
{
    const std::string* value = valueCi(block, key);
    return value ? std::atoi(value->c_str()) : fallback;
}

// Whether a child block would answer KeyValues::GetFirstSubKey() - that is the
// test ParseDetailObjectFile and ParseDetailGroup use to tell a nested block
// from a plain key. A KeyValues leaf key has no subkeys; a block does.
bool hasSubKeys(const hammer::vmf::Block& block)
{
    return !block.entries.empty();
}

// --- The RNG ---------------------------------------------------------------
//
// VBSP seeds the C runtime with the Hammer face id and takes every placement
// decision from rand(). Reimplementing Microsoft's rand() rather than calling
// the local one keeps placement identical across platforms and identical to
// what a Windows VBSP would decide for the same face - which is the whole
// point of the per-face seeding.
class SourceRandom
{
public:
    explicit SourceRandom(int seed) : state_(static_cast<std::uint32_t>(seed)) {}

    // MSVC rand(): a 32-bit LCG whose top bits are returned. VALVE_RAND_MAX is
    // 0x7fff (tier0/platform.h), which is exactly this generator's range.
    int next()
    {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7fffu);
    }

    // The "rand() / (float)VALVE_RAND_MAX" idiom used throughout the emitter.
    float unit() { return static_cast<float>(next()) / 32767.0f; }

    // Stands in for vstdlib's RandomGaussianFloat, which draws from a stream
    // this tree does not carry. Box-Muller on the same LCG gives the authored
    // mean and standard deviation, just not VBSP's exact sequence.
    float gaussian(float mean, float stdDev)
    {
        const float u1 = std::max(unit(), 1e-7f);
        const float u2 = unit();
        const float magnitude = std::sqrt(-2.0f * std::log(u1));
        return mean + stdDev * magnitude *
                          std::cos(2.0f * static_cast<float>(std::numbers::pi) * u2);
    }

private:
    std::uint32_t state_{0};
};

Vec3 add(const Vec3& left, const Vec3& right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 subtract(const Vec3& left, const Vec3& right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 scale(const Vec3& value, double factor)
{
    return {value.x * factor, value.y * factor, value.z * factor};
}

Vec3 cross(const Vec3& left, const Vec3& right)
{
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

double dot(const Vec3& left, const Vec3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

double length(const Vec3& value)
{
    return std::sqrt(dot(value, value));
}

Vec3 normalized(const Vec3& value, const Vec3& fallback = {0.0, 0.0, 1.0})
{
    const double magnitude = length(value);
    if (!std::isfinite(magnitude) || magnitude < 1e-12) return fallback;
    return scale(value, 1.0 / magnitude);
}

constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

// Source's MatrixAngles for a basis given as three column vectors.
Vec3 matrixAngles(const Vec3& forward, const Vec3& left, const Vec3& up)
{
    Vec3 angles{};
    const double xyDistance = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    if (xyDistance > 0.001) {
        angles.y = std::atan2(forward.y, forward.x) * kDegreesPerRadian;
        angles.x = std::atan2(-forward.z, xyDistance) * kDegreesPerRadian;
        angles.z = std::atan2(left.z, up.z) * kDegreesPerRadian;
    } else {
        angles.y = std::atan2(-left.x, left.y) * kDegreesPerRadian;
        angles.x = std::atan2(-forward.z, xyDistance) * kDegreesPerRadian;
        angles.z = 0.0;
    }
    return angles;
}

// Source's VectorAngles.
Vec3 vectorAngles(const Vec3& forward)
{
    Vec3 angles{};
    if (std::abs(forward.x) < 1e-9 && std::abs(forward.y) < 1e-9) {
        angles.y = 0.0;
        angles.x = forward.z > 0.0 ? -90.0 : 90.0;
    } else {
        angles.y = std::atan2(forward.y, forward.x) * kDegreesPerRadian;
        if (angles.y < 0.0) angles.y += 360.0;
        const double xyDistance = std::sqrt(forward.x * forward.x + forward.y * forward.y);
        angles.x = std::atan2(-forward.z, xyDistance) * kDegreesPerRadian;
        if (angles.x < 0.0) angles.x += 360.0;
    }
    angles.z = 0.0;
    return angles;
}

// Source's AngleVectors, returning only the two axes the sprite quad needs.
void angleVectors(const Vec3& angles, Vec3& right, Vec3& up)
{
    const double pitch = angles.x / kDegreesPerRadian;
    const double yaw = angles.y / kDegreesPerRadian;
    const double roll = angles.z / kDegreesPerRadian;
    const double sp = std::sin(pitch), cp = std::cos(pitch);
    const double sy = std::sin(yaw), cy = std::cos(yaw);
    const double sr = std::sin(roll), cr = std::cos(roll);
    right = {-1.0 * sr * sp * cy + -1.0 * cr * -sy,
             -1.0 * sr * sp * sy + -1.0 * cr * cy,
             -1.0 * sr * cp};
    up = {cr * sp * cy + -sr * -sy,
          cr * sp * sy + -sr * cy,
          cr * cp};
}

// ParseDetailGroup.
void parseDetailGroup(DetailObjectType& type, const hammer::vmf::Block& groupBlock)
{
    const float alpha = floatValue(groupBlock, "alpha", 1.0f);

    // Insert after the last group that is more transparent than this one, so
    // groups stay sorted by alpha.
    std::size_t insertAt = type.groups.size();
    while (insertAt > 0 && !(alpha > type.groups[insertAt - 1].alpha)) --insertAt;

    DetailObjectGroup group;
    group.alpha = alpha;

    float totalAmount = 0.0f;
    for (const hammer::vmf::Entry& entry : groupBlock.entries) {
        if (entry.kind != hammer::vmf::Entry::Kind::ChildBlock || !entry.child) continue;
        const hammer::vmf::Block& modelBlock = *entry.child;
        if (!hasSubKeys(modelBlock)) continue;

        DetailModelDefinition model;
        if (const std::string* modelName = valueCi(modelBlock, "model")) {
            model.modelName = *modelName;
            model.type = DetailPropType::Model;
        } else if (const std::string* spriteData = valueCi(modelBlock, "sprite")) {
            model.type = DetailPropType::Sprite;
            if (const std::string* shape = valueCi(modelBlock, "sprite_shape")) {
                if (equalsCi(*shape, "cross")) model.type = DetailPropType::ShapeCross;
                else if (equalsCi(*shape, "tri")) model.type = DetailPropType::ShapeTri;
            }

            float x = 0.0f, y = 0.0f, width = 64.0f, height = 64.0f, textureSize = 512.0f;
            const int parsed = std::sscanf(spriteData->c_str(), "%f %f %f %f %f",
                                           &x, &y, &width, &height, &textureSize);
            // VBSP hard errors on a malformed "sprite"; an editor cannot, so
            // the entry is dropped instead.
            if (parsed != 5 || textureSize == 0.0f) continue;

            model.texUpperLeft = {(x + 0.5f) / textureSize, (y + 0.5f) / textureSize};
            model.texLowerRight = {(x + width - 0.5f) / textureSize,
                                   (y + height - 0.5f) / textureSize};

            model.positionUpperLeft = {-10.0f, 20.0f};
            model.positionLowerRight = {10.0f, 0.0f};
            if (const std::string* spriteSize = valueCi(modelBlock, "spritesize")) {
                if (std::sscanf(spriteSize->c_str(), "%f %f %f %f", &x, &y, &width, &height) == 4) {
                    const float originX = width * x;
                    const float originY = height * y;
                    model.positionUpperLeft = {-originX, height - originY};
                    model.positionLowerRight = {width - originX, -originY};
                }
            }

            model.randomScaleStdDev = floatValue(modelBlock, "spriterandomscale", 0.0f);
            const float sway = std::clamp(floatValue(modelBlock, "sway", 0.0f), 0.0f, 1.0f);
            model.swayAmount = static_cast<std::uint8_t>(255.0f * sway);
            model.shapeAngle = static_cast<std::uint8_t>(intValue(modelBlock, "shape_angle", 0));
            const float shapeSize = std::clamp(floatValue(modelBlock, "shape_size", 0.0f), 0.0f, 1.0f);
            model.shapeSize = static_cast<std::uint8_t>(255.0f * shapeSize);
        } else {
            // Neither a model nor a sprite: VBSP leaves such an entry with an
            // invalid model name, which AddDetailToLump then rejects.
            continue;
        }

        model.amount = floatValue(modelBlock, "amount", 1.0f) + totalAmount;
        totalAmount = model.amount;
        model.upright = intValue(modelBlock, "upright", 0) != 0;

        // These prevent emission on steep surfaces. The FGD-less default of
        // 180 degrees means "any surface".
        const float minAngle = floatValue(modelBlock, "minAngle", 180.0f);
        const float maxAngle = floatValue(modelBlock, "maxAngle", 180.0f);
        model.minCosAngle = std::cos(minAngle * std::numbers::pi_v<float> / 180.0f);
        model.maxCosAngle = std::cos(maxAngle * std::numbers::pi_v<float> / 180.0f);
        model.orientation = intValue(modelBlock, "detailOrientation", 0);
        if (model.minCosAngle < model.maxCosAngle) model.minCosAngle = model.maxCosAngle;

        group.models.push_back(std::move(model));
    }

    // Renormalize the cumulative amounts if the group's total exceeds one.
    if (totalAmount > 1.0f) {
        for (DetailModelDefinition& model : group.models) model.amount /= totalAmount;
    }

    type.groups.insert(type.groups.begin() + static_cast<std::ptrdiff_t>(insertAt),
                       std::move(group));
}

// SelectGroup: interpolates between the two groups the alpha falls between.
int selectGroup(const DetailObjectType& type, float alpha, SourceRandom& random)
{
    const int count = static_cast<int>(type.groups.size());
    int start = 0;
    for (; start < count - 1; ++start) {
        if (alpha < type.groups[static_cast<std::size_t>(start) + 1].alpha) break;
    }

    int end = start + 1;
    if (end >= count) --end;
    if (start == end) return start;

    float distance = 0.0f;
    const float alphaSpan = type.groups[static_cast<std::size_t>(end)].alpha -
                            type.groups[static_cast<std::size_t>(start)].alpha;
    if (alphaSpan != 0.0f) {
        distance = (alpha - type.groups[static_cast<std::size_t>(start)].alpha) / alphaSpan;
    }

    // At distance 0 always start, at distance 1 always end - which is why the
    // comparison looks inverted.
    return (random.unit() > distance) ? start : end;
}

// SelectDetail.
int selectDetail(const DetailObjectGroup& group, SourceRandom& random)
{
    const float value = random.unit();
    for (std::size_t index = 0; index < group.models.size(); ++index) {
        if (value <= group.models[index].amount) return static_cast<int>(index);
    }
    return -1;
}

// PlaceDetail.
void placeDetail(const DetailModelDefinition& model, const Vec3& point, const Vec3& normal,
                 SourceRandom& random, DetailPropEmission& emission)
{
    // Never emit if the surface is too steep, and fade out probabilistically
    // between the min and max angles.
    const double cosAngle = normal.z;
    if (cosAngle < model.maxCosAngle) return;
    if (cosAngle < model.minCosAngle) {
        const double probability = (cosAngle - model.maxCosAngle) /
                                   (model.minCosAngle - model.maxCosAngle);
        if (random.unit() > probability) return;
    }

    Vec3 angles{};
    if (model.upright) {
        // Upright objects only get a random yaw.
        angles = {0.0, 360.0 * random.unit(), 0.0};
    } else {
        // Otherwise conform to the ground, with a random rotation about the
        // surface normal.
        const Vec3 zAxis = normalized(normal);
        Vec3 xAxis{1.0, 0.0, 0.0};
        if (std::abs(dot(xAxis, zAxis)) - 1.0 > -1e-3) xAxis = {0.0, 1.0, 0.0};
        Vec3 yAxis = normalized(cross(zAxis, xAxis));
        xAxis = normalized(cross(yAxis, zAxis));

        // matrix = basis * rotationAboutLocalZ(rotAngle).
        const double rotation = 360.0 * random.unit() / kDegreesPerRadian;
        const double sr = std::sin(rotation), cr = std::cos(rotation);
        const Vec3 forward = add(scale(xAxis, cr), scale(yAxis, sr));
        const Vec3 left = add(scale(xAxis, -sr), scale(yAxis, cr));
        angles = matrixAngles(forward, left, zAxis);
    }

    if (emission.props.size() >= 65535) {
        ++emission.overflowed;
        return;
    }

    DetailPropInstance instance;
    instance.type = model.type;
    instance.model = model.modelName;
    instance.origin = point;
    instance.angles = angles;
    instance.orientation = model.orientation;
    instance.positionUpperLeft = model.positionUpperLeft;
    instance.positionLowerRight = model.positionLowerRight;
    instance.texUpperLeft = model.texUpperLeft;
    instance.texLowerRight = model.texLowerRight;
    instance.shapeAngle = model.shapeAngle;
    instance.shapeSize = model.shapeSize;
    instance.swayAmount = model.swayAmount;
    instance.scale = 1.0f;
    if (model.type != DetailPropType::Model && model.randomScaleStdDev != 0.0f) {
        instance.scale = std::abs(random.gaussian(1.0f, model.randomScaleStdDev));
    }
    emission.props.push_back(std::move(instance));
}

// EmitDetailObjectsOnFace.
void emitOnFace(const hammer::vmf::BrushGeometry& brush, const hammer::vmf::FaceGeometry& face,
                const DetailObjectType& type, SourceRandom& random, DetailPropEmission& emission)
{
    if (face.vertices.size() < 3) return;
    const auto vertexAt = [&](std::size_t index) {
        return brush.vertices[face.vertices[index]];
    };
    for (const std::size_t index : face.vertices) {
        if (index >= brush.vertices.size()) return;
    }

    // The same triangle fan VBSP builds from the face's edges.
    const Vec3 first = vertexAt(0);
    for (std::size_t corner = 1; corner + 1 < face.vertices.size(); ++corner) {
        const Vec3 edge1 = subtract(vertexAt(corner), first);
        const Vec3 edge2 = subtract(vertexAt(corner + 1), first);

        const Vec3 areaVector = cross(edge1, edge2);
        const double normalLength = length(areaVector);
        if (normalLength < 1e-9) continue;
        const double area = 0.5 * normalLength;

        const int sampleCount = static_cast<int>(area * type.density * 0.000001);

        // VBSP derives the normal from the winding (negating the cross
        // product, which its face winding requires). This port takes the
        // direction from the face's own outward normal instead, so a
        // differently wound editor face still grows grass on the side the
        // surface actually faces - the sign here decides whether anything is
        // emitted at all, through the angle test in PlaceDetail.
        Vec3 normal = scale(areaVector, 1.0 / normalLength);
        if (dot(normal, face.normal) < 0.0) normal = scale(normal, -1.0);

        for (int sample = 0; sample < sampleCount; ++sample) {
            float u = random.unit();
            float v = random.unit();
            if (v > 1.0f - u) {
                u = 1.0f - u;
                v = 1.0f - v;
            }

            // Brush faces have no painted alpha, so they always resolve
            // against the fully opaque end of the group list.
            const float alpha = 1.0f;
            const int group = selectGroup(type, alpha, random);
            const int model = selectDetail(type.groups[static_cast<std::size_t>(group)], random);
            if (model < 0) continue;

            const Vec3 point = add(add(first, scale(edge1, u)), scale(edge2, v));
            placeDetail(type.groups[static_cast<std::size_t>(group)]
                            .models[static_cast<std::size_t>(model)],
                        point, normal, random, emission);
        }
    }
}

// The displacement grid's row width for a given power.
int displacementWidth(int power)
{
    return (1 << power) + 1;
}

// CCoreDispInfo::DispUVToSurf, reduced to what the emitter asks of it: the
// position, normal and painted alpha at a (u, v) on the displacement surface.
//
// The original walks the base face's flat grid, picks the cell's triangle
// using the same alternating diagonal the tessellation uses, and interpolates
// with barycentric coefficients taken on the FLAT vertices. Those flat
// vertices are a regular grid in (u, v), so the coefficients are computed here
// directly in grid space - identical for the rectangular base faces
// displacements are built on.
bool displacementSurfacePoint(const hammer::vmf::FaceGeometry& face, float u, float v,
                              Vec3& position, Vec3& normal, float& alpha)
{
    const int width = displacementWidth(face.displacementPower);
    if (width < 2) return false;
    if (face.displacementVertices.size() != static_cast<std::size_t>(width) *
                                            static_cast<std::size_t>(width)) {
        return false;
    }

    const float gridU = u * (static_cast<float>(width) - 1.000001f);
    const float gridV = v * (static_cast<float>(width) - 1.000001f);
    const int snapU = static_cast<int>(gridU);
    const int snapV = static_cast<int>(gridV);
    int nextU = std::min(snapU + 1, width - 1);
    int nextV = std::min(snapV + 1, width - 1);
    const float fracU = gridU - static_cast<float>(snapU);
    const float fracV = gridV - static_cast<float>(snapV);

    // The cell's diagonal alternates with the parity of the lower-left index,
    // the same rule the tessellation uses.
    const bool topLeftToBottomRight = (((snapV * width) + snapU) % 2) == 1;

    std::array<std::array<int, 2>, 3> corners{};
    if (topLeftToBottomRight) {
        constexpr float kTriangleEdgeEpsilon = 0.00001f;
        if ((fracU + fracV) >= (1.0f + kTriangleEdgeEpsilon)) {
            corners = {{{snapU, nextV}, {nextU, nextV}, {nextU, snapV}}};
        } else {
            corners = {{{snapU, snapV}, {snapU, nextV}, {nextU, snapV}}};
        }
    } else {
        if (fracU < fracV) {
            corners = {{{snapU, snapV}, {snapU, nextV}, {nextU, nextV}}};
        } else {
            corners = {{{snapU, snapV}, {nextU, nextV}, {nextU, snapV}}};
        }
    }

    // Barycentric coefficients of (gridU, gridV) in the flat triangle.
    const double x1 = corners[0][0], y1 = corners[0][1];
    const double x2 = corners[1][0], y2 = corners[1][1];
    const double x3 = corners[2][0], y3 = corners[2][1];
    const double determinant = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3);
    if (std::abs(determinant) < 1e-9) return false;
    double c0 = ((y2 - y3) * (gridU - x3) + (x3 - x2) * (gridV - y3)) / determinant;
    double c1 = ((y3 - y1) * (gridU - x3) + (x1 - x3) * (gridV - y3)) / determinant;
    c0 = std::clamp(c0, 0.0, 1.0);
    c1 = std::clamp(c1, 0.0, 1.0);
    const double c2 = std::clamp(1.0 - c0 - c1, 0.0, 1.0);

    const auto& v0 = face.displacementVertices[static_cast<std::size_t>(corners[0][1] * width +
                                                                       corners[0][0])];
    const auto& v1 = face.displacementVertices[static_cast<std::size_t>(corners[1][1] * width +
                                                                       corners[1][0])];
    const auto& v2 = face.displacementVertices[static_cast<std::size_t>(corners[2][1] * width +
                                                                       corners[2][0])];

    position = add(add(scale(v0.position, c0), scale(v1.position, c1)), scale(v2.position, c2));
    // DispUVToSurf takes the normal from the triangle it landed in rather than
    // from the stored vertex normals.
    const Vec3 edgeU = subtract(v2.position, v1.position);
    const Vec3 edgeV = subtract(v0.position, v1.position);
    normal = normalized(cross(edgeV, edgeU), face.normal);
    if (dot(normal, face.normal) < 0.0) normal = scale(normal, -1.0);
    // blendAlpha already holds the painted alpha normalized to 0..1 (it is not
    // the inverted VERTEXALPHATEXBLENDFACTOR form), which is exactly what
    // SelectGroup wants after VBSP's own "alpha /= 255".
    alpha = static_cast<float>(v0.blendAlpha * c0 + v1.blendAlpha * c1 + v2.blendAlpha * c2);
    return true;
}

// ComputeDisplacementFaceArea: the area of the BASE face, as two triangles.
double displacementBaseArea(const hammer::vmf::BrushGeometry& brush,
                            const hammer::vmf::FaceGeometry& face)
{
    if (face.vertices.size() < 4) return 0.0;
    double area = 0.0;
    const Vec3 first = brush.vertices[face.vertices[0]];
    for (std::size_t corner = 1; corner <= 2; ++corner) {
        const Vec3 edge1 = subtract(brush.vertices[face.vertices[corner]], first);
        const Vec3 edge2 = subtract(brush.vertices[face.vertices[corner + 1]], first);
        area += 0.5 * length(cross(edge1, edge2));
    }
    return area;
}

// EmitDetailObjectsOnDisplacementFace.
void emitOnDisplacementFace(const hammer::vmf::BrushGeometry& brush,
                            const hammer::vmf::FaceGeometry& face,
                            const DetailObjectType& type, SourceRandom& random,
                            DetailPropEmission& emission)
{
    const double area = displacementBaseArea(brush, face);
    const int sampleCount = static_cast<int>(area * type.density * 0.000001);

    for (int sample = 0; sample < sampleCount; ++sample) {
        const float u = random.unit();
        const float v = random.unit();

        Vec3 point{}, normal{};
        float alpha = 0.0f;
        if (!displacementSurfacePoint(face, u, v, point, normal, alpha)) continue;

        const int group = selectGroup(type, alpha, random);
        const int model = selectDetail(type.groups[static_cast<std::size_t>(group)], random);
        if (model < 0) continue;

        placeDetail(type.groups[static_cast<std::size_t>(group)]
                        .models[static_cast<std::size_t>(model)],
                    point, normal, random, emission);
    }
}

} // namespace

const DetailObjectType* DetailObjectDictionary::find(std::string_view name) const
{
    for (const DetailObjectType& type : types) {
        if (equalsCi(type.name, name)) return &type;
    }
    return nullptr;
}

DetailObjectDictionary parseDetailObjectDictionary(const hammer::vmf::Block& root)
{
    DetailObjectDictionary dictionary;
    for (const hammer::vmf::Entry& entry : root.entries) {
        if (entry.kind != hammer::vmf::Entry::Kind::ChildBlock || !entry.child) continue;
        const hammer::vmf::Block& typeBlock = *entry.child;
        if (!hasSubKeys(typeBlock)) continue;

        DetailObjectType type;
        type.name = typeBlock.name;
        type.density = floatValue(typeBlock, "density", 0.0f);

        for (const hammer::vmf::Entry& groupEntry : typeBlock.entries) {
            if (groupEntry.kind != hammer::vmf::Entry::Kind::ChildBlock || !groupEntry.child)
                continue;
            if (!hasSubKeys(*groupEntry.child)) continue;
            parseDetailGroup(type, *groupEntry.child);
        }
        dictionary.types.push_back(std::move(type));
    }
    return dictionary;
}

DetailObjectDictionary loadDetailObjectDictionary(const GameFileSystem& fileSystem,
                                                  std::string_view fileName)
{
    // FindDetailVBSPName's fallback.
    std::string path(fileName.empty() ? std::string_view("detail.vbsp") : fileName);
    const auto bytes = fileSystem.readFile(path);
    if (!bytes || bytes->empty()) return {};

    std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    const auto document = hammer::vmf::Document::parse(std::move(text));
    if (!document || document->roots().empty()) return {};
    return parseDetailObjectDictionary(document->roots().front());
}

DetailPropEmission emitDetailProps(const hammer::vmf::Scene& scene,
                                   const DetailObjectDictionary& dictionary,
                                   const DetailTypeForMaterial& detailTypeForMaterial)
{
    DetailPropEmission emission;
    if (dictionary.empty() || !detailTypeForMaterial) return emission;

    for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            // VBSP removes a displacement brush from the world outright -
            // LoadMapBrush does DispGetFaceInfo(b) and then b->numsides = 0 -
            // so only its displaced sides ever become BSP faces. The other
            // five sides carry no detail props no matter what they are
            // textured with, which matters because a mapper normally applies
            // the ground material to the whole brush and displaces only the
            // top. This is the same rule CMapSolid::Render3D masks 3D views
            // with (isFaceMaskedByDisplacementSolid).
            if (brush.hasDisplacement && !face.displacement) continue;
            // Nodraw and other tool surfaces are stripped before the BSP face
            // list is written, so they cannot carry props either.
            if (hammer::vmf::isToolMaterialPath(face.material)) continue;

            const std::string detailType = detailTypeForMaterial(face.material);
            if (detailType.empty()) continue;
            const DetailObjectType* type = dictionary.find(detailType);
            // VBSP warns about an unknown detail type and moves on.
            if (!type || type->groups.empty() || type->density <= 0.0f) continue;

            // VBSP seeds placement with the Hammer face id, so a face keeps its
            // objects when anything else in the map changes.
            SourceRandom random(face.sideId);

            const bool displacement = face.displacement && face.displacementPower >= 1 &&
                                      !face.displacementVertices.empty();
            if (displacement) {
                emitOnDisplacementFace(brush, face, *type, random, emission);
            } else {
                emitOnFace(brush, face, *type, random, emission);
            }
        }
    }
    return emission;
}

DetailPropFade detailPropFadeForScene(const hammer::vmf::Scene& scene)
{
    DetailPropFade fade;
    for (const hammer::vmf::EntityMarker& entity : scene.entities) {
        if (!equalsCi(entity.classname, "env_detail_controller")) continue;
        for (const auto& [key, value] : entity.properties) {
            // CEnvDetailController::KeyValue. "fademindist" feeds cl_detailfade,
            // which is the fade WIDTH - the naming in the original is not a
            // typo here.
            if (equalsCi(key, "fademindist"))
                fade.fadeWidth = std::min(fade.fadeWidth,
                                          static_cast<float>(std::atof(value.c_str())));
            else if (equalsCi(key, "fademaxdist"))
                fade.maxDistance = std::min(fade.maxDistance,
                                            static_cast<float>(std::atof(value.c_str())));
        }
        break;
    }
    return fade;
}

float detailPropAlpha(const Vec3& origin, const Vec3& viewOrigin, const DetailPropFade& fade)
{
    const double maxSquared = static_cast<double>(fade.maxDistance) * fade.maxDistance;
    double fadeSquared = fade.maxDistance - fade.fadeWidth;
    fadeSquared = fadeSquared > 0.0 ? fadeSquared * fadeSquared : 0.0;

    const Vec3 delta = subtract(origin, viewOrigin);
    const double squaredDistance = dot(delta, delta);
    if (squaredDistance >= maxSquared) return 0.0f;
    if (fadeSquared > 0.0 && squaredDistance > fadeSquared) {
        const double falloff = 1.0 / (maxSquared - fadeSquared);
        return static_cast<float>(std::clamp((maxSquared - squaredDistance) * falloff, 0.0, 1.0));
    }
    return 1.0f;
}

DetailSpriteQuad detailSpriteQuad(const DetailPropInstance& prop, const Vec3& viewOrigin)
{
    // CDetailModel::ComputeAngles: orientation 1 turns to face the viewer,
    // orientation 2 does the same but stays vertical.
    Vec3 angles = prop.angles;
    if (prop.orientation == 1 || prop.orientation == 2) {
        Vec3 direction = subtract(viewOrigin, prop.origin);
        if (prop.orientation == 2) direction.z = 0.0;
        angles = vectorAngles(direction);
    }

    Vec3 dx{}, dy{};
    angleVectors(angles, dx, dy);

    // CDetailModel::DrawTypeSprite.
    const double spriteScale = prop.scale;
    const std::array<double, 2> upperLeft{prop.positionUpperLeft[0] * spriteScale,
                                          prop.positionUpperLeft[1] * spriteScale};
    const std::array<double, 2> lowerRight{prop.positionLowerRight[0] * spriteScale,
                                           prop.positionLowerRight[1] * spriteScale};

    const Vec3 origin = add(add(prop.origin, scale(dx, upperLeft[0])), scale(dy, upperLeft[1]));
    const Vec3 acrossX = scale(dx, lowerRight[0] - upperLeft[0]);
    const Vec3 acrossY = scale(dy, lowerRight[1] - upperLeft[1]);

    DetailSpriteQuad quad;
    quad.corners[0] = origin;
    quad.corners[1] = add(origin, acrossY);
    quad.corners[2] = add(add(origin, acrossY), acrossX);
    quad.corners[3] = add(origin, acrossX);
    quad.texCoords[0] = {prop.texUpperLeft[0], prop.texUpperLeft[1]};
    quad.texCoords[1] = {prop.texUpperLeft[0], prop.texLowerRight[1]};
    quad.texCoords[2] = {prop.texLowerRight[0], prop.texLowerRight[1]};
    quad.texCoords[3] = {prop.texLowerRight[0], prop.texUpperLeft[1]};
    // Sprites are single quads with no back face; point the normal at the
    // viewer so a lit backend shades the side that is actually visible.
    quad.normal = normalized(cross(acrossY, acrossX), {0.0, 0.0, 1.0});
    if (dot(quad.normal, subtract(viewOrigin, prop.origin)) < 0.0)
        quad.normal = scale(quad.normal, -1.0);
    return quad;
}

} // namespace hammer::assets
