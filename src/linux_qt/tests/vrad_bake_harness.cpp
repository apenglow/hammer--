// Manual VRAD-bake diagnostic; not a registered test.
//
// Milestone 1 checks the lightmap layout in isolation: a luxel grid must cover
// each lit face at its authored $lightmapscale, and the world->luxel mapping
// must agree with the rect the face was packed into.

#include "RadiosityBake.hpp"
#include "VmfScene.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

// A sealed 512-unit room with one interior cube, all at the default lightmap
// scale of 16. A 512-wide wall must therefore land on a 33x33 grid:
// ceil(max) - floor(min) + 1 = 32 + 1.
std::shared_ptr<hammer::vmf::Scene> buildRoom(int lightmapScale = 16)
{
    std::string vmf = "versioninfo\n{\n\"editorversion\" \"400\"\n}\n"
                      "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n";
    int id = 100;
    auto box = [&](double x0, double y0, double z0, double x1, double y1, double z1,
                   const char* material) {
        const double planes[6][9] = {
            {x0, y1, z1, x1, y1, z1, x1, y0, z1}, {x0, y0, z0, x1, y0, z0, x1, y1, z0},
            {x0, y1, z1, x0, y0, z1, x0, y0, z0}, {x1, y1, z0, x1, y0, z0, x1, y0, z1},
            {x1, y1, z1, x0, y1, z1, x0, y1, z0}, {x0, y0, z1, x1, y0, z1, x1, y0, z0},
        };
        const char* axes[6][2] = {
            {"[1 0 0 0]", "[0 -1 0 0]"}, {"[1 0 0 0]", "[0 -1 0 0]"},
            {"[0 1 0 0]", "[0 0 -1 0]"}, {"[0 1 0 0]", "[0 0 -1 0]"},
            {"[1 0 0 0]", "[0 0 -1 0]"}, {"[1 0 0 0]", "[0 0 -1 0]"},
        };
        vmf += "solid\n{\n\"id\" \"" + std::to_string(id++) + "\"\n";
        for (int side = 0; side < 6; ++side) {
            const double* p = planes[side];
            char buffer[512];
            std::snprintf(buffer, sizeof(buffer),
                          "side\n{\n\"id\" \"%d\"\n"
                          "\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n"
                          "\"material\" \"%s\"\n"
                          "\"uaxis\" \"%s 0.25\"\n\"vaxis\" \"%s 0.25\"\n"
                          "\"lightmapscale\" \"%d\"\n}\n",
                          id * 8 + side, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8],
                          material, axes[side][0], axes[side][1], lightmapScale);
            vmf += buffer;
        }
        vmf += "}\n";
    };

    // Sealed shell, 512x512x256 interior, 16-unit walls.
    box(-272, -272, -272, 272, 272, -256, "DEV/DEV_MEASUREGENERIC01");  // floor
    box(-272, -272, 256, 272, 272, 272, "DEV/DEV_MEASUREGENERIC01");    // ceiling
    box(-272, -272, -256, -256, 272, 256, "DEV/DEV_MEASUREGENERIC01");
    box(256, -272, -256, 272, 272, 256, "DEV/DEV_MEASUREGENERIC01");
    box(-256, -272, -256, 256, -256, 256, "DEV/DEV_MEASUREGENERIC01");
    box(-256, 256, -256, 256, 272, 256, "DEV/DEV_MEASUREGENERIC01");
    box(-32, -32, -256, 32, 32, -192, "DEV/DEV_MEASUREGENERIC01");      // interior cube
    vmf += "}\n";

    const auto document = hammer::vmf::Document::parse(std::move(vmf));
    if (!document) return nullptr;
    return std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
}

} // namespace

int main()
{
    const auto scene = buildRoom();
    if (!scene) {
        std::printf("FAIL: test VMF did not parse\n");
        return 1;
    }

    hammer::render::LightmapLayoutOptions options;
    options.faceIsLit = [](std::string_view material) {
        return material.rfind("tools/", 0) != 0;
    };
    const hammer::render::LightmapLayout layout =
        hammer::render::buildLightmapLayout(*scene, options);

    if (!layout.valid()) {
        std::printf("FAIL: layout is empty\n");
        return 1;
    }
    std::printf("atlas %dx%d, %zu faces\n", layout.width, layout.height, layout.faces.size());

    int failures = 0;
    int wallSizedFaces = 0;
    long long luxels = 0;
    for (const auto& [key, rect] : layout.faces) {
        luxels += static_cast<long long>(rect.width) * rect.height;
        if (rect.x < 0 || rect.y < 0 ||
            rect.x + rect.width > layout.width || rect.y + rect.height > layout.height) {
            std::printf("FAIL: rect %dx%d at %d,%d escapes the atlas\n",
                        rect.width, rect.height, rect.x, rect.y);
            ++failures;
        }
        if (rect.lightmapScale != 16.0) {
            std::printf("FAIL: face %llu coarsened to scale %g; nothing here is oversized\n",
                        static_cast<unsigned long long>(key), rect.lightmapScale);
            ++failures;
        }
        // 544 units across a 16-unit luxel grid inclusive of both ends.
        if (rect.width == 35 && rect.height == 35) ++wallSizedFaces;
    }

    // Overlap check: no two rects may claim the same luxel.
    std::vector<const hammer::render::LightmapRect*> rects;
    rects.reserve(layout.faces.size());
    for (const auto& entry : layout.faces) rects.push_back(&entry.second);
    for (std::size_t a = 0; a < rects.size(); ++a) {
        for (std::size_t b = a + 1; b < rects.size(); ++b) {
            const auto& left = *rects[a];
            const auto& right = *rects[b];
            const bool disjoint = left.x + left.width <= right.x ||
                                  right.x + right.width <= left.x ||
                                  left.y + left.height <= right.y ||
                                  right.y + right.height <= left.y;
            if (!disjoint) {
                std::printf("FAIL: rects at %d,%d and %d,%d overlap\n",
                            left.x, left.y, right.x, right.y);
                ++failures;
            }
        }
    }

    // Luxel samples: one per texel, each reconstructed position must project
    // back onto the texel it came from.
    const auto rectTable = hammer::render::buildRectTable(*scene, layout);
    if (rectTable.records.size() != layout.faces.size()) {
        std::printf("FAIL: rect table has %zu records for %zu faces\n",
                    rectTable.records.size(), layout.faces.size());
        ++failures;
    }
    const auto samples = hammer::render::buildLuxelSamples(*scene, layout, rectTable);
    std::printf("%zu luxel samples for %lld grid texels\n", samples.size(), luxels);
    if (static_cast<long long>(samples.size()) != luxels) {
        std::printf("FAIL: expected one sample per grid texel\n");
        ++failures;
    }
    int offGrid = 0;
    int badNormal = 0;
    for (const auto& sample : samples) {
        if (sample.atlas[0] < 0.0f || sample.atlas[1] < 0.0f ||
            sample.atlas[0] >= static_cast<float>(layout.width) ||
            sample.atlas[1] >= static_cast<float>(layout.height)) {
            ++offGrid;
        }
        const float length = std::sqrt(sample.normal[0] * sample.normal[0] +
                                       sample.normal[1] * sample.normal[1] +
                                       sample.normal[2] * sample.normal[2]);
        if (std::fabs(length - 1.0f) > 1e-3f) ++badNormal;
        // Every brush in this map is inside the 272-unit shell, so a correct
        // luxel->world solve cannot place a sample outside it. A wrong plane or
        // a bad Cramer solve shows up immediately here.
        for (int axis = 0; axis < 3; ++axis) {
            if (std::fabs(sample.position[axis]) > 272.5f) { ++offGrid; break; }
        }
    }
    if (offGrid != 0) {
        std::printf("FAIL: %d samples landed outside the atlas or far outside the room\n", offGrid);
        ++failures;
    }
    if (badNormal != 0) {
        std::printf("FAIL: %d samples have a non-unit normal\n", badNormal);
        ++failures;
    }

    // The radiosity patch grid is the same construction at a coarser scale, so
    // it must cover the same faces with far fewer samples.
    hammer::render::LightmapLayoutOptions patchOptions = options;
    patchOptions.scaleMultiplier = hammer::render::kPatchScaleMultiplier;
    const hammer::render::LightmapLayout patchLayout =
        hammer::render::buildLightmapLayout(*scene, patchOptions);
    const auto patchTable = hammer::render::buildRectTable(*scene, patchLayout);
    const auto patches = hammer::render::buildLuxelSamples(*scene, patchLayout, patchTable);
    std::printf("%zu patches across %zu faces (patch atlas %dx%d)\n",
                patches.size(), patchLayout.faces.size(), patchLayout.width, patchLayout.height);
    if (patchLayout.faces.size() != layout.faces.size()) {
        std::printf("FAIL: patch layout covers %zu faces, luxel layout covers %zu\n",
                    patchLayout.faces.size(), layout.faces.size());
        ++failures;
    }
    // Equal at kPatchScaleMultiplier 1, where the patch grid is the luxel grid.
    if (patches.empty() || patches.size() > samples.size()) {
        std::printf("FAIL: patch grid must never be finer than the luxel grid\n");
        ++failures;
    }
    if (patches.size() > static_cast<std::size_t>(hammer::render::kMaximumPatches)) {
        std::printf("FAIL: patch count exceeds MAX_PATCHES\n");
        ++failures;
    }

    std::printf("%d wall-sized (35x35) faces, %lld luxels total\n", wallSizedFaces, luxels);
    if (wallSizedFaces == 0) {
        std::printf("FAIL: expected the 544-unit shell faces to produce 35x35 grids\n");
        ++failures;
    }
    if (layout.coarsenedFaces != 0) {
        std::printf("FAIL: %d faces were coarsened; none here is oversized\n",
                    layout.coarsenedFaces);
        ++failures;
    }

    // Per-face lightmap scale must survive into the grid. A face authored at 32
    // covers the same world extent in half as many luxels as one authored at 16.
    {
        const auto scaled = buildRoom(32);
        hammer::render::LightmapLayout coarse =
            hammer::render::buildLightmapLayout(*scaled, options);
        int coarseWalls = 0;
        bool wrongScale = false;
        for (const auto& [key, rect] : coarse.faces) {
            (void)key;
            if (rect.lightmapScale != 32.0) wrongScale = true;
            // 544 units at 32 per luxel straddles half-luxels at both ends:
            // floor(-8.5) = -9, ceil(8.5) = 9, so the grid is 19 not 18. The
            // grid always covers at least the face, never less.
            if (rect.width == 19 && rect.height == 19) ++coarseWalls;
        }
        std::printf("lightmapscale 32: %zu faces, %d at 19x19\n",
                    coarse.faces.size(), coarseWalls);
        if (wrongScale) {
            std::printf("FAIL: a face did not keep its authored lightmapscale\n");
            ++failures;
        }
        if (coarseWalls == 0) {
            std::printf("FAIL: expected 19x19 grids at lightmapscale 32\n");
            ++failures;
        }
    }

    std::printf(failures == 0 ? "PASS\n" : "FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
