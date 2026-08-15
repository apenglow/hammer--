#pragma once

#include "MaterialSystem.hpp"
#include "VmfScene.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace hammer::render {

// One env_cubemap entity: where the probe sits and how large a face it asked
// for. Populated from the map before a bake; the renderer never reads the VMF.
struct CubemapProbe
{
    hammer::vmf::Vec3 origin;
    int size{32};
    int entityId{-1};
};

// A baked probe: the six rendered faces plus the position they were rendered
// from, which is what the nearest-probe lookup needs at draw time.
struct BakedCubemap
{
    hammer::vmf::Vec3 origin;
    hammer::assets::CubeImage cube;
};

// Source's env_cubemap "cubemapsize" keyvalue is a choices index, not a pixel
// count: 0 means the 32x32 default and any other value n means an edge of
// 2^(n-1), so 1 -> 1x1 up to 9 -> 256x256.
inline int cubemapFaceSizeFromKeyValue(int cubemapSize)
{
    if (cubemapSize <= 0) return 32;
    return 1 << std::clamp(cubemapSize - 1, 0, 8);
}

inline std::vector<CubemapProbe> collectCubemapProbes(const hammer::vmf::Scene& scene)
{
    std::vector<CubemapProbe> probes;
    for (const hammer::vmf::EntityMarker& entity : scene.entities) {
        if (entity.classname != "env_cubemap") continue;
        CubemapProbe probe;
        probe.origin = entity.origin;
        probe.entityId = entity.id;
        for (const auto& property : entity.properties) {
            if (property.first != "cubemapsize") continue;
            probe.size = cubemapFaceSizeFromKeyValue(std::atoi(property.second.c_str()));
            break;
        }
        probes.push_back(probe);
    }
    return probes;
}

// Which baked probe a surface point reflects. Source assigns this per face at
// compile time by nearest probe origin; the editor preview does the same.
// Returns -1 when nothing has been baked.
inline int nearestCubemapIndex(const std::vector<BakedCubemap>& cubemaps,
                               const hammer::vmf::Vec3& point)
{
    int nearest = -1;
    double nearestDistance = 0.0;
    for (std::size_t index = 0; index < cubemaps.size(); ++index) {
        const hammer::vmf::Vec3& origin = cubemaps[index].origin;
        const double dx = origin.x - point.x;
        const double dy = origin.y - point.y;
        const double dz = origin.z - point.z;
        const double distance = dx * dx + dy * dy + dz * dz;
        if (nearest < 0 || distance < nearestDistance) {
            nearest = static_cast<int>(index);
            nearestDistance = distance;
        }
    }
    return nearest;
}

} // namespace hammer::render
