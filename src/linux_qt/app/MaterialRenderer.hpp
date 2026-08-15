#pragma once

#include "Camera3D.hpp"
#include "MaterialSystem.hpp"
#include "VmfScene.hpp"

#include <QImage>

namespace HammerMaterialRenderer {

void render(QImage& target, const hammer::vmf::Scene& scene,
            const hammer::camera::State& camera,
            hammer::camera::ProjectionMode projection,
            double logicalWidth, double logicalHeight,
            hammer::assets::MaterialSystem& materials,
            bool displacementSolidMask = true);

} // namespace HammerMaterialRenderer
