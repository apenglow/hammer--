#include "EnvCubemap.hpp"
#include "RayTracingScene.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "ray tracing scene test failed: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using namespace hammer::render;
    require(sizeof(RayTracingVertex) == 80, "std430 vertex size");
    require(sizeof(RayTracingTriangle) == 16, "std430 triangle size");
    require((sizeof(RayTracingMaterial) % 16) == 0, "std430 material alignment");
    require(sizeof(RayTracingMaterial) == 480, "std430 material size with transparency controls");
    require((RtPhong & RtWater) == 0, "material flags are independent");
    require((RtHasLightWarp & RtHasBumpMap) == 0, "lightwarp and bump flags are independent");
    require((Rt2HasDetailTexture & Rt2AlphaTest) == 0, "detail and alpha-test flags are independent");
    RayTracingAtlas atlas;
    require(!atlas.valid(), "empty atlas is invalid");

    // env_cubemap "cubemapsize" is a choices index, not a pixel count.
    require(cubemapFaceSizeFromKeyValue(0) == 32, "cubemapsize 0 is the 32x32 default");
    require(cubemapFaceSizeFromKeyValue(1) == 1, "cubemapsize 1 is 1x1");
    require(cubemapFaceSizeFromKeyValue(6) == 32, "cubemapsize 6 is 32x32");
    require(cubemapFaceSizeFromKeyValue(9) == 256, "cubemapsize 9 is 256x256");
    require(cubemapFaceSizeFromKeyValue(99) == 256, "oversized cubemapsize clamps");

    hammer::vmf::Scene cubemapScene;
    hammer::vmf::EntityMarker probeEntity;
    probeEntity.classname = "env_cubemap";
    probeEntity.origin = {64.0, -32.0, 16.0};
    probeEntity.properties.push_back({"cubemapsize", "7"});
    cubemapScene.entities.push_back(probeEntity);
    hammer::vmf::EntityMarker otherEntity;
    otherEntity.classname = "light";
    cubemapScene.entities.push_back(otherEntity);

    const auto probes = collectCubemapProbes(cubemapScene);
    require(probes.size() == 1, "only env_cubemap entities become probes");
    require(probes[0].size == 64, "cubemapsize 7 is 64x64");
    require(probes[0].origin.x == 64.0, "probe keeps its entity origin");

    // Nearest-probe selection is what assigns a cubemap to a surface.
    std::vector<BakedCubemap> baked;
    require(nearestCubemapIndex(baked, {0.0, 0.0, 0.0}) == -1, "no bake means no probe");
    baked.push_back({{0.0, 0.0, 0.0}, {}});
    baked.push_back({{100.0, 0.0, 0.0}, {}});
    require(nearestCubemapIndex(baked, {10.0, 0.0, 0.0}) == 0, "nearest probe wins");
    require(nearestCubemapIndex(baked, {90.0, 0.0, 0.0}) == 1, "nearest probe wins on the far side");
    // ---------------------------------------------------------------------
    // VRAD light entity parsing (CreateDirectLights / LightForString).
    // ---------------------------------------------------------------------
    const auto lightScene = [](std::vector<std::pair<std::string, std::string>> properties,
                               std::string classname = "light") {
        hammer::vmf::Scene scene;
        hammer::vmf::EntityMarker entity;
        entity.classname = std::move(classname);
        entity.properties = std::move(properties);
        scene.entities.push_back(entity);
        return scene;
    };
    const auto nearlyEqual = [](float a, float b) { return std::abs(a - b) < 1e-4f; };

    // LightForString converts with pow(x/255, 2.2), not the sRGB piecewise
    // curve. Getting this wrong shifts every light in the map.
    {
        const auto lights = mapLightsForScene(lightScene({{"_light", "128 128 128 255"}}));
        // A scene with no light_environment also gets the fallback sun, so the
        // entity's own light is simply the first record.
        require(lights.size() == 2, "a lone light entity plus the fallback sun");
        require(nearlyEqual(lights[0].colorIntensity[0],
                            static_cast<float>(std::pow(128.0 / 255.0, 2.2))),
                "_light uses VRAD's 2.2 gamma, not sRGB");
        require(nearlyEqual(lights[0].colorIntensity[3], 1.0f),
                "a brightness of 255 is unit intensity");
    }

    // A single value is a greyscale light, and a three-value string carries no
    // brightness scaler at all.
    {
        const auto grey = mapLightsForScene(lightScene({{"_light", "200"}}));
        require(nearlyEqual(grey[0].colorIntensity[0], grey[0].colorIntensity[2]),
                "a one-value _light is greyscale");
        const auto triple = mapLightsForScene(lightScene({{"_light", "255 255 255"}}));
        require(nearlyEqual(triple[0].colorIntensity[3], 1.0f),
                "a three-value _light has no brightness scaler");
    }

    // Eight values are two 4-tuples, LDR then HDR. The preview is HDR-only, so
    // the second tuple has to win.
    {
        const auto lights = mapLightsForScene(
            lightScene({{"_light", "255 0 0 100 0 255 0 200"}}));
        require(lights[0].colorIntensity[1] > lights[0].colorIntensity[0],
                "the HDR half of an eight-value _light wins");
        require(nearlyEqual(lights[0].colorIntensity[3], 200.0f / 255.0f),
                "the HDR brightness wins too");
    }

    // _lightHDR overrides _light outright; _lightscaleHDR then scales it.
    {
        const auto lights = mapLightsForScene(lightScene({{"_light", "255 255 255 100"},
                                                          {"_lightHDR", "255 255 255 200"},
                                                          {"_lightscaleHDR", "2"}}));
        require(nearlyEqual(lights[0].colorIntensity[3], 2.0f * 200.0f / 255.0f),
                "_lightHDR replaces _light and _lightscaleHDR scales the result");
    }
    {
        // A -1 sentinel tuple is not a usable colour, so _light stands.
        const auto lights = mapLightsForScene(lightScene({{"_light", "255 255 255 100"},
                                                          {"_lightHDR", "-1 -1 -1 1"}}));
        require(nearlyEqual(lights[0].colorIntensity[3], 100.0f / 255.0f),
                "an unusable _lightHDR falls back to _light");
    }

    // light_environment always emits an ambient light. When _ambient is absent
    // VRAD uses half the sun's own intensity rather than nothing.
    {
        const auto lights = mapLightsForScene(
            lightScene({{"_light", "255 255 255 200"}, {"pitch", "-45"}}, "light_environment"));
        require(lights.size() == 2, "light_environment emits a sun and an ambient");
        require(nearlyEqual(lights[1].positionType[3], 3.0f), "the second light is the ambient");
        require(nearlyEqual(lights[1].colorIntensity[3], 0.5f * 200.0f / 255.0f),
                "an absent _ambient defaults to half the sun");
    }

    // SunSpreadAngle is stored as its sine, and it is what makes a sun shadow's
    // edge soften with distance instead of staying hard.
    {
        const auto hard = mapLightsForScene(
            lightScene({{"_light", "255 255 255 200"}}, "light_environment"));
        require(nearlyEqual(hard[0].innerControls[2], 0.0f),
                "no SunSpreadAngle means a point sun");
        const auto soft = mapLightsForScene(
            lightScene({{"_light", "255 255 255 200"}, {"SunSpreadAngle", "5"}},
                       "light_environment"));
        require(nearlyEqual(soft[0].innerControls[2],
                            static_cast<float>(std::sin(5.0 * 3.14159265358979323846 / 180.0))),
                "SunSpreadAngle is stored as its sine");
    }

    // Old-style attenuation pre-scales intensity by the falloff at 100 units.
    {
        const auto lights = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                          {"_quadratic_attn", "1"}}));
        require(nearlyEqual(lights[0].attenuationOuter[2], 1.0f), "quadratic term survives");
        require(nearlyEqual(lights[0].colorIntensity[3], 10000.0f),
                "old-style attenuation scales intensity for unit 100 distance");
    }

    // _fifty_percent_distance takes the other branch entirely: a solved
    // quadratic, no unit-100 pre-scale, and a cap distance.
    {
        const auto lights = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                          {"_fifty_percent_distance", "100"},
                                                          {"_zero_percent_distance", "400"}}));
        require(nearlyEqual(lights[0].colorIntensity[3], 1.0f),
                "_fifty_percent_distance skips the unit-100 pre-scale");
        const float c = lights[0].attenuationOuter[0];
        const float l = lights[0].attenuationOuter[1];
        const float q = lights[0].attenuationOuter[2];
        // VRAD rescales the solved curve so the authored 50% distance lands on
        // a falloff of exactly 0.5, even after the monotonicity fixup has moved
        // the middle sample. That absolute value, not the ratio against d=0, is
        // what the key means.
        const float atFifty = 1.0f / (c + l * 100.0f + q * 100.0f * 100.0f);
        require(std::abs(atFifty - 0.5f) < 1e-3f,
                "the solved curve is exactly half-bright at the 50% distance");
        const float atZero = 1.0f / c;
        const float atFar = 1.0f / (c + l * 400.0f + q * 400.0f * 400.0f);
        require(atZero > atFifty && atFifty > atFar, "the solved curve falls off monotonically");
    }

    // "Maximum Distance" is non-functional in Source 2013: VRAD copies it into
    // the worldlights lump and never reads it back, so it must not clip the
    // light in the preview either.
    {
        const auto lights = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                          {"_distance", "128"}}));
        require(nearlyEqual(lights[0].directionRange[3], 0.0f),
                "_distance does not become a range cutoff");
    }

    // The same HDR scale field is spelled two ways in the wild.
    {
        const auto documented = mapLightsForScene(lightScene({{"_light", "255 255 255 100"},
                                                              {"_lightHDRscale", "3"}}));
        require(nearlyEqual(documented[0].colorIntensity[3], 3.0f * 100.0f / 255.0f),
                "_lightHDRscale is honoured");
        // When both are present VRAD's own spelling wins.
        const auto both = mapLightsForScene(lightScene({{"_light", "255 255 255 100"},
                                                        {"_lightscaleHDR", "2"},
                                                        {"_lightHDRscale", "3"}}));
        require(nearlyEqual(both[0].colorIntensity[3], 2.0f * 100.0f / 255.0f),
                "_lightscaleHDR wins when both spellings are present");
    }

    // A negative HDR scale is documented as subtracting light, so the sign has
    // to survive into the GPU record rather than being clamped away.
    {
        const auto lights = mapLightsForScene(lightScene({{"_light", "255 255 255 100"},
                                                          {"_lightscaleHDR", "-1"}}));
        require(lights[0].colorIntensity[3] < 0.0f,
                "a negative BrightnessScaleHDR stays negative");
    }

    // Appearance and Custom Appearance scale brightness: a is 0%, m is 100%.
    {
        const auto off = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                       {"pattern", "a"}}));
        require(nearlyEqual(off[0].colorIntensity[3], 0.0f),
                "a light patterned fully dark contributes nothing");
        const auto on = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                      {"pattern", "m"}}));
        require(nearlyEqual(on[0].colorIntensity[3], 1.0f),
                "'m' is exactly full brightness");
        // Style 0 is "Normal" and must not change anything.
        const auto normal = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                          {"style", "0"}}));
        require(nearlyEqual(normal[0].colorIntensity[3], 1.0f),
                "the Normal lightstyle leaves brightness alone");
        // A strobe averages back to roughly full brightness rather than to
        // whichever letter happens to come first.
        const auto strobe = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                          {"style", "9"}}));
        require(strobe[0].colorIntensity[3] > 0.9f,
                "an animated preset does not read as an unlit light");
        // A pattern overrides the preset.
        const auto overridden = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                              {"style", "9"},
                                                              {"pattern", "a"}}));
        require(nearlyEqual(overridden[0].colorIntensity[3], 0.0f),
                "Custom Appearance overrides the Appearance preset");
    }

    // The View > HDR toggle selects which entity keys apply, exactly as VRAD's
    // compile mode does. In LDR the HDR-only keys must be ignored outright.
    {
        const auto scene = lightScene({{"_light", "255 0 0 100 0 255 0 200"},
                                       {"_lightHDR", "0 0 255 250"},
                                       {"_lightscaleHDR", "4"}});
        const auto hdr = mapLightsForScene(scene, true);
        require(hdr[0].colorIntensity[2] > hdr[0].colorIntensity[0],
                "HDR mode takes _lightHDR");
        require(nearlyEqual(hdr[0].colorIntensity[3], 4.0f * 250.0f / 255.0f),
                "HDR mode applies the HDR scale");

        const auto ldr = mapLightsForScene(scene, false);
        require(ldr[0].colorIntensity[0] > ldr[0].colorIntensity[1] &&
                    ldr[0].colorIntensity[0] > ldr[0].colorIntensity[2],
                "LDR mode ignores _lightHDR and takes the LDR tuple of _light");
        require(nearlyEqual(ldr[0].colorIntensity[3], 100.0f / 255.0f),
                "LDR mode ignores the HDR brightness and scale");
    }

    // _exponent shapes the spot cone ramp; it is zero for anything else.
    {
        const auto spot = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                        {"_exponent", "3"}}, "light_spot"));
        require(nearlyEqual(spot[0].innerControls[1], 3.0f), "light_spot keeps _exponent");
        const auto point = mapLightsForScene(lightScene({{"_light", "255 255 255 255"},
                                                         {"_exponent", "3"}}));
        require(nearlyEqual(point[0].innerControls[1], 0.0f), "a point light has no cone ramp");
    }

    // ParseLightSpot's cone defaults are VRAD's, not the FGD's: an absent or
    // zero _inner_cone is 10 degrees and an absent or zero _cone matches it.
    {
        const auto bare = mapLightsForScene(
            lightScene({{"_light", "255 255 255 255"}}, "light_spot"));
        const float cos10 = std::cos(10.0f * 3.14159265358979323846f / 180.0f);
        require(nearlyEqual(bare[0].innerControls[0], cos10),
                "an unset _inner_cone is 10 degrees");
        require(nearlyEqual(bare[0].attenuationOuter[3], cos10),
                "an unset _cone matches the inner cone");
        const auto zeroed = mapLightsForScene(
            lightScene({{"_light", "255 255 255 255"}, {"_inner_cone", "0"}, {"_cone", "0"}},
                       "light_spot"));
        require(nearlyEqual(zeroed[0].innerControls[0], cos10),
                "an explicit zero takes the same default");
        // The outer cone is widened to the inner one, never narrower.
        const auto inverted = mapLightsForScene(
            lightScene({{"_light", "255 255 255 255"}, {"_inner_cone", "45"}, {"_cone", "20"}},
                       "light_spot"));
        require(nearlyEqual(inverted[0].attenuationOuter[3], inverted[0].innerControls[0]),
                "a _cone narrower than the inner cone is widened to it");
        // "This is a point light if stop dots are 180."
        const auto demoted = mapLightsForScene(
            lightScene({{"_light", "255 255 255 255"}, {"_inner_cone", "180"}, {"_cone", "180"},
                        {"_exponent", "3"}},
                       "light_spot"));
        require(nearlyEqual(demoted[0].positionType[3], 0.0f),
                "a 180/180 light_spot becomes a point light");
        require(nearlyEqual(demoted[0].innerControls[1], 0.0f),
                "the demoted light drops its exponent");
        // Angles above 90 are clamped, exactly as VRAD warns and does.
        const auto wide = mapLightsForScene(
            lightScene({{"_light", "255 255 255 255"}, {"_cone", "120"}}, "light_spot"));
        require(nearlyEqual(wide[0].attenuationOuter[3], 0.0f),
                "a cone wider than 90 degrees is clamped to 90");
    }

    // ParseLightGeneric aims a light at its "target" entity and ignores the
    // angles keys entirely when one is present.
    {
        hammer::vmf::Scene scene;
        hammer::vmf::EntityMarker spot;
        spot.id = 1;
        spot.classname = "light_spot";
        spot.origin = {0.0, 0.0, 100.0};
        spot.angles = {90.0, 0.0, 0.0};  // straight up, if angles were used
        spot.properties = {{"_light", "255 255 255 255"}, {"target", "aim"}};
        scene.entities.push_back(spot);
        hammer::vmf::EntityMarker aim;
        aim.id = 2;
        aim.classname = "info_target";
        aim.targetName = "aim";
        aim.origin = {0.0, 0.0, 0.0};
        scene.entities.push_back(aim);

        const auto lights = mapLightsForScene(scene);
        require(nearlyEqual(lights[0].directionRange[2], -1.0f),
                "a targeted light points at its target, not along its angles");

        // A target that names nothing falls back to the angles direction.
        scene.entities.pop_back();
        const auto orphaned = mapLightsForScene(scene);
        require(nearlyEqual(orphaned[0].directionRange[2], 1.0f),
                "a missing target leaves the angles direction in place");
    }

    // ---------------------------------------------------------------------
    // Bump-map gating. These have to agree with Hardware3DViewport's `hasBump`
    // or the same material shades differently in the two viewports.
    // ---------------------------------------------------------------------
    {
        const auto bumped = [] {
            hammer::assets::Material material;
            material.bumpMapped = true;
            material.bumpImage = hammer::assets::Image{1, 1, std::vector<std::uint32_t>{0xffff8080u}};
            material.editorBumpMapSupported = true;
            return material;
        };

        require((materialFlagsForMaterial(bumped()) & RtHasBumpMap) != 0,
                "an ordinary bumped material is bump mapped");

        // $ssbump stores self-shadowed radiosity-basis coefficients, not a
        // tangent-space normal. Decoding it as one produces a garbage normal,
        // so both viewports must decline to bump it.
        hammer::assets::Material selfShadowed = bumped();
        selfShadowed.ssBump = true;
        require((materialFlagsForMaterial(selfShadowed) & RtHasBumpMap) == 0,
                "$ssbump is not decoded as a tangent-space normal map");

        // A shader with no bump stage ignores $bumpmap in the engine.
        hammer::assets::Material unsupported = bumped();
        unsupported.editorBumpMapSupported = false;
        require((materialFlagsForMaterial(unsupported) & RtHasBumpMap) == 0,
                "a shader without a bump stage is not bump mapped");

        hammer::assets::Material noTexture = bumped();
        noTexture.bumpImage = {};
        require((materialFlagsForMaterial(noTexture) & RtHasBumpMap) == 0,
                "a missing bump texture is not bump mapped");
    }

    // ---------------------------------------------------------------------
    // Tool-material occlusion. These decide whether a shadow ray is stopped at
    // all, so getting them wrong darkens a map with invisible geometry.
    // ---------------------------------------------------------------------
    {
        // Light passes straight through nodraw. Ordinary brushes carry nodraw
        // on every face that is never seen, so occluding with it means hidden
        // geometry casts shadows into rooms.
        require((shadowFlagsForMaterialName("tools/toolsnodraw") & TriangleNoShadow) != 0,
                "tools/toolsnodraw does not cast a shadow");

        // toolsblack absorbs everything reaching it.
        require((shadowFlagsForMaterialName("tools/toolsblack") & TriangleNoShadow) == 0,
                "tools/toolsblack casts a shadow");

        // Other tool helpers stay non-occluding.
        require((shadowFlagsForMaterialName("tools/toolstrigger") & TriangleNoShadow) != 0,
                "tool helpers do not cast shadows");
        // The sky tool carries NoShadow like any other helper; the shader makes
        // its own exception for sky portals when a sun ray needs to escape
        // (TriangleSky is added separately, by the scene builder).
        require((shadowFlagsForMaterialName("tools/toolsskybox") & TriangleNoShadow) != 0,
                "the sky tool material does not occlude ordinary light");

        // A world material is unaffected by any of this.
        require(shadowFlagsForMaterialName("brick/brickwall014b") == 0,
                "ordinary world materials carry no tool flags");
    }

    // ---------------------------------------------------------------------
    // Editor helper models. Anything under models/editor is a mapper's handle,
    // not map geometry, so it neither blocks nor receives light. This replaced
    // a hardcoded classname list, so the point of these is that helpers the
    // list never named are covered now.
    // ---------------------------------------------------------------------
    {
        require(isEditorHelperModelPath("models/editor/spotlight.mdl"),
                "the light_spot helper is a helper");
        require(isEditorHelperModelPath("models/editor/info_target.mdl"),
                "info_target's helper is one too, though no classname list named it");
        // FGDs are written by hand; case and separators vary.
        require(isEditorHelperModelPath("models\\editor\\axis_helper.mdl"),
                "backslash-separated FGD paths still resolve");
        require(isEditorHelperModelPath("Models/Editor/Cone_Helper.mdl"),
                "mixed-case FGD paths still resolve");
        require(isEditorHelperModelPath("editor/spotlight.mdl"),
                "a path without the models/ prefix still resolves");

        // Real map geometry must keep blocking and receiving light.
        require(!isEditorHelperModelPath("models/props_c17/oildrum001.mdl"),
                "an ordinary prop is not a helper");
        require(!isEditorHelperModelPath("models/editorprops/crate.mdl"),
                "a directory that merely starts with 'editor' is not models/editor");
        require(!isEditorHelperModelPath(""),
                "an entity with no model is not a helper");
    }

    // ---------------------------------------------------------------------
    // Radiosity patch grid. The bounce solve resolves a ray hit back to a patch
    // through triangle.data[2], so a patch grid that does not line up with the
    // triangles that reference it produces a solve of exactly zero.
    // ---------------------------------------------------------------------
    {
        // A closed box of lit faces.
        hammer::vmf::Scene boxScene;
        hammer::vmf::BrushGeometry brush;
        brush.id = 1;
        const double lo = 0.0, hi = 256.0;
        const std::array<hammer::vmf::Vec3, 8> corners{{
            {lo, lo, lo}, {hi, lo, lo}, {hi, hi, lo}, {lo, hi, lo},
            {lo, lo, hi}, {hi, lo, hi}, {hi, hi, hi}, {lo, hi, hi}}};
        for (const auto& corner : corners) brush.vertices.push_back(corner);
        const std::array<std::array<std::size_t, 4>, 6> faceCorners{{
            {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
            {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 3, 7, 4}}};
        const std::array<hammer::vmf::Vec3, 6> normals{{
            {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}}};
        for (std::size_t face = 0; face < faceCorners.size(); ++face) {
            hammer::vmf::FaceGeometry geometry;
            geometry.sideId = static_cast<int>(face) + 1;
            geometry.material = "brick/brickwall014b";
            geometry.normal = normals[face];
            geometry.lightmapScale = 16;
            // Texture axes perpendicular to the face normal.
            const hammer::vmf::Vec3 n = normals[face];
            const hammer::vmf::Vec3 up = std::abs(n.z) > 0.5
                ? hammer::vmf::Vec3{1, 0, 0} : hammer::vmf::Vec3{0, 0, 1};
            geometry.uAxis.direction = {up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z,
                                        up.x * n.y - up.y * n.x};
            geometry.uAxis.scale = 0.25;
            geometry.vAxis.direction = up;
            geometry.vAxis.scale = 0.25;
            for (std::size_t corner : faceCorners[face]) geometry.vertices.push_back(corner);
            brush.faces.push_back(geometry);
        }
        boxScene.brushes.push_back(brush);

        hammer::render::LightmapLayoutOptions layoutOptions;
        layoutOptions.faceIsLit = [](std::string_view) { return true; };
        const auto patchData = hammer::render::buildRadiosityPatchData(boxScene, layoutOptions);
        require(patchData.patchesValid(),
                std::string("a lit box produces a patch grid: " + patchData.status).c_str());
        require(patchData.luxels.empty(),
                "the patch-only build skips the expensive luxel grid");
        require(patchData.patchRects.records.size() == 6,
                "every lit face gets a patch rect");
        // Each face must be reachable by the faceKey the scene builder uses.
        for (int side = 1; side <= 6; ++side) {
            require(patchData.patchRects.indices.count(hammer::render::faceKey(1, side)) == 1,
                    "each face is addressable by faceKey(brushId, sideId)");
        }
        // The grid is what turns a ray hit back into a patch; it must actually
        // reference patches, not be uniformly empty.
        // origin.w must carry the face's luxel scale, not the coarser patch
        // scale: the preview derives the luxel footprint it supersamples the
        // shadow ray across from exactly this value.
        for (const auto& record : patchData.patchRects.records) {
            require(record.origin[3] > 0.0f, "every rect carries a luxel scale");
            // Equal at kPatchScaleMultiplier 1, where the patch grid sits on
            // the luxel grid itself. Coarser is allowed, finer never is.
            require(record.origin[3] <= record.origin[2],
                    "the luxel scale is never coarser than the patch scale");
            // The ratio is the face's chop in luxel widths: -maxchop for brush
            // faces, -dispchop for displacements. This room has no
            // displacements, but the invariant is "one of the two", not "always
            // maxchop", and MAX_PATCHES coarsening scales both together.
            const float chop = record.origin[2] / record.origin[3];
            require(std::abs(chop - float(hammer::render::kPatchScaleMultiplier)) < 0.01f ||
                        std::abs(chop -
                                 float(hammer::render::kDisplacementPatchScaleMultiplier)) < 0.01f,
                    "the patch grid is chopped at maxchop or dispchop luxel widths");
        }

        std::size_t occupied = 0;
        for (std::uint32_t entry : patchData.patchIndexGrid)
            if (entry != 0u) ++occupied;
        require(occupied == patchData.patches.size(),
                "every patch is reachable through the patch index grid");
    }

    std::cout << "ray tracing scene layout, env_cubemap probe, VRAD light, bump, "
                 "tool occlusion, editor-helper and radiosity patch tests passed\n";
    return 0;
}
