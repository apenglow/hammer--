#pragma once

#include "VmfScene.hpp"

#include <functional>
#include <unordered_set>
#include <optional>
#include <string_view>

namespace hammer::vmf {

struct ProjectedMaterialInfo
{
    int width{0};
    int height{0};
    double decalScale{1.0};
};

using ProjectedMaterialResolver =
    std::function<std::optional<ProjectedMaterialInfo>(std::string_view)>;

// Rebuilds infodecal and info_overlay render geometry from the current VMF
// entity data and brush/displacement surfaces. The result is stored directly on
// each EntityMarker so rendering, bounds, 2D display, and picking all consume
// the same projected triangles.
void rebuildProjectedSurfaceGeometry(Scene& scene,
                                     const ProjectedMaterialResolver& materialResolver);

// Rebuilds the projected geometry of ONE entity (no-op for classnames that are
// not infodecal/info_overlay).
void rebuildEntityProjectedSurfaces(Scene& scene, EntityMarker& entity,
                                    const ProjectedMaterialResolver& materialResolver);

// True when changing exactly the given solids could alter this entity's
// projected geometry: the entity currently projects onto one of them, or one
// of them moved into the entity's projection range. Conservative (may say
// true unnecessarily) but never false when a re-clip would differ.
bool projectedEntityDependsOnSolids(const Scene& scene, const EntityMarker& entity,
                                    const std::unordered_set<int>& changedSolidIds,
                                    const ProjectedMaterialResolver& materialResolver);

} // namespace hammer::vmf
