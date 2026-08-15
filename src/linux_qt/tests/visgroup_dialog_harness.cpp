// Manual diagnostic: opens the VisGroup dialogs against a real MainWindow and
// writes a PNG of each, so their layout can be compared against the Material
// Browser / Entity Report house style. Not a registered test - it needs a full
// MainWindow, which pulls in the whole app.
//
//   hammer-visgroup-dialog-harness -platform offscreen <output-dir>

#include "MainWindow.hpp"
#include "MapDocumentWidget.hpp"
#include "VmfGroups.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QTemporaryDir>
#include <QGroupBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {

// Grabs whatever modal dialog is up, writes it, and closes it so exec()
// returns and the harness can move on to the next one.
void captureModal(const QString& file, int& failures)
{
    QWidget* modal = QApplication::activeModalWidget();
    if (!modal) {
        std::printf("FAIL - no dialog appeared for %s\n", qPrintable(file));
        ++failures;
        return;
    }
    const bool written = modal->grab().save(file);
    std::printf("%s - %s (%dx%d)\n", written ? "ok" : "FAIL", qPrintable(file),
                modal->width(), modal->height());
    if (!written) ++failures;
    if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
    else modal->close();
}

// Two world cubes, enough for a selection to be moved into a VisGroup.
const char* minimalMap()
{
    return "versioninfo\n{\n\t\"editorversion\" \"400\"\n\t\"mapversion\" \"1\"\n"
           "\t\"formatversion\" \"100\"\n}\n"
           "world\n{\n\t\"id\" \"1\"\n\t\"mapversion\" \"1\"\n"
           "\t\"classname\" \"worldspawn\"\n"
           "\tsolid\n\t{\n\t\t\"id\" \"10\"\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"100\"\n"
           "\t\t\t\"plane\" \"(0 64 64) (64 64 64) (64 0 64)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 -1 0 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"101\"\n"
           "\t\t\t\"plane\" \"(0 0 0) (64 0 0) (64 64 0)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 -1 0 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"102\"\n"
           "\t\t\t\"plane\" \"(0 64 64) (0 0 64) (0 0 0)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[0 1 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"103\"\n"
           "\t\t\t\"plane\" \"(64 64 0) (64 0 0) (64 0 64)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[0 1 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"104\"\n"
           "\t\t\t\"plane\" \"(64 64 64) (0 64 64) (0 64 0)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"105\"\n"
           "\t\t\t\"plane\" \"(64 0 0) (0 0 0) (0 0 64)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n\t}\n}\n"
           // A brush entity, so a double-click on one can be exercised too.
           "entity\n{\n\t\"id\" \"20\"\n\t\"classname\" \"func_detail\"\n"
           "\tsolid\n\t{\n\t\t\"id\" \"30\"\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"300\"\n"
           "\t\t\t\"plane\" \"(128 64 64) (192 64 64) (192 0 64)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 -1 0 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"301\"\n"
           "\t\t\t\"plane\" \"(128 0 0) (192 0 0) (192 64 0)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 -1 0 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"302\"\n"
           "\t\t\t\"plane\" \"(128 64 64) (128 0 64) (128 0 0)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[0 1 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"303\"\n"
           "\t\t\t\"plane\" \"(192 64 0) (192 0 0) (192 0 64)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[0 1 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"304\"\n"
           "\t\t\t\"plane\" \"(192 64 64) (128 64 64) (128 64 0)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n"
           "\t\tside\n\t\t{\n\t\t\t\"id\" \"305\"\n"
           "\t\t\t\"plane\" \"(192 0 0) (128 0 0) (128 0 64)\"\n"
           "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n"
           "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n"
           "\t\t\t\"vaxis\" \"[0 0 -1 0] 0.25\"\n\t\t}\n\t}\n"
           "\teditor\n\t{\n\t\t\"color\" \"0 180 0\"\n"
           "\t\t\"visgroupshown\" \"1\"\n\t}\n}\n"
           // A point entity: double-clicking one opened properties before any
           // of this, and must still.
           "entity\n{\n\t\"id\" \"40\"\n\t\"classname\" \"info_player_start\"\n"
           "\t\"origin\" \"320 32 32\"\n"
           "\teditor\n\t{\n\t\t\"color\" \"220 30 220\"\n"
           "\t\t\"visgroupshown\" \"1\"\n\t}\n}\n";
}

void require(bool condition, const char* message, int& failures)
{
    std::printf("%s - %s\n", condition ? "ok" : "FAIL", message);
    if (!condition) ++failures;
}

QAction* findAction(MainWindow& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text() == text) return action;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString outputDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(outputDir);

    int failures = 0;
    // A map is needed for the New VisGroup dialog to have a selection to move.
    // Without one on the command line, a minimal two-brush map is generated so
    // this runs unattended as a regression test.
    QTemporaryDir scratch;
    QString map = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString{};
    if (map.isEmpty()) {
        map = scratch.filePath(QStringLiteral("toggle.vmf"));
        std::FILE* file = std::fopen(map.toLocal8Bit().constData(), "w");
        if (file) {
            std::fputs(minimalMap(), file);
            std::fclose(file);
        }
    }
    MainWindow window(map.isEmpty() ? QStringList{} : QStringList{map});
    window.resize(1400, 900);
    window.show();
    // The sidebar and menus are built in the constructor, but the MDI child and
    // its document arrive on the event loop.
    for (int spin = 0; spin < 200; ++spin) QApplication::processEvents();
    for (MapDocumentWidget* document : window.findChildren<MapDocumentWidget*>()) {
        document->selectAll();
        std::printf("ok - %llu object(s) selected in the loaded map\n",
                    static_cast<unsigned long long>(document->selectionCount()));
        break;
    }

    struct Case { const char* action; const char* file; };
    const Case cases[] = {
        {"Move Selection To V&isgroup", "newvisgroup.png"},
    };

    for (const Case& item : cases) {
        QAction* action = findAction(window, QString::fromLatin1(item.action));
        if (!action) {
            std::printf("FAIL - no action named %s\n", item.action);
            ++failures;
            continue;
        }
        const QString file = outputDir + QLatin1Char('/') + QLatin1String(item.file);
        QTimer::singleShot(0, [&file, &failures] { captureModal(file, failures); });
        action->trigger();
        QApplication::processEvents();
    }

    // The Object Groups editor hangs off the Filter Control "Edit" button
    // rather than a menu action.
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() != QStringLiteral("&Edit")) continue;
        const QString file = outputDir + QStringLiteral("/objectgroups.png");
        QTimer::singleShot(0, [&file, &failures] { captureModal(file, failures); });
        button->click();
        QApplication::processEvents();
        break;
    }

    // Toggling VisGroup check boxes. This is the path that reenters the tree
    // rebuild from inside QTreeWidgetItem::setData. Run it hard, with nested
    // visgroups and repeated toggles, under MALLOC_PERTURB_ so a use-after-free
    // faults instead of quietly reading recycled memory.
    for (MapDocumentWidget* document : window.findChildren<MapDocumentWidget*>()) {
        document->selectAll();
        if (document->selectionCount() == 0) break;
        // Several visgroups, some sharing objects, so toggling one changes
        // another's derived shown/hidden/partial state too.
        const int alpha = document->createVisGroupFromSelection(QStringLiteral("Alpha"),
                                                                false, false);
        document->selectAll();
        document->createVisGroupFromSelection(QStringLiteral("Beta"), false, false);
        for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();

        QTreeWidget* tree = nullptr;
        for (QTreeWidget* candidate : window.findChildren<QTreeWidget*>()) {
            if (candidate->topLevelItemCount() > 0 &&
                candidate->topLevelItem(0)->data(0, Qt::UserRole).toInt() != 0) {
                tree = candidate;
                break;
            }
        }
        if (!tree) {
            std::printf("FAIL - the Filter Control tree has no VisGroups to toggle\n");
            ++failures;
            break;
        }
        std::printf("toggling %d visgroup(s), 12 passes...\n", tree->topLevelItemCount());
        std::fflush(stdout);
        for (int pass = 0; pass < 12; ++pass) {
            for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                QTreeWidgetItem* item = tree->topLevelItem(row);
                if (!item) continue;
                item->setCheckState(0, (pass + row) % 2 ? Qt::Checked : Qt::Unchecked);
                for (int spin = 0; spin < 6; ++spin) QApplication::processEvents();
                // The tree is rebuilt on every toggle, so the row count and the
                // item pointers are re-read from scratch each iteration.
                if (row >= tree->topLevelItemCount()) break;
            }
        }
        std::printf("ok - repeated VisGroup toggling did not crash\n");

        // The deferred rebuild must not have cost the toggle its effect.
        const auto brushCount = [document] {
            const auto scene = document->scene();
            return scene ? scene->brushes.size() : std::size_t{0};
        };
        // The toggle loop above left the visgroups in an arbitrary state, so
        // start from a known-shown one before measuring.
        document->setVisGroupVisible(alpha, true);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        const std::size_t shown = brushCount();
        document->setVisGroupVisible(alpha, false);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        const std::size_t hidden = brushCount();
        require(hidden < shown, "hiding a VisGroup still removes its brushes from the scene",
                failures);
        document->setVisGroupVisible(alpha, true);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(brushCount() == shown, "showing it again restores them", failures);
        std::fflush(stdout);
        break;
    }

    // --- Double-click routing ----------------------------------------------
    // A plain world brush opens Object Properties on the VisGroup page; a brush
    // entity opens it normally, on its class page.
    for (MapDocumentWidget* document : window.findChildren<MapDocumentWidget*>()) {
        MapViewWidget* topView = nullptr;
        for (MapViewWidget* view : window.findChildren<MapViewWidget*>()) {
            if (view->kind() == MapViewWidget::Kind::Top && view->width() > 32) {
                topView = view;
                break;
            }
        }
        if (!topView) {
            std::printf("FAIL - no 2D Top view to double-click in\n");
            ++failures;
            break;
        }

        // solid 10 is the world brush, entity 20 the brush entity.
        const struct { hammer::vmf::ObjectRef object; const char* expectedTab; const char* what; }
            targets[] = {
                {{hammer::vmf::ObjectType::Solid, 10}, "VisGroup", "a world brush"},
                {{hammer::vmf::ObjectType::Entity, 20}, "Class Info", "a brush entity"},
                {{hammer::vmf::ObjectType::Entity, 40}, "Class Info", "a point entity"},
            };

        for (const auto& target : targets) {
            document->selectObjects({target.object});
            document->centerViewsOnSelection();
            for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();

            QString openedTab;
            QTimer::singleShot(0, [&openedTab] {
                QWidget* modal = QApplication::activeModalWidget();
                if (!modal) return;
                if (auto* tabs = modal->findChild<QTabWidget*>())
                    openedTab = tabs->tabText(tabs->currentIndex());
                if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
            });

            const QPointF centre(topView->width() / 2.0, topView->height() / 2.0);
            const QPoint global = topView->mapToGlobal(centre.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, centre, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(topView, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, centre, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(topView, &release);
            QMouseEvent doubleClick(QEvent::MouseButtonDblClick, centre, global, Qt::LeftButton,
                                    Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(topView, &doubleClick);
            for (int spin = 0; spin < 20; ++spin) QApplication::processEvents();

            const bool ok = openedTab == QLatin1String(target.expectedTab);
            std::printf("%s - double-clicking %s opens the %s page (got \"%s\")\n",
                        ok ? "ok" : "FAIL", target.what, target.expectedTab,
                        qPrintable(openedTab.isEmpty() ? QStringLiteral("<no dialog>") : openedTab));
            if (!ok) ++failures;
            std::fflush(stdout);
        }
        break;
    }

    // --- Auto VisGroups -----------------------------------------------------
    // The Auto tree is a second QTreeWidget rebuilt from its own itemChanged,
    // so it is the same reentrancy class as the User tree crash. Stress it the
    // same way, under the same poisoned allocator.
    for (MapDocumentWidget* document : window.findChildren<MapDocumentWidget*>()) {
        const auto categories = document->autoVisGroups();
        require(!categories.empty(), "the map populates some auto-visgroup categories", failures);

        QTreeWidget* autoTree = nullptr;
        for (QTreeWidget* candidate : window.findChildren<QTreeWidget*>()) {
            if (candidate == nullptr || candidate->topLevelItemCount() == 0) continue;
            if (candidate->topLevelItem(0)->data(0, Qt::UserRole).toInt() < 0) {
                autoTree = candidate;
                break;
            }
        }
        if (!autoTree) {
            std::printf("FAIL - the Auto tab has no categories to toggle\n");
            ++failures;
            break;
        }
        // Auto tab items must not be renameable.
        require(!(autoTree->topLevelItem(0)->flags() & Qt::ItemIsEditable),
                "auto visgroups are not editable in place", failures);

        std::printf("toggling %d auto category/ies, 8 passes...\n",
                    autoTree->topLevelItemCount());
        std::fflush(stdout);
        for (int pass = 0; pass < 8; ++pass) {
            for (int row = 0; row < autoTree->topLevelItemCount(); ++row) {
                QTreeWidgetItem* item = autoTree->topLevelItem(row);
                if (!item) continue;
                item->setCheckState(0, (pass + row) % 2 ? Qt::Checked : Qt::Unchecked);
                for (int spin = 0; spin < 6; ++spin) QApplication::processEvents();
                if (row >= autoTree->topLevelItemCount()) break;
            }
        }
        std::printf("ok - toggling auto visgroups did not crash\n");

        // And it must actually hide: World Geometry covers the world brush.
        const auto brushCount = [document] {
            const auto scene = document->scene();
            return scene ? scene->brushes.size() : std::size_t{0};
        };
        document->setAutoVisGroupVisible(hammer::vmf::AutoVisGroup::WorldGeometry, true);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        const std::size_t shown = brushCount();
        document->setAutoVisGroupVisible(hammer::vmf::AutoVisGroup::WorldGeometry, false);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(brushCount() < shown, "hiding an auto visgroup removes its brushes", failures);
        document->setAutoVisGroupVisible(hammer::vmf::AutoVisGroup::WorldGeometry, true);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(brushCount() == shown, "showing it again restores them", failures);
        std::fflush(stdout);
        break;
    }

    // --- Show nodraw faces button -------------------------------------------
    // The button is a shortcut for the View > Tool Textures entry for
    // tools/toolsnodraw, so the two must stay in step in both directions.
    for (MapDocumentWidget* document : window.findChildren<MapDocumentWidget*>()) {
        QAction* nodraw = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->toolTip().startsWith(QStringLiteral("Show nodraw faces"))) {
                nodraw = action;
                break;
            }
        }
        if (!nodraw) {
            std::printf("FAIL - no nodraw toolbar action found\n");
            ++failures;
            break;
        }
        const QString nodrawMaterial = QStringLiteral("tools/toolsnodraw");
        require(nodraw->isChecked(), "the nodraw button starts checked (shown)", failures);
        require(document->toolTextureVisible(nodrawMaterial),
                "and the document agrees nodraw is visible", failures);

        // Button -> document.
        nodraw->trigger();
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(!document->toolTextureVisible(nodrawMaterial),
                "unchecking the button hides nodraw through the tool-texture filter", failures);
        nodraw->trigger();
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(document->toolTextureVisible(nodrawMaterial),
                "rechecking it shows nodraw again", failures);

        // Document (i.e. the Tool Textures menu) -> button.
        document->setToolTextureVisible(nodrawMaterial, false);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(!nodraw->isChecked(),
                "hiding nodraw from the Tool Textures menu unchecks the button", failures);
        document->setToolTextureVisible(nodrawMaterial, true);
        for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
        require(nodraw->isChecked(), "and showing it rechecks the button", failures);
        std::fflush(stdout);
        break;
    }

    // The Filter Control section, so its tree and buttons can be checked
    // against the Textures and New Objects sections around it in the sidebar.
    for (QGroupBox* box : window.findChildren<QGroupBox*>()) {
        if (box->title() != QStringLiteral("Filter Control")) continue;
        const QString file = outputDir + QStringLiteral("/filtercontrol.png");
        if (box->grab().save(file)) std::printf("ok - %s\n", qPrintable(file));
        else { std::printf("FAIL - %s\n", qPrintable(file)); ++failures; }
        // Again with the Auto tab forward, so both trees can be eyeballed.
        for (QTabWidget* tabs : box->findChildren<QTabWidget*>()) {
            tabs->setCurrentIndex(1);
            for (int spin = 0; spin < 10; ++spin) QApplication::processEvents();
            box->grab().save(outputDir + QStringLiteral("/filtercontrol-auto.png"));
            break;
        }
        break;
    }

    std::printf(failures == 0 ? "visgroup dialog harness done\n" : "visgroup dialog harness FAILED\n");
    std::fflush(stdout);
    // The GL/Vulkan viewports do not tear down cleanly on the offscreen
    // platform, and this harness has already written everything it exists for.
    std::_Exit(failures == 0 ? 0 : 1);
}
