#pragma once

#include "VmfScene.hpp"

#include <optional>

namespace hammer::camera {

enum class ProjectionMode { Perspective, Orthographic };

struct State
{
    vmf::Vec3 position{512.0, -512.0, 384.0};
    double yawRadians{2.35619449019234492885};
    double pitchRadians{-0.52359877559829887308};
    double verticalFovRadians{1.30899693899574718269};
    double orthographicHeight{1024.0};
    double nearPlane{1.0};
};

struct CameraPoint
{
    double right{0.0};
    double up{0.0};
    double forward{0.0};
};

struct ScreenPoint
{
    double x{0.0};
    double y{0.0};
    double depth{0.0};
};

// Source/Hammer QAngle basis. Angles are stored as pitch, yaw, roll in
// degrees. The columns correspond to local +X (forward), +Y (left), and +Z
// (up), matching mathlib's AngleMatrix exactly.
struct SourceAngleBasis
{
    vmf::Vec3 forward{1.0, 0.0, 0.0};
    vmf::Vec3 left{0.0, 1.0, 0.0};
    vmf::Vec3 up{0.0, 0.0, 1.0};

    vmf::Vec3 rotate(const vmf::Vec3& local) const;
};

struct SourceTransform
{
    SourceAngleBasis basis;
    vmf::Vec3 origin;

    vmf::Vec3 transformPoint(const vmf::Vec3& local) const;
    vmf::Vec3 transformVector(const vmf::Vec3& local) const;
};

// Exact mathlib AngleMatrix basis for a Source QAngle (pitch, yaw, roll).
SourceAngleBasis sourceAngleBasis(const vmf::Vec3& pitchYawRollDegrees);
SourceTransform sourceTransform(const vmf::Vec3& origin,
                                const vmf::Vec3& pitchYawRollDegrees);
vmf::Vec3 forwardVector(const State& state);
vmf::Vec3 rightVector(const State& state);
vmf::Vec3 upVector(const State& state);
CameraPoint toCamera(const State& state, const vmf::Vec3& point);
std::optional<ScreenPoint> projectPoint(const State& state, ProjectionMode mode,
                                        const vmf::Vec3& point, double width, double height);
bool projectLine(const State& state, ProjectionMode mode,
                 const vmf::Vec3& first, const vmf::Vec3& second,
                 double width, double height, ScreenPoint& projectedFirst,
                 ScreenPoint& projectedSecond);

} // namespace hammer::camera
