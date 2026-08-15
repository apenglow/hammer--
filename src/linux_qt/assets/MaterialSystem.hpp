#pragma once

#include "GameFileSystem.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hammer::assets {

struct Image
{
    int width{0};
    int height{0};
    std::vector<std::uint32_t> pixels; // ARGB32, 0xAARRGGBB

    bool valid() const { return width > 0 && height > 0 && pixels.size() == static_cast<std::size_t>(width * height); }
    std::uint32_t sampleWrapped(double uPixels, double vPixels) const;
};

struct CubeImage
{
    // Source VTF cubemap face order: right (+X), left (-X), back (+Y),
    // front (-Y), up (+Z), down (-Z).
    std::array<Image, 6> faces;

    bool valid() const
    {
        return std::all_of(faces.begin(), faces.end(), [](const Image& face) {
            return face.valid();
        });
    }
};

struct Material
{
    std::string name;
    std::string shader;
    std::string baseTexture;
    std::string baseTexture2;
    Image image;
    Image image2;
    bool blended{false};
    // Source detail layer used by character, prop, and world materials.
    std::string detailTexture;
    Image detailImage;
    float detailScale{4.0f};
    float detailBlendFactor{1.0f};
    int detailBlendMode{0};
    bool hasDetailTexture{false};
    bool alphaTest{false};
    float alphaTestReference{0.5f};
    // Optional Source material effects used by the shaded hardware viewport.
    // These remain separate from water's distortion normal map.
    std::string bumpMap;
    Image bumpImage;
    // AnimatedTexture proxies can advance a multi-frame VTF normal map. All
    // decoded frames are retained so the renderer uploads them once.
    std::vector<Image> bumpFrames;
    float bumpAnimationFrameRate{0.0f};
    bool bumpMapped{false};
    bool phong{false};
    bool specular{false};
    // $envmap selects either the current map cubemap (env_cubemap) or an
    // authored cubemap VTF. Explicit VTFs are decoded once with the material
    // and uploaded/cached by the hardware renderer.
    std::string envMap;
    CubeImage envMapCube;
    bool envMapUsesMapCubemap{false};
    bool hasEnvMapCube{false};
    std::string envMapSource;
    std::string envMapError;
    bool selfIllum{false};
    // Source's self-illumination Fresnel path uses the geometric vertex normal,
    // base alpha, and [minimum maximum exponent] parameters.
    bool selfIllumFresnel{false};
    std::array<float, 3> selfIllumFresnelMinMaxExp{0.0f, 1.0f, 1.0f};
    bool rimLight{false};
    // VertexLitGeneric diffuse modulation and alpha-driven tint replacement.
    // $color2 is the shader's g_DiffuseModulation.rgb; when
    // $blendtintbybasealpha is enabled, base alpha selects the tint and
    // $blendtintcoloroverbase controls multiplication versus replacement.
    std::array<float, 3> color2{1.0f, 1.0f, 1.0f};
    // False unless a top-level $color2 is intentionally authored or a
    // supported proxy resolves it. This prevents unresolved TF2 runtime proxy
    // placeholders (commonly black) from multiplying an entire character.
    bool color2Active{false};
    bool blendTintByBaseAlpha{false};
    float blendTintColorOverBase{0.0f};
    // A small static material-proxy evaluator resolves the color chains used
    // by TF2 effect and paint materials for editor/model-browser previews.
    bool modelGlowProxy{false};
    bool itemTintProxy{false};
    bool invulnLevelProxy{false};
    // Dedicated TF2 invulnerability/Über material path. This is narrower than
    // highEnergyEffect so ordinary glowing and painted materials keep their
    // authored base-texture detail.
    bool uberEffect{false};
    bool highEnergyEffect{false};
    // The editor only approximates these effects for Source shaders whose
    // normal/specular inputs have compatible semantics. Special-purpose model
    // shaders fall back to the stable shaded-texture path.
    bool editorBumpMapSupported{false};
    bool editorPhongSupported{false};
    bool editorSpecularSupported{false};
    bool editorSelfIllumSupported{false};
    bool editorRimLightSupported{false};
    bool ssBump{false};
    bool phongMaskFromBaseAlpha{false};
    // Cubemap/specular masking is separate from the direct Phong mask.
    // Source's skin shader selects base alpha by default, or normal-map alpha
    // when $normalmapalphaenvmapmask is enabled. $invertphongmask only flips
    // the cubemap mask; it does not invert the direct local-light Phong mask.
    bool envMapMaskFromBaseAlpha{false};
    bool envMapMaskFromNormalAlpha{false};
    bool invertPhongMask{false};
    // Source model Phong controls. The normal-map alpha channel is the
    // per-pixel mask unless $basemapalphaphongmask selects base alpha.
    std::array<float, 3> phongFresnelRanges{0.0f, 0.5f, 1.0f};
    std::array<float, 3> phongTint{1.0f, 1.0f, 1.0f};
    bool phongTintDefined{false};
    std::array<float, 3> envMapTint{1.0f, 1.0f, 1.0f};
    // $envmapcontrast blends the reflection toward its own square (1 = fully
    // squared); $envmapsaturation blends it toward greyscale (0 = grey).
    float envMapContrast{0.0f};
    float envMapSaturation{1.0f};
    // Optional diffuse lighting warp used heavily by TF2 character/effect VMTs.
    std::string lightWarpTexture;
    Image lightWarpImage;
    bool hasLightWarpTexture{false};
    bool halfLambert{false};
    bool phongAlbedoTint{false};
    float phongExponent{150.0f};
    bool phongExponentOverride{false};
    float phongBoost{1.0f};
    std::string phongExponentTexture;
    Image phongExponentImage;
    bool hasPhongExponentTexture{false};
    float specularStrength{0.18f};
    // VertexLitGeneric self-illumination. Source uses base texture alpha as
    // the mask unless an explicit $selfillummask texture is supplied.
    std::array<float, 3> selfIllumTint{1.0f, 1.0f, 1.0f};
    std::string selfIllumMask;
    Image selfIllumMaskImage;
    bool hasSelfIllumMask{false};
    // VertexLitGeneric rim lighting is part of the Phong/skin path and only
    // activates when both $phong and $rimlight are enabled.
    float rimLightExponent{4.0f};
    float rimLightBoost{1.0f};
    bool rimMaskFromExponentAlpha{false};
    // Water keeps its actual normal/distortion texture separate from the
    // editor thumbnail. The GPU shader uses this map for animated ripples.
    Image waterNormalImage;
    // Flow maps must remain unmodified because red and green encode signed
    // flow vectors rather than tangent-space normals.
    Image waterFlowImage;
    std::string waterFlowMap;
    std::string waterFlowSource;
    bool translucent{false};
    // Top-level VMT $alpha modulation. Opaque VMTs still ignore incidental VTF alpha.
    float alpha{1.0f};
    // DecalModulate uses Source's multiplicative decal blend rather than
    // ordinary source-alpha compositing.
    bool decalModulate{false};
    // Source decals size their projected quad from the VTF dimensions times
    // the VMT $decalscale value. Ordinary materials keep the default 1.0.
    float decalScale{1.0f};
    bool water{false};
    // Tool/trigger materials marked with %compiletrigger must stay visible
    // from either side in the editor even though ordinary brushes are culled.
    bool compileTrigger{false};
    // %detailtype: names an object type in the map's detail.vbsp. Empty for
    // the overwhelming majority of materials - only ground/foliage surfaces
    // carry it.
    std::string detailType;
    bool previewAnimated{false};
    std::array<float, 3> waterFogColor{0.17f, 0.35f, 0.42f};
    std::array<float, 3> waterRefractTint{0.75f, 0.90f, 0.94f};
    std::array<float, 3> waterReflectTint{0.82f, 0.91f, 1.00f};
    // Source Water_DX90 / WaterCheap controls. Reflection uses the current
    // map skybox cubemap in the editor instead of the engine reflection RT.
    float waterFresnelReflectance{0.20f};
    float waterReflectAmount{0.80f};
    float waterRefractAmount{0.0f};
    float waterReflectBlendFactor{1.0f};
    float waterFogStart{0.0f};
    float waterFogEnd{0.0f};
    float waterAlpha{0.94f};
    float waterNormalScale{1.0f};
    std::array<float, 2> waterScale{1.0f, 1.0f};
    std::array<float, 2> waterScroll1{0.0f, 0.0f};
    std::array<float, 2> waterScroll2{0.0f, 0.0f};
    bool waterNoFresnel{false};
    bool waterMultiTexture{false};
    bool waterHasFlowMap{false};
    float waterFlowCycleRate{1.0f};
    float waterFlowDistance{0.10f};
    float waterFlowMapScale{1.0f};
    float waterFlowNormalUvScale{1.0f};
    bool missing{false};
    std::string note;
    std::string vmtSource;
    std::string vtfSource;
    std::string error;
};

// Resolve the static team-colour approximation used by TF2 invulnerability
// materials. Runtime ModelGlowColor proxies do not expose team state to the
// editor, so neutral/white proxy results fall back to the selected model skin
// (even skins RED, odd skins BLU). Authored chromatic tints remain untouched.
std::array<float, 3> previewUberColor(const Material& material, int skin);

class MaterialSystem
{
public:
    explicit MaterialSystem(std::shared_ptr<GameFileSystem> fileSystem);

    std::shared_ptr<const Material> material(std::string_view name);

    // Thumbnail-quality load: every texture is decoded from the smallest stored
    // mip that still covers `maxDimension`, and the result lives in a cache
    // separate from the full-quality one. Keeping them apart matters - the 3D
    // renderer shares this MaterialSystem, and must never be handed a
    // downscaled material just because the browser asked for one first.
    std::shared_ptr<const Material> previewMaterial(std::string_view name,
                                                    int maxDimension);
    // Cache-only lookup: returns a material ONLY if it is already resident,
    // never reading a VMT or decoding a VTF. Auto-VisGroup classification runs
    // over every face of every solid on each scene rebuild, so it must not be
    // able to trigger a load storm on a map with thousands of materials.
    std::shared_ptr<const Material> residentMaterial(std::string_view name) const;

    std::vector<std::string> materialNames() const;
    std::shared_ptr<GameFileSystem> fileSystem() const { return fileSystem_; }
    void clearCache();

    // Drops every cached material that nothing else still references, and
    // returns the bytes reclaimed. Materials the 3D scene is drawing are held by
    // its batches and so survive; stale thumbnail entries from a previous
    // preview size do not. Safe to call at any time - it can only free entries
    // that no live code path is reading.
    std::size_t purgeUnusedMaterials();

    // Retunes the preview cache for a new thumbnail size, returning bytes
    // reclaimed. Stepping down re-derives the smaller previews by downscaling
    // what is already in memory (no VTF re-read) and drops the originals;
    // stepping up discards the too-small entries so the new size is decoded from
    // the right mip. Materials the 3D scene still references are left at their
    // existing, highest quality in every case.
    std::size_t adjustPreviewCache(int maxDimension);

    // `maxDimension` caps the decoded edge length by choosing a smaller stored
    // mip instead of the full surface. Thumbnails pass their pixel size here;
    // 0 decodes at native resolution.
    static std::optional<Image> decodeVtf(const std::vector<std::uint8_t>& bytes,
                                          std::string* error = nullptr,
                                          int maxDimension = 0);
    static std::optional<std::vector<Image>> decodeVtfFrames(
        const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);
    static std::optional<CubeImage> decodeVtfCubemap(
        const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);

private:
    std::shared_ptr<Material> loadMaterial(std::string normalizedName,
                                           int maxDimension = 0);
    std::shared_ptr<Material> missingMaterial(std::string normalizedName);

    std::shared_ptr<GameFileSystem> fileSystem_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Material>> cache_;
    // Keyed by "<name>@<maxDimension>" so different thumbnail sizes coexist.
    std::unordered_map<std::string, std::shared_ptr<Material>> previewCache_;
};

} // namespace hammer::assets
