#include "RayTracingScene.hpp"

#include "GameFileSystem.hpp"
#include "VmfRope.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace hammer::render {
namespace {

constexpr int AtlasBorder = 1;
// Triangle flags now live in RayTracingScene.hpp (RayTracingTriangleFlag).

bool isToolMaterial(std::string_view normalizedMaterial)
{
    return normalizedMaterial.rfind("tools/", 0) == 0;
}

bool isAlwaysOpaqueToolMaterial(std::string_view normalizedMaterial)
{
    // tools/toolsblack is not an editor helper: it is a real rendered surface
    // that absorbs everything reaching it. It occludes every ray type and casts
    // a shadow, and unlike the other tool materials it keeps doing so when tool
    // textures are hidden from the view.
    return normalizedMaterial == "tools/toolsblack";
}

bool toolMaterialCastsShadow(std::string_view normalizedMaterial)
{
    // tools/toolsnodraw is deliberately NOT in this set. A nodraw face marks a
    // surface that is never drawn, and blocking light with it made invisible
    // geometry throw shadows all over a map - the back faces of ordinary
    // brushes are nodraw, so this darkened rooms in a way that depended on
    // which hidden face a shadow ray happened to cross. Light passes through
    // nodraw; the visible faces of the same brushes still do the occluding.
    return isAlwaysOpaqueToolMaterial(normalizedMaterial);
}

bool isSkyToolMaterial(std::string_view normalizedMaterial)
{
    // Source admits light_environment sunlight/skylight through sky surfaces.
    // Support both the normal 3D/2D sky portal and the 2D-only variant.
    return normalizedMaterial == "tools/toolsskybox" ||
           normalizedMaterial == "tools/toolsskybox2d";
}

// Everything under models/editor is a Hammer helper: the light bulb, the
// spotlight cone, info_target's little box, and every other studio model an FGD
// points at purely so a point entity has something to click on.
//
// This replaces a hardcoded list of light classnames. That list could only ever
// name the helpers someone had already noticed, so any other helper model - and
// any helper added by a game's own FGD - still threw a shadow and got shaded as
// if it were real geometry. The directory is the thing that actually means
// "editor helper", so test that instead.
// "Appearance"/"Custom Appearance": a string of letters where a is 0%, m is
// 100% and z is 200% brightness, stepped at 10 Hz.
//
// A compiled map animates these; a static preview has to pick one number, and
// the average of the sequence is the only choice that behaves sensibly across
// all of them. Style 0 ("m") is exactly 100%, a slow strobe ("aaaaaaaazzzzzzzz")
// averages back to 100%, and a light explicitly authored dark ("a", which is
// what the TurnOff input sets) reads as off instead of fully lit.
double lightPatternScale(std::string_view pattern)
{
    double total = 0.0;
    std::size_t counted = 0;
    for (unsigned char letter : pattern) {
        const unsigned char lower = static_cast<unsigned char>(std::tolower(letter));
        if (lower < 'a' || lower > 'z') continue;
        // 'm' is twelve letters past 'a', so this puts 100% exactly on 'm'.
        total += static_cast<double>(lower - 'a') / 12.0;
        ++counted;
    }
    return counted == 0 ? 1.0 : total / static_cast<double>(counted);
}

// The documented lightstyle presets, indexed by the "style" keyvalue.
std::string_view lightStylePattern(int style)
{
    switch (style) {
        case 0:  return "m";
        case 1:  return "mmnmmommommnonmmonqnmmo";                       // flicker A
        case 2:  return "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba";  // slow pulse
        case 3:  return "mmmmmaaaaammmmmaaaaaabcdefgabcdefg";            // candle A
        case 4:  return "mamamamamama";                                  // fast strobe
        case 5:  return "jklmnopqrstuvwxyzyxwvutsrqponmlkj";             // gentle pulse
        case 6:  return "nmonqnmomnmomomno";                             // flicker B
        case 7:  return "mmmaaaabcdefgmmmmaaaammmaamm";                  // candle B
        case 8:  return "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa";    // candle C
        case 9:  return "aaaaaaaazzzzzzzz";                              // slow strobe
        case 10: return "mmamammmmammamamaaamammma";                     // fluorescent flicker
        case 11: return "abcdefghijklmnopqrrqponmlkjihgfedcba";          // slow pulse, noblack
        case 12: return "mmnnmmnnnmmnn";                                 // underwater mutation
        default: return "m";
    }
}

bool isEditorHelperModel(std::string_view modelPath)
{
    if (modelPath.empty()) return false;
    // FGD studio()/lightprop() arguments reach here exactly as the .fgd spelled
    // them - mixed case and backslashes both occur - so normalize before
    // comparing rather than assuming the VMF layer already did.
    std::string path = hammer::assets::GameFileSystem::normalizeResourcePath(modelPath);
    if (path.rfind("models/", 0) == 0) path.erase(0, 7);
    return path.rfind("editor/", 0) == 0;
}

std::uint32_t shadowFlagsForMaterial(std::string_view normalizedMaterial)
{
    return isToolMaterial(normalizedMaterial) &&
           !toolMaterialCastsShadow(normalizedMaterial)
        ? TriangleNoShadow : 0u;
}

struct AtlasItem
{
    std::string key;
    const hammer::assets::Image* image{nullptr};
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

class AtlasBuilder
{
public:
    int add(std::string key, const hammer::assets::Image& image)
    {
        if (!image.valid()) return -1;
        if (const auto found = keyToItem_.find(key); found != keyToItem_.end())
            return found->second;
        const int index = static_cast<int>(items_.size());
        items_.push_back({std::move(key), &image, 0, 0, image.width, image.height});
        keyToItem_.emplace(items_.back().key, index);
        return index;
    }

    bool build(int maximumSize, int maximumLayers, RayTracingAtlas& atlas,
               std::vector<std::array<float, 4>>& rects, std::string& error)
    {
        if (items_.empty()) {
            hammer::assets::Image checker;
            checker.width = 2;
            checker.height = 2;
            checker.pixels = {0xffff00ffu, 0xff202020u, 0xff202020u, 0xffff00ffu};
            ownedFallback_ = std::move(checker);
            add("__fallback", ownedFallback_);
        }

        const int limit = std::clamp(maximumSize, 1024, 16384);
        const int layerLimit = std::clamp(maximumLayers, 1, 2048);
        std::vector<int> order(items_.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (items_[a].height != items_[b].height)
                return items_[a].height > items_[b].height;
            return items_[a].width > items_[b].width;
        });

        int widest = 0;
        int tallest = 0;
        for (const AtlasItem& item : items_) {
            const int paddedWidth = item.width + AtlasBorder * 2;
            const int paddedHeight = item.height + AtlasBorder * 2;
            widest = std::max(widest, paddedWidth);
            tallest = std::max(tallest, paddedHeight);
        }
        if (widest > limit || tallest > limit) {
            error = "A ray-tracing material texture exceeds the Vulkan device image-size limit";
            return false;
        }

        auto alignedPageSize = [](int value) {
            constexpr int Granularity = 256;
            return ((value + Granularity - 1) / Granularity) * Granularity;
        };
        struct ShelfPage { int cursorX{0}; int cursorY{0}; int shelfHeight{0}; };

        // Vulkan does not require power-of-two texture dimensions. Array layers
        // all share the full page size, so a 4096 VTF plus a one-pixel gutter must
        // not force an 8192x8192 allocation. Try 256-texel-aligned page sizes and
        // keep the packing with the smallest actual width * height * layer count.
        int size = 0;
        std::uint64_t bestAllocatedTexels = std::numeric_limits<std::uint64_t>::max();
        std::vector<int> bestX(items_.size(), 0);
        std::vector<int> bestY(items_.size(), 0);
        std::vector<int> itemLayers(items_.size(), 0);
        int bestLayerCount = 0;
        const int firstSize = std::min(limit, alignedPageSize(std::max({512, widest, tallest})));
        for (int candidateSize = firstSize;;) {
            std::vector<ShelfPage> pages(1);
            std::vector<int> candidateX(items_.size(), 0);
            std::vector<int> candidateY(items_.size(), 0);
            std::vector<int> candidateLayers(items_.size(), 0);
            bool validPacking = true;
            for (int itemIndex : order) {
                const AtlasItem& item = items_[static_cast<std::size_t>(itemIndex)];
                const int paddedWidth = item.width + AtlasBorder * 2;
                const int paddedHeight = item.height + AtlasBorder * 2;
                bool placed = false;
                for (int layer = 0; layer < static_cast<int>(pages.size()); ++layer) {
                    ShelfPage candidate = pages[static_cast<std::size_t>(layer)];
                    if (candidate.cursorX + paddedWidth > candidateSize) {
                        candidate.cursorX = 0;
                        candidate.cursorY += candidate.shelfHeight;
                        candidate.shelfHeight = 0;
                    }
                    if (candidate.cursorY + paddedHeight > candidateSize) continue;
                    candidateX[static_cast<std::size_t>(itemIndex)] = candidate.cursorX + AtlasBorder;
                    candidateY[static_cast<std::size_t>(itemIndex)] = candidate.cursorY + AtlasBorder;
                    candidateLayers[static_cast<std::size_t>(itemIndex)] = layer;
                    candidate.cursorX += paddedWidth;
                    candidate.shelfHeight = std::max(candidate.shelfHeight, paddedHeight);
                    pages[static_cast<std::size_t>(layer)] = candidate;
                    placed = true;
                    break;
                }
                if (!placed) {
                    if (static_cast<int>(pages.size()) >= layerLimit) {
                        validPacking = false;
                        break;
                    }
                    pages.push_back({paddedWidth, 0, paddedHeight});
                    candidateX[static_cast<std::size_t>(itemIndex)] = AtlasBorder;
                    candidateY[static_cast<std::size_t>(itemIndex)] = AtlasBorder;
                    candidateLayers[static_cast<std::size_t>(itemIndex)] =
                        static_cast<int>(pages.size()) - 1;
                }
            }
            if (!validPacking) continue;
            const std::uint64_t allocatedTexels =
                static_cast<std::uint64_t>(candidateSize) * candidateSize * pages.size();
            if (allocatedTexels < bestAllocatedTexels ||
                (allocatedTexels == bestAllocatedTexels &&
                 static_cast<int>(pages.size()) < bestLayerCount)) {
                bestAllocatedTexels = allocatedTexels;
                size = candidateSize;
                bestLayerCount = static_cast<int>(pages.size());
                bestX = std::move(candidateX);
                bestY = std::move(candidateY);
                itemLayers = std::move(candidateLayers);
            }
            if (candidateSize >= limit) break;
            const int doubled = candidateSize > limit / 2 ? limit : candidateSize * 2;
            if (doubled <= candidateSize) break;
            candidateSize = doubled;
        }
        if (size <= 0 || bestLayerCount <= 0) {
            error = "Ray-tracing texture atlas exceeds the Vulkan device array-layer limit";
            return false;
        }
        for (std::size_t itemIndex = 0; itemIndex < items_.size(); ++itemIndex) {
            items_[itemIndex].x = bestX[itemIndex];
            items_[itemIndex].y = bestY[itemIndex];
        }
        std::vector<ShelfPage> pages(static_cast<std::size_t>(bestLayerCount));

        atlas.width = size;
        atlas.height = size;
        atlas.layers = static_cast<int>(pages.size());
        atlas.rgba.assign(static_cast<std::size_t>(size) * size * atlas.layers * 4u, 0u);
        rects.resize(items_.size());

        auto writePixel = [&](int layer, int x, int y, std::uint32_t argb) {
            if (layer < 0 || layer >= atlas.layers || x < 0 || y < 0 || x >= size || y >= size)
                return;
            const std::size_t pixel = (static_cast<std::size_t>(layer) * size * size) +
                                      static_cast<std::size_t>(y * size + x);
            const std::size_t offset = pixel * 4u;
            atlas.rgba[offset + 0] = static_cast<std::uint8_t>((argb >> 16) & 0xffu);
            atlas.rgba[offset + 1] = static_cast<std::uint8_t>((argb >> 8) & 0xffu);
            atlas.rgba[offset + 2] = static_cast<std::uint8_t>(argb & 0xffu);
            atlas.rgba[offset + 3] = static_cast<std::uint8_t>((argb >> 24) & 0xffu);
        };

        for (std::size_t itemIndex = 0; itemIndex < items_.size(); ++itemIndex) {
            const AtlasItem& item = items_[itemIndex];
            const int layer = itemLayers[itemIndex];
            const hammer::assets::Image& image = *item.image;
            for (int y = -AtlasBorder; y < image.height + AtlasBorder; ++y) {
                const int sourceY = std::clamp(y, 0, image.height - 1);
                for (int x = -AtlasBorder; x < image.width + AtlasBorder; ++x) {
                    const int sourceX = std::clamp(x, 0, image.width - 1);
                    const std::uint32_t pixel = image.pixels[static_cast<std::size_t>(
                        sourceY * image.width + sourceX)];
                    writePixel(layer, item.x + x, item.y + y, pixel);
                }
            }
            // The integer portion of rect.x stores the array layer. The shader
            // uses fract(rect.x) as the normalized page-local X origin.
            rects[itemIndex] = {
                static_cast<float>(layer) +
                    static_cast<float>(item.x) / static_cast<float>(size),
                static_cast<float>(item.y) / static_cast<float>(size),
                static_cast<float>(item.width) / static_cast<float>(size),
                static_cast<float>(item.height) / static_cast<float>(size)};
        }
        return true;
    }

private:
    std::vector<AtlasItem> items_;
    std::unordered_map<std::string, int> keyToItem_;
    hammer::assets::Image ownedFallback_;
};

struct PendingMaterial
{
    std::shared_ptr<const hammer::assets::Material> source;
    RayTracingMaterial gpu;
    int base{-1};
    int secondary{-1};
    int detail{-1};
    int bump{-1};
    int exponent{-1};
    int selfIllum{-1};
    int lightWarp{-1};
    int flow{-1};
    std::array<int, 6> environment{{-1, -1, -1, -1, -1, -1}};
};


std::optional<std::string> entityProperty(const hammer::vmf::EntityMarker& entity,
                                          std::string_view key)
{
    // Source's key lookup is case-insensitive, and it matters here: VRAD's
    // HDR and sun keys are authored mixed-case (SunSpreadAngle,
    // _AmbientScaleHDR) while everything else in a VMF is lower-case.
    const auto equalsIgnoringCase = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    for (const auto& [name, value] : entity.properties)
        if (equalsIgnoringCase(name, key)) return value;
    return std::nullopt;
}

double propertyNumber(const hammer::vmf::EntityMarker& entity, std::string_view key,
                      double fallback)
{
    const auto value = entityProperty(entity, key);
    if (!value) return fallback;
    try { return std::stod(*value); } catch (...) { return fallback; }
}

// Port of VRAD's LightForString (utils/vrad/lightmap.cpp).
//
// The differences from a naive "three floats and a brightness" parse all
// matter for how bright a map actually reads:
//
//   * VRAD converts with pow(x/255, 2.2), not the sRGB piecewise curve. The two
//     disagree by several percent in the darks, which is exactly where a bounce
//     estimate is most visible.
//   * A one-value string is a greyscale light, not a parse failure.
//   * An eight-value string is two 4-tuples: LDR first, HDR second. The preview
//     is HDR-only (see rad_common.glsl), so the second tuple always wins. Maps
//     that ship a dim LDR tuple and a bright HDR one were previously rendered
//     with their LDR intensity.
//
// Returns false when the key is absent or unparseable, so callers can
// reproduce VRAD's "did this key supply a value?" branches exactly.
bool lightForString(const std::optional<std::string>& text, std::array<float, 4>& intensity,
                    bool hdr = true)
{
    if (!text) return false;
    std::istringstream input(*text);
    double v[8]{};
    int count = 0;
    while (count < 8 && (input >> v[count])) ++count;
    if (count == 0) return false;

    double r = v[0], g = v[0], b = v[0], scaler = 0.0;
    if (count >= 8) {
        // Two 4-tuples: LDR first, HDR second. VRAD picks by compile mode.
        if (hdr) { r = v[4]; g = v[5]; b = v[6]; scaler = v[7]; }
        else     { r = v[0]; g = v[1]; b = v[2]; scaler = v[3]; }
        count = 4;
    } else if (count >= 3) {
        r = v[0]; g = v[1]; b = v[2];
        if (count >= 4) scaler = v[3];
    }
    if (r < 0.0 || g < 0.0 || b < 0.0 || scaler < 0.0) {
        intensity = {0.0f, 0.0f, 0.0f, 0.0f};
        return false;
    }

    auto linear = [](double value) {
        return static_cast<float>(std::pow(std::max(value, 0.0) / 255.0, 2.2));
    };
    // VRAD folds the scaler into the colour; the preview keeps it in w so the
    // shaders can keep colour and intensity separate.
    intensity = {linear(r), linear(g), linear(b),
                 count >= 4 ? static_cast<float>(scaler / 255.0) : 1.0f};
    return true;
}

std::array<float, 4> parseLightColor(const std::optional<std::string>& text,
                                     std::array<float, 4> fallback, bool hdr = true)
{
    std::array<float, 4> intensity{};
    return lightForString(text, intensity, hdr) ? intensity : fallback;
}

// mathlib's SolveInverseQuadratic / SolveInverseQuadraticMonotonic, needed to
// turn _fifty_percent_distance into the (c, l, q) VRAD actually shades with.
bool solveInverseQuadratic(double x1, double y1, double x2, double y2, double x3, double y3,
                           double& a, double& b, double& c)
{
    const double det = (x1 - x2) * (x1 - x3) * (x2 - x3);
    if (det == 0.0) return false;
    a = (x3 * (-y1 + y2) + x2 * (y1 - y3) + x1 * (-y2 + y3)) / det;
    b = (x3 * x3 * (y1 - y2) + x1 * x1 * (y2 - y3) + x2 * x2 * (-y1 + y3)) / det;
    c = (x1 * x3 * (-x1 + x3) * y2 + x2 * x2 * (x3 * y1 - x1 * y3) +
         x2 * (-(x3 * x3 * y1) + x1 * x1 * y3)) / det;
    return true;
}

bool solveInverseQuadraticMonotonic(double x1, double y1, double x2, double y2,
                                    double x3, double y3, double& a, double& b, double& c)
{
    // Sort by x, then walk the middle sample toward the straight line between
    // the endpoints until the curve stops doubling back on itself.
    auto swapPair = [](double& ax, double& ay, double& bx, double& by) {
        std::swap(ax, bx);
        std::swap(ay, by);
    };
    if (x1 > x2) swapPair(x1, y1, x2, y2);
    if (x2 > x3) swapPair(x2, y2, x3, y3);
    if (x1 > x2) swapPair(x1, y1, x2, y2);

    for (double blend = 0.0; blend <= 1.0; blend += 0.05) {
        const double span = (x3 - x1);
        const double linear = std::abs(span) < 1e-12
            ? y1 : y1 + (y3 - y1) * ((x2 - x1) / span);
        const double midpoint = (1.0 - blend) * y2 + blend * linear;
        if (!solveInverseQuadratic(x1, y1, x2, midpoint, x3, y3, a, b, c)) return false;
        const double derivative = 2.0 * a + b;
        if (y1 < y2 && y2 < y3) {
            if (derivative >= 0.0) return true;
        } else if (y1 > y2 && y2 > y3) {
            if (derivative <= 0.0) return true;
        } else {
            return true;
        }
    }
    return true;
}

// ParseLightGeneric checks "target" BEFORE it ever looks at angles: a light
// aimed at an entity takes its direction from that entity's origin and its own
// pitch/angles keys are not consulted at all. Spotlights pointed at an
// info_target are the usual way to aim a light in Hammer, and ignoring the key
// left every one of them shining wherever its (typically default) angles said.
bool targetedLightDirection(const hammer::vmf::Scene& scene,
                            const hammer::vmf::EntityMarker& entity,
                            hammer::vmf::Vec3& direction)
{
    const auto target = entityProperty(entity, "target");
    if (!target || target->empty()) return false;
    for (const auto& candidate : scene.entities) {
        if (candidate.id == entity.id || candidate.targetName != *target) continue;
        const double x = candidate.origin.x - entity.origin.x;
        const double y = candidate.origin.y - entity.origin.y;
        const double z = candidate.origin.z - entity.origin.z;
        const double length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-9) return false;
        direction = {x / length, y / length, z / length};
        return true;
    }
    // VRAD warns and carries on with whatever normal the light already had.
    // Falling back to the angles direction is the closest usable equivalent.
    return false;
}

hammer::vmf::Vec3 sourceLightDirection(const hammer::vmf::EntityMarker& entity)
{
    // Match Source's SetupLightNormalFromProps. Do not use renderAngles(): the
    // lightprop helper deliberately reverses pitch for editor model display,
    // which is not the direction used by VRAD/Hammer lighting.
    double yaw = propertyNumber(entity, "angle", 0.0);
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    if (yaw == -1.0) {
        z = 1.0;
    } else if (yaw == -2.0) {
        z = -1.0;
    } else {
        if (std::abs(yaw) < 1e-9) yaw = entity.angles.y;
        const double yawRadians = yaw * 3.14159265358979323846 / 180.0;
        x = std::cos(yawRadians);
        y = std::sin(yawRadians);
    }

    double pitch = propertyNumber(entity, "pitch", 0.0);
    if (std::abs(pitch) < 1e-9) pitch = entity.angles.x;
    const double pitchRadians = pitch * 3.14159265358979323846 / 180.0;
    z = std::sin(pitchRadians);
    x *= std::cos(pitchRadians);
    y *= std::cos(pitchRadians);
    return {x, y, z};
}

// VRAD's SetLightFalloffParams. The _fifty_percent_distance path is not just a
// convenience over _constant/_linear/_quadratic_attn: it takes a completely
// different branch, and in particular it does NOT apply the
// (c + 100l + 10000q) pre-scale that the old-style path does.
void applyLightFalloff(const hammer::vmf::EntityMarker& entity, RayTracingLight& light)
{
    const double d50 = propertyNumber(entity, "_fifty_percent_distance", 0.0);
    light.fadeControls = {1.0e22f, 0.0f, -1.0f, 0.0f};

    if (d50 != 0.0) {
        double d0 = propertyNumber(entity, "_zero_percent_distance", 0.0);
        if (d0 < d50) d0 = 2.0 * d50;
        double a = 0.0, b = 1.0, c = 0.0;
        solveInverseQuadraticMonotonic(0.0, 1.0, d50, 2.0, d0, 256.0, a, b, c);
        // Enforcing monotonicity can pull the curve off the authored 50%
        // point; rescale so that point is exact regardless.
        const double v50 = c + d50 * (b + d50 * a);
        if (std::abs(v50) > 1e-12) {
            const double scale = 2.0 / v50;
            a *= scale; b *= scale; c *= scale;
        }
        light.attenuationOuter[0] = static_cast<float>(c);
        light.attenuationOuter[1] = static_cast<float>(b);
        light.attenuationOuter[2] = static_cast<float>(a);

        if (propertyNumber(entity, "_hardfalloff", 0.0) != 0.0) {
            light.fadeControls[2] = static_cast<float>(d0);
            light.fadeControls[1] = static_cast<float>(0.75 * d0 + 0.25 * d50);
        } else if (std::abs(a) > 0.0) {
            // Past the quadratic's minimum the 1/x term starts brightening
            // again. Freeze it there and fade to nothing by 10x that distance.
            const double capDistance = b / (-2.0 * a);
            if (capDistance > 0.0) {
                light.fadeControls[0] = static_cast<float>(capDistance);
                light.fadeControls[1] = static_cast<float>(capDistance);
                light.fadeControls[2] = static_cast<float>(10.0 * capDistance);
            }
        }
        return;
    }

    float c = float(propertyNumber(entity, "_constant_attn", 0.0));
    float l = float(propertyNumber(entity, "_linear_attn", 0.0));
    float q = float(propertyNumber(entity, "_quadratic_attn", 0.0));
    if (c < 1e-6f) c = 0.0f;
    if (l < 1e-6f) l = 0.0f;
    if (q < 1e-6f) q = 0.0f;
    if (c == 0.0f && l == 0.0f && q == 0.0f) c = 1.0f;
    light.attenuationOuter[0] = c;
    light.attenuationOuter[1] = l;
    light.attenuationOuter[2] = q;
    // "scale intensity for unit 100 distance": VRAD pre-multiplies the authored
    // colour by the falloff evaluated at 100 units, so an authored brightness
    // means the same thing whichever attenuation curve is in use.
    const float ratio = c + 100.0f * l + 10000.0f * q;
    if (ratio > 0.0f) light.colorIntensity[3] *= ratio;
}

void appendMapLights(const hammer::vmf::Scene& scene, RayTracingScene& output,
                     bool hdr)
{
    bool hasEnvironment=false;
    for (const auto& entity: scene.entities) {
        bool point=entity.classname=="light";
        bool spot=entity.classname=="light_spot";
        bool environment=entity.classname=="light_environment";
        if (!point && !spot && !environment) continue;
        RayTracingLight light;
        auto direction=sourceLightDirection(entity);
        targetedLightDirection(scene, entity, direction);
        light.positionType={float(entity.origin.x),float(entity.origin.y),float(entity.origin.z),
                            environment?4.0f:(spot?1.0f:0.0f)};
        // "Maximum Distance" (_distance) is deliberately NOT a cutoff.
        //
        // VRAD sets dl->light.radius from it and copies that into the
        // worldlights lump, but never reads it back in any lighting
        // computation - it is non-functional in Source 2013 and later, and the
        // documentation says to use the 50/0 percent falloff distances
        // instead. Honouring it as a hard range made every light authored with
        // _distance stop short in the preview at a boundary a compiled map
        // does not have.
        light.directionRange={float(direction.x),float(direction.y),float(direction.z),0.0f};

        // ParseLightGeneric: the HDR colour wins outright when the map author
        // supplied one, and _lightscaleHDR then scales whichever was used.
        std::array<float, 4> intensity{};
        if (hdr && lightForString(entityProperty(entity, "_lightHDR"), intensity))
            light.colorIntensity = intensity;
        else
            light.colorIntensity = parseLightColor(entityProperty(entity, "_light"),
                                                   {1.0f, 0.96f, 0.88f, 1.0f}, hdr);
        // VRAD reads _lightscaleHDR; the entity documentation calls the same
        // field _lightHDRscale. Maps in the wild carry both spellings, so
        // accept either rather than silently ignoring half of them.
        if (hdr) {
            double hdrScale = propertyNumber(entity, "_lightscaleHDR", 1.0);
            if (!entityProperty(entity, "_lightscaleHDR"))
                hdrScale = propertyNumber(entity, "_lightHDRscale", 1.0);
            light.colorIntensity[3] *= float(hdrScale);
        }

        // Appearance/Custom Appearance. A pattern overrides the style preset.
        if (const auto pattern = entityProperty(entity, "pattern"); pattern && !pattern->empty()) {
            light.colorIntensity[3] *= float(lightPatternScale(*pattern));
        } else if (const auto style = entityProperty(entity, "style")) {
            const int preset = static_cast<int>(propertyNumber(entity, "style", 0.0));
            if (preset != 0)
                light.colorIntensity[3] *= float(lightPatternScale(lightStylePattern(preset)));
        }

        if (environment) {
            light.attenuationOuter={1.0f,0.0f,0.0f,-1.0f};
            light.directionRange[3]=131072.0f;
            // SunSpreadAngle is the half-angle of the sun's disc in degrees;
            // VRAD stores its sine as g_SunAngularExtent and jitters every sun
            // shadow ray by it. Leaving it at zero is what pinned the preview
            // to a single ray per sample and gave every sun shadow a hard edge
            // no matter how far the caster was from the receiver.
            const double spread = propertyNumber(entity, "SunSpreadAngle", 0.0);
            light.innerControls[2] =
                float(std::sin(std::clamp(spread, 0.0, 90.0) * 3.14159265358979323846 / 180.0));
        } else {
            applyLightFalloff(entity, light);
            // ParseLightSpot, key for key. The defaults are not the FGD's 30/45:
            // VRAD treats an absent OR zero _inner_cone as 10 degrees and an
            // absent or zero _cone as "same as the inner cone", then widens the
            // outer cone to at least the inner one. A map that leaves the keys
            // off gets a 10-degree hard-edged pencil beam in a compile, not the
            // wide soft cone the old defaults produced here.
            double inner = propertyNumber(entity, "_inner_cone", 0.0);
            if (inner == 0.0) inner = 10.0;
            double outer = propertyNumber(entity, "_cone", 0.0);
            if (outer == 0.0) outer = inner;
            if (outer < inner) outer = inner;
            double exponent = propertyNumber(entity, "_exponent", 0.0);
            // "This is a point light if stop dots are 180": a 180/180 spotlight
            // is demoted to an omnidirectional light with no cone and no
            // exponent at all.
            if (spot && inner == 180.0 && outer == 180.0) {
                spot = false;
            } else {
                // Clamped to 90 because that is all DX8 could handle, and VRAD
                // still clamps regardless of the target renderer.
                inner = std::min(inner, 90.0);
                outer = std::min(outer, 90.0);
            }
            light.positionType[3] = spot ? 1.0f : 0.0f;
            light.attenuationOuter[3] =
                spot?float(std::cos(outer*3.14159265358979323846/180.0)):-1.0f;
            light.innerControls[0]=spot?float(std::cos(inner*3.14159265358979323846/180.0)):1.0f;
            // ParseLightSpot reads _exponent straight through; 0 (the default)
            // means the cone ramp stays linear.
            light.innerControls[1]=spot?float(exponent):0.0f;
        }
        output.lights.push_back(light);
        if (environment) {
            // ParseLightEnvironment. An absent _ambient is NOT "no ambient":
            // VRAD falls back to half the sun's own intensity, which is what
            // keeps a sunlit exterior from reading as pitch black in shadow.
            RayTracingLight ambient;
            ambient.positionType[3]=3.0f;
            std::array<float, 4> ambientIntensity{};
            if (hdr && lightForString(entityProperty(entity, "_ambientHDR"), ambientIntensity)) {
                ambient.colorIntensity = ambientIntensity;
            } else if (lightForString(entityProperty(entity, "_ambient"), ambientIntensity, hdr)) {
                ambient.colorIntensity = ambientIntensity;
            } else {
                ambient.colorIntensity = light.colorIntensity;
                ambient.colorIntensity[3] *= 0.5f;
            }
            if (hdr)
                ambient.colorIntensity[3] *=
                    float(propertyNumber(entity, "_AmbientScaleHDR", 1.0));
            output.lights.push_back(ambient);
            hasEnvironment=true;
        }
    }
    if (!hasEnvironment) {
        RayTracingLight fallback;
        fallback.positionType[3]=2.0f;
        fallback.directionRange={0.42f,-0.36f,-0.83f,131072.0f};
        fallback.colorIntensity={1.0f,0.96f,0.88f,1.25f};
        fallback.attenuationOuter={1.0f,0.0f,0.0f,-1.0f};
        output.lights.push_back(fallback);
    }
}

float dot(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return static_cast<float>(a.x * b.x + a.y * b.y + a.z * b.z);
}

hammer::vmf::Vec3 cross(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

hammer::vmf::Vec3 subtract(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

hammer::vmf::Vec3 normalized(hammer::vmf::Vec3 value,
                             hammer::vmf::Vec3 fallback = {0.0, 0.0, 1.0})
{
    const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!std::isfinite(length) || length < 1e-12) return fallback;
    value.x /= length;
    value.y /= length;
    value.z /= length;
    return value;
}

float safeScale(double value)
{
    return static_cast<float>(std::abs(value) < 1e-9 ? 0.25 : value);
}

RayTracingVertex makeVertex(const hammer::vmf::Vec3& position,
                            const hammer::vmf::Vec3& normal,
                            const hammer::vmf::Vec3& tangent,
                            float tangentSign,
                            float u, float v, float u2, float v2,
                            float blendAlpha)
{
    const hammer::vmf::Vec3 n = normalized(normal);
    hammer::vmf::Vec3 t = tangent;
    const double projection = t.x * n.x + t.y * n.y + t.z * n.z;
    t = normalized({t.x - n.x * projection,
                    t.y - n.y * projection,
                    t.z - n.z * projection},
                   std::abs(n.z) < 0.9 ? hammer::vmf::Vec3{-n.y, n.x, 0.0}
                                       : hammer::vmf::Vec3{0.0, -n.z, n.y});
    RayTracingVertex vertex;
    vertex.position = {static_cast<float>(position.x), static_cast<float>(position.y),
                       static_cast<float>(position.z), 1.0f};
    vertex.normal = {static_cast<float>(n.x), static_cast<float>(n.y),
                     static_cast<float>(n.z), 0.0f};
    vertex.tangent = {static_cast<float>(t.x), static_cast<float>(t.y),
                      static_cast<float>(t.z), tangentSign};
    vertex.texCoord = {u, v, u2, v2};
    vertex.surface = {std::clamp(blendAlpha, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f};
    return vertex;
}

// Expand one settled rope polyline into a camera-facing ribbon, writing the
// 6 * (pointCount - 1) vertices the strand's triangles reference. Shared by the
// initial build and the camera-driven refresh so both produce identical
// geometry, and mirrors Hardware3DViewport::drawRopes so the two viewports
// agree on where a rope is.
void expandRopeRibbon(const hammer::vmf::Vec3* points, std::size_t pointCount,
                      double halfWidth, double vPerUnit,
                      const hammer::vmf::Vec3& cameraPosition,
                      const hammer::vmf::Vec3& cameraRight,
                      RayTracingVertex* out)
{
    if (pointCount < 2) return;
    const auto vectorLength = [](const hammer::vmf::Vec3& value) {
        return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    };
    std::vector<hammer::vmf::Vec3> sides(pointCount);
    std::vector<float> texV(pointCount, 0.0f);
    double travelled = 0.0;
    for (std::size_t point = 0; point < pointCount; ++point) {
        const hammer::vmf::Vec3& position = points[point];
        if (point > 0) travelled += vectorLength(subtract(position, points[point - 1]));
        texV[point] = static_cast<float>(travelled * vPerUnit);

        const hammer::vmf::Vec3 along = subtract(points[point + 1 < pointCount ? point + 1 : point],
                                                 points[point > 0 ? point - 1 : point]);
        const hammer::vmf::Vec3 toEye = subtract(cameraPosition, position);
        hammer::vmf::Vec3 side = cross(along, toEye);
        // Looking straight down the rope: any perpendicular keeps it visible.
        if (vectorLength(side) < 1e-6) side = cameraRight;
        side = normalized(side);
        sides[point] = {side.x * halfWidth, side.y * halfWidth, side.z * halfWidth};
    }

    std::size_t vertex = 0;
    for (std::size_t segment = 0; segment + 1 < pointCount; ++segment) {
        const hammer::vmf::Vec3& start = points[segment];
        const hammer::vmf::Vec3& end = points[segment + 1];
        const hammer::vmf::Vec3 normal = normalized(subtract(cameraPosition, start));
        const hammer::vmf::Vec3 tangent = normalized(sides[segment]);
        const auto corner = [&](const hammer::vmf::Vec3& base, const hammer::vmf::Vec3& side,
                                double sign, float v) {
            return makeVertex({base.x + side.x * sign, base.y + side.y * sign,
                               base.z + side.z * sign},
                              normal, tangent, 1.0f,
                              sign < 0.0 ? 0.0f : 1.0f, v,
                              sign < 0.0 ? 0.0f : 1.0f, v, 0.0f);
        };
        const float v0 = texV[segment];
        const float v1 = texV[segment + 1];
        out[vertex++] = corner(start, sides[segment], -1.0, v0);
        out[vertex++] = corner(start, sides[segment], 1.0, v0);
        out[vertex++] = corner(end, sides[segment + 1], 1.0, v1);
        out[vertex++] = corner(start, sides[segment], -1.0, v0);
        out[vertex++] = corner(end, sides[segment + 1], 1.0, v1);
        out[vertex++] = corner(end, sides[segment + 1], -1.0, v1);
    }
}

// Expand one detail sprite into its camera-facing quad, writing the six
// vertices its two triangles reference. Shared by the build and the
// camera-driven refresh, and identical to what the OpenGL viewport draws.
void expandDetailSprite(const hammer::assets::DetailPropInstance& prop,
                        const hammer::vmf::Vec3& viewOrigin,
                        const hammer::assets::DetailPropFade& fade,
                        RayTracingVertex* out)
{
    // Past cl_detaildist the engine does not draw the prop at all. There is no
    // per-vertex alpha here to fade with, so the quad collapses to a point:
    // the span stays in place for the next camera move, but the triangles
    // cover nothing and can never be hit.
    if (hammer::assets::detailPropAlpha(prop.origin, viewOrigin, fade) <= 0.0f) {
        const RayTracingVertex collapsed = makeVertex(prop.origin, {0.0, 0.0, 1.0},
                                                      {1.0, 0.0, 0.0}, 1.0f, 0, 0, 0, 0, 0);
        for (int index = 0; index < 6; ++index) out[index] = collapsed;
        return;
    }
    const hammer::assets::DetailSpriteQuad quad =
        hammer::assets::detailSpriteQuad(prop, viewOrigin);
    const hammer::vmf::Vec3 tangent = subtract(quad.corners[3], quad.corners[0]);
    const auto corner = [&](std::size_t index) {
        return makeVertex(quad.corners[index], quad.normal, tangent, 1.0f,
                          quad.texCoords[index][0], quad.texCoords[index][1],
                          quad.texCoords[index][0], quad.texCoords[index][1], 0.0f);
    };
    out[0] = corner(0);
    out[1] = corner(1);
    out[2] = corner(2);
    out[3] = corner(0);
    out[4] = corner(2);
    out[5] = corner(3);
}

std::pair<hammer::vmf::Vec3, float> brushTangent(
    const hammer::vmf::Vec3& normal,
    const hammer::vmf::TextureAxis& uAxis,
    const hammer::vmf::TextureAxis& vAxis)
{
    const hammer::vmf::Vec3 n = normalized(normal);
    hammer::vmf::Vec3 t{uAxis.direction.x / safeScale(uAxis.scale),
                        uAxis.direction.y / safeScale(uAxis.scale),
                        uAxis.direction.z / safeScale(uAxis.scale)};
    const double tProjection = t.x * n.x + t.y * n.y + t.z * n.z;
    t = normalized({t.x - n.x * tProjection,
                    t.y - n.y * tProjection,
                    t.z - n.z * tProjection});
    hammer::vmf::Vec3 b{vAxis.direction.x / safeScale(vAxis.scale),
                        vAxis.direction.y / safeScale(vAxis.scale),
                        vAxis.direction.z / safeScale(vAxis.scale)};
    const double bProjection = b.x * n.x + b.y * n.y + b.z * n.z;
    b = normalized({b.x - n.x * bProjection,
                    b.y - n.y * bProjection,
                    b.z - n.z * bProjection});
    const hammer::vmf::Vec3 bitangentFromFrame = cross(n, t);
    const float sign = dot(bitangentFromFrame, b) < 0.0f ? -1.0f : 1.0f;
    return {t, sign};
}

std::uint32_t materialFlags(const hammer::assets::Material& material)
{
    std::uint32_t flags = 0;
    if (material.image2.valid()) flags |= RtHasSecondaryTexture;
    if (material.blended) flags |= RtBlended;
    // Same gate the OpenGL viewport applies (Hardware3DViewport's `hasBump`).
    // Two exclusions matter and neither is optional:
    //
    //   editorBumpMapSupported - a per-shader capability. UnlitGeneric and
    //     friends have no bump stage, so a $bumpmap key on one is inert in the
    //     engine and must be inert here.
    //   ssBump - $bumpmap holds tangent-space normal vectors, but $ssbump does
    //     not: it stores three self-shadowed radiosity-basis coefficients, so
    //     decoding its texels as a normal (xyz*2-1) yields a meaningless
    //     direction. The RT path was doing exactly that, which is why ssbump
    //     world brushes (most of the HL2 era) looked nothing like they do under
    //     OpenGL.
    if (material.bumpMapped && material.bumpImage.valid() &&
        material.editorBumpMapSupported && !material.ssBump)
        flags |= RtHasBumpMap;
    if (material.phong && material.editorPhongSupported) flags |= RtPhong;
    if (material.specular && material.editorSpecularSupported) flags |= RtSpecular;
    if (material.selfIllum && material.editorSelfIllumSupported) flags |= RtSelfIllum;
    if (material.selfIllumFresnel) flags |= RtSelfIllumFresnel;
    if (material.hasSelfIllumMask) flags |= RtHasSelfIllumMask;
    if (material.hasLightWarpTexture) flags |= RtHasLightWarp;
    if (material.halfLambert) flags |= RtHalfLambert;
    if (material.rimLight && material.editorRimLightSupported) flags |= RtRimLight;
    if (material.water) flags |= RtWater;
    if (material.translucent) flags |= RtTranslucent;
    if (material.color2Active) flags |= RtColor2Active;
    if (material.blendTintByBaseAlpha) flags |= RtBlendTintByBaseAlpha;
    if (material.phongMaskFromBaseAlpha) flags |= RtPhongMaskFromBaseAlpha;
    if (material.envMapMaskFromBaseAlpha) flags |= RtEnvMaskFromBaseAlpha;
    if (material.envMapMaskFromNormalAlpha) flags |= RtEnvMaskFromNormalAlpha;
    if (material.invertPhongMask) flags |= RtInvertSpecularMask;
    if (material.hasPhongExponentTexture) flags |= RtHasExponentMap;
    if (material.phongExponentOverride) flags |= RtPhongExponentOverride;
    if (material.phongAlbedoTint) flags |= RtPhongAlbedoTint;
    if (material.rimMaskFromExponentAlpha) flags |= RtRimMaskFromExponentAlpha;
    if (material.highEnergyEffect) flags |= RtHighEnergy;
    if (material.uberEffect) flags |= RtUber;
    if (material.waterHasFlowMap) flags |= RtHasFlowMap;
    if (material.waterNoFresnel) flags |= RtWaterNoFresnel;
    if (material.waterMultiTexture) flags |= RtWaterMultiTexture;
    if (material.hasEnvMapCube || material.envMapUsesMapCubemap) flags |= RtHasEnvironmentMap;
    if (material.decalModulate) flags |= RtDecalModulate;
    if (material.compileTrigger) flags |= RtCompileTrigger;
    return flags;
}

void configureMaterial(PendingMaterial& pending, AtlasBuilder& atlas, double animationSeconds)
{
    const hammer::assets::Material& material = *pending.source;
    // Deduplicate by the authored texture source rather than VMT name. Many TF2
    // materials share the same VTF; storing one copy per VMT wastes large amounts
    // of texture-array VRAM. Fall back to material name for generated images.
    auto textureKey = [&](std::string_view role, const std::string& source) {
        return std::string(role) + ":" + (source.empty() ? material.name : source);
    };
    pending.base = atlas.add(textureKey("base", material.baseTexture), material.image);
    if (material.image2.valid())
        pending.secondary = atlas.add(textureKey("base2", material.baseTexture2), material.image2);
    if (material.detailImage.valid())
        pending.detail = atlas.add(textureKey("detail", material.detailTexture), material.detailImage);
    if (material.water && material.waterNormalImage.valid()) {
        pending.bump = atlas.add(textureKey("water-normal", material.bumpMap), material.waterNormalImage);
    } else if (material.bumpImage.valid()) {
        const hammer::assets::Image* bumpImage = &material.bumpImage;
        if (material.bumpFrames.size() > 1 && material.bumpAnimationFrameRate > 0.0f) {
            const std::size_t frame = static_cast<std::size_t>(std::floor(
                animationSeconds * material.bumpAnimationFrameRate)) % material.bumpFrames.size();
            bumpImage = &material.bumpFrames[frame];
        }
        pending.bump = atlas.add(textureKey("bump", material.bumpMap) +
                                 "#frame" + std::to_string(
                                     material.bumpFrames.size() > 1 && material.bumpAnimationFrameRate > 0.0f
                                         ? static_cast<std::size_t>(std::floor(animationSeconds * material.bumpAnimationFrameRate)) % material.bumpFrames.size()
                                         : 0u),
                                 *bumpImage);
    }
    if (material.phongExponentImage.valid())
        pending.exponent = atlas.add(textureKey("exponent", material.phongExponentTexture), material.phongExponentImage);
    if (material.selfIllumMaskImage.valid())
        pending.selfIllum = atlas.add(textureKey("selfillum", material.selfIllumMask), material.selfIllumMaskImage);
    if (material.lightWarpImage.valid())
        pending.lightWarp = atlas.add(textureKey("lightwarp", material.lightWarpTexture), material.lightWarpImage);
    if (material.waterFlowImage.valid())
        pending.flow = atlas.add(textureKey("flow", material.waterFlowMap), material.waterFlowImage);
    if (material.hasEnvMapCube && material.envMapCube.valid()) {
        const std::string envSource = material.envMapSource.empty() ? material.envMap : material.envMapSource;
        for (std::size_t face = 0; face < pending.environment.size(); ++face)
            pending.environment[face] = atlas.add(
                textureKey("env" + std::to_string(face), envSource), material.envMapCube.faces[face]);
    }

    RayTracingMaterial& gpu = pending.gpu;
    gpu.color2 = {material.color2[0], material.color2[1], material.color2[2],
                  material.blendTintColorOverBase};
    gpu.phongTintExponent = {material.phongTint[0], material.phongTint[1],
                             material.phongTint[2], material.phongExponent};
    gpu.envTintBoost = {material.envMapTint[0], material.envMapTint[1],
                        material.envMapTint[2], material.phongBoost};
    gpu.selfIllumTintSpecular = {material.selfIllumTint[0], material.selfIllumTint[1],
                                 material.selfIllumTint[2], material.specularStrength};
    gpu.phongFresnelRimExponent = {material.phongFresnelRanges[0],
                                   material.phongFresnelRanges[1],
                                   material.phongFresnelRanges[2],
                                   material.rimLightExponent};
    gpu.selfIllumFresnelRimBoost = {material.selfIllumFresnelMinMaxExp[0],
                                    material.selfIllumFresnelMinMaxExp[1],
                                    material.selfIllumFresnelMinMaxExp[2],
                                    material.rimLightBoost};
    gpu.detailAlphaControls = {material.detailScale, material.detailBlendFactor,
                               static_cast<float>(material.detailBlendMode),
                               material.alphaTestReference};
    gpu.waterFogAlpha = {material.waterFogColor[0], material.waterFogColor[1],
                         material.waterFogColor[2], material.waterAlpha};
    gpu.waterRefractAmount = {material.waterRefractTint[0], material.waterRefractTint[1],
                              material.waterRefractTint[2], material.waterRefractAmount};
    gpu.waterReflectAmount = {material.waterReflectTint[0], material.waterReflectTint[1],
                              material.waterReflectTint[2], material.waterReflectAmount};
    gpu.waterControls = {material.waterFresnelReflectance,
                         material.waterReflectBlendFactor,
                         material.waterFogStart, material.waterFogEnd};
    gpu.waterScaleScroll1 = {material.waterScale[0], material.waterScale[1],
                             material.waterScroll1[0], material.waterScroll1[1]};
    gpu.waterScroll2NormalCycle = {material.waterScroll2[0], material.waterScroll2[1],
                                   material.waterNormalScale, material.waterFlowCycleRate};
    gpu.waterFlow = {material.waterFlowDistance, material.waterFlowMapScale,
                     material.waterFlowNormalUvScale, 0.0f};
    gpu.transparencyControls = {material.alpha, 0.0f, 0.0f, 0.0f};
    gpu.flags[0] = materialFlags(material);
    if (material.hasDetailTexture) gpu.flags[1] |= Rt2HasDetailTexture;
    if (material.alphaTest) gpu.flags[1] |= Rt2AlphaTest;
}

void applyRects(PendingMaterial& pending, const std::vector<std::array<float, 4>>& rects)
{
    auto rect = [&](int item) -> std::array<float, 4> {
        return item >= 0 && item < static_cast<int>(rects.size())
            ? rects[static_cast<std::size_t>(item)] : std::array<float, 4>{};
    };
    pending.gpu.baseRect = rect(pending.base);
    pending.gpu.secondaryRect = rect(pending.secondary);
    pending.gpu.detailRect = rect(pending.detail);
    pending.gpu.bumpRect = rect(pending.bump);
    pending.gpu.exponentRect = rect(pending.exponent);
    pending.gpu.selfIllumRect = rect(pending.selfIllum);
    pending.gpu.lightWarpRect = rect(pending.lightWarp);
    pending.gpu.flowRect = rect(pending.flow);
    for (std::size_t face = 0; face < pending.environment.size(); ++face)
        pending.gpu.environmentRects[face] = rect(pending.environment[face]);
}

} // namespace

std::uint32_t materialFlagsForMaterial(const hammer::assets::Material& material)
{
    return materialFlags(material);
}

std::uint32_t shadowFlagsForMaterialName(std::string_view normalizedMaterial)
{
    return shadowFlagsForMaterial(normalizedMaterial);
}

bool isEditorHelperModelPath(std::string_view modelPath)
{
    return isEditorHelperModel(modelPath);
}

std::vector<RayTracingLight> mapLightsForScene(const hammer::vmf::Scene& scene,
                                              bool hdrLighting)
{
    RayTracingScene collected;
    appendMapLights(scene, collected, hdrLighting);
    return std::move(collected.lights);
}

RayTracingSceneBuilder::RayTracingSceneBuilder(
    std::shared_ptr<hammer::assets::MaterialSystem> materials,
    std::shared_ptr<hammer::assets::StudioModelSystem> studioModels)
    : materials_(std::move(materials)), studioModels_(std::move(studioModels))
{
}

RayTracingScene RayTracingSceneBuilder::build(
    const hammer::vmf::Scene& scene,
    const RayTracingBuildOptions& options) const
{
    RayTracingScene output;
    output.toneMap = scene.toneMap;
    appendMapLights(scene, output, options.hdrLighting);
    if (!materials_) {
        output.error = "Ray-traced preview has no active material system";
        return output;
    }

    // Source color-correction lookups are raw 32x32x32 RGB cubes. Keep
    // them outside the material atlas: the engine samples them as independent
    // 3D lookup textures in gamma space, and only the four strongest active
    // corrections are blended for a view.
    if (const auto fileSystem = materials_->fileSystem()) {
        constexpr std::size_t LutSide = 32;
        constexpr std::size_t LutTexels = LutSide * LutSide * LutSide;
        constexpr std::size_t LutBytes = LutTexels * 3;
        std::unordered_map<std::string, std::uint32_t> loadedLuts;
        auto loadLookup = [&](const std::string& authored) -> std::optional<std::uint32_t> {
            std::string normalized = hammer::assets::GameFileSystem::normalizeResourcePath(authored);
            if (normalized.empty()) return std::nullopt;
            std::vector<std::string> candidates;
            candidates.push_back(normalized);
            if (normalized.rfind("materials/", 0) != 0)
                candidates.push_back("materials/" + normalized);
            if (normalized.size() < 4 || normalized.substr(normalized.size() - 4) != ".raw") {
                candidates.push_back(normalized + ".raw");
                if (normalized.rfind("materials/", 0) != 0)
                    candidates.push_back("materials/" + normalized + ".raw");
            }
            for (const std::string& candidate : candidates) {
                if (const auto found = loadedLuts.find(candidate); found != loadedLuts.end())
                    return found->second;
                const auto bytes = fileSystem->readFile(candidate);
                if (!bytes || bytes->size() != LutBytes) continue;
                const std::uint32_t index = static_cast<std::uint32_t>(
                    output.colorCorrectionLutTexels.size() / LutTexels);
                output.colorCorrectionLutTexels.reserve(
                    output.colorCorrectionLutTexels.size() + LutTexels);
                for (std::size_t texel = 0; texel < LutTexels; ++texel) {
                    const std::uint32_t r = (*bytes)[texel * 3 + 0];
                    const std::uint32_t g = (*bytes)[texel * 3 + 1];
                    const std::uint32_t b = (*bytes)[texel * 3 + 2];
                    output.colorCorrectionLutTexels.push_back(
                        r | (g << 8u) | (b << 16u) | 0xff000000u);
                }
                loadedLuts.emplace(candidate, index);
                return index;
            }
            return std::nullopt;
        };

        for (const hammer::vmf::ColorCorrectionDefinition& source : scene.colorCorrections) {
            const auto lut = loadLookup(source.filename);
            if (!lut) continue;
            RayTracingColorCorrection correction;
            correction.originWeight = {static_cast<float>(source.origin.x),
                                       static_cast<float>(source.origin.y),
                                       static_cast<float>(source.origin.z),
                                       static_cast<float>(source.weight)};
            if (source.volume) {
                correction.minimum = {static_cast<float>(source.minimum.x),
                                      static_cast<float>(source.minimum.y),
                                      static_cast<float>(source.minimum.z), 0.0f};
                correction.maximum = {static_cast<float>(source.maximum.x),
                                      static_cast<float>(source.maximum.y),
                                      static_cast<float>(source.maximum.z), 1.0f};
            } else {
                correction.minimum = {static_cast<float>(source.minFalloff),
                                      static_cast<float>(source.maxFalloff), 0.0f, 0.0f};
                correction.maximum = {0.0f, 0.0f, 0.0f, 0.0f};
            }
            correction.lutIndex = *lut;
            correction.enabled = source.enabled;
            output.colorCorrections.push_back(correction);
        }
    }

    AtlasBuilder atlas;
    std::vector<PendingMaterial> pendingMaterials;
    std::unordered_map<std::string, std::uint32_t> materialIndices;

    auto materialIndex = [&](std::string_view materialName) -> std::optional<std::uint32_t> {
        const std::string normalized = hammer::vmf::normalizeMaterialPath(materialName);
        if (normalized.empty()) return std::nullopt;
        if (const auto found = materialIndices.find(normalized); found != materialIndices.end())
            return found->second;
        auto material = materials_->material(normalized);
        if (!material || material->missing || !material->image.valid()) return std::nullopt;
        PendingMaterial pending;
        pending.source = std::move(material);
        if (pending.source->bumpFrames.size() > 1 &&
            pending.source->bumpAnimationFrameRate > 0.0f) {
            output.hasAnimatedContent = true;
            output.hasAnimatedMaterialContent = true;
        }
        configureMaterial(pending, atlas, options.animationSeconds);
        const std::uint32_t index = static_cast<std::uint32_t>(pendingMaterials.size());
        materialIndices.emplace(normalized, index);
        pendingMaterials.push_back(std::move(pending));
        return index;
    };

    auto rayFlagsForMaterial = [&](std::string_view normalizedMaterial) {
        std::uint32_t flags = shadowFlagsForMaterial(normalizedMaterial);
        if (isSkyToolMaterial(normalizedMaterial)) flags |= TriangleSky;
        // View -> Tool Textures controls primary/reflection visibility only.
        // Hidden tools stay in the AS so their shadow behaviour is unchanged by
        // whether the editor is drawing them. tools/toolsblack is exempt: it
        // absorbs every ray unconditionally, so hiding tool textures must not
        // turn it into a window.
        if (isToolMaterial(normalizedMaterial) &&
            !isAlwaysOpaqueToolMaterial(normalizedMaterial) &&
            options.hiddenToolTextures.contains(std::string(normalizedMaterial)))
            flags |= TriangleNoPrimary;
        return flags;
    };

    auto appendTriangle = [&](const RayTracingVertex& a,
                              const RayTracingVertex& b,
                              const RayTracingVertex& c,
                              std::uint32_t material,
                              std::uint32_t flags,
                              std::uint32_t rectIndex = 0u) {
        const std::uint32_t first = static_cast<std::uint32_t>(output.vertices.size());
        output.vertices.push_back(a);
        output.vertices.push_back(b);
        output.vertices.push_back(c);
        // Source scene geometry and the Vulkan ray-query front-face test use
        // opposite winding conventions in this coordinate path. Reverse the
        // final two indices once here so every submitted primitive has the
        // same outward-facing side as the existing vertex normals and the
        // OpenGL selection geometry.
        output.indices.push_back(first + 0u);
        output.indices.push_back(first + 2u);
        output.indices.push_back(first + 1u);
        RayTracingTriangle triangle;
        triangle.data[0] = material;
        triangle.data[1] = flags;
        // Index into the radiosity patch rect table. The bake resolves a ray
        // hit back to the patch that received it through this, so a triangle
        // without it can neither send nor receive bounced light.
        triangle.data[2] = rectIndex;
        output.triangles.push_back(triangle);
    };

    // Radiosity patch grid. A face carries bounced light only if it is lit
    // geometry: tool, sky and water surfaces are excluded exactly as the
    // OpenGL viewport excludes them, since the caller owns the material system.
    {
        LightmapLayoutOptions layoutOptions;
        layoutOptions.displacementSolidMask = options.displacementSolidMask;
        layoutOptions.faceIsLit = [&](std::string_view normalizedMaterial) {
            if (isToolMaterial(normalizedMaterial)) return false;
            const auto material = materials_->material(std::string(normalizedMaterial));
            if (!material) return false;
            return !material->water && !material->translucent;
        };
        output.radiosity = buildRadiosityPatchData(scene, layoutOptions);
    }
    const RadRectTable& patchRects = output.radiosity.patchRects;  // read-only lookups

    for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            // CMapSolid::Render3D hides the non-displaced sides of a
            // displacement solid in the 3D views.
            if (hammer::vmf::isFaceMaskedByDisplacementSolid(
                    brush, face, options.displacementSolidMask)) {
                continue;
            }
            const std::string normalizedMaterial = hammer::vmf::normalizeMaterialPath(face.material);
            const auto index = materialIndex(normalizedMaterial);
            if (!index) continue;
            std::uint32_t shadowFlags = rayFlagsForMaterial(normalizedMaterial);
            // Attach this face to its radiosity patch rect, when it has one.
            std::uint32_t patchRectIndex = 0u;
            if (const auto found = patchRects.indices.find(faceKey(brush.id, face.sideId));
                found != patchRects.indices.end()) {
                patchRectIndex = found->second;
                shadowFlags |= TriangleHasLightmap;
                // buildRectTable cannot know material indices, so it leaves
                // identity.x at zero for the caller. The bounce solve reads a
                // patch's reflectivity through it; left unset, every surface in
                // the map would bounce the first material's colour and the one
                // thing radiosity exists to produce - colour bleed - would be
                // uniformly wrong.
                output.radiosity.patchRects.records[patchRectIndex].identity[0] = *index;
            }
            const hammer::assets::Material& material = *pendingMaterials[*index].source;
            const float primaryWidth = static_cast<float>(std::max(1, material.image.width));
            const float primaryHeight = static_cast<float>(std::max(1, material.image.height));
            const float secondaryWidth = static_cast<float>(
                material.image2.valid() ? material.image2.width : material.image.width);
            const float secondaryHeight = static_cast<float>(
                material.image2.valid() ? material.image2.height : material.image.height);
            const auto [tangent, tangentSign] = brushTangent(face.normal, face.uAxis, face.vAxis);

            auto convertBrush = [&](const hammer::vmf::Vec3& position,
                                    const hammer::vmf::Vec3& normal,
                                    double blendAlpha) {
                const float uPixels = dot(position, face.uAxis.direction) /
                                          safeScale(face.uAxis.scale) +
                                      static_cast<float>(face.uAxis.shift);
                const float vPixels = dot(position, face.vAxis.direction) /
                                          safeScale(face.vAxis.scale) +
                                      static_cast<float>(face.vAxis.shift);
                return makeVertex(position, normal, tangent, tangentSign,
                                  uPixels / primaryWidth, vPixels / primaryHeight,
                                  uPixels / std::max(1.0f, secondaryWidth),
                                  vPixels / std::max(1.0f, secondaryHeight),
                                  static_cast<float>(blendAlpha));
            };
            auto convertDisplacement = [&](const hammer::vmf::DisplacementVertex& source) {
                const hammer::vmf::Vec3 n = normalized(source.normal);
                const hammer::vmf::Vec3 tangentS = normalized(source.tangentS);
                const hammer::vmf::Vec3 tangentT = normalized(source.tangentT);
                // tangentT tracks textureV, which is no longer negated in the
                // vertex UVs, so the handedness test flips with it.
                const float sourceTangentSign =
                    dot(cross(n, tangentS), tangentT) < 0.0 ? 1.0f : -1.0f;
                const float u = static_cast<float>(source.textureU) / primaryWidth;
                const float v = static_cast<float>(source.textureV) / primaryHeight;
                // Source LightmappedGeneric feeds one normalized vBaseTexCoord
                // to both base textures; texture dimensions do not create a
                // second UV set for WorldVertexTransition.
                return makeVertex(source.position, source.normal, tangentS, sourceTangentSign,
                                  u, v, u, v, static_cast<float>(source.blendAlpha));
            };

            const bool displacement = face.displacement && face.displacementPower >= 1 &&
                                      !face.displacementVertices.empty() &&
                                      !face.displacementIndices.empty();
            if (displacement) {
                // Hammer's CMapDisp copies CCoreDispInfo::m_RenderIndices after
                // CreateWithoutLOD(). Consume that one authoritative index list
                // instead of independently tessellating the grid in the RT path.
                for (std::size_t tri = 0; tri + 2 < face.displacementIndices.size(); tri += 3) {
                    const std::size_t a = face.displacementIndices[tri + 0];
                    const std::size_t b = face.displacementIndices[tri + 1];
                    const std::size_t c = face.displacementIndices[tri + 2];
                    if (a >= face.displacementVertices.size() ||
                        b >= face.displacementVertices.size() ||
                        c >= face.displacementVertices.size()) continue;
                    const auto& va = face.displacementVertices[a];
                    const auto& vb = face.displacementVertices[b];
                    const auto& vc = face.displacementVertices[c];
                    appendTriangle(convertDisplacement(va),
                                   convertDisplacement(vb),
                                   convertDisplacement(vc), *index,
                                   TriangleTwoSided | TriangleDisplacement | shadowFlags,
                                   patchRectIndex);
                }
            } else if (face.vertices.size() >= 3) {
                auto faceVertex = [&](std::size_t vertexIndex) {
                    return vertexIndex < brush.vertices.size()
                        ? convertBrush(brush.vertices[vertexIndex], face.normal, 0.0)
                        : RayTracingVertex{};
                };
                const RayTracingVertex first = faceVertex(face.vertices.front());
                for (std::size_t corner = 1; corner + 1 < face.vertices.size(); ++corner) {
                    appendTriangle(first, faceVertex(face.vertices[corner]),
                                   faceVertex(face.vertices[corner + 1]), *index,
                                   (material.compileTrigger ? TriangleTwoSided : 0u) | shadowFlags,
                                   patchRectIndex);
                }
            }
        }
    }

    for (const hammer::vmf::EntityMarker& entity : scene.entities) {
        for (const hammer::vmf::ProjectedSurface& surface : entity.projectedSurfaces) {
            const std::string normalizedMaterial =
                hammer::vmf::normalizeMaterialPath(surface.material);
            const auto index = materialIndex(normalizedMaterial);
            if (!index || surface.triangles.size() < 3) continue;
            const std::uint32_t projectedShadowFlags =
                rayFlagsForMaterial(normalizedMaterial);
            for (std::size_t vertex = 0; vertex + 2 < surface.triangles.size(); vertex += 3) {
                std::array<RayTracingVertex, 3> triangle;
                for (int corner = 0; corner < 3; ++corner) {
                    const auto& source = surface.triangles[vertex + static_cast<std::size_t>(corner)];
                    const hammer::vmf::Vec3 edge = corner == 0
                        ? subtract(surface.triangles[vertex + 1].position, source.position)
                        : subtract(source.position, surface.triangles[vertex].position);
                    triangle[static_cast<std::size_t>(corner)] = makeVertex(
                        source.position, source.normal, edge, 1.0f,
                        static_cast<float>(source.u), static_cast<float>(source.v),
                        static_cast<float>(source.u), static_cast<float>(source.v), 0.0f);
                }
                appendTriangle(triangle[0], triangle[1], triangle[2], *index,
                               TriangleTwoSided | TriangleProjected | projectedShadowFlags);
            }
        }
    }

    if (studioModels_) {
        for (std::size_t entityIndex = 0; entityIndex < scene.entities.size(); ++entityIndex) {
            const hammer::vmf::EntityMarker& entity = scene.entities[entityIndex];
            if (entity.model.empty()) continue;
            const auto model = studioModels_->model(entity.model);
            if (!model || !model->valid) continue;

            int sequenceIndex = entity.animationSequenceIndex;
            bool entityAnimatedGeometry = false;
            if ((sequenceIndex < 0 || sequenceIndex >= model->sequenceCount()) &&
                !entity.animationSequence.empty())
                sequenceIndex = model->sequenceIndex(entity.animationSequence);
            if ((sequenceIndex < 0 || sequenceIndex >= model->sequenceCount()) &&
                entity.animateModel && entity.animationSequence.empty())
                sequenceIndex = model->sequenceCount() > 0 ? 0 : -1;

            double cycle = 0.0;
            if (sequenceIndex >= 0 && sequenceIndex < model->sequenceCount()) {
                const hammer::assets::StudioSequence& sequence =
                    model->sequences[static_cast<std::size_t>(sequenceIndex)];
                if (entity.animationCycle >= 0.0) {
                    cycle = entity.animationCycle;
                } else if (sequence.duration > 1e-6f) {
                    cycle = options.animationSeconds * entity.animationPlaybackRate /
                            sequence.duration;
                    if (entity.animationPlaybackRate < 0.0) cycle += 1.0;
                    if (std::abs(entity.animationPlaybackRate) > 1e-8)
                        entityAnimatedGeometry = true;
                }
            } else {
                sequenceIndex = -1;
            }

            std::vector<std::vector<hammer::assets::StudioVertex>> animatedMeshes;
            const bool animated = sequenceIndex >= 0 &&
                studioModels_->sampleAnimation(*model, sequenceIndex, cycle, animatedMeshes) &&
                animatedMeshes.size() == model->meshes.size();
            entityAnimatedGeometry = entityAnimatedGeometry && animated;
            if (entityAnimatedGeometry) {
                output.hasAnimatedContent = true;
                output.hasAnimatedGeometry = true;
            }
            const hammer::camera::SourceTransform transform =
                hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
            const int skin = model->normalizedSkin(entity.skin);

            for (std::size_t meshIndex = 0; meshIndex < model->meshes.size(); ++meshIndex) {
                const hammer::assets::StudioMesh& mesh = model->meshes[meshIndex];
                const std::string normalizedMaterial = hammer::vmf::normalizeMaterialPath(
                    model->materialForSkin(mesh.materialSlot, skin));
                const auto index = materialIndex(normalizedMaterial);
                if (!index) continue;
                // A helper model neither blocks light nor receives it: it is a
                // handle for the mapper, not geometry that exists in the map.
                const std::uint32_t helperFlags = isEditorHelperModel(entity.model)
                    ? (TriangleNoShadow | TriangleUnlit) : 0u;
                const std::uint32_t shadowFlags =
                    rayFlagsForMaterial(normalizedMaterial) | helperFlags |
                    (helperFlags == 0u ? TriangleStaticProp : 0u);
                const std::vector<hammer::assets::StudioVertex>& vertices =
                    animated ? animatedMeshes[meshIndex] : mesh.vertices;
                const std::size_t firstVertex = output.vertices.size();
                for (std::size_t vertex = 0; vertex + 2 < vertices.size(); vertex += 3) {
                    std::array<RayTracingVertex, 3> triangle;
                    for (int corner = 0; corner < 3; ++corner) {
                        const auto& source = vertices[vertex + static_cast<std::size_t>(corner)];
                        const hammer::vmf::Vec3 localPosition{source.x, source.y, source.z};
                        const hammer::vmf::Vec3 localNormal{source.nx, source.ny, source.nz};
                        const hammer::vmf::Vec3 localTangent{source.tx, source.ty, source.tz};
                        triangle[static_cast<std::size_t>(corner)] = makeVertex(
                            transform.transformPoint(localPosition),
                            transform.transformVector(localNormal),
                            transform.transformVector(localTangent), source.tangentSign,
                            source.u, source.v, source.u, source.v, 0.0f);
                    }
                    appendTriangle(triangle[0], triangle[1], triangle[2], *index,
                                   shadowFlags);
                }
                if (entityAnimatedGeometry && output.vertices.size() > firstVertex) {
                    output.animatedMeshSpans.push_back({
                        static_cast<std::uint32_t>(entityIndex),
                        static_cast<std::uint32_t>(meshIndex),
                        firstVertex,
                        output.vertices.size() - firstVertex});
                }
            }
        }
    }

    // Camera-facing point-entity helpers remain visible in RT. They are tagged
    // separately because Hammer's raster helper path always blends their image
    // alpha even when the helper VMT itself is not marked $translucent.
    const hammer::vmf::Vec3 cameraRight = hammer::camera::rightVector(options.camera);
    const hammer::vmf::Vec3 cameraUp = hammer::camera::upVector(options.camera);
    for (std::size_t entityIndex = 0;
         options.entityHelpers && entityIndex < scene.entities.size(); ++entityIndex) {
        const hammer::vmf::EntityMarker& entity = scene.entities[entityIndex];
        if (entity.sprite.empty() || !entity.model.empty()) continue;
        const std::string normalizedMaterial =
            hammer::vmf::normalizeMaterialPath(entity.sprite);
        const auto index = materialIndex(normalizedMaterial);
        if (!index) continue;
        // Editor point-entity sprites (including light helpers) are visual aids.
        // Keep them visible to primary rays but never let them occlude lighting/AO.
        const std::uint32_t shadowFlags =
            rayFlagsForMaterial(normalizedMaterial) | TriangleNoShadow;
        output.hasCameraFacingSprites = true;
        const auto& material = *pendingMaterials[*index].source;
        const double aspect = material.image.height > 0
            ? static_cast<double>(material.image.width) / material.image.height : 1.0;
        const double boundsWidth = std::max(entity.sizeMaximum.x - entity.sizeMinimum.x,
                                            entity.sizeMaximum.y - entity.sizeMinimum.y);
        const double boundsHeight = entity.sizeMaximum.z - entity.sizeMinimum.z;
        double height = std::max(16.0, std::abs(boundsHeight));
        double width = std::max(16.0, std::abs(boundsWidth));
        // Match Hardware3DViewport::drawBillboardSprite so RT helpers have the
        // same on-screen proportions as the non-RT viewport.
        if (width <= 16.0 && height > 16.0) width = height * aspect;
        else if (height <= 16.0 && width > 16.0)
            height = width / std::max(0.01, aspect);
        else if (std::abs(boundsWidth) <= 16.0 && std::abs(boundsHeight) <= 16.0) {
            height = 24.0;
            width = height * aspect;
        }
        auto point = [&](double x, double y) {
            return hammer::vmf::Vec3{
                entity.origin.x + cameraRight.x * x + cameraUp.x * y,
                entity.origin.y + cameraRight.y * x + cameraUp.y * y,
                entity.origin.z + cameraRight.z * x + cameraUp.z * y};
        };
        const hammer::vmf::Vec3 normal = normalized(subtract(options.camera.position, entity.origin));
        const double halfWidth = width * 0.5;
        const double halfHeight = height * 0.5;
        const std::size_t firstVertex = output.vertices.size();
        const RayTracingVertex bl = makeVertex(point(-halfWidth, -halfHeight), normal,
                                               cameraRight, 1.0f, 0, 1, 0, 1, 0);
        const RayTracingVertex br = makeVertex(point(halfWidth, -halfHeight), normal,
                                               cameraRight, 1.0f, 1, 1, 1, 1, 0);
        const RayTracingVertex tr = makeVertex(point(halfWidth, halfHeight), normal,
                                               cameraRight, 1.0f, 1, 0, 1, 0, 0);
        const RayTracingVertex tl = makeVertex(point(-halfWidth, halfHeight), normal,
                                               cameraRight, 1.0f, 0, 0, 0, 0, 0);
        appendTriangle(bl, br, tr, *index, TriangleTwoSided | TriangleSprite | shadowFlags);
        appendTriangle(bl, tr, tl, *index, TriangleTwoSided | TriangleSprite | shadowFlags);
        output.spriteSpans.push_back({static_cast<std::uint32_t>(entityIndex), firstVertex,
                                     static_cast<float>(halfWidth), static_cast<float>(halfHeight)});
    }

    // move_rope / keyframe_rope strands. Their settled shape is world geometry,
    // but the ribbon they are drawn as faces the camera, so - like the sprite
    // helpers above - they are tracked as spans that get re-expanded whenever
    // the camera moves.
    for (const hammer::vmf::RopeStrand& strand : hammer::vmf::buildRopeStrands(scene)) {
        const std::string normalizedMaterial =
            hammer::vmf::normalizeMaterialPath(strand.material);
        const auto index = materialIndex(normalizedMaterial);
        if (!index || strand.points.size() < 2) continue;
        const auto& material = *pendingMaterials[*index].source;
        // A rope is a thin camera-facing ribbon with no lightmap and no volume:
        // shadowing the world with it would produce a shadow of the billboard
        // rather than of a rope, which VRAD does not bake either.
        const std::uint32_t shadowFlags =
            rayFlagsForMaterial(normalizedMaterial) | TriangleNoShadow;

        const std::size_t firstPoint = output.ropePoints.size();
        output.ropePoints.insert(output.ropePoints.end(), strand.points.begin(),
                                 strand.points.end());
        const double vPerUnit = hammer::vmf::ropeTextureVPerUnit(strand, material.image.height);
        const std::size_t vertexCount = (strand.points.size() - 1) * 6;
        std::vector<RayTracingVertex> ribbon(vertexCount);
        expandRopeRibbon(strand.points.data(), strand.points.size(), strand.width * 0.5,
                         vPerUnit, options.camera.position, cameraRight, ribbon.data());
        const std::size_t firstVertex = output.vertices.size();
        for (std::size_t vertex = 0; vertex + 2 < ribbon.size(); vertex += 3) {
            appendTriangle(ribbon[vertex], ribbon[vertex + 1], ribbon[vertex + 2], *index,
                           TriangleTwoSided | shadowFlags);
        }
        output.hasCameraFacingSprites = true;
        output.ropeSpans.push_back({firstVertex, firstPoint, strand.points.size(),
                                   static_cast<float>(strand.width * 0.5),
                                   static_cast<float>(vPerUnit)});
    }

    // Source detail props. These are world content - VBSP bakes them into the
    // map - so unlike the entity helper sprites above they are emitted even
    // when helpers are off. VRAD does not include them in its bake, so they
    // neither cast shadows nor receive bounced light here either.
    {
        const std::string dictionaryName = scene.detailVbspName.empty()
            ? std::string("detail.vbsp") : scene.detailVbspName;
        const hammer::assets::DetailObjectDictionary& dictionary =
            options.detailProps ? detailDictionary(dictionaryName)
                                : hammer::assets::DetailObjectDictionary{};
        if (!dictionary.empty()) {
            const hammer::assets::DetailPropFade detailFade =
                hammer::assets::detailPropFadeForScene(scene);
            const hammer::assets::DetailPropEmission emission = hammer::assets::emitDetailProps(
                scene, dictionary, [this](std::string_view name) {
                    const auto material = materials_->material(std::string(name));
                    return material ? material->detailType : std::string();
                });

            const std::string spriteMaterialName = scene.detailMaterial.empty()
                ? std::string("detail/detailsprites") : scene.detailMaterial;
            const auto spriteMaterialIndex = materialIndex(spriteMaterialName);

            for (const hammer::assets::DetailPropInstance& prop : emission.props) {
                if (prop.type == hammer::assets::DetailPropType::Model) {
                    if (!studioModels_ || prop.model.empty()) continue;
                    const auto model = studioModels_->model(prop.model);
                    if (!model || !model->valid) continue;
                    const hammer::camera::SourceTransform transform =
                        hammer::camera::sourceTransform(prop.origin, prop.angles);
                    for (const hammer::assets::StudioMesh& mesh : model->meshes) {
                        const std::string normalizedMaterial = hammer::vmf::normalizeMaterialPath(
                            model->materialForSkin(mesh.materialSlot, 0));
                        const auto index = materialIndex(normalizedMaterial);
                        if (!index) continue;
                        const std::uint32_t shadowFlags =
                            rayFlagsForMaterial(normalizedMaterial) | TriangleNoShadow;
                        for (std::size_t vertex = 0; vertex + 2 < mesh.vertices.size();
                             vertex += 3) {
                            std::array<RayTracingVertex, 3> triangle;
                            for (int corner = 0; corner < 3; ++corner) {
                                const auto& source =
                                    mesh.vertices[vertex + static_cast<std::size_t>(corner)];
                                triangle[static_cast<std::size_t>(corner)] = makeVertex(
                                    transform.transformPoint({source.x, source.y, source.z}),
                                    transform.transformVector({source.nx, source.ny, source.nz}),
                                    transform.transformVector({source.tx, source.ty, source.tz}),
                                    source.tangentSign, source.u, source.v, source.u, source.v,
                                    0.0f);
                            }
                            appendTriangle(triangle[0], triangle[1], triangle[2], *index,
                                           shadowFlags);
                        }
                    }
                    continue;
                }

                if (!spriteMaterialIndex) continue;
                const std::uint32_t shadowFlags =
                    rayFlagsForMaterial(hammer::vmf::normalizeMaterialPath(spriteMaterialName)) |
                    TriangleNoShadow;
                std::array<RayTracingVertex, 6> quad;
                expandDetailSprite(prop, options.camera.position, detailFade, quad.data());
                const std::size_t firstVertex = output.vertices.size();
                appendTriangle(quad[0], quad[1], quad[2], *spriteMaterialIndex,
                               TriangleTwoSided | shadowFlags);
                appendTriangle(quad[3], quad[4], quad[5], *spriteMaterialIndex,
                               TriangleTwoSided | shadowFlags);
                output.detailSpriteSpans.push_back({firstVertex, output.detailSprites.size()});
                output.detailSprites.push_back(prop);
                output.hasCameraFacingSprites = true;
            }
        }
    }

    std::array<int, 6> skyItems{{-1, -1, -1, -1, -1, -1}};
    if (!scene.skyName.empty()) {
        constexpr std::array<const char*, 6> suffixes{{"bk", "lf", "ft", "rt", "up", "dn"}};
        bool complete = true;
        for (std::size_t face = 0; face < suffixes.size(); ++face) {
            const auto material = materials_->material("skybox/" + scene.skyName + suffixes[face]);
            if (!material || material->missing || !material->image.valid()) {
                complete = false;
                break;
            }
            skyItems[face] = atlas.add("__sky" + std::to_string(face), material->image);
        }
        output.hasSky = complete;
    }

    std::vector<std::array<float, 4>> rects;
    if (!atlas.build(options.maximumAtlasSize, options.maximumAtlasLayers,
                     output.atlas, rects, output.error)) return output;
    output.materials.reserve(pendingMaterials.size());
    for (PendingMaterial& pending : pendingMaterials) {
        applyRects(pending, rects);
        output.materials.push_back(pending.gpu);
    }
    if (output.hasSky) {
        for (std::size_t face = 0; face < skyItems.size(); ++face) {
            const int item = skyItems[face];
            output.skyRects[face] = item >= 0 && item < static_cast<int>(rects.size())
                ? rects[static_cast<std::size_t>(item)] : std::array<float, 4>{};
        }
    }

    // Hand every static-prop triangle a slot in the ambient-cube array. VRAD
    // lights props with ComputeStaticPropLighting rather than through the
    // lightmap, and without this the preview has nowhere to deposit bounced
    // light for a prop - which is why props read flat and dark against
    // lightmapped world geometry in the same room.
    {
        std::uint32_t propTriangles = 0;
        for (RayTracingTriangle& triangle : output.triangles) {
            triangle.data[3] = (triangle.data[1] & TriangleStaticProp) != 0u
                ? propTriangles++ : 0xffffffffu;
        }
        output.propCubeTriangles = propTriangles;
    }

    if (output.vertices.empty()) output.error = "Ray-traced preview scene contains no triangles";
    return output;
}

bool RayTracingSceneBuilder::updateDynamicGeometry(
    const hammer::vmf::Scene& source,
    const RayTracingBuildOptions& options,
    RayTracingScene& scene) const
{
    if (!studioModels_ && !scene.animatedMeshSpans.empty()) return false;

    std::size_t spanIndex = 0;
    while (spanIndex < scene.animatedMeshSpans.size()) {
        const std::uint32_t entityIndex = scene.animatedMeshSpans[spanIndex].entityIndex;
        if (entityIndex >= source.entities.size()) return false;
        const hammer::vmf::EntityMarker& entity = source.entities[entityIndex];
        const auto model = studioModels_->model(entity.model);
        if (!model || !model->valid) return false;

        int sequenceIndex = entity.animationSequenceIndex;
        if ((sequenceIndex < 0 || sequenceIndex >= model->sequenceCount()) &&
            !entity.animationSequence.empty())
            sequenceIndex = model->sequenceIndex(entity.animationSequence);
        if ((sequenceIndex < 0 || sequenceIndex >= model->sequenceCount()) &&
            entity.animateModel && entity.animationSequence.empty())
            sequenceIndex = model->sequenceCount() > 0 ? 0 : -1;
        if (sequenceIndex < 0 || sequenceIndex >= model->sequenceCount()) return false;

        const hammer::assets::StudioSequence& sequence =
            model->sequences[static_cast<std::size_t>(sequenceIndex)];
        double cycle = entity.animationCycle >= 0.0 ? entity.animationCycle : 0.0;
        if (entity.animationCycle < 0.0 && sequence.duration > 1e-6f) {
            cycle = options.animationSeconds * entity.animationPlaybackRate / sequence.duration;
            if (entity.animationPlaybackRate < 0.0) cycle += 1.0;
        }

        std::vector<std::vector<hammer::assets::StudioVertex>> animatedMeshes;
        if (!studioModels_->sampleAnimation(*model, sequenceIndex, cycle, animatedMeshes) ||
            animatedMeshes.size() != model->meshes.size()) return false;
        const hammer::camera::SourceTransform transform =
            hammer::camera::sourceTransform(entity.origin, entity.renderAngles());

        while (spanIndex < scene.animatedMeshSpans.size() &&
               scene.animatedMeshSpans[spanIndex].entityIndex == entityIndex) {
            const RayTracingAnimatedMeshSpan& span = scene.animatedMeshSpans[spanIndex++];
            if (span.meshIndex >= animatedMeshes.size() ||
                span.firstVertex + span.vertexCount > scene.vertices.size()) return false;
            const auto& vertices = animatedMeshes[span.meshIndex];
            if (vertices.size() < span.vertexCount) return false;
            for (std::size_t vertex = 0; vertex < span.vertexCount; ++vertex) {
                const auto& sourceVertex = vertices[vertex];
                const hammer::vmf::Vec3 localPosition{sourceVertex.x, sourceVertex.y, sourceVertex.z};
                const hammer::vmf::Vec3 localNormal{sourceVertex.nx, sourceVertex.ny, sourceVertex.nz};
                const hammer::vmf::Vec3 localTangent{sourceVertex.tx, sourceVertex.ty, sourceVertex.tz};
                scene.vertices[span.firstVertex + vertex] = makeVertex(
                    transform.transformPoint(localPosition),
                    transform.transformVector(localNormal),
                    transform.transformVector(localTangent), sourceVertex.tangentSign,
                    sourceVertex.u, sourceVertex.v, sourceVertex.u, sourceVertex.v, 0.0f);
            }
        }
    }

    return updateSpriteGeometry(source, options, scene);
}

const hammer::assets::DetailObjectDictionary& RayTracingSceneBuilder::detailDictionary(
    const std::string& name) const
{
    if (detailDictionaryLoaded_ && detailDictionaryName_ == name) return detailDictionary_;
    detailDictionaryName_ = name;
    detailDictionaryLoaded_ = true;
    detailDictionary_ = {};
    if (materials_) {
        if (const auto fileSystem = materials_->fileSystem())
            detailDictionary_ = hammer::assets::loadDetailObjectDictionary(*fileSystem, name);
    }
    return detailDictionary_;
}

bool RayTracingSceneBuilder::updateSpriteGeometry(
    const hammer::vmf::Scene& source,
    const RayTracingBuildOptions& options,
    RayTracingScene& scene) const
{
    if (scene.spriteSpans.empty() && scene.ropeSpans.empty() &&
        scene.detailSpriteSpans.empty()) {
        return true;
    }
    const hammer::vmf::Vec3 cameraRight = hammer::camera::rightVector(options.camera);
    const hammer::vmf::Vec3 cameraUp = hammer::camera::upVector(options.camera);
    for (const RayTracingSpriteSpan& span : scene.spriteSpans) {
        if (span.entityIndex >= source.entities.size() ||
            span.firstVertex + 6 > scene.vertices.size()) return false;
        const hammer::vmf::EntityMarker& entity = source.entities[span.entityIndex];
        auto point = [&](double x, double y) {
            return hammer::vmf::Vec3{
                entity.origin.x + cameraRight.x * x + cameraUp.x * y,
                entity.origin.y + cameraRight.y * x + cameraUp.y * y,
                entity.origin.z + cameraRight.z * x + cameraUp.z * y};
        };
        const hammer::vmf::Vec3 normal = normalized(subtract(options.camera.position, entity.origin));
        const RayTracingVertex bl = makeVertex(point(-span.halfWidth, -span.halfHeight), normal,
                                               cameraRight, 1.0f, 0, 1, 0, 1, 0);
        const RayTracingVertex br = makeVertex(point(span.halfWidth, -span.halfHeight), normal,
                                               cameraRight, 1.0f, 1, 1, 1, 1, 0);
        const RayTracingVertex tr = makeVertex(point(span.halfWidth, span.halfHeight), normal,
                                               cameraRight, 1.0f, 1, 0, 1, 0, 0);
        const RayTracingVertex tl = makeVertex(point(-span.halfWidth, span.halfHeight), normal,
                                               cameraRight, 1.0f, 0, 0, 0, 0, 0);
        scene.vertices[span.firstVertex + 0] = bl;
        scene.vertices[span.firstVertex + 1] = br;
        scene.vertices[span.firstVertex + 2] = tr;
        scene.vertices[span.firstVertex + 3] = bl;
        scene.vertices[span.firstVertex + 4] = tr;
        scene.vertices[span.firstVertex + 5] = tl;
    }

    // Rope ribbons face the camera for the same reason sprites do, and the
    // settled polyline they are expanded from does not depend on the camera, so
    // this is a pure re-expansion of geometry that is already in the scene.
    for (const RayTracingRopeSpan& span : scene.ropeSpans) {
        if (span.pointCount < 2 ||
            span.firstPoint + span.pointCount > scene.ropePoints.size()) return false;
        const std::size_t vertexCount = (span.pointCount - 1) * 6;
        if (span.firstVertex + vertexCount > scene.vertices.size()) return false;
        expandRopeRibbon(scene.ropePoints.data() + span.firstPoint, span.pointCount,
                         span.halfWidth, span.vPerUnit, options.camera.position, cameraRight,
                         scene.vertices.data() + span.firstVertex);
    }

    // Detail sprites turn with the camera the same way, through the same
    // placement data the emitter produced - and the same camera move changes
    // which of them are inside cl_detaildist.
    const hammer::assets::DetailPropFade detailFade =
        hammer::assets::detailPropFadeForScene(source);
    for (const RayTracingDetailSpriteSpan& span : scene.detailSpriteSpans) {
        if (span.propIndex >= scene.detailSprites.size() ||
            span.firstVertex + 6 > scene.vertices.size()) return false;
        expandDetailSprite(scene.detailSprites[span.propIndex], options.camera.position,
                           detailFade, scene.vertices.data() + span.firstVertex);
    }
    return true;
}

} // namespace hammer::render
