#include "VmfProjectedSurfaces.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace hammer::vmf {
namespace {

constexpr double Epsilon = 1e-7;
// How far in front of its host face a decal or overlay is built, so it wins the
// depth test instead of z-fighting.
//
// Any renderer that walks a ray through transparent surfaces has to step past a
// hit by less than this, or it jumps straight over the face the projected
// surface is stuck to and shows what lies beyond it. raytraced_preview.comp
// advances by raising the ray's minimum t (see advancedRayMinimum) rather than
// by a fixed world step, so it stays correct for any value here - but keep the
// coupling in mind before shrinking this.
constexpr double SurfaceOffset = 0.04;
constexpr double Sin45 = 0.70710678118654752440;

struct ClipVertex
{
    Vec3 position;
    double u{0.0};
    double v{0.0};
};

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(const Vec3& value, double amount) { return {value.x * amount, value.y * amount, value.z * amount}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double lengthSquared(const Vec3& value) { return dot(value, value); }
Vec3 normalized(const Vec3& value, const Vec3& fallback = {0.0, 0.0, 1.0})
{
    const double magnitude = std::sqrt(lengthSquared(value));
    return magnitude > Epsilon ? scale(value, 1.0 / magnitude) : fallback;
}

std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

const std::string* property(const std::vector<std::pair<std::string, std::string>>& values,
                            std::string_view key)
{
    const std::string wanted = lower(key);
    for (const auto& [name, value] : values) {
        if (lower(name) == wanted) return &value;
    }
    return nullptr;
}

const std::string* projectedProperty(const EntityMarker& entity, std::string_view key)
{
    // The compiler-facing info_overlay values live on the entity. Hammer also
    // serializes its helper state in an overlaydata child block. Prefer the
    // entity keys so SmartEdit changes take effect immediately, while retaining
    // the nested block as a compatibility fallback for editor-only VMFs.
    if (const std::string* value = property(entity.properties, key)) return value;
    return property(entity.overlayProperties, key);
}

bool parseNumbers(std::string_view text, double* output, std::size_t count)
{
    std::string cleaned(text);
    for (char& ch : cleaned) {
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',') ch = ' ';
    }
    std::istringstream stream(cleaned);
    for (std::size_t index = 0; index < count; ++index) {
        if (!(stream >> output[index]) || !std::isfinite(output[index])) return false;
    }
    return true;
}

bool parseVec3(const std::string* text, Vec3& value)
{
    if (!text) return false;
    double numbers[3];
    if (!parseNumbers(*text, numbers, 3)) return false;
    value = {numbers[0], numbers[1], numbers[2]};
    return true;
}

double parseDouble(const std::string* text, double fallback)
{
    if (!text) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text->c_str(), &end);
    return end && end != text->c_str() && std::isfinite(value) ? value : fallback;
}

std::vector<int> parseSideIds(const std::string* text)
{
    std::vector<int> ids;
    if (!text) return ids;
    std::string cleaned = *text;
    for (char& ch : cleaned) if (ch == ',' || ch == ';') ch = ' ';
    std::istringstream stream(cleaned);
    int id = -1;
    while (stream >> id) if (id >= 0) ids.push_back(id);
    return ids;
}

void decalBasis(const Vec3& surfaceNormal, Vec3& s, Vec3& t)
{
    const Vec3 n = normalized(surfaceNormal);
    if (std::abs(n.z) > Sin45) {
        s = {1.0, 0.0, 0.0};
        t = cross(s, n);
        s = cross(n, t);
    } else {
        t = {0.0, 0.0, -1.0};
        s = cross(n, t);
        t = cross(s, n);
    }
    s = normalized(s, {1.0, 0.0, 0.0});
    t = normalized(t, {0.0, 1.0, 0.0});
}

ClipVertex interpolate(const ClipVertex& a, const ClipVertex& b, double amount)
{
    return {add(a.position, scale(subtract(b.position, a.position), amount)),
            a.u + (b.u - a.u) * amount,
            a.v + (b.v - a.v) * amount};
}

std::vector<ClipVertex> clipHalfspace(const std::vector<ClipVertex>& input,
                                      const Vec3& planePoint,
                                      const Vec3& inwardNormal)
{
    std::vector<ClipVertex> output;
    if (input.empty()) return output;
    output.reserve(input.size() + 2);
    ClipVertex previous = input.back();
    double previousDistance = dot(inwardNormal, subtract(previous.position, planePoint));
    bool previousInside = previousDistance >= -1e-5;
    for (const ClipVertex& current : input) {
        const double currentDistance = dot(inwardNormal, subtract(current.position, planePoint));
        const bool currentInside = currentDistance >= -1e-5;
        if (currentInside != previousInside) {
            const double denominator = previousDistance - currentDistance;
            if (std::abs(denominator) > Epsilon) {
                output.push_back(interpolate(previous, current,
                    std::clamp(previousDistance / denominator, 0.0, 1.0)));
            }
        }
        if (currentInside) output.push_back(current);
        previous = current;
        previousDistance = currentDistance;
        previousInside = currentInside;
    }
    return output;
}

std::vector<ClipVertex> projectAndClip(const std::array<ClipVertex, 4>& source,
                                       const Vec3& projectionDirection,
                                       const std::vector<Vec3>& targetPolygon,
                                       const Vec3& targetNormal)
{
    if (targetPolygon.size() < 3) return {};
    const Vec3 normal = normalized(targetNormal);
    const Vec3 direction = normalized(projectionDirection, normal);
    const double denominator = dot(normal, direction);
    if (std::abs(denominator) < 1e-6) return {};
    const double planeDistance = dot(normal, targetPolygon.front());

    std::vector<ClipVertex> polygon;
    polygon.reserve(source.size());
    for (ClipVertex vertex : source) {
        const double travel = (dot(normal, vertex.position) - planeDistance) / denominator;
        vertex.position = subtract(vertex.position, scale(direction, travel));
        polygon.push_back(vertex);
    }

    Vec3 center{};
    for (const Vec3& point : targetPolygon) center = add(center, point);
    center = scale(center, 1.0 / static_cast<double>(targetPolygon.size()));
    for (std::size_t index = 0; index < targetPolygon.size() && polygon.size() >= 3; ++index) {
        const Vec3& first = targetPolygon[index];
        const Vec3& second = targetPolygon[(index + 1) % targetPolygon.size()];
        Vec3 inward = cross(normal, subtract(second, first));
        if (dot(inward, subtract(center, first)) < 0.0) inward = scale(inward, -1.0);
        polygon = clipHalfspace(polygon, first, inward);
    }
    return polygon;
}

void appendPolygon(ProjectedSurface& destination,
                   const std::vector<ClipVertex>& polygon,
                   const Vec3& surfaceNormal)
{
    if (polygon.size() < 3) return;
    const Vec3 normal = normalized(surfaceNormal);
    const Vec3 offset = scale(normal, SurfaceOffset);
    for (std::size_t index = 1; index + 1 < polygon.size(); ++index) {
        const ClipVertex triangle[3] = {polygon[0], polygon[index], polygon[index + 1]};
        const Vec3 area = cross(subtract(triangle[1].position, triangle[0].position),
                                subtract(triangle[2].position, triangle[0].position));
        if (lengthSquared(area) < 1e-10) continue;
        for (const ClipVertex& vertex : triangle) {
            destination.triangles.push_back({add(vertex.position, offset), normal,
                                             vertex.u, vertex.v});
        }
    }
}

std::vector<Vec3> facePolygon(const BrushGeometry& brush, const FaceGeometry& face)
{
    std::vector<Vec3> points;
    points.reserve(face.vertices.size());
    for (std::size_t index : face.vertices) {
        if (index < brush.vertices.size()) points.push_back(brush.vertices[index]);
    }
    return points;
}

template <typename Callback>
void forEachTargetPatch(const BrushGeometry& brush, const FaceGeometry& face,
                        Callback&& callback)
{
    if (face.displacement && face.displacementIndices.size() >= 3) {
        for (std::size_t index = 0; index + 2 < face.displacementIndices.size(); index += 3) {
            std::vector<Vec3> triangle;
            triangle.reserve(3);
            for (int corner = 0; corner < 3; ++corner) {
                const std::size_t vertexIndex = face.displacementIndices[index + static_cast<std::size_t>(corner)];
                if (vertexIndex >= face.displacementVertices.size()) break;
                triangle.push_back(face.displacementVertices[vertexIndex].position);
            }
            if (triangle.size() != 3) continue;
            Vec3 normal = normalized(cross(subtract(triangle[1], triangle[0]),
                                           subtract(triangle[2], triangle[0])), face.normal);
            if (dot(normal, face.normal) < 0.0) normal = scale(normal, -1.0);
            callback(triangle, normal);
        }
        return;
    }
    const std::vector<Vec3> polygon = facePolygon(brush, face);
    if (polygon.size() >= 3) callback(polygon, face.normal);
}

std::array<ClipVertex, 4> makeQuad(const Vec3& origin, const Vec3& axisU,
                                   const Vec3& axisV,
                                   double minimumU, double maximumU,
                                   double minimumV, double maximumV,
                                   double startU, double endU,
                                   double startV, double endV)
{
    const auto point = [&](double u, double v) {
        return add(origin, add(scale(axisU, u), scale(axisV, v)));
    };
    return {{
        {point(minimumU, minimumV), startU, startV},
        {point(minimumU, maximumV), startU, endV},
        {point(maximumU, maximumV), endU, endV},
        {point(maximumU, minimumV), endU, startV}
    }};
}

void buildDecal(Scene& scene, EntityMarker& entity,
                const ProjectedMaterialResolver& materialResolver)
{
    const std::string* texture = property(entity.properties, "texture");
    if (!texture || texture->empty() || !materialResolver) return;
    const auto material = materialResolver(*texture);
    if (!material || material->width <= 0 || material->height <= 0) return;

    ProjectedSurface surface;
    surface.material = normalizeMaterialPath(*texture);
    surface.kind = ProjectedSurfaceKind::Decal;
    const double scaleValue = std::clamp(material->decalScale, 0.001, 128.0);

    for (const BrushGeometry& brush : scene.brushes) {
        for (const FaceGeometry& face : brush.faces) {
            const std::vector<Vec3> basePolygon = facePolygon(brush, face);
            if (basePolygon.size() < 3) continue;
            const Vec3 normal = normalized(face.normal);
            const double planeDistance = dot(normal, basePolygon.front());
            const double distance = dot(normal, entity.origin) - planeDistance;
            if (distance > 16.0 || distance < -0.0001) continue;

            Vec3 axisU, axisV;
            decalBasis(normal, axisU, axisV);
            const double halfWidth = static_cast<double>(material->width) * scaleValue * 0.5;
            const double halfHeight = static_cast<double>(material->height) * scaleValue * 0.5;
            const auto quad = makeQuad(entity.origin, axisU, axisV,
                                       -halfWidth, halfWidth, -halfHeight, halfHeight,
                                       0.0, 1.0, 0.0, 1.0);
            const std::size_t before = surface.triangles.size();
            forEachTargetPatch(brush, face, [&](const std::vector<Vec3>& target, const Vec3& targetNormal) {
                appendPolygon(surface, projectAndClip(quad, normal, target, targetNormal), targetNormal);
            });
            if (surface.triangles.size() > before) {
                entity.projectedSourceSolidIds.push_back(brush.id);
                break; // Hammer applies at most one eligible face per solid.
            }
        }
    }
    if (!surface.triangles.empty()) entity.projectedSurfaces.push_back(std::move(surface));
}

void buildOverlay(Scene& scene, EntityMarker& entity)
{
    const std::string* materialName = projectedProperty(entity, "material");
    if (!materialName || materialName->empty()) return;

    Vec3 origin = entity.origin;
    Vec3 axisU{1.0, 0.0, 0.0};
    Vec3 axisV{0.0, 1.0, 0.0};
    Vec3 normal{0.0, 0.0, 1.0};
    parseVec3(projectedProperty(entity, "BasisOrigin"), origin);
    parseVec3(projectedProperty(entity, "BasisU"), axisU);
    parseVec3(projectedProperty(entity, "BasisV"), axisV);
    if (!parseVec3(projectedProperty(entity, "BasisNormal"), normal)) normal = cross(axisU, axisV);
    axisU = normalized(axisU, {1.0, 0.0, 0.0});
    axisV = normalized(axisV, {0.0, 1.0, 0.0});
    normal = normalized(normal, normalized(cross(axisU, axisV)));

    const Vec3 defaults[4] = {{-32.0, -32.0, 0.0}, {-32.0, 32.0, 0.0},
                              {32.0, 32.0, 0.0}, {32.0, -32.0, 0.0}};
    Vec3 uv[4] = {defaults[0], defaults[1], defaults[2], defaults[3]};
    for (int index = 0; index < 4; ++index) {
        parseVec3(projectedProperty(entity, "uv" + std::to_string(index)), uv[index]);
    }
    const double startU = parseDouble(projectedProperty(entity, "StartU"), 0.0);
    const double endU = parseDouble(projectedProperty(entity, "EndU"), 1.0);
    const double startV = parseDouble(projectedProperty(entity, "StartV"), 0.0);
    const double endV = parseDouble(projectedProperty(entity, "EndV"), 1.0);

    const auto point = [&](const Vec3& basisCoordinate) {
        return add(origin, add(scale(axisU, basisCoordinate.x), scale(axisV, basisCoordinate.y)));
    };
    const std::array<ClipVertex, 4> quad{{
        {point(uv[0]), startU, startV},
        {point(uv[1]), startU, endV},
        {point(uv[2]), endU, endV},
        {point(uv[3]), endU, startV}
    }};

    const std::vector<int> sideIds = parseSideIds(projectedProperty(entity, "sides"));
    const std::unordered_set<int> sideSet(sideIds.begin(), sideIds.end());
    ProjectedSurface surface;
    surface.material = normalizeMaterialPath(*materialName);
    surface.kind = ProjectedSurfaceKind::Overlay;

    auto appendFace = [&](const BrushGeometry& brush, const FaceGeometry& face) {
        forEachTargetPatch(brush, face, [&](const std::vector<Vec3>& target, const Vec3& targetNormal) {
            appendPolygon(surface, projectAndClip(quad, normal, target, targetNormal), targetNormal);
        });
    };

    if (!sideSet.empty()) {
        for (const BrushGeometry& brush : scene.brushes) {
            for (const FaceGeometry& face : brush.faces) {
                if (sideSet.contains(face.sideId)) {
                    entity.projectedSourceSolidIds.push_back(brush.id);
                    appendFace(brush, face);
                }
            }
        }
    } else {
        // Legacy/custom maps occasionally omit the side list. Match Hammer's
        // practical behavior by attaching to nearby, similarly-facing surfaces.
        double nearest = std::numeric_limits<double>::infinity();
        const BrushGeometry* nearestBrush = nullptr;
        const FaceGeometry* nearestFace = nullptr;
        for (const BrushGeometry& brush : scene.brushes) {
            for (const FaceGeometry& face : brush.faces) {
                const std::vector<Vec3> polygon = facePolygon(brush, face);
                if (polygon.empty()) continue;
                const Vec3 faceNormal = normalized(face.normal);
                if (std::abs(dot(faceNormal, normal)) < 0.05) continue;
                const double distance = std::abs(dot(faceNormal, origin) - dot(faceNormal, polygon.front()));
                if (distance < nearest) {
                    nearest = distance;
                    nearestBrush = &brush;
                    nearestFace = &face;
                }
            }
        }
        if (nearestBrush && nearestFace) {
            entity.projectedSourceSolidIds.push_back(nearestBrush->id);
            appendFace(*nearestBrush, *nearestFace);
        }
    }

    if (!surface.triangles.empty()) entity.projectedSurfaces.push_back(std::move(surface));
}

} // namespace

void rebuildEntityProjectedSurfaces(Scene& scene, EntityMarker& entity,
                                    const ProjectedMaterialResolver& materialResolver)
{
    entity.projectedSurfaces.clear();
    entity.projectedSourceSolidIds.clear();
    const std::string classname = lower(entity.classname);
    if (classname == "infodecal" || classname == "info_decal") {
        buildDecal(scene, entity, materialResolver);
    } else if (classname == "info_overlay") {
        buildOverlay(scene, entity);
    }
}

void rebuildProjectedSurfaceGeometry(Scene& scene,
                                     const ProjectedMaterialResolver& materialResolver)
{
    for (EntityMarker& entity : scene.entities) {
        rebuildEntityProjectedSurfaces(scene, entity, materialResolver);
    }
}

bool projectedEntityDependsOnSolids(const Scene& scene, const EntityMarker& entity,
                                    const std::unordered_set<int>& changedSolidIds,
                                    const ProjectedMaterialResolver& materialResolver)
{
    if (changedSolidIds.empty()) return false;
    const std::string classname = lower(entity.classname);
    const bool isDecal = classname == "infodecal" || classname == "info_decal";
    const bool isOverlay = classname == "info_overlay";
    if (!isDecal && !isOverlay) return false;

    // Currently projecting onto a changed solid (covers a brush moving away).
    for (const int id : entity.projectedSourceSolidIds) {
        if (changedSolidIds.contains(id)) return true;
    }

    if (isOverlay) {
        // An overlay only ever attaches to its listed sides, so only the
        // solids carrying those sides matter. Without a side list the legacy
        // nearest-face fallback can jump to any brush.
        const std::vector<int> sideIds = parseSideIds(projectedProperty(entity, "sides"));
        if (sideIds.empty()) return true;
        const std::unordered_set<int> sideSet(sideIds.begin(), sideIds.end());
        for (const BrushGeometry& brush : scene.brushes) {
            if (!changedSolidIds.contains(brush.id)) continue;
            for (const FaceGeometry& face : brush.faces) {
                if (sideSet.contains(face.sideId)) return true;
            }
        }
        return false;
    }

    // Decal: a changed solid can only start receiving the decal when a face
    // plane passes within 16 units of the origin and the projected quad
    // reaches it. Conservative sphere test against the brush bounds.
    const std::string* texture = property(entity.properties, "texture");
    if (!texture || texture->empty() || !materialResolver) return false;
    const auto material = materialResolver(*texture);
    if (!material || material->width <= 0 || material->height <= 0) return false;
    const double scaleValue = std::clamp(material->decalScale, 0.001, 128.0);
    const double halfWidth = static_cast<double>(material->width) * scaleValue * 0.5;
    const double halfHeight = static_cast<double>(material->height) * scaleValue * 0.5;
    const double range = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight) + 16.0;
    for (const BrushGeometry& brush : scene.brushes) {
        if (!changedSolidIds.contains(brush.id) || brush.vertices.empty()) continue;
        Vec3 minimum = brush.vertices.front();
        Vec3 maximum = minimum;
        for (const Vec3& vertex : brush.vertices) {
            minimum.x = std::min(minimum.x, vertex.x);
            minimum.y = std::min(minimum.y, vertex.y);
            minimum.z = std::min(minimum.z, vertex.z);
            maximum.x = std::max(maximum.x, vertex.x);
            maximum.y = std::max(maximum.y, vertex.y);
            maximum.z = std::max(maximum.z, vertex.z);
        }
        if (entity.origin.x >= minimum.x - range && entity.origin.x <= maximum.x + range &&
            entity.origin.y >= minimum.y - range && entity.origin.y <= maximum.y + range &&
            entity.origin.z >= minimum.z - range && entity.origin.z <= maximum.z + range) {
            return true;
        }
    }
    return false;
}

} // namespace hammer::vmf
