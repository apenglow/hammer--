// Presence-overlay regression check: renders a MapDocumentWidget offscreen,
// injects collaborator poses, and asserts the views' pixels actually change —
// the whole point of the overlay is pixels, so that is what is tested.
//
//   hammer-collab-overlay-harness -platform offscreen [output-dir]
//
// With an output dir it also writes before/after PNGs for eyeballing.

#include "CollabSession.hpp"
#include "FaceEditSheet.hpp"
#include "MapDocumentWidget.hpp"
#include "MapViewWidget.hpp"
#include "VmfDocument.hpp"
#include "VmfSync.hpp"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void require(bool condition, const char* message)
{
    std::printf("%s - %s\n", condition ? "ok" : "FAIL", message);
    if (!condition) ++failures;
}

// One small world cube so the scene is not empty.
const char* minimalMap()
{
    return "world\n{\n\t\"id\" \"1\"\n\t\"classname\" \"worldspawn\"\n"
           "\tsolid\n\t{\n\t\t\"id\" \"10\"\n"
           "\t\tside { \"id\" \"100\" \"plane\" \"(0 64 64) (64 64 64) (64 0 64)\" \"material\" \"TOOLS/TOOLSNODRAW\" \"uaxis\" \"[1 0 0 0] 0.25\" \"vaxis\" \"[0 -1 0 0] 0.25\" }\n"
           "\t\tside { \"id\" \"101\" \"plane\" \"(0 0 0) (64 0 0) (64 64 0)\" \"material\" \"TOOLS/TOOLSNODRAW\" \"uaxis\" \"[1 0 0 0] 0.25\" \"vaxis\" \"[0 -1 0 0] 0.25\" }\n"
           "\t\tside { \"id\" \"102\" \"plane\" \"(0 64 64) (0 0 64) (0 0 0)\" \"material\" \"TOOLS/TOOLSNODRAW\" \"uaxis\" \"[0 1 0 0] 0.25\" \"vaxis\" \"[0 0 -1 0] 0.25\" }\n"
           "\t\tside { \"id\" \"103\" \"plane\" \"(64 64 0) (64 0 0) (64 0 64)\" \"material\" \"TOOLS/TOOLSNODRAW\" \"uaxis\" \"[0 1 0 0] 0.25\" \"vaxis\" \"[0 0 -1 0] 0.25\" }\n"
           "\t\tside { \"id\" \"104\" \"plane\" \"(64 64 64) (0 64 64) (0 64 0)\" \"material\" \"TOOLS/TOOLSNODRAW\" \"uaxis\" \"[1 0 0 0] 0.25\" \"vaxis\" \"[0 0 -1 0] 0.25\" }\n"
           "\t\tside { \"id\" \"105\" \"plane\" \"(64 0 0) (0 0 0) (0 0 64)\" \"material\" \"TOOLS/TOOLSNODRAW\" \"uaxis\" \"[1 0 0 0] 0.25\" \"vaxis\" \"[0 0 -1 0] 0.25\" }\n"
           "\t}\n}\n";
}

int differingPixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size()) return -1;
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* rowA = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* rowB = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (rowA[x] != rowB[x]) ++count;
        }
    }
    return count;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString outputDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{};
    if (!outputDir.isEmpty()) QDir().mkpath(outputDir);

    MapDocumentWidget document;
    document.resize(1200, 800);
    document.show();
    for (int spin = 0; spin < 100; ++spin) QApplication::processEvents();
    document.adoptCollabDocument(*hammer::vmf::Document::parse(minimalMap()));
    for (int spin = 0; spin < 100; ++spin) QApplication::processEvents();

    const auto grabAll = [&] { return document.grab().toImage(); };

    const QImage before = grabAll();
    require(!before.isNull(), "baseline grab renders");

    // The default perspective camera sits at (512,-512,384) looking toward
    // the origin; an avatar at the origin is squarely in front of it, and in
    // every 2D view it lands near the cube.
    QList<CollabPeerPose> poses;
    poses.append({2000000, QStringLiteral("Alice"), 0.0, 0.0, 64.0, 0.0, 135.0});
    document.setCollabPeerPoses(poses);
    for (int spin = 0; spin < 50; ++spin) QApplication::processEvents();
    const QImage withPeer = grabAll();

    const int changed = differingPixels(before, withPeer);
    std::printf("info - %d pixels changed with a visible peer\n", changed);
    require(changed > 200, "a peer avatar and name tag draw visibly");

    // Behind the camera: must neither crash nor spray mirrored garbage; the
    // overlay may legitimately draw nothing.
    QList<CollabPeerPose> behind;
    behind.append({2000000, QStringLiteral("Alice"), 900.0, -900.0, 550.0, 0.0, 135.0});
    document.setCollabPeerPoses(behind);
    for (int spin = 0; spin < 50; ++spin) QApplication::processEvents();
    const QImage withBehind = grabAll();
    require(!withBehind.isNull(), "behind-camera pose renders without crashing");

    // Clearing removes the avatar again.
    document.setCollabPeerPoses({});
    for (int spin = 0; spin < 50; ++spin) QApplication::processEvents();
    const QImage cleared = grabAll();
    require(differingPixels(before, cleared) == 0, "clearing poses restores the clean frame");

    // --- remote edits must not rebuild the whole map --------------------
    // A full rebuild replaces the Scene (new shared_ptr, every view's GPU
    // cache invalidated, every entity helper re-resolved through the material
    // system); the incremental path updates it in place and keeps its
    // address. Connected editors froze when every collaborator edit took the
    // full path, so pin the cheap one down.
    document.setCollabPeerPoses({});
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();

    const hammer::vmf::Document docBefore = document.vmfDocument();
    hammer::vmf::Document docAfter = docBefore;
    for (hammer::vmf::Block& root : docAfter.roots()) {
        if (root.name != "world") continue;
        for (hammer::vmf::Block* solid : root.children("solid")) {
            solid->children("side")[0]->setValue("material", "TOOLS/TOOLSSKIP");
            break;
        }
    }
    const auto sceneBefore = document.scene();
    document.applyRemoteDelta(hammer::vmf::diffDocuments(docBefore, docAfter));
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();
    require(document.scene() == sceneBefore,
            "a remote solid edit updates the scene in place (no full rebuild)");

    // A worldspawn keyvalue is not a scene object, so that one legitimately
    // falls back to the full path — the fallback must still work.
    hammer::vmf::Document skyChanged = document.vmfDocument();
    skyChanged.firstRoot("world")->setValue("skyname", "sky_day02_01");
    document.applyRemoteDelta(
        hammer::vmf::diffDocuments(document.vmfDocument(), skyChanged));
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();
    require(document.scene() != sceneBefore,
            "a worldspawn change still falls back to a full rebuild");

    // A collaborator editing an object hidden locally must not resurrect it:
    // the incremental path REGENERATES changed objects from the document,
    // which knows nothing about local hiding.
    document.selectAll();
    document.quickHideSelected();
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();
    require(document.scene()->brushes.empty(), "quickhide removes the cube from the scene");
    hammer::vmf::Document hiddenEdit = document.vmfDocument();
    hiddenEdit.firstRoot("world")->children("solid")[0]->children("side")[1]->setValue(
        "material", "TOOLS/TOOLSCLIP");
    document.applyRemoteDelta(
        hammer::vmf::diffDocuments(document.vmfDocument(), hiddenEdit));
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();
    require(document.scene()->brushes.empty(),
            "a peer editing a locally hidden solid does not resurrect it");
    document.quickHideUnhideAll();
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();
    require(document.scene()->brushes.size() == 1, "unhide brings it back");

    // A remote DELETE cannot be applied in place (the id is gone from the
    // document), so it must take the full-rebuild fallback rather than leave
    // ghost geometry behind — a stale scene the desync hash could never catch,
    // since the documents would agree.
    hammer::vmf::Document deleted = document.vmfDocument();
    for (hammer::vmf::Block& root : deleted.roots()) {
        if (root.name != "world") continue;
        for (auto it = root.entries.begin(); it != root.entries.end(); ++it) {
            if (it->kind == hammer::vmf::Entry::Kind::ChildBlock && it->child &&
                it->child->name == "solid") {
                root.entries.erase(it);
                break;
            }
        }
    }
    document.applyRemoteDelta(hammer::vmf::diffDocuments(document.vmfDocument(), deleted));
    for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();
    require(document.scene()->brushes.empty(), "a remote delete leaves no ghost geometry");

    // --- Face tool behaviours --------------------------------------------
    // Reload a fresh cube: the sections above deleted it.
    document.adoptCollabDocument(*hammer::vmf::Document::parse(minimalMap()));
    for (int spin = 0; spin < 50; ++spin) QApplication::processEvents();
    require(document.scene()->brushes.size() == 1, "fresh cube for the face tool");
    const int cubeId = document.scene()->brushes.front().id;
    const int firstSide = document.scene()->brushes.front().faces.front().sideId;
    const int secondSide = document.scene()->brushes.front().faces[1].sideId;

    // 3. Switching to the face tool with a brush selected seeds every face of
    //    that brush into the face list (CToolMaterial::OnActivate).
    document.setTool(MapViewWidget::Tool::Selection);
    document.selectAll();
    document.setTool(MapViewWidget::Tool::TextureApplication);
    require(document.faceSelection().size() == 6,
            "entering the face tool with a brush selected selects all six faces");
    // ...and re-clicking the active tool must NOT re-seed a hand-picked list.
    document.handleFaceSelect(nullptr, cubeId, firstSide, false, false);
    require(document.faceSelection().size() == 1, "a plain click narrows to one face");
    document.setTool(MapViewWidget::Tool::TextureApplication);
    require(document.faceSelection().size() == 1,
            "re-clicking the active face tool leaves the picked list alone");
    // Nothing selected -> nothing seeded (no invented select-all).
    document.setTool(MapViewWidget::Tool::Selection);
    document.clearSelection();
    document.setTool(MapViewWidget::Tool::TextureApplication);
    require(document.faceSelection().empty(),
            "entering the face tool with nothing selected seeds no faces");

    // 2. Selecting a face lifts its material into the CURRENT texture
    //    (default click mode is Lift+Select).
    document.setCurrentMaterial(QStringLiteral("brick/brickwall001a"));
    QString lifted;
    QObject::connect(&document, &MapDocumentWidget::currentMaterialLifted,
                     [&](const QString& material) { lifted = material; });
    document.handleFaceSelect(nullptr, cubeId, firstSide, false, false);
    require(document.currentMaterial().compare(QStringLiteral("TOOLS/TOOLSNODRAW"),
                                               Qt::CaseInsensitive) == 0,
            "clicking a face makes its material the current texture");
    require(!lifted.isEmpty(), "the lift is announced for the Textures bar and sheet to mirror");

    // 1. Right-click applies the current texture to the face under the
    //    cursor regardless of what is selected.
    document.setCurrentMaterial(QStringLiteral("brick/brickwall001a"));
    document.clearFaceSelection();
    require(document.faceSelection().empty(), "face list cleared before the apply test");
    document.handleFaceApply(nullptr, cubeId, secondSide, false, false);
    {
        const auto texture = document.editorModel().faceTexture({cubeId, secondSide});
        require(texture && texture->material == "brick/brickwall001a",
                "right-click applies the current texture to the hovered, unselected face");
        const auto untouched = document.editorModel().faceTexture({cubeId, firstSide});
        require(untouched && untouched->material != "brick/brickwall001a",
                "only the hovered face changed");
    }
    document.setTool(MapViewWidget::Tool::Selection);

    // --- Arrow-key nudge / clone ------------------------------------------
    // Real key events into the Top view, so the modifier and autorepeat
    // rules in keyPressEvent are what is under test.
    document.adoptCollabDocument(*hammer::vmf::Document::parse(minimalMap()));
    for (int spin = 0; spin < 50; ++spin) QApplication::processEvents();
    document.setTool(MapViewWidget::Tool::Selection);
    document.selectAll();
    MapViewWidget* top = nullptr;
    for (MapViewWidget* view : document.findChildren<MapViewWidget*>()) {
        if (view->kind() == MapViewWidget::Kind::Top) top = view;
    }
    require(top != nullptr, "the Top view is reachable");
    const auto brushCount = [&] { return document.scene()->brushes.size(); };
    const auto selectedMinX = [&] {
        double minX = 1e9;
        for (const hammer::vmf::BrushGeometry& brush : document.scene()->brushes) {
            const bool selected = std::any_of(
                document.selection().begin(), document.selection().end(),
                [&](const hammer::vmf::ObjectRef& ref) { return ref.id == brush.id; });
            if (!selected) continue;
            for (const hammer::vmf::Vec3& v : brush.vertices) minX = std::min(minX, v.x);
        }
        return minX;
    };
    const double startX = selectedMinX();
    document.setGridSpacing(64);
    document.setGridSnapEnabled(true);
    const auto press = [&](Qt::KeyboardModifiers modifiers, bool autoRepeat = false) {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Right, modifiers, QString(), autoRepeat);
        QApplication::sendEvent(top, &event);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
    };

    // Plain arrow: one grid step, no new objects.
    press(Qt::NoModifier);
    require(brushCount() == 1, "a plain arrow does not duplicate");
    require(std::abs(selectedMinX() - (startX + 64.0)) < 1e-6,
            "a plain arrow moves the selection by the grid size");
    // Plain arrows keep autorepeat.
    press(Qt::NoModifier, /*autoRepeat=*/true);
    require(std::abs(selectedMinX() - (startX + 128.0)) < 1e-6,
            "held plain arrows keep nudging (autorepeat)");
    // Alt: a fine one-unit step.
    press(Qt::AltModifier);
    require(std::abs(selectedMinX() - (startX + 129.0)) < 1e-6, "Alt+arrow nudges one unit");
    // A different grid size is respected on the next press.
    document.setGridSpacing(16);
    press(Qt::NoModifier);
    require(std::abs(selectedMinX() - (startX + 145.0)) < 1e-6,
            "changing the grid size changes the nudge step");
    // Snap off: one unit regardless of the grid size.
    document.setGridSnapEnabled(false);
    press(Qt::NoModifier);
    require(std::abs(selectedMinX() - (startX + 146.0)) < 1e-6,
            "with grid snap off an arrow moves one unit");
    document.setGridSnapEnabled(true);

    // Shift: duplicate, and the COPY moves one grid step and stays selected.
    const double beforeClone = selectedMinX();
    press(Qt::ShiftModifier);
    require(brushCount() == 2, "Shift+arrow duplicates the selection");
    require(std::abs(selectedMinX() - (beforeClone + 16.0)) < 1e-6,
            "the copy is what moved, by the grid size, and is now selected");
    // Holding Shift and pressing again clones the newest copy: a row walks out.
    press(Qt::ShiftModifier);
    require(brushCount() == 3, "each Shift+arrow press makes another copy");
    require(std::abs(selectedMinX() - (beforeClone + 32.0)) < 1e-6,
            "the newest copy is the one selected and moved a grid step further");
    // Autorepeat must NOT spray copies.
    press(Qt::ShiftModifier, /*autoRepeat=*/true);
    require(brushCount() == 3, "held Shift+arrow (autorepeat) does not spawn extra copies");
    // Each clone is its own undo step.
    document.undo();
    require(brushCount() == 2, "undo removes exactly the last clone");

    // --- Face Edit sheet spinner arrows ------------------------------------
    // Increments and ranges are the original's (hammer.rc spin buddies,
    // OnInitDialog's UDM_SETRANGE, OnDeltaPosFloatSpin's 0.1f/%.2f), and each
    // click applies immediately (OnVScroll -> Apply(FACE_APPLY_MAPPING)).
    {
        FaceEditSheet sheet;
        int applied = 0;
        QObject::connect(&sheet, &FaceEditSheet::mappingEdited, [&] { ++applied; });
        FaceEditValues values;
        values.faceCount = 1;
        values.scaleX = 0.25;
        values.scaleY = 0.25;
        values.shiftX = 0.0;
        values.shiftY = 0.0;
        values.rotation = 0.0;
        values.lightmapScale = 16;
        sheet.setFaceValues(values);

        // The arrow buttons were created in field order (scaleX, scaleY,
        // shiftX, shiftY, rotation, lightmap), two per field, up before down.
        QList<QToolButton*> arrows;
        for (QToolButton* button : sheet.findChildren<QToolButton*>()) {
            if (button->arrowType() == Qt::UpArrow || button->arrowType() == Qt::DownArrow)
                arrows.append(button);
        }
        require(arrows.size() >= 12, "six mapping fields each have an up and a down arrow");
        // arrows[0..1] scaleX up/down, [2..3] scaleY, [4..5] shiftX, [6..7]
        // shiftY, [8..9] rotation, [10..11] lightmap.
        arrows[0]->click();
        require(std::abs(*sheet.currentEdit(false).scaleX - 0.35) < 1e-9,
                "scale X arrow steps by 0.1");
        arrows[1]->click();
        arrows[1]->click();
        require(std::abs(*sheet.currentEdit(false).scaleX - 0.15) < 1e-9,
                "scale X down arrow steps back by 0.1");
        arrows[4]->click();
        require(std::abs(*sheet.currentEdit(false).shiftX - 1.0) < 1e-9,
                "shift X arrow steps by 1");
        arrows[9]->click();
        require(std::abs(*sheet.currentEdit(false).rotation - (-1.0)) < 1e-9,
                "rotation down arrow steps by -1");
        arrows[11]->click();
        require(*sheet.currentEdit(false).lightmapScale == 15, "lightmap arrow steps by 1");
        // Lightmap scale never drops below 1 (UDM_SETRANGE ... MAKELONG(UD_MAXVAL, 1)).
        for (int i = 0; i < 30; ++i) arrows[11]->click();
        require(*sheet.currentEdit(false).lightmapScale == 1, "lightmap scale is clamped at 1");
        // Rotation is clamped to +/-359.
        for (int i = 0; i < 400; ++i) arrows[8]->click();
        require(std::abs(*sheet.currentEdit(false).rotation - 359.0) < 1e-9,
                "rotation is clamped at 359");
        // Every click applied.
        require(applied == 1 + 2 + 1 + 1 + 1 + 30 + 400,
                "each spinner click applies the mapping (OnVScroll)");
        if (!outputDir.isEmpty()) {
            sheet.show();
            for (int spin = 0; spin < 30; ++spin) QApplication::processEvents();
            sheet.grab().save(outputDir + QStringLiteral("/face_edit_sheet.png"));
        }
    }

    if (!outputDir.isEmpty()) {
        before.save(outputDir + QStringLiteral("/overlay_before.png"));
        withPeer.save(outputDir + QStringLiteral("/overlay_with_peer.png"));
        withBehind.save(outputDir + QStringLiteral("/overlay_behind.png"));
    }

    std::printf(failures == 0 ? "collab overlay harness passed\n"
                              : "collab overlay harness FAILED\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
