#include "VmfSolidMorph.hpp"

#include "VmfDisplacement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hammer::vmf {
namespace {

Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 multiply(const Vec3& value, double scalar) { return {value.x * scalar, value.y * scalar, value.z * scalar}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(const Vec3& value) { return std::sqrt(dot(value, value)); }

constexpr double MovedEpsilon = 0.0001;

bool samePoint(const Vec3& a, const Vec3& b)
{
    return std::abs(a.x - b.x) < MovedEpsilon && std::abs(a.y - b.y) < MovedEpsilon &&
           std::abs(a.z - b.z) < MovedEpsilon;
}

// Newell's method over the face loop. A dragged vertex can tilt the face, so
// the plane has to come from the moved points rather than the stored normal;
// the sign is taken from the face's original normal so the plane keeps
// pointing out of the solid (a VMF side normal always does).
Vec3 polygonNormal(const std::vector<Vec3>& points, const Vec3& reference)
{
    Vec3 normal{};
    const std::size_t count = points.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3& current = points[i];
        const Vec3& next = points[(i + 1) % count];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }
    const double magnitude = length(normal);
    if (magnitude < 1e-9) return {};
    normal = multiply(normal, 1.0 / magnitude);
    if (dot(normal, reference) < 0.0) normal = multiply(normal, -1.0);
    return normal;
}

// Three well separated points of the loop, in loop order, so CMapFace::CalcPlane
// reproduces the given normal instead of being spoiled by nearly collinear
// neighbours. Same construction as VmfSolidClip's choosePlanePoints, but
// validated against the RE-DERIVED normal.
bool choosePlanePoints(const std::vector<Vec3>& points, const Vec3& normal, Vec3& a, Vec3& b, Vec3& c)
{
    const std::size_t count = points.size();
    if (count < 3) return false;
    double bestMagnitude = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t j = (i + std::max<std::size_t>(1, count / 3)) % count;
        const std::size_t k = (i + std::max<std::size_t>(2, (2 * count) / 3)) % count;
        if (i == j || j == k || i == k) continue;
        const Vec3 candidate = cross(subtract(points[i], points[j]), subtract(points[k], points[j]));
        const double magnitude = length(candidate);
        if (magnitude > bestMagnitude && dot(candidate, normal) > 0.0) {
            bestMagnitude = magnitude;
            a = points[i];
            b = points[j];
            c = points[k];
        }
    }
    return bestMagnitude > 1e-4;
}

std::string formatMorphNumber(double value)
{
    const double rounded = std::round(value);
    if (std::abs(value - rounded) < 1e-6) return std::to_string(static_cast<long long>(rounded));
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

bool parseBracketVector(const std::string& text, Vec3& result)
{
    return std::sscanf(text.c_str(), " [ %lf %lf %lf ]", &result.x, &result.y, &result.z) == 3;
}

} // namespace

Vec3 MorphSolid::edgeCenter(std::size_t edge) const
{
    if (edge >= edges.size()) return {};
    const Vec3& first = vertices[edges[edge].first];
    const Vec3& second = vertices[edges[edge].second];
    return {(first.x + second.x) * 0.5, (first.y + second.y) * 0.5, (first.z + second.z) * 0.5};
}

bool MorphSolid::moved() const
{
    if (vertices.size() != originalVertices.size()) return false;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (!samePoint(vertices[i], originalVertices[i])) return true;
    }
    for (const MorphDispFace& disp : dispFaces) {
        if (disp.positions.size() != disp.originalPositions.size()) continue;
        for (std::size_t i = 0; i < disp.positions.size(); ++i) {
            if (!samePoint(disp.positions[i], disp.originalPositions[i])) return true;
        }
    }
    return false;
}

MorphSolid buildMorphSolid(const BrushGeometry& brush)
{
    MorphSolid solid;
    solid.solidId = brush.id;
    solid.vertices = brush.vertices;
    solid.originalVertices = brush.vertices;
    solid.edges = brush.edges;
    solid.faces.reserve(brush.faces.size());
    for (const FaceGeometry& face : brush.faces) {
        MorphFace morphFace;
        morphFace.sideId = face.sideId;
        morphFace.normal = face.normal;
        morphFace.vertices = face.vertices;
        solid.faces.push_back(std::move(morphFace));
        if (face.displacement && face.displacementPower >= 1 &&
            face.displacementVertices.size() ==
                static_cast<std::size_t>(displacementVertexCount(face.displacementPower))) {
            MorphDispFace disp;
            disp.sideId = face.sideId;
            disp.power = face.displacementPower;
            disp.positions.reserve(face.displacementVertices.size());
            for (const auto& vertex : face.displacementVertices)
                disp.positions.push_back(vertex.position);
            disp.originalPositions = disp.positions;
            solid.dispFaces.push_back(std::move(disp));
        }
    }
    return solid;
}

std::vector<MorphSolid> buildMorphSolids(const Scene& scene, const std::vector<ObjectRef>& selection)
{
    // Morph3D::OnActivate puts every selected solid (and every solid child of a
    // selected entity) into morph mode.
    std::vector<MorphSolid> solids;
    for (const BrushGeometry& brush : scene.brushes) {
        const bool selected =
            std::any_of(selection.begin(), selection.end(), [&](const ObjectRef& object) {
                if (object.type == ObjectType::Solid) return object.id == brush.id;
                return object.id == brush.ownerEntityId;
            });
        if (!selected) continue;
        if (brush.vertices.size() < 4 || brush.faces.size() < 4) continue;
        solids.push_back(buildMorphSolid(brush));
    }
    return solids;
}

std::vector<MorphSolid> buildMorphSolidsById(const Scene& scene, const std::vector<int>& solidIds)
{
    std::vector<MorphSolid> solids;
    for (int id : solidIds) {
        const auto brush = std::find_if(scene.brushes.begin(), scene.brushes.end(),
                                        [&](const BrushGeometry& candidate) { return candidate.id == id; });
        if (brush == scene.brushes.end()) continue;
        if (brush->vertices.size() < 4 || brush->faces.size() < 4) continue;
        solids.push_back(buildMorphSolid(*brush));
    }
    return solids;
}

MorphHandleMode nextMorphHandleMode(MorphHandleMode mode)
{
    // Morph3D::ToggleMode: both -> vertex -> edge -> both.
    switch (mode) {
    case MorphHandleMode::VerticesAndEdges: return MorphHandleMode::Vertices;
    case MorphHandleMode::Vertices: return MorphHandleMode::Edges;
    case MorphHandleMode::Edges: return MorphHandleMode::VerticesAndEdges;
    }
    return MorphHandleMode::VerticesAndEdges;
}

std::vector<MorphHandle> morphHandles(const std::vector<MorphSolid>& solids, MorphHandleMode mode,
                                      const std::vector<MorphHandleRef>& selected,
                                      std::vector<MorphHandleRef>* refs)
{
    const bool showVertices = mode != MorphHandleMode::Edges;
    const bool showEdges = mode != MorphHandleMode::Vertices;
    std::vector<MorphHandle> handles;
    if (refs) refs->clear();
    const auto isSelected = [&](const MorphHandleRef& ref) {
        return std::find(selected.begin(), selected.end(), ref) != selected.end();
    };
    for (std::size_t s = 0; s < solids.size(); ++s) {
        const MorphSolid& solid = solids[s];
        if (showVertices) {
            for (std::size_t i = 0; i < solid.vertices.size(); ++i) {
                const MorphHandleRef ref{s, i, false};
                handles.push_back({solid.vertices[i], false, isSelected(ref)});
                if (refs) refs->push_back(ref);
            }
        }
        if (showEdges) {
            for (std::size_t i = 0; i < solid.edges.size(); ++i) {
                const MorphHandleRef ref{s, i, true};
                handles.push_back({solid.edgeCenter(i), true, isSelected(ref)});
                if (refs) refs->push_back(ref);
            }
        }
        if (showVertices) {
            for (std::size_t d = 0; d < solid.dispFaces.size(); ++d) {
                const MorphDispFace& disp = solid.dispFaces[d];
                for (std::size_t i = 0; i < disp.positions.size(); ++i) {
                    const MorphHandleRef ref{s, i, false, static_cast<int>(d)};
                    handles.push_back({disp.positions[i], false, isSelected(ref), true});
                    if (refs) refs->push_back(ref);
                }
            }
        }
    }
    return handles;
}

std::vector<FacePolygons> morphFacePolygons(const std::vector<MorphSolid>& solids)
{
    std::vector<FacePolygons> result;
    result.reserve(solids.size());
    for (const MorphSolid& solid : solids) {
        FacePolygons polygons;
        for (const MorphFace& face : solid.faces) {
            std::vector<Vec3> points;
            points.reserve(face.vertices.size());
            for (std::size_t index : face.vertices) {
                if (index < solid.vertices.size()) points.push_back(solid.vertices[index]);
            }
            if (points.size() >= 3) polygons.push_back(std::move(points));
        }
        result.push_back(std::move(polygons));
    }
    return result;
}

std::vector<MorphDispGrid> morphDispGrids(const std::vector<MorphSolid>& solids)
{
    std::vector<MorphDispGrid> grids;
    for (const MorphSolid& solid : solids) {
        for (const MorphDispFace& disp : solid.dispFaces) {
            grids.push_back({disp.power, disp.positions});
        }
    }
    return grids;
}

void moveMorphHandles(MorphSolid& solid, const std::vector<std::size_t>& vertexHandles,
                      const std::vector<std::size_t>& edgeHandles, const Vec3& delta)
{
    std::vector<bool> touched(solid.vertices.size(), false);
    const auto touch = [&](std::size_t index) {
        if (index < touched.size()) touched[index] = true;
    };
    for (std::size_t index : vertexHandles) touch(index);
    for (std::size_t index : edgeHandles) {
        if (index >= solid.edges.size()) continue;
        touch(solid.edges[index].first);
        touch(solid.edges[index].second);
    }
    for (std::size_t i = 0; i < solid.vertices.size(); ++i) {
        if (!touched[i]) continue;
        solid.vertices[i].x += delta.x;
        solid.vertices[i].y += delta.y;
        solid.vertices[i].z += delta.z;
    }
}

void moveMorphDispHandles(MorphSolid& solid, std::size_t dispFace,
                          const std::vector<std::size_t>& vertexIndices, const Vec3& delta)
{
    if (dispFace >= solid.dispFaces.size()) return;
    MorphDispFace& disp = solid.dispFaces[dispFace];
    std::vector<bool> touched(disp.positions.size(), false);
    for (std::size_t index : vertexIndices) {
        if (index < touched.size()) touched[index] = true;
    }
    for (std::size_t i = 0; i < disp.positions.size(); ++i) {
        if (!touched[i]) continue;
        disp.positions[i].x += delta.x;
        disp.positions[i].y += delta.y;
        disp.positions[i].z += delta.z;
    }
}

std::optional<Block> morphSolid(const Block& solid, const MorphSolid& morph)
{
    if (!morph.moved()) return std::nullopt;

    Block result = solid;
    int rewritten = 0;
    for (Block* side : result.children("side")) {
        const std::string* idText = side->value("id");
        if (!idText) continue;
        const int sideId = std::atoi(idText->c_str());
        const auto face = std::find_if(morph.faces.begin(), morph.faces.end(),
                                       [&](const MorphFace& candidate) { return candidate.sideId == sideId; });
        if (face == morph.faces.end() || face->vertices.size() < 3) continue;

        // Leave untouched faces exactly as they were: their half space did not
        // change, and rewriting the plane text would needlessly churn the file.
        bool faceMoved = false;
        std::vector<Vec3> points;
        points.reserve(face->vertices.size());
        for (std::size_t index : face->vertices) {
            if (index >= morph.vertices.size()) return std::nullopt;
            points.push_back(morph.vertices[index]);
            if (!samePoint(morph.vertices[index], morph.originalVertices[index])) faceMoved = true;
        }
        if (faceMoved) {
            const Vec3 normal = polygonNormal(points, face->normal);
            if (length(normal) < 0.5) return std::nullopt;
            Vec3 a{};
            Vec3 b{};
            Vec3 c{};
            if (!choosePlanePoints(points, normal, a, b, c)) return std::nullopt;
            side->setValue("plane", planePointsText(a, b, c));

            // A displacement rides on its face: keep dispinfo's startposition
            // glued to the corner it named, so the resampled displacement keeps
            // its orientation instead of snapping to whichever corner is now
            // nearest.
            for (Block* disp : side->children("dispinfo")) {
                const std::string* startText = disp->value("startposition");
                if (!startText) continue;
                Vec3 start{};
                if (!parseBracketVector(*startText, start)) continue;
                for (std::size_t index : face->vertices) {
                    const Vec3& original = morph.originalVertices[index];
                    if (std::abs(original.x - start.x) > 1.0 ||
                        std::abs(original.y - start.y) > 1.0 ||
                        std::abs(original.z - start.z) > 1.0) {
                        continue;
                    }
                    const Vec3& moved = morph.vertices[index];
                    disp->setValue("startposition",
                                   "[" + formatMorphNumber(moved.x) + " " + formatMorphNumber(moved.y) +
                                       " " + formatMorphNumber(moved.z) + "]");
                    break;
                }
            }
            ++rewritten;
        }

        // Displacement grid vertices dragged individually: their deltas become
        // changes to the dispinfo "offsets" rows, which is the term of the
        // vertex position equation that moves a vertex verbatim.
        const auto dispFace = std::find_if(morph.dispFaces.begin(), morph.dispFaces.end(),
                                           [&](const MorphDispFace& candidate) {
                                               return candidate.sideId == sideId;
                                           });
        if (dispFace != morph.dispFaces.end() &&
            dispFace->positions.size() == dispFace->originalPositions.size()) {
            bool dispMoved = false;
            for (std::size_t i = 0; i < dispFace->positions.size(); ++i) {
                if (!samePoint(dispFace->positions[i], dispFace->originalPositions[i])) {
                    dispMoved = true;
                    break;
                }
            }
            if (dispMoved) {
                std::optional<DisplacementInfo> info = readDisplacement(*side);
                // A sparse dispinfo (rows omitted, as the scene builder
                // tolerates) still edits: rebuild it as a flat displacement of
                // the same power and start position first.
                if (!info) {
                    for (const Block* disp : side->children("dispinfo")) {
                        const std::string* powerText = disp->value("power");
                        const std::string* startText = disp->value("startposition");
                        Vec3 start{};
                        if (!powerText || !startText || !parseBracketVector(*startText, start))
                            continue;
                        const int power = std::atoi(powerText->c_str());
                        if (displacementVertexCount(power) !=
                            static_cast<int>(dispFace->positions.size()))
                            continue;
                        info = makeDisplacement(power, start,
                                                face != morph.faces.end() ? face->normal : Vec3{0, 0, 1});
                        break;
                    }
                }
                if (info &&
                    info->offsets.size() == dispFace->positions.size()) {
                    for (std::size_t i = 0; i < dispFace->positions.size(); ++i) {
                        info->offsets[i].x += dispFace->positions[i].x - dispFace->originalPositions[i].x;
                        info->offsets[i].y += dispFace->positions[i].y - dispFace->originalPositions[i].y;
                        info->offsets[i].z += dispFace->positions[i].z - dispFace->originalPositions[i].z;
                    }
                    writeDisplacement(*side, *info);
                    ++rewritten;
                }
            }
        }
    }

    if (rewritten == 0) return std::nullopt;

    // Morph3D::FinishTranslation's validity pass: a solid that no longer has
    // four contributing faces (the user collapsed it) is thrown away rather
    // than committed.
    if (solidFacePolygons(result).size() < 4) return std::nullopt;
    return result;
}

} // namespace hammer::vmf
