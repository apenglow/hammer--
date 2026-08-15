// Document-level Grouping / VisGrouping behaviour, driven through
// MapDocumentWidget on the offscreen platform.
//
// The model layer has its own tests (vmf_groups_tests.cpp). What this covers is
// the integration the model cannot: that hidden objects actually leave the
// published Scene, that they leave the selection with it, that a pick expands
// to a whole group, and that Ignore Groups turns that expansion off.

#include "MapDocumentWidget.hpp"
#include "VmfGroups.hpp"

#include <QApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace {
int failures = 0;

void require(bool condition, const char* message)
{
    if (condition) {
        std::printf("ok - %s\n", message);
    } else {
        std::printf("FAIL - %s\n", message);
        ++failures;
    }
}

// A cube with the six axis-aligned sides Hammer emits, offset along x so the
// solids in a map do not sit on top of each other.
std::string cube(int solidId, int offset, const std::string& editorBlock)
{
    const auto number = [](int value) { return std::to_string(value); };
    const int x0 = offset;
    const int x1 = offset + 64;
    const std::string planes[6] = {
        "(" + number(x0) + " 64 64) (" + number(x1) + " 64 64) (" + number(x1) + " 0 64)",
        "(" + number(x0) + " 0 0) (" + number(x1) + " 0 0) (" + number(x1) + " 64 0)",
        "(" + number(x0) + " 64 64) (" + number(x0) + " 0 64) (" + number(x0) + " 0 0)",
        "(" + number(x1) + " 64 0) (" + number(x1) + " 0 0) (" + number(x1) + " 0 64)",
        "(" + number(x1) + " 64 64) (" + number(x0) + " 64 64) (" + number(x0) + " 64 0)",
        "(" + number(x1) + " 0 0) (" + number(x0) + " 0 0) (" + number(x0) + " 0 64)",
    };
    std::string text = "\tsolid\n\t{\n\t\t\"id\" \"" + number(solidId) + "\"\n";
    for (int i = 0; i < 6; ++i) {
        text += "\t\tside\n\t\t{\n";
        text += "\t\t\t\"id\" \"" + number(solidId * 10 + i) + "\"\n";
        text += "\t\t\t\"plane\" \"" + planes[i] + "\"\n";
        text += "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n";
        text += "\t\t\t\"uaxis\" \"[1 0 0 0] 0.25\"\n";
        text += "\t\t\t\"vaxis\" \"[0 -1 0 0] 0.25\"\n";
        text += "\t\t\t\"lightmapscale\" \"16\"\n";
        text += "\t\t}\n";
    }
    text += editorBlock;
    text += "\t}\n";
    return text;
}

// Solids 10 and 11 are grouped (group 50); solid 12 is loose. Solids 10 and 12
// are in visgroup 1.
std::string groupedMap()
{
    std::string text =
        "versioninfo\n{\n\t\"editorversion\" \"400\"\n\t\"mapversion\" \"1\"\n"
        "\t\"formatversion\" \"100\"\n}\n"
        "visgroups\n{\n\tvisgroup\n\t{\n\t\t\"name\" \"Furniture\"\n"
        "\t\t\"visgroupid\" \"1\"\n\t\t\"color\" \"255 0 0\"\n\t}\n}\n"
        "world\n{\n\t\"id\" \"1\"\n\t\"mapversion\" \"1\"\n\t\"classname\" \"worldspawn\"\n";
    text += cube(10, 0,
                 "\t\teditor\n\t\t{\n\t\t\t\"color\" \"0 180 0\"\n\t\t\t\"groupid\" \"50\"\n"
                 "\t\t\t\"visgroupid\" \"1\"\n\t\t\t\"visgroupshown\" \"1\"\n\t\t}\n");
    text += cube(11, 128,
                 "\t\teditor\n\t\t{\n\t\t\t\"color\" \"0 180 0\"\n\t\t\t\"groupid\" \"50\"\n"
                 "\t\t\t\"visgroupshown\" \"1\"\n\t\t}\n");
    text += cube(12, 256,
                 "\t\teditor\n\t\t{\n\t\t\t\"color\" \"0 180 0\"\n"
                 "\t\t\t\"visgroupid\" \"1\"\n\t\t\t\"visgroupshown\" \"1\"\n\t\t}\n");
    text +=
        "\tgroup\n\t{\n\t\t\"id\" \"50\"\n\t\teditor\n\t\t{\n\t\t\t\"color\" \"0 180 0\"\n"
        "\t\t\t\"visgroupshown\" \"1\"\n\t\t}\n\t}\n";
    text += "}\n";
    text +=
        "entity\n{\n\t\"id\" \"20\"\n\t\"classname\" \"light\"\n\t\"origin\" \"0 0 128\"\n"
        "\teditor\n\t{\n\t\t\"color\" \"220 30 220\"\n\t\t\"visgroupshown\" \"1\"\n\t}\n}\n";
    return text;
}

const hammer::vmf::ObjectRef Solid10{hammer::vmf::ObjectType::Solid, 10};
const hammer::vmf::ObjectRef Solid11{hammer::vmf::ObjectType::Solid, 11};
const hammer::vmf::ObjectRef Solid12{hammer::vmf::ObjectType::Solid, 12};

bool sceneHasSolid(const MapDocumentWidget& widget, int id)
{
    const std::shared_ptr<const hammer::vmf::Scene> scene = widget.scene();
    if (!scene) return false;
    return std::any_of(scene->brushes.cbegin(), scene->brushes.cend(),
                       [id](const hammer::vmf::BrushGeometry& brush) { return brush.id == id; });
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("grouped.vmf"));
    {
        std::ofstream file(path.toStdString());
        file << groupedMap();
    }

    auto fgd = std::make_shared<hammer::fgd::Database>();
    MapDocumentWidget widget(fgd);
    QString error;
    require(widget.loadFromFile(path, &error), "the grouped map loads");
    require(sceneHasSolid(widget, 10) && sceneHasSolid(widget, 12),
            "every solid starts in the scene");

    // --- Group expansion ----------------------------------------------------
    const std::vector<hammer::vmf::ObjectRef> expanded =
        widget.expandSelectionToGroups({Solid10});
    require(expanded.size() == 2, "a pick on a grouped solid expands to the whole group");
    require(std::find(expanded.begin(), expanded.end(), Solid11) != expanded.end(),
            "the group's other member comes with it");
    require(widget.expandSelectionToGroups({Solid12}).size() == 1,
            "a loose solid expands to itself");

    // Ignore Groups turns expansion off, so one member can be worked on alone.
    widget.setIgnoreGroups(true);
    require(widget.expandSelectionToGroups({Solid10}).size() == 1,
            "Ignore Groups stops a pick expanding to the group");
    widget.setIgnoreGroups(false);

    // --- Ungroup / group ----------------------------------------------------
    widget.selectObjects({Solid10, Solid11});
    require(widget.selectedGroups().size() == 1, "a fully selected group is recognised");
    require(widget.ungroupSelection(), "the selected group is ungrouped");
    require(widget.expandSelectionToGroups({Solid10}).size() == 1,
            "an ungrouped solid no longer drags its former partner in");

    widget.selectObjects({Solid10, Solid12});
    require(widget.groupSelection(), "a new group is made from the selection");
    const std::vector<hammer::vmf::ObjectRef> regrouped =
        widget.expandSelectionToGroups({Solid12});
    require(regrouped.size() == 2, "the new group expands like the old one");

    // --- VisGroup visibility ------------------------------------------------
    widget.clearSelection();
    widget.selectObjects({Solid10, Solid12});
    const int visGroupId = widget.createVisGroupFromSelection(QStringLiteral("Hidden Two"), true,
                                                              /*removeFromOtherVisGroups=*/true);
    require(visGroupId != 0, "a visgroup is created from the selection");
    require(!sceneHasSolid(widget, 10) && !sceneHasSolid(widget, 12),
            "hiding a visgroup removes its members from the published scene");
    require(sceneHasSolid(widget, 11), "an object outside the visgroup stays in the scene");
    require(widget.selectionCount() == 0,
            "hidden objects leave the selection (CSelection::RemoveInvisibles)");
    require(widget.visGroupState(visGroupId) == hammer::vmf::VisGroupState::Hidden,
            "the visgroup reads back as hidden");

    // Select All must not resurrect them.
    widget.selectAll();
    const std::vector<hammer::vmf::ObjectRef>& all = widget.selection();
    require(std::find(all.begin(), all.end(), Solid10) == all.end(),
            "Select All skips visgroup-hidden objects");

    // Show All is an override: it shows everything without clearing the flags.
    widget.setShowAllVisGroups(true);
    require(sceneHasSolid(widget, 10), "Show All brings hidden objects back");
    widget.setShowAllVisGroups(false);
    require(!sceneHasSolid(widget, 10), "turning Show All off re-hides exactly what was hidden");

    // Unchecking the visgroup shows its members again.
    widget.setVisGroupVisible(visGroupId, true);
    require(sceneHasSolid(widget, 10) && sceneHasSolid(widget, 12),
            "showing the visgroup restores its members");

    // --- QuickHide still works alongside ------------------------------------
    widget.clearSelection();
    widget.selectObjects({Solid11});
    widget.quickHideSelected();
    require(!sceneHasSolid(widget, 11), "QuickHide removes the object from the scene");
    require(widget.quickHiddenCount() == 1, "the quickhide set holds one object");
    const std::size_t moved = widget.quickHideConvertToVisGroup(QStringLiteral("_FromQuickHide(1)"));
    require(moved == 1, "the quickhidden object converts to a visgroup");
    require(widget.quickHiddenCount() == 0, "quickhide hands the object over completely");
    require(!sceneHasSolid(widget, 11), "the new visgroup keeps it hidden");
    widget.quickHideUnhideAll();
    require(!sceneHasSolid(widget, 11),
            "Unhide QuickHide Objects no longer restores a converted object");

    // --- Deleting a group's members purges the orphan blocks ----------------
    widget.setVisGroupVisible(visGroupId, true);
    widget.quickHideUnhideAll();
    {
        // Solids 10 and 12 are the group made above and the only members of
        // the visgroup made above; deleting them must leave neither behind.
        widget.clearSelection();
        widget.selectObjects({Solid10, Solid12});
        widget.deleteSelection();
        const hammer::vmf::MapObjectIndex index =
            hammer::vmf::indexMapObjects(widget.vmfDocument());
        const bool anyGroup =
            std::any_of(index.objects.cbegin(), index.objects.cend(),
                        [](const hammer::vmf::MapObjectEntry& entry) {
                            return entry.key.kind == hammer::vmf::MapObjectKind::Group;
                        });
        require(!anyGroup, "deleting a group's last members purges the empty group block");
        // The visgroup solid 11 was converted into still has its member, so
        // only the one that just lost both of its objects is purged.
        require(index.findVisGroup(visGroupId) == nullptr,
                "deleting a visgroup's last members purges the empty visgroup");
        require(widget.canUndo(), "the delete left an undo entry");
        widget.undo();
        const hammer::vmf::MapObjectIndex restored =
            hammer::vmf::indexMapObjects(widget.vmfDocument());
        require(restored.findVisGroup(visGroupId) != nullptr,
                "one undo restores the purged visgroup along with its objects");
    }

    if (failures == 0) std::printf("grouping integration tests passed\n");
    return failures == 0 ? 0 : 1;
}
