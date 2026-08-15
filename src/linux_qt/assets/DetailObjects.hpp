#pragma once

#include "GameFileSystem.hpp"
#include "VmfScene.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hammer::assets {

// --- Detail objects ---------------------------------------------------------
//
// Source's detail props: the grass, weeds and pebbles VBSP scatters across
// world surfaces at compile time. Nothing about them is authored per instance.
// A material declares a detail type with "%detailtype", the map's detail.vbsp
// (worldspawn "detailvbsp") defines what that type plants and how densely, and
// utils/vbsp/detailobjects.cpp EmitDetailModels does the scattering.
//
// This is a port of that emitter, so the preview plants the same objects, at
// the same density, chosen the same way. Two deliberate differences, both
// unavoidable in an editor:
//
//   * VBSP emits onto BSP faces, which are the editor's faces after the BSP
//     tree has split them; every fragment is re-seeded with the same Hammer
//     face id. The preview has only the unsplit editor face. Placement is
//     therefore statistically identical and stable, but the individual objects
//     do not land where a compiled map puts them.
//   * "spriterandomscale" uses VBSP's RandomGaussianFloat, from a random
//     stream (vstdlib) that is not part of this tree. The preview draws its
//     Gaussian from the same LCG as everything else, so scale jitter has the
//     right distribution but not the same sequence.
//
// Everything else - the per-face seeding, the sample counts, the group and
// model selection, the surface-angle rejection, the orientation math - runs
// the original's arithmetic in the original's order.

enum class DetailPropType
{
    // Mirrors DetailPropType_t (public/gamebspfile.h).
    Model = 0,
    Sprite = 1,
    ShapeCross = 2,
    ShapeTri = 3,
};

struct DetailModelDefinition
{
    std::string modelName;
    DetailPropType type{DetailPropType::Sprite};
    // Cumulative within its group, as ParseDetailGroup accumulates "amount",
    // renormalized when the group's total exceeds one.
    float amount{1.0f};
    float minCosAngle{-1.0f};
    float maxCosAngle{-1.0f};
    bool upright{false};
    int orientation{0};
    // m_Pos: sprite corners in world units, relative to the origin, on the
    // sprite's own axes. m_Tex: normalized atlas coordinates.
    std::array<float, 2> positionUpperLeft{-10.0f, 20.0f};
    std::array<float, 2> positionLowerRight{10.0f, 0.0f};
    std::array<float, 2> texUpperLeft{0.0f, 0.0f};
    std::array<float, 2> texLowerRight{1.0f, 1.0f};
    float randomScaleStdDev{0.0f};
    std::uint8_t shapeSize{0};
    std::uint8_t shapeAngle{0};
    std::uint8_t swayAmount{0};
};

struct DetailObjectGroup
{
    // The displacement blend alpha this group is authored for. Groups are kept
    // sorted ascending, as ParseDetailGroup inserts them.
    float alpha{1.0f};
    std::vector<DetailModelDefinition> models;
};

struct DetailObjectType
{
    std::string name;
    // Objects per million square units of surface: VBSP computes a triangle's
    // sample count as area * density * 0.000001.
    float density{0.0f};
    std::vector<DetailObjectGroup> groups;
};

struct DetailObjectDictionary
{
    std::vector<DetailObjectType> types;

    // Case-insensitive, matching the material key lookup VBSP does.
    const DetailObjectType* find(std::string_view name) const;
    bool empty() const { return types.empty(); }
};

// ParseDetailObjectFile. "root" is the parsed detail.vbsp - one outer block
// whose children are the detail object types.
DetailObjectDictionary parseDetailObjectDictionary(const hammer::vmf::Block& root);

// Reads and parses the dictionary named by a scene's worldspawn "detailvbsp"
// key, defaulting to "detail.vbsp" (FindDetailVBSPName). Returns an empty
// dictionary when the game mount has no such file.
DetailObjectDictionary loadDetailObjectDictionary(const GameFileSystem& fileSystem,
                                                  std::string_view fileName);

// One placed detail object. Sprites carry their own corner and atlas rects
// because a single detail type mixes several sprites out of one atlas.
struct DetailPropInstance
{
    DetailPropType type{DetailPropType::Sprite};
    std::string model;
    hammer::vmf::Vec3 origin{};
    // Source pitch/yaw/roll. For orientation 1 and 2 these are overwritten at
    // render time by detailSpriteAxes, exactly as CDetailModel::ComputeAngles
    // recomputes them per frame.
    hammer::vmf::Vec3 angles{};
    float scale{1.0f};
    int orientation{0};
    std::array<float, 2> positionUpperLeft{};
    std::array<float, 2> positionLowerRight{};
    std::array<float, 2> texUpperLeft{};
    std::array<float, 2> texLowerRight{};
    std::uint8_t shapeAngle{0};
    std::uint8_t shapeSize{0};
    std::uint8_t swayAmount{0};
};

// Returns the "%detailtype" of a material, or an empty string. Injected rather
// than taken as a MaterialSystem so the emitter can be tested without a game
// mount.
using DetailTypeForMaterial = std::function<std::string(std::string_view)>;

struct DetailPropEmission
{
    std::vector<DetailPropInstance> props;
    // VBSP refuses to emit past 65535 detail objects and warns; the preview
    // does the same rather than quietly drawing a different map.
    std::size_t overflowed{0};
};

// EmitDetailModels, restricted to surface emission. The prop_detail /
// prop_detail_sprite entity forms are deliberately not handled here.
DetailPropEmission emitDetailProps(const hammer::vmf::Scene& scene,
                                   const DetailObjectDictionary& dictionary,
                                   const DetailTypeForMaterial& detailTypeForMaterial);

// The four corners of a detail sprite's quad, in the order
// CDetailModel::DrawTypeSprite emits them (upper left, lower left, lower
// right, upper right), together with their atlas coordinates. Orientation 1
// and 2 face the camera, so this needs the view origin; orientation 0 ignores
// it and uses the angles baked in at emission.
struct DetailSpriteQuad
{
    std::array<hammer::vmf::Vec3, 4> corners{};
    std::array<std::array<float, 2>, 4> texCoords{};
    // Faces the viewer for orientations 1 and 2; the sprite's own normal
    // otherwise.
    hammer::vmf::Vec3 normal{0.0, 0.0, 1.0};
};

DetailSpriteQuad detailSpriteQuad(const DetailPropInstance& prop,
                                  const hammer::vmf::Vec3& viewOrigin);

// --- Distance fade ----------------------------------------------------------
//
// The engine never draws every detail prop in a map: CDetailObjectSystem drops
// anything past cl_detaildist (1200) and fades sprites in across the last
// cl_detailfade (400) units. A preview that draws them all shows a far denser
// world than the game does, so both viewports apply the same rule.
struct DetailPropFade
{
    // cl_detailfade: the WIDTH of the fade band, not where it starts.
    float fadeWidth{400.0f};
    // cl_detaildist: nothing is drawn at or past this distance.
    float maxDistance{1200.0f};
};

// CDetailObjectSystem::LevelInitPostEntity - an env_detail_controller can only
// narrow the defaults, never widen them.
DetailPropFade detailPropFadeForScene(const hammer::vmf::Scene& scene);

// CDetailObjectSystem::SortSpritesBackToFront's per-object alpha, 0 to 1. Zero
// means the object is past cl_detaildist and is not drawn at all. Note the
// falloff is linear in SQUARED distance, as the original computes it.
float detailPropAlpha(const hammer::vmf::Vec3& origin, const hammer::vmf::Vec3& viewOrigin,
                      const DetailPropFade& fade);

} // namespace hammer::assets
