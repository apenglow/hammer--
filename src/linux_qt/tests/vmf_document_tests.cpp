#include "Camera3D.hpp"
#include "VmfDocument.hpp"
#include "VmfScene.hpp"
#include "VmfProjectedSurfaces.hpp"
#include "VmfRope.hpp"
#include "FgdDatabase.hpp"
#include "VmfEditor.hpp"
#include "VmfSolidCarve.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using hammer::vmf::Block;
using hammer::vmf::Document;
using hammer::vmf::ParseError;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

const char* SampleVmf =
    "// a comment that must survive an untouched save\r\n"
    "versioninfo\r\n"
    "{\r\n"
    "\t\"editorversion\" \"400\"\r\n"
    "\t\"mapversion\" \"7\"\r\n"
    "\t\"formatversion\" \"100\"\r\n"
    "}\r\n"
    "world\r\n"
    "{\r\n"
    "\t\"id\" \"1\"\r\n"
    "\t\"classname\" \"worldspawn\"\r\n"
    "\t\"skyname\" \"sky_day01_01\"\r\n"
    "\tsolid\r\n"
    "\t{\r\n"
    "\t\t\"id\" \"2\"\r\n"
    "\t\tside\r\n"
    "\t\t{\r\n"
    "\t\t\t\"id\" \"3\"\r\n"
    "\t\t\t\"plane\" \"(0 0 0) (0 128 0) (128 128 0)\"\r\n"
    "\t\t\t\"material\" \"BRICK/BRICKWALL001A\"\r\n"
    "\t\t\tdispinfo\r\n"
    "\t\t\t{\r\n"
    "\t\t\t\t\"power\" \"2\"\r\n"
    "\t\t\t}\r\n"
    "\t\t}\r\n"
    "\t}\r\n"
    "}\r\n"
    "entity\r\n"
    "{\r\n"
    "\t\"id\" \"4\"\r\n"
    "\t\"classname\" \"logic_relay\"\r\n"
    "\tconnections\r\n"
    "\t{\r\n"
    "\t\t\"OnTrigger\" \"door,Open,,0,-1\"\r\n"
    "\t\t\"OnTrigger\" \"light,TurnOn,,0,-1\"\r\n"
    "\t}\r\n"
    "}\r\n"
    "custom_extension\r\n"
    "{\r\n"
    "\t\"plugin_data\" \"kept\\verbatim\"\r\n"
    "}\r\n";
}

int main()
{
    ParseError error;

    const char* GameInfoWithWildcard =
        "GameInfo\n"
        "{\n"
        "  FileSystem\n"
        "  {\n"
        "    SearchPaths\n"
        "    {\n"
        "      // The /* suffix is a wildcard path, not a block comment.\n"
        "      game+mod+custom_mod+vgui |gameinfo_path|custom/*\n"
        "      game |appid_440|tf\n"
        "    }\n"
        "  }\n"
        "}\n";
    auto gameInfo = Document::parse(GameInfoWithWildcard, &error);
    require(gameInfo.has_value(), "gameinfo SearchPaths wildcard parses");
    const Block* gameInfoRoot = gameInfo->firstRoot("GameInfo");
    require(gameInfoRoot != nullptr, "gameinfo root found");
    const auto fileSystems = gameInfoRoot->children("FileSystem");
    require(fileSystems.size() == 1, "gameinfo filesystem block found");
    const auto searchPaths = fileSystems.front()->children("SearchPaths");
    require(searchPaths.size() == 1, "gameinfo searchpaths block found");
    const std::string* wildcardPath = searchPaths.front()->value("game+mod+custom_mod+vgui");
    require(wildcardPath && *wildcardPath == "|gameinfo_path|custom/*",
            "unquoted wildcard search path is preserved");

    auto parsed = Document::parse(SampleVmf, &error);
    require(parsed.has_value(), "representative VMF parses");

    const auto stats = parsed->statistics();
    require(stats.topLevelBlocks == 4, "top-level blocks counted");
    require(stats.worlds == 1, "world block counted");
    require(stats.entities == 1, "entity block counted");
    require(stats.solids == 1, "solid block counted");
    require(stats.sides == 1, "side block counted");
    require(stats.displacements == 1, "displacement block counted");
    require(stats.mapVersion == 7, "map version read");
    require(stats.formatVersion == 100, "format version read");

    require(parsed->serialize(true) == SampleVmf,
            "an untouched loaded VMF is written byte-for-byte unchanged");

    Block* world = parsed->firstRoot("WORLD");
    require(world != nullptr, "block lookup is case insensitive");
    world->setValue("skyname", "sky_l4d_rural02_ldr");
    parsed->markDirty();
    require(hammer::vmf::buildScene(*parsed).skyName == "sky_l4d_rural02_ldr",
            "worldspawn skyname is exposed to the 3D scene");

    auto mapPropertiesDocument = Document::parse(SampleVmf, &error);
    require(mapPropertiesDocument.has_value(), "map-properties VMF parses");
    hammer::vmf::EditorModel mapPropertiesEditor(std::move(*mapPropertiesDocument));
    auto worldProperties = mapPropertiesEditor.worldProperties();
    require(!worldProperties.empty(), "worldspawn properties are exposed");
    for (auto& property : worldProperties) {
        if (property.key == "skyname") property.value = "sky_custom_preview";
    }
    worldProperties.push_back({"detailmaterial", "detail/detailsprites"});
    require(mapPropertiesEditor.replaceWorldProperties(worldProperties),
            "worldspawn properties can be replaced");
    const auto editedMapScene = hammer::vmf::buildScene(mapPropertiesEditor.document());
    require(editedMapScene.skyName == "sky_custom_preview",
            "changed map skyname reaches the scene");
    const Block* editedWorld = mapPropertiesEditor.document().firstRoot("world");
    require(editedWorld && editedWorld->children("solid").size() == 1,
            "changing map properties preserves world geometry");
    require(mapPropertiesEditor.undo(), "map-property edit can be undone");
    require(hammer::vmf::buildScene(mapPropertiesEditor.document()).skyName == "sky_day01_01",
            "undo restores the previous skyname");
    require(mapPropertiesEditor.redo(), "map-property edit can be redone");
    require(hammer::vmf::buildScene(mapPropertiesEditor.document()).skyName == "sky_custom_preview",
            "redo restores the changed skyname");

    // Edit > Undo/Redo Active off: the history is dropped and edits made while
    // it is off leave nothing behind, including after it is switched back on.
    mapPropertiesEditor.setUndoEnabled(false);
    require(!mapPropertiesEditor.canUndo() && !mapPropertiesEditor.canRedo(),
            "turning undo off clears the history");
    for (auto& property : worldProperties) {
        if (property.key == "skyname") property.value = "sky_undo_disabled";
    }
    require(mapPropertiesEditor.replaceWorldProperties(worldProperties),
            "edits still apply while undo is off");
    require(hammer::vmf::buildScene(mapPropertiesEditor.document()).skyName == "sky_undo_disabled",
            "an edit made with undo off reaches the scene");
    require(!mapPropertiesEditor.canUndo(), "an edit made with undo off is not undoable");
    require(!mapPropertiesEditor.undo(), "undo does nothing while it is off");
    mapPropertiesEditor.setUndoEnabled(true);
    require(!mapPropertiesEditor.canUndo(),
            "turning undo back on does not resurrect the discarded history");
    require(hammer::vmf::buildScene(mapPropertiesEditor.document()).skyName == "sky_undo_disabled",
            "re-enabling undo leaves the document alone");

    const std::string canonical = parsed->serialize(true);
    require(canonical.find("custom_extension") != std::string::npos, "unknown chunks survive canonical save");
    require(canonical.find("kept\\verbatim") != std::string::npos, "backslashes survive canonical save");
    require(canonical.find("\"OnTrigger\" \"door,Open,,0,-1\"") != std::string::npos,
            "first duplicate output survives");
    require(canonical.find("\"OnTrigger\" \"light,TurnOn,,0,-1\"") != std::string::npos,
            "second duplicate output survives");

    auto reparsed = Document::parse(canonical, &error);
    require(reparsed.has_value(), "serialized VMF parses again");
    const Block* reparsedWorld = reparsed->firstRoot("world");
    require(reparsedWorld && reparsedWorld->value("skyname") &&
            *reparsedWorld->value("skyname") == "sky_l4d_rural02_ldr", "changed key round trips");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "hammer-vmf-document-test.vmf";
    std::string saveError;
    require(parsed->save(path, &saveError), "VMF saves atomically to disk");
    require(!parsed->dirty(), "successful save marks the document clean");

    std::string ioError;
    auto loaded = Document::load(path, &error, &ioError);
    require(loaded.has_value(), "saved VMF loads from disk");
    require(loaded->statistics().entities == 1, "loaded VMF retains entities");
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    auto defaultMap = Document::createDefault();
    require(defaultMap.firstRoot("versioninfo") != nullptr, "new map has versioninfo");
    require(defaultMap.firstRoot("world") != nullptr, "new map has worldspawn");
    require(defaultMap.firstRoot("cameras") != nullptr, "new map has cameras");
    require(Document::parse(defaultMap.serialize(false), &error).has_value(), "new map serialization is valid VMF");

    auto geometryMap = Document::createDefault();
    Block* geometryWorld = geometryMap.firstRoot("world");
    require(geometryWorld != nullptr, "geometry test world exists");
    Block& cube = geometryWorld->appendChild("solid");
    cube.setValue("id", "100");
    const char* cubePlanes[] = {
        "(-64 -64 64) (-64 64 64) (64 64 64)",
        "(64 64 -64) (-64 64 -64) (-64 -64 -64)",
        "(64 -64 -64) (64 -64 64) (64 64 64)",
        "(-64 64 -64) (-64 64 64) (-64 -64 64)",
        "(64 64 -64) (64 64 64) (-64 64 64)",
        "(-64 -64 -64) (-64 -64 64) (64 -64 64)"
    };
    int sideId = 101;
    for (const char* plane : cubePlanes) {
        Block& side = cube.appendChild("side");
        side.setValue("id", std::to_string(sideId++));
        side.setValue("plane", plane);
        side.setValue("material", "BRICK/BRICKWALL001A");
        side.setValue("uaxis", "[1 0 0 0] 0.25");
        side.setValue("vaxis", "[0 -1 0 0] 0.25");
    }
    auto cubeSides = cube.children("side");
    require(!cubeSides.empty(), "cube side list exists for displacement test");
    Block& displacement = cubeSides.front()->appendChild("dispinfo");
    displacement.setValue("power", "2");
    displacement.setValue("startposition", "[64 64 64]");
    displacement.setValue("elevation", "0");
    Block& displacementNormals = displacement.appendChild("normals");
    Block& displacementDistances = displacement.appendChild("distances");
    Block& displacementOffsets = displacement.appendChild("offsets");
    Block& displacementAlphas = displacement.appendChild("alphas");
    const char* normalRow = "0 0 1  0 0 1  0 0 1  0 0 1  0 0 1";
    const char* offsetRows[] = {
        "0 0 0  0 0 0  0 0 0  0 0 0  0 0 0",
        "0 0 0  0 0 0  0 0 0  0 0 0  0 0 0",
        "0 0 0  0 0 0  16 0 0  0 0 0  0 0 0",
        "0 0 0  0 0 0  0 0 0  0 0 0  0 0 0",
        "0 0 0  0 0 0  0 0 0  0 0 0  0 0 0"
    };
    const char* distanceRows[] = {
        "0 0 0 0 0", "0 8 16 8 0", "0 16 32 16 0",
        "0 8 16 8 0", "0 0 0 0 0"
    };
    for (int row = 0; row < 5; ++row) {
        const std::string key = "row" + std::to_string(row);
        displacementNormals.setValue(key, normalRow);
        displacementOffsets.setValue(key, offsetRows[row]);
        displacementDistances.setValue(key, distanceRows[row]);
        displacementAlphas.setValue(key, "0 64 128 192 255");
    }
    Block& pointEntity = geometryMap.appendRoot("entity");
    pointEntity.setValue("id", "200");
    pointEntity.setValue("classname", "info_player_start");
    pointEntity.setValue("origin", "128 256 32");

    const auto scene = hammer::vmf::buildScene(geometryMap);
    require(scene.brushes.size() == 1, "solid planes produce one rendered brush");
    require(scene.brushes.front().vertices.size() == 8, "cube reconstructs eight vertices");
    require(scene.brushes.front().edges.size() == 12, "cube reconstructs twelve edges");
    require(scene.brushes.front().faces.size() == 6, "cube reconstructs six textured faces");
    require(scene.brushes.front().faces.front().material == "BRICK/BRICKWALL001A",
            "face material is preserved for the 3D material renderer");
    const auto displacementFace = std::find_if(
        scene.brushes.front().faces.begin(), scene.brushes.front().faces.end(),
        [](const hammer::vmf::FaceGeometry& face) { return face.displacement; });
    require(displacementFace != scene.brushes.front().faces.end(),
            "dispinfo creates a rendered displacement surface");
    // CMapSolid::HasDisp -- true when ANY side carries a dispinfo chunk, which
    // is what arms the 3D draw mask for the whole solid.
    require(scene.brushes.front().hasDisplacement,
            "a solid with one dispinfo side reports HasDisp for the whole solid");
    {
        int masked = 0;
        int drawn = 0;
        for (const hammer::vmf::FaceGeometry& face : scene.brushes.front().faces) {
            if (hammer::vmf::isFaceMaskedByDisplacementSolid(scene.brushes.front(), face, true)) {
                ++masked;
                require(!face.displacement, "the mask never hides a displaced side");
            } else {
                ++drawn;
            }
        }
        require(masked == 5 && drawn == 1,
                "CMapSolid::Render3D draws only the displaced side of a displacement solid");
        // With the toggle off (CMapDoc::OnToggleDispSolidMask) every side draws.
        for (const hammer::vmf::FaceGeometry& face : scene.brushes.front().faces) {
            require(!hammer::vmf::isFaceMaskedByDisplacementSolid(
                        scene.brushes.front(), face, false),
                    "disabling the draw mask restores every side of the solid");
        }
    }

    require(displacementFace->displacementPower == 2 &&
            displacementFace->displacementVertices.size() == 25,
            "power-2 displacement creates a five by five vertex grid");
    require(displacementFace->displacementIndices.size() == 96,
            "power-2 displacement creates thirty-two triangles");
    const std::vector<std::size_t> sourceFirstCells{
        0, 5, 6, 0, 6, 1,      // even ndx: BuildTriBLtoTR
        1, 6, 2, 2, 6, 7};     // odd ndx: BuildTriTLtoBR
    require(std::equal(sourceFirstCells.begin(), sourceFirstCells.end(),
                       displacementFace->displacementIndices.begin()),
            "displacement indices match Source CCoreDispInfo checkerboard topology");
    require(displacementFace->displacementVertices[12].normal.z > 0.0,
            "Source-style displacement normal is oriented to the brush face");

    // Hammer conventions, verified against the SDK rather than assumed:
    //  * CMapFace::CalcPlane -> GetNormalFromPoints( p0, p1, p2 ) yields the
    //    OUTWARD normal for the plane triples Hammer writes.
    //  * ::CheckFace (hammer/ssolid.cpp) only accepts face points wound
    //    CLOCKWISE about that outward normal, and CMapDisp::InitDispSurfaceData
    //    hands those points to the displacement surface in that order.
    // If either is inverted, displacements still "look like" terrain but
    // elevation, vertex normals, tangent handedness and decal offsets all flip.
    {
        const auto& cubeBrush = scene.brushes.front();
        hammer::vmf::Vec3 brushCenter{};
        for (const hammer::vmf::Vec3& vertex : cubeBrush.vertices) {
            brushCenter.x += vertex.x;
            brushCenter.y += vertex.y;
            brushCenter.z += vertex.z;
        }
        const double vertexTotal = static_cast<double>(cubeBrush.vertices.size());
        brushCenter = {brushCenter.x / vertexTotal, brushCenter.y / vertexTotal,
                       brushCenter.z / vertexTotal};
        for (const hammer::vmf::FaceGeometry& face : cubeBrush.faces) {
            hammer::vmf::Vec3 faceCentre{};
            for (const std::size_t index : face.vertices) {
                faceCentre.x += cubeBrush.vertices[index].x;
                faceCentre.y += cubeBrush.vertices[index].y;
                faceCentre.z += cubeBrush.vertices[index].z;
            }
            const double faceTotal = static_cast<double>(face.vertices.size());
            faceCentre = {faceCentre.x / faceTotal, faceCentre.y / faceTotal,
                          faceCentre.z / faceTotal};
            const double outward = face.normal.x * (faceCentre.x - brushCenter.x) +
                                   face.normal.y * (faceCentre.y - brushCenter.y) +
                                   face.normal.z * (faceCentre.z - brushCenter.z);
            require(outward > 0.0,
                    "VMF plane normals match GetNormalFromPoints and face outward");

            const hammer::vmf::Vec3& first = cubeBrush.vertices[face.vertices[0]];
            const hammer::vmf::Vec3& second = cubeBrush.vertices[face.vertices[1]];
            const hammer::vmf::Vec3& third = cubeBrush.vertices[face.vertices[2]];
            const hammer::vmf::Vec3 edgeA{second.x - first.x, second.y - first.y,
                                          second.z - first.z};
            const hammer::vmf::Vec3 edgeB{third.x - first.x, third.y - first.y,
                                          third.z - first.z};
            const hammer::vmf::Vec3 geometric{edgeA.y * edgeB.z - edgeA.z * edgeB.y,
                                              edgeA.z * edgeB.x - edgeA.x * edgeB.z,
                                              edgeA.x * edgeB.y - edgeA.y * edgeB.x};
            require(geometric.x * face.normal.x + geometric.y * face.normal.y +
                        geometric.z * face.normal.z < 0.0,
                    "face points wind clockwise about the outward normal (CheckFace)");
        }
    }

    // Every displacement vertex normal must agree with the triangle fan that is
    // actually rendered, without any dot() < 0 correction against the base
    // plane -- that correction used to hide mismatched diagonals and would
    // wrongly invert steep or overhanging displacements.
    for (std::size_t triangle = 0;
         triangle + 2 < displacementFace->displacementIndices.size(); triangle += 3) {
        const auto& vertexA = displacementFace->displacementVertices[
            displacementFace->displacementIndices[triangle]];
        const auto& vertexB = displacementFace->displacementVertices[
            displacementFace->displacementIndices[triangle + 1]];
        const auto& vertexC = displacementFace->displacementVertices[
            displacementFace->displacementIndices[triangle + 2]];
        const hammer::vmf::Vec3 edgeA{vertexC.position.x - vertexA.position.x,
                                      vertexC.position.y - vertexA.position.y,
                                      vertexC.position.z - vertexA.position.z};
        const hammer::vmf::Vec3 edgeB{vertexB.position.x - vertexA.position.x,
                                      vertexB.position.y - vertexA.position.y,
                                      vertexB.position.z - vertexA.position.z};
        const hammer::vmf::Vec3 geometric{edgeA.y * edgeB.z - edgeA.z * edgeB.y,
                                          edgeA.z * edgeB.x - edgeA.x * edgeB.z,
                                          edgeA.x * edgeB.y - edgeA.y * edgeB.x};
        require(geometric.z > 0.0,
                "displacement triangles wind consistently with the outward normal");
        require(vertexA.normal.z > 0.0 && vertexB.normal.z > 0.0 && vertexC.normal.z > 0.0,
                "displacement vertex normals follow the rendered tesselation");
    }
    const auto highestDisplacement = std::max_element(
        displacementFace->displacementVertices.begin(),
        displacementFace->displacementVertices.end(),
        [](const auto& a, const auto& b) { return a.position.z < b.position.z; });
    require(highestDisplacement != displacementFace->displacementVertices.end() &&
            highestDisplacement->position.z > 95.9,
            "displacement normals and distances deform the base face");
    require(displacementFace->displacementVertices[0].blendAlpha < 0.01 &&
            displacementFace->displacementVertices[4].blendAlpha > 0.99,
            "painted alpha 255 blends fully to $basetexture2 (alpha/255, no inversion)");
    const auto& centerDisp = displacementFace->displacementVertices[12];
    require(std::abs(centerDisp.textureU) < 1e-6 && std::abs(centerDisp.textureV) < 1e-6,
            "displacement UVs are interpolated from the undeformed base-face corners");
    require(centerDisp.position.x > 15.9,
            "displacement test actually moves the center sideways without reprojecting its UV");
    const double tangentSLength = std::sqrt(centerDisp.tangentS.x * centerDisp.tangentS.x +
                                            centerDisp.tangentS.y * centerDisp.tangentS.y +
                                            centerDisp.tangentS.z * centerDisp.tangentS.z);
    const double tangentTLength = std::sqrt(centerDisp.tangentT.x * centerDisp.tangentT.x +
                                            centerDisp.tangentT.y * centerDisp.tangentT.y +
                                            centerDisp.tangentT.z * centerDisp.tangentT.z);
    require(std::abs(tangentSLength - 1.0) < 1e-5 &&
            std::abs(tangentTLength - 1.0) < 1e-5 &&
            std::abs(centerDisp.tangentS.x) > 0.5 && centerDisp.tangentT.y < -0.5,
            "displacement tangent frame follows Source GenerateDispSurfTangentSpaces");
    require(scene.entities.size() == 1, "point entity origin produces a viewport marker");
    require(scene.hasBounds && scene.maximum.y == 256.0, "scene bounds include entity origins");

    auto animationMap = Document::createDefault();
    Block& animatedProp = animationMap.appendRoot("entity");
    animatedProp.setValue("id", "201");
    animatedProp.setValue("classname", "prop_dynamic");
    animatedProp.setValue("origin", "0 0 0");
    animatedProp.setValue("model", "models/props/test.mdl");
    animatedProp.setValue("DefaultAnim", "idle_loop");
    animatedProp.setValue("playbackrate", "1.5");
    animatedProp.setValue("cycle", "0.25");
    const auto animationScene = hammer::vmf::buildScene(animationMap);
    require(animationScene.entities.size() == 1, "animated prop enters the scene");
    require(animationScene.entities.front().animationSequence == "idle_loop",
            "DefaultAnim is preserved for studio sequence lookup");
    require(animationScene.entities.front().animationSequenceIndex == -1,
            "named sequences remain names until the MDL is available");
    require(std::abs(animationScene.entities.front().animationPlaybackRate - 1.5) < 0.0001,
            "prop playback rate is parsed");
    require(std::abs(animationScene.entities.front().animationCycle - 0.25) < 0.0001,
            "fixed prop animation cycle is parsed");
    require(animationScene.entities.front().animateModel, "authored prop sequence enables animation");

    auto idleDynamicMap = Document::createDefault();
    Block& idleDynamic = idleDynamicMap.appendRoot("entity");
    idleDynamic.setValue("id", "202");
    idleDynamic.setValue("classname", "prop_dynamic");
    idleDynamic.setValue("origin", "0 0 0");
    idleDynamic.setValue("model", "models/props/test.mdl");
    const auto idleDynamicScene = hammer::vmf::buildScene(idleDynamicMap);
    require(idleDynamicScene.entities.size() == 1 && !idleDynamicScene.entities.front().animateModel,
            "prop_dynamic without DefaultAnim is not forced to sequence zero");

    auto emptyAnimationMap = Document::createDefault();
    Block& emptyAnimation = emptyAnimationMap.appendRoot("entity");
    emptyAnimation.setValue("id", "204");
    emptyAnimation.setValue("classname", "prop_dynamic");
    emptyAnimation.setValue("origin", "0 0 0");
    emptyAnimation.setValue("model", "models/props/test.mdl");
    emptyAnimation.setValue("DefaultAnim", "");
    const auto emptyAnimationScene = hammer::vmf::buildScene(emptyAnimationMap);
    require(emptyAnimationScene.entities.size() == 1 &&
            !emptyAnimationScene.entities.front().animateModel,
            "an empty DefaultAnim keeps a dynamic prop in its reference pose");

    auto cyclerMap = Document::createDefault();
    Block& cycler = cyclerMap.appendRoot("entity");
    cycler.setValue("id", "203");
    cycler.setValue("classname", "cycler");
    cycler.setValue("origin", "0 0 0");
    cycler.setValue("model", "models/props/test.mdl");
    const auto cyclerScene = hammer::vmf::buildScene(cyclerMap);
    require(cyclerScene.entities.size() == 1 && cyclerScene.entities.front().animateModel,
            "cycler previews sequence zero when no sequence key is authored");

    auto postProcessMap = Document::createDefault();
    Block& tonemapController = postProcessMap.appendRoot("entity");
    tonemapController.setValue("id", "301");
    tonemapController.setValue("classname", "env_tonemap_controller");
    tonemapController.setValue("targetname", "tonemap_global");
    Block& tonemapLogic = postProcessMap.appendRoot("entity");
    tonemapLogic.setValue("id", "302");
    tonemapLogic.setValue("classname", "logic_auto");
    Block& tonemapConnections = tonemapLogic.appendChild("connections");
    tonemapConnections.setValue("OnMapSpawn", "tonemap_global,SetAutoExposureMin,0.25,0,-1");
    Block& correctionEntity = postProcessMap.appendRoot("entity");
    correctionEntity.setValue("id", "303");
    correctionEntity.setValue("classname", "color_correction");
    correctionEntity.setValue("origin", "64 32 16");
    correctionEntity.setValue("filename", "materials/correction/test.raw");
    correctionEntity.setValue("minfalloff", "128");
    correctionEntity.setValue("maxfalloff", "512");
    correctionEntity.setValue("maxweight", "0.75");
    const auto postProcessScene = hammer::vmf::buildScene(postProcessMap);
    require(postProcessScene.toneMap.customAutoExposure &&
            std::abs(postProcessScene.toneMap.autoExposureMin - 0.25) < 0.0001,
            "logic_auto OnMapSpawn applies env_tonemap_controller startup exposure");
    require(postProcessScene.colorCorrections.size() == 1 &&
            postProcessScene.colorCorrections.front().filename == "materials/correction/test.raw" &&
            std::abs(postProcessScene.colorCorrections.front().weight - 0.75) < 0.0001 &&
            std::abs(postProcessScene.colorCorrections.front().minFalloff - 128.0) < 0.0001 &&
            std::abs(postProcessScene.colorCorrections.front().maxFalloff - 512.0) < 0.0001,
            "Source color_correction point settings enter the render scene");

    hammer::vmf::EditorModel editor(geometryMap);
    editor.select({hammer::vmf::ObjectType::Entity, 200});
    require(editor.selection().size() == 1, "point entity can be selected");
    require(editor.translateSelection({16, -32, 8}), "selected entity can be translated");
    const auto movedScene = hammer::vmf::buildScene(editor.document());
    require(movedScene.entities.size() == 1, "moved entity remains in scene");
    require(movedScene.entities.front().origin.x == 144.0 &&
            movedScene.entities.front().origin.y == 224.0 &&
            movedScene.entities.front().origin.z == 40.0,
            "entity origin translation updates VMF data");
    require(editor.canUndo() && editor.undoLabel() == "Move Objects", "move creates a named undo step");
    require(editor.undo(), "entity movement can be undone");
    require(hammer::vmf::buildScene(editor.document()).entities.front().origin.x == 128.0,
            "undo restores entity origin");
    require(editor.redo(), "entity movement can be redone");
    require(hammer::vmf::buildScene(editor.document()).entities.front().origin.x == 144.0,
            "redo restores entity movement");

    editor.select({hammer::vmf::ObjectType::Solid, 100});
    require(editor.beginTransaction("Drag Objects"), "drag transaction starts");
    require(editor.translateSelectionInTransaction({32, 0, 0}), "drag transaction changes brush planes");
    require(editor.translateSelectionInTransaction({0, 16, 0}), "drag transaction can accumulate movement");
    require(editor.commitTransaction(), "drag transaction commits one undo step");
    const auto draggedScene = hammer::vmf::buildScene(editor.document());
    require(draggedScene.brushes.front().vertices.front().x >= -32.001,
            "translated brush geometry is rebuilt from changed side planes");
    require(editor.undoLabel() == "Drag Objects", "drag has one named undo entry");

    editor.select({hammer::vmf::ObjectType::Entity, 200});
    auto properties = editor.selectedProperties();
    require(!properties.empty(), "selected entity properties are exposed");
    for (auto& property : properties) {
        if (property.key == "classname") property.value = "info_target";
    }
    properties.push_back({"targetname", "linux_port_test"});
    require(editor.replaceSelectedProperties(properties), "entity properties can be replaced");
    const std::string editedBytes = editor.document().serialize(false);
    require(editedBytes.find("\"classname\" \"info_target\"") != std::string::npos,
            "edited classname is serialized");
    require(editedBytes.find("\"targetname\" \"linux_port_test\"") != std::string::npos,
            "new entity key is serialized");

    require(editor.deleteSelection(), "selected entity can be deleted");
    require(hammer::vmf::buildScene(editor.document()).entities.empty(), "deleted entity leaves the scene");
    require(editor.undo(), "entity deletion can be undone");
    require(!hammer::vmf::buildScene(editor.document()).entities.empty(), "undo restores deleted entity");



    auto brushEntityMap = Document::createDefault();
    Block& brushEntity = brushEntityMap.appendRoot("entity");
    brushEntity.setValue("id", "300");
    brushEntity.setValue("classname", "func_detail");
    Block& entitySolid = brushEntity.appendChild("solid");
    entitySolid.setValue("id", "301");
    sideId = 302;
    for (const char* plane : cubePlanes) {
        Block& side = entitySolid.appendChild("side");
        side.setValue("id", std::to_string(sideId++));
        side.setValue("plane", plane);
    }
    const auto brushEntityScene = hammer::vmf::buildScene(brushEntityMap);
    require(brushEntityScene.brushes.size() == 1 && brushEntityScene.brushes.front().ownerEntityId == 300,
            "brush geometry retains its owning entity for Groups/Objects selection modes");
    hammer::vmf::EditorModel brushEntityEditor(std::move(brushEntityMap));
    brushEntityEditor.setSelection({{hammer::vmf::ObjectType::Entity, 300},
                                    {hammer::vmf::ObjectType::Solid, 301}});
    require(brushEntityEditor.translateSelection({16, 0, 0}), "brush entity can be translated as one object");
    const auto movedBrushEntity = hammer::vmf::buildScene(brushEntityEditor.document());
    double entityMinX = 1e9;
    for (const auto& vertex : movedBrushEntity.brushes.front().vertices) entityMinX = std::min(entityMinX, vertex.x);
    require(entityMinX > -48.001 && entityMinX < -47.999,
            "selecting an entity and its child solid does not translate the child twice");


    auto creationMap = Document::createDefault();
    hammer::vmf::EditorModel creationEditor(std::move(creationMap));
    const auto createdBlock = creationEditor.createBlock({-32, -48, -16}, {64, 80, 96}, "brick/brickwall001a");
    require(createdBlock.has_value() && createdBlock->type == hammer::vmf::ObjectType::Solid,
            "block tool creates and selects a world solid");
    auto creationScene = hammer::vmf::buildScene(creationEditor.document());
    require(creationScene.brushes.size() == 1 && creationScene.brushes.front().vertices.size() == 8,
            "created block reconstructs as a valid convex brush");
    require(creationEditor.document().serialize(false).find("brick/brickwall001a") != std::string::npos,
            "created block stores the current material");

    std::vector<hammer::vmf::Property> entityDefaults = {
        {"targetname", "spawn_a"}, {"StartDisabled", "1"}
    };
    const auto createdEntity = creationEditor.createPointEntity("info_target", {128, 64, 32}, entityDefaults);
    require(createdEntity.has_value() && createdEntity->type == hammer::vmf::ObjectType::Entity,
            "entity tool creates and selects a point entity");
    creationScene = hammer::vmf::buildScene(creationEditor.document());
    require(creationScene.entities.size() == 1 && creationScene.entities.front().origin.x == 128.0,
            "created entity appears at its VMF origin");
    require(creationEditor.document().serialize(false).find("\"StartDisabled\" \"1\"") != std::string::npos,
            "FGD defaults are stored on created entities");

    creationEditor.setSelection({*createdBlock, *createdEntity});
    const hammer::vmf::ClipboardData copied = creationEditor.copySelection();
    require(copied.objects.size() == 2 && copied.bounds.valid, "copy captures selected VMF objects and bounds");
    require(creationEditor.paste(copied, {16, 16, 0}), "paste clones copied objects");
    creationScene = hammer::vmf::buildScene(creationEditor.document());
    require(creationScene.brushes.size() == 2 && creationScene.entities.size() == 2,
            "paste adds independent brush and entity clones");
    require(creationEditor.selection().size() == 2, "pasted objects become the active selection");
    require(creationEditor.selection()[0].id != createdBlock->id && creationEditor.selection()[1].id != createdEntity->id,
            "pasted objects receive fresh VMF IDs");

    const hammer::vmf::Bounds pastedBounds = creationEditor.selectionBounds();
    require(pastedBounds.valid, "selection bounds are available for transform handles");
    require(creationEditor.scaleSelection({2.0, 0.5, 1.0}, pastedBounds.minimum),
            "resize handles scale VMF objects about an anchor");
    const hammer::vmf::Bounds scaledBounds = creationEditor.selectionBounds();
    require((scaledBounds.maximum.x - scaledBounds.minimum.x) >
            (pastedBounds.maximum.x - pastedBounds.minimum.x) * 1.9,
            "selection width changes after scaling");
    require(creationEditor.rotateSelection(3.14159265358979323846 / 2.0,
                                           hammer::vmf::RotationAxis::Z,
                                           scaledBounds.center()),
            "rotation handles rotate selection around a pivot");
    require(creationEditor.undoLabel() == "Rotate Objects", "rotation creates a named undo step");
    require(creationEditor.undo(), "rotation can be undone");
    require(creationEditor.redo(), "rotation can be redone");
    const std::string beforeCanceledTransform = creationEditor.document().serialize(false);
    require(creationEditor.beginTransaction("Canceled Resize"), "transform cancellation test begins a transaction");
    require(creationEditor.scaleSelectionInTransaction({1.25, 1.0, 1.0}, creationEditor.selectionBounds().center()),
            "canceled transform changes the working document");
    creationEditor.cancelTransaction();
    require(creationEditor.document().serialize(false) == beforeCanceledTransform,
            "canceling a transform restores VMF data without an undo step");

    // Rotating a point entity about its own origin moves no vertices, so the
    // rotation must land in the "angles" keyvalue or the edit is silently lost.
    const auto spinEntity = creationEditor.createPointEntity("info_target", {0, 0, 0}, {});
    require(spinEntity.has_value(), "angles rotation test creates a point entity");
    creationEditor.setSelection({*spinEntity});
    require(creationEditor.rotateSelection(3.14159265358979323846 / 2.0,
                                           hammer::vmf::RotationAxis::Z,
                                           {0, 0, 0}),
            "rotating a point entity about its origin still counts as a change");
    require(creationEditor.document().serialize(false).find("\"angles\" \"0 90 0\"") != std::string::npos,
            "a 90-degree top-view rotation becomes yaw 90 in the angles key");

    hammer::vmf::Scene toolTextureScene;
    hammer::vmf::BrushGeometry toolTextureBrush;
    toolTextureBrush.faces.push_back({.material = "TOOLS\\ToolTrigger.vmt"});
    toolTextureBrush.faces.push_back({.material = "materials/tools/toolsnodraw"});
    toolTextureBrush.faces.push_back({.material = "tools/toolsnodraw"});
    toolTextureBrush.faces.push_back({.material = "brick/brickwall001a"});
    toolTextureScene.brushes.push_back(std::move(toolTextureBrush));
    require(hammer::vmf::normalizeMaterialPath("Materials\\TOOLS\\ToolTrigger.VMT") ==
                "tools/tooltrigger",
            "material paths normalize case, slashes, prefix, and extension");
    require(hammer::vmf::isToolMaterialPath("TOOLS/toolsnodraw"),
            "tools material paths are detected case-insensitively");
    const auto toolPaths = hammer::vmf::toolMaterialPaths(toolTextureScene);
    require(toolPaths.size() == 2 && toolPaths[0] == "tools/toolsnodraw" &&
                toolPaths[1] == "tools/tooltrigger",
            "scene tool texture list is unique, normalized, and sorted");

    // Tool-texture visibility semantics (see VmfScene.hpp): faces hide
    // individually; a solid only counts as hidden when EVERY face is hidden.
    const std::unordered_set<std::string> hideNodraw{"tools/toolsnodraw"};
    require(hammer::vmf::isMaterialHiddenByToolTextures("Materials\\TOOLS\\ToolsNoDraw.vmt",
                                                        hideNodraw),
            "hidden tool materials are matched after normalization");
    require(!hammer::vmf::isMaterialHiddenByToolTextures("brick/brickwall001a", hideNodraw),
            "non-tool materials are never hidden by the tool texture menu");
    require(!hammer::vmf::isMaterialHiddenByToolTextures("tools/tooltrigger", hideNodraw),
            "tool materials that are not in the hidden set stay visible");
    require(!hammer::vmf::isMaterialHiddenByToolTextures("tools/toolsnodraw", {}),
            "an empty hidden set hides nothing");

    const hammer::vmf::BrushGeometry& mixedBrush = toolTextureScene.brushes.front();
    require(hammer::vmf::isFaceHiddenByToolTextures(mixedBrush.faces[1], hideNodraw) &&
                hammer::vmf::isFaceHiddenByToolTextures(mixedBrush.faces[2], hideNodraw),
            "nodraw faces are hidden individually");
    require(!hammer::vmf::isFaceHiddenByToolTextures(mixedBrush.faces[0], hideNodraw) &&
                !hammer::vmf::isFaceHiddenByToolTextures(mixedBrush.faces[3], hideNodraw),
            "trigger and world faces stay visible when only nodraw is hidden");
    require(!hammer::vmf::isBrushHiddenByToolTextures(mixedBrush, hideNodraw),
            "a solid with only some hidden faces is not hidden as an object");

    hammer::vmf::BrushGeometry allNodraw;
    allNodraw.faces.push_back({.material = "TOOLS\\ToolsNoDraw"});
    allNodraw.faces.push_back({.material = "materials/tools/toolsnodraw.vmt"});
    require(hammer::vmf::isBrushHiddenByToolTextures(allNodraw, hideNodraw),
            "a solid whose every face is a hidden tool material is hidden");
    require(!hammer::vmf::isBrushHiddenByToolTextures(allNodraw, {}),
            "no solid is hidden when nothing is hidden");
    require(!hammer::vmf::isBrushHiddenByToolTextures({}, hideNodraw),
            "a solid with no faces is never treated as hidden");

    // Billboard sizing is the single source of truth shared by the hardware
    // renderer, the 3D ray pick, and the 2D helper drawing and picking.
    hammer::vmf::EntityMarker spriteEntity;
    spriteEntity.sizeMinimum = {-4.0, -4.0, -4.0};
    spriteEntity.sizeMaximum = {4.0, 4.0, 4.0};
    const auto tinySprite = hammer::vmf::billboardSpriteSize(spriteEntity, 64, 32);
    require(std::abs(tinySprite.height - 24.0) < 1e-9 &&
                std::abs(tinySprite.width - 48.0) < 1e-9,
            "a sub-16-unit helper draws at 24 units high times the image aspect");
    spriteEntity.sizeMinimum = {-8.0, -8.0, -40.0};
    spriteEntity.sizeMaximum = {8.0, 8.0, 40.0};
    const auto tallSprite = hammer::vmf::billboardSpriteSize(spriteEntity, 64, 32);
    require(std::abs(tallSprite.height - 80.0) < 1e-9 &&
                std::abs(tallSprite.width - 160.0) < 1e-9,
            "a tall helper keeps its bounds height and widens by the image aspect");
    const auto squareImage = hammer::vmf::billboardSpriteSize(spriteEntity, 0, 0);
    require(squareImage.width > 0.0 && squareImage.height > 0.0,
            "a degenerate image size still produces a usable billboard size");

    constexpr std::string_view SampleFgd = R"FGD(
@BaseClass color(24 48 72) = Targetname [
    targetname(target_source) : "Name" : "" : "Entity name"
]
@BaseClass = EnableDisable [
    StartDisabled(choices) : "Start Disabled" : 0 =
    [
        0 : "No"
        1 : "Yes"
    ]
]
@PointClass base(Targetname, EnableDisable) color(64 128 255) size(-8 -12 -16, 8 12 16) studio("models/editor/info_target.mdl") iconsprite("editor/info_target.vmt") = info_target : "Target" [
    angles(angle) : "Pitch Yaw Roll" : "0 0 0"
    spawnflags(flags) =
    [
        1 : "Transmit to client" : 1
        2 : "Always transmit" : 0
    ]
]
@SolidClass base(Targetname) = func_detail : "Detail geometry" [
    solidity(choices) : "Solidity" : 0 = [ 0 : "Normal" 1 : "Never solid" ]
]
)FGD";
    hammer::fgd::Database fgd;
    hammer::fgd::ParseError fgdError;
    require(fgd.loadText(SampleFgd, &fgdError), "FGD point, solid, and base classes parse");
    require(fgd.pointClasses().size() == 1 && fgd.solidClasses().size() == 1,
            "FGD class kinds are indexed");
    const auto effective = fgd.effectiveProperties("info_target");
    require(effective.size() == 4, "FGD base-class properties are inherited");
    const auto startDisabled = std::find_if(effective.begin(), effective.end(), [](const auto& property) {
        return property.key == "StartDisabled";
    });
    require(startDisabled != effective.end() && startDisabled->type == hammer::fgd::PropertyType::Choices &&
            startDisabled->choices.size() == 2,
            "FGD typed choices and defaults are retained");
    const auto spawnflags = std::find_if(effective.begin(), effective.end(), [](const auto& property) {
        return property.key == "spawnflags";
    });
    require(spawnflags != effective.end() && spawnflags->type == hammer::fgd::PropertyType::Flags &&
            spawnflags->choices.front().defaultOn,
            "FGD spawnflag bit definitions are retained");
    const auto visualization = fgd.effectiveVisualization("info_target");
    require(visualization.displayColor == std::array<int, 3>{64, 128, 255} &&
            visualization.sizeMinimum == std::array<double, 3>{-8.0, -12.0, -16.0} &&
            visualization.sizeMaximum == std::array<double, 3>{8.0, 12.0, 16.0},
            "FGD point-class color and size helpers are retained");
    require(visualization.model == "models/editor/info_target.mdl" &&
            visualization.modelHelper == hammer::fgd::ModelHelperKind::Studio &&
            visualization.sprite == "editor/info_target.vmt" &&
            visualization.description == "Target",
            "FGD model, sprite, helper kind, and description are retained");

    constexpr std::string_view ModelHelperFgd = R"FGD(
@PointClass studioprop() = prop_static : "Static prop" [ model(studio) : "World model" ]
@PointClass lightprop("models/editor/spotlight.mdl") = light_spot : "Spotlight" [ pitch(float) : "Pitch" : -90 ]
)FGD";
    hammer::fgd::Database modelHelperFgd;
    require(modelHelperFgd.loadText(ModelHelperFgd, &fgdError),
            "FGD studioprop and lightprop helpers parse");
    const auto staticPropVisualization = modelHelperFgd.effectiveVisualization("prop_static");
    require(staticPropVisualization.modelHelper == hammer::fgd::ModelHelperKind::StudioProp &&
            staticPropVisualization.model.empty(),
            "parameterless studioprop uses the entity model key");
    const auto lightPropVisualization = modelHelperFgd.effectiveVisualization("light_spot");
    require(lightPropVisualization.modelHelper == hammer::fgd::ModelHelperKind::LightProp &&
            lightPropVisualization.model == "models/editor/spotlight.mdl",
            "lightprop retains its model and reverse-pitch helper kind");

    // FGD entity IO declarations (fgdlib InputOutput; op_output.cpp consumes
    // them for the Outputs page combos).
    {
        const char* IoFgd = R"FGD(
@BaseClass = Targetname
[
    targetname(target_source) : "Name"
    input Kill(void) : "Removes this entity."
    output OnUser1(void) : "Fired for FireUser1."
]
@PointClass base(Targetname) = logic_relay : "Relay"
[
    input Trigger(void) : "Triggers the relay."
    output OnTrigger(void) : "Fired when triggered."
    output OnSpawn(float)
]
)FGD";
        hammer::fgd::Database ioFgd;
        hammer::fgd::ParseError ioError;
        require(ioFgd.loadText(IoFgd, &ioError), "IO test FGD parses");
        const auto outputs = ioFgd.effectiveOutputs("logic_relay");
        const auto inputs = ioFgd.effectiveInputs("logic_relay");
        require(outputs.size() == 3 && inputs.size() == 2,
                "base-class IO declarations are inherited");
        bool foundSpawn = false;
        for (const auto& io : outputs) {
            if (io.name == "OnSpawn") {
                foundSpawn = true;
                require(io.valueType == "float" && io.description.empty(),
                        "an undescribed output keeps its value type");
            }
            if (io.name == "OnTrigger") {
                require(io.description == "Fired when triggered.",
                        "output descriptions parse");
            }
        }
        require(foundSpawn, "every declared output is listed");
    }

    const char* AngleVmf = R"VMF(
world { "id" "1" "classname" "worldspawn" }
entity { "id" "2" "classname" "prop_static" "origin" "0 0 0" "angles" "10 20 30" "pitch" "40" }
entity { "id" "3" "classname" "info_target" "origin" "1 0 0" "angle" "45" }
)VMF";
    auto angleDocument = Document::parse(AngleVmf, &error);
    require(angleDocument.has_value(), "entity transform VMF parses");
    auto angleScene = hammer::vmf::buildScene(*angleDocument);
    require(angleScene.entities.size() == 2, "entity transform scene builds");
    require(std::abs(angleScene.entities[0].angles.x - 10.0) < 1e-9 &&
            std::abs(angleScene.entities[0].angles.y - 20.0) < 1e-9 &&
            std::abs(angleScene.entities[0].angles.z - 30.0) < 1e-9 &&
            angleScene.entities[0].hasPitchOverride &&
            std::abs(angleScene.entities[0].pitchOverride - 40.0) < 1e-9,
            "raw angles and separate pitch override remain distinct");
    require(std::abs(angleScene.entities[1].angles.x) < 1e-9 &&
            std::abs(angleScene.entities[1].angles.y) < 1e-9 &&
            std::abs(angleScene.entities[1].angles.z) < 1e-9,
            "studio transform does not invent QAngles from the unrelated angle key");

    const auto zeroAngles = hammer::camera::sourceAngleBasis({0.0, 0.0, 0.0});
    require(std::abs(zeroAngles.forward.x - 1.0) < 1e-9 &&
            std::abs(zeroAngles.left.y - 1.0) < 1e-9 &&
            std::abs(zeroAngles.up.z - 1.0) < 1e-9,
            "Source QAngle zero basis is forward X, left Y, up Z");
    const auto yawLeft = hammer::camera::sourceAngleBasis({0.0, 90.0, 0.0});
    require(std::abs(yawLeft.forward.y - 1.0) < 1e-9 &&
            std::abs(yawLeft.left.x + 1.0) < 1e-9,
            "Source positive yaw rotates forward toward positive Y");
    angleScene.entities[0].reversePitch = true;
    const auto renderAngles = angleScene.entities[0].renderAngles();
    require(std::abs(renderAngles.x + 40.0) < 1e-9 &&
            std::abs(renderAngles.y - 20.0) < 1e-9 &&
            std::abs(renderAngles.z - 30.0) < 1e-9,
            "lightprop reverses only the final render pitch");
    const auto transform = hammer::camera::sourceTransform({10.0, 20.0, 30.0}, {0.0, 90.0, 0.0});
    const auto transformedPoint = transform.transformPoint({2.0, 0.0, 0.0});
    require(std::abs(transformedPoint.x - 10.0) < 1e-9 &&
            std::abs(transformedPoint.y - 22.0) < 1e-9 &&
            std::abs(transformedPoint.z - 30.0) < 1e-9,
            "shared Source affine transform applies yaw and translation exactly once");

    const std::filesystem::path fgdDirectory =
        std::filesystem::temp_directory_path() / "hammer-fgd-database-test";
    std::filesystem::create_directories(fgdDirectory);
    {
        std::ofstream baseFile(fgdDirectory / "base.fgd", std::ios::binary);
        baseFile << "@BaseClass = IncludedBase [ rendercolor(color255) : \"Render Color\" : \"255 255 255\" ]\n";
        std::ofstream mainFile(fgdDirectory / "main.fgd", std::ios::binary);
        mainFile << "@include \"base.fgd\"\n"
                    "@PointClass base(IncludedBase) = light : \"Light\" [ brightness(integer) : \"Brightness\" : 200 ]\n";
    }
    hammer::fgd::Database includedFgd;
    std::string fgdIoError;
    require(includedFgd.loadFile(fgdDirectory / "main.fgd", &fgdError, &fgdIoError),
            "FGD files load with relative @include directives");
    require(includedFgd.effectiveProperties("light").size() == 2,
            "included FGD base properties are inherited by point classes");
    std::filesystem::remove_all(fgdDirectory, removeError);

    // base.fgd names a @BaseClass "Light" and a @PointClass "light". FGD names
    // are case insensitive, so the two must not be merged: doing so left every
    // light entity with no brightness, style or falloff keys in SmartEdit.
    hammer::fgd::Database lightFgd;
    require(lightFgd.loadText(
                "@BaseClass color(180 10 180) = Light\n"
                "[\n"
                "  _light(color255) : \"Brightness\" : \"255 255 255 200\"\n"
                "  style(choices) : \"Appearance\" : 0 = [ 0 : \"Normal\" ]\n"
                "]\n"
                "@BaseClass = Targetname [ targetname(target_source) : \"Name\" ]\n"
                "@PointClass base(Targetname, Light) = light : \"A light.\"\n"
                "[ _distance(integer) : \"Maximum Distance\" : 0 ]\n"
                "@PointClass base(Targetname, Light) = light_spot : \"A spotlight.\"\n"
                "[ _cone(integer) : \"Outer angle\" : 45 ]\n",
                &fgdError),
            "an FGD declaring both a Light base class and a light point class parses");
    const auto lightProperties = lightFgd.effectiveProperties("light");
    const auto hasKey = [](const std::vector<hammer::fgd::PropertyDefinition>& properties,
                           std::string_view key) {
        return std::any_of(properties.begin(), properties.end(),
                           [key](const hammer::fgd::PropertyDefinition& property) {
                               return property.key == key;
                           });
    };
    require(hasKey(lightProperties, "_light") && hasKey(lightProperties, "style") &&
                hasKey(lightProperties, "targetname") && hasKey(lightProperties, "_distance"),
            "light inherits the like-named Light base class instead of shadowing it");
    const auto spotProperties = lightFgd.effectiveProperties("light_spot");
    require(hasKey(spotProperties, "_light") && hasKey(spotProperties, "_cone"),
            "light_spot inherits the Light base class as well");
    const hammer::fgd::EntityClass* lightPointClass = lightFgd.findClass("light");
    require(lightPointClass && lightPointClass->kind == hammer::fgd::ClassKind::Point,
            "a classname lookup still resolves to the point class, not the base class");

    // Decal and overlay helpers project onto the same brush/displacement
    // geometry consumed by the viewport, and preserve nested overlaydata.
    hammer::vmf::EditorModel projectedEditor(Document::createDefault());
    require(projectedEditor.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 16.0},
                                        "test/base").has_value(),
            "projection test brush is created");
    hammer::vmf::Scene projectedBase = hammer::vmf::buildScene(projectedEditor.document());
    require(!projectedBase.brushes.empty(), "projection test brush builds scene geometry");
    const hammer::vmf::BrushGeometry& projectedBrush = projectedBase.brushes.front();
    const hammer::vmf::FaceGeometry* projectedFace = nullptr;
    for (const auto& face : projectedBrush.faces) {
        if (std::abs(face.normal.z) > 0.9 && !face.vertices.empty()) {
            projectedFace = &face;
            break;
        }
    }
    require(projectedFace != nullptr, "projection test finds a horizontal face");
    const hammer::vmf::Vec3 facePoint =
        projectedBrush.vertices[projectedFace->vertices.front()];
    hammer::vmf::Vec3 faceCenter{};
    for (std::size_t index : projectedFace->vertices) {
        const auto& point = projectedBrush.vertices[index];
        faceCenter.x += point.x;
        faceCenter.y += point.y;
        faceCenter.z += point.z;
    }
    const double inverseFacePoints = 1.0 / static_cast<double>(projectedFace->vertices.size());
    faceCenter.x *= inverseFacePoints;
    faceCenter.y *= inverseFacePoints;
    faceCenter.z *= inverseFacePoints;
    const hammer::vmf::Vec3 decalOrigin{
        faceCenter.x + projectedFace->normal.x * 0.25,
        faceCenter.y + projectedFace->normal.y * 0.25,
        faceCenter.z + projectedFace->normal.z * 0.25};
    require(projectedEditor.createPointEntity(
                "infodecal", decalOrigin, {{"texture", "decals/test"}}).has_value(),
            "infodecal entity is created");
    const auto overlayObject = projectedEditor.createPointEntity("info_overlay", faceCenter);
    require(overlayObject.has_value(), "info_overlay entity is created");
    auto formatVector = [](const hammer::vmf::Vec3& value) {
        return std::to_string(value.x) + " " + std::to_string(value.y) + " " +
               std::to_string(value.z);
    };
    for (Block& root : projectedEditor.document().roots()) {
        if (root.name != "entity" || !root.value("id") ||
            std::atoi(root.value("id")->c_str()) != overlayObject->id) continue;
        Block& data = root.appendChild("overlaydata");
        auto setOverlayValue = [&](const std::string& key, const std::string& value) {
            root.setValue(key, value);
            data.setValue(key, value);
        };
        setOverlayValue("material", "decals/overlay_test");
        setOverlayValue("StartU", "0");
        setOverlayValue("EndU", "1");
        setOverlayValue("StartV", "0");
        setOverlayValue("EndV", "1");
        setOverlayValue("BasisOrigin", formatVector(faceCenter));
        setOverlayValue("BasisU", "1 0 0");
        setOverlayValue("BasisV", "0 1 0");
        setOverlayValue("BasisNormal", formatVector(projectedFace->normal));
        setOverlayValue("uv0", "-24 -24 0");
        setOverlayValue("uv1", "-24 24 0");
        setOverlayValue("uv2", "24 24 0");
        setOverlayValue("uv3", "24 -24 0");
        setOverlayValue("sides", std::to_string(projectedFace->sideId));
    }
    hammer::vmf::Scene projectedScene = hammer::vmf::buildScene(projectedEditor.document());
    hammer::vmf::rebuildProjectedSurfaceGeometry(projectedScene,
        [](std::string_view material) -> std::optional<hammer::vmf::ProjectedMaterialInfo> {
            if (material == "decals/test") return hammer::vmf::ProjectedMaterialInfo{64, 64, 1.0};
            return std::nullopt;
        });
    const auto projectedDecal = std::find_if(projectedScene.entities.begin(), projectedScene.entities.end(),
        [](const auto& entity) { return entity.classname == "infodecal"; });
    const auto projectedOverlay = std::find_if(projectedScene.entities.begin(), projectedScene.entities.end(),
        [](const auto& entity) { return entity.classname == "info_overlay"; });
    require(projectedDecal != projectedScene.entities.end() &&
            !projectedDecal->projectedSurfaces.empty() &&
            projectedDecal->projectedSurfaces.front().triangles.size() >= 3,
            "infodecal is clipped into renderable projected triangles");
    require(projectedOverlay != projectedScene.entities.end() &&
            !projectedOverlay->overlayProperties.empty() &&
            std::find_if(projectedOverlay->properties.begin(), projectedOverlay->properties.end(),
                         [](const auto& property) {
                             return property.first == "material" &&
                                    property.second == "decals/overlay_test";
                         }) != projectedOverlay->properties.end() &&
            !projectedOverlay->projectedSurfaces.empty() &&
            projectedOverlay->projectedSurfaces.front().triangles.size() >= 3,
            "root and nested info_overlay data project onto the referenced side");
    const hammer::vmf::Vec3 overlayBefore = projectedOverlay->projectedSurfaces.front().triangles.front().position;
    projectedEditor.setSelection({*overlayObject});
    auto overlayProperties = projectedEditor.selectedProperties();
    for (auto& property : overlayProperties) {
        if (property.key == "material") property.value = "decals/edited_overlay";
    }
    require(projectedEditor.replaceSelectedProperties(overlayProperties, "Edit Overlay Material"),
            "overlay entity properties are editable");
    bool nestedMaterialSynchronized = false;
    for (const Block& root : projectedEditor.document().roots()) {
        if (root.name != "entity" || !root.value("id") ||
            std::atoi(root.value("id")->c_str()) != overlayObject->id) continue;
        const auto overlayData = root.children("overlaydata");
        nestedMaterialSynchronized = !overlayData.empty() &&
            overlayData.front()->value("material") &&
            *overlayData.front()->value("material") == "decals/edited_overlay";
    }
    require(nestedMaterialSynchronized,
            "editing compiler-facing overlay keys synchronizes nested overlaydata");
    require(projectedEditor.translateSelection({8.0, 4.0, 0.0}),
            "overlay entity translation updates root and helper data");
    hammer::vmf::Scene movedOverlayScene = hammer::vmf::buildScene(projectedEditor.document());
    hammer::vmf::rebuildProjectedSurfaceGeometry(movedOverlayScene,
        [](std::string_view) -> std::optional<hammer::vmf::ProjectedMaterialInfo> { return std::nullopt; });
    const auto movedOverlay = std::find_if(movedOverlayScene.entities.begin(), movedOverlayScene.entities.end(),
        [](const auto& entity) { return entity.classname == "info_overlay"; });
    require(movedOverlay != movedOverlayScene.entities.end() && !movedOverlay->projectedSurfaces.empty(),
            "translated overlay remains projectable");
    const hammer::vmf::Vec3 overlayAfter = movedOverlay->projectedSurfaces.front().triangles.front().position;
    require(std::abs((overlayAfter.x - overlayBefore.x) - 8.0) < 0.1 &&
            std::abs((overlayAfter.y - overlayBefore.y) - 4.0) < 0.1,
            "overlay BasisOrigin and UV handles move with the entity transform");

    hammer::camera::State camera;
    camera.position = {0.0, 0.0, 0.0};
    camera.yawRadians = 0.0;
    camera.pitchRadians = 0.0;
    const auto cameraForward = hammer::camera::forwardVector(camera);
    require(std::abs(cameraForward.x - 1.0) < 1e-9 && std::abs(cameraForward.y) < 1e-9,
            "3D camera yaw zero faces positive X");
    const auto cameraRight = hammer::camera::rightVector(camera);
    require(std::abs(cameraRight.x) < 1e-9 &&
            std::abs(cameraRight.y + 1.0) < 1e-9 &&
            std::abs(cameraRight.z) < 1e-9,
            "3D camera uses Source negative Y as screen-right at yaw zero");
    const auto cameraUp = hammer::camera::upVector(camera);
    require(std::abs(cameraUp.z - 1.0) < 1e-9,
            "3D camera keeps positive world Z toward the top of the viewport");
    const double basisX = cameraRight.y * cameraUp.z - cameraRight.z * cameraUp.y;
    const double basisY = cameraRight.z * cameraUp.x - cameraRight.x * cameraUp.z;
    const double basisZ = cameraRight.x * cameraUp.y - cameraRight.y * cameraUp.x;
    require(std::abs(basisX + cameraForward.x) < 1e-9 &&
            std::abs(basisY + cameraForward.y) < 1e-9 &&
            std::abs(basisZ + cameraForward.z) < 1e-9,
            "3D camera basis is right-handed instead of horizontally reflected");
    const auto centeredPerspective = hammer::camera::projectPoint(
        camera, hammer::camera::ProjectionMode::Perspective, {128.0, 0.0, 0.0}, 800.0, 600.0);
    require(centeredPerspective.has_value() &&
            std::abs(centeredPerspective->x - 400.0) < 1e-9 &&
            std::abs(centeredPerspective->y - 300.0) < 1e-9,
            "perspective camera projects its forward axis to viewport center");
    const auto sourceLeft = hammer::camera::projectPoint(
        camera, hammer::camera::ProjectionMode::Perspective, {128.0, 32.0, 0.0}, 800.0, 600.0);
    const auto sourceRight = hammer::camera::projectPoint(
        camera, hammer::camera::ProjectionMode::Perspective, {128.0, -32.0, 0.0}, 800.0, 600.0);
    require(sourceLeft && sourceRight && sourceLeft->x < 400.0 && sourceRight->x > 400.0,
            "3D projection places Source +Y on the left and -Y on the right");
    require(!hammer::camera::projectPoint(
                camera, hammer::camera::ProjectionMode::Perspective, {-16.0, 0.0, 0.0}, 800.0, 600.0),
            "camera rejects points behind the near plane");
    hammer::camera::ScreenPoint clippedA;
    hammer::camera::ScreenPoint clippedB;
    require(hammer::camera::projectLine(
                camera, hammer::camera::ProjectionMode::Perspective,
                {-16.0, 32.0, 0.0}, {128.0, 32.0, 0.0}, 800.0, 600.0,
                clippedA, clippedB),
            "3D grid and brush lines crossing the near plane are clipped instead of exploding");
    const auto orthoNear = hammer::camera::projectPoint(
        camera, hammer::camera::ProjectionMode::Orthographic, {64.0, 32.0, 0.0}, 800.0, 600.0);
    const auto orthoFar = hammer::camera::projectPoint(
        camera, hammer::camera::ProjectionMode::Orthographic, {256.0, 32.0, 0.0}, 800.0, 600.0);
    require(orthoNear && orthoFar && std::abs(orthoNear->x - orthoFar->x) < 1e-9,
            "orthographic 3D projection does not shrink geometry with distance");

    // Clipping tool (hammer/ToolClipper.cpp, CMapSolid::Split).
    {
        using hammer::vmf::ClipPlane;
        using hammer::vmf::EditorModel;
        using hammer::vmf::ObjectType;
        using hammer::vmf::classifySolid;
        using hammer::vmf::clipPlaneFromLine;
        using hammer::vmf::SolidPlaneRelation;

        const ClipPlane xPlane = clipPlaneFromLine({0.0, -64.0, 0.0}, {0.0, 64.0, 0.0}, {0.0, 0.0, 1.0});
        require(std::abs(std::abs(xPlane.normal.x) - 1.0) < 1e-9 && std::abs(xPlane.distance) < 1e-9,
                "clip plane built from a 2D line is extruded along the view axis");

        EditorModel model;
        const auto block = model.createBlock({-64.0, -64.0, 0.0}, {64.0, 64.0, 64.0});
        require(block.has_value(), "clip test block created");

        const auto solidCount = [&model] {
            return model.document().statistics().solids;
        };
        require(solidCount() == 1, "clip test starts with one solid");

        const auto* solidBlock = model.document().firstRoot("world")->children("solid").front();
        require(classifySolid(*solidBlock, xPlane) == SolidPlaneRelation::Split,
                "a solid straddling the clip plane is classified as split");

        auto preview = model.previewClip(xPlane, EditorModel::ClipMode::Front);
        require(preview.kept.size() == 1 && preview.discarded.size() == 1,
                "keep-front preview shows one kept and one discarded half");
        require(preview.kept.front().size() == 6 && preview.discarded.front().size() == 6,
                "each clipped half of a box keeps five original sides plus the new cap");

        auto both = model.previewClip(xPlane, EditorModel::ClipMode::Both);
        require(both.kept.size() == 2 && both.discarded.empty(),
                "keep-both preview keeps both halves");

        require(model.clipSelection(xPlane, EditorModel::ClipMode::Both), "keep-both clip applies");
        require(solidCount() == 2, "keep-both clip replaces the solid with two halves");
        require(model.selection().size() == 2, "both clipped halves are selected");
        require(model.undoLabel() == "Clip Objects", "clip is a single undoable operation");

        const auto halves = model.document().firstRoot("world")->children("solid");
        std::vector<int> sideIds;
        for (const Block* half : halves) {
            require(half->children("side").size() == 6, "each clipped half of a box has six sides");
            for (const Block* side : half->children("side")) sideIds.push_back(std::atoi(side->value("id")->c_str()));
        }
        std::sort(sideIds.begin(), sideIds.end());
        require(std::adjacent_find(sideIds.begin(), sideIds.end()) == sideIds.end(),
                "clipped halves receive unique side ids");

        const auto clippedScene = hammer::vmf::buildScene(model.document());
        require(clippedScene.brushes.size() == 2, "clipped halves rebuild as real solids");
        for (const auto& brush : clippedScene.brushes) {
            require(brush.faces.size() == 6, "clipped halves render six faces");
            require(brush.vertices.size() == 8, "clipped halves are closed boxes");
        }

        require(model.undo(), "clip can be undone");
        require(solidCount() == 1, "undo restores the original solid");

        require(model.clipSelection(xPlane, EditorModel::ClipMode::Front), "keep-front clip applies");
        require(solidCount() == 1, "keep-front clip leaves a single half");

        require(model.undo(), "keep-front clip can be undone");
        const ClipPlane outside = clipPlaneFromLine({256.0, -64.0, 0.0}, {256.0, 64.0, 0.0}, {0.0, 0.0, 1.0});
        require(classifySolid(*model.document().firstRoot("world")->children("solid").front(), outside) !=
                    SolidPlaneRelation::Split,
                "a solid entirely on one side of the plane is not split");
        require(!model.clipSelection(outside, EditorModel::ClipMode::Front),
                "a solid entirely on the kept side survives whole and needs no undo entry");
        require(solidCount() == 1, "clipping outside the solid does not create geometry");
        require(model.clipSelection(outside, EditorModel::ClipMode::Back),
                "a solid entirely on the discarded side is removed");
        require(solidCount() == 0, "the discarded solid is gone");
        require(model.undo() && solidCount() == 1, "removing a whole solid is undoable");
    }

    // Carve tool (VmfSolidCarve.cpp — deliberately not CMapSolid::Carve's
    // clip-against-every-plane fragment generator).
    {
        using hammer::vmf::EditorModel;
        using hammer::vmf::ObjectType;
        using hammer::vmf::solidVolume;

        const auto solidId = [](const Block* solid) {
            return std::atoi(solid->value("id")->c_str());
        };
        const auto worldSolids = [](const EditorModel& model) {
            return model.document().firstRoot("world")->children("solid");
        };

        // Corner overlap: only the 3 carver planes that actually cut the
        // target may produce pieces (Valve's carve clips against all 6).
        EditorModel model;
        require(model.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 128.0}).has_value(),
                "carve target created");
        const auto carver = model.createBlock({64.0, 64.0, 64.0}, {192.0, 192.0, 192.0});
        require(carver.has_value(), "carve carver created");
        require(model.selection().size() == 1 && model.selection().front() == *carver,
                "the carver is the selection");

        require(model.carveSelection(), "corner-overlap carve applies");
        require(model.undoLabel() == "Carve", "carve is a single undoable operation");
        require(worldSolids(model).size() == 4,
                "corner overlap carves the target into exactly three pieces");
        require(model.selection().size() == 1 && model.selection().front() == *carver,
                "the carver stays selected after the carve");

        double carvedVolume = 0.0;
        for (const Block* solid : worldSolids(model)) {
            if (solidId(solid) == carver->id) {
                require(std::abs(solidVolume(*solid) - 128.0 * 128.0 * 128.0) < 1.0,
                        "the carver itself is untouched");
                continue;
            }
            const double volume = solidVolume(*solid);
            require(volume > 1.0, "carve produces no degenerate slivers");
            carvedVolume += volume;
        }
        require(std::abs(carvedVolume - (128.0 * 128.0 * 128.0 - 64.0 * 64.0 * 64.0)) < 1.0,
                "the pieces sum to target volume minus the overlap");

        const auto carvedScene = hammer::vmf::buildScene(model.document());
        require(carvedScene.brushes.size() == 4, "carved pieces rebuild as real solids");

        require(model.undo(), "carve can be undone");
        require(worldSolids(model).size() == 2, "undo restores the uncarved target");

        // Carver floating wholly inside the target: six pieces is the minimum
        // a convex decomposition of a hollowed box can produce.
        EditorModel inside;
        require(inside.createBlock({0.0, 0.0, 0.0}, {256.0, 256.0, 256.0}).has_value() &&
                    inside.createBlock({64.0, 64.0, 64.0}, {192.0, 192.0, 192.0}).has_value(),
                "contained-carver blocks created");
        require(inside.carveSelection(), "contained carve applies");
        require(worldSolids(inside).size() == 7,
                "a fully contained carver leaves six pieces around the hole");
        double hollowVolume = 0.0;
        for (const Block* solid : worldSolids(inside)) hollowVolume += solidVolume(*solid);
        require(std::abs(hollowVolume - 256.0 * 256.0 * 256.0) < 1.0,
                "the six pieces plus the carver still fill the original box");

        // A face-touching neighbour is byte-identical untouched — nicking
        // adjacent geometry was the classic Valve carve failure.
        EditorModel touching;
        require(touching.createBlock({128.0, 0.0, 0.0}, {256.0, 128.0, 128.0}).has_value() &&
                    touching.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 128.0}).has_value(),
                "touching-neighbour blocks created");
        const std::string before = touching.document().serialize(false);
        require(!touching.carveSelection(),
                "carving against a merely touching neighbour is refused without an undo entry");
        require(touching.document().serialize(false) == before,
                "a touching neighbour is left byte-identical");

        // A target swallowed whole is deleted.
        EditorModel swallowed;
        require(swallowed.createBlock({32.0, 32.0, 32.0}, {96.0, 96.0, 96.0}).has_value() &&
                    swallowed.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 128.0}).has_value(),
                "swallowed-target blocks created");
        require(swallowed.carveSelection(), "swallowing carve applies");
        require(worldSolids(swallowed).size() == 1 && swallowed.undo() &&
                    worldSolids(swallowed).size() == 2,
                "a target inside the carver is deleted, undoably");

        // Displacement solids are never carved, like the clip tool.
        EditorModel displaced;
        require(displaced.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 128.0}).has_value() &&
                    displaced.createBlock({64.0, 64.0, 64.0}, {192.0, 192.0, 192.0}).has_value(),
                "displacement-carve blocks created");
        displaced.document().firstRoot("world")->children("solid").front()
            ->children("side").front()->appendChild("dispinfo").setValue("power", "2");
        require(!displaced.carveSelection(), "a displacement target is never carved");
    }

    // Vertex manipulation (hammer/ToolMorph.cpp, hammer/SSolid.cpp).
    {
        using hammer::vmf::EditorModel;
        using hammer::vmf::MorphHandleMode;
        using hammer::vmf::MorphHandleRef;
        using hammer::vmf::ObjectType;
        using hammer::vmf::Vec3;

        EditorModel model;
        const auto created = model.createBlock({-64.0, -64.0, 0.0}, {64.0, 64.0, 64.0});
        require(created.has_value(), "morph test block created");

        auto sceneOf = [&] { return hammer::vmf::buildScene(model.document()); };
        auto morphScene = sceneOf();
        require(morphScene.brushes.size() == 1, "morph test starts with one solid");

        auto solids = hammer::vmf::buildMorphSolids(morphScene, model.selection());
        require(solids.size() == 1, "the selected solid is put into morph mode");
        require(solids.front().vertices.size() == 8 && solids.front().edges.size() == 12 &&
                    solids.front().faces.size() == 6,
                "a box's vertex mesh is 8 vertices, 12 edges and 6 faces");
        require(!solids.front().moved(), "a freshly built mesh reports no movement");

        // Handle lists follow the display mode, as CSSolid::ShowHandles does.
        require(hammer::vmf::morphHandles(solids, MorphHandleMode::VerticesAndEdges, {}).size() == 20,
                "vertices+edges mode shows a handle per vertex and per edge midpoint");
        require(hammer::vmf::morphHandles(solids, MorphHandleMode::Vertices, {}).size() == 8,
                "vertex mode shows only vertex handles");
        require(hammer::vmf::morphHandles(solids, MorphHandleMode::Edges, {}).size() == 12,
                "edge mode shows only edge handles");
        require(hammer::vmf::nextMorphHandleMode(MorphHandleMode::VerticesAndEdges) == MorphHandleMode::Vertices &&
                    hammer::vmf::nextMorphHandleMode(MorphHandleMode::Vertices) == MorphHandleMode::Edges &&
                    hammer::vmf::nextMorphHandleMode(MorphHandleMode::Edges) == MorphHandleMode::VerticesAndEdges,
                "Morph3D::ToggleMode cycles both -> vertex -> edge -> both");

        // Moving an edge midpoint handle drags both of its vertices
        // (CSSolid::MoveSelectedHandles).
        {
            auto edgeTest = solids;
            const auto [first, second] = edgeTest.front().edges.front();
            const Vec3 beforeFirst = edgeTest.front().vertices[first];
            const Vec3 beforeSecond = edgeTest.front().vertices[second];
            hammer::vmf::moveMorphHandles(edgeTest.front(), {}, {0}, {0.0, 0.0, 8.0});
            require(std::abs(edgeTest.front().vertices[first].z - (beforeFirst.z + 8.0)) < 1e-9 &&
                        std::abs(edgeTest.front().vertices[second].z - (beforeSecond.z + 8.0)) < 1e-9,
                    "an edge handle moves both of its vertices");
        }

        // Raise the four top vertices: the top face's plane must be re-derived.
        std::vector<std::size_t> topVertices;
        for (std::size_t i = 0; i < solids.front().vertices.size(); ++i) {
            if (solids.front().vertices[i].z > 63.0) topVertices.push_back(i);
        }
        require(topVertices.size() == 4, "a box has four top vertices");
        hammer::vmf::moveMorphHandles(solids.front(), topVertices, {}, {0.0, 0.0, 32.0});
        require(solids.front().moved(), "moving handles marks the mesh as moved");

        require(model.applyMorph(solids), "a moved vertex mesh commits to the document");
        require(model.undoLabel() == "Morphing", "the morph is one undoable step, labelled as in Hammer");

        morphScene = sceneOf();
        require(morphScene.brushes.size() == 1, "the morphed solid is still one solid");
        double topZ = -1e9;
        for (const Vec3& vertex : morphScene.brushes.front().vertices) topZ = std::max(topZ, vertex.z);
        require(std::abs(topZ - 96.0) < 0.01, "the re-derived top plane sits at the moved height");
        require(morphScene.brushes.front().vertices.size() == 8 &&
                    morphScene.brushes.front().faces.size() == 6,
                "the morphed solid still has six faces and eight corners");

        require(model.undo(), "the morph can be undone");
        morphScene = sceneOf();
        topZ = -1e9;
        for (const Vec3& vertex : morphScene.brushes.front().vertices) topZ = std::max(topZ, vertex.z);
        require(std::abs(topZ - 64.0) < 0.01, "undo restores the original plane");

        // Tilting the top face by raising only two of its vertices re-derives a
        // slanted plane rather than keeping the axial one.
        {
            auto tilted = hammer::vmf::buildMorphSolids(morphScene, model.selection());
            require(tilted.size() == 1, "the solid can be morphed again after undo");
            std::vector<std::size_t> raised;
            for (std::size_t i = 0; i < tilted.front().vertices.size(); ++i) {
                const Vec3& vertex = tilted.front().vertices[i];
                if (vertex.z > 63.0 && vertex.x > 0.0) raised.push_back(i);
            }
            require(raised.size() == 2, "two top vertices share the +x edge");
            hammer::vmf::moveMorphHandles(tilted.front(), raised, {}, {0.0, 0.0, 32.0});
            require(model.applyMorph(tilted), "the tilting morph commits");
            const auto tiltedScene = sceneOf();
            const auto& faces = tiltedScene.brushes.front().faces;
            const bool slanted = std::any_of(faces.begin(), faces.end(), [](const auto& face) {
                return face.normal.z > 0.1 && std::abs(face.normal.x) > 0.1;
            });
            require(slanted, "the moved face's plane is re-derived from its new points");
            require(model.undo(), "the tilting morph is undoable");
        }

        // Collapsing every vertex onto one point cannot produce a solid, so the
        // commit is refused and the document is left alone.
        {
            auto collapsed = hammer::vmf::buildMorphSolids(sceneOf(), model.selection());
            require(collapsed.size() == 1, "collapse test mesh built");
            for (Vec3& vertex : collapsed.front().vertices) vertex = {0.0, 0.0, 0.0};
            const std::string before = model.document().serialize(false);
            require(!model.applyMorph(collapsed), "a collapsed mesh is not committed");
            require(model.document().serialize(false) == before,
                    "a refused morph leaves the document untouched");
        }
    }

    // ---------------------------------------------------------------------
    // Texture Application tool: the justification math (mapface.cpp
    // JustifyTextureUsingExtents / GetTextureExtents) and the material apply
    // (faceedit_materialpage.cpp Apply, ToolMaterial.cpp "Apply texture").
    // ---------------------------------------------------------------------
    {
        using hammer::vmf::FaceExtents;
        using hammer::vmf::FaceTexture;
        using hammer::vmf::TextureJustification;
        using Vec3 = hammer::vmf::Vec3;

        // A 100 x 60 axis-aligned face at z = 32, mapped with Hammer's default
        // world-aligned axes and scale, against a 128 x 128 texture.
        const std::vector<Vec3> points{{0.0, 0.0, 32.0}, {100.0, 0.0, 32.0},
                                       {100.0, 60.0, 32.0}, {0.0, 60.0, 32.0}};
        FaceExtents extents{};
        require(hammer::vmf::faceExtents(points, extents), "face extents built");

        const auto justified = [&](TextureJustification justification) {
            FaceTexture texture;
            texture.uAxis = {1.0, 0.0, 0.0};
            texture.vAxis = {0.0, -1.0, 0.0};
            texture.uScale = 0.25;
            texture.vScale = 0.25;
            hammer::vmf::justifyTextureUsingExtents(texture, justification, extents, 128, 128);
            return texture;
        };

        const auto close = [](double a, double b) { return std::fabs(a - b) < 1e-6; };

        require(close(justified(TextureJustification::Left).uShift, 0.0), "justify left");
        require(close(justified(TextureJustification::Right).uShift, -16.0), "justify right");
        require(close(justified(TextureJustification::Top).vShift, 112.0), "justify top");
        require(close(justified(TextureJustification::Bottom).vShift, 0.0), "justify bottom");
        const FaceTexture centered = justified(TextureJustification::Center);
        require(close(centered.uShift, -8.0) && close(centered.vShift, 56.0), "justify center");

        // Fit computes its scale at a scale of 1 and then justifies top left.
        const FaceTexture fitted = justified(TextureJustification::Fit);
        require(close(fitted.uScale, 100.0 / 128.0) && close(fitted.vScale, 60.0 / 128.0),
                "fit to face scales the texture onto the face");
        require(close(fitted.uShift, 0.0) && close(fitted.vShift, 0.0),
                "fit to face justifies top left");

        // CMapFace::RotateTextureAxes turns the axes about V x U.
        FaceTexture rotated;
        rotated.uAxis = {1.0, 0.0, 0.0};
        rotated.vAxis = {0.0, -1.0, 0.0};
        hammer::vmf::rotateTextureAxes(rotated, 90.0);
        require(close(rotated.uAxis.x, 0.0) && close(rotated.uAxis.y, 1.0) &&
                close(rotated.vAxis.x, 1.0) && close(rotated.vAxis.y, 0.0),
                "texture axes rotate about the texture normal");
    }

    {
        hammer::vmf::EditorModel model;
        const auto solid = model.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 128.0},
                                             "tools/toolsnodraw");
        require(solid.has_value(), "face-edit test block created");

        // "Apply current texture": every side of the selected solid, one undo step.
        require(model.applyMaterialToSelection("brick/brickwall001a"),
                "apply current texture changes the selection");
        require(!model.applyMaterialToSelection("brick/brickwall001a"),
                "re-applying the same texture is not an undo step");

        const hammer::vmf::Block* block = nullptr;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name != "world") continue;
            for (const hammer::vmf::Block* child : root.children("solid")) block = child;
        }
        require(block != nullptr, "test solid found");
        const auto sides = block->children("side");
        require(sides.size() == 6, "test solid has six sides");
        for (const hammer::vmf::Block* side : sides) {
            const std::string* material = side->value("material");
            require(material && *material == "brick/brickwall001a", "every side received the texture");
        }

        // One face, one field: everything else on that side is left alone.
        const int sideId = std::atoi(sides.front()->value("id")->c_str());
        const hammer::vmf::FaceRef face{solid->id, sideId};
        hammer::vmf::FaceTextureEdit edit;
        edit.scaleX = 0.5;
        require(model.applyFaceTextures({face}, edit), "single-face scale applied");
        const auto texture = model.faceTexture(face);
        require(texture.has_value(), "face texture read back");
        require(std::fabs(texture->uScale - 0.5) < 1e-9, "scale X written");
        require(std::fabs(texture->vScale - 0.25) < 1e-9, "blank fields are left alone");

        require(model.undo(), "face edit undone");
        require(std::fabs(model.faceTexture(face)->uScale - 0.25) < 1e-9, "undo restored the scale");
        require(model.redo(), "face edit redone");
        require(std::fabs(model.faceTexture(face)->uScale - 0.5) < 1e-9, "redo reapplied the scale");
    }

    {
        // Replace Textures (hammer/replacetexdlg.cpp CReplaceTexDlg::DoReplaceTextures).
        hammer::vmf::EditorModel model;
        const auto solidA = model.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                              "brick/brickwall001a");
        require(solidA.has_value(), "replace-textures solid A created");
        const auto solidB = model.createBlock({128.0, 0.0, 0.0}, {192.0, 64.0, 64.0},
                                              "brick/brickwall002a");
        require(solidB.has_value(), "replace-textures solid B created");

        // Exact match only touches the solid whose material equals "find"
        // exactly; "brick/brickwall002a" survives untouched.
        hammer::vmf::ReplaceTexturesRequest exactRequest;
        exactRequest.find = "brick/brickwall001a";
        exactRequest.replace = "concrete/concretefloor001a";
        exactRequest.mode = hammer::vmf::TextureMatchMode::Exact;
        const auto exactResult = model.replaceTextures(exactRequest);
        require(exactResult.facesMatched == 6 && exactResult.facesChanged == 6,
                "exact match replaces every side of the matching solid");

        const auto materialOf = [&](const std::optional<hammer::vmf::ObjectRef>& solid) {
            const hammer::vmf::Block* block = nullptr;
            for (const hammer::vmf::Block& root : model.document().roots()) {
                if (root.name != "world") continue;
                for (const hammer::vmf::Block* child : root.children("solid")) {
                    if (std::atoi(child->value("id")->c_str()) == solid->id) block = child;
                }
            }
            return block ? *block->children("side").front()->value("material") : std::string{};
        };
        require(materialOf(solidA) == "concrete/concretefloor001a", "solid A got the new material");
        require(materialOf(solidB) == "brick/brickwall002a", "solid B untouched by exact match");

        require(model.undo(), "replace textures undone as one step");
        require(materialOf(solidA) == "brick/brickwall001a", "undo restored solid A's material");

        // Partial match: "brickwall" matches both bricks; substitute-partial
        // keeps the rest of each name and only swaps the matched substring.
        hammer::vmf::ReplaceTexturesRequest substituteRequest;
        substituteRequest.find = "brickwall";
        substituteRequest.replace = "stonewall";
        substituteRequest.mode = hammer::vmf::TextureMatchMode::SubstitutePartial;
        const auto substituteResult = model.replaceTextures(substituteRequest);
        require(substituteResult.facesMatched == 12, "partial match finds both solids' sides");
        require(materialOf(solidA) == "brick/stonewall001a", "substitution keeps the rest of the name (A)");
        require(materialOf(solidB) == "brick/stonewall002a", "substitution keeps the rest of the name (B)");

        require(model.undo(), "substitution undone as one step");
        require(materialOf(solidA) == "brick/brickwall001a" && materialOf(solidB) == "brick/brickwall002a",
                "undo restored both materials");

        // Mark only selects the matching solids instead of touching the document,
        // and is not an undo step.
        model.clearSelection();
        const std::size_t undoDepthBeforeMark = model.canUndo() ? 1 : 0;
        hammer::vmf::ReplaceTexturesRequest markRequest;
        markRequest.find = "brickwall";
        markRequest.replace = "stonewall";
        markRequest.mode = hammer::vmf::TextureMatchMode::Partial;
        markRequest.markOnly = true;
        const auto markResult = model.replaceTextures(markRequest);
        require(markResult.facesMatched == 12 && markResult.facesChanged == 0,
                "mark only reports matches without changing sides");
        require(model.selection().size() == 2, "mark only selects both matching solids");
        require(materialOf(solidA) == "brick/brickwall001a", "mark only leaves materials untouched");
        require((model.canUndo() ? 1u : 0u) == undoDepthBeforeMark, "mark only is not an undo step");

        // "Marked objects" scope only touches the current selection.
        model.setSelection({*solidA});
        hammer::vmf::ReplaceTexturesRequest scopedRequest;
        scopedRequest.find = "brickwall001a";
        scopedRequest.replace = "concretefloor001a";
        scopedRequest.mode = hammer::vmf::TextureMatchMode::SubstitutePartial;
        scopedRequest.selectionOnly = true;
        const auto scopedResult = model.replaceTextures(scopedRequest);
        require(scopedResult.facesChanged == 6, "selection-only scope changes only the marked solid");
        require(materialOf(solidA) == "brick/concretefloor001a", "selection-only scope edited solid A");
        require(materialOf(solidB) == "brick/brickwall002a", "selection-only scope left solid B alone");
        require(model.undo(), "selection-only replace undone");
    }

    {
        // Smoothing groups: the "smoothing_groups" bitmask on each side, edited
        // the way CSmoothingGroupMgr::AddFaceToGroup / RemoveFaceFromGroup do.
        hammer::vmf::EditorModel model;
        const auto solid = model.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                             "tools/toolsnodraw");
        require(solid.has_value(), "smoothing-group test block created");

        const hammer::vmf::Block* block = nullptr;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name != "world") continue;
            for (const hammer::vmf::Block* child : root.children("solid")) block = child;
        }
        require(block != nullptr, "smoothing-group test solid found");
        const auto sides = block->children("side");
        require(sides.size() == 6, "smoothing-group test solid has six sides");
        const hammer::vmf::FaceRef faceA{solid->id, std::atoi(sides[0]->value("id")->c_str())};
        const hammer::vmf::FaceRef faceB{solid->id, std::atoi(sides[1]->value("id")->c_str())};

        require(model.faceSmoothingGroups(faceA) == 0u, "new sides start in no smoothing group");
        require(hammer::vmf::smoothingGroupBit(1) == 1u, "group 1 is bit 0");
        require(hammer::vmf::smoothingGroupBit(32) == 0x80000000u, "group 32 is bit 31");
        require(hammer::vmf::smoothingGroupBit(0) == 0u && hammer::vmf::smoothingGroupBit(33) == 0u,
                "out of range groups have no bit");

        // Two faces into group 3 in one undo step.
        const std::uint32_t group3 = hammer::vmf::smoothingGroupBit(3);
        require(model.applySmoothingGroups({faceA, faceB}, group3, 0u),
                "faces added to a smoothing group");
        require(model.faceSmoothingGroups(faceA) == group3 &&
                model.faceSmoothingGroups(faceB) == group3,
                "both faces carry the group bit");
        require(!model.applySmoothingGroups({faceA, faceB}, group3, 0u),
                "re-adding faces already in the group is not an undo step");

        // A second group ORs in rather than replacing.
        const std::uint32_t group7 = hammer::vmf::smoothingGroupBit(7);
        require(model.applySmoothingGroups({faceA}, group7, 0u), "second group added");
        require(model.faceSmoothingGroups(faceA) == (group3 | group7),
                "group bits accumulate");

        auto groupFaces = model.facesInSmoothingGroup(3);
        require(groupFaces.size() == 2, "both faces are listed in group 3");
        require(model.facesInSmoothingGroup(7).size() == 1, "one face is listed in group 7");
        require(model.facesInSmoothingGroup(9).empty(), "an empty group lists no faces");

        // Removing clears only that bit.
        require(model.applySmoothingGroups({faceA}, 0u, group3), "face removed from a group");
        require(model.faceSmoothingGroups(faceA) == group7, "only the removed bit is cleared");

        // One undo per operation, in order.
        require(model.undo(), "group removal undone");
        require(model.faceSmoothingGroups(faceA) == (group3 | group7), "undo restored the bit");
        require(model.undo(), "second group add undone");
        require(model.faceSmoothingGroups(faceA) == group3, "undo restored the earlier mask");
        require(model.undo(), "first group add undone");
        require(model.faceSmoothingGroups(faceA) == 0u && model.faceSmoothingGroups(faceB) == 0u,
                "one undo step covered both faces");
        require(model.redo(), "group add redone");
        require(model.faceSmoothingGroups(faceB) == group3, "redo reapplied the group");

        // The mask reaches the VMF text as a plain decimal value.
        const std::string text = model.document().serialize(false);
        require(text.find("\"smoothing_groups\" \"4\"") != std::string::npos,
                "group 3 serializes as smoothing_groups 4");
    }

    // Entity IO connections (hammer/entityconnection.h, editgameclass.cpp).
    {
        hammer::vmf::EditorModel model;
        hammer::vmf::Block& entity = model.document().appendRoot("entity");
        entity.setValue("id", "9200");
        entity.setValue("classname", "logic_relay");
        entity.setValue("targetname", "relay1");
        entity.setValue("origin", "0 0 0");
        hammer::vmf::Block& wires = entity.appendChild("connections");
        wires.setValue("OnTrigger", "door1,Open,,0.5,1");
        // Modern Hammer writes the ESC (0x1B) separator when a field holds a
        // comma; the parameter here is "1,2,3".
        wires.setValue("OnSpawn", std::string("door2\033SetSpeed\0331,2,3\0330\033-1"));

        model.setSelection({{hammer::vmf::ObjectType::Entity, 9200}});
        auto connections = model.selectedConnections();
        require(connections.size() == 2, "both connections parse");
        require(connections[0].output == "OnTrigger" && connections[0].target == "door1" &&
                    connections[0].input == "Open" && connections[0].parameter.empty() &&
                    connections[0].delay == 0.5 && connections[0].timesToFire == 1,
                "the comma-delimited connection parses field for field");
        require(connections[1].parameter == "1,2,3" && connections[1].timesToFire == -1,
                "the ESC-delimited connection preserves its comma parameter");

        // Edit: retarget the first wire; write, reparse, and read it back.
        connections[0].target = "door3";
        connections[0].delay = 1.25;
        const auto properties = model.selectedProperties();
        require(model.replaceSelectedPropertiesAndConnections(properties, connections),
                "changed connections commit");
        const std::string text = model.document().serialize(false);
        hammer::vmf::ParseError parseError;
        auto reparsed = hammer::vmf::Document::parse(text, &parseError);
        require(reparsed.has_value(), "a document with connections reparses");
        hammer::vmf::EditorModel reread;
        reread.setDocument(std::move(*reparsed));
        reread.setSelection({{hammer::vmf::ObjectType::Entity, 9200}});
        const auto roundTripped = reread.selectedConnections();
        require(roundTripped == connections, "connections survive a VMF round trip");

        // One undo step reverts both keyvalues and connections together.
        require(model.undo(), "the connection edit undoes");
        require(model.selectedConnections()[0].target == "door1",
                "undo restored the original wiring");
        require(!model.replaceSelectedPropertiesAndConnections(model.selectedProperties(),
                                                               model.selectedConnections()),
                "an identical apply is not an undo step");
    }

    // Tie to Entity (CMapDoc::OnEditToEntity, Ctrl+T).
    {
        hammer::vmf::EditorModel model;
        const auto solidA = model.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0}, "BRICK/A");
        const auto solidB = model.createBlock({128.0, 0.0, 0.0}, {192.0, 64.0, 64.0}, "BRICK/A");
        require(solidA.has_value() && solidB.has_value(), "tie test solids created");
        model.setSelection({*solidA, *solidB});
        const auto tied = model.tieSelectionToEntity("func_detail");
        require(tied.has_value() && tied->type == hammer::vmf::ObjectType::Entity,
                "tying two world solids creates a brush entity");
        int entitySolids = 0;
        const hammer::vmf::Block* tiedRoot = nullptr;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name == "world") {
                require(root.children("solid").empty(), "tied solids left the world block");
            } else if (root.name == "entity" &&
                       std::atoi(root.value("id")->c_str()) == tied->id) {
                tiedRoot = &root;
                entitySolids = static_cast<int>(root.children("solid").size());
            }
        }
        require(tiedRoot && entitySolids == 2 && *tiedRoot->value("classname") == "func_detail",
                "the new entity holds both solids and the default class");
        // Re-tie: selecting the brush entity moves its solids into a fresh
        // entity and deletes the emptied one.
        model.setSelection({*tied});
        const auto retied = model.tieSelectionToEntity("func_breakable");
        require(retied.has_value() && retied->id != tied->id, "re-tying creates a new entity");
        bool oldEntityGone = true;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name == "entity" && std::atoi(root.value("id")->c_str()) == tied->id) {
                oldEntityGone = false;
            }
        }
        require(oldEntityGone, "the emptied donor brush entity is deleted");
        require(model.undo() && model.undo(), "both ties undo");
        int worldSolids = 0;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name == "world") worldSolids = static_cast<int>(root.children("solid").size());
        }
        require(worldSolids == 2, "undo returns the solids to the world");
    }

    // Displacement editing (hammer/faceedit_disppage.cpp, disppaint.cpp).
    {
        hammer::vmf::EditorModel model;
        const auto solid = model.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                             "BRICK/BRICKWALL001A");
        require(solid.has_value(), "displacement test solid created");
        const hammer::vmf::Block* block = nullptr;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name != "world") continue;
            for (const hammer::vmf::Block* child : root.children("solid")) block = child;
        }
        require(block != nullptr, "displacement test solid found");
        const auto sides = block->children("side");
        require(sides.size() == 6, "displacement test solid has six sides");
        const hammer::vmf::FaceRef faceA{solid->id, std::atoi(sides[0]->value("id")->c_str())};
        const hammer::vmf::FaceRef faceB{solid->id, std::atoi(sides[1]->value("id")->c_str())};

        require(!model.faceDisplacement(faceA).has_value(), "new sides carry no dispinfo");

        // OnButtonCreate over two faces is one undo step.
        require(model.createDisplacements({faceA, faceB}, 3), "displacements created");
        const auto created = model.faceDisplacement(faceA);
        require(created.has_value(), "the created side has a dispinfo chunk");
        require(created->power == 3, "the created displacement has the requested power");
        require(created->gridSize() == 9 && created->vertexCount() == 81,
                "a power 3 displacement is a 9x9 grid");
        require(created->elevation == 0.0 && !created->subdiv,
                "a created displacement has zero elevation and no subdivision");
        bool flat = true;
        for (int i = 0; i < created->vertexCount(); ++i) {
            flat = flat && created->distances[static_cast<std::size_t>(i)] == 0.0 &&
                   created->alphas[static_cast<std::size_t>(i)] == 0.0 &&
                   created->normals[static_cast<std::size_t>(i)].x == 0.0 &&
                   created->normals[static_cast<std::size_t>(i)].y == 0.0 &&
                   created->normals[static_cast<std::size_t>(i)].z == 0.0;
        }
        require(flat, "a created displacement is flat: zero normals, distances and alphas");
        require(!model.createDisplacements({faceA}, 3),
                "creating over an existing displacement is not an undo step");

        // The chunk survives a serialize/reparse round trip and builds a
        // displacement surface in the scene.
        {
            const std::string text = model.document().serialize(false);
            require(text.find("dispinfo") != std::string::npos, "dispinfo reaches the VMF text");
            require(text.find("\"startposition\" \"[") != std::string::npos,
                    "startposition is written in the bracketed vector form");
            ParseError roundTripError;
            auto reparsed = Document::parse(text, &roundTripError);
            require(reparsed.has_value(), "a document with displacements reparses");
            const auto scene = hammer::vmf::buildScene(*reparsed);
            int displacedFaces = 0;
            for (const auto& brush : scene.brushes) {
                for (const auto& face : brush.faces) {
                    if (!face.displacement) continue;
                    ++displacedFaces;
                    require(face.displacementPower == 3, "the rebuilt surface keeps its power");
                    require(face.displacementVertices.size() == 81,
                            "the rebuilt surface has 81 vertices");
                }
            }
            require(displacedFaces == 2, "both created displacements render");
        }

        // Paint one stroke on faceA. The sphere centre snaps to the nearest
        // displacement vertex, so any point on the surface paints.
        const auto before = model.displacementVertices(faceA);
        require(before.size() == 81, "the displacement reports its world vertices");
        hammer::vmf::SpatialPaintData paint;
        paint.center = before[40];
        paint.radius = 32.0;
        paint.scalar = 8.0;
        paint.paintAxis = {0.0, 0.0, 1.0};
        paint.brushType = hammer::vmf::PaintBrushType::Soft;
        require(model.paintDisplacements({faceA, faceB}, paint), "a paint stroke changed vertices");

        const auto painted = model.faceDisplacement(faceA);
        require(painted.has_value(), "the painted side still has a dispinfo chunk");
        int moved = 0;
        for (const double distance : painted->distances) {
            if (distance != 0.0) ++moved;
        }
        require(moved > 0, "painting moved at least one vertex off the flat surface");

        // The default spatial brush must reach past the snapped centre vertex
        // at the common 16-unit texel spacing (128-unit face at power 3);
        // otherwise every stroke edits exactly one vertex.
        {
            const std::array<hammer::vmf::Vec3, 4> corners{{{0.0, 0.0, 0.0},
                                                            {0.0, 128.0, 0.0},
                                                            {128.0, 128.0, 0.0},
                                                            {128.0, 0.0, 0.0}}};
            auto info = hammer::vmf::makeDisplacement(3, corners[0], {0.0, 0.0, 1.0});
            hammer::vmf::SpatialPaintData defaults;
            defaults.center = {64.0, 64.0, 0.0};
            defaults.paintAxis = {0.0, 0.0, 1.0};
            require(hammer::vmf::paintDisplacement(info, corners, {0.0, 0.0, 1.0}, defaults),
                    "the default paint data paints");
            int touched = 0;
            for (const double distance : info.distances) {
                if (distance != 0.0) ++touched;
            }
            require(touched > 1, "the default radius reaches neighbouring vertices");
        }

        // The paint sphere centres on the vertex nearest the ray hit. The
        // nearest-vertex search must measure every candidate against the fixed
        // hit point; measuring against the running winner walks the centre
        // away from the cursor (the "paints somewhere else" bug).
        {
            hammer::vmf::EditorModel offVertex;
            const auto paintSolid = offVertex.createBlock({0.0, 0.0, 0.0}, {128.0, 128.0, 128.0},
                                                          "BRICK/BRICKWALL001A");
            require(paintSolid.has_value(), "off-vertex paint solid created");
            const hammer::vmf::Block* paintBlock = nullptr;
            for (const hammer::vmf::Block& root : offVertex.document().roots()) {
                if (root.name != "world") continue;
                for (const hammer::vmf::Block* child : root.children("solid")) paintBlock = child;
            }
            bool tested = false;
            for (const hammer::vmf::Block* side : paintBlock->children("side")) {
                const hammer::vmf::FaceRef paintFace{
                    paintSolid->id, std::atoi(side->value("id")->c_str())};
                if (!offVertex.createDisplacements({paintFace}, 3)) continue;
                const auto flatVerts = offVertex.displacementVertices(paintFace);
                bool topFace = true;
                for (const auto& vertex : flatVerts) {
                    if (std::abs(vertex.z - 128.0) > 0.5) { topFace = false; break; }
                }
                if (!topFace) { offVertex.undo(); continue; }
                // Hit slightly off the (32, 96) grid vertex.
                hammer::vmf::SpatialPaintData paint;
                paint.center = {30.0, 94.0, 128.0};
                paint.paintAxis = {0.0, 0.0, 1.0};
                require(offVertex.paintDisplacements({paintFace}, paint), "off-vertex hit paints");
                const auto paintedInfo = offVertex.faceDisplacement(paintFace);
                std::size_t peak = 0;
                for (std::size_t i = 0; i < paintedInfo->distances.size(); ++i) {
                    if (paintedInfo->distances[i] > paintedInfo->distances[peak]) peak = i;
                }
                const auto paintedVerts = offVertex.displacementVertices(paintFace);
                const double dx = paintedVerts[peak].x - 32.0;
                const double dy = paintedVerts[peak].y - 96.0;
                require(dx * dx + dy * dy < 1.0,
                        "the paint peak lands on the grid vertex nearest the hit point");
                tested = true;
                break;
            }
            require(tested, "off-vertex paint test found the top face");
        }

        // A brush entity typically has no origin key and therefore no entity
        // marker. Dragging it must stay on the incremental scene-rebuild path
        // instead of falling back to a full rebuild on every mouse move.
        {
            hammer::vmf::EditorModel entityModel;
            const auto worldSolid = entityModel.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                                            "BRICK/BRICKWALL001A");
            require(worldSolid.has_value(), "brush entity test solid created");
            hammer::vmf::Document& doc = entityModel.document();
            hammer::vmf::Block& entityRoot = doc.appendRoot("entity");
            entityRoot.setValue("id", "9001");
            entityRoot.setValue("classname", "func_detail");
            for (hammer::vmf::Block& root : doc.roots()) {
                if (root.name != "world") continue;
                for (auto it = root.entries.begin(); it != root.entries.end(); ++it) {
                    if (it->kind == hammer::vmf::Entry::Kind::ChildBlock && it->child &&
                        it->child->name == "solid") {
                        entityRoot.entries.push_back(std::move(*it));
                        root.entries.erase(it);
                        break;
                    }
                }
            }
            hammer::vmf::Scene entityScene = hammer::vmf::buildScene(doc);
            entityModel.setSelection({{hammer::vmf::ObjectType::Entity, 9001}});
            require(entityModel.beginTransaction("Move"), "brush entity transaction opened");
            require(entityModel.translateSelectionInTransaction({8.0, 0.0, 0.0}),
                    "brush entity translated");
            hammer::vmf::rebuildSceneObjectsInPlace(doc, entityScene,
                                                    {worldSolid->id}, {9001});
            require(entityScene.baseRevision != 0,
                    "moving an origin-less brush entity stays on the incremental path");
            // Several edit steps routinely land between two paints of a view.
            // The lineage must keep a step history so a cache two revisions
            // behind can still catch up incrementally instead of forcing a
            // full rebuild per frame.
            const std::uint64_t afterFirstStep = entityScene.revision;
            require(entityScene.lineageSteps.size() == 1,
                    "the first incremental step is recorded in the lineage history");
            require(entityModel.translateSelectionInTransaction({8.0, 0.0, 0.0}),
                    "brush entity translated again");
            hammer::vmf::rebuildSceneObjectsInPlace(doc, entityScene,
                                                    {worldSolid->id}, {9001});
            require(entityScene.lineageSteps.size() == 2 &&
                        entityScene.lineageSteps[1].baseRevision == afterFirstStep &&
                        entityScene.lineageSteps[0].baseRevision != afterFirstStep,
                    "the lineage history chains consecutive incremental steps");
            entityModel.commitTransaction();
        }

        // Incremental edits only re-clip the decals/overlays a changed solid
        // can affect. A false negative here silently leaves a stale decal
        // floating in space, so the dependency test is pinned.
        {
            hammer::vmf::EditorModel decalModel;
            const auto nearSolid = decalModel.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                                          "BRICK/BRICKWALL001A");
            const auto farSolid = decalModel.createBlock({4096.0, 4096.0, 0.0},
                                                         {4160.0, 4160.0, 64.0},
                                                         "BRICK/BRICKWALL001A");
            require(nearSolid.has_value() && farSolid.has_value(), "decal test solids created");
            hammer::vmf::Block& decal = decalModel.document().appendRoot("entity");
            decal.setValue("id", "9100");
            decal.setValue("classname", "infodecal");
            decal.setValue("origin", "32 32 65"); // just above the near solid's top face
            decal.setValue("texture", "decals/decal_test");

            hammer::vmf::Scene decalScene = hammer::vmf::buildScene(decalModel.document());
            const auto resolver = [](std::string_view)
                -> std::optional<hammer::vmf::ProjectedMaterialInfo> {
                return hammer::vmf::ProjectedMaterialInfo{64, 64, 0.25};
            };
            hammer::vmf::rebuildProjectedSurfaceGeometry(decalScene, resolver);

            const hammer::vmf::EntityMarker* marker = nullptr;
            for (const auto& entity : decalScene.entities) {
                if (entity.id == 9100) marker = &entity;
            }
            require(marker && !marker->projectedSurfaces.empty(),
                    "the decal clipped onto the near solid");
            require(std::find(marker->projectedSourceSolidIds.begin(),
                              marker->projectedSourceSolidIds.end(),
                              nearSolid->id) != marker->projectedSourceSolidIds.end(),
                    "the decal records the solid it projected onto");
            require(hammer::vmf::projectedEntityDependsOnSolids(
                        decalScene, *marker, {nearSolid->id}, resolver),
                    "changing the source solid re-clips the decal");
            require(!hammer::vmf::projectedEntityDependsOnSolids(
                        decalScene, *marker, {farSolid->id}, resolver),
                    "changing a far-away solid does not re-clip the decal");
            // A solid moved into projection range must trigger a re-clip even
            // though the decal has never projected onto it.
            decalModel.setSelection({*farSolid});
            require(decalModel.translateSelection({-4096.0, -4048.0, 64.0}, "Move Near"),
                    "far solid moved next to the decal");
            hammer::vmf::rebuildSceneObjectsInPlace(decalModel.document(), decalScene,
                                                    {farSolid->id}, {});
            const hammer::vmf::EntityMarker* movedMarker = nullptr;
            for (const auto& entity : decalScene.entities) {
                if (entity.id == 9100) movedMarker = &entity;
            }
            require(movedMarker && hammer::vmf::projectedEntityDependsOnSolids(
                        decalScene, *movedMarker, {farSolid->id}, resolver),
                    "a solid moved into range re-clips the decal");
        }

        // The painted geometry round-trips through the renderer: the centre
        // vertex moved by the full soft-brush scalar.
        const auto after = model.displacementVertices(faceA);
        require(after.size() == before.size(), "the painted displacement keeps its vertex count");
        double centreDelta = 0.0;
        for (std::size_t i = 0; i < after.size(); ++i) {
            const double delta = std::abs(after[i].z - before[i].z);
            centreDelta = std::max(centreDelta, delta);
        }
        require(std::abs(centreDelta - 8.0) < 0.001,
                "the vertex under the brush centre moved by the full paint distance");

        // Undo restores the flat surface exactly, and the paint was one step.
        require(model.undo(), "paint undone");
        const auto restored = model.faceDisplacement(faceA);
        require(restored.has_value(), "undo kept the displacement itself");
        bool restoredFlat = true;
        for (const double distance : restored->distances) restoredFlat = restoredFlat && distance == 0.0;
        require(restoredFlat, "undo restored every field distance");
        require(model.redo(), "paint redone");
        require(model.faceDisplacement(faceA)->distances == painted->distances,
                "redo reapplied the exact painted distances");

        // OnButtonDestroy removes the chunk from every listed face in one step.
        require(model.destroyDisplacements({faceA, faceB}), "displacements destroyed");
        require(!model.faceDisplacement(faceA).has_value(), "destroy removed the dispinfo");
        require(!model.faceDisplacement(faceB).has_value(), "destroy covered both faces");
        require(!model.destroyDisplacements({faceA}), "destroying a plain face is not an undo step");
        require(model.document().serialize(false).find("dispinfo") == std::string::npos,
                "destroyed displacements leave no dispinfo in the VMF text");
        require(model.undo(), "destroy undone");
        require(model.faceDisplacement(faceA).has_value() &&
                model.faceDisplacement(faceB).has_value(),
                "one undo step restored both displacements");
    }

    // Displacement attributes (CFaceEditDispPage::OnButtonApply -> CMapDisp::
    // Resample / Elevate / Scale), noise and sewing (dispsew.cpp).
    {
        hammer::vmf::EditorModel model;
        const auto solid = model.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                             "BRICK/BRICKWALL001A");
        require(solid.has_value(), "attribute test solid created");
        const hammer::vmf::Block* block = nullptr;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name != "world") continue;
            for (const hammer::vmf::Block* child : root.children("solid")) block = child;
        }
        require(block != nullptr, "attribute test solid found");
        const auto sides = block->children("side");
        std::vector<hammer::vmf::FaceRef> faces;
        for (const hammer::vmf::Block* side : sides) {
            faces.push_back({solid->id, std::atoi(side->value("id")->c_str())});
        }
        require(faces.size() == 6, "attribute test solid has six sides");
        require(model.createDisplacements(faces, 3), "displacements created on every side");

        // Paint one face so the resample has something to preserve.
        const auto vertices = model.displacementVertices(faces[0]);
        require(vertices.size() == 81, "power 3 surface has 81 vertices");
        hammer::vmf::SpatialPaintData paint;
        paint.center = vertices[40];
        paint.radius = 64.0;
        paint.scalar = 12.0;
        paint.paintAxis = {0.0, 0.0, 1.0};
        require(model.paintDisplacements({faces[0]}, paint), "a stroke painted the first face");
        const auto painted = model.faceDisplacement(faces[0]);
        require(painted.has_value(), "painted face keeps its dispinfo");

        // Resample down and back up. The grid size follows the power, the edit
        // is one undo step, and the surface stays displaced.
        hammer::vmf::EditorModel::DisplacementAttributeEdit toPower2;
        toPower2.power = 2;
        require(model.applyDisplacementAttributes({faces[0]}, toPower2), "resampled to power 2");
        const auto downsampled = model.faceDisplacement(faces[0]);
        require(downsampled.has_value() && downsampled->power == 2,
                "the resampled displacement reports the new power");
        require(downsampled->gridSize() == 5 && downsampled->distances.size() == 25,
                "a power 2 displacement is a 5x5 grid");
        double peak = 0.0;
        for (const double distance : downsampled->distances) peak = std::max(peak, distance);
        require(peak > 0.0, "downsampling preserved the painted displacement");
        require(!model.applyDisplacementAttributes({faces[0]}, toPower2),
                "resampling to the power it already has is not an undo step");

        hammer::vmf::EditorModel::DisplacementAttributeEdit toPower3;
        toPower3.power = 3;
        require(model.applyDisplacementAttributes({faces[0]}, toPower3), "resampled back to power 3");
        const auto upsampled = model.faceDisplacement(faces[0]);
        require(upsampled.has_value() && upsampled->power == 3 && upsampled->distances.size() == 81,
                "the round trip is back to a 9x9 grid");
        // Corner samples survive both directions untouched (UpSample copies the
        // old vertex through, DownSample keeps every other one).
        require(upsampled->gridSize() == 9, "grid size follows the power");
        require(model.undo() && model.undo(), "both resamples undone");
        require(model.faceDisplacement(faces[0])->distances == painted->distances,
                "undo restored the painted distances exactly");
        require(model.redo() && model.redo(), "both resamples redone");

        // Elevation (CMapDisp::Elevate).
        hammer::vmf::EditorModel::DisplacementAttributeEdit elevate;
        elevate.elevation = 24.0;
        require(model.applyDisplacementAttributes(faces, elevate), "elevation applied");
        require(model.faceDisplacement(faces[0])->elevation == 24.0,
                "the elevation reached the dispinfo chunk");
        require(model.faceDisplacement(faces[3])->elevation == 24.0,
                "one undo step covered every listed face");
        require(!model.applyDisplacementAttributes(faces, elevate),
                "re-applying the same elevation is not an undo step");
        require(model.undo(), "elevation undone");
        require(model.faceDisplacement(faces[0])->elevation == 0.0, "undo restored the elevation");

        // Scale (CMapDisp::Scale) doubles the field distances.
        const auto beforeScale = model.faceDisplacement(faces[0]);
        hammer::vmf::EditorModel::DisplacementAttributeEdit scale;
        scale.scale = 2.0;
        scale.previousScale = 1.0;
        require(model.applyDisplacementAttributes({faces[0]}, scale), "scale applied");
        const auto scaled = model.faceDisplacement(faces[0]);
        bool doubled = true;
        for (std::size_t i = 0; i < scaled->distances.size(); ++i) {
            doubled = doubled && std::abs(scaled->distances[i] - beforeScale->distances[i] * 2.0) < 1e-9;
        }
        require(doubled, "scaling doubled every field distance");
        require(model.undo(), "scale undone");

        // Noise (CMapDisp::ApplyNoise). min == max is a no-op, as the original
        // returns early on it.
        require(!model.applyDisplacementNoise({faces[1]}, 4.0, 4.0),
                "noise with min == max does nothing");
        require(model.applyDisplacementNoise({faces[1]}, 2.0, 8.0), "noise applied");
        const auto noised = model.faceDisplacement(faces[1]);
        bool anyNoise = false;
        for (const double distance : noised->distances) anyNoise = anyNoise || distance != 0.0;
        require(anyNoise, "noise displaced the flat surface");
        require(model.undo(), "noise undone");

        // Sewing. The seam vertices of two neighbouring displacements end up
        // with the same field vector and distance (AverageVectorFieldData), so
        // a stroke on one face pulls its neighbours' shared edges with it.
        require(!model.sewDisplacementFaces({faces[0]}), "sewing needs more than one face");
        require(model.sewDisplacementFaces(faces), "sewing the whole solid is an undo step");
        int touched = 0;
        for (std::size_t i = 1; i < faces.size(); ++i) {
            const auto info = model.faceDisplacement(faces[i]);
            for (const double distance : info->distances) {
                if (distance != 0.0) { ++touched; break; }
            }
        }
        require(touched > 0, "sewing pulled a neighbour's seam onto the painted surface");
        require(model.undo(), "sewing undone");
        for (std::size_t i = 1; i < faces.size(); ++i) {
            const auto info = model.faceDisplacement(faces[i]);
            for (const double distance : info->distances) {
                require(distance == 0.0, "undo restored the unsewn neighbours");
            }
        }
    }

    // Lightmap scale (IDC_LIGHTMAP_SCALE / FACE_APPLY_LIGHTMAP_SCALE). The
    // lightmap-only edit leaves the material and the mapping alone.
    {
        hammer::vmf::EditorModel model;
        const auto solid = model.createBlock({0.0, 0.0, 0.0}, {64.0, 64.0, 64.0},
                                             "BRICK/BRICKWALL001A");
        require(solid.has_value(), "lightmap test solid created");
        const hammer::vmf::Block* block = nullptr;
        for (const hammer::vmf::Block& root : model.document().roots()) {
            if (root.name != "world") continue;
            for (const hammer::vmf::Block* child : root.children("solid")) block = child;
        }
        require(block != nullptr, "lightmap test solid found");
        const auto sides = block->children("side");
        const hammer::vmf::FaceRef face{solid->id, std::atoi(sides[0]->value("id")->c_str())};

        const auto original = model.faceTexture(face);
        require(original.has_value(), "the side reports a texture");
        require(original->lightmapScale == 16, "new sides default to lightmap scale 16");

        hammer::vmf::FaceTextureEdit edit;
        edit.lightmapScale = 8;
        require(model.applyFaceTextures({face}, edit, "Apply lightmap scale"),
                "the lightmap-only edit is an undo step");
        const auto changed = model.faceTexture(face);
        require(changed->lightmapScale == 8, "the lightmap scale reached the side");
        require(changed->material == original->material, "the material was left alone");
        require(changed->uScale == original->uScale && changed->vScale == original->vScale,
                "the mapping was left alone");
        require(model.document().serialize(false).find("\"lightmapscale\" \"8\"") !=
                    std::string::npos,
                "the lightmap scale reaches the VMF text");
        require(model.undoLabel() == "Apply lightmap scale", "the undo step keeps its label");
        require(model.undo() && model.faceTexture(face)->lightmapScale == 16,
                "undo restored the lightmap scale");

        // The scene carries the value so the 3D lightmap grid can draw it.
        edit.lightmapScale = 32;
        require(model.applyFaceTextures({face}, edit), "second lightmap edit applied");
        const auto scene = hammer::vmf::buildScene(model.document());
        bool found = false;
        for (const auto& brush : scene.brushes) {
            for (const auto& geometry : brush.faces) {
                if (geometry.sideId != face.sideId) continue;
                found = geometry.lightmapScale == 32;
            }
        }
        require(found, "the built scene reports the face's lightmap scale");
    }

    // Incremental scene updates (the interactive drag/paint fast path) must
    // produce exactly what a full rebuild produces.
    {
        auto sceneEqual = [](const hammer::vmf::Scene& a, const hammer::vmf::Scene& b) {
            if (a.brushes.size() != b.brushes.size() || a.entities.size() != b.entities.size())
                return false;
            if (a.hasBounds != b.hasBounds) return false;
            auto sameVec = [](const hammer::vmf::Vec3& x, const hammer::vmf::Vec3& y) {
                return std::abs(x.x - y.x) < 1e-9 && std::abs(x.y - y.y) < 1e-9 &&
                       std::abs(x.z - y.z) < 1e-9;
            };
            if (!sameVec(a.minimum, b.minimum) || !sameVec(a.maximum, b.maximum)) return false;
            for (std::size_t i = 0; i < a.brushes.size(); ++i) {
                if (a.brushes[i].id != b.brushes[i].id ||
                    a.brushes[i].ownerEntityId != b.brushes[i].ownerEntityId ||
                    a.brushes[i].vertices.size() != b.brushes[i].vertices.size() ||
                    a.brushes[i].faces.size() != b.brushes[i].faces.size()) {
                    return false;
                }
                for (std::size_t v = 0; v < a.brushes[i].vertices.size(); ++v)
                    if (!sameVec(a.brushes[i].vertices[v], b.brushes[i].vertices[v])) return false;
            }
            for (std::size_t i = 0; i < a.entities.size(); ++i) {
                if (a.entities[i].id != b.entities[i].id) return false;
                if (!sameVec(a.entities[i].origin, b.entities[i].origin)) return false;
            }
            return true;
        };

        hammer::vmf::EditorModel model(geometryMap);
        const hammer::vmf::Scene before = hammer::vmf::buildScene(model.document());

        model.setSelection({{hammer::vmf::ObjectType::Solid, 100}});
        require(model.beginTransaction("Move Objects"), "incremental drag transaction starts");
        require(model.translateSelectionInTransaction({24, -8, 16}), "incremental drag moves solid");
        require(sceneEqual(hammer::vmf::rebuildSceneObjects(model.document(), before, {100}, {}),
                           hammer::vmf::buildScene(model.document())),
                "incremental solid rebuild matches a full scene build");
        require(model.commitTransaction(), "incremental drag commits");

        const hammer::vmf::Scene moved = hammer::vmf::buildScene(model.document());
        model.setSelection({{hammer::vmf::ObjectType::Entity, 200}});
        require(model.translateSelection({8, 8, 8}), "point entity moves");
        require(sceneEqual(hammer::vmf::rebuildSceneObjects(model.document(), moved, {}, {200}),
                           hammer::vmf::buildScene(model.document())),
                "incremental entity rebuild matches a full scene build");

        // Scene revision lineage. Render backends keep their GPU buffers across
        // an interactive edit by checking that a new scene descends from the
        // one they cached and that every id it changed is one they expected.
        {
            hammer::vmf::EditorModel lineageModel(geometryMap);
            hammer::vmf::Scene scene = hammer::vmf::buildScene(lineageModel.document());
            require(scene.revision != 0, "a built scene carries a revision");
            require(scene.baseRevision == 0, "a full build has no reusable predecessor");

            const std::uint64_t built = scene.revision;
            lineageModel.setSelection({{hammer::vmf::ObjectType::Solid, 100}});
            require(lineageModel.beginTransaction("Move Objects"), "lineage transaction starts");
            require(lineageModel.translateSelectionInTransaction({16, 0, 0}), "lineage drag moves");
            hammer::vmf::rebuildSceneObjectsInPlace(lineageModel.document(), scene, {100}, {});
            require(scene.baseRevision == built, "an incremental rebuild records its predecessor");
            require(scene.revision != built, "an incremental rebuild takes a new revision");
            require(scene.changedSolidIds == std::vector<int>{100},
                    "an incremental rebuild reports exactly the changed solids");
            require(scene.changedEntityIds.empty(), "no entities changed");
            require(sceneEqual(scene, hammer::vmf::buildScene(lineageModel.document())),
                    "an in-place rebuild matches a full scene build");
            lineageModel.cancelTransaction();

            // A change the incremental path cannot express falls back to a full
            // build, which must clear the lineage so caches rebuild everything.
            const std::uint64_t stale = scene.revision;
            hammer::vmf::rebuildSceneObjectsInPlace(lineageModel.document(), scene, {999999}, {});
            require(scene.baseRevision == 0, "an unknown id clears the lineage");
            require(scene.revision != stale, "the fallback still takes a new revision");
        }

        // Selection bounds no longer build the whole scene; they must still be
        // the bounds of the selected geometry.
        model.setSelection({{hammer::vmf::ObjectType::Solid, 100}});
        const hammer::vmf::Bounds solidBounds = model.selectionBounds();
        hammer::vmf::Bounds expected;
        for (const auto& brush : hammer::vmf::buildScene(model.document()).brushes) {
            if (brush.id != 100) continue;
            for (const auto& vertex : brush.vertices) {
                if (!expected.valid) {
                    expected.minimum = expected.maximum = vertex;
                    expected.valid = true;
                    continue;
                }
                expected.minimum.x = std::min(expected.minimum.x, vertex.x);
                expected.minimum.y = std::min(expected.minimum.y, vertex.y);
                expected.minimum.z = std::min(expected.minimum.z, vertex.z);
                expected.maximum.x = std::max(expected.maximum.x, vertex.x);
                expected.maximum.y = std::max(expected.maximum.y, vertex.y);
                expected.maximum.z = std::max(expected.maximum.z, vertex.z);
            }
        }
        require(solidBounds.valid && expected.valid &&
                std::abs(solidBounds.minimum.x - expected.minimum.x) < 1e-9 &&
                std::abs(solidBounds.maximum.z - expected.maximum.z) < 1e-9,
                "selection bounds match the selected solid's geometry");

        // Undo after a fast-path drag still goes through the full rebuild and
        // must restore the pre-drag geometry exactly.
        require(model.undo(), "entity move undone");
        require(model.undo(), "drag undone");
        require(sceneEqual(hammer::vmf::buildScene(model.document()), before),
                "undo after incremental rebuilds restores the original scene");
        require(model.redo() && model.redo(), "drag and entity move redone");
        require(sceneEqual(hammer::vmf::buildScene(model.document()),
                           hammer::vmf::buildScene(model.document())),
                "redo produces a stable scene");

        // The partial scene builder sees only what it was asked for.
        const hammer::vmf::Scene partial =
            hammer::vmf::buildSceneForSolids(model.document(), {100});
        require(partial.brushes.size() == 1 && partial.brushes.front().id == 100 &&
                partial.entities.empty(),
                "buildSceneForSolids returns only the requested solids");
    }

    // --- move_rope / keyframe_rope ------------------------------------------
    {
        const char* ropeVmf =
            "world\n{\n\"id\" \"1\"\n}\n"
            "entity\n{\n\"id\" \"10\"\n\"classname\" \"move_rope\"\n"
            "\"targetname\" \"rope_start\"\n\"NextKey\" \"rope_end\"\n"
            "\"Slack\" \"200\"\n\"Width\" \"3\"\n\"Subdiv\" \"2\"\n\"Type\" \"0\"\n"
            "\"RopeMaterial\" \"cable/rope\"\n\"origin\" \"0 0 256\"\n}\n"
            "entity\n{\n\"id\" \"11\"\n\"classname\" \"keyframe_rope\"\n"
            "\"targetname\" \"rope_end\"\n\"origin\" \"256 0 256\"\n}\n";
        auto document = Document::parse(ropeVmf, &error);
        require(document.has_value(), "rope VMF parses");
        const hammer::vmf::Scene scene = hammer::vmf::buildScene(*document);
        const auto strands = hammer::vmf::buildRopeStrands(scene);
        require(strands.size() == 1, "one rope strand per NextKey link");
        const hammer::vmf::RopeStrand& strand = strands.front();
        require(strand.startEntityId == 10 && strand.endEntityId == 11,
                "strand records both of its keyframes");
        require(strand.material == "cable/rope" && std::abs(strand.width - 3.0) < 1e-9,
                "RopeMaterial and Width are read from the start keyframe");
        // Type 0 settles ROPE_MAX_SEGMENTS nodes; Subdiv 2 adds two points
        // between each pair.
        require(strand.points.size() == (10 - 1) * 3 + 1, "Subdiv tessellates the settled nodes");
        const hammer::vmf::Vec3& first = strand.points.front();
        const hammer::vmf::Vec3& last = strand.points.back();
        require(std::abs(first.x - 0.0) < 1e-6 && std::abs(first.z - 256.0) < 1e-6 &&
                std::abs(last.x - 256.0) < 1e-6 && std::abs(last.z - 256.0) < 1e-6,
                "both endpoints stay pinned to their keyframe entities");
        double lowest = first.z;
        for (const hammer::vmf::Vec3& point : strand.points) lowest = std::min(lowest, point.z);
        // Slack 200 plus ROPESLACK_FUDGEFACTOR leaves 100 units of extra rope
        // over a 256-unit span, which has to hang below the endpoints.
        require(lowest < 256.0 - 1.0, "an authored slack sags the rope below its endpoints");

        // A rope with no NextKey, or one naming an entity that does not exist,
        // is not a strand - and a self-reference must not produce one either.
        const char* danglingVmf =
            "world\n{\n\"id\" \"1\"\n}\n"
            "entity\n{\n\"id\" \"10\"\n\"classname\" \"move_rope\"\n"
            "\"targetname\" \"a\"\n\"NextKey\" \"missing\"\n\"origin\" \"0 0 0\"\n}\n"
            "entity\n{\n\"id\" \"11\"\n\"classname\" \"keyframe_rope\"\n"
            "\"targetname\" \"b\"\n\"NextKey\" \"b\"\n\"origin\" \"64 0 0\"\n}\n"
            "entity\n{\n\"id\" \"12\"\n\"classname\" \"keyframe_rope\"\n"
            "\"targetname\" \"c\"\n\"origin\" \"128 0 0\"\n}\n";
        auto danglingDocument = Document::parse(danglingVmf, &error);
        require(danglingDocument.has_value(), "dangling rope VMF parses");
        require(hammer::vmf::buildRopeStrands(hammer::vmf::buildScene(*danglingDocument)).empty(),
                "unresolvable, absent and self-referencing NextKeys build no strands");

        // A two-entity cycle is a closed loop, not an infinite walk: each link
        // is built once, from the entity that owns it.
        const char* loopVmf =
            "world\n{\n\"id\" \"1\"\n}\n"
            "entity\n{\n\"id\" \"10\"\n\"classname\" \"move_rope\"\n"
            "\"targetname\" \"a\"\n\"NextKey\" \"b\"\n\"origin\" \"0 0 0\"\n}\n"
            "entity\n{\n\"id\" \"11\"\n\"classname\" \"keyframe_rope\"\n"
            "\"targetname\" \"b\"\n\"NextKey\" \"a\"\n\"origin\" \"64 0 0\"\n}\n";
        auto loopDocument = Document::parse(loopVmf, &error);
        require(loopDocument.has_value(), "looping rope VMF parses");
        require(hammer::vmf::buildRopeStrands(hammer::vmf::buildScene(*loopDocument)).size() == 2,
                "a NextKey cycle builds one strand per link and terminates");
    }

    // Edit > Paste Special: accumulative offset and rotation, one undo step for
    // the whole run, unique names that carry the I/O connections with them.
    {
        const char* pasteVmf =
            "world\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n"
            "solid\n{\n\"id\" \"2\"\nside\n{\n\"id\" \"3\"\n"
            "\"plane\" \"(0 0 0) (0 16 0) (16 16 0)\"\n\"material\" \"TOOLS/TOOLSNODRAW\"\n}\n}\n}\n"
            "entity\n{\n\"id\" \"4\"\n\"classname\" \"logic_relay\"\n"
            "\"targetname\" \"switch\"\n\"origin\" \"0 0 0\"\n"
            "connections\n{\n\"OnTrigger\" \"lamp,TurnOn,,0,-1\"\n}\n}\n"
            "entity\n{\n\"id\" \"5\"\n\"classname\" \"light\"\n"
            "\"targetname\" \"lamp\"\n\"origin\" \"32 0 0\"\n}\n";
        auto pasteDocument = Document::parse(pasteVmf, &error);
        require(pasteDocument.has_value(), "paste-special VMF parses");
        hammer::vmf::EditorModel pasteEditor(std::move(*pasteDocument));
        pasteEditor.select({hammer::vmf::ObjectType::Solid, 2});
        pasteEditor.select({hammer::vmf::ObjectType::Entity, 4}, false, true);
        pasteEditor.select({hammer::vmf::ObjectType::Entity, 5}, false, true);
        const auto pasteClipboard = pasteEditor.copySelection();
        require(pasteClipboard.objects.size() == 3, "three objects reach the clipboard");

        const std::size_t entitiesBefore = pasteEditor.document().statistics().entities;
        const std::size_t solidsBefore = pasteEditor.document().statistics().solids;

        hammer::vmf::PasteSpecialOptions pasteOptions;
        pasteOptions.copies = 2;
        pasteOptions.offset = {0.0, 0.0, 8.0};
        pasteOptions.rotation = {0.0, 0.0, 90.0};
        pasteOptions.groupCopies = true;
        pasteOptions.uniqueEntityNames = true;
        pasteOptions.namePrefix = "west_";
        require(pasteEditor.pasteSpecial(pasteClipboard, pasteOptions), "paste special pastes");
        require(pasteEditor.selection().size() == 6, "two copies of three objects are selected");
        require(pasteEditor.document().statistics().entities == entitiesBefore + 4,
                "both copies of both entities are in the document");
        require(pasteEditor.document().statistics().solids == solidsBefore + 2,
                "both copies of the solid are in the document");

        // Accumulative offset: copy 2 sits twice as far up as copy 1.
        std::vector<double> pastedHeights;
        std::vector<std::string> pastedNames;
        for (const Block& root : pasteEditor.document().roots()) {
            if (root.name != "entity") continue;
            const std::string* name = root.value("targetname");
            const std::string* origin = root.value("origin");
            if (!name || !origin) continue;
            if (name->rfind("west_", 0) != 0) continue;
            pastedNames.push_back(*name);
            double values[3] = {0.0, 0.0, 0.0};
            std::sscanf(origin->c_str(), "%lf %lf %lf", &values[0], &values[1], &values[2]);
            pastedHeights.push_back(values[2]);
        }
        require(pastedNames.size() == 4, "every pasted entity was prefixed");
        require(std::count(pastedHeights.begin(), pastedHeights.end(), 8.0) == 2 &&
                std::count(pastedHeights.begin(), pastedHeights.end(), 16.0) == 2,
                "offsets accumulate: copy 1 rises 8, copy 2 rises 16");
        require(std::set<std::string>(pastedNames.begin(), pastedNames.end()).size() == 4,
                "every pasted entity name is unique");

        // Copy 1's relay must trigger copy 1's lamp, not the original's.
        for (const Block& root : pasteEditor.document().roots()) {
            const std::string* name = root.value("targetname");
            if (!name || name->rfind("west_switch", 0) != 0) continue;
            const std::vector<const Block*> connections = root.children("connections");
            require(connections.size() == 1, "the pasted relay keeps its connections");
            const std::string* output = connections.front()->value("OnTrigger");
            require(output && output->rfind("west_lamp", 0) == 0,
                    "connections follow the renamed entity into the copy");
            require(output && output->rfind("lamp,", 0) != 0,
                    "connections no longer point at the original entity");
        }

        // Group copies: every pasted object carries the same new group id.
        const Block* pastedWorld = pasteEditor.document().firstRoot("world");
        require(pastedWorld && pastedWorld->children("group").size() == 1,
                "grouping the copies writes one group block");
        const std::string* groupId = pastedWorld->children("group").front()->value("id");
        require(groupId != nullptr, "the new group has an id");
        int groupedObjects = 0;
        const auto countGrouped = [&](const Block& block) {
            for (const Block* editor : block.children("editor")) {
                const std::string* id = editor->value("groupid");
                if (id && *id == *groupId) ++groupedObjects;
            }
        };
        for (const Block& root : pasteEditor.document().roots()) {
            countGrouped(root);
            for (const Block* solid : root.children("solid")) countGrouped(*solid);
        }
        require(groupedObjects == 6, "every pasted object joins the new group");

        // The whole run is one undo step.
        require(pasteEditor.undo(), "paste special can be undone");
        require(pasteEditor.document().statistics().entities == entitiesBefore &&
                pasteEditor.document().statistics().solids == solidsBefore,
                "one undo removes every copy");
        require(!pasteEditor.canUndo(), "paste special pushed exactly one undo entry");
    }

    // Map > Entity Gallery batches its entities into one undo step and leaves
    // them all selected.
    {
        hammer::vmf::EditorModel galleryEditor;
        const std::size_t entitiesBefore = galleryEditor.document().statistics().entities;
        std::vector<hammer::vmf::EditorModel::PointEntitySpec> specs{
            {"info_target", {0.0, 0.0, 0.0}, {}},
            {"light", {64.0, 0.0, 0.0}, {{"_light", "255 255 255 200"}}},
            {"", {128.0, 0.0, 0.0}, {}},  // classless entries are skipped
        };
        require(galleryEditor.createPointEntities(specs, "Entity Gallery") == 2,
                "the gallery creates one entity per named class");
        require(galleryEditor.document().statistics().entities == entitiesBefore + 2,
                "the created entities are in the document");
        require(galleryEditor.selection().size() == 2, "the gallery leaves its entities selected");
        const Block* galleryLight = nullptr;
        for (const Block& root : galleryEditor.document().roots()) {
            const std::string* classname = root.value("classname");
            if (classname && *classname == "light") galleryLight = &root;
        }
        require(galleryLight != nullptr, "the gallery writes each entity's classname");
        require(galleryLight && galleryLight->value("_light") &&
                *galleryLight->value("_light") == "255 255 255 200",
                "the gallery applies each class's game-data defaults");
        require(galleryLight && galleryLight->value("origin") &&
                *galleryLight->value("origin") == "64 0 0",
                "each gallery entity lands at its own cell");
        require(galleryEditor.undo(), "the gallery can be undone");
        require(galleryEditor.document().statistics().entities == entitiesBefore,
                "one undo removes the whole gallery");
        require(!galleryEditor.canUndo(), "the gallery is a single undo step");
    }

    // Newly created brushes are World-aligned on EVERY face (Hammer's
    // CMapFace::InitializeTextureAxes with TEXTURE_ALIGN_WORLD): the texture
    // axes come from the dominant world axis of the face normal.
    {
        using hammer::vmf::EditorModel;
        using hammer::vmf::Vec3;
        EditorModel aligned;
        require(aligned.createBlock({0.0, 0.0, 0.0}, {128.0, 96.0, 64.0},
                                    "brick/brickwall001a")
                    .has_value(),
                "world-aligned block creates");
        // A spike's slanted faces are the case a fixed axis pair can never get
        // right, so the invariant below has to hold for them too.
        require(aligned
                    .createPrimitive(EditorModel::PrimitiveKind::Spike, {256.0, 0.0, 0.0},
                                     {384.0, 128.0, 128.0}, 2, 6)
                    .has_value(),
                "world-aligned spike creates");

        const auto dot = [](const Vec3& a, const Vec3& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        const auto unit = [&](const Vec3& v) {
            const double length = std::sqrt(dot(v, v));
            return length > 0.0 ? Vec3{v.x / length, v.y / length, v.z / length} : v;
        };

        const hammer::vmf::Scene scene = hammer::vmf::buildScene(aligned.document());
        std::size_t faces = 0;
        std::size_t axisAlignedChecked = 0;
        for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
            for (const hammer::vmf::FaceGeometry& face : brush.faces) {
                const Vec3 normal = unit(face.normal);
                // World alignment picks the axes perpendicular to the normal's
                // DOMINANT axis, so no texture axis can ever run more than 45
                // degrees toward the normal - let alone parallel to it, which
                // is the degenerate projection the old fixed pair produced on
                // four of a block's six faces.
                require(std::abs(dot(normal, face.uAxis.direction)) < 0.7072,
                        "the U axis is never parallel to the face normal");
                require(std::abs(dot(normal, face.vAxis.direction)) < 0.7072,
                        "the V axis is never parallel to the face normal");

                // The block's six faces are axis aligned, so their world axes
                // are exactly Hammer's.
                const auto axisIs = [&](const Vec3& axis, double x, double y, double z) {
                    return std::abs(axis.x - x) < 1e-9 && std::abs(axis.y - y) < 1e-9 &&
                           std::abs(axis.z - z) < 1e-9;
                };
                if (std::abs(normal.z) > 0.999) {
                    require(axisIs(face.uAxis.direction, 1.0, 0.0, 0.0) &&
                                axisIs(face.vAxis.direction, 0.0, -1.0, 0.0),
                            "floor/ceiling faces use the world X/-Y axes");
                    ++axisAlignedChecked;
                } else if (std::abs(normal.x) > 0.999) {
                    require(axisIs(face.uAxis.direction, 0.0, 1.0, 0.0) &&
                                axisIs(face.vAxis.direction, 0.0, 0.0, -1.0),
                            "east/west faces use the world Y/-Z axes");
                    ++axisAlignedChecked;
                } else if (std::abs(normal.y) > 0.999) {
                    require(axisIs(face.uAxis.direction, 1.0, 0.0, 0.0) &&
                                axisIs(face.vAxis.direction, 0.0, 0.0, -1.0),
                            "north/south faces use the world X/-Z axes");
                    ++axisAlignedChecked;
                }
                ++faces;
            }
        }
        require(faces >= 13, "block and spike faces were all inspected");
        require(axisAlignedChecked >= 6, "the block contributed six axis-aligned faces");
    }

    auto malformed = Document::parse("world\n{\n\"id\" \"1\"\n", &error);
    require(!malformed.has_value(), "unterminated VMF is rejected");
    require(error.line >= 3, "parse errors include a useful line number");

    std::cout << "VMF document read/write tests passed\n";
    return EXIT_SUCCESS;
}
