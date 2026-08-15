#pragma once

#include "Camera3D.hpp"
#include "DetailObjects.hpp"
#include "MaterialSystem.hpp"
#include "RadiosityBake.hpp"
#include "StudioModelSystem.hpp"
#include "VmfScene.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace hammer::render {

// GPU records deliberately use only 16-byte lanes so their C++ layout exactly
// matches GLSL std430 on every Vulkan implementation.
struct alignas(16) RayTracingVertex
{
    std::array<float, 4> position{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> normal{0.0f, 0.0f, 1.0f, 0.0f};
    std::array<float, 4> tangent{1.0f, 0.0f, 0.0f, 1.0f};
    // xy = primary UV, zw = secondary UV.
    std::array<float, 4> texCoord{0.0f, 0.0f, 0.0f, 0.0f};
    // x = displacement blend alpha. Remaining lanes are reserved.
    std::array<float, 4> surface{0.0f, 0.0f, 0.0f, 0.0f};
};

struct alignas(16) RayTracingTriangle
{
    std::array<std::uint32_t, 4> data{0u, 0u, 0u, 0u};
    // data.x = material index, data.y = triangle flags, data.z = rect index
    // into the radiosity rect tables (lightmapped faces only).
    //
    // data.w = this triangle's slot in the static-prop ambient-cube array, or
    // 0xffffffff. A prop has no lightmap parameterisation to hang bounced light
    // off, so VRAD lights props per vertex instead; the preview does the same,
    // storing three cubes per prop triangle - one per corner - starting at
    // data.w * 3. Keying it off the triangle rather than the vertex means a hit
    // can find its cubes from the primitive index alone, and only prop
    // triangles pay for the storage.
};

struct alignas(16) RayTracingMaterial
{
    std::array<float, 4> baseRect{};
    std::array<float, 4> secondaryRect{};
    std::array<float, 4> detailRect{};
    std::array<float, 4> bumpRect{};
    std::array<float, 4> exponentRect{};
    std::array<float, 4> selfIllumRect{};
    std::array<float, 4> lightWarpRect{};
    std::array<float, 4> flowRect{};
    std::array<std::array<float, 4>, 6> environmentRects{};

    std::array<float, 4> color2{1.0f, 1.0f, 1.0f, 0.0f};
    std::array<float, 4> phongTintExponent{1.0f, 1.0f, 1.0f, 150.0f};
    std::array<float, 4> envTintBoost{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> selfIllumTintSpecular{1.0f, 1.0f, 1.0f, 0.18f};
    std::array<float, 4> phongFresnelRimExponent{0.0f, 0.5f, 1.0f, 4.0f};
    std::array<float, 4> selfIllumFresnelRimBoost{0.0f, 1.0f, 1.0f, 1.0f};
    // x = detail scale, y = detail blend factor, z = detail blend mode,
    // w = alpha-test reference.
    std::array<float, 4> detailAlphaControls{4.0f, 1.0f, 0.0f, 0.5f};

    std::array<float, 4> waterFogAlpha{0.17f, 0.35f, 0.42f, 0.94f};
    std::array<float, 4> waterRefractAmount{0.75f, 0.90f, 0.94f, 0.0f};
    std::array<float, 4> waterReflectAmount{0.82f, 0.91f, 1.0f, 0.80f};
    std::array<float, 4> waterControls{0.20f, 1.0f, 0.0f, 0.0f};
    // xy = water scale, zw = first scroll vector.
    std::array<float, 4> waterScaleScroll1{1.0f, 1.0f, 0.0f, 0.0f};
    // xy = second scroll vector, z = normal scale, w = flow cycle rate.
    std::array<float, 4> waterScroll2NormalCycle{0.0f, 0.0f, 1.0f, 1.0f};
    // x = flow distance, y = flow-map scale, z = flow-normal UV scale.
    std::array<float, 4> waterFlow{0.10f, 1.0f, 1.0f, 0.0f};
    // x = top-level VMT $alpha. Remaining lanes reserved.
    std::array<float, 4> transparencyControls{1.0f, 0.0f, 0.0f, 0.0f};

    std::array<std::uint32_t, 4> flags{};
};

static_assert(sizeof(RayTracingVertex) == 80);
static_assert(sizeof(RayTracingTriangle) == 16);
static_assert((sizeof(RayTracingMaterial) % 16) == 0);


struct alignas(16) RayTracingLight
{
    // positionType.xyz = position; w = 0 point, 1 spot, 2 directional, 3 ambient, 4 sky-gated environment.
    std::array<float, 4> positionType{};
    // directionRange.xyz = rays leaving light; w = range.
    std::array<float, 4> directionRange{};
    // rgb = linear color, w = intensity.
    std::array<float, 4> colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
    // constant, linear, quadratic, cos outer cone.
    std::array<float, 4> attenuationOuter{0.0f, 0.0f, 1.0f, -1.0f};
    // x = cos inner cone, y = _exponent (spot ramp power, 0 = linear),
    // z = sun angular extent in radians (sin(SunSpreadAngle)), w = unused.
    std::array<float, 4> innerControls{1.0f, 0.0f, 0.0f, 0.0f};
    // VRAD's SetLightFalloffParams outputs, which only the
    // _fifty_percent_distance path ever sets to anything but their defaults.
    // x = m_flCapDist (distance past which the quadratic is frozen so an
    //     extreme falloff cannot start brightening again),
    // y = m_flStartFadeDistance, z = m_flEndFadeDistance (< 0 = no fade),
    // w = unused.
    std::array<float, 4> fadeControls{1.0e22f, 0.0f, -1.0f, 0.0f};
};
static_assert(sizeof(RayTracingLight) == 96);

struct RayTracingAtlas
{
    int width{0};
    int height{0};
    int layers{0};
    std::vector<std::uint8_t> rgba;

    bool valid() const
    {
        return width > 0 && height > 0 && layers > 0 &&
               rgba.size() == static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height) *
                              static_cast<std::size_t>(layers) * 4u;
    }
};

struct RayTracingAnimatedMeshSpan
{
    std::uint32_t entityIndex{0};
    std::uint32_t meshIndex{0};
    std::size_t firstVertex{0};
    std::size_t vertexCount{0};
};

struct RayTracingSpriteSpan
{
    std::uint32_t entityIndex{0};
    std::size_t firstVertex{0};
    float halfWidth{0.0f};
    float halfHeight{0.0f};
};

// One move_rope/keyframe_rope strand's ribbon. Like a sprite, a rope is
// camera-facing, so its vertices have to be re-expanded whenever the camera
// moves; the settled polyline itself does not change, so it is kept here and
// the expansion is redone from it.
struct RayTracingRopeSpan
{
    std::size_t firstVertex{0};
    // Indices into RayTracingScene::ropePoints.
    std::size_t firstPoint{0};
    std::size_t pointCount{0};
    float halfWidth{1.0f};
    // V texture coordinate per world unit along the strand.
    float vPerUnit{0.0f};
};

// One scattered detail sprite. Like a rope and a helper billboard, a detail
// sprite faces the camera, so its four vertices are re-expanded whenever the
// camera moves; the emitted placement it expands from never changes.
struct RayTracingDetailSpriteSpan
{
    std::size_t firstVertex{0};
    std::size_t propIndex{0};
};

struct RayTracingColorCorrection
{
    // xyz origin for point corrections; weight.w is the authored maximum.
    std::array<float, 4> originWeight{};
    // point: x=min falloff, y=max falloff. volume: xyz=min bounds.
    std::array<float, 4> minimum{};
    // volume: xyz=max bounds. w=1 for volume, 0 for point.
    std::array<float, 4> maximum{};
    std::uint32_t lutIndex{0};
    bool enabled{true};
};

struct RayTracingScene
{
    std::vector<RayTracingVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RayTracingTriangle> triangles;
    std::vector<RayTracingMaterial> materials;
    std::vector<RayTracingLight> lights;
    // Packed RGBA8 32^3 Source color-correction lookup cubes.
    std::vector<std::uint32_t> colorCorrectionLutTexels;
    std::vector<RayTracingColorCorrection> colorCorrections;
    hammer::vmf::ToneMapSettings toneMap{};
    RayTracingAtlas atlas;
    std::array<std::array<float, 4>, 6> skyRects{};
    bool hasSky{false};
    bool hasAnimatedContent{false};
    bool hasAnimatedGeometry{false};
    bool hasAnimatedMaterialContent{false};
    bool hasCameraFacingSprites{false};
    std::vector<RayTracingAnimatedMeshSpan> animatedMeshSpans;
    std::vector<RayTracingSpriteSpan> spriteSpans;
    std::vector<RayTracingRopeSpan> ropeSpans;
    std::vector<RayTracingDetailSpriteSpan> detailSpriteSpans;
    // The emitted sprites the spans above index. Detail models are baked into
    // the vertex buffer like any other studio geometry and are not kept here.
    std::vector<hammer::assets::DetailPropInstance> detailSprites;
    // Settled rope polylines, referenced by ropeSpans.
    std::vector<hammer::vmf::Vec3> ropePoints;
    // Radiosity patch grid for the bounce solve. Only the patch members are
    // populated (see buildRadiosityPatchData); TriangleHasLightmap marks the
    // triangles whose data[2] indexes patchRects.
    hammer::render::RadiosityData radiosity;
    // Static-prop triangles that carry an ambient-cube slot in data.w. The prop
    // bake allocates 3 cubes per triangle, so this is the whole size of that
    // array; zero means the scene has no props to light.
    std::uint32_t propCubeTriangles{0};
    std::string error;

    bool valid() const
    {
        return error.empty() && atlas.valid() && !vertices.empty() &&
               indices.size() % 3 == 0 && triangles.size() == indices.size() / 3;
    }
};

enum RayTracingMaterialFlag : std::uint32_t
{
    RtHasSecondaryTexture       = 1u << 0,
    RtBlended                   = 1u << 1,
    RtHasBumpMap                = 1u << 2,
    RtPhong                     = 1u << 3,
    RtSpecular                  = 1u << 4,
    RtSelfIllum                 = 1u << 5,
    RtSelfIllumFresnel          = 1u << 6,
    RtHasSelfIllumMask          = 1u << 7,
    RtHasLightWarp              = 1u << 8,
    RtHalfLambert               = 1u << 9,
    RtRimLight                  = 1u << 10,
    RtWater                     = 1u << 11,
    RtTranslucent               = 1u << 12,
    RtColor2Active              = 1u << 13,
    RtBlendTintByBaseAlpha      = 1u << 14,
    RtPhongMaskFromBaseAlpha    = 1u << 15,
    RtEnvMaskFromBaseAlpha      = 1u << 16,
    RtEnvMaskFromNormalAlpha    = 1u << 17,
    RtInvertSpecularMask        = 1u << 18,
    RtHasExponentMap            = 1u << 19,
    RtPhongExponentOverride     = 1u << 20,
    RtPhongAlbedoTint           = 1u << 21,
    RtRimMaskFromExponentAlpha  = 1u << 22,
    RtHighEnergy                = 1u << 23,
    RtUber                      = 1u << 24,
    RtHasFlowMap                = 1u << 25,
    RtWaterNoFresnel            = 1u << 26,
    RtWaterMultiTexture         = 1u << 27,
    RtHasEnvironmentMap         = 1u << 28,
    RtDecalModulate             = 1u << 29,
    RtCompileTrigger            = 1u << 30,
};

enum RayTracingMaterialFlag2 : std::uint32_t
{
    Rt2HasDetailTexture = 1u << 0,
    Rt2AlphaTest        = 1u << 1,
};

// Per-triangle flags, mirrored by the TRIANGLE_* constants in
// raytraced_preview.comp. These decide which ray types a surface interacts
// with, so they are the difference between a surface that occludes light and
// one that light passes straight through.
enum RayTracingTriangleFlag : std::uint32_t
{
    TriangleTwoSided  = 1u << 0,
    TriangleProjected = 1u << 1,
    TriangleNoShadow  = 1u << 2,
    TriangleNoPrimary = 1u << 3,
    TriangleSky       = 1u << 4,
    TriangleSprite    = 1u << 5,
    // Editor helper geometry: shaded at its authored colour, never lit. Paired
    // with TriangleNoShadow so a helper neither receives nor blocks light.
    TriangleUnlit     = 1u << 6,
    // Read by the radiosity bake, which needs to tell a static prop from a
    // brush face (prop hits are exempt from shadow tests near either end of a
    // ray) and needs to know which faces carry a lightmap rect in data[2].
    TriangleStaticProp  = 1u << 7,
    TriangleHasLightmap = 1u << 8,
    // A displacement triangle. The luxel grid a lightmapped face carries is
    // parameterised on the BASE face's plane, which a displaced surface does
    // not lie in, so anything that steps sideways in luxel space has to know
    // not to do it here.
    TriangleDisplacement = 1u << 9,
};

struct RayTracingBuildOptions
{
    std::unordered_set<std::string> hiddenToolTextures;
    // CMapDoc::IsDispSolidDrawMask - defaults on, as in Hammer.
    bool displacementSolidMask{true};
    hammer::camera::State camera;
    // Editor-only visual aids (camera-facing point-entity sprites). Cubemap
    // bakes clear this so a baked face records the world as the game would
    // see it, without helper billboards burned into the reflection.
    bool entityHelpers{true};
    // View > Show detail objects. Detail props are world content, so this is
    // separate from entityHelpers - it exists because a grassy map scatters
    // tens of thousands of sprites and a mapper may not want to pay for them.
    bool detailProps{true};
    // VRAD's g_bHDR. In LDR mode the HDR-only entity keys are ignored and an
    // eight-value _light resolves to its first (LDR) tuple, which is what a
    // non-HDR compile of the same map would light with.
    bool hdrLighting{true};
    double animationSeconds{0.0};
    int maximumAtlasSize{8192};
    int maximumAtlasLayers{256};
};

// Translates a scene's light/light_spot/light_environment entities into GPU
// light records, following utils/vrad's CreateDirectLights. Exposed so the
// entity-key handling (HDR colour keys, SunSpreadAngle, the two attenuation
// forms) can be tested without standing up a material system.
std::vector<RayTracingLight> mapLightsForScene(const hammer::vmf::Scene& scene,
                                              bool hdrLighting = true);

// The RayTracingMaterial::flags[0] word a material resolves to. Exposed so the
// effect gates can be tested against the OpenGL viewport's equivalents without
// standing up a material system or a GPU.
std::uint32_t materialFlagsForMaterial(const hammer::assets::Material& material);

// The per-triangle ray flags a material name contributes (TriangleNoShadow,
// TriangleSky). Exposed so the tool-material occlusion rules can be tested
// directly - they decide whether light passes through a surface at all.
std::uint32_t shadowFlagsForMaterialName(std::string_view normalizedMaterial);

// Whether a studio model path is a Hammer editor helper (anything under
// models/editor), which neither blocks nor receives light.
bool isEditorHelperModelPath(std::string_view modelPath);

class RayTracingSceneBuilder
{
public:
    RayTracingSceneBuilder(std::shared_ptr<hammer::assets::MaterialSystem> materials,
                           std::shared_ptr<hammer::assets::StudioModelSystem> studioModels);

    RayTracingScene build(const hammer::vmf::Scene& scene,
                          const RayTracingBuildOptions& options) const;

    bool updateDynamicGeometry(const hammer::vmf::Scene& source,
                               const RayTracingBuildOptions& options,
                               RayTracingScene& scene) const;

    // Billboard helpers only need camera-facing vertex updates. Keeping this
    // separate from animated StudioModel sampling lets the RT viewport refresh
    // sprites once the camera settles without paying the prop-animation cost.
    bool updateSpriteGeometry(const hammer::vmf::Scene& source,
                              const RayTracingBuildOptions& options,
                              RayTracingScene& scene) const;

private:
    // detail.vbsp is read once per dictionary name and reused across rebuilds.
    const hammer::assets::DetailObjectDictionary& detailDictionary(
        const std::string& name) const;

    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    std::shared_ptr<hammer::assets::StudioModelSystem> studioModels_;
    mutable hammer::assets::DetailObjectDictionary detailDictionary_;
    mutable std::string detailDictionaryName_;
    mutable bool detailDictionaryLoaded_{false};
};

} // namespace hammer::render
