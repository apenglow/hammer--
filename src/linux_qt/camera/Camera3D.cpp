#include "Camera3D.hpp"

#include <algorithm>
#include <cmath>

namespace hammer::camera {
namespace {
vmf::Vec3 subtract(const vmf::Vec3& a, const vmf::Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double dot(const vmf::Vec3& a, const vmf::Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

CameraPoint interpolate(const CameraPoint& a, const CameraPoint& b, double t)
{
    return {
        a.right + (b.right - a.right) * t,
        a.up + (b.up - a.up) * t,
        a.forward + (b.forward - a.forward) * t
    };
}

ScreenPoint projectCameraPoint(const State& state, ProjectionMode mode,
                               const CameraPoint& point, double width, double height)
{
    const double safeHeight = std::max(1.0, height);
    if (mode == ProjectionMode::Perspective) {
        const double focalLength = safeHeight / (2.0 * std::tan(state.verticalFovRadians * 0.5));
        return {
            width * 0.5 + point.right * focalLength / point.forward,
            height * 0.5 - point.up * focalLength / point.forward,
            point.forward
        };
    }

    const double pixelsPerUnit = safeHeight / std::max(1.0, state.orthographicHeight);
    return {
        width * 0.5 + point.right * pixelsPerUnit,
        height * 0.5 - point.up * pixelsPerUnit,
        point.forward
    };
}
} // namespace


vmf::Vec3 SourceAngleBasis::rotate(const vmf::Vec3& local) const
{
    return {
        forward.x * local.x + left.x * local.y + up.x * local.z,
        forward.y * local.x + left.y * local.y + up.y * local.z,
        forward.z * local.x + left.z * local.y + up.z * local.z
    };
}

vmf::Vec3 SourceTransform::transformPoint(const vmf::Vec3& local) const
{
    const vmf::Vec3 rotated = basis.rotate(local);
    return {rotated.x + origin.x, rotated.y + origin.y, rotated.z + origin.z};
}

vmf::Vec3 SourceTransform::transformVector(const vmf::Vec3& local) const
{
    return basis.rotate(local);
}

SourceAngleBasis sourceAngleBasis(const vmf::Vec3& pitchYawRollDegrees)
{
    constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;
    const double pitch = pitchYawRollDegrees.x * DegreesToRadians;
    const double yaw = pitchYawRollDegrees.y * DegreesToRadians;
    const double roll = pitchYawRollDegrees.z * DegreesToRadians;

    const double sp = std::sin(pitch);
    const double cp = std::cos(pitch);
    const double sy = std::sin(yaw);
    const double cy = std::cos(yaw);
    const double sr = std::sin(roll);
    const double cr = std::cos(roll);

    // Source mathlib AngleMatrix. The three vectors are matrix columns:
    // local +X (forward), local +Y (left), and local +Z (up).
    return {
        {cp * cy, cp * sy, -sp},
        {sr * sp * cy - cr * sy,
         sr * sp * sy + cr * cy,
         sr * cp},
        {cr * sp * cy + sr * sy,
         cr * sp * sy - sr * cy,
         cr * cp}
    };
}

SourceTransform sourceTransform(const vmf::Vec3& origin,
                                const vmf::Vec3& pitchYawRollDegrees)
{
    return {sourceAngleBasis(pitchYawRollDegrees), origin};
}

vmf::Vec3 forwardVector(const State& state)
{
    const double cosPitch = std::cos(state.pitchRadians);
    return {
        cosPitch * std::cos(state.yawRadians),
        cosPitch * std::sin(state.yawRadians),
        std::sin(state.pitchRadians)
    };
}

vmf::Vec3 rightVector(const State& state)
{
    // Source/Hammer coordinates use +X forward, +Y left, and +Z up.
    // Therefore the camera's screen-right direction at yaw zero is -Y.
    // The previous +Y basis was actually the camera's left vector and made
    // every 3D projection a horizontal mirror image.
    return {std::sin(state.yawRadians), -std::cos(state.yawRadians), 0.0};
}

vmf::Vec3 upVector(const State& state)
{
    const vmf::Vec3 forward = forwardVector(state);
    const vmf::Vec3 right = rightVector(state);
    // right x forward preserves +Z as screen-up while keeping a proper
    // right-handed camera basis: right x up == -forward.
    return {
        right.y * forward.z - right.z * forward.y,
        right.z * forward.x - right.x * forward.z,
        right.x * forward.y - right.y * forward.x
    };
}

CameraPoint toCamera(const State& state, const vmf::Vec3& point)
{
    const vmf::Vec3 relative = subtract(point, state.position);
    return {
        dot(relative, rightVector(state)),
        dot(relative, upVector(state)),
        dot(relative, forwardVector(state))
    };
}

std::optional<ScreenPoint> projectPoint(const State& state, ProjectionMode mode,
                                        const vmf::Vec3& point, double width, double height)
{
    const CameraPoint cameraPoint = toCamera(state, point);
    if (cameraPoint.forward < state.nearPlane) return std::nullopt;
    return projectCameraPoint(state, mode, cameraPoint, width, height);
}

bool projectLine(const State& state, ProjectionMode mode,
                 const vmf::Vec3& first, const vmf::Vec3& second,
                 double width, double height, ScreenPoint& projectedFirst,
                 ScreenPoint& projectedSecond)
{
    CameraPoint a = toCamera(state, first);
    CameraPoint b = toCamera(state, second);
    if (a.forward < state.nearPlane && b.forward < state.nearPlane) return false;

    if (a.forward < state.nearPlane || b.forward < state.nearPlane) {
        const double denominator = b.forward - a.forward;
        if (std::abs(denominator) < 1e-12) return false;
        const double t = std::clamp((state.nearPlane - a.forward) / denominator, 0.0, 1.0);
        const CameraPoint clipped = interpolate(a, b, t);
        if (a.forward < state.nearPlane) a = clipped;
        else b = clipped;
    }

    projectedFirst = projectCameraPoint(state, mode, a, width, height);
    projectedSecond = projectCameraPoint(state, mode, b, width, height);
    return true;
}

} // namespace hammer::camera
