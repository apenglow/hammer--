// Manual diagnostic harness: renders one flat floor lit by a single coloured
// light through the ray-traced preview and reports the measured hue of the lit
// area. Built to answer "my red light renders purple" with numbers instead of
// eyeballing. Not registered as a ctest.
//
// usage: hammer-rt-light-color-harness <gameinfo.txt> ["R G B brightness"|map.vmf] [outPrefix]
//        [eyeX eyeY eyeZ targetX targetY targetZ]
#include "MapViewWidget.hpp"
#include "VmfScene.hpp"
#include "GameFileSystem.hpp"
#include "MaterialSystem.hpp"
#include "RayTracingScene.hpp"
#include "StudioModelSystem.hpp"

#include <QApplication>
#include <QImage>
#include <QSurfaceFormat>

#include <QFile>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(4, 6);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> [\"R G B brightness\"] [outPrefix]\n",
                     argv[0]);
        return 2;
    }
    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "game configure failed: %s\n", error.message.c_str());
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);

    const std::string lightColor = argc > 2 ? argv[2] : "255 21 28 10";
    const std::string material = "dev/dev_measuregeneric01";
    // A single floor slab plus one light above it. Nothing is sealed, so with no
    // sky in the map every contribution other than the light itself is meant to
    // be zero - which makes any hue shift in the result unambiguous.
    const std::string vmf =
        "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
        "solid\n{\n\"id\" \"2\"\n"
        "side\n{\n\"id\" \"3\"\n\"plane\" \"(-512 -512 0) (-512 512 0) (512 512 0)\"\n"
        "\"material\" \"" + material + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n"
        "\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"4\"\n\"plane\" \"(-512 512 -32) (-512 -512 -32) (512 -512 -32)\"\n"
        "\"material\" \"" + material + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n"
        "\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"5\"\n\"plane\" \"(-512 -512 0) (-512 -512 -32) (-512 512 -32)\"\n"
        "\"material\" \"" + material + "\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n"
        "\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"6\"\n\"plane\" \"(512 512 0) (512 512 -32) (512 -512 -32)\"\n"
        "\"material\" \"" + material + "\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n"
        "\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"7\"\n\"plane\" \"(512 -512 0) (512 -512 -32) (-512 -512 -32)\"\n"
        "\"material\" \"" + material + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n"
        "\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"8\"\n\"plane\" \"(-512 512 0) (-512 512 -32) (512 512 -32)\"\n"
        "\"material\" \"" + material + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n"
        "\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "}\n}\n"
        "entity\n{\n\"id\" \"30\"\n\"classname\" \"light\"\n"
        "\"origin\" \"0 0 96\"\n"
        "\"_light\" \"" + lightColor + "\"\n"
        "\"_quadratic_attn\" \"1\"\n}\n";

    // A real map can be measured instead of the synthetic floor: pass its path.
    std::string source = vmf;
    const bool fromFile = lightColor.size() > 4 &&
                          lightColor.compare(lightColor.size() - 4, 4, ".vmf") == 0;
    if (fromFile) {
        QFile file(QString::fromStdString(lightColor));
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot open %s\n", lightColor.c_str());
            return 1;
        }
        source = file.readAll().toStdString();
        std::fprintf(stderr, "loaded %s (%zu bytes)\n", lightColor.c_str(), source.size());
    }
    const auto document = hammer::vmf::Document::parse(source);
    if (!document) {
        std::fprintf(stderr, "vmf parse failed\n");
        return 1;
    }
    auto scene = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
    scene->invalidateLineage();

    // Build the ray-tracing scene directly as well, to report how many
    // materials ended up without a usable atlas rect. Those render as the
    // missing-texture magenta, which is easy to mistake for a lighting bug.
    {
        hammer::render::RayTracingSceneBuilder builder(
            materials, std::make_shared<hammer::assets::StudioModelSystem>(fileSystem));
        hammer::render::RayTracingBuildOptions options;
        const hammer::render::RayTracingScene built = builder.build(*scene, options);
        int missingBase = 0;
        for (const auto& gpuMaterial : built.materials)
            if (!(gpuMaterial.baseRect[2] > 0.0f && gpuMaterial.baseRect[3] > 0.0f))
                ++missingBase;
        int baseEqualsBump = 0;
        for (const auto& gpuMaterial : built.materials) {
            if (gpuMaterial.bumpRect[2] > 0.0f &&
                gpuMaterial.baseRect == gpuMaterial.bumpRect) ++baseEqualsBump;
        }
        std::fprintf(stderr, "materials whose base rect equals their bump rect: %d\n",
                     baseEqualsBump);
        for (std::size_t i = 0; i < built.materials.size() && i < 12; ++i) {
            const auto& m = built.materials[i];
            std::fprintf(stderr, "  [%zu] base=%.4f,%.4f,%.4f,%.4f bump=%.4f,%.4f,%.4f,%.4f\n", i,
                         m.baseRect[0], m.baseRect[1], m.baseRect[2], m.baseRect[3],
                         m.bumpRect[0], m.bumpRect[1], m.bumpRect[2], m.bumpRect[3]);
        }
        if (built.atlas.valid() && std::getenv("HARNESS_DUMP_ATLAS")) {
            for (int layer = 0; layer < built.atlas.layers; ++layer) {
                const QImage page(built.atlas.rgba.data() +
                                      std::size_t(layer) * built.atlas.width * built.atlas.height * 4,
                                  built.atlas.width, built.atlas.height, QImage::Format_RGBA8888);
                page.save(QStringLiteral("atlas_%1.png").arg(layer));
            }
        }
        std::fprintf(stderr,
                     "rt scene: materials=%zu without-base-rect=%d atlas=%dx%dx%d error=%s\n",
                     built.materials.size(), missingBase, built.atlas.width,
                     built.atlas.height, built.atlas.layers,
                     built.error.empty() ? "none" : built.error.c_str());
    }

    MapViewWidget view(MapViewWidget::Kind::Perspective);
    view.resize(512, 512);
    view.setGridVisible(false);
    view.setMaterialSystem(materials);
    view.setMaterialRenderingEnabled(true);
    // HARNESS_MODE=smp renders the same view through the OpenGL material
    // renderer instead, so the two pipelines can be compared directly.
    const char* mode = std::getenv("HARNESS_MODE");
    const bool useRayTracing = !mode || std::strcmp(mode, "smp") != 0;
    view.setTexturedRenderMode(useRayTracing
        ? MapViewWidget::TexturedRenderMode::RayTracedPreview
        : MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons);
    // HARNESS_EFFECTS=phong,specular,bump,lightwarp,selfillum,rim as 0/1 digits
    // lets a hue problem be bisected to the term that causes it.
    const char* effects = std::getenv("HARNESS_EFFECTS");
    auto effectOn = [effects](int index) {
        return !effects || int(std::strlen(effects)) <= index || effects[index] != '0';
    };
    std::fprintf(stderr, "effects phong=%d specular=%d bump=%d warp=%d selfillum=%d rim=%d\n",
                 effectOn(0), effectOn(1), effectOn(2), effectOn(3), effectOn(4), effectOn(5));
    view.setMaterialEffectsEnabled(effectOn(0), effectOn(1), effectOn(2),
                                   effectOn(3), effectOn(4), effectOn(5));
    view.setMaterialEffectIntensities(1.0f, 1.0f, 1.0f);
    view.setScene(scene, false);
    hammer::vmf::Vec3 eye{0.0, -260.0, 190.0};
    hammer::vmf::Vec3 target{0.0, 0.0, 0.0};
    if (argc > 9) {
        eye = {std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6])};
        target = {std::atof(argv[7]), std::atof(argv[8]), std::atof(argv[9])};
    }
    view.setCameraTransform(eye, target);
    view.show();

    // Let the accumulation settle so the measurement is of the converged image.
    QImage frame;
    int passes = 24;
    if (const char* override = std::getenv("HARNESS_PASSES")) passes = std::max(1, std::atoi(override));
    for (int pass = 0; pass < passes; ++pass) {
        QApplication::processEvents();
        view.repaint();
        QApplication::processEvents();
        frame = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
    }

    double red = 0.0, green = 0.0, blue = 0.0;
    int counted = 0;
    int peakRed = 0, peakGreen = 0, peakBlue = 0;
    for (int y = frame.height() / 4; y < frame.height() * 3 / 4; ++y) {
        for (int x = frame.width() / 4; x < frame.width() * 3 / 4; ++x) {
            const QColor texel = frame.pixelColor(x, y);
            if (texel.red() + texel.green() + texel.blue() < 12) continue;
            red += texel.red();
            green += texel.green();
            blue += texel.blue();
            ++counted;
            if (texel.red() + texel.green() + texel.blue() > peakRed + peakGreen + peakBlue) {
                peakRed = texel.red();
                peakGreen = texel.green();
                peakBlue = texel.blue();
            }
        }
    }
    const QString prefix = argc > 3 ? QString::fromLocal8Bit(argv[3])
                                    : QStringLiteral("rt_light_color");
    frame.save(prefix + QStringLiteral(".png"));
    if (counted == 0) {
        std::printf("light \"%s\": nothing lit in the sampled region\n", lightColor.c_str());
        return 0;
    }
    std::printf("light \"%s\"  lit texels=%d\n", lightColor.c_str(), counted);
    std::printf("  mean rgb = %.1f %.1f %.1f\n", red / counted, green / counted, blue / counted);
    std::printf("  peak rgb = %d %d %d\n", peakRed, peakGreen, peakBlue);
    std::printf("  mean green/red = %.3f   blue/red = %.3f (both near zero for a red light)\n",
                green / std::max(red, 1.0), blue / std::max(red, 1.0));
    return 0;
}
