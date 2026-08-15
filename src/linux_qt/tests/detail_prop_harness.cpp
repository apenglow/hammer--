// Manual diagnostic harness: renders a ground surface whose material declares
// a "%detailtype" and screenshots the detail props VBSP would scatter over it,
// through both the OpenGL and the ray-traced viewport. Not a ctest - it needs
// a game mount with a detail.vbsp.
//
//   detail_prop_harness <gameinfo.txt> <material> [outPrefix] [gl|rt] [detail.vbsp]
#include "DetailObjects.hpp"
#include "MapViewWidget.hpp"
#include "MaterialSystem.hpp"
#include "GameFileSystem.hpp"
#include "VmfScene.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QSurfaceFormat>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(4, 6);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <gameinfo.txt> <material> [outPrefix] [gl|rt] [detail.vbsp]\n",
                     argv[0]);
        return 2;
    }

    auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
    hammer::assets::AssetError error;
    if (!fileSystem->configure(argv[1], &error)) {
        std::fprintf(stderr, "configure failed: %s\n", error.message.c_str());
        return 1;
    }
    auto materials = std::make_shared<hammer::assets::MaterialSystem>(fileSystem);

    const std::string groundMaterial = argv[2];
    const auto material = materials->material(groundMaterial);
    if (!material || material->missing) {
        std::fprintf(stderr, "material not found: %s\n", groundMaterial.c_str());
        return 1;
    }
    const std::string dictionaryName = argc > 5 ? argv[5] : "detail.vbsp";
    std::fprintf(stderr, "material=%s %%detailtype=\"%s\" dictionary=%s\n",
                 groundMaterial.c_str(), material->detailType.c_str(), dictionaryName.c_str());

    const auto dictionary =
        hammer::assets::loadDetailObjectDictionary(*fileSystem, dictionaryName);
    std::fprintf(stderr, "dictionary types=%zu\n", dictionary.types.size());
    for (const auto& type : dictionary.types) {
        std::fprintf(stderr, "  %s density=%g groups=%zu\n", type.name.c_str(),
                     static_cast<double>(type.density), type.groups.size());
    }

    // A 1024x1024 ground slab in the detail material, plus a displacement of
    // the same material beside it so the painted-alpha group selection shows.
    const auto box = [](int firstId, const char* material, double ax, double ay, double az,
                        double bx, double by, double bz, const std::string& topExtra) {
        char buffer[4096];
        std::snprintf(buffer, sizeof(buffer),
            "solid\n{\n\"id\" \"%d\"\n"
            "side\n{\n\"id\" \"%d\"\n\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n\"material\" \"%s\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n%s}\n"
            "side\n{\n\"id\" \"%d\"\n\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 -1 0 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"%d\"\n\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"%d\"\n\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[0 1 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"%d\"\n\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "side\n{\n\"id\" \"%d\"\n\"plane\" \"(%g %g %g) (%g %g %g) (%g %g %g)\"\n\"material\" \"tools/toolsnodraw\"\n\"uaxis\" \"[1 0 0 0] 0.25\"\n\"vaxis\" \"[0 0 -1 0] 0.25\"\n\"lightmapscale\" \"16\"\n}\n"
            "}\n",
            firstId, firstId + 1, ax, ay, bz, ax, by, bz, bx, by, bz, material, topExtra.c_str(),
            firstId + 2, ax, by, az, ax, ay, az, bx, ay, az,
            firstId + 3, ax, ay, bz, ax, ay, az, ax, by, az,
            firstId + 4, bx, by, bz, bx, by, az, bx, ay, az,
            firstId + 5, bx, ay, bz, bx, ay, az, ax, ay, az,
            firstId + 6, ax, by, bz, ax, by, az, bx, by, az);
        return std::string(buffer);
    };

    // A power-3 displacement with a diagonal alpha ramp, so the group the
    // painted alpha selects is visible as a gradient of grass density.
    std::string dispInfo = "dispinfo\n{\n\"power\" \"3\"\n\"startposition\" \"[512 -512 0]\"\n"
                           "\"elevation\" \"0\"\n\"subdiv\" \"0\"\nnormals\n{\n";
    for (int row = 0; row < 9; ++row) {
        dispInfo += "\"row" + std::to_string(row) + "\" \"";
        for (int column = 0; column < 9; ++column) dispInfo += "0 0 1  ";
        dispInfo += "\"\n";
    }
    dispInfo += "}\ndistances\n{\n";
    for (int row = 0; row < 9; ++row) {
        dispInfo += "\"row" + std::to_string(row) + "\" \"";
        for (int column = 0; column < 9; ++column) dispInfo += "0 ";
        dispInfo += "\"\n";
    }
    dispInfo += "}\noffsets\n{\n";
    for (int row = 0; row < 9; ++row) {
        dispInfo += "\"row" + std::to_string(row) + "\" \"";
        for (int column = 0; column < 9; ++column) dispInfo += "0 0 0  ";
        dispInfo += "\"\n";
    }
    // HARNESS_DISP_ALPHA pins the whole displacement to one painted alpha,
    // which is how a detail type's low-alpha group (often the only one with
    // model entries) can be driven on demand.
    const char* forcedAlpha = std::getenv("HARNESS_DISP_ALPHA");
    dispInfo += "}\nalphas\n{\n";
    for (int row = 0; row < 9; ++row) {
        dispInfo += "\"row" + std::to_string(row) + "\" \"";
        for (int column = 0; column < 9; ++column) {
            dispInfo += (forcedAlpha ? std::string(forcedAlpha)
                                     : std::to_string(column * 255 / 8)) + " ";
        }
        dispInfo += "\"\n";
    }
    dispInfo += "}\n}\n";

    const bool coverTest = std::getenv("HARNESS_COVER") != nullptr;
    const std::string vmf =
        "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
        "\"detailvbsp\" \"" + dictionaryName + "\"\n"
        "\"detailmaterial\" \"detail/detailsprites\"\n" +
        box(2, groundMaterial.c_str(), -512, -512, -32, 512, 512, 0, {}) +
        box(20, groundMaterial.c_str(), 512, -512, -32, 1536, 512, 0, dispInfo) +
        // HARNESS_COVER puts a metal slab over the displacement and textures
        // the displacement brush's other sides with the ground material, which
        // is how a real map is built and how detail props were reported
        // showing up under metal surfaces.
        (std::getenv("HARNESS_COVER")
            ? box(40, "metal/metalwall001a", 640, -384, 48, 1408, 384, 64, {})
            : std::string()) +
        "}\n";

    std::string finalVmf = vmf;
    if (coverTest) {
        std::string::size_type at = 0;
        while ((at = finalVmf.find("tools/toolsnodraw", at)) != std::string::npos) {
            finalVmf.replace(at, std::string("tools/toolsnodraw").size(), groundMaterial);
            at += groundMaterial.size();
        }
    }
    const auto document = hammer::vmf::Document::parse(finalVmf);
    if (!document) {
        std::fprintf(stderr, "vmf parse failed\n");
        return 1;
    }
    auto scene = std::make_shared<hammer::vmf::Scene>(hammer::vmf::buildScene(*document));
    scene->skyName = "sky_day01_01";
    scene->invalidateLineage();

    const auto emission = hammer::assets::emitDetailProps(
        *scene, dictionary, [&materials](std::string_view name) {
            const auto found = materials->material(std::string(name));
            return found ? found->detailType : std::string();
        });
    std::size_t sprites = 0, models = 0;
    for (const auto& prop : emission.props) {
        if (prop.type == hammer::assets::DetailPropType::Model) ++models;
        else ++sprites;
    }
    std::fprintf(stderr, "emitted %zu detail props (%zu sprites, %zu models, %zu overflowed)\n",
                 emission.props.size(), sprites, models, emission.overflowed);

    // Detail MODELS are ordinary studio models, and rare enough that a general
    // shot may not contain one. Report whether the first one loads and frame it
    // directly, so the model path is actually exercised and visible.
    hammer::vmf::Vec3 modelOrigin{};
    bool haveModel = false;
    {
        hammer::assets::StudioModelSystem studioModels(fileSystem);
        for (const auto& prop : emission.props) {
            if (prop.type != hammer::assets::DetailPropType::Model) continue;
            const auto loaded = studioModels.model(prop.model);
            std::fprintf(stderr, "first detail model %s: %s at (%g %g %g)\n",
                         prop.model.c_str(),
                         loaded && loaded->valid ? "loaded" : "FAILED TO LOAD",
                         prop.origin.x, prop.origin.y, prop.origin.z);
            modelOrigin = prop.origin;
            haveModel = loaded && loaded->valid;
            break;
        }
    }

    MapViewWidget view(MapViewWidget::Kind::Perspective);
    view.resize(800, 600);
    view.setGridVisible(false);
    view.setMaterialSystem(materials);
    view.setMaterialRenderingEnabled(true);
    const QString modeArg = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("gl");
    view.setTexturedRenderMode(modeArg == QStringLiteral("rt")
        ? MapViewWidget::TexturedRenderMode::RayTracedPreview
        : MapViewWidget::TexturedRenderMode::Shaded);
    view.setMaterialEffectsEnabled(true, true, true, true, true, true);
    view.setMaterialEffectIntensities(1.0f, 1.0f, 1.0f);
    view.setScene(scene, true);
    view.show();

    const QString prefix = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("detail");
    const struct Shot { const char* name; hammer::vmf::Vec3 eye; hammer::vmf::Vec3 target; } shots[] = {
        {"ground", {-400.0, -400.0, 40.0}, {200.0, 200.0, 20.0}},
        {"low", {-100.0, -300.0, 16.0}, {300.0, 100.0, 16.0}},
        {"disp", {700.0, -500.0, 60.0}, {1100.0, 100.0, 10.0}},
        {"above", {0.0, -600.0, 600.0}, {300.0, 0.0, 0.0}},
    };
    std::vector<Shot> shotList(std::begin(shots), std::end(shots));
    if (haveModel) {
        // Eye level with the model, close enough that its geometry - not the
        // billboards around it - fills the frame.
        shotList.push_back({"model", {modelOrigin.x - 34.0, modelOrigin.y - 34.0,
                                      modelOrigin.z + 14.0}, modelOrigin});
    }
    for (const Shot& shot : shotList) {
        view.setCameraTransform(shot.eye, shot.target);
        QApplication::processEvents();
        QElapsedTimer frameTimer;
        frameTimer.start();
        view.repaint();
        const qint64 firstFrameMs = frameTimer.elapsed();
        QApplication::processEvents();
        // A second repaint with the camera unchanged is the deferred
        // billboard/sprite refresh path, which is what runs while the editor
        // sits still - and what a heavy detail-prop scene makes expensive.
        frameTimer.restart();
        view.repaint();
        const qint64 settledFrameMs = frameTimer.elapsed();
        std::fprintf(stderr, "shot %s: first frame %lld ms, settled frame %lld ms\n",
                     shot.name, static_cast<long long>(firstFrameMs),
                     static_cast<long long>(settledFrameMs));
        const QImage frame = view.grab().toImage();
        const QString file = QStringLiteral("%1_%2_%3.png")
                                 .arg(prefix, modeArg, QLatin1StringView(shot.name));
        frame.save(file);
        std::fprintf(stderr, "wrote %s (%dx%d)\n", qPrintable(file), frame.width(), frame.height());
    }
    return 0;
}
