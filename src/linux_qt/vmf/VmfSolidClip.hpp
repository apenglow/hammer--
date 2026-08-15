#pragma once

// Plane-vs-convex-solid clipping, the geometry half of Hammer's Clipping tool
// (hammer/ToolClipper.cpp, hammer/mapsolid.cpp CMapSolid::Split).
//
// A VMF solid is stored as a set of half spaces, one per "side" chunk, whose
// plane normals point OUT of the solid. Splitting such a solid by a plane is
// therefore just "add one more half space and rebuild": CMapSolid::Split copies
// the solid, adds a face whose plane is the clip plane (negated for the front
// piece), and calls CreateFromPlanes, which discards the planes that no longer
// contribute a face. The same construction is used here so the two pieces stay
// exact VMF solids rather than triangulated soup.

#include "VmfDocument.hpp"
#include "VmfScene.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hammer::vmf {

// PLANE from hammer/mapdefs.h: dot( normal, point ) == distance on the plane,
// positive values are the FRONT side.
struct ClipPlane
{
    Vec3 normal{};
    double distance{0.0};

    bool valid() const;
};

// Clipper3D::BuildClipPlane. The two clip points are dragged in a 2D view and
// the plane is extruded along that view's third ("up") axis:
//     forward = up x (p1 - p0);  dist = dot( p0, forward )
ClipPlane clipPlaneFromLine(const Vec3& first, const Vec3& second, const Vec3& viewAxis);

// One closed loop of world-space points per side of a solid, in the winding
// CMapFace stores its points in. Used for the 2D wireframe preview.
using FacePolygons = std::vector<std::vector<Vec3>>;

FacePolygons solidFacePolygons(const Block& solid);

// The "plane" keyvalue text of a VMF side: three points of the face loop,
// written the way Hammer writes them so CMapFace::CalcPlane reads back
// (p0 - p1) x (p2 - p1) as the outward normal. Shared with the Morph tool,
// which re-derives plane text after dragging vertices.
std::string planePointsText(const Vec3& a, const Vec3& b, const Vec3& c);

// CMapSolid::Split's front/back point counts: Front and Back mean the solid
// lies wholly on that side of the plane, Split means the plane cuts it.
enum class SolidPlaneRelation { Front, Back, Split };

SolidPlaneRelation classifySolid(const Block& solid, const ClipPlane& plane);

// Every side's half-space plane (outward normal), parsed the way CMapFace::
// CalcPlane does. Sides whose plane text fails to parse are skipped. Used by
// the Carve tool, which cuts targets with the carver's planes one at a time.
std::vector<ClipPlane> solidClipPlanes(const Block& solid);

// True when any side of this solid carries a dispinfo chunk. Displacement
// solids are never clipped (see EditorModel::clipSelection).
bool solidHasDisplacement(const Block& solid);

// Returns the requested half of the solid as a new "solid" block, or nullopt
// when nothing survives on that side. Every side of the returned solid, plus
// the solid itself, receives a freshly allocated id. The cap face inherits its
// material from side 0 of the original, as CMapSolid::Split does
// (face.SetTexture( GetFace(0)->GetTexture() )), and gets world-aligned
// texture axes like CMapFace::InitializeTextureAxes( TEXTURE_ALIGN_WORLD ).
std::optional<Block> clipSolid(const Block& solid, const ClipPlane& plane, bool keepFront,
                               const std::function<int()>& allocateId);

} // namespace hammer::vmf
