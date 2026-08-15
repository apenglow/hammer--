// Manual diagnostic: does the VRAD bake key change across repeated scene
// builds of the same map? VulkanRayTracedViewport re-solves the lightmap
// whenever radiosityKey() differs from the previous upload, so a key that
// churns between identical builds means a permanent re-bake.
//
// usage: hammer-bake-key-harness <gameinfo.txt> <map.vmf> [builds]
#include "VmfScene.hpp"
#include "VmfEditor.hpp"
#include "GameFileSystem.hpp"
#include "MaterialSystem.hpp"
#include "RayTracingScene.hpp"
#include "StudioModelSystem.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
// Mirrors VulkanRayTracedViewport::radiosityKey().
std::uint64_t radiosityKey(const hammer::render::RayTracingScene& scene)
{
    std::uint64_t key = 1469598103934665603ull;
    auto mix = [&key](std::uint64_t value) { key = (key ^ value) * 1099511628211ull; };
    mix(scene.radiosity.contentKey());
    mix(static_cast<std::uint64_t>(scene.staticPropVertexCount));
    mix(static_cast<std::uint64_t>(scene.radiosity.luxelLayout.width));
    mix(static_cast<std::uint64_t>(scene.radiosity.luxelLayout.height));
    mix(static_cast<std::uint64_t>(scene.radiosity.patchLayout.width));
    mix(static_cast<std::uint64_t>(scene.radiosity.patchLayout.height));
    mix(scene.lights.size());
    for (const hammer::render::RayTracingLight& light : scene.lights) {
        for (float component : {light.positionType[0], light.positionType[1],
                                light.positionType[2], light.positionType[3],
                                light.directionRange[0], light.directionRange[1],
                                light.directionRange[2], light.colorIntensity[0],
                                light.colorIntensity[1], light.colorIntensity[2],
                                light.colorIntensity[3]}) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &component, sizeof(bits));
            mix(bits);
        }
    }
    return key;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> <map.vmf> [builds]\n", argv[0]);
        return 2;
    }
    const int builds = argc > 3 ? std::atoi(argv[3]) : 4;
    // Grid-aligned by default: a drag that keeps every luxel count identical
    // while relocating the whole grid is the case a count-only key misses.
    const double editDelta = argc > 4 ? std::atof(argv[4]) : 128.0;
    const bool singleObject = argc > 5 ? std::atoi(argv[5]) != 0 : true;

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "game configure failed: %s\n", error.message.c_str());
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    auto models = std::make_shared<hammer::assets::StudioModelSystem>(fileSystem);

    QFile file(QString::fromLocal8Bit(argv[2]));
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    const auto document = hammer::vmf::Document::parse(file.readAll().toStdString());
    if (!document) {
        std::fprintf(stderr, "vmf parse failed\n");
        return 1;
    }
    auto scene = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
    scene->invalidateLineage();

    // Builds 0..n-2 repeat the same scene the way the animation timer does; the
    // last one repeats it after nudging one solid, the way an edit does. The key
    // must hold across the former and change across the latter.
    hammer::vmf::EditorModel editor{*document};

    std::uint64_t previous = 0;
    hammer::render::RadiosityData carried;
    for (int build = 0; build < builds; ++build) {
        const bool afterEdit = build == builds - 1;
        if (afterEdit) {
            editor.selectAll();
            // One object, not the whole map: a single grid-aligned brush drag is
            // the ordinary edit, and it is the case where luxel *counts* survive.
            if (singleObject) {
                // Specifically a brush that carries a lightmap. A tools/nodraw or
                // skybox brush contributes no luxel, so moving it legitimately
                // leaves the solve valid and would prove nothing here.
                int litBrushId = -1;
                for (const auto& brush : scene->brushes) {
                    const bool lit = std::any_of(
                        brush.faces.begin(), brush.faces.end(),
                        [](const hammer::vmf::FaceGeometry& face) {
                            std::string material = face.material;
                            for (char& c : material) c = char(std::tolower(c));
                            return material.rfind("tools/", 0) != 0 && !face.displacement;
                        });
                    if (lit) { litBrushId = brush.id; break; }
                }
                if (litBrushId < 0) {
                    std::fprintf(stderr, "no lit brush found\n");
                    return 1;
                }
                editor.setSelection({{hammer::vmf::ObjectType::Solid, litBrushId}});
            }
            std::printf("edit: translating %zu object(s) by %g (first type=%d id=%d)\n",
                        editor.selection().size(), editDelta,
                        editor.selection().empty()
                            ? -1 : int(editor.selection().front().type),
                        editor.selection().empty() ? -1 : editor.selection().front().id);
            if (!editor.translateSelection({editDelta, 0.0, 0.0})) {
                std::fprintf(stderr, "translateSelection failed\n");
                return 1;
            }
            scene = std::make_shared<hammer::vmf::Scene>(
                hammer::vmf::buildScene(editor.document()));
            scene->invalidateLineage();
        }
        hammer::render::RayTracingSceneBuilder builder(materials, models);
        hammer::render::RayTracingBuildOptions options;
        // What the viewport passes on each animation tick.
        options.animationSeconds = build * 0.1;
        // A materials-only rebuild carries the previous grid forward, which is
        // what the viewport now does on an animation tick.
        if (build > 0 && !afterEdit && carried.valid()) options.reuseRadiosity = &carried;
        QElapsedTimer timer;
        timer.start();
        const hammer::render::RayTracingScene built = builder.build(*scene, options);
        const qint64 milliseconds = timer.elapsed();
        const std::uint64_t key = radiosityKey(built);
        if (build == 0) carried = built.radiosity;
        // Independent check that the edit reached the geometry at all: a solid
        // made only of tool materials contributes no luxel, so an unchanged
        // radiosity key is correct for it and proves nothing on its own.
        std::uint64_t geometryKey = 1469598103934665603ull;
        for (const auto& vertex : built.vertices) {
            std::uint32_t bits = 0;
            for (float component : {vertex.position[0], vertex.position[1], vertex.position[2]}) {
                std::memcpy(&bits, &component, sizeof(bits));
                geometryKey = (geometryKey ^ bits) * 1099511628211ull;
            }
        }
        std::printf("    geometryKey=%llu\n",
                    static_cast<unsigned long long>(geometryKey));
        std::printf("build %d%s: %lld ms key=%llu %s\n"
                    "    luxels=%zu patches=%zu atlas=%dx%d patchAtlas=%dx%d lights=%zu\n"
                    "    animatedContent=%d animatedMaterialContent=%d cameraSprites=%d\n"
                    "    radiosityValid=%d status='%s' propVerts=%zu\n",
                    build, afterEdit ? " (after edit)" : "",
                    static_cast<long long>(milliseconds),
                    static_cast<unsigned long long>(key),
                    build && key != previous ? "*** CHANGED ***" : "",
                    built.radiosity.luxels.size(), built.radiosity.patches.size(),
                    built.radiosity.luxelLayout.width, built.radiosity.luxelLayout.height,
                    built.radiosity.patchLayout.width, built.radiosity.patchLayout.height,
                    built.lights.size(), int(built.hasAnimatedContent),
                    int(built.hasAnimatedMaterialContent), int(built.hasCameraFacingSprites),
                    int(built.radiosity.valid()), built.radiosity.status.c_str(),
                    static_cast<std::size_t>(built.staticPropVertexCount));
        std::fflush(stdout);
        previous = key;
    }
    return 0;
}
