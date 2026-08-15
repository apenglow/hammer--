// Manual diagnostic harness: renders a move_rope / keyframe_rope chain over a
// floor brush through the real MapViewWidget pipeline and saves PNGs. Not a
// ctest - it needs a game mount - but it is the only way to see that the
// camera-facing rope ribbon actually reaches the screen in both the OpenGL and
// the ray-traced viewport.
//
//   rope_preview_harness <gameinfo.txt> [outPrefix] [gl|rt]
#include "MapViewWidget.hpp"
#include "VmfScene.hpp"
#include "GameFileSystem.hpp"
#include "MaterialSystem.hpp"

#include <QApplication>
#include <QImage>
#include <QSurfaceFormat>

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(4, 6);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> [outPrefix] [gl|rt]\n", argv[0]);
        return 2;
    }

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed: %s\n", error.message.c_str());
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);

    // A floor to see the rope against, and a three-keyframe chain strung across
    // it: two links, the first slack and the second taut.
    const std::string vmf =
        std::string("world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
        "solid\n{\n\"id\" \"2\"\n"
        "side\n{\n\"id\" \"3\"\n\"plane\" \"(-1024 -1024 0) (-1024 1024 0) (1024 1024 0)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"4\"\n\"plane\" \"(-1024 1024 -32) (-1024 -1024 -32) (1024 -1024 -32)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"5\"\n\"plane\" \"(-1024 -1024 0) (-1024 -1024 -32) (-1024 1024 -32)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"6\"\n\"plane\" \"(1024 1024 0) (1024 1024 -32) (1024 -1024 -32)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"7\"\n\"plane\" \"(1024 -1024 0) (1024 -1024 -32) (-1024 -1024 -32)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "side\n{\n\"id\" \"8\"\n\"plane\" \"(-1024 1024 0) (-1024 1024 -32) (1024 1024 -32)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
        "}\n}\n") +
        "entity\n{\n\"id\" \"30\"\n\"classname\" \"move_rope\"\n\"targetname\" \"rope_a\"\n"
        "\"NextKey\" \"rope_b\"\n\"Slack\" \"250\"\n\"Width\" \"4\"\n\"Subdiv\" \"3\"\n"
        "\"Type\" \"0\"\n\"TextureScale\" \"1\"\n\"RopeMaterial\" \"cable/cable\"\n"
        "\"origin\" \"-256 0 256\"\n}\n"
        "entity\n{\n\"id\" \"31\"\n\"classname\" \"keyframe_rope\"\n\"targetname\" \"rope_b\"\n"
        "\"NextKey\" \"rope_c\"\n\"Slack\" \"0\"\n\"Width\" \"4\"\n\"Subdiv\" \"3\"\n"
        "\"Type\" \"0\"\n\"RopeMaterial\" \"cable/cable\"\n\"origin\" \"0 0 192\"\n}\n"
        "entity\n{\n\"id\" \"32\"\n\"classname\" \"keyframe_rope\"\n\"targetname\" \"rope_c\"\n"
        "\"origin\" \"256 0 256\"\n}\n";

    const auto document = hammer::vmf::Document::parse(vmf);
    if (!document) {
        std::fprintf(stderr, "vmf parse failed\n");
        return 1;
    }
    auto scene = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
    scene->skyName = "sky_day01_01";
    scene->invalidateLineage();

    MapViewWidget view(MapViewWidget::Kind::Perspective);
    view.resize(800, 600);
    view.setGridVisible(false);
    view.setMaterialSystem(materials);
    view.setMaterialRenderingEnabled(true);
    const QString modeArg = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("gl");
    view.setTexturedRenderMode(modeArg == QStringLiteral("rt")
        ? MapViewWidget::TexturedRenderMode::RayTracedPreview
        : MapViewWidget::TexturedRenderMode::Shaded);
    view.setMaterialEffectsEnabled(true, true, true, true, true, true);
    view.setMaterialEffectIntensities(1.0f, 1.0f, 1.0f);
    view.setScene(scene, true);
    view.show();

    const QString prefix = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("rope");
    const struct Shot { const char* name; hammer::vmf::Vec3 eye; } shots[] = {
        {"side", {0.0, -600.0, 220.0}},
        {"near", {-120.0, -260.0, 200.0}},
        // Along the rope: the ribbon has to stay visible edge-on.
        {"along", {-700.0, 0.0, 256.0}},
        {"above", {0.0, -120.0, 700.0}},
    };
    for (const Shot& shot : shots) {
        view.setCameraTransform(shot.eye, {0.0, 0.0, 200.0});
        QApplication::processEvents();
        view.repaint();
        QApplication::processEvents();
        const QImage frame = view.grab().toImage();
        const QString file = QStringLiteral("%1_%2_%3.png")
                                 .arg(prefix, modeArg, QLatin1StringView(shot.name));
        frame.save(file);
        std::fprintf(stderr, "wrote %s (%dx%d)\n", qPrintable(file), frame.width(), frame.height());
    }
    return 0;
}
