#pragma once

#include "VmfScene.hpp"

#include <string>
#include <vector>

namespace hammer::vmf {

// --- move_rope / keyframe_rope ----------------------------------------------
//
// A rope is a chain of point entities linked by the "NextKey" key, which names
// the targetname of the next keyframe. Each LINK (one entity to the one its
// NextKey points at) is an independent strand: Hammer previews it by running
// CPositionInterpolator_Rope (public/keyframe/keyframe.cpp), which drops a
// CRopePhysics<10> between the two endpoints and lets it settle under gravity.
// This port reproduces that simulation exactly - same node count, spring
// length, Verlet integrator, damping and iteration counts - so a strand sags
// the way the original editor draws it.
//
// The result is backend-neutral: a polyline plus the render keys the two
// viewports need to expand it into a camera-facing ribbon.
struct RopeStrand
{
    // The entity this strand starts at, and the one NextKey resolved to.
    // Backends key caches and picking off these.
    int startEntityId{-1};
    int endEntityId{-1};
    std::string material;
    // "Width (1-64)", in world units. Hammer's FGD default is 2.
    double width{2.0};
    // "TextureScale". 1 means the authored 4 texels per world unit.
    double textureScale{1.0};
    // Settled node positions, subdivided by the "Subdiv" key. Always at least
    // two points; consecutive points form the ribbon's segments.
    std::vector<Vec3> points;
};

// Every rope strand in the scene, in entity order. Entities whose classname is
// neither move_rope nor keyframe_rope are ignored, as are keyframes with no
// NextKey, a NextKey that names nothing, or a NextKey that points at
// themselves. Chains that loop back on themselves are fine: each link is built
// once, from the entity that owns it, so a cycle produces a closed loop of
// strands rather than an infinite walk.
//
// This is not cheap - each strand runs a five-second physics settle - so
// backends cache the result against Scene::revision rather than calling it per
// frame.
std::vector<RopeStrand> buildRopeStrands(const Scene& scene);

// The V texture coordinate one world unit along a strand advances, for a rope
// material whose base texture is "textureHeight" texels tall. Source authors
// rope materials at 4 texels per world unit and divides that by TextureScale
// (the wiki's "default resolution is 4 pixels per inch"), so a larger scale
// stretches the texture along the rope. Shared so both backends parameterise
// a strand identically.
double ropeTextureVPerUnit(const RopeStrand& strand, int textureHeight);

} // namespace hammer::vmf
