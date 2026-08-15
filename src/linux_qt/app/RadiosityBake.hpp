#pragma once

#include "VmfScene.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace hammer::render {

// VRAD's lightmap layout, rebuilt on top of VMF geometry instead of a compiled
// BSP. utils/vrad derives every luxel from texinfo::lightmapVecsLuxelsPerWorldUnit,
// which is the face's texture axis divided by the face's lightmap scale. The
// same construction works directly from hammer::vmf::FaceGeometry, so the grid
// this produces lines up luxel-for-luxel with what vrad.exe would emit.

// Source clamps a face's lightmap to MAX_LIGHTMAP_DIM_WITHOUT_BORDER. The editor
// preview is not writing a BSP lump, so it only needs a bound that keeps a single
// oversized brush from consuming the whole atlas.
inline constexpr int kMaximumLuxelsPerAxis = 128;

struct LightmapRect
{
    // Placement inside the lightmap atlas, in luxels.
    int x{0};
    int y{0};
    int width{1};
    int height{1};
    // Luxel-space origin of the face: floor(min) over the face's vertices. A
    // world position maps to this rect via
    //   luxel = dot(position, axis.direction) / lightmapScale - origin
    double originS{0.0};
    double originT{0.0};
    double lightmapScale{16.0};
    // The face's own $lightmapscale, before any chop multiplier or oversize
    // coarsening. The preview needs the true luxel width to size the footprint
    // it supersamples shadow rays across, and that cannot be recovered by
    // dividing lightmapScale by a single global multiplier once displacements
    // chop differently from brush faces.
    double authoredScale{16.0};
};

struct LightmapLayout
{
    int width{0};
    int height{0};
    // Keyed by faceKey(brushId, sideId).
    std::unordered_map<std::uint64_t, LightmapRect> faces;
    // Faces whose authored lightmap scale had to be coarsened because their
    // grid exceeded kMaximumLuxelsPerAxis. VBSP would subdivide the face
    // instead; we cannot, so this is reported rather than done silently.
    int coarsenedFaces{0};

    bool valid() const { return width > 0 && height > 0 && !faces.empty(); }
};

inline std::uint64_t faceKey(int brushId, int sideId)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(brushId)) << 32u) |
           static_cast<std::uint32_t>(sideId);
}

struct LightmapLayoutOptions
{
    // Mirrors CMapDoc::IsDispSolidDrawMask, so the layout skips exactly the
    // faces RayTracingSceneBuilder skips.
    bool displacementSolidMask{true};
    // Faces whose material should receive a lightmap. Tool textures, sky and
    // %compilenolight surfaces are excluded by the caller, which owns the
    // material system.
    std::function<bool(std::string_view normalizedMaterial)> faceIsLit;
    // Upper bound on the square atlas edge, in luxels.
    int maximumAtlasSize{8192};
    // Patch size in luxel widths - VRAD's -maxchop, which is measured in luxels
    // and not in world units. 1 gives the luxel grid itself; kPatchScaleMultiplier
    // gives the radiosity patch grid.
    int scaleMultiplier{1};
    // VRAD chops displacements with -dispchop, which defaults COARSER than
    // brush faces. Zero follows scaleMultiplier.
    int displacementScaleMultiplier{0};
    // Used only by faces that carry no "lightmapscale" key of their own.
    // Hammer takes this from g_pGameConfig->GetDefaultLightmapScale(); a face
    // that does declare a scale always keeps the value it declared.
    int defaultLightmapScale{16};
};

// Measures every lit face's luxel grid and shelf-packs the grids into one
// square atlas. Returns an empty layout when nothing is lit or the faces cannot
// be packed within maximumAtlasSize.
LightmapLayout buildLightmapLayout(const hammer::vmf::Scene& scene,
                                   const LightmapLayoutOptions& options);

// GPU-side description of one face's lightmap rect. Lets a shader map any world
// position on that face to its luxel, which is what turns a ray-query primitive
// hit back into a patch during transfer gathering.
struct alignas(16) RadRectRecord
{
    // x, y, width, height in luxels.
    std::array<float, 4> rect{};
    // originS, originT, patch lightmapScale, face luxel scale.
    std::array<float, 4> origin{};
    // xyz = uAxis.direction, w = uAxis.shift.
    std::array<float, 4> uAxis{};
    // xyz = vAxis.direction, w = vAxis.shift.
    std::array<float, 4> vAxis{};
    // x = material index (filled in by RayTracingSceneBuilder), yz = faceKey,
    // w = reserved.
    std::array<std::uint32_t, 4> identity{};
};
static_assert(sizeof(RadRectRecord) == 80);

struct RadRectTable
{
    std::vector<RadRectRecord> records;
    // faceKey -> index into records.
    std::unordered_map<std::uint64_t, std::uint32_t> indices;
};

// Builds the GPU rect table for a layout. Material indices are left at zero for
// the caller to fill, since only the scene builder knows them.
RadRectTable buildRectTable(const hammer::vmf::Scene& scene, const LightmapLayout& layout);

// One VRAD sample point. BuildFacesamples() produces one of these per luxel; it
// records where the luxel sits in the world but carries no lighting itself.
struct alignas(16) LuxelSample
{
    // xyz world position, w unused.
    std::array<float, 4> position{};
    // xyz surface normal, w unused.
    std::array<float, 4> normal{};
    // x,y = atlas texel this sample writes; z = 1 when the sample is inside the
    // face winding (edge luxels are nudged inward and flagged 0, matching VRAD's
    // handling of samples that fall off the face); w = rect-table index, which
    // is how pass 1 reaches the sample's material for patch reflectivity.
    std::array<float, 4> atlas{};
};
static_assert(sizeof(LuxelSample) == 48);

// VRAD's chop, and it is measured in LUXEL WIDTHS, not world units.
//
// utils/vrad declares `maxchop = 4; // coarsest allowed number of luxel widths
// for a patch`, and SubdividePatch scales a patch's extent by patch->luxscale -
// luxels per world unit, taken from lightmapVecsLuxelsPerWorldUnits - before
// comparing it against chop. So the default -chop 4 is four luxels across,
// which at the default $lightmapscale of 16 is a 64-unit patch, not a 4-unit
// one. Reading it as world units makes VRAD look sixteen times finer per axis
// than it is, and makes a preview that matches it look broken.
//
// scaleMultiplier is in exactly the same unit, so this constant IS the chop:
// 4 reproduces VRAD's default. Going finer does not move the result toward a
// compile, it moves away from one, and it costs patch count quadratically in
// transfer memory (patches * kTransfersPerPatch * 8 bytes).
//
// buildRadiosityData coarsens further on its own when a map exceeds
// MAX_PATCHES, so this is a floor rather than a guarantee: plr_hightower lands
// on 4 either way, because at 1 it needs 1.47M patches against a 262144 cap.
inline constexpr int kPatchScaleMultiplier = 4;

// VRAD's dispchop, also in luxel widths. Displacements are deliberately chopped
// coarser than brush faces: they are dense, and their bounce contribution does
// not repay patches at the brush-face rate.
inline constexpr int kDisplacementPatchScaleMultiplier = 8;

// VRAD's MAX_PATCHES. Beyond this the patch layout is coarsened further.
inline constexpr int kMaximumPatches = 4 * 65536;

// Generates one LuxelSample per texel of every rect in the layout. Faces are
// reconstructed from their plane plus the two lightmap axes; displacement faces
// are rasterized from their triangles so samples follow the displaced surface.
std::vector<LuxelSample> buildLuxelSamples(const hammer::vmf::Scene& scene,
                                           const LightmapLayout& layout,
                                           const RadRectTable& rectTable);

// Everything the GPU bake needs about a scene's lighting surfaces: the luxel
// grid that receives the final lightmap, and the coarser patch grid that carries
// radiosity between bounces. Both are the same construction at different scales,
// and both key their faces by faceKey(), so a luxel can always find the patch
// grid cell covering it.
struct RadiosityData
{
    LightmapLayout luxelLayout;
    LightmapLayout patchLayout;
    RadRectTable luxelRects;
    RadRectTable patchRects;
    std::vector<LuxelSample> luxels;
    std::vector<LuxelSample> patches;
    // patchAtlas-sized grid of patch index + 1, or 0 where no patch covers the
    // texel. Turns a ray-query hit position into the patch that received it,
    // which is what transfer gathering needs.
    std::vector<std::uint32_t> patchIndexGrid;
    // Why the bake is unavailable, when it is. A map that silently falls back to
    // the pre-bake approximation looks like weak lighting rather than a missing
    // feature, so the reason has to travel with the data.
    std::string status;

    // buildRectTable walks brushes and faces in the same order for both grids
    // and both layouts contain the same faces, so a face has the same index in
    // luxelRects and patchRects. RayTracingTriangle::data.z therefore addresses
    // both tables. Verified by buildRadiosityData.
    bool rectTablesAligned() const
    {
        return luxelRects.records.size() == patchRects.records.size() &&
               luxelRects.indices == patchRects.indices;
    }

    bool valid() const
    {
        return luxelLayout.valid() && patchLayout.valid() &&
               !luxels.empty() && !patches.empty() && rectTablesAligned();
    }

    // What the patch-only path (buildRadiosityPatchData) needs to be usable.
    bool patchesValid() const
    {
        return patchLayout.valid() && !patches.empty() && !patchRects.records.empty() &&
               !patchIndexGrid.empty();
    }

    // Identifies this solve's inputs by content, not by size. A brush drag, a
    // prop nudge or a material swap almost always leaves the luxel and patch
    // *counts* untouched while moving every sample in the grid, so anything
    // deciding "can the previous solution be kept" has to compare the samples
    // themselves. Hashing ~70 MB costs a few milliseconds against a rebuild
    // that already costs hundreds.
    std::uint64_t contentKey() const
    {
        std::uint64_t key = 1469598103934665603ull;
        const auto mix = [&key](std::uint64_t value) {
            key = (key ^ value) * 1099511628211ull;
        };
        const auto mixSamples = [&](const std::vector<LuxelSample>& samples) {
            mix(samples.size());
            static_assert(std::is_trivially_copyable_v<LuxelSample>);
            const auto* words = reinterpret_cast<const std::uint64_t*>(samples.data());
            const std::size_t count = samples.size() * sizeof(LuxelSample) / sizeof(std::uint64_t);
            for (std::size_t i = 0; i < count; ++i) mix(words[i]);
        };
        mixSamples(luxels);
        mixSamples(patches);
        mix(luxelRects.records.size());
        for (const RadRectRecord& record : luxelRects.records) {
            const auto* words = reinterpret_cast<const std::uint64_t*>(&record);
            for (std::size_t i = 0; i < sizeof(RadRectRecord) / sizeof(std::uint64_t); ++i)
                mix(words[i]);
        }
        return key;
    }
};

// Builds both grids. Patch density is reduced further if the coarse grid still
// exceeds MAX_PATCHES.
RadiosityData buildRadiosityData(const hammer::vmf::Scene& scene,
                                 const LightmapLayoutOptions& options);

// Builds only the radiosity patch grid, leaving the luxel members empty.
//
// This is what the live preview uses. It reads bounced light straight out of
// the patch grid instead of resolving it down onto a luxel lightmap first,
// which is affordable because indirect light is low frequency and because the
// preview already computes direct light per pixel. valid() is false on the
// result by design - check patchesValid() instead.
RadiosityData buildRadiosityPatchData(const hammer::vmf::Scene& scene,
                                      const LightmapLayoutOptions& options);

// Luxel-space coordinate of a world position on a face, before the rect offset.
// `scale` is LightmapRect::lightmapScale, which may be coarser than the face's
// authored $lightmapscale when the face was too large for the luxel cap.
void lightmapCoordinate(const hammer::vmf::FaceGeometry& face,
                        const hammer::vmf::Vec3& position,
                        double scale,
                        double& outS, double& outT);

} // namespace hammer::render
