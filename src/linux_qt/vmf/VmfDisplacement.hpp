#pragma once

// Displacement editing, the second half of Hammer's Texture Application tool.
//
// Original sources:
//   hammer/faceedit_disppage.cpp - CFaceEditDispPage: the page's buttons and
//                                  their enable states (UpdateEditControls),
//                                  OnButtonCreate / OnButtonDestroy
//   hammer/mapdisp.cpp           - CMapDisp::InitDispSurfaceData, InitData,
//                                  PaintPosition_Update, Paint_Update
//   hammer/disppaint.cpp         - CDispPaintMgr: the spatial paint sphere
//                                  (DoPaintAdd / DoPaintOneOverR / DoPaintOne)
//   hammer/tooldisplace.cpp      - CToolDisplace: paint defaults and the paint
//                                  axis, GetSelectedDisps (the paint scope)
//   hammer/dispdlg.cpp           - CDispCreateDlg / CDispPaintDistDlg: the
//                                  power range and the distance/radius ranges
//
// Everything here operates on the VMF "dispinfo" chunk inside a side, which is
// what CMapDisp serializes to.

#include "VmfDocument.hpp"
#include "VmfFaceEdit.hpp"
#include "VmfScene.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace hammer::vmf {

// CDispCreateDlg clamps the creation power to [2..4] (dispdlg.cpp), and
// CMapDisp/CCoreDispInfo only build surfaces in that range.
inline constexpr int MinDisplacementPower = 2;
inline constexpr int MaxDisplacementPower = 4;

// CDispPaintDistDlg's slider ranges (dispdlg.cpp DISPPAINT_DISTANCE_MIN/MAX and
// DISPPAINT_SPATIALRADIUS_MIN/MAX/STEP).
inline constexpr double PaintDistanceMin = 0.0;
inline constexpr double PaintDistanceMax = 30.0;
inline constexpr double PaintRadiusMin = 1.0;
inline constexpr double PaintRadiusMax = 256.0;
inline constexpr double PaintRadiusStep = 16.0;

// CToolDisplace's constructor defaults (tooldisplace.cpp): m_flPaintValueGeo =
// 5.0f, m_uiBrushType = DISPPAINT_BRUSHTYPE_SOFT. The original defaults its
// spatial radius to 15, but its default paint path is the grid-footprint
// filter brush the port does not have; spatial is all there is here, and a
// 15-unit sphere centred on the snapped vertex covers no neighbour at the
// common 16+ unit texel spacing - exactly one vertex would ever move. Default
// to a radius that reaches a small neighbourhood instead.
inline constexpr double DefaultPaintDistance = 5.0;
inline constexpr double DefaultPaintRadius = 48.0;

// tooldisplace.h DISPPAINT_BRUSHTYPE_SOFT / _HARD.
enum class PaintBrushType { Soft, Hard };

// tooldisplace.h DISPPAINT_AXIS_X..DISPPAINT_AXIS_FACE. Subdiv is listed to
// keep the combo's order but is not implemented (the port never subdivides).
enum class PaintAxis { X, Y, Z, Subdiv, Face };

// The number of vertices along one edge of a power-N displacement:
// CCoreDispInfo's (1 << power) + 1.
inline constexpr int displacementGridSize(int power) { return (1 << power) + 1; }
inline constexpr int displacementVertexCount(int power)
{
    return displacementGridSize(power) * displacementGridSize(power);
}

// The dispinfo chunk. Field names match the VMF keys, which are in turn the
// CMapDisp members CMapDisp::SaveDispData writes.
struct DisplacementInfo
{
    int power{3};
    Vec3 startPosition{};
    double elevation{0.0};
    bool subdiv{false};
    std::vector<Vec3> normals;   // the field vectors
    std::vector<double> distances;
    std::vector<Vec3> offsets;
    std::vector<Vec3> offsetNormals;
    std::vector<double> alphas;

    int gridSize() const { return displacementGridSize(power); }
    int vertexCount() const { return displacementVertexCount(power); }
    bool valid() const;
};

// CMapDisp::InitData( power ) + ResetFieldData(): a flat displacement of the
// given power whose field vectors, distances, offsets and alphas are all zero.
// The offset normals are the face normal, which is what CMapDisp writes for a
// freshly created displacement (ResetFieldData leaves m_SubdivNormals at the
// surface normal).
DisplacementInfo makeDisplacement(int power, const Vec3& startPosition, const Vec3& faceNormal);

// Reads / writes the "dispinfo" child of a side. Reading a chunk whose power is
// out of range, or whose rows do not match the power, returns nullopt.
std::optional<DisplacementInfo> readDisplacement(const Block& side);
void writeDisplacement(Block& side, const DisplacementInfo& info);
bool removeDisplacement(Block& side);
bool hasDisplacement(const Block& side);

// The four base-surface corners rotated so corner 0 is the one nearest
// startposition, which is what CCoreDispSurface::FindSurfPointStartIndex +
// AdjustSurfPointData do at load time (and what VmfScene::buildDisplacement
// replicates).
std::array<Vec3, 4> orientDisplacementCorners(const std::array<Vec3, 4>& corners,
                                              const Vec3& startPosition);

// CCoreDispInfo::GenerateDispSurf's undisplaced ("flat") grid: the bilinear
// interpolation of the four oriented corners.
std::vector<Vec3> displacementFlatVertices(const std::array<Vec3, 4>& corners, int power);

// The rendered position of every vertex, i.e. what VmfScene::buildDisplacement
// produces: flat + elevation * normal + offset + fieldVector * fieldDistance.
std::vector<Vec3> displacementVertexPositions(const DisplacementInfo& info,
                                              const std::array<Vec3, 4>& corners,
                                              const Vec3& faceNormal);

// One displacement the paint sphere can touch: the side it lives on plus the
// geometry the VMF chunk does not carry. The document adapter fills these in
// from the built Scene, as the original reads them off CMapDisp.
struct DisplacementTarget
{
    FaceRef face;
    std::array<Vec3, 4> corners{};
    Vec3 normal{};
};

// SpatialPaintData_t (disppaint.h), reduced to the Raise/Lower geometry effect
// the port implements. m_flScalar is already signed: CToolDisplace negates it
// when the right mouse button is down (ApplySpatialPaintTool).
struct SpatialPaintData
{
    Vec3 center{};
    double radius{DefaultPaintRadius};
    double scalar{DefaultPaintDistance};
    Vec3 paintAxis{0.0, 0.0, 1.0};
    PaintBrushType brushType{PaintBrushType::Soft};
};

// CDispPaintMgr::DoPaintAdd for one displacement: every vertex strictly inside
// the sphere is moved along the paint axis and folded back into the field
// vector / field distance pair.
//
// DEVIATION: CMapDisp::PaintPosition_Update recovers the field vector by
// subtracting the flat vertex, the subdivision position and the elevation from
// the painted position. The port has no subdivision positions (subdiv is always
// 0 here), and it does keep the VMF "offsets" row, so the inverse used is the
// inverse of the port's own renderer (VmfScene::buildDisplacement):
//   segment  = painted - flat - offset - elevation * faceNormal
//   distance = |segment|, fieldVector = normalize(segment)
// which round-trips paint through the renderer exactly.
//
// Returns true if any vertex moved.
bool paintDisplacement(DisplacementInfo& info, const std::array<Vec3, 4>& corners,
                       const Vec3& faceNormal, const SpatialPaintData& paint);

// CDispPaintMgr::DoPaintAdd applied to the alpha channel
// (CMapDisp::PaintAlpha_Update): the same sphere and falloff, but the painted
// value is a 0..255 blend alpha rather than a position.
bool paintDisplacementAlpha(DisplacementInfo& info, const std::array<Vec3, 4>& corners,
                            const Vec3& faceNormal, const SpatialPaintData& paint);

// ---------------------------------------------------------------------------
// Attribute editing of an EXISTING displacement (the Attributes group on
// IDD_FACEEDIT_DISP: Power / Elev / Scale + Apply).
// ---------------------------------------------------------------------------

// CMapDisp::Resample( power ) (hammer/mapdisp.cpp), which walks one power at a
// time through UpSample / DownSample. Every per-vertex channel the port keeps
// is resampled with the original's rules:
//   UpSample   - copy the old vertex, then average with the right / up
//                neighbour; the diagonal slot is the average of the RIGHT and
//                UP neighbours (the original's anti-diagonal quirk, mapdisp.cpp
//                lines 616-635), not of the two diagonal corners.
//   DownSample - each kept vertex becomes the average of itself and its up to
//                eight neighbours (SamplePoints / GetValidSamplePoints).
//
// DEVIATION: the original resamples distances, field vectors, alphas,
// subdivision positions and subdivision normals. The port has no subdivision
// data, but it does keep the VMF "offsets" and "offset_normals" rows, so those
// are resampled with the same rule the original applies to the subdiv rows.
// Returns false when the power is unchanged or out of range.
bool resampleDisplacement(DisplacementInfo& info, int power);

// CMapDisp::Elevate( elevation ): sets the elevation and lets the surface
// rebuild. Returns false when nothing changed.
bool elevateDisplacement(DisplacementInfo& info, double elevation);

// CMapDisp::Scale( scale ). CMapDisp keeps m_Scale as the *current* scale and
// un-scales by 1/m_Scale before applying the new one; m_Scale is not part of
// the VMF dispinfo chunk (CMapDisp::SaveDispData never writes it), so the port
// cannot recover it after a reload. previousScale is therefore passed in by the
// caller, which holds it for the lifetime of the sheet exactly as the original
// dialog's edit box does.
bool scaleDisplacement(DisplacementInfo& info, double previousScale, double scale);

// CMapDisp::ApplyNoise( min, max, rockiness ) (hammer/mapdisp.cpp). The noise
// value comes from the original's PerlinNoise2D (hammer/noise.cpp), sampled at
// (x + z, y + z) of the vertex's current world position, and is applied along
// the field vector - or the offset normal when the field vector is zero, which
// is the port's stand-in for GetSubdivNormal (see makeDisplacement).
bool applyDisplacementNoise(DisplacementInfo& info, const std::array<Vec3, 4>& corners,
                            const Vec3& faceNormal, double minimum, double maximum,
                            double rockiness = 1.0);

// ---------------------------------------------------------------------------
// Sewing (hammer/dispsew.cpp).
// ---------------------------------------------------------------------------

// DISPSEW_POINT_TOLERANCE - "one unit".
inline constexpr double SewPointTolerance = 1.0;

// One displacement taking part in a sew pass: the chunk plus the base-surface
// corners already oriented so corner 0 is the one nearest startposition, which
// is the order CMapDisp's corner / edge point indices assume.
struct SewSurface
{
    DisplacementInfo* info{nullptr};
    std::array<Vec3, 4> corners{};
    // Alphas are only blended between faces whose material matches
    // (AverageVectorFieldData / SewCorner_ResolveDisp compare GetShortName).
    std::string material;
};

// dispsew.cpp GetCornerPointIndex / GetEdgePointIndex.
int displacementCornerPointIndex(int gridSize, int corner);
int displacementEdgePointIndex(int gridSize, int edge, int pointIndex, bool counterClockwise);

// FaceListSewEdges reduced to the two resolve paths that only involve
// displacement surfaces: SewCorner_Resolve -> SewCorner_ResolveDisp, then
// SewEdge_Resolve -> SewEdge_ResolveDispNormal.
//
// DEVIATION: the T-junction paths (SewTJunc_*, SewEdge_ResolveDispTJunc), the
// displacement-to-solid paths (SewCorner_ResolveSolid,
// SewEdge_ResolveSolidNormal / _ResolveSolidTJunc) and PlanarizeDependentVerts
// are not ported; edges that compare as a T-junction are skipped rather than
// half-sewn. Returns true when any surface changed.
bool sewDisplacements(std::vector<SewSurface>& surfaces);

} // namespace hammer::vmf
