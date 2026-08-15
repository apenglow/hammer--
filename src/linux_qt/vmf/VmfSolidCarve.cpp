#include "VmfSolidCarve.hpp"

#include <cmath>
#include <cstddef>
#include <utility>

namespace hammer::vmf {
namespace {

Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

} // namespace

double solidVolume(const Block& solid)
{
    // Divergence theorem over the fan-triangulated face loops. The loops wind
    // clockwise about the outward normal (CMapFace order), which makes the
    // signed sum negative for a well-formed solid; the magnitude is the volume
    // either way.
    double sum = 0.0;
    for (const std::vector<Vec3>& polygon : solidFacePolygons(solid)) {
        for (std::size_t i = 2; i < polygon.size(); ++i) {
            const Vec3 edgeA = subtract(polygon[i - 1], polygon[0]);
            const Vec3 edgeB = subtract(polygon[i], polygon[0]);
            sum += dot(polygon[0], cross(edgeA, edgeB));
        }
    }
    return std::abs(sum) / 6.0;
}

std::optional<std::vector<Block>> carveSolid(const Block& target, const Block& carver,
                                             const std::function<int()>& allocateId)
{
    if (!allocateId || solidHasDisplacement(target)) return std::nullopt;
    const std::vector<ClipPlane> planes = solidClipPlanes(carver);
    if (planes.size() < 4) return std::nullopt;

    // Overlap rejection: a target wholly in FRONT of any carver plane lies
    // outside the carver's convex volume. Face-touching neighbours land here
    // too (their shared-plane points are within SplitDistanceEpsilon, the
    // rest are in front), so adjacency never triggers a rebuild.
    std::vector<ClipPlane> candidates;
    for (const ClipPlane& plane : planes) {
        switch (classifySolid(target, plane)) {
        case SolidPlaneRelation::Front: return std::nullopt;
        case SolidPlaneRelation::Split: candidates.push_back(plane); break;
        case SolidPlaneRelation::Back: break;
        }
    }

    std::vector<Block> pieces;
    Block remainder = target;
    while (!candidates.empty()) {
        // Re-classify against the shrinking remainder: planes that no longer
        // cut it are dropped instead of producing degenerate clips, and a
        // remainder that has moved wholly outside the carver survives intact.
        std::vector<ClipPlane> cutting;
        bool outside = false;
        for (const ClipPlane& plane : candidates) {
            const SolidPlaneRelation relation = classifySolid(remainder, plane);
            if (relation == SolidPlaneRelation::Front) {
                outside = true;
                break;
            }
            if (relation == SolidPlaneRelation::Split) cutting.push_back(plane);
        }
        if (outside) {
            pieces.push_back(std::move(remainder));
            return pieces;
        }
        if (cutting.empty()) break;

        // Greedy order: cut off the biggest outside chunk first.
        std::size_t bestIndex = cutting.size();
        double bestVolume = -1.0;
        std::optional<Block> bestPiece;
        for (std::size_t index = 0; index < cutting.size(); ++index) {
            std::optional<Block> front = clipSolid(remainder, cutting[index], true, allocateId);
            if (!front) continue;
            const double volume = solidVolume(*front);
            if (volume > bestVolume) {
                bestVolume = volume;
                bestPiece = std::move(front);
                bestIndex = index;
            }
        }
        // Every Split plane failed to produce a front piece: the remainder is
        // degenerate against all of them (epsilon disagreement between
        // classifySolid and clipSolid). Treat it as the intersection.
        if (!bestPiece) break;

        std::optional<Block> back = clipSolid(remainder, cutting[bestIndex], false, allocateId);
        pieces.push_back(std::move(*bestPiece));
        if (!back) return pieces;
        remainder = std::move(*back);
        cutting.erase(cutting.begin() + static_cast<std::ptrdiff_t>(bestIndex));
        candidates = std::move(cutting);
    }
    // The remainder is the target∩carver intersection — the carved-away part.
    return pieces;
}

} // namespace hammer::vmf
