// Shared bindings and VRAD lighting math for the radiosity bake passes.
//
// This is utils/vrad's model running against the ray-query acceleration
// structure the preview already builds, rather than against a compiled BSP's
// PVS. Everything here stays in linear float RGB: HDR mode is the only mode.
//
// All six rad_*.comp passes share this one descriptor set layout.

#ifndef RAD_COMMON_GLSL
#define RAD_COMMON_GLSL

const uint RT_HAS_SECONDARY             = 1u << 0;
const uint RT_TRANSLUCENT               = 1u << 12;
const uint RT_WATER                     = 1u << 11;
const uint RT2_ALPHA_TEST               = 1u << 1;

// These MUST match RayTracingTriangleFlag in app/RayTracingScene.hpp: the bake
// and the live preview read the same triangle buffer, so a bit that means
// "editor helper" to one and "static prop" to the other silently corrupts both.
const uint TRIANGLE_TWO_SIDED           = 1u << 0;
const uint TRIANGLE_NO_SHADOW           = 1u << 2;
const uint TRIANGLE_SKY                 = 1u << 4;
const uint TRIANGLE_SPRITE              = 1u << 5;
const uint TRIANGLE_UNLIT               = 1u << 6;
const uint TRIANGLE_STATIC_PROP         = 1u << 7;
const uint TRIANGLE_HAS_LIGHTMAP        = 1u << 8;

// LightRecord.positionType.w, matching appendMapLights in RayTracingScene.cpp.
const float RAD_EMIT_POINT       = 0.0;
const float RAD_EMIT_SPOTLIGHT   = 1.0;
const float RAD_EMIT_DIRECTIONAL = 2.0;
const float RAD_EMIT_SKYAMBIENT  = 3.0;
const float RAD_EMIT_ENVIRONMENT = 4.0;

struct VertexRecord {
    vec4 position;
    vec4 normal;
    vec4 tangent;
    vec4 texCoord;
    vec4 surface;
};

struct TriangleRecord {
    // x = material index, y = flags, z = rect-table index,
    // w = static-prop ambient-cube slot or 0xffffffff.
    uvec4 data;
};

// Layout-identical to hammer::render::RayTracingMaterial. The bake only reads a
// few fields, but the record has to be declared in full so the std430 stride
// matches the buffer the preview already uploads.
struct MaterialRecord {
    vec4 baseRect;
    vec4 secondaryRect;
    vec4 detailRect;
    vec4 bumpRect;
    vec4 exponentRect;
    vec4 selfIllumRect;
    vec4 lightWarpRect;
    vec4 flowRect;
    vec4 environmentRects[6];
    vec4 color2;
    vec4 phongTintExponent;
    vec4 envTintBoost;
    vec4 selfIllumTintSpecular;
    vec4 phongFresnelRimExponent;
    vec4 selfIllumFresnelRimBoost;
    vec4 detailAlphaControls;
    vec4 waterFogAlpha;
    vec4 waterRefractAmount;
    vec4 waterReflectAmount;
    vec4 waterControls;
    vec4 waterScaleScroll1;
    vec4 waterScroll2NormalCycle;
    vec4 waterFlow;
    vec4 transparencyControls;
    uvec4 flags;
};

struct LightRecord {
    vec4 positionType;
    vec4 directionRange;
    vec4 colorIntensity;
    vec4 attenuationOuter;
    vec4 innerControls;
    // x = m_flCapDist, y = m_flStartFadeDistance, z = m_flEndFadeDistance.
    vec4 fadeControls;
};

// VRAD's point/spot distance falloff, shared by every path that shades a map
// light so the luxel grid, the patch grid and the prop cubes cannot disagree.
//
// Two details here are easy to get wrong and both change where a light stops:
// the distance fed to the quadratic is clamped to m_flCapDist (past the
// curve's minimum an extreme falloff would otherwise start brightening again),
// and the fade between m_flStartFadeDistance and m_flEndFadeDistance is a
// separate linear term applied on top.
float radDistanceFalloff(LightRecord light, float distance) {
    // dist = max(dist, 1) BEFORE the cap. The floor is not cosmetic: with
    // old-style attenuation the light's intensity has already been pre-scaled
    // by (c + 100l + 10000q), so without it a sample a fraction of a unit from
    // a quadratic light evaluates 1/(q*d*d) against that scale and blows out.
    float evaluated = max(distance, 1.0);
    float capped = min(evaluated, max(light.fadeControls.x, 1.0));
    float c = light.attenuationOuter.x;
    float l = light.attenuationOuter.y;
    float q = light.attenuationOuter.z;
    float denominator = c + l * capped + q * capped * capped;
    if (denominator <= 1e-6) return 0.0;
    float falloff = 1.0 / denominator;

    // The hard-falloff fade is a quintic ease, not a linear ramp, so a light
    // that fades to zero does not show a crease where the fade begins.
    float startFade = light.fadeControls.y;
    float endFade = light.fadeControls.z;
    if (endFade > startFade) {
        if (evaluated >= endFade) return 0.0;
        float t = clamp((evaluated - startFade) / max(endFade - startFade, 1e-6), 0.0, 1.0);
        t = 1.0 - t;
        falloff *= t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    }
    return falloff;
}

// The spotlight cone ramp. VRAD raises the normalised ramp to _exponent, with
// 0 and 1 both meaning "linear".
//
// GatherSampleLightSSE's emit_spotlight case also multiplies the falloff by
// dot2 itself - the cosine between the spot's axis and the direction to the
// sample - on top of the cone ramp. That cosine applies across the whole cone,
// not just the fringe, so leaving it out made a spot's pool of light flat and
// too bright away from its axis, and the wider the cone the further off it was.
float radSpotFalloff(LightRecord light, vec3 toLight) {
    float stopdot2 = light.attenuationOuter.w;  // cos(_cone)
    float stopdot = light.innerControls.x;      // cos(_inner_cone)
    float dot2 = dot(-toLight, normalize(light.directionRange.xyz));
    if (dot2 <= stopdot2) return 0.0;
    if (dot2 >= stopdot) return dot2;
    float ramp = clamp((dot2 - stopdot2) / max(stopdot - stopdot2, 1e-6), 0.0, 1.0);
    float exponent = light.innerControls.y;
    if (exponent != 0.0 && exponent != 1.0) ramp = pow(ramp, exponent);
    return ramp * dot2;
}

// One VRAD sample point: the output of BuildFacesamples().
struct LuxelRecord {
    vec4 position;
    vec4 normal;
    // xy = atlas texel, z = inside-winding flag, w = rect index.
    vec4 atlas;
};

struct RectRecord {
    vec4 rect;      // x, y, width, height in luxels
    vec4 origin;    // originS, originT, lightmapScale
    vec4 uAxis;     // xyz direction, w shift
    vec4 vAxis;     // xyz direction, w shift
    uvec4 identity; // x = material index
};

// transfer_t: VRAD stores {int patch; float transfer;}.
struct TransferRecord {
    uint target;
    float weight;
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT sceneAccelerationStructure;
layout(std430, set = 0, binding = 1) readonly buffer RadVertexBuffer { VertexRecord vertices[]; };
layout(std430, set = 0, binding = 2) readonly buffer RadIndexBuffer { uint indices[]; };
layout(std430, set = 0, binding = 3) readonly buffer RadTriangleBuffer { TriangleRecord triangles[]; };
layout(std430, set = 0, binding = 4) readonly buffer RadMaterialBuffer { MaterialRecord materials[]; };
layout(set = 0, binding = 5) uniform sampler2DArray materialAtlas;
layout(std430, set = 0, binding = 6) readonly buffer RadLightBuffer { LightRecord mapLights[]; };
layout(std430, set = 0, binding = 7) readonly buffer RadLuxelBuffer { LuxelRecord luxels[]; };
layout(std430, set = 0, binding = 8) readonly buffer RadPatchBuffer { LuxelRecord patches[]; };
layout(std430, set = 0, binding = 9) readonly buffer RadLuxelRectBuffer { RectRecord luxelRects[]; };
layout(std430, set = 0, binding = 10) readonly buffer RadPatchRectBuffer { RectRecord patchRects[]; };
layout(std430, set = 0, binding = 11) buffer RadTransferBuffer { TransferRecord transfers[]; };
layout(std430, set = 0, binding = 12) buffer RadPatchDirect { vec4 patchDirect[]; };
layout(std430, set = 0, binding = 13) buffer RadPatchReflectivity { vec4 patchReflectivity[]; };
layout(std430, set = 0, binding = 14) buffer RadPatchBounceA { vec4 patchBounceA[]; };
layout(std430, set = 0, binding = 15) buffer RadPatchBounceB { vec4 patchBounceB[]; };
layout(std430, set = 0, binding = 16) buffer RadPatchTotal { vec4 patchTotal[]; };
layout(set = 0, binding = 17, rgba32f) uniform image2D lightmapImage;
// Patch index + 1 per texel of the patch atlas, 0 where no patch covers it.
layout(std430, set = 0, binding = 19) readonly buffer RadPatchIndexBuffer {
    uint patchIndexGrid[];
};
// Six directional coefficients per static-prop vertex, in Source's ambient-cube
// order: +X, -X, +Y, -Y, +Z, -Z. A single scalar irradiance per vertex throws
// away the direction the light came from, which leaves nothing for a normal map
// to modulate and makes low-poly props read as flat triangles.
layout(std430, set = 0, binding = 20) buffer RadPropCube { vec4 propCube[]; };

layout(std140, set = 0, binding = 18) uniform RadControlBuffer {
    // x = luxel count, y = patch count, z = transfers per patch, w = bounce count.
    uvec4 counts;
    // xy = lightmap atlas size, zw = patch atlas size.
    uvec4 atlasSize;
    // rgb = light_environment _ambient, w = sun angular extent in radians.
    vec4 skyAmbient;
    // rgb = sky/sun colour used when a shadow ray escapes to a sky surface.
    vec4 skyColor;
    // x = prop shadow slop, y = surface bias, z = global light scale,
    // w = current bounce index.
    vec4 tuning;
} radControl;

// Per-dispatch state. All bake pipelines share one push range so they can share
// one pipeline layout; each pass documents its own use of the lanes.
//   rad_bounce      x = bounce index (also selects the ping-pong direction)
//   rad_resolve     x = 0 add indirect at luxels, 1 dilate uncovered texels
//   rad_prop_vertex x = first triangle, y = triangle count, z = indirect rays
layout(push_constant) uniform RadPushBuffer { uvec4 value; } push;

// ---------------------------------------------------------------------------
// Material coverage. Identical rules to raytraced_preview.comp so a surface
// casts the same shadow in the bake that it casts in the live preview.
// ---------------------------------------------------------------------------

bool hasRect(vec4 rect) { return rect.z > 0.0 && rect.w > 0.0; }

vec3 atlasCoordinate(vec4 rect, vec2 uv) {
    ivec3 size = textureSize(materialAtlas, 0);
    vec2 inset = vec2(0.5) / vec2(size.xy);
    float layer = floor(rect.x + 0.0001);
    vec2 pageOrigin = vec2(fract(rect.x), rect.y);
    vec2 pageUv = pageOrigin + inset + fract(uv) * max(rect.zw - inset * 2.0, vec2(0.0));
    return vec3(pageUv, layer);
}

vec4 sampleRect(vec4 rect, vec2 uv, vec4 fallbackValue) {
    return hasRect(rect) ? texture(materialAtlas, atlasCoordinate(rect, uv)) : fallbackValue;
}

vec3 sourceGammaToLinear(vec3 color) {
    return pow(max(color, vec3(0.0)), vec3(2.2));
}

// -TextureShadows reads the caster's own texel, so this has to agree with the
// preview's sampler exactly: a blended WorldVertexTransition surface takes its
// alpha from the mix of both textures, not from the base alone.
vec4 baseSampleForCandidate(uint primitive, vec2 bary) {
    uint first = primitive * 3u;
    VertexRecord a = vertices[indices[first + 0u]];
    VertexRecord b = vertices[indices[first + 1u]];
    VertexRecord c = vertices[indices[first + 2u]];
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    MaterialRecord material = materials[triangles[primitive].data.x];
    vec2 uv = a.texCoord.xy * w.x + b.texCoord.xy * w.y + c.texCoord.xy * w.z;
    vec4 base = sampleRect(material.baseRect, uv, vec4(1.0, 0.0, 1.0, 1.0));
    if ((material.flags.x & RT_HAS_SECONDARY) != 0u) {
        vec2 uv2 = a.texCoord.zw * w.x + b.texCoord.zw * w.y + c.texCoord.zw * w.z;
        float blend = a.surface.x * w.x + b.surface.x * w.y + c.surface.x * w.z;
        base = mix(base, sampleRect(material.secondaryRect, uv2, base),
                   clamp(blend, 0.0, 1.0));
    }
    return base;
}

float materialCoverage(MaterialRecord material, vec4 base, uint triangleFlags) {
    if ((material.flags.x & RT_WATER) != 0u) return 1.0;
    if ((triangleFlags & TRIANGLE_SPRITE) != 0u)
        return clamp(base.a * material.transparencyControls.x, 0.0, 1.0);
    if ((material.flags.y & RT2_ALPHA_TEST) == 0u &&
        (material.flags.x & RT_TRANSLUCENT) == 0u) return 1.0;
    return clamp(base.a * material.transparencyControls.x, 0.0, 1.0);
}

bool candidateHasCoverage(MaterialRecord material, vec4 base, uint triangleFlags) {
    float coverage = materialCoverage(material, base, triangleFlags);
    if ((triangleFlags & TRIANGLE_SPRITE) != 0u) return coverage >= 0.005;
    if ((material.flags.y & RT2_ALPHA_TEST) != 0u)
        return coverage >= clamp(material.detailAlphaControls.w, 0.0, 1.0);
    if ((material.flags.x & RT_TRANSLUCENT) != 0u) return coverage >= 0.005;
    return true;
}

// -StaticPropTextureShadows: an alpha-tested or translucent caster attenuates
// rather than blocking. Alpha-tested texels stay binary, as in the raster path.
float candidateOpacity(MaterialRecord material, vec4 base, uint triangleFlags) {
    if ((triangleFlags & TRIANGLE_SPRITE) != 0u)
        return materialCoverage(material, base, triangleFlags);
    if ((material.flags.y & RT2_ALPHA_TEST) != 0u) return 1.0;
    if ((material.flags.x & RT_TRANSLUCENT) != 0u)
        return materialCoverage(material, base, triangleFlags);
    return 1.0;
}

// ---------------------------------------------------------------------------
// Shadow tracing
// ---------------------------------------------------------------------------

struct ShadowBoundary {
    bool hit;
    bool sky;
    float distance;
    float opacity;
    // Linear texel colour of a translucent caster, so tinted glass filters the
    // light it passes rather than only dimming it.
    vec3 tint;
};

// `propSlop` implements the rule Valve maps rely on: VRAD does not black out a
// sample because a light happens to sit inside or flush against a static prop.
// Prop hits within that distance of either end of the ray are ignored, so light
// still spreads out from a light buried in prop geometry.
ShadowBoundary closestShadowBoundary(vec3 origin, vec3 direction, float maximumDistance,
                                     bool requireSkyPortal, float propSlop) {
    ShadowBoundary result;
    result.hit = false;
    result.sky = false;
    result.distance = maximumDistance;
    result.opacity = 0.0;
    result.tint = vec3(1.0);

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneAccelerationStructure, gl_RayFlagsNoneEXT, 0xffu,
                          origin, 0.03, direction, maximumDistance);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) !=
            gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
        uint flags = triangles[primitive].data.y;
        bool skyPortal = (flags & TRIANGLE_SKY) != 0u;
        if ((flags & TRIANGLE_NO_SHADOW) != 0u && !(requireSkyPortal && skyPortal)) continue;
        if ((flags & TRIANGLE_STATIC_PROP) != 0u && propSlop > 0.0) {
            float t = rayQueryGetIntersectionTEXT(query, false);
            if (t < propSlop || t > maximumDistance - propSlop) continue;
        }
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, false);
        MaterialRecord material = materials[triangles[primitive].data.x];
        vec4 base = baseSampleForCandidate(primitive, bary);
        if (!candidateHasCoverage(material, base, flags)) continue;
        rayQueryConfirmIntersectionEXT(query);
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) !=
        gl_RayQueryCommittedIntersectionTriangleEXT) return result;

    result.hit = true;
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    result.sky = (triangles[primitive].data.y & TRIANGLE_SKY) != 0u;
    result.distance = rayQueryGetIntersectionTEXT(query, true);
    vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
    MaterialRecord material = materials[triangles[primitive].data.x];
    vec4 base = baseSampleForCandidate(primitive, bary);
    result.opacity = candidateOpacity(material, base, triangles[primitive].data.y);
    // Atlas colour is UNORM sRGB; transmission is linear.
    if ((material.flags.x & RT_TRANSLUCENT) != 0u)
        result.tint = sourceGammaToLinear(base.rgb);
    return result;
}

// Walks up to four translucent panes front-to-back instead of treating glass as
// an opaque caster. Returns the surviving fraction of the light.
// Returns the per-channel fraction of the light that survives the ray. Stock
// VRAD tracks a scalar here; carrying RGB is what lets a stained-glass texel
// cast coloured light instead of a grey smudge.
vec3 radTransmission(vec3 origin, vec3 direction, float maximumDistance,
                     bool requireSkyPortal) {
    vec3 transmission = vec3(1.0);
    float remaining = maximumDistance;
    vec3 cursor = origin;
    float propSlop = radControl.tuning.x;
    for (int layer = 0; layer < 4 && remaining > 0.03 &&
                        max(max(transmission.r, transmission.g), transmission.b) > 0.005;
         ++layer) {
        ShadowBoundary boundary = closestShadowBoundary(cursor, direction, remaining,
                                                        requireSkyPortal, propSlop);
        if (!boundary.hit) return requireSkyPortal ? vec3(0.0) : transmission;
        if (boundary.sky) return transmission;
        if (boundary.opacity >= 0.999) return vec3(0.0);
        // Alpha decides how much gets through; the texel's own colour decides
        // what colour it is once through.
        transmission *= (1.0 - clamp(boundary.opacity, 0.0, 1.0)) * boundary.tint;
        float advance = boundary.distance + 0.06;
        cursor += direction * advance;
        remaining -= advance;
        propSlop = 0.0;
    }
    return requireSkyPortal ? vec3(0.0) : transmission;
}

// ---------------------------------------------------------------------------
// VRAD direct lighting: GatherSampleLight
// ---------------------------------------------------------------------------

// Returns the light's contribution to a sample, already multiplied by N.L and
// visibility. Attenuation follows 1/(c + l*d + q*d^2), the falloff VRAD applies
// after CreateDirectLights has pre-scaled intensity by (c + 100l + 10000q).
// VRAD spends NSAMPLES_SUN_AREA_LIGHT (30) rays on the sun per sample. This is
// nested inside the 2x2 luxel supersample and each subsample rotates to a
// different stratum (see sampleSeed), so 8 here costs 8 * 4 = 32 rays per
// luxel across 32 distinct directions - VRAD's budget, spent the same way.
const int RAD_SUN_AREA_SAMPLES = 8;

// Defined below; the sun's disc sampling needs it before that point.
float radRadicalInverse(uint bits);

// As gatherSampleLight, but the shadow ray starts from `shadowPosition` while
// the light direction, N.L and falloff still come from `position`. VRAD's extra
// sampling pass averages several sample points across a luxel's footprint; the
// term that actually varies over that footprint is visibility, and keeping the
// shading point fixed at the luxel centre avoids dragging N.L or the distance
// falloff off the surface at a face edge.
vec3 gatherSampleLightFrom(LightRecord light, vec3 position, vec3 normal,
                           vec3 shadowPosition, uint sampleSeed) {
    float type = light.positionType.w;
    vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
    vec3 toLight;
    float maximumDistance;
    bool requireSkyPortal = false;
    float falloff = 1.0;

    if (type == RAD_EMIT_DIRECTIONAL || type == RAD_EMIT_ENVIRONMENT) {
        // emit_skylight: the sun is infinitely distant, and only reaches a
        // sample whose shadow ray escapes through a sky surface.
        toLight = normalize(-light.directionRange.xyz);
        maximumDistance = light.directionRange.w;
        requireSkyPortal = (type == RAD_EMIT_ENVIRONMENT);
    } else {
        vec3 delta = light.positionType.xyz - position;
        float distance = length(delta);
        if (distance < 1e-4) return vec3(0.0);
        toLight = delta / distance;
        maximumDistance = distance;
        // _distance is not a cutoff: VRAD stores it in the worldlights lump and
        // never reads it back. The scene builder now leaves this at zero for
        // point and spot lights, so this only guards the sun's trace length.

        falloff = radDistanceFalloff(light, distance);
        if (type == RAD_EMIT_SPOTLIGHT) falloff *= radSpotFalloff(light, toLight);
    }

    float ndotl = dot(normal, toLight);
    if (ndotl <= 0.0 || falloff <= 0.0) return vec3(0.0);

    vec3 origin = shadowPosition + normal * radControl.tuning.y;

    // SunSpreadAngle: the sun is an area light, not a point, which is what makes
    // a shadow's edge widen with occluder distance - crisp at the contact point,
    // diffuse at the far end of a long shadow. A single ray, whatever its
    // origin, can only ever produce a binary edge.
    //
    // GatherSampleSkyLightSSE jitters by a point on the UNIT SPHERE scaled by
    // g_SunAngularExtent, added to the (trace-length) sun vector:
    //
    //     Vector ofs = sampler.NextValue();      // unit vector
    //     ofs *= MAX_TRACE_LENGTH * g_SunAngularExtent;
    //     delta += ofs;
    //
    // That is a spherical SHELL of radius `extent`, not a filled disc, and it is
    // deliberately wider than a disc: projected onto the plane across the sun
    // vector the offsets pile up at the rim (mean lateral offset pi/4 = 0.785 of
    // the extent) instead of thinning out toward it (2/3 = 0.667 for a
    // uniform-area disc). Sampling a disc here is what made this penumbra too
    // narrow against VRAD's. Sample 0 is unjittered, as in VRAD.
    float sunExtent = (type == RAD_EMIT_DIRECTIONAL || type == RAD_EMIT_ENVIRONMENT)
        ? light.innerControls.z : 0.0;
    vec3 transmission;
    if (sunExtent > 0.0) {
        vec3 tangent = normalize(abs(toLight.z) < 0.999
                                     ? cross(toLight, vec3(0.0, 0.0, 1.0))
                                     : cross(toLight, vec3(0.0, 1.0, 0.0)));
        vec3 bitangent = cross(toLight, tangent);
        vec3 accumulated = vec3(0.0);
        for (uint s = 0u; s < uint(RAD_SUN_AREA_SAMPLES); ++s) {
            if (s == 0u) {
                accumulated += radTransmission(origin, toLight, maximumDistance,
                                               requireSkyPortal);
                continue;
            }
            // A uniform point on the unit sphere, stratified in azimuth and
            // deterministic per sample index so neighbouring luxels agree and
            // the penumbra reads as a gradient, not as noise.
            uint stratum = s + sampleSeed * uint(RAD_SUN_AREA_SAMPLES);
            float u1 = (float(s) + 0.5) / float(RAD_SUN_AREA_SAMPLES);
            float u2 = radRadicalInverse(stratum);
            float angle = 6.28318530718 * u1;
            float height = 1.0 - 2.0 * u2;
            float ring = sqrt(max(0.0, 1.0 - height * height));
            vec3 offset = (tangent * (cos(angle) * ring) +
                           bitangent * (sin(angle) * ring) + toLight * height) * sunExtent;
            accumulated += radTransmission(origin, normalize(toLight + offset),
                                           maximumDistance, requireSkyPortal);
        }
        transmission = accumulated / float(RAD_SUN_AREA_SAMPLES);
    } else {
        transmission = radTransmission(origin, toLight, maximumDistance, requireSkyPortal);
    }
    if (max(max(transmission.r, transmission.g), transmission.b) <= 0.005) return vec3(0.0);
    return radiance * transmission * (ndotl * falloff * radControl.tuning.z);
}

vec3 gatherSampleLight(LightRecord light, vec3 position, vec3 normal) {
    return gatherSampleLightFrom(light, position, normal, position, 0u);
}

// Source's ambient-cube axes.
const vec3 kAmbientCubeAxis[6] = vec3[6](
    vec3( 1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3( 0.0, 1.0, 0.0), vec3( 0.0,-1.0, 0.0),
    vec3( 0.0, 0.0, 1.0), vec3( 0.0, 0.0,-1.0));

// Distributes one directional sample over the cube. `toLight` points from the
// surface toward the source, matching AddDirectionToAmbientCube.
void radAccumulateCube(inout vec3 cube[6], vec3 toLight, vec3 radiance) {
    for (int axis = 0; axis < 6; ++axis) {
        float weight = max(dot(toLight, kAmbientCubeAxis[axis]), 0.0);
        cube[axis] += radiance * weight;
    }
}

// Evaluates a cube for a shading normal. Squared components are Source's
// reconstruction, and because it takes the normal at shading time a normal map
// still modulates the result.
vec3 radEvaluateCube(vec3 cube[6], vec3 normal) {
    vec3 squared = normal * normal;
    return cube[normal.x > 0.0 ? 0 : 1] * squared.x +
           cube[normal.y > 0.0 ? 2 : 3] * squared.y +
           cube[normal.z > 0.0 ? 4 : 5] * squared.z;
}

// gatherSampleLight folds N.L into its result, which is correct for a luxel but
// wrong for a cube: the cube stores incoming radiance per direction and applies
// the cosine term later, at shading time. This returns the unattenuated arrival
// plus the direction it arrived from.
vec3 gatherSampleLightDirectional(LightRecord light, vec3 position, vec3 normal,
                                  out vec3 toLight) {
    float type = light.positionType.w;
    vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w;
    float maximumDistance;
    bool requireSkyPortal = false;
    float falloff = 1.0;
    toLight = vec3(0.0, 0.0, 1.0);

    if (type == RAD_EMIT_DIRECTIONAL || type == RAD_EMIT_ENVIRONMENT) {
        toLight = normalize(-light.directionRange.xyz);
        maximumDistance = light.directionRange.w;
        requireSkyPortal = (type == RAD_EMIT_ENVIRONMENT);
    } else {
        vec3 delta = light.positionType.xyz - position;
        float distance = length(delta);
        if (distance < 1e-4) return vec3(0.0);
        toLight = delta / distance;
        maximumDistance = distance;
        // _distance is not a cutoff: VRAD stores it in the worldlights lump and
        // never reads it back. The scene builder now leaves this at zero for
        // point and spot lights, so this only guards the sun's trace length.
        falloff = radDistanceFalloff(light, distance);
        if (type == RAD_EMIT_SPOTLIGHT) falloff *= radSpotFalloff(light, toLight);
    }

    // Still gated on the vertex normal so a light behind the surface cannot
    // deposit energy into the cube through the mesh.
    if (dot(normal, toLight) <= 0.0 || falloff <= 0.0) return vec3(0.0);
    vec3 origin = position + normal * radControl.tuning.y;
    vec3 transmission = radTransmission(origin, toLight, maximumDistance, requireSkyPortal);
    if (max(max(transmission.r, transmission.g), transmission.b) <= 0.005) return vec3(0.0);
    return radiance * transmission * (falloff * radControl.tuning.z);
}

uint radHash(uvec3 value) {
    value = value * 1664525u + 1013904223u;
    value.x += value.y * value.z;
    value.y += value.z * value.x;
    value.z += value.x * value.y;
    value ^= value >> 16u;
    return value.x ^ value.y ^ value.z;
}

float radRandom(inout uint state) {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return float(state) * (1.0 / 4294967296.0);
}

// Van der Corput radical inverse, base 2.
float radRadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Deterministic cosine-weighted hemisphere direction, shared by every patch.
//
// Giving each patch its own random ray set makes neighbouring patches land on
// independent estimates of the same incoming light, and that difference reads
// as low-frequency mottling across a wall. Using one stratified Hammersley set
// for all of them, rotated into each patch's tangent frame, makes coplanar
// neighbours sample the same directions: the residual error becomes a smooth
// bias over the surface instead of per-patch noise. The tangent frame is built
// deterministically from the normal, so equal normals really do get equal
// directions.
vec3 radStratifiedHemisphere(vec3 normal, uint sampleIndex, uint sampleCount) {
    float u1 = (float(sampleIndex) + 0.5) / float(max(sampleCount, 1u));
    float u2 = radRadicalInverse(sampleIndex);
    float angle = 6.28318530718 * u1;
    float radius = sqrt(u2);
    vec3 tangent = normalize(abs(normal.z) < 0.999 ? cross(normal, vec3(0.0, 0.0, 1.0))
                                                   : cross(normal, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * (cos(angle) * radius) + bitangent * (sin(angle) * radius) +
                     normal * sqrt(max(0.0, 1.0 - u2)));
}

vec3 radCosineHemisphere(vec3 normal, inout uint state) {
    float r1 = radRandom(state);
    float r2 = radRandom(state);
    float angle = 6.28318530718 * r1;
    float radius = sqrt(r2);
    vec3 tangent = normalize(abs(normal.z) < 0.999 ? cross(normal, vec3(0.0, 0.0, 1.0))
                                                   : cross(normal, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * (cos(angle) * radius) + bitangent * (sin(angle) * radius) +
                     normal * sqrt(max(0.0, 1.0 - r2)));
}

// emit_skyambient: VRAD samples the hemisphere and accumulates the fraction that
// reaches a sky surface. One stratified sample per call, averaged by the caller.
vec3 skyAmbientSample(vec3 position, vec3 normal, inout uint state) {
    if (dot(radControl.skyAmbient.rgb, vec3(1.0)) <= 0.0) return vec3(0.0);
    vec3 direction = radCosineHemisphere(normal, state);
    vec3 origin = position + normal * radControl.tuning.y;
    vec3 visible = radTransmission(origin, direction, 131072.0, true);
    return radControl.skyAmbient.rgb * visible;
}

// Maps a world position on a known face to its texel in that face's rect.
vec2 rectTexel(RectRecord record, vec3 position) {
    float scale = max(record.origin.z, 1e-4);
    float s = (dot(position, record.uAxis.xyz) + record.uAxis.w) / scale - record.origin.x;
    float t = (dot(position, record.vAxis.xyz) + record.vAxis.w) / scale - record.origin.y;
    return vec2(clamp(s, 0.0, record.rect.z - 1.0), clamp(t, 0.0, record.rect.w - 1.0));
}

// Resolves a surface hit to the patch covering it. Returns 0xffffffff when the
// triangle carries no lightmap or no patch reached that texel.
uint patchAtHit(uint primitive, vec3 position) {
    uint flags = triangles[primitive].data.y;
    if ((flags & TRIANGLE_HAS_LIGHTMAP) == 0u) return 0xffffffffu;
    uint rectIndex = triangles[primitive].data.z;
    RectRecord record = patchRects[rectIndex];
    vec2 texel = rectTexel(record, position);
    ivec2 atlas = ivec2(record.rect.xy + floor(texel + 0.5));
    if (atlas.x < 0 || atlas.y < 0 ||
        atlas.x >= int(radControl.atlasSize.z) || atlas.y >= int(radControl.atlasSize.w))
        return 0xffffffffu;
    uint stored = patchIndexGrid[uint(atlas.y) * radControl.atlasSize.z + uint(atlas.x)];
    return stored == 0u ? 0xffffffffu : stored - 1u;
}

#endif // RAD_COMMON_GLSL
