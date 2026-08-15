#pragma once

// Vertex manipulation, the geometry half of Hammer's Morph tool
// (hammer/ToolMorph.cpp, hammer/SSolid.cpp).
//
// Hammer converts every solid put into vertex-manipulation mode into a CSSolid:
// an explicit vertex / edge / face mesh whose handles the user drags around
// (CSSolid::Convert). When the morph ends, CSSolid::Convert(FALSE) turns the
// mesh back into a CMapSolid by re-deriving each face's plane from its (moved)
// points -- CMapFace::CalcPlane.
//
// The port keeps the same two-step shape: MorphSolid is the editable mesh, and
// morphSolid() converts it back into a VMF "solid" block by rewriting the plane
// text of every side whose polygon moved.

#include "VmfDocument.hpp"
#include "VmfScene.hpp"
#include "VmfSolidClip.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace hammer::vmf {

// One face of the mesh: the VMF side it came from and the loop of vertex
// indices, in the winding CMapFace stores its points in.
struct MorphFace
{
    int sideId{-1};
    // The face normal before any handle was dragged. Used to keep the
    // re-derived plane pointing the same way out of the solid.
    Vec3 normal{};
    std::vector<std::size_t> vertices;
};

// One displacement grid riding on a face of the mesh. Its vertices are morph
// handles too: moving one writes the delta into the dispinfo "offsets" rows.
struct MorphDispFace
{
    int sideId{-1};
    int power{0};
    std::vector<Vec3> positions;
    std::vector<Vec3> originalPositions;
};

// CSSolid: the vertex/edge/face mesh for one solid in morph mode.
struct MorphSolid
{
    int solidId{-1};
    // Current handle positions and the positions the morph started from.
    std::vector<Vec3> vertices;
    std::vector<Vec3> originalVertices;
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    std::vector<MorphFace> faces;
    std::vector<MorphDispFace> dispFaces;

    // CSSEdge::ptCenter, the yellow midpoint handle.
    Vec3 edgeCenter(std::size_t edge) const;
    bool moved() const;
};

// CSSolid::Convert: build the editable mesh from a solid's rendered geometry.
// buildScene has already welded the corner points and given every face its
// side id, so the mesh is just a copy of that topology.
MorphSolid buildMorphSolid(const BrushGeometry& brush);

std::vector<MorphSolid> buildMorphSolids(const Scene& scene, const std::vector<ObjectRef>& selection);
std::vector<MorphSolid> buildMorphSolidsById(const Scene& scene, const std::vector<int>& solidIds);

// Morph3D's handle display modes (ToolMorph.h hmVertex / hmEdge / hmBoth),
// cycled by pressing the tool's shortcut while it is already active.
enum class MorphHandleMode { VerticesAndEdges, Vertices, Edges };
MorphHandleMode nextMorphHandleMode(MorphHandleMode mode);

// One drawable handle: a white square at a vertex (CSSVertex) or a yellow one
// at an edge midpoint (CSSEdge::ptCenter), red when selected.
struct MorphHandle
{
    Vec3 position{};
    bool edge{false};
    bool selected{false};
    // True for a displacement grid vertex handle.
    bool displacement{false};
};

// Identifies a handle inside a set of morph solids, independently of the
// current display mode.
struct MorphHandleRef
{
    std::size_t solid{0};
    std::size_t index{0};
    bool edge{false};
    // >= 0: index is a grid vertex of MorphSolid::dispFaces[dispFace].
    int dispFace{-1};

    bool operator==(const MorphHandleRef&) const = default;
};

// The handles the given display mode shows, in a deterministic order; refs
// receives the matching identity of each entry.
std::vector<MorphHandle> morphHandles(const std::vector<MorphSolid>& solids, MorphHandleMode mode,
                                      const std::vector<MorphHandleRef>& selected,
                                      std::vector<MorphHandleRef>* refs = nullptr);

// The face loops of the meshes as they currently stand, for the 2D preview.
std::vector<FacePolygons> morphFacePolygons(const std::vector<MorphSolid>& solids);

// The displacement grids of the meshes, for drawing the grid edge wireframe
// while the tool is up.
struct MorphDispGrid
{
    int power{0};
    std::vector<Vec3> positions;
};
std::vector<MorphDispGrid> morphDispGrids(const std::vector<MorphSolid>& solids);

// CSSolid::MoveSelectedHandles: move the given vertices, plus both endpoints of
// every given edge, by delta. Vertex indices and edge indices are the ones of
// this mesh; duplicates are applied only once.
void moveMorphHandles(MorphSolid& solid, const std::vector<std::size_t>& vertexHandles,
                      const std::vector<std::size_t>& edgeHandles, const Vec3& delta);

// Moves the given grid vertices of one displacement face by delta. The delta
// is committed as a change to the dispinfo "offsets" rows by morphSolid().
void moveMorphDispHandles(MorphSolid& solid, std::size_t dispFace,
                          const std::vector<std::size_t>& vertexIndices, const Vec3& delta);

// CSSolid::Convert(FALSE): returns a copy of the solid block with the plane
// text of every moved face re-derived from its new points, or nullopt when the
// result would no longer be a solid (fewer than four contributing faces).
std::optional<Block> morphSolid(const Block& solid, const MorphSolid& morph);

} // namespace hammer::vmf
