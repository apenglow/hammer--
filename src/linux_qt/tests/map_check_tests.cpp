// Map > Check for Problems (hammer/mapcheckdlg.cpp): every check ported into
// MapDocumentWidget::checkForProblems must fire on a map built to break it, and
// every fix must make its own problem go away without inventing new ones.
//
// The material checks are not exercised here: they need a mounted game
// filesystem, which a unit test has no business needing.
#include "MapDocumentWidget.hpp"

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

using Type = MapDocumentWidget::MapProblem::Type;

bool hasType(const std::vector<MapDocumentWidget::MapProblem>& problems, Type type)
{
    return std::any_of(problems.cbegin(), problems.cend(),
                       [&](const MapDocumentWidget::MapProblem& problem) {
                           return problem.type == type;
                       });
}

// One cube, written as the six axis-aligned sides Hammer emits. `sideIds` gives
// each side its id, so a caller can hand two sides the same one.
std::string cube(int solidId, const int (&sideIds)[6], const char* firstUAxis)
{
    static const char* const kPlanes[6] = {
        "(0 64 64) (64 64 64) (64 0 64)", "(0 0 0) (64 0 0) (64 64 0)",
        "(0 64 64) (0 0 64) (0 0 0)",     "(64 64 0) (64 0 0) (64 0 64)",
        "(64 64 64) (0 64 64) (0 64 0)",  "(64 0 0) (0 0 0) (0 0 64)",
    };
    std::string text = "\tsolid\n\t{\n\t\t\"id\" \"" + std::to_string(solidId) + "\"\n";
    for (int i = 0; i < 6; ++i) {
        text += "\t\tside\n\t\t{\n";
        text += "\t\t\t\"id\" \"" + std::to_string(sideIds[i]) + "\"\n";
        text += std::string("\t\t\t\"plane\" \"") + kPlanes[i] + "\"\n";
        text += "\t\t\t\"material\" \"TOOLS/TOOLSNODRAW\"\n";
        text += std::string("\t\t\t\"uaxis\" \"") +
                (i == 0 && firstUAxis ? firstUAxis : "[1 0 0 0] 0.25") + "\"\n";
        text += "\t\t\t\"vaxis\" \"[0 -1 0 0] 0.25\"\n";
        text += "\t\t\t\"rotation\" \"0\"\n\t\t\t\"lightmapscale\" \"16\"\n";
        text += "\t\t}\n";
    }
    text += "\t}\n";
    return text;
}

// Deliberately broken on every count the port checks for, and with no
// info_player_start anywhere.
std::string brokenMap()
{
    // Sides 20 and 30 share an id; the first side of solid 11 has a collapsed
    // U axis.
    const int firstSolidSides[6] = {20, 21, 22, 23, 24, 25};
    const int secondSolidSides[6] = {20, 31, 32, 33, 34, 35};
    std::string text =
        "versioninfo\n{\n\t\"editorversion\" \"400\"\n}\n"
        "world\n{\n\t\"id\" \"1\"\n\t\"classname\" \"worldspawn\"\n";
    text += cube(10, firstSolidSides, nullptr);
    text += cube(11, secondSolidSides, "[0 0 0 0] 0.25");
    text += "}\n";

    // Duplicate node ids.
    text += "entity\n{\n\t\"id\" \"40\"\n\t\"classname\" \"info_node\"\n"
            "\t\"nodeid\" \"7\"\n\t\"origin\" \"0 0 0\"\n}\n";
    text += "entity\n{\n\t\"id\" \"41\"\n\t\"classname\" \"info_node\"\n"
            "\t\"nodeid\" \"7\"\n\t\"origin\" \"64 0 0\"\n}\n";
    // An unused keyvalue, a target that resolves to nothing, and an output
    // fired at an entity that does not exist.
    text += "entity\n{\n\t\"id\" \"42\"\n\t\"classname\" \"trigger_once\"\n"
            "\t\"target\" \"nobody_here\"\n\t\"bogus_key\" \"1\"\n\t\"origin\" \"0 0 0\"\n"
            "\tconnections\n\t{\n\t\t\"OnTrigger\" \"missing_entity,Kill,,0,-1\"\n\t}\n";
    const int triggerSides[6] = {50, 51, 52, 53, 54, 55};
    text += cube(43, triggerSides, nullptr);
    text += "}\n";
    // A brush entity with no solids, and an overlay applied to no face.
    text += "entity\n{\n\t\"id\" \"44\"\n\t\"classname\" \"func_detail\"\n}\n";
    text += "entity\n{\n\t\"id\" \"45\"\n\t\"classname\" \"info_overlay\"\n"
            "\t\"sides\" \"\"\n\t\"origin\" \"0 0 0\"\n}\n";
    return text;
}

// Just enough game data for the class-driven checks: a brush class, a point
// class with a target_destination, and one with no extra keys.
constexpr const char* kFgd = R"FGD(
@PointClass = info_node : "AI node" [ nodeid(integer) : "Node ID" ]
@SolidClass = func_detail : "Detail brush" []
@SolidClass = trigger_once : "Trigger" [ target(target_destination) : "Target" ]
@PointClass = info_overlay : "Overlay" [ sides(string) : "Sides" ]
@PointClass = info_player_start : "Player start" []
)FGD";

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("broken.vmf"));
    {
        std::ofstream file(path.toStdString());
        file << brokenMap();
    }

    auto fgd = std::make_shared<hammer::fgd::Database>();
    require(fgd->loadText(kFgd), "test game data parses");

    MapDocumentWidget widget(fgd);
    QString error;
    require(widget.loadFromFile(path, &error), "broken map loads");

    const std::vector<MapDocumentWidget::MapProblem> problems = widget.checkForProblems(false);
    require(hasType(problems, Type::NoPlayerStart), "missing info_player_start is reported");
    require(hasType(problems, Type::DuplicateFaceIds), "duplicate face ids are reported");
    require(hasType(problems, Type::DuplicateNodeIds), "duplicate node ids are reported");
    require(hasType(problems, Type::InvalidTextureAxes), "collapsed texture axes are reported");
    require(hasType(problems, Type::UnusedKeyvalue), "a key the game data lacks is reported");
    require(hasType(problems, Type::EmptyEntity), "a brush entity with no solids is reported");
    require(hasType(problems, Type::MissingTarget), "an unresolved target is reported");
    require(hasType(problems, Type::BadConnection), "an output at a missing entity is reported");
    require(hasType(problems, Type::OverlayFaceList), "an overlay with no faces is reported");
    require(!hasType(problems, Type::SolidStructure), "the valid cubes are not called degenerate");

    // The unfixable one is exactly the map-wide one.
    for (const MapDocumentWidget::MapProblem& problem : problems) {
        if (problem.type == Type::NoPlayerStart) {
            require(!problem.canFix && problem.objects.empty(),
                    "no-player-start is unfixable and points at nothing");
        } else {
            require(problem.canFix && !problem.objects.empty(),
                    "every object problem is fixable and points at its object");
        }
    }

    std::vector<MapDocumentWidget::MapProblem> fixable;
    std::copy_if(problems.cbegin(), problems.cend(), std::back_inserter(fixable),
                 [](const MapDocumentWidget::MapProblem& problem) { return problem.canFix; });
    const std::size_t fixed = widget.fixProblems(fixable);
    require(fixed == fixable.size(), "every fixable problem reports itself fixed");
    require(widget.canUndo(), "the fixes land as one undo step");

    const std::vector<MapDocumentWidget::MapProblem> after = widget.checkForProblems(false);
    require(!hasType(after, Type::DuplicateFaceIds), "duplicate face ids are gone");
    require(!hasType(after, Type::DuplicateNodeIds), "duplicate node ids are gone");
    require(!hasType(after, Type::InvalidTextureAxes), "texture axes are repaired");
    require(!hasType(after, Type::UnusedKeyvalue), "the unused key is gone");
    require(!hasType(after, Type::EmptyEntity), "the empty brush entity is gone");
    require(!hasType(after, Type::MissingTarget), "the dangling target key is gone");
    require(!hasType(after, Type::BadConnection), "the bad connection is gone");
    require(!hasType(after, Type::OverlayFaceList), "the faceless overlay is gone");
    require(hasType(after, Type::NoPlayerStart), "the unfixable problem survives the fixes");
    require(!hasType(after, Type::SolidStructure),
            "repairing texture axes did not break any solid");

    widget.undo();
    const std::vector<MapDocumentWidget::MapProblem> undone = widget.checkForProblems(false);
    require(undone.size() == problems.size(), "undo restores every problem");

    std::printf(failures == 0 ? "\nall map check tests passed\n" : "\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
