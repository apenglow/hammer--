#pragma once

#include "GameFileSystem.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hammer::assets {

struct StudioAnimationData;

// Row-major affine 3x4 matrix used by the renderer's GPU bone palette.
// Keeping this Qt/OpenGL-independent lets the portable asset tests validate
// the exact matrices that the viewport uploads.
struct StudioBoneMatrix
{
    std::array<float, 12> values{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };
};

struct StudioVertex
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float nx{0.0f};
    float ny{0.0f};
    float nz{1.0f};
    float u{0.0f};
    float v{0.0f};
    // VVD stores one authored tangent frame per source vertex. Preserving it
    // avoids triangle-by-triangle tangent reconstruction seams on props.
    float tx{1.0f};
    float ty{0.0f};
    float tz{0.0f};
    float tangentSign{1.0f};
    bool hasTangent{false};

    // Unskinned VVD source data and authored bone weights are retained so the
    // renderer can evaluate arbitrary MDL sequences without re-reading the
    // model files. Source vertices support up to three bone influences.
    std::array<float, 3> sourcePosition{0.0f, 0.0f, 0.0f};
    std::array<float, 3> sourceNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> sourceTangent{1.0f, 0.0f, 0.0f};
    std::array<float, 3> boneWeights{0.0f, 0.0f, 0.0f};
    std::array<std::uint8_t, 3> boneIndices{0u, 0u, 0u};
    std::uint8_t influenceCount{0u};
};

struct StudioMesh
{
    // mstudiomesh_t::material is a column in the MDL skin-family table.
    // Keep that authored slot so each prop entity can select its own family.
    int materialSlot{0};
    std::string material; // resolved family-zero material for compatibility/debugging
    std::vector<StudioVertex> vertices; // expanded triangle list
};


struct StudioSequence
{
    std::string label;
    std::string activityName;
    float fps{0.0f};
    int frameCount{1};
    float duration{0.0f};
    bool looping{false};
    bool delta{false};
    std::array<float, 3> minimum{0.0f, 0.0f, 0.0f};
    std::array<float, 3> maximum{0.0f, 0.0f, 0.0f};
};

struct StudioModel
{
    std::string name;
    std::vector<StudioMesh> meshes;
    // [skin family][skin reference/material slot] -> normalized VMT path.
    std::vector<std::vector<std::string>> skinFamilies;
    std::vector<StudioSequence> sequences;
    std::shared_ptr<const StudioAnimationData> animationData;
    // Skin matrices for the cached reference mesh. Animated rendering uploads
    // these when no sequence is selected, avoiding a second static vertex copy.
    std::vector<StudioBoneMatrix> referencePoseMatrices;
    std::array<float, 3> minimum{0.0f, 0.0f, 0.0f};
    std::array<float, 3> maximum{0.0f, 0.0f, 0.0f};
    bool valid{false};
    std::string error;

    int sequenceCount() const
    {
        return static_cast<int>(sequences.size());
    }

    int normalizedSequence(int requested) const
    {
        return requested >= 0 && requested < sequenceCount() ? requested : 0;
    }

    int sequenceIndex(std::string_view labelOrActivity) const;

    int skinCount() const
    {
        return std::max(1, static_cast<int>(skinFamilies.size()));
    }

    int normalizedSkin(int requested) const
    {
        return requested >= 0 && requested < skinCount() ? requested : 0;
    }

    std::string materialForSkin(int materialSlot, int requestedSkin) const
    {
        if (!skinFamilies.empty()) {
            const auto& family = skinFamilies[static_cast<std::size_t>(normalizedSkin(requestedSkin))];
            if (materialSlot >= 0 && materialSlot < static_cast<int>(family.size()))
                return family[static_cast<std::size_t>(materialSlot)];
        }
        for (const StudioMesh& mesh : meshes) {
            if (mesh.materialSlot == materialSlot && !mesh.material.empty()) return mesh.material;
        }
        return {};
    }
};

// Source 1 studio-model reader used by FGD point helpers and animated prop previews. It reads
// .mdl + .vvd + .dx90.vtx (with normal VTX fallbacks), composes the default
// MDL bone hierarchy, combines bone-to-pose with the embedded pose-to-bone
// inverse bind matrices, and supports studiohdr2 linear-bone arrays. This is
// It evaluates local inline/external animation blocks without the engine MDL cache.
class StudioModelSystem
{
public:
    explicit StudioModelSystem(std::shared_ptr<GameFileSystem> fileSystem);

    std::shared_ptr<const StudioModel> model(std::string_view path);

    // Samples one sequence into mesh-local triangle lists. The output preserves
    // UVs, tangent handedness, material slots, and all source skinning data.
    // cycle is normalized to [0,1]; looping/clamping follows the sequence flags.
    bool sampleAnimation(const StudioModel& model, int sequence, double cycle,
                         std::vector<std::vector<StudioVertex>>& output) const;

    // Evaluates only the bone palette. The hardware viewport uses this path so
    // vertex positions, normals, and tangents are skinned in the vertex shader.
    bool sampleAnimationMatrices(const StudioModel& model, int sequence, double cycle,
                                 std::vector<StudioBoneMatrix>& output) const;

    void clearCache();

private:
    std::shared_ptr<StudioModel> load(std::string normalizedPath) const;

    std::shared_ptr<GameFileSystem> fileSystem_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<StudioModel>> cache_;
};

} // namespace hammer::assets
