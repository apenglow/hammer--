#pragma once

// The texture half of Hammer's Texture Application tool.
//
// Original sources:
//   hammer/ToolMaterial.cpp            - CToolMaterial, the tool itself
//   hammer/faceeditsheet.cpp/.h        - CFaceEditSheet, the face list and click modes
//   hammer/faceedit_materialpage.cpp   - CFaceEditMaterialPage::Apply / OnJustify /
//                                        OnAlign / AlignToView / CopyTCoordSystem
//   hammer/mapface.cpp                 - CMapFace::InitializeTextureAxes,
//                                        GetFaceExtents, GetTextureExtents,
//                                        JustifyTextureUsingExtents,
//                                        RotateTextureAxes, NormalizeTextureShifts
//
// Everything here operates on the VMF "side" chunk, which is what CMapFace
// serializes to: material, uaxis, vaxis, rotation, lightmapscale.

#include "VmfDocument.hpp"
#include "VmfScene.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hammer::vmf {

// One face of one solid. CFaceEditSheet::StoredFace_t keeps the CMapSolid and
// the CMapFace pointer; the port has no persistent face objects, so the face
// list stores the solid and side ids and resolves them per operation.
struct FaceRef
{
    int solidId{-1};
    int sideId{-1};

    bool operator==(const FaceRef&) const = default;
};

// CMapFace::texture (the TEXTURE struct in hammer/mapface.h) reduced to the
// fields the material page reads and writes.
struct FaceTexture
{
    std::string material;
    Vec3 uAxis{1.0, 0.0, 0.0};
    double uShift{0.0};
    double uScale{0.25};
    Vec3 vAxis{0.0, -1.0, 0.0};
    double vShift{0.0};
    double vScale{0.25};
    double rotation{0.0};
    int lightmapScale{16};
};

FaceTexture readFaceTexture(const Block& side);
void writeFaceTexture(Block& side, const FaceTexture& texture);

// hammer/SmoothingGroupMgr.h MAX_SMOOTHING_GROUP_COUNT. The manager keeps one
// bucket of faces per group; the VMF stores the same membership per side as the
// "smoothing_groups" bitmask, which is what the port works from (there are no
// persistent CMapFace objects to hang a manager off).
//
// DEVIATION/ASSUMPTION: the group numbering shown in the original's dialog
// (ID_SMOOTHING_GROUP_1..ID_SMOOTHING_GROUP_32 in hammer/resource.h) is mapped
// to bit N-1 of the mask, so group 1 is value 1. The dialog implementation is
// not in this tree (only the resource ids are), so this is the natural reading
// of SmoothingGroupHandle_t 0..31 and matches what vbsp ANDs the masks with.
inline constexpr int MaxSmoothingGroupCount = 32;

// Reads the side's "smoothing_groups" value. Missing or unparsable reads 0,
// which is what the port writes on every newly created side.
std::uint32_t readSmoothingGroups(const Block& side);
void writeSmoothingGroups(Block& side, std::uint32_t groups);

// Bit for a 1-based group number; 0 for an out of range group.
inline constexpr std::uint32_t smoothingGroupBit(int group)
{
    if (group < 1 || group > MaxSmoothingGroupCount) return 0u;
    return 1u << (group - 1);
}

// CFaceEditMaterialPage::Apply reads every dialog field into a float that stays
// at NOT_INIT when the field is blank (multiple selected faces disagree), and
// only writes back the fields that are not NOT_INIT. std::optional is that.
struct FaceTextureEdit
{
    std::optional<double> shiftX;
    std::optional<double> shiftY;
    std::optional<double> scaleX;
    std::optional<double> scaleY;
    std::optional<double> rotation;
    std::optional<int> lightmapScale;
    std::optional<std::string> material;

    bool empty() const
    {
        return !shiftX && !shiftY && !scaleX && !scaleY && !rotation && !lightmapScale && !material;
    }
};

// CMapFace::GetOrientation: index into FaceNormals/DownVectors/RightVectors.
// Returns -1 for a degenerate normal (FACE_ORIENTATION_INVALID).
int faceOrientation(const Vec3& normal);

// CMapFace::RotateTextureAxes: rotate U and V around the texture normal axis
// (V x U), counter-clockwise, in degrees.
void rotateTextureAxes(FaceTexture& texture, double degrees);

// CMapFace::NormalizeTextureShifts. Texture size 0 skips the fmod, exactly as
// the original skips it when the face has no texture loaded.
void normalizeTextureShifts(FaceTexture& texture, int textureWidth, int textureHeight);

enum class TextureAlignment { World, Face };

// CMapFace::InitializeTextureAxes( eAlignment, INIT_TEXTURE_AXES | INIT_TEXTURE_FORCE ),
// which is what the material page's Align World / Align Face buttons issue.
// The shift components are deliberately preserved, as in the original.
void initializeTextureAxes(FaceTexture& texture, const Vec3& normal, TextureAlignment alignment);

// CMapFace::GetFaceExtents: the six world points of the face that are extreme
// along -X, +X, -Y, +Y, -Z, +Z. Not a bounding box - every point is on the face.
using FaceExtents = std::array<Vec3, 6>;

bool faceExtents(const std::vector<Vec3>& points, FaceExtents& extents);

// CFaceEditMaterialPage::GetAllFaceExtents: the same per-axis extreme selection
// applied across several faces ("Treat as one").
void mergeFaceExtents(FaceExtents& into, const FaceExtents& from, bool first);

enum class TextureJustification { Top, Bottom, Left, Right, Center, Fit };

// CMapFace::JustifyTextureUsingExtents.
void justifyTextureUsingExtents(FaceTexture& texture, TextureJustification justification,
                                const FaceExtents& extents, int textureWidth, int textureHeight);

// CFaceEditMaterialPage::AlignToView: map the texture using the 3D view's right
// and up vectors, reset rotation, and reset scale to the default texture scale.
void alignTextureToView(FaceTexture& texture, const Vec3& viewRight, const Vec3& viewUp,
                        const Vec3& viewPoint, int textureWidth, int textureHeight);

// A face plus everything the justify/align operations need that the VMF side
// chunk does not carry: the face polygon (for the extents), the face normal,
// and the size of the material currently on it. The document adapter fills
// these in from the built Scene and the MaterialSystem, the way the original
// reads CMapFace::Points and IEditorTexture::GetWidth/GetHeight.
struct FaceEditTarget
{
    FaceRef face;
    Vec3 normal{};
    std::vector<Vec3> points;
    int textureWidth{0};
    int textureHeight{0};
};

// CFaceEditMaterialPage::CopyTCoordSystem: copy the coordinate system from one
// face onto another and rotate it about their shared edge. Used by Alt+RMB
// ("edge align") in the 3D view.
void copyTextureCoordinateSystem(const FaceTexture& from, const Vec3& fromNormal, double fromDistance,
                                 FaceTexture& to, const Vec3& toNormal, double toDistance,
                                 int textureWidth, int textureHeight);

} // namespace hammer::vmf
