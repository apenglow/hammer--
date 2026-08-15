#include "VmfFaceEdit.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>

namespace hammer::vmf {
namespace {

// mapface.cpp FaceNormals / DownVectors / RightVectors.
const Vec3 FaceNormals[6] = {{0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
const Vec3 DownVectors[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
const Vec3 RightVectors[6] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 1, 0}};

// g_pGameConfig->GetDefaultTextureScale(). Hammer ships 0.25 and every VMF the
// port writes already uses it (see VmfSolidClip.cpp).
constexpr double DefaultTextureScale = 0.25;

// mapface.cpp TEXTURE_AXIS_ROUND_EPSILON
constexpr double TextureAxisRoundEpsilon = 0.01;

double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 multiply(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

Vec3 normalize(const Vec3& value, const Vec3& fallback)
{
    const double length = std::sqrt(dot(value, value));
    if (length < 1e-9) return fallback;
    return multiply(value, 1.0 / length);
}

bool parseNumbers(std::string_view text, double* output, std::size_t count)
{
    std::string cleaned(text);
    for (char& ch : cleaned) {
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',') ch = ' ';
    }
    std::istringstream stream(cleaned);
    for (std::size_t i = 0; i < count; ++i) {
        if (!(stream >> output[i]) || !std::isfinite(output[i])) return false;
    }
    return true;
}

std::string formatNumber(double value)
{
    if (std::abs(value) < 0.0000005) value = 0.0;
    const double rounded = std::round(value);
    if (std::abs(value - rounded) < 0.0000005) return std::to_string(static_cast<long long>(rounded));

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    std::string result = stream.str();
    while (!result.empty() && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result.empty() || result == "-0" ? "0" : result;
}

// The VMF axis form Hammer writes: "[x y z shift] scale".
std::string formatAxis(const Vec3& axis, double shift, double scale)
{
    return "[" + formatNumber(axis.x) + " " + formatNumber(axis.y) + " " + formatNumber(axis.z) +
           " " + formatNumber(shift) + "] " + formatNumber(scale);
}

bool parseAxis(const std::string* text, Vec3& axis, double& shift, double& scale)
{
    if (!text) return false;
    double values[5];
    if (!parseNumbers(*text, values, 5)) return false;
    axis = {values[0], values[1], values[2]};
    shift = values[3];
    scale = std::abs(values[4]) < 1e-9 ? DefaultTextureScale : values[4];
    return true;
}

double parseDouble(const std::string* text, double fallback)
{
    if (!text) return fallback;
    double value = fallback;
    if (!parseNumbers(*text, &value, 1)) return fallback;
    return value;
}

int parseIntValue(const std::string* text, int fallback)
{
    double value = 0.0;
    if (!text || !parseNumbers(*text, &value, 1)) return fallback;
    return static_cast<int>(std::lround(value));
}

// CMapFace::GetTextureExtents: project the six extent points into texture
// space (divided by scale) and take the min/max corner of that rectangle.
void textureExtents(const FaceTexture& texture, const FaceExtents& extents,
                    double topLeft[2], double bottomRight[2])
{
    bool first = true;
    for (const Vec3& point : extents) {
        const double test[2] = {dot(point, texture.uAxis) / texture.uScale,
                                dot(point, texture.vAxis) / texture.vScale};
        if (first || test[0] < topLeft[0]) topLeft[0] = test[0];
        if (first || test[1] < topLeft[1]) topLeft[1] = test[1];
        if (first || test[0] > bottomRight[0]) bottomRight[0] = test[0];
        if (first || test[1] > bottomRight[1]) bottomRight[1] = test[1];
        first = false;
    }
}

// Intersection of three planes, as PlaneIntersection in mathlib/vmatrix.
bool planeIntersection(const Vec3& n0, double d0, const Vec3& n1, double d1,
                       const Vec3& n2, double d2, Vec3& point)
{
    const Vec3 c12 = cross(n1, n2);
    const double determinant = dot(n0, c12);
    if (std::abs(determinant) < 1e-9) return false;
    const Vec3 c20 = cross(n2, n0);
    const Vec3 c01 = cross(n0, n1);
    point = multiply(add(add(multiply(c12, d0), multiply(c20, d1)), multiply(c01, d2)),
                     1.0 / determinant);
    return true;
}

// SetupMatrixAxisRot / ApplyRotation: rotate a vector about an axis by degrees.
Vec3 rotateAboutAxis(const Vec3& value, const Vec3& axis, double degrees)
{
    const Vec3 unit = normalize(axis, {0.0, 0.0, 1.0});
    const double radians = degrees * (M_PI / 180.0);
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return add(add(multiply(value, c), multiply(cross(unit, value), s)),
               multiply(unit, dot(unit, value) * (1.0 - c)));
}

} // namespace

int faceOrientation(const Vec3& normal)
{
    if (normal.x == 0.0 && normal.y == 0.0 && normal.z == 0.0) return -1;
    const Vec3 unit = normalize(normal, {0.0, 0.0, 1.0});
    double best = 0.0;
    int orientation = -1;
    for (int i = 0; i < 6; ++i) {
        const double value = dot(unit, FaceNormals[i]);
        if (value >= best) {
            best = value;
            orientation = i;
        }
    }
    return orientation;
}

FaceTexture readFaceTexture(const Block& side)
{
    FaceTexture texture;
    if (const std::string* material = side.value("material")) texture.material = *material;
    parseAxis(side.value("uaxis"), texture.uAxis, texture.uShift, texture.uScale);
    parseAxis(side.value("vaxis"), texture.vAxis, texture.vShift, texture.vScale);
    texture.rotation = parseDouble(side.value("rotation"), 0.0);
    texture.lightmapScale = parseIntValue(side.value("lightmapscale"), 16);
    return texture;
}

void writeFaceTexture(Block& side, const FaceTexture& texture)
{
    side.setValue("material", texture.material);
    side.setValue("uaxis", formatAxis(texture.uAxis, texture.uShift, texture.uScale));
    side.setValue("vaxis", formatAxis(texture.vAxis, texture.vShift, texture.vScale));
    side.setValue("rotation", formatNumber(texture.rotation));
    side.setValue("lightmapscale", std::to_string(std::max(1, texture.lightmapScale)));
}

std::uint32_t readSmoothingGroups(const Block& side)
{
    const std::string* text = side.value("smoothing_groups");
    if (!text) return 0u;
    try {
        return static_cast<std::uint32_t>(std::stoul(*text, nullptr, 0));
    } catch (const std::exception&) {
        return 0u;
    }
}

void writeSmoothingGroups(Block& side, std::uint32_t groups)
{
    side.setValue("smoothing_groups", std::to_string(static_cast<unsigned long>(groups)));
}

void rotateTextureAxes(FaceTexture& texture, double degrees)
{
    if (std::abs(degrees) < 1e-9) return;
    // CMapFace::RotateTextureAxes rotates about V x U, the texture normal,
    // which need not be the face normal.
    const Vec3 textureNormal = cross(texture.vAxis, texture.uAxis);
    if (dot(textureNormal, textureNormal) < 1e-18) return;
    texture.uAxis = rotateAboutAxis(texture.uAxis, textureNormal, degrees);
    texture.vAxis = rotateAboutAxis(texture.vAxis, textureNormal, degrees);
}

void normalizeTextureShifts(FaceTexture& texture, int textureWidth, int textureHeight)
{
    // The original rounds every component of both axes, including the shift.
    const auto round = [](double& value) {
        const double nearest = std::round(value);
        if (std::abs(value - nearest) < TextureAxisRoundEpsilon) value = nearest;
    };
    round(texture.uAxis.x);
    round(texture.uAxis.y);
    round(texture.uAxis.z);
    round(texture.uShift);
    round(texture.vAxis.x);
    round(texture.vAxis.y);
    round(texture.vAxis.z);
    round(texture.vShift);

    if (textureWidth > 0) texture.uShift = std::fmod(texture.uShift, textureWidth);
    if (textureHeight > 0) texture.vShift = std::fmod(texture.vShift, textureHeight);
}

void initializeTextureAxes(FaceTexture& texture, const Vec3& normal, TextureAlignment alignment)
{
    const int orientation = faceOrientation(normal);
    if (orientation < 0) return;

    // INIT_TEXTURE_AXES only: the shift components are left alone.
    texture.vAxis = DownVectors[orientation];
    if (alignment == TextureAlignment::Face) {
        const Vec3 unitNormal = normalize(normal, FaceNormals[orientation]);
        texture.uAxis = normalize(cross(unitNormal, texture.vAxis), RightVectors[orientation]);
        texture.vAxis = normalize(cross(texture.uAxis, unitNormal), DownVectors[orientation]);
    } else {
        texture.uAxis = RightVectors[orientation];
    }

    if (texture.rotation != 0.0) rotateTextureAxes(texture, texture.rotation);
}

bool faceExtents(const std::vector<Vec3>& points, FaceExtents& extents)
{
    if (points.empty()) return false;
    bool first = true;
    for (const Vec3& point : points) {
        if (first || point.x < extents[0].x) extents[0] = point;
        if (first || point.x > extents[1].x) extents[1] = point;
        if (first || point.y < extents[2].y) extents[2] = point;
        if (first || point.y > extents[3].y) extents[3] = point;
        if (first || point.z < extents[4].z) extents[4] = point;
        if (first || point.z > extents[5].z) extents[5] = point;
        first = false;
    }
    return true;
}

void mergeFaceExtents(FaceExtents& into, const FaceExtents& from, bool first)
{
    if (first || from[0].x < into[0].x) into[0] = from[0];
    if (first || from[1].x > into[1].x) into[1] = from[1];
    if (first || from[2].y < into[2].y) into[2] = from[2];
    if (first || from[3].y > into[3].y) into[3] = from[3];
    if (first || from[4].z < into[4].z) into[4] = from[4];
    if (first || from[5].z > into[5].z) into[5] = from[5];
}

void justifyTextureUsingExtents(FaceTexture& texture, TextureJustification justification,
                                const FaceExtents& extents, int textureWidth, int textureHeight)
{
    if (texture.uScale == 0.0) texture.uScale = DefaultTextureScale;
    if (texture.vScale == 0.0) texture.vScale = DefaultTextureScale;

    // TEXTURE_JUSTIFY_FIT computes its extents at a scale of 1.
    if (justification == TextureJustification::Fit) {
        texture.uScale = 1.0;
        texture.vScale = 1.0;
    }

    double topLeft[2] = {0.0, 0.0};
    double bottomRight[2] = {0.0, 0.0};
    textureExtents(texture, extents, topLeft, bottomRight);

    const double center[2] = {(topLeft[0] + bottomRight[0]) / 2.0,
                              (topLeft[1] + bottomRight[1]) / 2.0};

    switch (justification) {
    case TextureJustification::Top:
        texture.vShift = -topLeft[1];
        break;
    case TextureJustification::Bottom:
        texture.vShift = -bottomRight[1] + textureHeight;
        break;
    case TextureJustification::Left:
        texture.uShift = -topLeft[0];
        break;
    case TextureJustification::Right:
        texture.uShift = -bottomRight[0] + textureWidth;
        break;
    case TextureJustification::Center:
        texture.uShift = -center[0] + (textureWidth / 2);
        texture.vShift = -center[1] + (textureHeight / 2);
        break;
    case TextureJustification::Fit:
        if (textureWidth > 0 && textureHeight > 0) {
            texture.uScale = (bottomRight[0] - topLeft[0]) / textureWidth;
            texture.vScale = (bottomRight[1] - topLeft[1]) / textureHeight;
        } else {
            texture.uScale = DefaultTextureScale;
            texture.vScale = DefaultTextureScale;
        }
        // Justify top left at the new scale, exactly as the original recurses.
        justifyTextureUsingExtents(texture, TextureJustification::Top, extents,
                                   textureWidth, textureHeight);
        justifyTextureUsingExtents(texture, TextureJustification::Left, extents,
                                   textureWidth, textureHeight);
        break;
    }

    normalizeTextureShifts(texture, textureWidth, textureHeight);
}

void alignTextureToView(FaceTexture& texture, const Vec3& viewRight, const Vec3& viewUp,
                        const Vec3& viewPoint, int textureWidth, int textureHeight)
{
    texture.uAxis = viewRight;
    texture.vAxis = viewUp;
    texture.uShift = dot(viewRight, viewPoint);
    texture.vShift = dot(viewUp, viewPoint);
    normalizeTextureShifts(texture, textureWidth, textureHeight);
    texture.rotation = 0.0;
    texture.uScale = DefaultTextureScale;
    texture.vScale = DefaultTextureScale;
}

void copyTextureCoordinateSystem(const FaceTexture& from, const Vec3& fromNormal, double fromDistance,
                                 FaceTexture& to, const Vec3& toNormal, double toDistance,
                                 int textureWidth, int textureHeight)
{
    Vec3 edge = normalize(cross(fromNormal, toNormal), {0.0, 0.0, 0.0});
    Vec3 edgePoint{};
    bool rotate = dot(edge, edge) > 1e-12 &&
                  planeIntersection(fromNormal, fromDistance, toNormal, toDistance,
                                    edge, 0.0, edgePoint);

    Vec3 axis[2] = {from.uAxis, from.vAxis};
    Vec3 origin = add(multiply(axis[0], from.uShift * from.uScale),
                      multiply(axis[1], from.vShift * from.vScale));

    if (rotate) {
        const Vec3 textureNormal = normalize(cross(axis[0], axis[1]), {0.0, 0.0, 1.0});
        // Project both normals into the plane of rotation to get the angle.
        Vec3 projectedTexture = normalize(
            subtract(textureNormal, multiply(edge, dot(edge, textureNormal))), {0.0, 0.0, 0.0});
        Vec3 projectedPoly = normalize(
            subtract(toNormal, multiply(edge, dot(edge, toNormal))), {0.0, 0.0, 0.0});

        const double value = std::clamp(dot(projectedTexture, projectedPoly), -1.0, 1.0);
        double angle = std::acos(value) * (180.0 / M_PI);
        if (value < 0.0) angle = 180.0 - angle;

        axis[0] = rotateAboutAxis(axis[0], edge, angle);
        axis[1] = rotateAboutAxis(axis[1], edge, angle);

        // Rotate the origin about the edge point, not about the world origin.
        origin = add(rotateAboutAxis(subtract(origin, edgePoint), edge, angle), edgePoint);
    }

    to.uAxis = axis[0];
    to.vAxis = axis[1];
    to.uShift = dot(axis[0], origin) / from.uScale;
    to.vShift = dot(axis[1], origin) / from.vScale;
    to.uScale = from.uScale;
    to.vScale = from.vScale;
    normalizeTextureShifts(to, textureWidth, textureHeight);
    // "rotate is only for UI purposes, it doesn't actually do anything."
    to.rotation = 0.0;
}

} // namespace hammer::vmf
