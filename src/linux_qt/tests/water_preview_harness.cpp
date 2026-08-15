// Manual diagnostic harness: renders a flat water brush through the real
// MapViewWidget hardware pipeline at several camera distances and saves the
// frames as PNGs. Not registered as a ctest; run by hand when debugging the
// water shader.
#include "MapViewWidget.hpp"
#include "VmfScene.hpp"
#include "GameFileSystem.hpp"
#include "MaterialSystem.hpp"

#include <QApplication>
#include <QImage>
#include <QSurfaceFormat>
#include <QTimer>

#include <QKeyEvent>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
std::shared_ptr<const hammer::vmf::Scene> buildMaterialPreviewScene(const QString& materialName,
                                                                    const std::string& skyName,
                                                                    bool water)
{
    const std::string material = materialName.toStdString();
    std::string vmf = "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n";
    int nextId = 2;

    const auto side = [&](const std::array<double, 9>& plane, const char* uAxis,
                          const char* vAxis, const std::string& extra = {}) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "side\n{\n\"id\" \"%d\"\n"
                      "\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n"
                      "\"material\" \"%s\"\n"
                      "\"uaxis\" \"[%s 0] 0.25\"\n\"vaxis\" \"[%s 0] 0.25\"\n"
                      "\"lightmapscale\" \"16\"\n",
                      nextId++, plane[0], plane[1], plane[2], plane[3], plane[4], plane[5],
                      plane[6], plane[7], plane[8], material.c_str(), uAxis, vAxis);
        vmf += buffer;
        vmf += extra;
        vmf += "}\n";
    };
    const auto box = [&](double ax, double ay, double az, double bx, double by, double bz,
                         const std::string& topExtra) {
        vmf += "solid\n{\n";
        char idLine[48];
        std::snprintf(idLine, sizeof(idLine), "\"id\" \"%d\"\n", nextId++);
        vmf += idLine;
        side({ax, ay, bz, ax, by, bz, bx, by, bz}, "1 0 0", "0 -1 0", topExtra); // +Z
        side({ax, by, az, ax, ay, az, bx, ay, az}, "1 0 0", "0 -1 0");           // -Z
        side({ax, ay, bz, ax, ay, az, ax, by, az}, "0 1 0", "0 0 -1");           // -X
        side({bx, by, bz, bx, by, az, bx, ay, az}, "0 1 0", "0 0 -1");           // +X
        side({bx, ay, bz, bx, ay, az, ax, ay, az}, "1 0 0", "0 0 -1");           // -Y
        side({ax, by, bz, ax, by, az, bx, by, az}, "1 0 0", "0 0 -1");           // +Y
        vmf += "}\n";
    };

    // Sculpted ground: gentle rolling bumps, flat under the cube, and an
    // alpha gradient so blend materials show their painted second texture.
    std::string disp = "dispinfo\n{\n\"power\" \"3\"\n"
                       "\"startposition\" \"[-256 -256 0]\"\n\"elevation\" \"0\"\n"
                       "\"subdiv\" \"0\"\nnormals\n{\n";
    std::string distances = "distances\n{\n";
    std::string alphas = "alphas\n{\n";
    for (int row = 0; row < 9; ++row) {
        char rowKey[16];
        std::snprintf(rowKey, sizeof(rowKey), "\"row%d\" \"", row);
        std::string normalsRow(rowKey), distancesRow(rowKey), alphasRow(rowKey);
        for (int column = 0; column < 9; ++column) {
            const double dx = column - 4.0;
            const double dy = row - 4.0;
            const double centerFalloff = std::min(1.0, (dx * dx + dy * dy) / 10.0);
            const double height = centerFalloff *
                (14.0 + 10.0 * std::sin(column * 1.1) * std::cos(row * 0.9));
            const int alpha = static_cast<int>(std::clamp(
                255.0 * (column + row) / 16.0, 0.0, 255.0));
            char value[64];
            normalsRow += column ? " 0 0 1" : "0 0 1";
            std::snprintf(value, sizeof(value), column ? " %.1f" : "%.1f", height);
            distancesRow += value;
            std::snprintf(value, sizeof(value), column ? " %d" : "%d", alpha);
            alphasRow += value;
        }
        disp += normalsRow + "\"\n";
        distances += distancesRow + "\"\n";
        alphas += alphasRow + "\"\n";
    }
    disp += "}\n" + distances + "}\n" + alphas + "}\n}\n";

    if (water) {
        // Water reads as a surface, not a solid: one flat brush covering the
        // same footprint as the displacement, nothing else.
        box(-256, -256, -32, 256, 256, 0, {});
    } else {
        box(-256, -256, -32, 256, 256, 0, disp); // ground
        box(-32, -32, 0, 32, 32, 64, {});        // cube
    }
    vmf += "}\n";

    const auto document = hammer::vmf::Document::parse(std::move(vmf));
    if (!document) return nullptr;
    auto scene = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
    scene->skyName = skyName;
    scene->invalidateLineage();
    return scene;
}
}

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    // Desktop OpenGL 4.6 only - the ES and pre-4.6 shader paths were removed, so
    // HARNESS_GLES no longer exists: an ES context is now refused by the renderer
    // instead of quietly compiling a different shader. Requesting 4.6 explicitly
    // matters here because the offscreen platform hands back exactly what is
    // asked for (unlike Wayland, which upgraded a 3.3 request to 4.6).
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(4, 6);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <gameinfo.txt> <material> [outPrefix]\n", argv[0]);
        return 2;
    }

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed: %s\n", error.message.c_str());
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);
    const auto material = materials->material(argv[2]);
    if (!material) {
        std::fprintf(stderr, "material not found: %s\n", argv[2]);
        return 1;
    }
    std::fprintf(stderr, "material=%s water=%d shader=%s normal=%dx%d\n", argv[2],
                 material->water ? 1 : 0, material->shader.c_str(),
                 material->waterNormalImage.width, material->waterNormalImage.height);
    std::fprintf(stderr,
                 "fog=(%.2f %.2f %.2f) start=%.1f end=%.1f reflectTint=(%.2f %.2f %.2f) "
                 "refractTint=(%.2f %.2f %.2f) reflectAmount=%.2f blendFactor=%.2f refractAmount=%.2f fresnel=%.2f\n",
                 material->waterFogColor[0], material->waterFogColor[1], material->waterFogColor[2],
                 material->waterFogStart, material->waterFogEnd,
                 material->waterReflectTint[0], material->waterReflectTint[1], material->waterReflectTint[2],
                 material->waterRefractTint[0], material->waterRefractTint[1], material->waterRefractTint[2],
                 material->waterReflectAmount, material->waterReflectBlendFactor,
                 material->waterRefractAmount, material->waterFresnelReflectance);

    // Flat 512x512 water brush, top at z=0 (same as the browser preview).
    const std::string vmf = std::string("world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
        "solid\n{\n\"id\" \"2\"\n") +
        "side\n{\n\"id\" \"3\"\n\"plane\" \"(-1024 -1024 0) (-1024 1024 0) (1024 1024 0)\"\n\"material\" \"" + argv[2] + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"4\"\n\"plane\" \"(-1024 1024 -32) (-1024 -1024 -32) (1024 -1024 -32)\"\n\"material\" \"" + argv[2] + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"5\"\n\"plane\" \"(-1024 -1024 0) (-1024 -1024 -32) (-1024 1024 -32)\"\n\"material\" \"" + argv[2] + "\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"6\"\n\"plane\" \"(1024 1024 0) (1024 1024 -32) (1024 -1024 -32)\"\n\"material\" \"" + argv[2] + "\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"7\"\n\"plane\" \"(1024 -1024 0) (1024 -1024 -32) (-1024 -1024 -32)\"\n\"material\" \"" + argv[2] + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"8\"\n\"plane\" \"(-1024 1024 0) (-1024 1024 -32) (1024 1024 -32)\"\n\"material\" \"" + argv[2] + "\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "}\n" +
        std::string("solid\n{\n\"id\" \"20\"\n") +
        "side\n{\n\"id\" \"21\"\n\"plane\" \"(-1024 -1024 -128) (-1024 1024 -128) (1024 1024 -128)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"22\"\n\"plane\" \"(-1024 1024 -160) (-1024 -1024 -160) (1024 -1024 -160)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"23\"\n\"plane\" \"(-1024 -1024 -128) (-1024 -1024 -160) (-1024 1024 -160)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"24\"\n\"plane\" \"(1024 1024 -128) (1024 1024 -160) (1024 -1024 -160)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"25\"\n\"plane\" \"(1024 -1024 -128) (1024 -1024 -160) (-1024 -1024 -160)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "side\n{\n\"id\" \"26\"\n\"plane\" \"(-1024 1024 -128) (-1024 1024 -160) (1024 1024 -160)\"\n\"material\" \"dev/dev_measuregeneric01\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n" +
        "}\n}\n";
    const auto document = hammer::vmf::Document::parse(vmf);
    if (!document) {
        std::fprintf(stderr, "vmf parse failed\n");
        return 1;
    }
    std::shared_ptr<const hammer::vmf::Scene> scene;
    if (argc > 6 && QString::fromLocal8Bit(argv[6]) == QStringLiteral("browser")) {
        scene = buildMaterialPreviewScene(QString::fromLocal8Bit(argv[2]),
                                          argc > 4 ? argv[4] : "sky_day01_01",
                                          material->water);
    } else {
        auto built = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
        built->skyName = argc > 4 ? argv[4] : "sky_day01_01";
        built->invalidateLineage();
        scene = built;
    }

    MapViewWidget view(MapViewWidget::Kind::Perspective);
    // HARNESS_SIZE=WxH emulates a small panel. Viewport size drives texture LOD,
    // so a 460x400 Material Browser preview samples much deeper mip levels than
    // an 800x600 grab - which is exactly where bump-normal flattening bites.
    int viewWidth = 800, viewHeight = 600;
    if (const char* size = std::getenv("HARNESS_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 32 && h > 32) {
            viewWidth = w;
            viewHeight = h;
        }
    }
    std::fprintf(stderr, "viewport=%dx%d\n", viewWidth, viewHeight);
    view.resize(viewWidth, viewHeight);
    view.setGridVisible(false);
    view.setMaterialSystem(materials);
    view.setMaterialRenderingEnabled(true);
    const QString modeArg = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
    view.setTexturedRenderMode(modeArg == QStringLiteral("rt")
        ? MapViewWidget::TexturedRenderMode::RayTracedPreview
        : modeArg == QStringLiteral("smp")
            ? MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons
            : MapViewWidget::TexturedRenderMode::Shaded);
    view.setMaterialEffectsEnabled(true, true, true, true, true, true);
    view.setMaterialEffectIntensities(1.0f, 1.0f, 1.0f);
    view.setScene(scene, true);
    Q_UNUSED(argc);
    view.show();

    const QString prefix = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("water");

    // The Material Browser never positions its camera explicitly - it takes
    // whatever framing setScene(scene, fit=true) chooses. Capture that first,
    // before the shot list below overrides the camera, so this rig can show
    // the same view the browser panel actually displays.
    {
        QApplication::processEvents();
        view.repaint();
        QApplication::processEvents();
        const QString file = QStringLiteral("%1_asframed.png").arg(prefix);
        view.grab().toImage().save(file);
        std::fprintf(stderr, "wrote %s (as framed by setScene)\n", qPrintable(file));
    }

    const struct Shot { const char* name; hammer::vmf::Vec3 eye; } shots[] = {
        {"zoom1", {-90.0, -260.0, 120.0}},
        {"zoom2", {-60.0, -170.0, 80.0}},
        {"zoom3", {-140.0, -140.0, 200.0}},
        {"shore_near", {0.0, -1200.0, 80.0}},
        {"shore_far", {0.0, -4200.0, 260.0}},
        {"high_near", {0.0, -900.0, 500.0}},
        {"high_far", {0.0, -3600.0, 2000.0}},
        {"inside", {-700.0, -700.0, 500.0}},
        {"vfar", {0.0, -9000.0, 700.0}},
        {"vfar_low", {0.0, -7000.0, 200.0}},
        // Steep downward views. Camera-centred reflection artifacts sit directly
        // below the viewer, which every shot above frames out of view entirely.
        {"steep", {0.0, -300.0, 900.0}},
        {"steeper", {0.0, -150.0, 700.0}},
        {"overhead", {0.0, -40.0, 500.0}},
    };
    for (const Shot& shot : shots) {
        view.setCameraTransform(shot.eye, {0.0, 0.0, 0.0});
        QApplication::processEvents();
        view.repaint();
        QApplication::processEvents();
        const QImage frame = view.grab().toImage();
        const QString file = QStringLiteral("%1_%2.png").arg(prefix, QLatin1StringView(shot.name));
        frame.save(file);
        std::fprintf(stderr, "wrote %s (%dx%d)\n", qPrintable(file), frame.width(), frame.height());
    }
    // The End key in a perspective view is wired to the "overhead" shot above.
    // Capture it so the two stay in step: this frame must match _overhead.png.
    {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
        QApplication::sendEvent(&view, &press);
        QApplication::processEvents();
        view.repaint();
        QApplication::processEvents();
        const QString file = QStringLiteral("%1_endkey.png").arg(prefix);
        view.grab().toImage().save(file);
        std::fprintf(stderr, "wrote %s (End key)\n", qPrintable(file));
    }
    return 0;
}
