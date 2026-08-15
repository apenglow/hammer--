#include "VmfSolidClip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace hammer::vmf {
namespace {

// Tolerances are the ones VmfScene.cpp's brush builder uses, so a clipped piece
// classifies its faces exactly the way buildScene will when it renders it.
constexpr double InsideTolerance = 0.08;
constexpr double MergeDistanceSquared = 0.01;
constexpr double PlaneTolerance = 0.12;
// CMapSolid::Split's SPLIT_DIST_EPSILON.
constexpr double SplitDistanceEpsilon = 0.001;

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 multiply(const Vec3& value, double scalar) { return {value.x * scalar, value.y * scalar, value.z * scalar}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double lengthSquared(const Vec3& value) { return dot(value, value); }
double distanceSquared(const Vec3& a, const Vec3& b) { return lengthSquared(subtract(a, b)); }

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        return a == b;
    });
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

std::string formatPlanePoints(const Vec3& a, const Vec3& b, const Vec3& c)
{
    const auto point = [](const Vec3& value) {
        return "(" + formatNumber(value.x) + " " + formatNumber(value.y) + " " + formatNumber(value.z) + ")";
    };
    return point(a) + " " + point(b) + " " + point(c);
}

struct PlaneRecord
{
    Vec3 normal{};
    double distance{0.0};
    const Block* side{nullptr};
    bool cap{false};
};

// CMapFace::CalcPlane: GetNormalFromPoints( p0, p1, p2 ) = (p0 - p1) x (p2 - p1),
// dist = dot( p0, normal ). Matches VmfScene.cpp's parsePlane exactly.
bool parsePlaneText(const std::string* text, PlaneRecord& plane)
{
    if (!text) return false;
    double values[9];
    if (!parseNumbers(*text, values, 9)) return false;
    const Vec3 a{values[0], values[1], values[2]};
    const Vec3 b{values[3], values[4], values[5]};
    const Vec3 c{values[6], values[7], values[8]};
    Vec3 normal = cross(subtract(a, b), subtract(c, b));
    const double magnitude = std::sqrt(lengthSquared(normal));
    if (magnitude < 1e-9) return false;
    normal = multiply(normal, 1.0 / magnitude);
    plane.normal = normal;
    plane.distance = dot(normal, a);
    return true;
}

std::vector<PlaneRecord> solidPlanes(const Block& solid)
{
    std::vector<PlaneRecord> planes;
    for (const Block* side : solid.children("side")) {
        PlaneRecord plane;
        if (!parsePlaneText(side->value("plane"), plane)) continue;
        plane.side = side;
        planes.push_back(plane);
    }
    return planes;
}

bool intersect(const PlaneRecord& a, const PlaneRecord& b, const PlaneRecord& c, Vec3& point)
{
    const Vec3 bCrossC = cross(b.normal, c.normal);
    const double denominator = dot(a.normal, bCrossC);
    if (std::abs(denominator) < 1e-8) return false;
    point = multiply(add(add(multiply(bCrossC, a.distance), multiply(cross(c.normal, a.normal), b.distance)),
                         multiply(cross(a.normal, b.normal), c.distance)),
                     1.0 / denominator);
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

// The corner points of the convex region bounded by the given outward-facing
// half spaces (VmfScene.cpp candidateVertices, with the outward-normal sign).
std::vector<Vec3> solidVertices(const std::vector<PlaneRecord>& planes)
{
    std::vector<Vec3> vertices;
    for (std::size_t i = 0; i < planes.size(); ++i) {
        for (std::size_t j = i + 1; j < planes.size(); ++j) {
            for (std::size_t k = j + 1; k < planes.size(); ++k) {
                Vec3 point;
                if (!intersect(planes[i], planes[j], planes[k], point)) continue;
                bool inside = true;
                for (const PlaneRecord& plane : planes) {
                    if (dot(plane.normal, point) - plane.distance > InsideTolerance) {
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

// VmfScene.cpp orderedFaceVertices: CMapFace stores points CLOCKWISE about the
// outward normal, so the angular sort descends.
std::vector<Vec3> facePolygon(const std::vector<Vec3>& vertices, const PlaneRecord& plane)
{
    std::vector<Vec3> points;
    Vec3 center{};
    for (const Vec3& vertex : vertices) {
        if (std::abs(dot(plane.normal, vertex) - plane.distance) <= PlaneTolerance) {
            points.push_back(vertex);
            center = add(center, vertex);
        }
    }
    if (points.size() < 3) return {};
    center = multiply(center, 1.0 / static_cast<double>(points.size()));

    const Vec3 reference = std::abs(plane.normal.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0};
    Vec3 tangent = cross(reference, plane.normal);
    const double tangentLength = std::sqrt(lengthSquared(tangent));
    if (tangentLength < 1e-9) return {};
    tangent = multiply(tangent, 1.0 / tangentLength);
    const Vec3 bitangent = cross(plane.normal, tangent);
    std::sort(points.begin(), points.end(), [&](const Vec3& a, const Vec3& b) {
        const Vec3 da = subtract(a, center);
        const Vec3 db = subtract(b, center);
        return std::atan2(dot(da, bitangent), dot(da, tangent)) >
               std::atan2(dot(db, bitangent), dot(db, tangent));
    });
    return points;
}

// mapface.cpp FaceNormals / DownVectors / RightVectors, used by
// CMapFace::GetOrientation and InitializeTextureAxes( TEXTURE_ALIGN_WORLD ).
const Vec3 FaceNormals[6] = {{0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
const Vec3 DownVectors[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
const Vec3 RightVectors[6] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 1, 0}};

int faceOrientation(const Vec3& normal)
{
    double best = 0.0;
    int orientation = 0;
    for (int i = 0; i < 6; ++i) {
        const double value = dot(normal, FaceNormals[i]);
        if (value >= best) {
            best = value;
            orientation = i;
        }
    }
    return orientation;
}

std::string formatAxis(const Vec3& axis, double scale)
{
    return "[" + formatNumber(axis.x) + " " + formatNumber(axis.y) + " " + formatNumber(axis.z) + " 0] " +
           formatNumber(scale);
}

const std::string* sideValue(const Block* side, std::string_view key)
{
    return side ? side->value(key) : nullptr;
}

// Three well separated points of a face loop, in loop order, so that
// CMapFace::CalcPlane's (p0 - p1) x (p2 - p1) reproduces the outward normal
// without being spoiled by three nearly collinear neighbours.
bool choosePlanePoints(const std::vector<Vec3>& points, const Vec3& normal,
                       Vec3& a, Vec3& b, Vec3& c)
{
    const std::size_t count = points.size();
    if (count < 3) return false;
    double bestMagnitude = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t j = (i + std::max<std::size_t>(1, count / 3)) % count;
        const std::size_t k = (i + std::max<std::size_t>(2, (2 * count) / 3)) % count;
        if (i == j || j == k || i == k) continue;
        const Vec3 candidate = cross(subtract(points[i], points[j]), subtract(points[k], points[j]));
        const double magnitude = std::sqrt(lengthSquared(candidate));
        if (magnitude > bestMagnitude && dot(candidate, normal) > 0.0) {
            bestMagnitude = magnitude;
            a = points[i];
            b = points[j];
            c = points[k];
        }
    }
    return bestMagnitude > 1e-4;
}

} // namespace

bool ClipPlane::valid() const
{
    return std::isfinite(distance) && lengthSquared(normal) > 0.5;
}

ClipPlane clipPlaneFromLine(const Vec3& first, const Vec3& second, const Vec3& viewAxis)
{
    // Clipper3D::BuildClipPlane
    const Vec3 right = subtract(second, first);
    Vec3 forward = cross(viewAxis, right);
    const double magnitude = std::sqrt(lengthSquared(forward));
    if (magnitude < 1e-6) return {};
    forward = multiply(forward, 1.0 / magnitude);
    return {forward, dot(first, forward)};
}

std::string planePointsText(const Vec3& a, const Vec3& b, const Vec3& c)
{
    return formatPlanePoints(a, b, c);
}

FacePolygons solidFacePolygons(const Block& solid)
{
    const std::vector<PlaneRecord> planes = solidPlanes(solid);
    if (planes.size() < 4) return {};
    const std::vector<Vec3> vertices = solidVertices(planes);
    if (vertices.size() < 4) return {};

    FacePolygons polygons;
    for (const PlaneRecord& plane : planes) {
        std::vector<Vec3> points = facePolygon(vertices, plane);
        if (points.size() >= 3) polygons.push_back(std::move(points));
    }
    return polygons;
}

SolidPlaneRelation classifySolid(const Block& solid, const ClipPlane& plane)
{
    // CMapSolid::Split counts face points on either side of the plane.
    int frontCount = 0;
    int backCount = 0;
    for (const std::vector<Vec3>& polygon : solidFacePolygons(solid)) {
        for (const Vec3& point : polygon) {
            const double distance = dot(point, plane.normal) - plane.distance;
            if (distance > SplitDistanceEpsilon) ++frontCount;
            else if (distance < -SplitDistanceEpsilon) ++backCount;
        }
    }
    if (frontCount == 0) return SolidPlaneRelation::Back;
    if (backCount == 0) return SolidPlaneRelation::Front;
    return SolidPlaneRelation::Split;
}

std::vector<ClipPlane> solidClipPlanes(const Block& solid)
{
    std::vector<ClipPlane> planes;
    for (const PlaneRecord& record : solidPlanes(solid)) {
        planes.push_back({record.normal, record.distance});
    }
    return planes;
}

bool solidHasDisplacement(const Block& solid)
{
    for (const Block* side : solid.children("side")) {
        if (!side->children("dispinfo").empty()) return true;
    }
    return false;
}

std::optional<Block> clipSolid(const Block& solid, const ClipPlane& plane, bool keepFront,
                               const std::function<int()>& allocateId)
{
    if (!plane.valid() || !allocateId) return std::nullopt;

    std::vector<PlaneRecord> planes = solidPlanes(solid);
    if (planes.size() < 4) return std::nullopt;

    // CMapSolid::Split: the front piece is bounded by the NEGATED clip plane,
    // the back piece by the clip plane itself, because a VMF side's normal
    // points out of the solid.
    PlaneRecord cap;
    cap.normal = keepFront ? multiply(plane.normal, -1.0) : plane.normal;
    cap.distance = keepFront ? -plane.distance : plane.distance;
    cap.cap = true;
    planes.push_back(cap);

    const std::vector<Vec3> vertices = solidVertices(planes);
    if (vertices.size() < 4) return std::nullopt;

    Block result("solid");
    result.setValue("id", std::to_string(allocateId()));

    const Block* firstSide = solid.children("side").empty() ? nullptr : solid.children("side").front();

    int keptSides = 0;
    for (const PlaneRecord& record : planes) {
        const std::vector<Vec3> points = facePolygon(vertices, record);
        // CreateFromPlanes drops planes that no longer contribute a face.
        if (points.size() < 3) continue;

        Block& side = result.appendChild("side");
        side.setValue("id", std::to_string(allocateId()));
        if (!record.cap) {
            // Preserve every key of the original side (plane, material, axes,
            // lightmapscale, smoothing groups, ...) except its id. The half
            // space is unchanged by the clip, so the original plane points are
            // kept verbatim rather than re-derived from the new winding.
            for (const Entry& entry : record.side->entries) {
                if (entry.kind == Entry::Kind::KeyValue) {
                    if (equalsIgnoreCase(entry.key, "id")) continue;
                    side.setValue(entry.key, entry.value);
                }
            }
        } else {
            Vec3 a{};
            Vec3 b{};
            Vec3 c{};
            if (!choosePlanePoints(points, record.normal, a, b, c)) return std::nullopt;
            side.setValue("plane", formatPlanePoints(a, b, c));
            // The new cap face takes its texture from face zero of the solid.
            const std::string* material = sideValue(firstSide, "material");
            side.setValue("material", material ? *material : std::string("TOOLS/TOOLSNODRAW"));
            const int orientation = faceOrientation(record.normal);
            side.setValue("uaxis", formatAxis(RightVectors[orientation], 0.25));
            side.setValue("vaxis", formatAxis(DownVectors[orientation], 0.25));
            side.setValue("rotation", "0");
            const std::string* lightmap = sideValue(firstSide, "lightmapscale");
            side.setValue("lightmapscale", lightmap ? *lightmap : std::string("16"));
            side.setValue("smoothing_groups", "0");
        }
        ++keptSides;
    }

    if (keptSides < 4) return std::nullopt;

    // Carry over the solid's non-side children (the editor chunk) and its
    // remaining keyvalues, as CMapSolid::CopyFrom does.
    for (const Entry& entry : solid.entries) {
        if (entry.kind == Entry::Kind::KeyValue) {
            if (equalsIgnoreCase(entry.key, "id")) continue;
            result.setValue(entry.key, entry.value);
        } else if (entry.child && !equalsIgnoreCase(entry.child->name, "side")) {
            result.entries.emplace_back(*entry.child);
        }
    }
    return result;
}

} // namespace hammer::vmf
