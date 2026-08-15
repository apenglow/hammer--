#include "RadiosityBake.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hammer::render {
namespace {

// One luxel of gutter around every face keeps bilinear taps on a face's edge
// from reaching into the neighbouring rect. VRAD solves the same problem with
// the lightmap border it bakes into each face's grid.
constexpr int kLightmapBorder = 1;

double dot(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

hammer::vmf::Vec3 normalize(const hammer::vmf::Vec3& value)
{
    const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length < 1e-12) return {0.0, 0.0, 1.0};
    return {value.x / length, value.y / length, value.z / length};
}

double faceLightmapScale(const hammer::vmf::FaceGeometry& face, int defaultScale)
{
    // CMapFace::texture.nLightmapScale, parsed per side by VmfScene. Zero or
    // negative would divide the face into an unbounded grid, so only those fall
    // back to the configured default.
    if (face.lightmapScale > 0) return static_cast<double>(face.lightmapScale);
    return defaultScale > 0 ? static_cast<double>(defaultScale) : 16.0;
}

struct PendingFace
{
    std::uint64_t key{0};
    LightmapRect rect;
};

// Shelf packer over rect sizes only. AtlasBuilder in RayTracingScene.cpp uses
// the same strategy but is bound to hammer::assets::Image inputs and to array
// layers, neither of which apply to a single-layer lightmap page.
bool packShelves(std::vector<PendingFace>& faces, int maximumSize, int& outSize)
{
    std::vector<int> order(faces.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const LightmapRect& left = faces[static_cast<std::size_t>(a)].rect;
        const LightmapRect& right = faces[static_cast<std::size_t>(b)].rect;
        if (left.height != right.height) return left.height > right.height;
        return left.width > right.width;
    });

    int widest = 0;
    int tallest = 0;
    for (const PendingFace& face : faces) {
        widest = std::max(widest, face.rect.width + kLightmapBorder * 2);
        tallest = std::max(tallest, face.rect.height + kLightmapBorder * 2);
    }

    for (int size = 64; size <= maximumSize; size *= 2) {
        if (size < widest || size < tallest) continue;
        int cursorX = 0;
        int cursorY = 0;
        int shelfHeight = 0;
        bool packed = true;
        for (int index : order) {
            LightmapRect& rect = faces[static_cast<std::size_t>(index)].rect;
            const int paddedWidth = rect.width + kLightmapBorder * 2;
            const int paddedHeight = rect.height + kLightmapBorder * 2;
            if (cursorX + paddedWidth > size) {
                cursorX = 0;
                cursorY += shelfHeight;
                shelfHeight = 0;
            }
            if (cursorY + paddedHeight > size) {
                packed = false;
                break;
            }
            rect.x = cursorX + kLightmapBorder;
            rect.y = cursorY + kLightmapBorder;
            cursorX += paddedWidth;
            shelfHeight = std::max(shelfHeight, paddedHeight);
        }
        if (packed) {
            outSize = size;
            return true;
        }
    }
    return false;
}

// Solves the 3x3 system [rowA; rowB; rowC] * x = rhs by Cramer's rule. Returns
// false when the rows are degenerate, which happens if a face's texture axes are
// parallel to each other or lie in the face plane.
bool solve3x3(const hammer::vmf::Vec3& rowA, const hammer::vmf::Vec3& rowB,
              const hammer::vmf::Vec3& rowC, const hammer::vmf::Vec3& rhs,
              hammer::vmf::Vec3& out)
{
    auto determinant = [](const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b,
                          const hammer::vmf::Vec3& c) {
        return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) +
               a.z * (b.x * c.y - b.y * c.x);
    };
    const double base = determinant(rowA, rowB, rowC);
    if (std::abs(base) < 1e-12) return false;
    const hammer::vmf::Vec3 columnX{rhs.x, rhs.y, rhs.z};
    out.x = determinant({columnX.x, rowA.y, rowA.z}, {columnX.y, rowB.y, rowB.z},
                        {columnX.z, rowC.y, rowC.z}) / base;
    out.y = determinant({rowA.x, columnX.x, rowA.z}, {rowB.x, columnX.y, rowB.z},
                        {rowC.x, columnX.z, rowC.z}) / base;
    out.z = determinant({rowA.x, rowA.y, columnX.x}, {rowB.x, rowB.y, columnX.y},
                        {rowC.x, rowC.y, columnX.z}) / base;
    return true;
}

} // namespace

void lightmapCoordinate(const hammer::vmf::FaceGeometry& face,
                        const hammer::vmf::Vec3& position,
                        double scale,
                        double& outS, double& outT)
{
    // VRAD builds lightmapVecsLuxelsPerWorldUnit from the *unscaled* texture
    // axis divided by the lightmap scale; the material's texture scale plays no
    // part. Shifts are in texels, so they convert at the same ratio.
    const double divisor = scale > 0.0 ? scale : 16.0;
    outS = (dot(position, face.uAxis.direction) + face.uAxis.shift) / divisor;
    outT = (dot(position, face.vAxis.direction) + face.vAxis.shift) / divisor;
}

RadiosityData buildRadiosityPatchData(const hammer::vmf::Scene& scene,
                                      const LightmapLayoutOptions& options)
{
    // The patch half of buildRadiosityData, without the luxel grid.
    //
    // The full VRAD pipeline bakes indirect light down onto a luxel-resolution
    // lightmap. The live preview does not need that: it already evaluates
    // direct light per pixel at better than luxel resolution, and indirect
    // light is low frequency by nature, so it can be read straight from the
    // patch grid with a bilinear tap. Skipping the luxel side removes the
    // single most expensive part of the bake - a full-map luxel layout can
    // reach tens of millions of samples at 48 bytes each - along with the
    // luxel_direct ray cost and the lightmap atlas image.
    RadiosityData data;
    for (int multiplier = kPatchScaleMultiplier;; multiplier *= 2) {
        LightmapLayoutOptions patchOptions = options;
        patchOptions.scaleMultiplier = multiplier;
        // Keep displacements at their own chop as the whole grid coarsens.
        patchOptions.displacementScaleMultiplier =
            multiplier * kDisplacementPatchScaleMultiplier / kPatchScaleMultiplier;
        LightmapLayout patchLayout = buildLightmapLayout(scene, patchOptions);
        if (!patchLayout.valid()) {
            data.status = "the radiosity patch grid does not fit its atlas";
            return data;
        }
        RadRectTable patchRects = buildRectTable(scene, patchLayout);
        std::vector<LuxelSample> patches = buildLuxelSamples(scene, patchLayout, patchRects);
        if (patches.size() <= static_cast<std::size_t>(kMaximumPatches) ||
            multiplier > 4096) {
            data.patchLayout = std::move(patchLayout);
            data.patchRects = std::move(patchRects);
            data.patches = std::move(patches);
            // origin.w already carries each face's authored luxel scale, set
            // when the rect was built. It cannot be recovered here by dividing
            // by `multiplier`: displacements chop at a different rate, and an
            // oversized face may have been coarsened further on its own.
            break;
        }
    }
    if (data.patches.empty()) {
        data.status = "no lit faces produced radiosity patches";
        return data;
    }

    data.patchIndexGrid.assign(static_cast<std::size_t>(data.patchLayout.width) *
                                   data.patchLayout.height,
                               0u);
    for (std::size_t patch = 0; patch < data.patches.size(); ++patch) {
        const LuxelSample& sample = data.patches[patch];
        const int x = static_cast<int>(sample.atlas[0]);
        const int y = static_cast<int>(sample.atlas[1]);
        if (x < 0 || y < 0 || x >= data.patchLayout.width || y >= data.patchLayout.height)
            continue;
        data.patchIndexGrid[static_cast<std::size_t>(y) * data.patchLayout.width + x] =
            static_cast<std::uint32_t>(patch) + 1u;
    }
    return data;
}

RadiosityData buildRadiosityData(const hammer::vmf::Scene& scene,
                                 const LightmapLayoutOptions& options)
{
    RadiosityData data;
    data.luxelLayout = buildLightmapLayout(scene, options);
    if (!data.luxelLayout.valid()) {
        data.status = "no lit faces, or their luxel grids do not fit the lightmap atlas";
        return data;
    }
    data.luxelRects = buildRectTable(scene, data.luxelLayout);
    data.luxels = buildLuxelSamples(scene, data.luxelLayout, data.luxelRects);

    // VRAD caps the solve at MAX_PATCHES. Coarsening the patch grid is the
    // cheapest way to stay under it on a large map, and indirect light is low
    // frequency enough to absorb it.
    for (int multiplier = kPatchScaleMultiplier;; multiplier *= 2) {
        LightmapLayoutOptions patchOptions = options;
        patchOptions.scaleMultiplier = multiplier;
        // Keep displacements at their own chop as the whole grid coarsens.
        patchOptions.displacementScaleMultiplier =
            multiplier * kDisplacementPatchScaleMultiplier / kPatchScaleMultiplier;
        LightmapLayout patchLayout = buildLightmapLayout(scene, patchOptions);
        if (!patchLayout.valid()) {
            data.status = "the radiosity patch grid does not fit its atlas";
            return data;
        }
        RadRectTable patchRects = buildRectTable(scene, patchLayout);
        std::vector<LuxelSample> patches = buildLuxelSamples(scene, patchLayout, patchRects);
        if (patches.size() <= static_cast<std::size_t>(kMaximumPatches) ||
            multiplier > 4096) {
            data.patchLayout = std::move(patchLayout);
            data.patchRects = std::move(patchRects);
            data.patches = std::move(patches);
            // origin.w already carries each face's authored luxel scale, set
            // when the rect was built. It cannot be recovered here by dividing
            // by `multiplier`: displacements chop at a different rate, and an
            // oversized face may have been coarsened further on its own.
            break;
        }
    }

    // Transfer gathering resolves a ray hit to a patch through this grid.
    data.patchIndexGrid.assign(static_cast<std::size_t>(data.patchLayout.width) *
                                   data.patchLayout.height,
                               0u);
    for (std::size_t patch = 0; patch < data.patches.size(); ++patch) {
        const LuxelSample& sample = data.patches[patch];
        const int x = static_cast<int>(sample.atlas[0]);
        const int y = static_cast<int>(sample.atlas[1]);
        if (x < 0 || y < 0 || x >= data.patchLayout.width || y >= data.patchLayout.height)
            continue;
        data.patchIndexGrid[static_cast<std::size_t>(y) * data.patchLayout.width + x] =
            static_cast<std::uint32_t>(patch) + 1u;
    }
    if (!data.rectTablesAligned())
        data.status = "the luxel and patch rect tables disagree";
    return data;
}

RadRectTable buildRectTable(const hammer::vmf::Scene& scene, const LightmapLayout& layout)
{
    RadRectTable table;
    if (!layout.valid()) return table;
    for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            const std::uint64_t key = faceKey(brush.id, face.sideId);
            const auto entry = layout.faces.find(key);
            if (entry == layout.faces.end()) continue;
            if (table.indices.contains(key)) continue;
            const LightmapRect& rect = entry->second;
            RadRectRecord record;
            record.rect = {static_cast<float>(rect.x), static_cast<float>(rect.y),
                           static_cast<float>(rect.width), static_cast<float>(rect.height)};
            record.origin = {static_cast<float>(rect.originS), static_cast<float>(rect.originT),
                             static_cast<float>(rect.lightmapScale),
                             static_cast<float>(rect.authoredScale)};
            record.uAxis = {static_cast<float>(face.uAxis.direction.x),
                            static_cast<float>(face.uAxis.direction.y),
                            static_cast<float>(face.uAxis.direction.z),
                            static_cast<float>(face.uAxis.shift)};
            record.vAxis = {static_cast<float>(face.vAxis.direction.x),
                            static_cast<float>(face.vAxis.direction.y),
                            static_cast<float>(face.vAxis.direction.z),
                            static_cast<float>(face.vAxis.shift)};
            record.identity = {0u, static_cast<std::uint32_t>(key & 0xffffffffu),
                               static_cast<std::uint32_t>(key >> 32u), 0u};
            table.indices.emplace(key, static_cast<std::uint32_t>(table.records.size()));
            table.records.push_back(record);
        }
    }
    return table;
}

std::vector<LuxelSample> buildLuxelSamples(const hammer::vmf::Scene& scene,
                                           const LightmapLayout& layout,
                                           const RadRectTable& rectTable)
{
    std::vector<LuxelSample> samples;
    if (!layout.valid()) return samples;

    std::uint32_t currentRectIndex = 0;
    auto emit = [&](const LightmapRect& rect, int luxelX, int luxelY,
                    const hammer::vmf::Vec3& position, const hammer::vmf::Vec3& normal,
                    bool inside) {
        LuxelSample sample;
        sample.position = {static_cast<float>(position.x), static_cast<float>(position.y),
                           static_cast<float>(position.z), 0.0f};
        const hammer::vmf::Vec3 unit = normalize(normal);
        sample.normal = {static_cast<float>(unit.x), static_cast<float>(unit.y),
                         static_cast<float>(unit.z), 0.0f};
        sample.atlas = {static_cast<float>(rect.x + luxelX),
                        static_cast<float>(rect.y + luxelY),
                        inside ? 1.0f : 0.0f, static_cast<float>(currentRectIndex)};
        samples.push_back(sample);
    };

    for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            const std::uint64_t key = faceKey(brush.id, face.sideId);
            const auto entry = layout.faces.find(key);
            if (entry == layout.faces.end()) continue;
            const auto rectIndex = rectTable.indices.find(key);
            if (rectIndex == rectTable.indices.end()) continue;
            currentRectIndex = rectIndex->second;
            const LightmapRect& rect = entry->second;

            const bool displacement = face.displacement && face.displacementPower >= 1 &&
                                      !face.displacementVertices.empty() &&
                                      !face.displacementIndices.empty();
            if (displacement) {
                // The displaced surface is not planar, so rasterize its triangles
                // in luxel space and interpolate position and normal. This follows
                // the displacement rather than the base face's plane.
                std::vector<char> written(static_cast<std::size_t>(rect.width) * rect.height, 0);
                for (std::size_t tri = 0; tri + 2 < face.displacementIndices.size(); tri += 3) {
                    const std::size_t ia = face.displacementIndices[tri + 0];
                    const std::size_t ib = face.displacementIndices[tri + 1];
                    const std::size_t ic = face.displacementIndices[tri + 2];
                    if (ia >= face.displacementVertices.size() ||
                        ib >= face.displacementVertices.size() ||
                        ic >= face.displacementVertices.size()) continue;
                    const auto& va = face.displacementVertices[ia];
                    const auto& vb = face.displacementVertices[ib];
                    const auto& vc = face.displacementVertices[ic];
                    double sa = 0.0, ta = 0.0, sb = 0.0, tb = 0.0, sc = 0.0, tc = 0.0;
                    lightmapCoordinate(face, va.position, rect.lightmapScale, sa, ta);
                    lightmapCoordinate(face, vb.position, rect.lightmapScale, sb, tb);
                    lightmapCoordinate(face, vc.position, rect.lightmapScale, sc, tc);
                    sa -= rect.originS; ta -= rect.originT;
                    sb -= rect.originS; tb -= rect.originT;
                    sc -= rect.originS; tc -= rect.originT;
                    const double area = (sb - sa) * (tc - ta) - (sc - sa) * (tb - ta);
                    if (std::abs(area) < 1e-12) continue;
                    const int minX = std::max(0, static_cast<int>(std::floor(std::min({sa, sb, sc}))));
                    const int maxX = std::min(rect.width - 1,
                                              static_cast<int>(std::ceil(std::max({sa, sb, sc}))));
                    const int minY = std::max(0, static_cast<int>(std::floor(std::min({ta, tb, tc}))));
                    const int maxY = std::min(rect.height - 1,
                                              static_cast<int>(std::ceil(std::max({ta, tb, tc}))));
                    for (int y = minY; y <= maxY; ++y) {
                        for (int x = minX; x <= maxX; ++x) {
                            const std::size_t slot =
                                static_cast<std::size_t>(y) * rect.width + x;
                            if (written[slot]) continue;
                            const double px = x;
                            const double py = y;
                            double w0 = ((sb - px) * (tc - py) - (sc - px) * (tb - py)) / area;
                            double w1 = ((sc - px) * (ta - py) - (sa - px) * (tc - py)) / area;
                            double w2 = 1.0 - w0 - w1;
                            // A small tolerance keeps luxels on a shared edge from
                            // being dropped by both triangles.
                            if (w0 < -0.02 || w1 < -0.02 || w2 < -0.02) continue;
                            w0 = std::clamp(w0, 0.0, 1.0);
                            w1 = std::clamp(w1, 0.0, 1.0);
                            w2 = std::clamp(w2, 0.0, 1.0);
                            const double sum = w0 + w1 + w2;
                            if (sum < 1e-9) continue;
                            w0 /= sum; w1 /= sum; w2 /= sum;
                            const hammer::vmf::Vec3 position{
                                va.position.x * w0 + vb.position.x * w1 + vc.position.x * w2,
                                va.position.y * w0 + vb.position.y * w1 + vc.position.y * w2,
                                va.position.z * w0 + vb.position.z * w1 + vc.position.z * w2};
                            const hammer::vmf::Vec3 normal{
                                va.normal.x * w0 + vb.normal.x * w1 + vc.normal.x * w2,
                                va.normal.y * w0 + vb.normal.y * w1 + vc.normal.y * w2,
                                va.normal.z * w0 + vb.normal.z * w1 + vc.normal.z * w2};
                            written[slot] = 1;
                            emit(rect, x, y, position, normal, true);
                        }
                    }
                }
                continue;
            }

            if (face.vertices.size() < 3) continue;
            // Planar face: a luxel is the unique point satisfying the two lightmap
            // axis equations and the face plane. This is VRAD's WorldFromLuxel.
            hammer::vmf::Vec3 anchor{};
            bool haveAnchor = false;
            std::vector<std::pair<double, double>> polygon;
            polygon.reserve(face.vertices.size());
            for (std::size_t vertexIndex : face.vertices) {
                if (vertexIndex >= brush.vertices.size()) continue;
                const hammer::vmf::Vec3& world = brush.vertices[vertexIndex];
                if (!haveAnchor) { anchor = world; haveAnchor = true; }
                double s = 0.0;
                double t = 0.0;
                lightmapCoordinate(face, world, rect.lightmapScale, s, t);
                polygon.emplace_back(s - rect.originS, t - rect.originT);
            }
            if (!haveAnchor || polygon.size() < 3) continue;

            const hammer::vmf::Vec3 normal = normalize(face.normal);
            const double planeDistance = dot(anchor, normal);
            double centerS = 0.0;
            double centerT = 0.0;
            for (const auto& point : polygon) { centerS += point.first; centerT += point.second; }
            centerS /= static_cast<double>(polygon.size());
            centerT /= static_cast<double>(polygon.size());

            // Winding order is not guaranteed, so accept a point that is on the
            // same side of every edge, whichever side that is.
            auto insidePolygon = [&](double s, double t) {
                bool anyPositive = false;
                bool anyNegative = false;
                for (std::size_t i = 0; i < polygon.size(); ++i) {
                    const auto& a = polygon[i];
                    const auto& b = polygon[(i + 1) % polygon.size()];
                    const double side = (b.first - a.first) * (t - a.second) -
                                        (b.second - a.second) * (s - a.first);
                    if (side > 1e-9) anyPositive = true;
                    if (side < -1e-9) anyNegative = true;
                }
                return !(anyPositive && anyNegative);
            };

            for (int y = 0; y < rect.height; ++y) {
                for (int x = 0; x < rect.width; ++x) {
                    double s = x;
                    double t = y;
                    bool inside = insidePolygon(s, t);
                    if (!inside) {
                        // VRAD pulls samples that land off the face back toward the
                        // face center rather than discarding them, so edge luxels
                        // still hold usable light instead of black.
                        for (double pull : {0.25, 0.5, 0.75, 0.95}) {
                            const double ps = s + (centerS - s) * pull;
                            const double pt = t + (centerT - t) * pull;
                            if (insidePolygon(ps, pt)) { s = ps; t = pt; break; }
                        }
                    }
                    const hammer::vmf::Vec3 rhs{
                        (s + rect.originS) * rect.lightmapScale - face.uAxis.shift,
                        (t + rect.originT) * rect.lightmapScale - face.vAxis.shift,
                        planeDistance};
                    hammer::vmf::Vec3 world{};
                    if (!solve3x3(face.uAxis.direction, face.vAxis.direction, normal,
                                  rhs, world)) {
                        continue;
                    }
                    emit(rect, x, y, world, normal, inside);
                }
            }
        }
    }
    return samples;
}

LightmapLayout buildLightmapLayout(const hammer::vmf::Scene& scene,
                                   const LightmapLayoutOptions& options)
{
    std::vector<PendingFace> pending;
    int coarsened = 0;

    for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            if (hammer::vmf::isFaceMaskedByDisplacementSolid(
                    brush, face, options.displacementSolidMask)) {
                continue;
            }
            const std::string normalized = hammer::vmf::normalizeMaterialPath(face.material);
            if (options.faceIsLit && !options.faceIsLit(normalized)) continue;

            double minimumS = std::numeric_limits<double>::max();
            double minimumT = std::numeric_limits<double>::max();
            double maximumS = std::numeric_limits<double>::lowest();
            double maximumT = std::numeric_limits<double>::lowest();
            auto accumulate = [&](const hammer::vmf::Vec3& position) {
                double s = 0.0;
                double t = 0.0;
                lightmapCoordinate(face, position, faceLightmapScale(face, options.defaultLightmapScale),
                                   s, t);
                minimumS = std::min(minimumS, s);
                minimumT = std::min(minimumT, t);
                maximumS = std::max(maximumS, s);
                maximumT = std::max(maximumT, t);
            };

            const bool displacement = face.displacement && face.displacementPower >= 1 &&
                                      !face.displacementVertices.empty();
            if (displacement) {
                for (const hammer::vmf::DisplacementVertex& vertex : face.displacementVertices)
                    accumulate(vertex.position);
            } else {
                if (face.vertices.size() < 3) continue;
                for (std::size_t vertexIndex : face.vertices) {
                    if (vertexIndex >= brush.vertices.size()) continue;
                    accumulate(brush.vertices[vertexIndex]);
                }
            }
            if (minimumS > maximumS || minimumT > maximumT) continue;

            // A huge face must not be clamped to the luxel cap: the rect and the
            // world->luxel mapping have to keep agreeing, so coarsen the scale
            // until the grid fits, exactly as VRAD does for oversized faces.
            const double authoredScale =
                faceLightmapScale(face, options.defaultLightmapScale);
            // -dispchop for displacements, -maxchop for everything else. Both
            // are counts of luxel widths, so they multiply the authored scale.
            const int chop = displacement && options.displacementScaleMultiplier > 0
                ? options.displacementScaleMultiplier
                : std::max(1, options.scaleMultiplier);
            double scale = authoredScale * std::max(1, chop);
            int width = 0;
            int height = 0;
            for (;;) {
                const double ratio = authoredScale / scale;
                const double originS = std::floor(minimumS * ratio);
                const double originT = std::floor(minimumT * ratio);
                // VRAD sizes a face's grid as ceil(max) - floor(min) + 1 so the
                // grid spans the face's extents inclusively at both ends.
                width = static_cast<int>(std::ceil(maximumS * ratio) - originS) + 1;
                height = static_cast<int>(std::ceil(maximumT * ratio) - originT) + 1;
                if ((width <= kMaximumLuxelsPerAxis && height <= kMaximumLuxelsPerAxis) ||
                    scale > 16384.0) {
                    PendingFace entry;
                    entry.key = faceKey(brush.id, face.sideId);
                    entry.rect.originS = originS;
                    entry.rect.originT = originT;
                    entry.rect.lightmapScale = scale;
                    entry.rect.authoredScale = authoredScale;
                    if (scale > authoredScale * std::max(1, chop))
                        ++coarsened;
                    entry.rect.width = std::clamp(width, 1, kMaximumLuxelsPerAxis);
                    entry.rect.height = std::clamp(height, 1, kMaximumLuxelsPerAxis);
                    pending.push_back(entry);
                    break;
                }
                scale *= 2.0;
            }
        }
    }

    LightmapLayout layout;
    if (pending.empty()) return layout;

    int size = 0;
    if (!packShelves(pending, std::clamp(options.maximumAtlasSize, 256, 16384), size))
        return layout;

    layout.width = size;
    layout.height = size;
    layout.coarsenedFaces = coarsened;
    layout.faces.reserve(pending.size());
    for (PendingFace& entry : pending) layout.faces.emplace(entry.key, entry.rect);
    return layout;
}

} // namespace hammer::render
