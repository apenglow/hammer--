#include "VmfRope.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace hammer::vmf {

namespace {

// --- rope_shared.h ----------------------------------------------------------
constexpr int kRopeMaxSegments = 10;      // ROPE_MAX_SEGMENTS
constexpr int kRopeType1Segments = 4;     // ROPE_TYPE1_NUMSEGMENTS
constexpr int kRopeType2Segments = 2;     // ROPE_TYPE2_NUMSEGMENTS
// ROPESLACK_FUDGEFACTOR. Added to every authored slack so a slack of zero does
// not dangle.
constexpr double kRopeSlackFudge = -100.0;
// CRopeDelegate::GetNodeForces - the only force the editor's preview applies.
constexpr double kRopeGravity = -1500.0;

std::string toLower(std::string_view text)
{
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

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

const std::string* property(const EntityMarker& entity, std::string_view wanted)
{
    for (const auto& [key, value] : entity.properties) {
        if (equalsCi(key, wanted)) return &value;
    }
    return nullptr;
}

double propertyDouble(const EntityMarker& entity, std::string_view wanted, double fallback)
{
    const std::string* value = property(entity, wanted);
    if (!value || value->empty()) return fallback;
    // The originals read these keys with atof/atoi, which stop at the first
    // character they cannot use and never throw. Match that.
    return std::atof(value->c_str());
}

int propertyInt(const EntityMarker& entity, std::string_view wanted, int fallback)
{
    const std::string* value = property(entity, wanted);
    if (!value || value->empty()) return fallback;
    return std::atoi(value->c_str());
}

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

double lengthSquared(const Vec3& value)
{
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

double length(const Vec3& value)
{
    return std::sqrt(lengthSquared(value));
}

Vec3 lerp(const Vec3& from, const Vec3& to, double time)
{
    return add(from, scale(subtract(to, from), time));
}

// public/mathlib Catmull_Rom_Spline, used to smooth the settled nodes into the
// "Subdiv" subdivisions the engine renders a rope with.
Vec3 catmullRom(const Vec3& p1, const Vec3& p2, const Vec3& p3, const Vec3& p4, double t)
{
    const double tSqr = t * t;
    const double tCube = tSqr * t;
    const double b1 = 0.5 * (-tCube + 2.0 * tSqr - t);
    const double b2 = 0.5 * (3.0 * tCube - 5.0 * tSqr + 2.0);
    const double b3 = 0.5 * (-3.0 * tCube + 4.0 * tSqr + t);
    const double b4 = 0.5 * (tCube - tSqr);
    Vec3 result = scale(p1, b1);
    result = add(result, scale(p2, b2));
    result = add(result, scale(p3, b3));
    return add(result, scale(p4, b4));
}

// CSimplePhysics::CNode.
struct RopeNode
{
    Vec3 position{};
    Vec3 previousPosition{};
};

// CPositionInterpolator_Rope::InterpolatePosition, which is
// CRopePhysics<10> driven by CRopeDelegate:
//
//   * every node accelerates by ROPE_GRAVITY,
//   * CBaseRopePhysics::ApplyConstraints runs three spring iterations per step,
//     each followed by the delegate pinning both endpoints,
//   * CSimplePhysics integrates with Verlet at a 1/50 timestep and 0.98 damping
//     (CBaseRopePhysics::Restart / Simulate),
//   * the rope is settled by simulating five seconds before it is drawn.
//
// The engine's runtime rope also carries wind and collision; the editor preview
// deliberately does not, so neither does this.
std::vector<Vec3> simulateRope(const Vec3& start, const Vec3& end, double slack, int nodeCount)
{
    nodeCount = std::clamp(nodeCount, 2, kRopeMaxSegments);
    std::vector<RopeNode> nodes(static_cast<std::size_t>(nodeCount));
    for (RopeNode& node : nodes) {
        node.position = start;
        node.previousPosition = start;
    }

    // CPositionInterpolator_Rope::InterpolatePosition: the spring length is the
    // endpoint distance plus the slack, spread over the segments.
    const double totalLength = length(subtract(start, end)) + slack;
    const double springDistance = std::max(totalLength / (nodeCount - 1), 0.0);
    const double springDistanceSquared = springDistance * springDistance;

    constexpr double kTimeStep = 1.0 / 50.0;                 // CBaseRopePhysics::Restart
    constexpr double kTimeStepMul = kTimeStep * kTimeStep * 0.5;
    constexpr double kDamping = 0.98;                        // flEnergy
    constexpr int kConstraintIterations = 3;                 // nIterations
    constexpr int kSettleSteps = static_cast<int>(5.0 / kTimeStep);

    for (int step = 0; step < kSettleSteps; ++step) {
        for (RopeNode& node : nodes) {
            const Vec3 previous = node.position;
            node.position = add(add(node.position,
                                    scale(subtract(node.position, node.previousPosition), kDamping)),
                                Vec3{0.0, 0.0, kRopeGravity * kTimeStepMul});
            node.previousPosition = previous;
        }

        for (int iteration = 0; iteration < kConstraintIterations; ++iteration) {
            for (std::size_t spring = 0; spring + 1 < nodes.size(); ++spring) {
                Vec3 toNode = subtract(nodes[spring].position, nodes[spring + 1].position);
                const double distanceSquared = lengthSquared(toNode);
                if (distanceSquared > springDistanceSquared) {
                    const double distance = std::sqrt(distanceSquared);
                    toNode = scale(toNode, 1.0 - springDistance / distance);
                    nodes[spring].position = subtract(nodes[spring].position, scale(toNode, 0.5));
                    nodes[spring + 1].position = add(nodes[spring + 1].position, scale(toNode, 0.5));
                }
            }
            // CRopeDelegate::ApplyConstraints - both ends stay pinned to the
            // keyframe entities, inside every iteration.
            nodes.front().position = start;
            nodes.back().position = end;
        }
    }

    // CSimplePhysics::Simulate leaves m_vPredicted at the end of the settled
    // step, which for a converged rope equals m_vPos.
    std::vector<Vec3> points;
    points.reserve(nodes.size());
    for (const RopeNode& node : nodes) points.push_back(node.position);
    return points;
}

// Smooth the settled nodes with "Subdiv" extra points per segment, the way the
// engine tessellates a rope for rendering. The endpoints stay exactly on the
// keyframe entities.
std::vector<Vec3> subdivide(const std::vector<Vec3>& nodes, int subdivisions)
{
    if (nodes.size() < 2 || subdivisions <= 0) return nodes;
    const auto at = [&](int index) {
        return nodes[static_cast<std::size_t>(
            std::clamp(index, 0, static_cast<int>(nodes.size()) - 1))];
    };
    std::vector<Vec3> points;
    points.reserve(nodes.size() * static_cast<std::size_t>(subdivisions + 1));
    for (int node = 0; node + 1 < static_cast<int>(nodes.size()); ++node) {
        points.push_back(at(node));
        for (int step = 1; step <= subdivisions; ++step) {
            const double t = static_cast<double>(step) / (subdivisions + 1);
            points.push_back(catmullRom(at(node - 1), at(node), at(node + 1), at(node + 2), t));
        }
    }
    points.push_back(nodes.back());
    return points;
}

bool isRopeClassname(const std::string& classname)
{
    const std::string lowered = toLower(classname);
    return lowered == "move_rope" || lowered == "keyframe_rope";
}

} // namespace

std::vector<RopeStrand> buildRopeStrands(const Scene& scene)
{
    std::vector<RopeStrand> strands;

    // Hammer resolves NextKey through the world's targetname map, which keeps
    // the first entity with a given name. Build the same map once.
    std::unordered_map<std::string, const EntityMarker*> byName;
    for (const EntityMarker& entity : scene.entities) {
        if (entity.targetName.empty()) continue;
        byName.emplace(toLower(entity.targetName), &entity);
    }

    for (const EntityMarker& entity : scene.entities) {
        if (!isRopeClassname(entity.classname)) continue;
        const std::string* nextKey = property(entity, "NextKey");
        if (!nextKey || nextKey->empty()) continue;
        const auto next = byName.find(toLower(*nextKey));
        if (next == byName.end()) continue;
        const EntityMarker& endEntity = *next->second;
        // A keyframe pointing at itself is a degenerate strand, not a rope.
        if (&endEntity == &entity) continue;

        RopeStrand strand;
        strand.startEntityId = entity.id;
        strand.endEntityId = endEntity.id;

        if (const std::string* material = property(entity, "RopeMaterial");
            material && !material->empty()) {
            strand.material = *material;
        } else {
            // The FGD default, and what Hammer falls back to when the key is
            // absent. (Source refuses to draw cable/chain.vmt in game because
            // its Cable shader has no transparency; that is a runtime
            // limitation, not an editor one.)
            strand.material = "cable/cable";
        }

        strand.width = std::clamp(propertyDouble(entity, "Width", 2.0), 0.05, 64.0);
        strand.textureScale = propertyDouble(entity, "TextureScale", 1.0);
        if (!(strand.textureScale > 0.0)) strand.textureScale = 1.0;

        // CPositionInterpolator_Rope::ProcessKey maps Type to a node count.
        int nodeCount = kRopeType2Segments;
        switch (propertyInt(entity, "Type", 0)) {
        case 0: nodeCount = kRopeMaxSegments; break;
        case 1: nodeCount = kRopeType1Segments; break;
        default: nodeCount = kRopeType2Segments; break;
        }
        // The interpolator's default node count when no Type key is present is
        // 5, not the Type 2 value it would map an unknown Type to.
        if (!property(entity, "Type")) nodeCount = 5;

        // ProcessKey only applies the fudge factor to an authored Slack; a rope
        // with no Slack key keeps the interpolator's m_flSlack default of 0.
        const std::string* slackKey = property(entity, "Slack");
        const double slack = slackKey ? std::atof(slackKey->c_str()) + kRopeSlackFudge : 0.0;
        const int subdivisions = std::clamp(propertyInt(entity, "Subdiv", 2), 0, 8);

        strand.points = subdivide(simulateRope(entity.origin, endEntity.origin, slack, nodeCount),
                                  subdivisions);
        if (strand.points.size() < 2) continue;
        strands.push_back(std::move(strand));
    }

    return strands;
}

double ropeTextureVPerUnit(const RopeStrand& strand, int textureHeight)
{
    const double texelsPerUnit = 4.0 / std::max(strand.textureScale, 1e-4);
    return texelsPerUnit / std::max(textureHeight, 1);
}

} // namespace hammer::vmf
