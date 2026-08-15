#include "VmfScene.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace hammer::vmf {
namespace {

struct Plane
{
    Vec3 normal;
    double distance{0.0};
    const Block* side{nullptr};
};

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 multiply(const Vec3& value, double scalar) { return {value.x * scalar, value.y * scalar, value.z * scalar}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double lengthSquared(const Vec3& value) { return dot(value, value); }

double distanceSquared(const Vec3& a, const Vec3& b)
{
    return lengthSquared(subtract(a, b));
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

bool parseVec3(const std::string* text, Vec3& value)
{
    if (!text) return false;
    double values[3];
    if (!parseNumbers(*text, values, 3)) return false;
    value = {values[0], values[1], values[2]};
    return true;
}

bool parsePlane(const std::string* text, Plane& plane)
{
    if (!text) return false;
    double values[9];
    if (!parseNumbers(*text, values, 9)) return false;
    const Vec3 a{values[0], values[1], values[2]};
    const Vec3 b{values[3], values[4], values[5]};
    const Vec3 c{values[6], values[7], values[8]};
    // CMapFace::CalcPlane feeds the three VMF plane points straight into
    // GetNormalFromPoints( p0, p1, p2 ) (hammer/hammer_mathlib.cpp), which is
    //     v1 = p0 - p1;  v2 = p2 - p1;  normal = v1 x v2
    // and then plane.dist = DotProduct( p0, normal ). This is the negation of
    // (p1 - p0) x (p2 - p0). Getting it backwards silently inverts every face
    // normal in the scene, which is what made displacement elevation, vertex
    // normals, tangent handedness and decal offsets all come out reversed.
    Vec3 normal = cross(subtract(a, b), subtract(c, b));
    const double magnitude = std::sqrt(lengthSquared(normal));
    if (magnitude < 1e-9) return false;
    normal = multiply(normal, 1.0 / magnitude);
    plane = {normal, dot(normal, a), nullptr};
    return true;
}

int parseInt(const std::string* text)
{
    if (!text) return -1;
    int value = -1;
    const char* const end = text->data() + text->size();
    const auto result = std::from_chars(text->data(), end, value);
    return result.ec == std::errc{} && result.ptr == end ? value : -1;
}

double parseDouble(const std::string* text, double fallback = 0.0)
{
    if (!text) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text->c_str(), &end);
    return end && end != text->c_str() && std::isfinite(value) ? value : fallback;
}

Vec3 normalized(const Vec3& value, const Vec3& fallback)
{
    const double magnitude = std::sqrt(lengthSquared(value));
    if (magnitude < 1e-9) return fallback;
    return multiply(value, 1.0 / magnitude);
}

const Block* firstChild(const Block& block, std::string_view name)
{
    const auto children = block.children(name);
    return children.empty() ? nullptr : children.front();
}

void parseScalarRows(const Block* block, int gridSize, std::vector<double>& output)
{
    if (!block) return;
    std::vector<double> row(static_cast<std::size_t>(gridSize));
    for (int y = 0; y < gridSize; ++y) {
        const std::string key = "row" + std::to_string(y);
        const std::string* text = block->value(key);
        if (!text || !parseNumbers(*text, row.data(), row.size())) continue;
        for (int x = 0; x < gridSize; ++x) {
            output[static_cast<std::size_t>(y * gridSize + x)] = row[static_cast<std::size_t>(x)];
        }
    }
}

void parseVectorRows(const Block* block, int gridSize, std::vector<Vec3>& output)
{
    if (!block) return;
    std::vector<double> row(static_cast<std::size_t>(gridSize) * 3u);
    for (int y = 0; y < gridSize; ++y) {
        const std::string key = "row" + std::to_string(y);
        const std::string* text = block->value(key);
        if (!text || !parseNumbers(*text, row.data(), row.size())) continue;
        for (int x = 0; x < gridSize; ++x) {
            const std::size_t source = static_cast<std::size_t>(x) * 3u;
            output[static_cast<std::size_t>(y * gridSize + x)] =
                {row[source], row[source + 1], row[source + 2]};
        }
    }
}

void buildDisplacement(FaceGeometry& face, const std::vector<Vec3>& brushVertices,
                       const Block& side)
{
    const Block* disp = firstChild(side, "dispinfo");
    if (!disp || face.vertices.size() != 4) return;

    const int power = parseInt(disp->value("power"));
    if (power < 2 || power > 4) return;
    const int cells = 1 << power;
    const int gridSize = cells + 1;
    const std::size_t vertexCount = static_cast<std::size_t>(gridSize * gridSize);

    // CCoreDispSurface::FindSurfPointStartIndex + AdjustSurfPointData only
    // ROTATE the four base-surface points so point 0 is nearest startposition.
    // It does not reverse the winding. Keep the brush face order intact here.
    std::array<std::size_t, 4> corners{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        corners[index] = face.vertices[index];
        if (corners[index] >= brushVertices.size()) return;
    }

    Vec3 startPosition{};
    if (parseVec3(disp->value("startposition"), startPosition)) {
        std::size_t startCorner = 0;
        double bestDistance = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < corners.size(); ++index) {
            const double candidate = distanceSquared(brushVertices[corners[index]], startPosition);
            if (candidate < bestDistance) {
                bestDistance = candidate;
                startCorner = index;
            }
        }
        std::rotate(corners.begin(), corners.begin() + static_cast<std::ptrdiff_t>(startCorner),
                    corners.end());
    }

    const Vec3& point0 = brushVertices[corners[0]];
    const Vec3& point1 = brushVertices[corners[1]];
    const Vec3& point2 = brushVertices[corners[2]];
    const Vec3& point3 = brushVertices[corners[3]];

    std::vector<Vec3> fieldVectors(vertexCount, Vec3{});
    std::vector<Vec3> offsets(vertexCount, Vec3{});
    std::vector<double> fieldDistances(vertexCount, 0.0);
    std::vector<double> alphas(vertexCount, 0.0);
    parseVectorRows(firstChild(*disp, "normals"), gridSize, fieldVectors);
    parseVectorRows(firstChild(*disp, "offsets"), gridSize, offsets);
    parseScalarRows(firstChild(*disp, "distances"), gridSize, fieldDistances);
    parseScalarRows(firstChild(*disp, "alphas"), gridSize, alphas);

    const double elevation = parseDouble(disp->value("elevation"), 0.0);
    const double inverseIntervals = 1.0 / static_cast<double>(gridSize - 1);
    const Vec3 edgeInterval0 = multiply(subtract(point1, point0), inverseIntervals);
    const Vec3 edgeInterval1 = multiply(subtract(point2, point3), inverseIntervals);
    const Vec3 elevationVector = multiply(face.normal, elevation);

    // Hammer's CMapDisp::InitDispSurfaceData calls CMapFace::GetTexCoord for
    // the four UNDISTORTED face corners before AdjustSurfPointData rotates
    // those coordinates along with the points. CCoreDispInfo::CalcDispSurfCoords
    // then bilinearly interpolates the four values. Keep them in pixel space
    // here; normalizing after interpolation is mathematically identical and
    // lets this VMF-only layer remain independent from loaded VTF dimensions.
    const double uScale = std::abs(face.uAxis.scale) < 1e-9 ? 0.25 : face.uAxis.scale;
    const double vScale = std::abs(face.vAxis.scale) < 1e-9 ? 0.25 : face.vAxis.scale;
    std::array<double, 4> cornerU{};
    std::array<double, 4> cornerV{};
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        const Vec3& point = brushVertices[corners[corner]];
        cornerU[corner] = dot(face.uAxis.direction, point) / uScale + face.uAxis.shift;
        cornerV[corner] = dot(face.vAxis.direction, point) / vScale + face.vAxis.shift;
    }
    const double texEdgeU0 = (cornerU[1] - cornerU[0]) * inverseIntervals;
    const double texEdgeV0 = (cornerV[1] - cornerV[0]) * inverseIntervals;
    const double texEdgeU1 = (cornerU[2] - cornerU[3]) * inverseIntervals;
    const double texEdgeV1 = (cornerV[2] - cornerV[3]) * inverseIntervals;

    face.displacementVertices.assign(vertexCount, DisplacementVertex{});

    // Port of CCoreDispInfo::GenerateDispSurf plus CalcDispSurfCoords(false,0).
    // The VMF offset vector is the
    // editor's already-authored positional offset, while normals/distances are
    // the displacement field vector and distance. Match Source here: field
    // vectors default to zero and are consumed exactly as authored (no
    // normalization or face-normal fallback).
    for (int row = 0; row < gridSize; ++row) {
        const Vec3 end0 = add(point0, multiply(edgeInterval0, static_cast<double>(row)));
        const Vec3 end1 = add(point3, multiply(edgeInterval1, static_cast<double>(row)));
        const Vec3 segmentInterval =
            multiply(subtract(end1, end0), inverseIntervals);

        // CCoreDispInfo::CalcDispSurfCoords uses exactly the same two-edge /
        // perpendicular-segment interpolation as GenerateDispSurf.
        const double texEndU0 = cornerU[0] + texEdgeU0 * static_cast<double>(row);
        const double texEndV0 = cornerV[0] + texEdgeV0 * static_cast<double>(row);
        const double texEndU1 = cornerU[3] + texEdgeU1 * static_cast<double>(row);
        const double texEndV1 = cornerV[3] + texEdgeV1 * static_cast<double>(row);
        const double texSegmentU = (texEndU1 - texEndU0) * inverseIntervals;
        const double texSegmentV = (texEndV1 - texEndV0) * inverseIntervals;

        for (int column = 0; column < gridSize; ++column) {
            const std::size_t index = static_cast<std::size_t>(row * gridSize + column);
            const Vec3 flatPosition =
                add(end0, multiply(segmentInterval, static_cast<double>(column)));
            DisplacementVertex& vertex = face.displacementVertices[index];
            vertex.position = add(add(add(flatPosition, elevationVector), offsets[index]),
                                  multiply(fieldVectors[index], fieldDistances[index]));
            vertex.normal = face.normal;
            vertex.textureU = texEndU0 + texSegmentU * static_cast<double>(column);
            vertex.textureV = texEndV0 + texSegmentV * static_cast<double>(column);
            // Painted alpha 255 shows $basetexture2. Source's shader lerps
            // basetexture2 -> basetexture by (255 - m_Alpha)/255; every renderer
            // here mixes basetexture -> basetexture2 by blendAlpha, so the
            // equivalent factor is m_Alpha/255 with no inversion.
            vertex.blendAlpha = std::clamp(alphas[index] / 255.0, 0.0, 1.0);
        }
    }

    // Port of CCoreDispInfo::GenerateCollisionSurface. Source alternates each
    // cell diagonal using the LINEAR lower-left index parity. This index list is
    // the authoritative full-resolution render/collision triangle list copied
    // by Hammer's CMapDisp.
    face.displacementIndices.clear();
    face.displacementIndices.reserve(static_cast<std::size_t>(cells * cells * 6));
    auto pushIndex = [&](int value) {
        face.displacementIndices.push_back(static_cast<std::size_t>(value));
    };
    for (int row = 0; row < cells; ++row) {
        for (int column = 0; column < cells; ++column) {
            const int ndx = row * gridSize + column;
            if ((ndx & 1) != 0) {
                // CCoreDispInfo::BuildTriTLtoBR.
                pushIndex(ndx);
                pushIndex(ndx + gridSize);
                pushIndex(ndx + 1);
                pushIndex(ndx + 1);
                pushIndex(ndx + gridSize);
                pushIndex(ndx + gridSize + 1);
            } else {
                // CCoreDispInfo::BuildTriBLtoTR.
                pushIndex(ndx);
                pushIndex(ndx + gridSize);
                pushIndex(ndx + gridSize + 1);
                pushIndex(ndx);
                pushIndex(ndx + gridSize + 1);
                pushIndex(ndx + 1);
            }
        }
    }

    // Vertex normals are averaged from the triangles that are ACTUALLY in the
    // rendered/collision mesh. The previous version split each surrounding quad
    // along a fixed diagonal, so on roughly half the quads it averaged
    // triangles that do not exist in the surface above, and shading did not
    // follow the visible faceting.
    //
    // The checkerboard split used below is the same one Hammer relies on when
    // it maps a point on a displacement back to a triangle -- see
    // CMapOverlay::GetTriVerts (hammer/mapoverlay.cpp), which selects the
    // diagonal from ( ( nSnapV * nWidth ) + nSnapU ) % 2. Orientation is
    // therefore correct by construction: for a base quad wound clockwise about
    // the outward normal, cross( C - A, B - A ) of every emitted triangle
    // points outward, so no sign correction against face.normal is needed. The
    // old dot() < 0 flip could wrongly invert legitimately steep or
    // overhanging displacement normals.
    std::vector<Vec3> normalAccumulator(vertexCount, Vec3{});
    std::vector<int> normalCounts(vertexCount, 0);
    for (std::size_t triangle = 0; triangle + 2 < face.displacementIndices.size();
         triangle += 3) {
        const std::size_t a = face.displacementIndices[triangle];
        const std::size_t b = face.displacementIndices[triangle + 1];
        const std::size_t c = face.displacementIndices[triangle + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        const Vec3& positionA = face.displacementVertices[a].position;
        const Vec3 triangleNormal = normalized(
            cross(subtract(face.displacementVertices[c].position, positionA),
                  subtract(face.displacementVertices[b].position, positionA)),
            Vec3{});
        if (lengthSquared(triangleNormal) < 0.5) continue;
        // Average of UNIT triangle normals, as CCoreDispInfo does, rather than
        // an area-weighted sum: the corner and edge triangles of a displacement
        // differ wildly in area and would otherwise skew the border normals.
        for (const std::size_t index : {a, b, c}) {
            normalAccumulator[index] = add(normalAccumulator[index], triangleNormal);
            ++normalCounts[index];
        }
    }
    for (std::size_t index = 0; index < vertexCount; ++index) {
        face.displacementVertices[index].normal =
            normalCounts[index] > 0
                ? normalized(normalAccumulator[index], face.normal)
                : face.normal;
    }

    // Port CCoreDispInfo::GenerateDispSurfTangentSpaces exactly: T begins as
    // the base surface V axis, S = normal x T, then T is reconstructed from
    // S x normal. Source flips S when the surface mapping axes have the same
    // handedness test as the base plane.
    const Vec3 sourceSAxis = face.uAxis.direction;
    const Vec3 sourceTAxis = face.vAxis.direction;
    const bool flipTangentS = dot(face.normal, cross(sourceSAxis, sourceTAxis)) > 0.0;
    for (DisplacementVertex& vertex : face.displacementVertices) {
        const Vec3 nForFrame = normalized(vertex.normal, face.normal);
        Vec3 tangentT = normalized(sourceTAxis, Vec3{0.0, 1.0, 0.0});
        Vec3 tangentS = normalized(cross(nForFrame, tangentT),
                                   std::abs(nForFrame.z) < 0.9
                                       ? Vec3{-nForFrame.y, nForFrame.x, 0.0}
                                       : Vec3{0.0, -nForFrame.z, nForFrame.y});
        tangentT = normalized(cross(tangentS, nForFrame), tangentT);
        if (flipTangentS) tangentS = multiply(tangentS, -1.0);
        vertex.tangentS = tangentS;
        vertex.tangentT = tangentT;
    }

    face.displacement = !face.displacementIndices.empty();
    face.displacementPower = power;
}

bool intersect(const Plane& a, const Plane& b, const Plane& c, Vec3& point)
{
    const Vec3 bCrossC = cross(b.normal, c.normal);
    const double denominator = dot(a.normal, bCrossC);
    if (std::abs(denominator) < 1e-8) return false;
    point = multiply(add(add(multiply(bCrossC, a.distance),
                             multiply(cross(c.normal, a.normal), b.distance)),
                         multiply(cross(a.normal, b.normal), c.distance)),
                     1.0 / denominator);
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

std::vector<Vec3> candidateVertices(const std::vector<Plane>& planes, double sign)
{
    constexpr double InsideTolerance = 0.08;
    constexpr double MergeDistanceSquared = 0.01;
    std::vector<Vec3> vertices;
    for (std::size_t i = 0; i < planes.size(); ++i) {
        for (std::size_t j = i + 1; j < planes.size(); ++j) {
            for (std::size_t k = j + 1; k < planes.size(); ++k) {
                Vec3 point;
                if (!intersect(planes[i], planes[j], planes[k], point)) continue;
                bool inside = true;
                for (const Plane& plane : planes) {
                    if (sign * (dot(plane.normal, point) - plane.distance) > InsideTolerance) {
                        inside = false;
                        break;
                    }
                }
                if (!inside) continue;
                if (std::none_of(vertices.begin(), vertices.end(), [&](const Vec3& existing) {
                        return distanceSquared(existing, point) < MergeDistanceSquared;
                    })) {
                    vertices.push_back(point);
                }
            }
        }
    }
    return vertices;
}

bool parseTextureAxis(const std::string* text, TextureAxis& axis)
{
    if (!text) return false;
    double values[5];
    if (!parseNumbers(*text, values, 5)) return false;
    axis.direction = {values[0], values[1], values[2]};
    axis.shift = values[3];
    axis.scale = std::abs(values[4]) < 1e-9 ? 0.25 : values[4];
    return true;
}

std::vector<std::size_t> orderedFaceVertices(const std::vector<Vec3>& vertices,
                                             const Plane& plane)
{
    constexpr double PlaneTolerance = 0.12;
    std::vector<std::size_t> indices;
    Vec3 center{};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (std::abs(dot(plane.normal, vertices[i]) - plane.distance) <= PlaneTolerance) {
            indices.push_back(i);
            center = add(center, vertices[i]);
        }
    }
    if (indices.size() < 3) return {};
    center = multiply(center, 1.0 / static_cast<double>(indices.size()));

    Vec3 reference = std::abs(plane.normal.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0};
    Vec3 tangent = cross(reference, plane.normal);
    const double tangentLength = std::sqrt(lengthSquared(tangent));
    if (tangentLength < 1e-9) return {};
    tangent = multiply(tangent, 1.0 / tangentLength);
    Vec3 bitangent = cross(plane.normal, tangent);
    // (tangent, bitangent, normal) is right handed, so ASCENDING atan2 winds
    // counter-clockwise about the normal. CMapFace stores its points CLOCKWISE
    // about the outward normal: ::CheckFace (hammer/ssolid.cpp) builds
    // edgenormal = normal x (Points[i+1] - Points[i]) and rejects the face
    // unless every other point is on the NEGATIVE side of it, which only holds
    // for a clockwise winding. CMapDisp::InitDispSurfaceData feeds these points
    // to the displacement surface in that order, so descend here to match.
    std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        const Vec3 da = subtract(vertices[a], center);
        const Vec3 db = subtract(vertices[b], center);
        const double aa = std::atan2(dot(da, bitangent), dot(da, tangent));
        const double ab = std::atan2(dot(db, bitangent), dot(db, tangent));
        return aa > ab;
    });
    return indices;
}

BrushGeometry buildBrush(const Block& solid, int ownerEntityId)
{
    std::vector<Plane> planes;
    for (const Block* side : solid.children("side")) {
        Plane plane;
        if (parsePlane(side->value("plane"), plane)) {
            plane.side = side;
            planes.push_back(plane);
        }
    }

    BrushGeometry brush;
    brush.id = parseInt(solid.value("id"));
    brush.object = {ObjectType::Solid, brush.id};
    brush.ownerEntityId = ownerEntityId;
    if (planes.size() < 4) return brush;

    std::vector<Vec3> negative = candidateVertices(planes, 1.0);
    std::vector<Vec3> positive = candidateVertices(planes, -1.0);
    brush.vertices = positive.size() > negative.size() ? std::move(positive) : std::move(negative);
    if (brush.vertices.size() < 4) {
        brush.vertices.clear();
        return brush;
    }

    constexpr double PlaneTolerance = 0.12;
    std::vector<std::set<std::size_t>> memberships(brush.vertices.size());
    for (std::size_t vertexIndex = 0; vertexIndex < brush.vertices.size(); ++vertexIndex) {
        for (std::size_t planeIndex = 0; planeIndex < planes.size(); ++planeIndex) {
            if (std::abs(dot(planes[planeIndex].normal, brush.vertices[vertexIndex]) -
                         planes[planeIndex].distance) <= PlaneTolerance) {
                memberships[vertexIndex].insert(planeIndex);
            }
        }
    }

    for (std::size_t a = 0; a < brush.vertices.size(); ++a) {
        for (std::size_t b = a + 1; b < brush.vertices.size(); ++b) {
            std::size_t sharedPlanes = 0;
            for (std::size_t plane : memberships[a]) {
                if (memberships[b].contains(plane)) ++sharedPlanes;
            }
            if (sharedPlanes >= 2) brush.edges.emplace_back(a, b);
        }
    }

    for (const Plane& plane : planes) {
        FaceGeometry face;
        face.normal = plane.normal;
        face.vertices = orderedFaceVertices(brush.vertices, plane);
        if (face.vertices.size() < 3 || !plane.side) continue;
        face.sideId = parseInt(plane.side->value("id"));
        if (const std::string* material = plane.side->value("material")) face.material = *material;
        parseTextureAxis(plane.side->value("uaxis"), face.uAxis);
        parseTextureAxis(plane.side->value("vaxis"), face.vAxis);
        // CMapFace::texture.nLightmapScale, used by the 3D lightmap grid view.
        if (const int parsed = parseInt(plane.side->value("lightmapscale")); parsed > 0) {
            face.lightmapScale = parsed;
        }
        buildDisplacement(face, brush.vertices, *plane.side);
        brush.hasDisplacement = brush.hasDisplacement || face.displacement;
        brush.faces.push_back(std::move(face));
    }
    return brush;
}

void expandBounds(Scene& scene, const Vec3& point)
{
    if (!scene.hasBounds) {
        scene.minimum = scene.maximum = point;
        scene.hasBounds = true;
        return;
    }
    scene.minimum.x = std::min(scene.minimum.x, point.x);
    scene.minimum.y = std::min(scene.minimum.y, point.y);
    scene.minimum.z = std::min(scene.minimum.z, point.z);
    scene.maximum.x = std::max(scene.maximum.x, point.x);
    scene.maximum.y = std::max(scene.maximum.y, point.y);
    scene.maximum.z = std::max(scene.maximum.z, point.z);
}

void collectSolids(const Block& owner, Scene& scene, int ownerEntityId = -1,
                   const std::unordered_set<int>* solidFilter = nullptr)
{
    for (const Block* solid : owner.children("solid")) {
        if (solidFilter && !solidFilter->contains(parseInt(solid->value("id")))) continue;
        BrushGeometry brush = buildBrush(*solid, ownerEntityId);
        if (!brush.vertices.empty()) {
            for (const Vec3& vertex : brush.vertices) expandBounds(scene, vertex);
            for (const FaceGeometry& face : brush.faces) {
                for (const DisplacementVertex& vertex : face.displacementVertices) {
                    expandBounds(scene, vertex.position);
                }
            }
            scene.brushes.push_back(std::move(brush));
        }
    }
}

// The point-entity marker buildScene creates for one entity block, or nothing
// when the entity has no usable origin.
std::optional<EntityMarker> buildEntityMarker(const Block& root)
{
    Vec3 origin;
    if (parseVec3(root.value("origin"), origin)) {
        EntityMarker marker;
        marker.id = parseInt(root.value("id"));
        marker.object = {ObjectType::Entity, marker.id};
        if (const std::string* classname = root.value("classname")) marker.classname = *classname;
        if (const std::string* targetName = root.value("targetname")) marker.targetName = *targetName;
        marker.origin = origin;
        if (const std::string* angles = root.value("angles"))
        parseVec3(angles, marker.angles);
        if (const std::string* skin = root.value("skin"))
        marker.skin = parseInt(skin);
        // CMapStudioModel keeps pitch separate from m_Angles and applies it
        // only at render time. Do not fold it into the stored QAngle.
        if (const std::string* pitch = root.value("pitch")) {
        try {
            marker.pitchOverride = std::stod(*pitch);
            marker.hasPitchOverride = true;
        } catch (...) {}
        }
        for (const Entry& entry : root.entries) {
        if (entry.kind == Entry::Kind::KeyValue) {
            marker.properties.emplace_back(entry.key, entry.value);
        } else if (entry.kind == Entry::Kind::ChildBlock && entry.child &&
                   (entry.child->name == "overlaydata" || entry.child->name == "OVERLAYDATA")) {
            for (const Entry& overlayEntry : entry.child->entries) {
                if (overlayEntry.kind == Entry::Kind::KeyValue)
                    marker.overlayProperties.emplace_back(overlayEntry.key, overlayEntry.value);
            }
        }
        }

        auto propertyCi = [&](std::string_view wanted) -> const std::string* {
        for (const auto& [key, value] : marker.properties) {
            if (key.size() != wanted.size()) continue;
            bool same = true;
            for (std::size_t index = 0; index < key.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(key[index])) !=
                    std::tolower(static_cast<unsigned char>(wanted[index]))) {
                    same = false;
                    break;
                }
            }
            if (same) return &value;
        }
        return nullptr;
        };
        const std::string* authoredSequence = propertyCi("DefaultAnim");
        if (!authoredSequence || authoredSequence->empty()) authoredSequence = propertyCi("sequence");
        if (authoredSequence && !authoredSequence->empty()) {
        marker.animationSequence = *authoredSequence;
        marker.animationSequenceIndex = parseInt(authoredSequence);
        }
        if (const std::string* rate = propertyCi("playbackrate"))
        marker.animationPlaybackRate = std::clamp(parseDouble(rate, 1.0), -16.0, 16.0);
        else if (const std::string* rate = propertyCi("animationrate"))
        marker.animationPlaybackRate = std::clamp(parseDouble(rate, 1.0), -16.0, 16.0);
        if (const std::string* cycle = propertyCi("cycle"))
        marker.animationCycle = std::clamp(parseDouble(cycle, 0.0), 0.0, 1.0);

        std::string classLower = marker.classname;
        std::transform(classLower.begin(), classLower.end(), classLower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
        });
        // prop_dynamic does not implicitly mean "play sequence zero". Source
        // only starts its authored DefaultAnim (or a sequence selected by an
        // input). Cycler-style helpers are the exception: their purpose in an
        // editor view is to continuously preview the model's first sequence.
        marker.animateModel = (authoredSequence && !authoredSequence->empty()) ||
        classLower == "cycler" || classLower == "cycler_actor" ||
        classLower == "generic_actor";

        return marker;
    }
    return std::nullopt;
}

} // namespace


std::string normalizeMaterialPath(std::string_view material)
{
    std::string normalized(material);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    while (!normalized.empty() && normalized.front() == '/') normalized.erase(normalized.begin());
    if (normalized.rfind("materials/", 0) == 0) normalized.erase(0, 10);
    if (normalized.size() >= 4) {
        const std::string_view suffix(normalized.data() + normalized.size() - 4, 4);
        if (suffix == ".vmt" || suffix == ".vtf") normalized.resize(normalized.size() - 4);
    }
    return normalized;
}

bool isToolMaterialPath(std::string_view material)
{
    return normalizeMaterialPath(material).rfind("tools/", 0) == 0;
}

std::vector<std::string> toolMaterialPaths(const Scene& scene)
{
    std::set<std::string> unique;
    for (const BrushGeometry& brush : scene.brushes) {
        for (const FaceGeometry& face : brush.faces) {
            const std::string normalized = normalizeMaterialPath(face.material);
            if (normalized.rfind("tools/", 0) == 0) unique.insert(normalized);
        }
    }
    return {unique.begin(), unique.end()};
}

bool isMaterialHiddenByToolTextures(std::string_view material,
                                    const std::unordered_set<std::string>& hiddenToolTextures)
{
    if (hiddenToolTextures.empty()) return false;
    const std::string normalized = normalizeMaterialPath(material);
    if (normalized.rfind("tools/", 0) != 0) return false;
    return hiddenToolTextures.find(normalized) != hiddenToolTextures.end();
}

bool isFaceHiddenByToolTextures(const FaceGeometry& face,
                                const std::unordered_set<std::string>& hiddenToolTextures)
{
    return isMaterialHiddenByToolTextures(face.material, hiddenToolTextures);
}

bool isBrushHiddenByToolTextures(const BrushGeometry& brush,
                                 const std::unordered_set<std::string>& hiddenToolTextures)
{
    if (hiddenToolTextures.empty() || brush.faces.empty()) return false;
    for (const FaceGeometry& face : brush.faces) {
        if (!isFaceHiddenByToolTextures(face, hiddenToolTextures)) return false;
    }
    return true;
}

BillboardSize billboardSpriteSize(const EntityMarker& entity, int imageWidth, int imageHeight)
{
    const double boundsWidth = std::max(entity.sizeMaximum.x - entity.sizeMinimum.x,
                                        entity.sizeMaximum.y - entity.sizeMinimum.y);
    const double boundsHeight = entity.sizeMaximum.z - entity.sizeMinimum.z;
    const double aspect = static_cast<double>(std::max(1, imageWidth)) /
                          static_cast<double>(std::max(1, imageHeight));
    BillboardSize size;
    size.height = std::max(16.0, std::abs(boundsHeight));
    size.width = std::max(16.0, std::abs(boundsWidth));
    if (size.width <= 16.0 && size.height > 16.0) {
        size.width = size.height * aspect;
    } else if (size.height <= 16.0 && size.width > 16.0) {
        size.height = size.width / std::max(0.01, aspect);
    } else if (std::abs(boundsWidth) <= 16.0 && std::abs(boundsHeight) <= 16.0) {
        size.height = 24.0;
        size.width = size.height * aspect;
    }
    return size;
}

std::uint64_t nextSceneRevision()
{
    static std::atomic<std::uint64_t> counter{0};
    // 0 is reserved for "no lineage", so the first scene gets 1.
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

void Scene::invalidateLineage()
{
    revision = nextSceneRevision();
    baseRevision = 0;
    changedSolidIds.clear();
    changedEntityIds.clear();
    lineageSteps.clear();
}

Scene buildScene(const Document& document)
{
    auto lowerCopy = [](std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    };
    Scene scene;
    for (const Block& root : document.roots()) {
        if (root.name == "world" || root.name == "WORLD") {
            if (const std::string* skyName = root.value("skyname")) scene.skyName = *skyName;
            // VBSP's FindDetailVBSPName / the detail sprite atlas. Both are
            // worldspawn keys a new Hammer map writes by default; the detail
            // prop emitter needs them to find its dictionary and its material.
            if (const std::string* detailVbsp = root.value("detailvbsp"))
                scene.detailVbspName = *detailVbsp;
            if (const std::string* detailMaterial = root.value("detailmaterial"))
                scene.detailMaterial = *detailMaterial;
            collectSolids(root, scene);
            continue;
        }
        if (lowerCopy(root.name) != "entity") continue;

        const int entityId = parseInt(root.value("id"));
        collectSolids(root, scene, entityId);
        if (auto marker = buildEntityMarker(root)) {
            const Vec3 origin = marker->origin;
            scene.entities.push_back(std::move(*marker));
            expandBounds(scene, origin);
        }
    }
    // Parse Source post-processing entities independently of point-entity
    // marker creation. logic_auto and brush color-correction volumes often have
    // no usable origin, but they still affect the in-game view.
    auto boolValue = [&](const std::string* value, bool fallback) {
        if (!value) return fallback;
        const std::string lowered = lowerCopy(*value);
        if (lowered == "1" || lowered == "true" || lowered == "yes") return true;
        if (lowered == "0" || lowered == "false" || lowered == "no") return false;
        return fallback;
    };

    std::unordered_set<std::string> tonemapTargets;
    for (const Block& root : document.roots()) {
        if (lowerCopy(root.name) != "entity") continue;
        const std::string className = lowerCopy(root.value("classname") ? *root.value("classname") : std::string{});
        if (className == "env_tonemap_controller") {
            if (const std::string* target = root.value("targetname"); target && !target->empty())
                tonemapTargets.insert(lowerCopy(*target));
        }

        if (className != "color_correction" && className != "color_correction_volume") continue;
        const std::string* filename = root.value("filename");
        if (!filename || filename->empty()) continue;

        ColorCorrectionDefinition correction;
        correction.filename = *filename;
        correction.weight = std::clamp(parseDouble(root.value("maxweight"), 1.0), 0.0, 1.0);
        correction.enabled = boolValue(root.value("enabled"), true) &&
                             !boolValue(root.value("StartDisabled"), false);
        correction.volume = className == "color_correction_volume";
        if (!correction.volume) {
            if (!parseVec3(root.value("origin"), correction.origin)) continue;
            correction.minFalloff = parseDouble(root.value("minfalloff"), 0.0);
            correction.maxFalloff = parseDouble(root.value("maxfalloff"), 1000.0);
        } else {
            const int id = parseInt(root.value("id"));
            bool haveBounds = false;
            for (const BrushGeometry& brush : scene.brushes) {
                if (brush.ownerEntityId != id) continue;
                for (const Vec3& vertex : brush.vertices) {
                    if (!haveBounds) {
                        correction.minimum = correction.maximum = vertex;
                        haveBounds = true;
                    } else {
                        correction.minimum.x = std::min(correction.minimum.x, vertex.x);
                        correction.minimum.y = std::min(correction.minimum.y, vertex.y);
                        correction.minimum.z = std::min(correction.minimum.z, vertex.z);
                        correction.maximum.x = std::max(correction.maximum.x, vertex.x);
                        correction.maximum.y = std::max(correction.maximum.y, vertex.y);
                        correction.maximum.z = std::max(correction.maximum.z, vertex.z);
                    }
                }
            }
            if (!haveBounds) continue;
        }
        scene.colorCorrections.push_back(std::move(correction));
    }

    // Hammer serializes outputs as target,input,param,delay,times. Apply the
    // zero-delay OnMapSpawn tone-map inputs so the preview starts with the same
    // map-authored controller state as Source. Delayed/runtime logic remains a
    // deliberate editor-preview limitation.
    for (const Block& root : document.roots()) {
        if (lowerCopy(root.name) != "entity") continue;
        const std::string className = lowerCopy(root.value("classname") ? *root.value("classname") : std::string{});
        if (className != "logic_auto") continue;
        for (const Block* connections : root.children("connections")) {
            for (const Entry& entry : connections->entries) {
                if (entry.kind != Entry::Kind::KeyValue || lowerCopy(entry.key) != "onmapspawn") continue;
                std::array<std::string, 5> fields{};
                const char separator = entry.value.find('\x1b') != std::string::npos ? '\x1b' : ',';
                std::size_t begin = 0;
                for (std::size_t field = 0; field < fields.size(); ++field) {
                    const std::size_t split = entry.value.find(separator, begin);
                    if (split == std::string::npos) {
                        fields[field] = entry.value.substr(begin);
                        break;
                    }
                    fields[field] = entry.value.substr(begin, split - begin);
                    begin = split + 1;
                }
                if (!tonemapTargets.contains(lowerCopy(fields[0]))) continue;
                if (parseDouble(&fields[3], 0.0) > 0.0001) continue;
                const std::string input = lowerCopy(fields[1]);
                const double value = parseDouble(&fields[2], 0.0);
                if (input == "settonemapscale") {
                    scene.toneMap.scale = std::clamp(value, 0.0, 16.0);
                } else if (input == "blendtonemapscale") {
                    // Input syntax is "targetScale blendSeconds". For a static
                    // editor preview, use the blend destination as the startup state.
                    scene.toneMap.scale = std::clamp(value, 0.0, 16.0);
                } else if (input == "settonemaprate") {
                    scene.toneMap.rate = std::max(0.0, value);
                } else if (input == "setautoexposuremin") {
                    scene.toneMap.autoExposureMin = std::max(0.0, value);
                    scene.toneMap.customAutoExposure = true;
                } else if (input == "setautoexposuremax") {
                    scene.toneMap.autoExposureMax = std::max(0.0, value);
                    scene.toneMap.customAutoExposure = true;
                } else if (input == "usedefaultautoexposure") {
                    scene.toneMap.autoExposureMin = 0.5;
                    scene.toneMap.autoExposureMax = 2.0;
                    scene.toneMap.customAutoExposure = false;
                } else if (input == "setbloomscale" || input == "setbloomscalerange") {
                    scene.toneMap.bloomScale = std::max(0.0, value);
                    scene.toneMap.customBloomScale = true;
                } else if (input == "usedefaultbloomscale") {
                    scene.toneMap.bloomScale = 1.0;
                    scene.toneMap.customBloomScale = false;
                }
            }
        }
    }
    if (scene.toneMap.autoExposureMin > scene.toneMap.autoExposureMax)
        std::swap(scene.toneMap.autoExposureMin, scene.toneMap.autoExposureMax);

    scene.invalidateLineage();
    return scene;
}

BrushGeometry buildSolidGeometry(const Block& solid, int ownerEntityId)
{
    return buildBrush(solid, ownerEntityId);
}

void rebuildSceneObjectsInPlace(const Document& document, Scene& scene,
                                const std::unordered_set<int>& changedSolidIds,
                                const std::unordered_set<int>& changedEntityIds)
{
    // Everything the incremental path cannot guarantee falls back to this,
    // which also re-stamps the lineage as "no reusable predecessor".
    const auto fallBackToFullBuild = [&] { scene = buildScene(document); };
    const std::uint64_t baseRevision = scene.revision;
    const auto lowerCopy = [](std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    };
    const auto isEntityRoot = [&](const Block& root) {
        return lowerCopy(root.name) == "entity";
    };

    // The scene-wide post-processing state and the color-correction volume
    // bounds are derived from whole-document scans, so any edit that can touch
    // them takes the full path.
    for (const Block& root : document.roots()) {
        if (!isEntityRoot(root)) continue;
        const int entityId = parseInt(root.value("id"));
        bool touched = changedEntityIds.contains(entityId);
        if (!touched) {
            for (const Block* solid : root.children("solid")) {
                if (changedSolidIds.contains(parseInt(solid->value("id")))) {
                    touched = true;
                    break;
                }
            }
        }
        if (!touched) continue;
        const std::string className = lowerCopy(root.value("classname") ? *root.value("classname")
                                                                        : std::string{});
        if (className == "color_correction" || className == "color_correction_volume" ||
            className == "env_tonemap_controller" || className == "logic_auto") {
            fallBackToFullBuild();
            return;
        }
    }

    std::unordered_map<int, std::size_t> brushIndex;
    brushIndex.reserve(scene.brushes.size());
    for (std::size_t index = 0; index < scene.brushes.size(); ++index)
        brushIndex.emplace(scene.brushes[index].id, index);
    std::unordered_map<int, std::size_t> entityIndex;
    entityIndex.reserve(scene.entities.size());
    for (std::size_t index = 0; index < scene.entities.size(); ++index)
        entityIndex.emplace(scene.entities[index].id, index);

    std::size_t rebuiltSolids = 0;
    std::size_t rebuiltEntities = 0;
    const auto rebuildSolids = [&](const Block& owner, int ownerEntityId) {
        for (const Block* solid : owner.children("solid")) {
            const int id = parseInt(solid->value("id"));
            if (!changedSolidIds.contains(id)) continue;
            const auto slot = brushIndex.find(id);
            if (slot == brushIndex.end()) return false;
            BrushGeometry brush = buildSolidGeometry(*solid, ownerEntityId);
            // buildScene drops solids that no longer form a body; falling back
            // keeps the brush list identical to a full rebuild.
            if (brush.vertices.empty()) return false;
            scene.brushes[slot->second] = std::move(brush);
            ++rebuiltSolids;
        }
        return true;
    };

    for (const Block& root : document.roots()) {
        if (root.name == "world" || root.name == "WORLD") {
            if (!rebuildSolids(root, -1)) { fallBackToFullBuild(); return; }
            continue;
        }
        if (!isEntityRoot(root)) continue;
        const int entityId = parseInt(root.value("id"));
        if (!rebuildSolids(root, entityId)) { fallBackToFullBuild(); return; }
        if (!changedEntityIds.contains(entityId)) continue;
        const auto slot = entityIndex.find(entityId);
        auto marker = buildEntityMarker(root);
        // An entity that gained or lost its origin key changes the marker list.
        // A brush entity without an origin never had a marker; its solids were
        // rebuilt above and there is nothing else to refresh, so it must not
        // push the whole drag onto the full-rebuild path.
        if (marker.has_value() != (slot != entityIndex.end())) { fallBackToFullBuild(); return; }
        if (marker) scene.entities[slot->second] = std::move(*marker);
        ++rebuiltEntities;
    }
    if (rebuiltSolids != changedSolidIds.size() || rebuiltEntities != changedEntityIds.size()) {
        fallBackToFullBuild();
        return;
    }

    // Bounds are a whole-scene reduction, but only over geometry that already
    // exists - no plane intersection, no displacement tessellation.
    scene.hasBounds = false;
    for (const BrushGeometry& brush : scene.brushes) {
        for (const Vec3& vertex : brush.vertices) expandBounds(scene, vertex);
        for (const FaceGeometry& face : brush.faces) {
            for (const DisplacementVertex& vertex : face.displacementVertices)
                expandBounds(scene, vertex.position);
        }
    }
    for (const EntityMarker& marker : scene.entities) expandBounds(scene, marker.origin);

    // Record the delta so render backends can keep every buffer whose objects
    // did not move and re-upload only these ids.
    scene.revision = nextSceneRevision();
    scene.baseRevision = baseRevision;
    scene.changedSolidIds.assign(changedSolidIds.begin(), changedSolidIds.end());
    scene.changedEntityIds.assign(changedEntityIds.begin(), changedEntityIds.end());
    scene.lineageSteps.push_back({baseRevision, scene.changedSolidIds, scene.changedEntityIds});
    // Bounded: a cache more than this many paints behind does a full rebuild,
    // which is the pre-history behavior.
    constexpr std::size_t MaxLineageSteps = 32;
    if (scene.lineageSteps.size() > MaxLineageSteps) {
        scene.lineageSteps.erase(scene.lineageSteps.begin(),
                                 scene.lineageSteps.begin() +
                                     static_cast<std::ptrdiff_t>(scene.lineageSteps.size() -
                                                                 MaxLineageSteps));
    }
}

Scene rebuildSceneObjects(const Document& document, const Scene& previous,
                          const std::unordered_set<int>& changedSolidIds,
                          const std::unordered_set<int>& changedEntityIds)
{
    Scene scene = previous;
    rebuildSceneObjectsInPlace(document, scene, changedSolidIds, changedEntityIds);
    return scene;
}

Scene buildSceneForSolids(const Document& document, const std::unordered_set<int>& solidIds)
{
    Scene scene;
    scene.invalidateLineage();
    if (solidIds.empty()) return scene;
    for (const Block& root : document.roots()) {
        if (root.name == "world" || root.name == "WORLD") {
            collectSolids(root, scene, -1, &solidIds);
            continue;
        }
        std::string name = root.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (name != "entity") continue;
        collectSolids(root, scene, parseInt(root.value("id")), &solidIds);
    }
    return scene;
}


bool isFaceMaskedByDisplacementSolid(const BrushGeometry& brush, const FaceGeometry& face,
                                     bool maskEnabled)
{
    // Exactly CMapSolid::Render3D's test: the mask only applies to solids that
    // actually have a displacement, and then only hides the sides that do not.
    return maskEnabled && brush.hasDisplacement && !face.displacement;
}

} // namespace hammer::vmf
