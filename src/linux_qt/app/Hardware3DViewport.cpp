#include "Hardware3DViewport.hpp"

#include "EnvCubemap.hpp"
#include "MapViewWidget.hpp"
#include "DetailObjects.hpp"
#include "StudioModelSystem.hpp"
#include "VmfRope.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QPaintEvent>
#include <QPointF>
#include <QRectF>
#include <QResizeEvent>
#include <QSurfaceFormat>
#include <QString>
#include <QTimer>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QtMath>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
constexpr float FarPlane = 262144.0f;
constexpr int MaxStudioPaletteBones = 32;
float wrappedUnitPhase(double seconds, double cyclesPerSecond)
{
    double phase = std::fmod(seconds * cyclesPerSecond, 1.0);
    if (phase < 0.0) phase += 1.0;
    return static_cast<float>(phase);
}


struct GpuVertex
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float u{0.0f};
    float v{0.0f};
    float nx{0.0f};
    float ny{0.0f};
    float nz{1.0f};
    float u2{0.0f};
    float v2{0.0f};
    float blendAlpha{0.0f};
    float tx{1.0f};
    float ty{0.0f};
    float tz{0.0f};
    float tangentSign{1.0f};
    // Lightmap-space coordinates: dot(position, normalized texture axis) /
    // lightmap scale. Integer boundaries are luxel boundaries, which is what
    // the shader's lightmap-grid overlay strokes.
    float lu{0.0f};
    float lv{0.0f};
};

// Studio vertices are kept separate so adding bone data does not inflate every
// cached world, displacement, sprite, and orthographic vertex.
struct StudioGpuVertex
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float u{0.0f};
    float v{0.0f};
    float nx{0.0f};
    float ny{0.0f};
    float nz{1.0f};
    float u2{0.0f};
    float v2{0.0f};
    float blendAlpha{0.0f};
    float tx{1.0f};
    float ty{0.0f};
    float tz{0.0f};
    float tangentSign{1.0f};
    float boneWeight0{0.0f};
    float boneWeight1{0.0f};
    float boneWeight2{0.0f};
    float boneWeight3{0.0f};
    float boneIndex0{0.0f};
    float boneIndex1{0.0f};
    float boneIndex2{0.0f};
    float boneIndex3{0.0f};
};

struct TextureRecord
{
    GLuint id{0};
    GLuint secondaryId{0};
    GLuint flowId{0};
    GLuint bumpId{0};
    std::vector<GLuint> bumpFrameIds;
    GLuint phongExponentId{0};
    GLuint selfIllumMaskId{0};
    GLuint lightWarpId{0};
    int width{0};
    int height{0};
    bool translucent{false};
    bool water{false};
    bool hasSecondTexture{false};
    bool hasFlowMap{false};
    bool hasBumpMap{false};
    bool hasPhongExponentMap{false};
    bool hasSelfIllumMask{false};
    bool hasLightWarp{false};
};

struct MaterialBatch
{
    std::string name;
    std::shared_ptr<const hammer::assets::Material> material;
    // Displacements are rendered from the same full grid used by the
    // selection outline. Keep them in separate batches so their authored
    // surface cannot lose cells to brush-face backface culling.
    bool displacement{false};
    // Which baked env_cubemap this batch's faces reflect, or -1 for none. Faces
    // are split into separate batches by probe, exactly as vbsp assigns a
    // cubemap per face, because the cubemap is a per-draw texture binding.
    int cubemapIndex{-1};
    std::vector<GpuVertex> vertices;
    GLint first{0};
    GLsizei count{0};
};

struct StudioGpuMesh
{
    int materialSlot{0};
    std::size_t sourceMeshIndex{0};
    // Every skin family is resolved and uploaded when the model enters the GPU
    // cache. Switching an entity or browser preview skin must never perform
    // synchronous VMT/VTF work on the paint path.
    std::vector<std::shared_ptr<const hammer::assets::Material>> skinMaterials;
    std::vector<TextureRecord> skinTextures;
    // Local palette slot -> model-global bone index. Meshes are greedily split
    // on triangle boundaries so every draw fits conservative ES2 uniforms.
    std::vector<int> paletteBones;
    GLuint vbo{0};
    GLuint vao{0};
    GLsizei count{0};
};

struct StudioGpuModel
{
    std::vector<StudioGpuMesh> meshes;
    int cachedSequence{-2};
    std::int64_t cachedCycleKey{std::numeric_limits<std::int64_t>::min()};
    std::vector<hammer::assets::StudioBoneMatrix> cachedPose;
};

struct ProjectedGpuBatch
{
    std::shared_ptr<const hammer::assets::Material> material;
    hammer::vmf::Vec3 minimum;
    hammer::vmf::Vec3 maximum;
    GLint first{0};
    GLsizei count{0};
};

struct OrthographicGpuBatch
{
    QVector4D color;
    GLuint vbo{0};
    GLuint vao{0};
    GLsizei count{0};
};

// --- Incremental scene caching ----------------------------------------------
//
// The GPU buffers below used to be keyed on the address of the Scene they were
// built from. MapDocumentWidget hands out an updated Scene for every drag
// mouse-move, so every view re-assembled and re-uploaded the whole map 60 times
// a second while one brush moved.
//
// Instead, a cache records the Scene revision it was built from together with
// the object ids it deliberately left out of its static buffers - the current
// selection, which is exactly what an interactive edit moves. A newer Scene can
// then reuse the static buffers whenever it descends from that revision and
// every object it changed is one of the excluded ones.
struct SceneCacheLineage
{
    std::uint64_t revision{0};
    std::unordered_set<int> dynamicSolidIds;
    std::unordered_set<int> dynamicEntityIds;

    void clear()
    {
        revision = 0;
        dynamicSolidIds.clear();
        dynamicEntityIds.clear();
    }
};

enum class SceneCacheState
{
    Current,     // identical scene state; nothing to do
    DynamicOnly, // static buffers still valid, rebuild the moving objects only
    Rebuild      // no usable relationship, rebuild everything
};

SceneCacheState classifySceneCache(const SceneCacheLineage& cache, const hammer::vmf::Scene* scene)
{
    // Revision 0 means "unstamped": never match it, or two unrelated scenes
    // would look identical.
    if (!scene || cache.revision == 0 || scene->revision == 0) return SceneCacheState::Rebuild;
    if (scene->revision == cache.revision) return SceneCacheState::Current;
    // The cache may be several edit steps behind the scene - during a drag,
    // edits routinely land faster than each view paints. Find the step this
    // cache last consumed and require every object changed since then to be
    // in the cache's dynamic set.
    for (std::size_t index = 0; index < scene->lineageSteps.size(); ++index) {
        if (scene->lineageSteps[index].baseRevision != cache.revision) continue;
        for (std::size_t step = index; step < scene->lineageSteps.size(); ++step) {
            for (const int id : scene->lineageSteps[step].solidIds)
                if (!cache.dynamicSolidIds.contains(id)) return SceneCacheState::Rebuild;
            for (const int id : scene->lineageSteps[step].entityIds)
                if (!cache.dynamicEntityIds.contains(id)) return SceneCacheState::Rebuild;
        }
        return SceneCacheState::DynamicOnly;
    }
    return SceneCacheState::Rebuild;
}

// Whether any BRUSH changed between a cached revision and this scene. Detail
// props are scattered over brush faces and nothing else, so an edit that only
// moved entities - which is most of an interactive session - must not force
// the whole map's props to be re-emitted.
bool solidsChangedSince(std::uint64_t cachedRevision, const hammer::vmf::Scene& scene)
{
    if (cachedRevision == 0 || scene.revision == 0) return true;
    if (scene.revision == cachedRevision) return false;
    for (std::size_t index = 0; index < scene.lineageSteps.size(); ++index) {
        if (scene.lineageSteps[index].baseRevision != cachedRevision) continue;
        for (std::size_t step = index; step < scene.lineageSteps.size(); ++step) {
            if (!scene.lineageSteps[step].solidIds.empty()) return true;
        }
        return false;
    }
    return true;
}

float safeScale(double value)
{
    return static_cast<float>(std::abs(value) < 1e-9 ? 0.25 : value);
}

float dot(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return static_cast<float>(a.x * b.x + a.y * b.y + a.z * b.z);
}

QVector3D vector(const hammer::vmf::Vec3& value)
{
    return QVector3D(static_cast<float>(value.x), static_cast<float>(value.y),
                     static_cast<float>(value.z));
}

QMatrix4x4 viewProjection(const hammer::camera::State& camera,
                          hammer::camera::ProjectionMode projectionMode,
                          int width, int height)
{
    const float aspect = static_cast<float>(std::max(1, width)) /
                         static_cast<float>(std::max(1, height));
    QMatrix4x4 projection;
    if (projectionMode == hammer::camera::ProjectionMode::Perspective) {
        projection.perspective(static_cast<float>(qRadiansToDegrees(camera.verticalFovRadians)),
                               aspect, static_cast<float>(std::max(0.05, camera.nearPlane)),
                               FarPlane);
    } else {
        const float halfHeight = static_cast<float>(std::max(1.0, camera.orthographicHeight) * 0.5);
        const float halfWidth = halfHeight * aspect;
        projection.ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
                         static_cast<float>(std::max(0.05, camera.nearPlane)), FarPlane);
    }

    const QVector3D eye = vector(camera.position);
    const QVector3D forward = vector(hammer::camera::forwardVector(camera));
    const QVector3D right = vector(hammer::camera::rightVector(camera));
    const QVector3D up = vector(hammer::camera::upVector(camera));

    // Build the view matrix from Hammer's Source-coordinate camera basis
    // explicitly. At yaw zero, +Y is world-left and -Y is screen-right. The
    // basis is proper/right-handed (right x up == -forward), so OpenGL winding,
    // the CPU projection, sprites, selection overlays, and picking all agree
    // without a horizontal reflection.
    QMatrix4x4 view;
    view.setRow(0, QVector4D(right, -QVector3D::dotProduct(right, eye)));
    view.setRow(1, QVector4D(up, -QVector3D::dotProduct(up, eye)));
    view.setRow(2, QVector4D(-forward, QVector3D::dotProduct(forward, eye)));
    view.setRow(3, QVector4D(0.0f, 0.0f, 0.0f, 1.0f));
    return projection * view;
}

std::array<hammer::vmf::Vec3, 8> boundsCorners(
    const hammer::vmf::Vec3& minimum, const hammer::vmf::Vec3& maximum)
{
    std::array<hammer::vmf::Vec3, 8> corners{};
    for (int index = 0; index < 8; ++index) {
        corners[static_cast<std::size_t>(index)] = {
            (index & 1) ? maximum.x : minimum.x,
            (index & 2) ? maximum.y : minimum.y,
            (index & 4) ? maximum.z : minimum.z};
    }
    return corners;
}

bool clipCornersVisible(const QMatrix4x4& matrix,
                        const std::array<hammer::vmf::Vec3, 8>& corners)
{
    std::array<int, 6> outside{};
    for (const hammer::vmf::Vec3& corner : corners) {
        const QVector4D clip = matrix * QVector4D(
            static_cast<float>(corner.x), static_cast<float>(corner.y),
            static_cast<float>(corner.z), 1.0f);
        if (!std::isfinite(clip.x()) || !std::isfinite(clip.y()) ||
            !std::isfinite(clip.z()) || !std::isfinite(clip.w())) {
            // Invalid helper bounds should remain visible rather than disappearing.
            return true;
        }
        outside[0] += clip.x() < -clip.w();
        outside[1] += clip.x() >  clip.w();
        outside[2] += clip.y() < -clip.w();
        outside[3] += clip.y() >  clip.w();
        outside[4] += clip.z() < -clip.w();
        outside[5] += clip.z() >  clip.w();
    }
    return std::none_of(outside.begin(), outside.end(),
                        [](int count) { return count == 8; });
}

bool entityVisibleInClip(const hammer::vmf::EntityMarker& entity,
                         const QMatrix4x4& matrix)
{
    if (entity.hasSelectionCorners)
        return clipCornersVisible(matrix, entity.selectionCorners);
    const hammer::vmf::Vec3 minimum{
        entity.origin.x + entity.sizeMinimum.x,
        entity.origin.y + entity.sizeMinimum.y,
        entity.origin.z + entity.sizeMinimum.z};
    const hammer::vmf::Vec3 maximum{
        entity.origin.x + entity.sizeMaximum.x,
        entity.origin.y + entity.sizeMaximum.y,
        entity.origin.z + entity.sizeMaximum.z};
    return clipCornersVisible(matrix, boundsCorners(minimum, maximum));
}

std::vector<std::uint8_t> rgbaPixels(const hammer::assets::Image& image)
{
    std::vector<std::uint8_t> rgba;
    rgba.resize(image.pixels.size() * 4);
    for (std::size_t index = 0; index < image.pixels.size(); ++index) {
        const std::uint32_t pixel = image.pixels[index];
        rgba[index * 4 + 0] = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        rgba[index * 4 + 1] = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        rgba[index * 4 + 2] = static_cast<std::uint8_t>(pixel & 0xff);
        rgba[index * 4 + 3] = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
    }
    return rgba;
}

std::array<std::uint8_t, 4> sampleImageClamped(
    const hammer::assets::Image& image, float u, float v)
{
    if (!image.valid()) return {0, 0, 0, 255};
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float x = u * static_cast<float>(std::max(0, image.width - 1));
    const float y = v * static_cast<float>(std::max(0, image.height - 1));
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, image.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, image.height - 1);
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    auto channel = [](std::uint32_t pixel, int shift) {
        return static_cast<float>((pixel >> shift) & 255u);
    };
    const std::uint32_t p00 = image.pixels[static_cast<std::size_t>(y0 * image.width + x0)];
    const std::uint32_t p10 = image.pixels[static_cast<std::size_t>(y0 * image.width + x1)];
    const std::uint32_t p01 = image.pixels[static_cast<std::size_t>(y1 * image.width + x0)];
    const std::uint32_t p11 = image.pixels[static_cast<std::size_t>(y1 * image.width + x1)];
    std::array<std::uint8_t, 4> result{};
    constexpr int shifts[4] = {16, 8, 0, 24};
    for (int component = 0; component < 4; ++component) {
        const float top = channel(p00, shifts[component]) * (1.0f - fx) +
                          channel(p10, shifts[component]) * fx;
        const float bottom = channel(p01, shifts[component]) * (1.0f - fx) +
                             channel(p11, shifts[component]) * fx;
        result[static_cast<std::size_t>(component)] = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(top * (1.0f - fy) + bottom * fy)),
                       0, 255));
    }
    return result;
}

// A Source sky is six unequal images covering only the world above the
// horizon, while an authored $envmap VTF is a true cubemap with real content in
// every direction. Only the former needs the horizon and zenith workarounds.
enum class SkyCubeKind
{
    SourceSky,
    AuthoredCubemap,
};

std::array<std::uint8_t, 4> sampleHammerSkyDirection(
    const std::array<const hammer::assets::Image*, 6>& faces,
    float x, float y, float z, SkyCubeKind kind)
{
    if (kind == SkyCubeKind::SourceSky) {
        // Source sky art carries almost no detail at the zenith - the "up" face
        // is smooth, and on many skies a single flat color - so a reflection ray
        // that climbs into the top cap goes matte, and the cap's 45 degree
        // boundary draws a ring across the surface. Squeeze elevation toward the
        // horizon so the detailed side faces cover nearly the whole hemisphere
        // and the flat spot shrinks toward a point directly under the viewer.
        // Lower values widen the detailed region; 1.0 restores true elevation.
        constexpr float ZenithCompression = 0.25f;
        z *= ZenithCompression;

        // Compression shrinks the flat cap but cannot remove it: a ray at the
        // zenith still samples the zenith. The shader fades the reflection out
        // over what remains, so the cap is left honest here.

        // Below the horizon there is only a 1-16 pixel placeholder face, because
        // in-game an env_cubemap captures the world there instead. Hold those
        // rays at the horizon so the placeholder is never sampled.
        if (z < 0.0f) {
            const float lateral = std::max(std::abs(x), std::abs(y));
            if (lateral <= 1e-8f) {
                // Straight down keeps no heading; any side face reads the same row.
                x = 1.0f; y = 0.0f; z = -1.0f;
            } else {
                z = std::max(z, -lateral);
            }
        }
    }

    const float ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
    float u = 0.5f, v = 0.5f, major = 1.0f, elevation = 0.0f;
    std::size_t face = 0;
    bool sideFace = true;
    if (ay >= ax && ay >= az) {                    // BK / +Y or FT / -Y
        major = std::max(ay, 1e-8f);
        if (y >= 0.0f) { face = 0; u = (x / major + 1.0f) * 0.5f; }
        else           { face = 2; u = (1.0f - x / major) * 0.5f; }
        elevation = z / major;
    } else if (ax >= ay && ax >= az) {             // LF / -X or RT / +X
        major = std::max(ax, 1e-8f);
        if (x < 0.0f) { face = 1; u = (y / major + 1.0f) * 0.5f; }
        else          { face = 3; u = (1.0f - y / major) * 0.5f; }
        elevation = z / major;
    } else if (z >= 0.0f) {                        // UP / +Z
        // Only an authored cubemap reaches either cap; the sky path clamps
        // every ray into the side band above.
        face = 4; major = std::max(az, 1e-8f);
        sideFace = false;
        u = (1.0f - y / major) * 0.5f;
        v = (x / major + 1.0f) * 0.5f;
    } else {                                       // DN / -Z
        face = 5; major = std::max(az, 1e-8f);
        sideFace = false;
        u = (1.0f - y / major) * 0.5f;
        v = (1.0f - x / major) * 0.5f;
    }
    if (!faces[face]) return {0, 0, 0, 255};
    // The face image is stretched to fill its square, so v spans the full
    // elevation sweep regardless of the source aspect ratio.
    if (sideFace) v = (1.0f - elevation) * 0.5f;
    return sampleImageClamped(*faces[face], u, v);
}

template <typename Vertex>
void buildTriangleTangents(std::vector<Vertex>& vertices)
{
    auto normalize3 = [](float& x, float& y, float& z) {
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length <= 1e-8f) return false;
        x /= length; y /= length; z /= length;
        return true;
    };
    for (std::size_t index = 0; index + 2 < vertices.size(); index += 3) {
        Vertex& a = vertices[index];
        Vertex& b = vertices[index + 1];
        Vertex& c = vertices[index + 2];
        const float edge1x = b.x - a.x, edge1y = b.y - a.y, edge1z = b.z - a.z;
        const float edge2x = c.x - a.x, edge2y = c.y - a.y, edge2z = c.z - a.z;
        const float du1 = b.u - a.u, dv1 = b.v - a.v;
        const float du2 = c.u - a.u, dv2 = c.v - a.v;
        const float determinant = du1 * dv2 - du2 * dv1;

        float tx = 1.0f, ty = 0.0f, tz = 0.0f;
        float bx = 0.0f, by = 1.0f, bz = 0.0f;
        if (std::abs(determinant) > 1e-8f) {
            const float inverse = 1.0f / determinant;
            tx = (edge1x * dv2 - edge2x * dv1) * inverse;
            ty = (edge1y * dv2 - edge2y * dv1) * inverse;
            tz = (edge1z * dv2 - edge2z * dv1) * inverse;
            bx = (edge2x * du1 - edge1x * du2) * inverse;
            by = (edge2y * du1 - edge1y * du2) * inverse;
            bz = (edge2z * du1 - edge1z * du2) * inverse;
            normalize3(tx, ty, tz);
            normalize3(bx, by, bz);
        } else {
            const float nx = a.nx, ny = a.ny, nz = a.nz;
            if (std::abs(nz) < 0.9f) {
                tx = -ny; ty = nx; tz = 0.0f;
            } else {
                tx = 0.0f; ty = -nz; tz = ny;
            }
            normalize3(tx, ty, tz);
            bx = ny * tz - nz * ty;
            by = nz * tx - nx * tz;
            bz = nx * ty - ny * tx;
            normalize3(bx, by, bz);
        }

        for (Vertex* vertex : {&a, &b, &c}) {
            float vx = tx, vy = ty, vz = tz;
            const float normalDot = vx * vertex->nx + vy * vertex->ny + vz * vertex->nz;
            vx -= vertex->nx * normalDot;
            vy -= vertex->ny * normalDot;
            vz -= vertex->nz * normalDot;
            if (!normalize3(vx, vy, vz)) { vx = 1.0f; vy = 0.0f; vz = 0.0f; }
            const float crossX = vertex->ny * vz - vertex->nz * vy;
            const float crossY = vertex->nz * vx - vertex->nx * vz;
            const float crossZ = vertex->nx * vy - vertex->ny * vx;
            vertex->tx = vx;
            vertex->ty = vy;
            vertex->tz = vz;
            vertex->tangentSign = (crossX * bx + crossY * by + crossZ * bz) < 0.0f ? -1.0f : 1.0f;
        }
    }
}

void assignBrushTextureTangent(GpuVertex& vertex,
                               const hammer::vmf::TextureAxis& uAxis,
                               const hammer::vmf::TextureAxis& vAxis)
{
    auto normalize3 = [](float& x, float& y, float& z) {
        const float length = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(length) || length <= 1e-8f) return false;
        x /= length; y /= length; z /= length;
        return true;
    };

    float nx = vertex.nx, ny = vertex.ny, nz = vertex.nz;
    if (!normalize3(nx, ny, nz)) { nx = 0.0f; ny = 0.0f; nz = 1.0f; }

    // VMF texture axes are gradients in world space. Preserve negative scales
    // because they flip the tangent frame. V pixels increase straight down the
    // texture, matching the un-negated V the brush vertices now carry.
    const float uScale = safeScale(uAxis.scale);
    const float vScale = safeScale(vAxis.scale);
    float tx = static_cast<float>(uAxis.direction.x) / uScale;
    float ty = static_cast<float>(uAxis.direction.y) / uScale;
    float tz = static_cast<float>(uAxis.direction.z) / uScale;
    const float tangentNormalDot = tx * nx + ty * ny + tz * nz;
    tx -= nx * tangentNormalDot;
    ty -= ny * tangentNormalDot;
    tz -= nz * tangentNormalDot;

    float bx = static_cast<float>(vAxis.direction.x) / vScale;
    float by = static_cast<float>(vAxis.direction.y) / vScale;
    float bz = static_cast<float>(vAxis.direction.z) / vScale;
    const float bitangentNormalDot = bx * nx + by * ny + bz * nz;
    bx -= nx * bitangentNormalDot;
    by -= ny * bitangentNormalDot;
    bz -= nz * bitangentNormalDot;

    if (!normalize3(tx, ty, tz)) {
        // Degenerate/custom axes still get a stable frame instead of sampling
        // the normal map through NaNs or flattening an entire brush face.
        if (std::abs(nz) < 0.9f) {
            tx = -ny; ty = nx; tz = 0.0f;
        } else {
            tx = 0.0f; ty = -nz; tz = ny;
        }
        normalize3(tx, ty, tz);
    }

    const float crossX = ny * tz - nz * ty;
    const float crossY = nz * tx - nx * tz;
    const float crossZ = nx * ty - ny * tx;
    const bool validBitangent = normalize3(bx, by, bz);
    vertex.tx = tx;
    vertex.ty = ty;
    vertex.tz = tz;
    vertex.tangentSign = validBitangent &&
        (crossX * bx + crossY * by + crossZ * bz) < 0.0f ? -1.0f : 1.0f;
}
} // namespace

class Hardware3DViewport::Renderer final : protected QOpenGLExtraFunctions
{
public:
    bool initialize()
    {
        if (initialized_) return ready_;
        initialized_ = true;
        error_.clear();
        description_.clear();
        QOpenGLContext* current = QOpenGLContext::currentContext();
        if (!current || !current->isValid()) {
            error_ = QStringLiteral("No valid OpenGL context is current");
            ready_ = false;
            return false;
        }

        // Hammer-- renders through desktop OpenGL 4.6 or the Vulkan ray-traced
        // viewport; there is no ES or pre-4.6 fallback. Refusing the context
        // outright beats silently rendering through a reduced path - an ES
        // context used to be accepted here and quietly changed how the scene
        // looked.
        const QSurfaceFormat actual = current->format();
        const int major = actual.majorVersion();
        const int minor = actual.minorVersion();
        if (current->isOpenGLES() || major < 4 || (major == 4 && minor < 6)) {
            error_ = QStringLiteral(
                "The active context is %1 %2.%3; Hammer-- requires desktop OpenGL 4.6")
                .arg(current->isOpenGLES() ? QStringLiteral("OpenGL ES")
                                           : QStringLiteral("OpenGL"))
                .arg(major)
                .arg(minor);
            ready_ = false;
            return false;
        }

        initializeOpenGLFunctions();
        program_ = std::make_unique<QOpenGLShaderProgram>();

        static constexpr const char* DesktopVertexShader = R"GLSL(
            #version 460 core
            #define LIGHTMAP_FWIDTH 1
            layout(location = 0) in vec3 aPosition;
            layout(location = 1) in vec2 aTexCoord;
            layout(location = 2) in vec3 aNormal;
            layout(location = 3) in vec2 aTexCoord2;
            layout(location = 4) in float aBlendAlpha;
            layout(location = 5) in vec4 aTangent;
            layout(location = 8) in vec2 aLightmapCoord;
            layout(location = 6) in vec4 aBoneWeights;
            layout(location = 7) in vec4 aBoneIndices;
            uniform mat4 uViewProjection;
            uniform mat4 uModel;
            uniform int uGpuSkinning;
            uniform vec4 uBoneRow0[32];
            uniform vec4 uBoneRow1[32];
            uniform vec4 uBoneRow2[32];
            out vec2 vTexCoord;
            out vec3 vWorldPosition;
            out vec3 vNormal;
            out vec2 vTexCoord2;
            out float vBlendAlpha;
            out vec2 vLightmapCoord;
            out vec4 vTangent;
            vec3 studioPoint(int bone, vec3 point)
            {
                return vec3(dot(uBoneRow0[bone].xyz, point) + uBoneRow0[bone].w,
                            dot(uBoneRow1[bone].xyz, point) + uBoneRow1[bone].w,
                            dot(uBoneRow2[bone].xyz, point) + uBoneRow2[bone].w);
            }
            vec3 studioDirection(int bone, vec3 direction)
            {
                return vec3(dot(uBoneRow0[bone].xyz, direction),
                            dot(uBoneRow1[bone].xyz, direction),
                            dot(uBoneRow2[bone].xyz, direction));
            }
            void main()
            {
                vec3 localPosition = aPosition;
                vec3 localNormal = aNormal;
                vec3 localTangent = aTangent.xyz;
                if (uGpuSkinning != 0) {
                    float totalWeight = aBoneWeights.x + aBoneWeights.y +
                                        aBoneWeights.z + aBoneWeights.w;
                    if (totalWeight > 0.000001) {
                        int bone0 = int(aBoneIndices.x + 0.5);
                        int bone1 = int(aBoneIndices.y + 0.5);
                        int bone2 = int(aBoneIndices.z + 0.5);
                        int bone3 = int(aBoneIndices.w + 0.5);
                        localPosition =
                            studioPoint(bone0, aPosition) * aBoneWeights.x +
                            studioPoint(bone1, aPosition) * aBoneWeights.y +
                            studioPoint(bone2, aPosition) * aBoneWeights.z +
                            studioPoint(bone3, aPosition) * aBoneWeights.w;
                        localPosition /= totalWeight;
                        localNormal =
                            studioDirection(bone0, aNormal) * aBoneWeights.x +
                            studioDirection(bone1, aNormal) * aBoneWeights.y +
                            studioDirection(bone2, aNormal) * aBoneWeights.z +
                            studioDirection(bone3, aNormal) * aBoneWeights.w;
                        localTangent =
                            studioDirection(bone0, aTangent.xyz) * aBoneWeights.x +
                            studioDirection(bone1, aTangent.xyz) * aBoneWeights.y +
                            studioDirection(bone2, aTangent.xyz) * aBoneWeights.z +
                            studioDirection(bone3, aTangent.xyz) * aBoneWeights.w;
                        float normalLengthSquared = dot(localNormal, localNormal);
                        localNormal = normalLengthSquared > 0.0000001
                            ? localNormal * inversesqrt(normalLengthSquared) : aNormal;
                        localTangent -= localNormal * dot(localNormal, localTangent);
                        float tangentLengthSquared = dot(localTangent, localTangent);
                        localTangent = tangentLengthSquared > 0.0000001
                            ? localTangent * inversesqrt(tangentLengthSquared) : aTangent.xyz;
                    }
                }
                vec4 worldPosition = uModel * vec4(localPosition, 1.0);
                gl_Position = uViewProjection * worldPosition;
                vTexCoord = aTexCoord;
                vWorldPosition = worldPosition.xyz;
                vNormal = normalize(mat3(uModel) * localNormal);
                vTexCoord2 = aTexCoord2;
                vBlendAlpha = aBlendAlpha;
                vLightmapCoord = aLightmapCoord;
                vTangent = vec4(normalize(mat3(uModel) * localTangent), aTangent.w);
            }
        )GLSL";
        static constexpr const char* DesktopFragmentShader = R"GLSL(
            #version 460 core
            #define LIGHTMAP_FWIDTH 1
            in vec2 vTexCoord;
            in vec3 vWorldPosition;
            in vec3 vNormal;
            in vec2 vTexCoord2;
            in float vBlendAlpha;
            in vec2 vLightmapCoord;
            in vec4 vTangent;
            uniform sampler2D uTexture;
            uniform sampler2D uTexture2;
            uniform sampler2D uWaterFlowMap;
            uniform sampler2D uBumpMap;
            uniform samplerCube uEnvironmentMap;
            uniform sampler2D uPhongExponentMap;
            uniform sampler2D uSelfIllumMask;
            uniform sampler2D uLightWarpTexture;
            uniform vec4 uColor;
            uniform bool uUseTexture;
            uniform int uHasTexture2;
            uniform int uShaded;
            uniform int uForceOpaque;
            uniform float uMaterialAlpha;
            uniform int uLightmapGrid;
            uniform int uAlphaTest;
            uniform int uVertexAlpha;
            uniform float uAlphaTestReference;
            uniform int uUseBumpMap;
            uniform int uHasBumpMap;
            uniform int uUsePhong;
            uniform int uUseSpecular;
            uniform int uUseSelfIllum;
            uniform int uUseSelfIllumFresnel;
            uniform int uUseLightWarp;
            uniform int uHalfLambert;
            uniform int uUseRimLight;
            uniform int uHasSelfIllumMask;
            uniform int uRimMaskFromExponentAlpha;
            uniform int uHasEnvironmentMap;
            uniform int uPhongMaskFromBaseAlpha;
            uniform int uSpecularMaskMode;
            uniform int uInvertSpecularMask;
            uniform int uHasPhongExponentMap;
            uniform int uPhongExponentOverride;
            uniform int uPhongAlbedoTint;
            uniform vec3 uPhongFresnelRanges;
            uniform vec3 uPhongTint;
            uniform vec3 uEnvMapTint;
            uniform float uEnvMapContrast;
            uniform float uEnvMapSaturation;
            uniform float uPhongExponent;
            uniform float uPhongBoost;
            uniform float uSpecularStrength;
            uniform vec3 uSelfIllumTint;
            uniform vec3 uSelfIllumFresnelMinMaxExp;
            uniform vec3 uColor2;
            uniform int uBlendTintByBaseAlpha;
            uniform float uBlendTintColorOverBase;
            uniform int uHighEnergyEffect;
            uniform float uRimLightExponent;
            uniform float uRimLightBoost;
            uniform float uPhongIntensity;
            uniform float uSpecularIntensity;
            uniform float uBumpMapIntensity;
            uniform int uWater;
            uniform vec2 uWaterScrollOffsetA;
            uniform vec2 uWaterScrollOffsetB;
            uniform vec2 uWaterScale;
            uniform int uWaterMultiTexture;
            uniform float uWaterFlowPhase;
            uniform vec3 uCameraPosition;
            uniform vec3 uWaterFogColor;
            uniform vec3 uWaterRefractTint;
            uniform vec3 uWaterReflectTint;
            uniform float uWaterFresnel;
            uniform float uWaterReflectAmount;
            uniform float uWaterRefractAmount;
            uniform float uWaterReflectBlendFactor;
            uniform float uWaterFogStart;
            uniform float uWaterFogEnd;
            uniform float uWaterAlpha;
            uniform float uWaterNormalScale;
            uniform int uWaterNoFresnel;
            uniform int uWaterHasFlowMap;
            uniform float uWaterFlowDistance;
            uniform float uWaterFlowMapScale;
            uniform float uWaterFlowNormalUvScale;
            // Scene captured behind the water surface: colour on unit 2 and
            // depth on unit 3, bound only for the water pass.
            uniform sampler2D uSceneColor;
            uniform highp sampler2D uSceneDepth;
            uniform int uHasSceneCapture;
            uniform highp vec2 uSceneSize;
            uniform highp vec2 uDepthPlanes;

            // Window-space depth -> eye-space distance, using the same near/far
            // the projection was built with. mediump cannot hold this on ES.
            highp float eyeDistance(highp float windowZ)
            {
                highp float near = uDepthPlanes.x;
                highp float far = uDepthPlanes.y;
                highp float ndc = windowZ * 2.0 - 1.0;
                return (2.0 * near * far) / (far + near - ndc * (far - near));
            }
            out vec4 fragmentColor;

            vec4 shadeWater()
            {
                // Source Water_DX90-style editor approximation. The normal
                // map independently distorts reflection and refraction in the
                // brush face's tangent space. The current map skybox substitutes
                // for both Source runtime water render targets.
                vec3 geometricNormal = normalize(vNormal);
                vec3 tangent = vTangent.xyz;
                tangent -= geometricNormal * dot(geometricNormal, tangent);
                if (length(tangent) < 0.05) {
                    vec3 referenceAxis = abs(geometricNormal.z) < 0.99
                        ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
                    tangent = normalize(cross(referenceAxis, geometricNormal));
                } else {
                    tangent = normalize(tangent);
                }
                vec3 bitangent = normalize(cross(geometricNormal, tangent)) *
                    (vTangent.w < 0.0 ? -1.0 : 1.0);

                vec2 baseUv = vTexCoord * uWaterScale;
                vec3 normalBase;
                vec3 normalDetailA;
                vec3 normalDetailB;
                if (uWaterHasFlowMap != 0) {
                    vec2 encodedFlow = texture(uWaterFlowMap,
                        vTexCoord * uWaterFlowMapScale).rg;
                    vec2 flowDirection = vec2(1.0 - encodedFlow.r * 2.0,
                                              encodedFlow.g * 2.0 - 1.0);
                    float phaseA = uWaterFlowPhase;
                    float phaseB = fract(uWaterFlowPhase + 0.5);
                    float phaseBlend = abs(phaseA * 2.0 - 1.0);
                    vec2 flowUv = baseUv * uWaterFlowNormalUvScale;
                    vec3 flowNormalA = texture(uTexture,
                        flowUv - flowDirection * phaseA * uWaterFlowDistance,
                        -2.0).xyz * 2.0 - 1.0;
                    vec3 flowNormalB = texture(uTexture,
                        flowUv - flowDirection * phaseB * uWaterFlowDistance,
                        -2.0).xyz * 2.0 - 1.0;
                    normalBase = mix(flowNormalA, flowNormalB, phaseBlend);

                    float phaseC = fract(uWaterFlowPhase + 0.25);
                    float phaseD = fract(uWaterFlowPhase + 0.75);
                    float secondaryBlend = abs(phaseC * 2.0 - 1.0);
                    vec2 secondaryUv = flowUv * 1.53;
                    vec2 secondaryFlow = flowDirection * 1.17;
                    vec3 flowNormalC = texture(uTexture,
                        secondaryUv - secondaryFlow * phaseC * uWaterFlowDistance,
                        -2.0).xyz * 2.0 - 1.0;
                    vec3 flowNormalD = texture(uTexture,
                        secondaryUv - secondaryFlow * phaseD * uWaterFlowDistance,
                        -2.0).xyz * 2.0 - 1.0;
                    normalDetailA = mix(flowNormalC, flowNormalD, secondaryBlend);
                    normalDetailB = normalBase;
                } else {
                    normalBase = texture(uTexture, baseUv, -2.0).xyz * 2.0 - 1.0;
                    // Keep Source's broader WaterCheap normal layers even when a
                    // material does not animate $scroll1/$scroll2. The lower UV
                    // frequencies plus a conservative negative mip bias preserve
                    // authored slope variation when the surface is far from the eye.
                    vec2 centered = baseUv - vec2(0.5);
                    vec2 rotated45 = vec2(centered.x - centered.y,
                                          centered.x + centered.y) * 0.101015254;
                    vec2 rotated90 = vec2(-centered.y, centered.x) * 0.5;
                    vec2 detailOffsetA = uWaterMultiTexture != 0
                        ? uWaterScrollOffsetA : vec2(0.0);
                    vec2 detailOffsetB = uWaterMultiTexture != 0
                        ? uWaterScrollOffsetB : vec2(0.0);
                    normalDetailA = texture(uTexture,
                        rotated45 + vec2(0.5) + detailOffsetA,
                        -2.0).xyz * 2.0 - 1.0;
                    normalDetailB = texture(uTexture,
                        rotated90 + vec2(0.5) + detailOffsetB,
                        -2.0).xyz * 2.0 - 1.0;
                }

                vec2 authoredSlope;
                if (uWaterHasFlowMap != 0) {
                    authoredSlope = normalBase.xy * 0.62 + normalDetailA.xy * 0.38;
                } else {
                    authoredSlope = normalBase.xy * 0.55 +
                                    normalDetailA.xy * 0.27 +
                                    normalDetailB.xy * 0.18;
                }
                authoredSlope *= max(uWaterNormalScale, 0.0);

                // Source passes $reflectamount and $refractamount directly as
                // independent normal-map distortion scales. Keep separate surface
                // normals so changing one does not incorrectly affect the other.
                float reflectionDistortion = clamp(abs(uWaterReflectAmount), 0.0, 4.0);
                float refractionDistortion = clamp(abs(uWaterRefractAmount), 0.0, 4.0);
                vec3 reflectionTangentNormal = normalize(
                    vec3(authoredSlope * reflectionDistortion, 1.0));
                vec3 refractionTangentNormal = normalize(
                    vec3(authoredSlope * refractionDistortion, 1.0));
                vec3 reflectionSurfaceNormal = normalize(
                    tangent * reflectionTangentNormal.x +
                    bitangent * reflectionTangentNormal.y +
                    geometricNormal * reflectionTangentNormal.z);
                vec3 refractionSurfaceNormal = normalize(
                    tangent * refractionTangentNormal.x +
                    bitangent * refractionTangentNormal.y +
                    geometricNormal * refractionTangentNormal.z);

                vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
                float interfaceSide = dot(geometricNormal, viewDirection) >= 0.0
                    ? 1.0 : -1.0;
                vec3 interfaceReflectionNormal = reflectionSurfaceNormal * interfaceSide;
                vec3 interfaceRefractionNormal = refractionSurfaceNormal * interfaceSide;
                float normalView = clamp(dot(interfaceReflectionNormal, viewDirection),
                                         0.0, 1.0);
                // Schlick fresnel with F0 = $fresnelreflectance, identical to
                // the RT viewport's water path so both renderers agree.
                float fresnel = uWaterNoFresnel != 0
                    ? uWaterFresnel
                    : clamp(uWaterFresnel + (1.0 - uWaterFresnel) *
                            pow(1.0 - normalView, 5.0), 0.0, 1.0);

                vec3 incidentDirection = -viewDirection;
                vec3 reflectedDirection = normalize(
                    reflect(incidentDirection, interfaceReflectionNormal));

                // Source's expensive path distorts a scene refraction render
                // target. The editor has no such buffer, so use the current map
                // skybox as a transmission proxy without sampling the cubemap's
                // usually featureless down face. First calculate the physical
                // air/water ray, then fold it into the camera-facing skybox
                // hemisphere. The separate refraction normal still bends the
                // environment according to $refractamount.
                float relativeIor = interfaceSide > 0.0
                    ? (1.0 / 1.33333333) : 1.33333333;
                vec3 refractedDirection = refract(
                    incidentDirection, interfaceRefractionNormal, relativeIor);
                if (dot(refractedDirection, refractedDirection) < 0.000001) {
                    // Total internal reflection when looking from below.
                    refractedDirection = reflect(
                        incidentDirection, interfaceRefractionNormal);
                }
                refractedDirection = normalize(refractedDirection);
                vec3 visibleSkyHemisphere = geometricNormal * interfaceSide;
                // How steeply the physical refracted ray dives into the water
                // before the sky-hemisphere fold below. Steep rays would hit
                // the bottom and fog out in Source (and in the RT view);
                // grazing rays escape to the sky.
                float transmissionDepth = clamp(
                    -dot(refractedDirection, visibleSkyHemisphere), 0.0, 1.0);
                if (dot(refractedDirection, visibleSkyHemisphere) < 0.0) {
                    refractedDirection = normalize(reflect(
                        refractedDirection, visibleSkyHemisphere));
                }

                // Source-accurate composition without scene render targets:
                // the reflection samples the map skybox along the distorted
                // reflection ray, tinted by $reflecttint (and scaled by
                // $reflectblendfactor); the transmission is the fully fogged
                // water volume ($fogcolor) — what Source's expensive water
                // converges to once its refraction sample is fogged out, and
                // exactly what the shipped WaterCheap shader draws. Schlick
                // fresnel with F0 = $fresnelreflectance blends the two. With
                // no map skybox the surface stays pure water fog instead of
                // vanishing. The refracted ray above still shapes the fresnel
                // through the refraction normal; $refractamount has no further
                // visual effect because the transmission is fully fogged,
                // matching Source's own fogged-out limit.
                // Opaque composition: water owns its pixels. The refraction
                // stand-in is the sky along the refracted ray, fogged toward
                // $fogcolor by the authored $fogstart/$fogend over an estimated
                // underwater path; the reflection is the skybox scaled by
                // $reflecttint and $reflectamount, blended by Schlick fresnel
                // times $reflectblendfactor.
                // Real refraction and depth fog, read from the captured scene
                // behind the surface - the editor's equivalent of Source's
                // $refracttexture/$refractdepth grab. Legacy contexts with no
                // capture keep the older cubemap-only approximation below.
                vec3 color;
                highp float waterEye = eyeDistance(gl_FragCoord.z);
                vec3 reflected = uHasEnvironmentMap != 0
                    ? textureLod(uEnvironmentMap, reflectedDirection, 0.0).rgb *
                          uWaterReflectTint * clamp(abs(uWaterReflectAmount), 0.0, 4.0)
                    : uWaterFogColor * 1.25;
                if (uHasSceneCapture != 0) {
                    highp vec2 screenUv = gl_FragCoord.xy / uSceneSize;
                    // Distortion shrinks with distance, otherwise far water smears.
                    highp float distanceFade = 1.0 / (1.0 + waterEye * 0.004);
                    vec2 refractOffset = reflectionTangentNormal.xy *
                        refractionDistortion * 0.06 * distanceFade;
                    highp vec2 refractUv =
                        clamp(screenUv + refractOffset, vec2(0.0), vec2(1.0));
                    highp float behindEye = eyeDistance(texture(uSceneDepth, refractUv).r);
                    // Never bend geometry that is in front of the water into it.
                    if (behindEye < waterEye) {
                        refractUv = screenUv;
                        behindEye = eyeDistance(texture(uSceneDepth, screenUv).r);
                    }
                    vec3 behind = texture(uSceneColor, refractUv).rgb * uWaterRefractTint;
                    // $fogstart/$fogend finally describe a real path length: the
                    // distance from the surface to whatever is behind it. The sky
                    // sits at the far plane, so open water fogs out completely.
                    highp float path = max(behindEye - waterEye, 0.0);
                    highp float fogAmount = uWaterFogEnd > uWaterFogStart + 1.0
                        ? clamp((path - uWaterFogStart) /
                                (uWaterFogEnd - uWaterFogStart), 0.0, 1.0)
                        : 0.35;
                    vec3 refracted = mix(behind, uWaterFogColor, fogAmount);
                    color = mix(refracted, reflected,
                                clamp(fresnel * uWaterReflectBlendFactor, 0.0, 1.0));
                } else if (uHasEnvironmentMap != 0) {
                    vec3 refracted = textureLod(uEnvironmentMap, refractedDirection, 0.0).rgb *
                        uWaterRefractTint;
                    refracted = mix(refracted, uWaterFogColor, 0.35);
                    color = mix(refracted * max(uWaterRefractAmount, 0.35), reflected,
                                clamp(fresnel * uWaterReflectBlendFactor, 0.0, 1.0));
                } else {
                    color = mix(uWaterFogColor, uWaterFogColor * 1.25, fresnel);
                }
                // Keep the authored wave pattern visible even where the sky
                // sample is featureless.
                float waveShade = 1.0 +
                    (reflectionTangentNormal.x * 0.30 + reflectionTangentNormal.y * 0.22);
                color *= clamp(waveShade, 0.72, 1.28);
                return vec4(clamp(color, 0.0, 1.0), 1.0);
            }

            void main()
            {
                if (uWater != 0) {
                    fragmentColor = shadeWater();
                } else {
                    fragmentColor = uUseTexture ? texture(uTexture, vTexCoord) : uColor;
                    if (uUseTexture && uHasTexture2 != 0) {
                        vec4 secondColor = texture(uTexture2, vTexCoord2);
                        fragmentColor = mix(fragmentColor, secondColor,
                                            clamp(vBlendAlpha, 0.0, 1.0));
                    }
                    if (uShaded != 0 && uUseTexture) {
                        vec3 geometricNormal = normalize(vNormal);
                        vec3 normal = geometricNormal;
                        vec4 bumpSample = uHasBumpMap != 0
                            ? texture(uBumpMap, vTexCoord) : vec4(0.5, 0.5, 1.0, 1.0);
                        if (uUseBumpMap != 0) {
                            vec3 tangentCandidate = vTangent.xyz;
                            float tangentLength = length(tangentCandidate);
                            if (tangentLength > 0.25) {
                                vec3 tangent = tangentCandidate / tangentLength;
                                tangent -= geometricNormal * dot(geometricNormal, tangent);
                                float orthogonalLength = length(tangent);
                                if (orthogonalLength > 0.05) {
                                    tangent /= orthogonalLength;
                                    vec3 bitangent = normalize(cross(geometricNormal, tangent)) *
                                                     (vTangent.w < 0.0 ? -1.0 : 1.0);
                                    vec3 tangentNormal = bumpSample.xyz * 2.0 - 1.0;
                                    float mappedLength = length(tangentNormal);
                                    if (mappedLength > 0.25) {
                                        // At 100% the authored normal map is used at full
                                        // strength. Values above 100% steepen its X/Y slope;
                                        // zero returns exactly to the geometric vertex normal.
                                        float bumpStrength = clamp(uBumpMapIntensity, 0.0, 4.0);
                                        tangentNormal.xy *= bumpStrength;
                                        tangentNormal.z = max(tangentNormal.z, 0.06);
                                        tangentNormal = normalize(tangentNormal);
                                        normal = normalize(tangent * tangentNormal.x +
                                                           bitangent * tangentNormal.y +
                                                           geometricNormal * tangentNormal.z);
                                    }
                                }
                            }
                        }

                        vec3 lightDirection = normalize(vec3(-0.42, 0.36, 0.83));
                        vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
                        vec3 halfDirection = normalize(lightDirection + viewDirection);
                        float normalLight = clamp(dot(normal, lightDirection), -1.0, 1.0);
                        float lambertDiffuse = max(normalLight, 0.0);
                        // Source's half-Lambert response remaps [-1, 1] into [0, 1]
                        // and squares the result. Do this before the lightwarp lookup:
                        // the LUT replaces each local light's diffuse response rather
                        // than post-processing the already accumulated lighting.
                        float halfLambertDiffuse = clamp(normalLight * 0.5 + 0.5, 0.0, 1.0);
                        halfLambertDiffuse *= halfLambertDiffuse;
                        float sourceDiffuse = uHalfLambert != 0
                            ? halfLambertDiffuse : lambertDiffuse;
                        float fill = max(dot(normal, -lightDirection), 0.0);
                        // Source adds ambient-cube lighting independently of warped
                        // local lights. This neutral preview term stands in for the
                        // per-entity ambient cube that is only available after VRAD.
                        vec3 ambientCubeLighting = vec3(0.26 + fill * 0.06);
                        float normalHalf = max(dot(normal, halfDirection), 0.0);
                        float normalView = max(dot(normal, viewDirection), 0.0);
                        float baseAlphaMask = clamp(fragmentColor.a, 0.0, 1.0);
                        vec3 linearAlbedo = pow(max(fragmentColor.rgb, vec3(0.0)), vec3(2.2));
                        vec3 linearColor2 = pow(max(uColor2, vec3(0.0)), vec3(2.2));
                        if (uBlendTintByBaseAlpha != 0) {
                            vec3 tintedColor = linearAlbedo * linearColor2;
                            tintedColor = mix(tintedColor, linearColor2,
                                clamp(uBlendTintColorOverBase, 0.0, 1.0));
                            linearAlbedo = mix(linearAlbedo, tintedColor, baseAlphaMask);
                        } else {
                            linearAlbedo *= linearColor2;
                        }
                        vec3 directDiffuseLighting = vec3(sourceDiffuse * 0.78);
                        if (uUseLightWarp != 0) {
                            float lightWarpCoordinate = clamp(sourceDiffuse, 0.0, 1.0);
                            // Source binds $lightwarptexture without sRGB sampling.
                            // The texel is already the replacement local-light value;
                            // do not gamma-decode it and do not multiply it by N.L again.
                            vec3 lightWarpColor = max(
                                texture(uLightWarpTexture,
                                        vec2(lightWarpCoordinate, 0.5)).rgb,
                                vec3(0.0));
                            directDiffuseLighting = lightWarpColor * 0.78;
                        }
                        vec3 diffuseLightingColor = clamp(
                            ambientCubeLighting + directDiffuseLighting,
                            vec3(0.18), vec3(1.18));
                        vec3 linearTexture = linearAlbedo * diffuseLightingColor;
                        vec3 materialHighlight = vec3(0.0);
                        vec4 exponentSample = uHasPhongExponentMap != 0
                            ? texture(uPhongExponentMap, vTexCoord)
                            : vec4(1.0);
                        float normalAlphaMask = uHasBumpMap != 0
                            ? clamp(bumpSample.a, 0.0, 1.0) : 1.0;
                        // Direct Phong and cubemap reflection are masked through
                        // related but distinct Source controls. The Phong lobe uses
                        // normal-map alpha unless $basemapalphaphongmask selects
                        // base alpha. The envmap path chooses its mask separately.
                        float phongMask = uPhongMaskFromBaseAlpha != 0
                            ? baseAlphaMask : normalAlphaMask;

                        if (uUsePhong != 0) {
                            // skin_ps20b uses 1 + 149 * red for the exponent map.
                            // A positive scalar $phongexponent overrides that map.
                            float exponent = uPhongExponentOverride != 0
                                ? uPhongExponent
                                : (uHasPhongExponentMap != 0
                                    ? 1.0 + 149.0 * exponentSample.r : 150.0);
                            exponent = clamp(exponent, 1.0, 256.0);
                            float boost = clamp(uPhongBoost, 0.0, 16.0);
                            float phongLobe = pow(normalHalf, exponent);
                            float fresnelCoordinate = clamp(1.0 - normalView, 0.0, 1.0);
                            float phongFresnel = fresnelCoordinate < 0.5
                                ? mix(uPhongFresnelRanges.x, uPhongFresnelRanges.y,
                                      fresnelCoordinate * 2.0)
                                : mix(uPhongFresnelRanges.y, uPhongFresnelRanges.z,
                                      (fresnelCoordinate - 0.5) * 2.0);
                            float phongStrength = boost * 0.42 *
                                clamp(uPhongIntensity, 0.0, 4.0) * phongMask;
                            // Stock Source uses exponent-map green as the albedo
                            // tint blend only when $phongalbedotint is enabled and
                            // the constant $phongtint is explicitly [0 0 0].
                            vec3 phongColor = uPhongAlbedoTint != 0
                                ? mix(vec3(1.0), linearAlbedo, exponentSample.g)
                                : uPhongTint;
                            materialHighlight += phongColor * phongLobe *
                                                 phongStrength * phongFresnel;
                        }

                        if (uUseSpecular != 0 && uHasEnvironmentMap != 0) {
                            vec3 reflectedView = reflect(-viewDirection, normal);
                            vec3 envSample = max(
                                texture(uEnvironmentMap, reflectedView).rgb, vec3(0.0));
                            // $envmapsaturation then $envmapcontrast, in Source's
                            // order. Contrast squares the reflection.
                            float envGrey = dot(envSample, vec3(0.299, 0.587, 0.114));
                            envSample = mix(vec3(envGrey), envSample, uEnvMapSaturation);
                            envSample = mix(envSample, envSample * envSample,
                                            uEnvMapContrast);
                            vec3 environmentColor =
                                pow(envSample, vec3(2.2)) * uEnvMapTint;
                            // Source's skin shader does not apply the cubemap as an
                            // unconditional full-model coat. It uses base alpha by
                            // default, or normal-map alpha when requested by the VMT.
                            // $invertphongmask flips only this environment mask.
                            float specularMask = 1.0;
                            if (uSpecularMaskMode == 1) specularMask = baseAlphaMask;
                            else if (uSpecularMaskMode == 2) specularMask = normalAlphaMask;
                            if (uInvertSpecularMask != 0)
                                specularMask = 1.0 - specularMask;
                            specularMask = clamp(specularMask, 0.0, 1.0);
                            float strength = clamp(uSpecularStrength *
                                uSpecularIntensity * 2.40, 0.0, 2.0);
                            // Source's lightmapped envmap is tint x mask, with
                            // no view-dependent weighting at all. The editor
                            // previously scaled this by a Fresnel ramp and by a
                            // pow(N.H, 12) lobe. Both are anchored to the
                            // viewer, so each painted a soft-edged disc that
                            // tracked the camera with duller surroundings -
                            // the "reflection circle". Keep the reflection
                            // uniform so no such boundary can exist.
                            float environmentAmount = strength * specularMask;
                            materialHighlight += environmentColor * environmentAmount;
                        }

                        if (uUseSelfIllum != 0) {
                            vec3 selfIllumMask = uHasSelfIllumMask != 0
                                ? texture(uSelfIllumMask, vTexCoord).rgb
                                : vec3(baseAlphaMask);
                            vec3 selfIllumColor = linearAlbedo * uSelfIllumTint;
                            if (uUseSelfIllumFresnel != 0) {
                                float vertexNormalView = clamp(
                                    dot(normalize(geometricNormal), viewDirection), 0.0, 1.0);
                                float selfIllumMinimum = uSelfIllumFresnelMinMaxExp.x;
                                float selfIllumMaximum = uSelfIllumFresnelMinMaxExp.y;
                                float selfIllumExponent = max(
                                    uSelfIllumFresnelMinMaxExp.z, 0.01);
                                float selfIllumBias = abs(selfIllumMaximum) > 0.00001
                                    ? selfIllumMinimum / selfIllumMaximum : 0.0;
                                float selfIllumScale = 1.0 - selfIllumBias;
                                float selfIllumFresnel = clamp(
                                    pow(vertexNormalView, selfIllumExponent) *
                                    selfIllumScale + selfIllumBias, 0.0, 1.0);
                                selfIllumMask = vec3(baseAlphaMask * selfIllumFresnel);
                                selfIllumColor *= max(selfIllumMaximum, 0.0);
                            }
                            linearTexture = mix(linearTexture, selfIllumColor,
                                                clamp(selfIllumMask, 0.0, 1.0));
                        }

                        if (uUseRimLight != 0) {
                            // Match Source's VertexLitGeneric composition more
                            // closely. PixelShaderDoSpecularLighting first makes
                            // a light-derived rim lobe; skin_ps20b then multiplies
                            // it by the traditional fourth-power Fresnel and the
                            // optional exponent-map-alpha mask. $rimlightboost is
                            // reserved for the ambient-cube term, not the whole rim.
                            float rimMask = uRimMaskFromExponentAlpha != 0
                                ? clamp(exponentSample.a, 0.0, 1.0) : 1.0;
                            float edge = clamp(1.0 - normalView, 0.0, 1.0);
                            float rimFresnel = pow(edge, 4.0);

                            // Source's real local-light attenuation and model LODs
                            // naturally suppress sub-pixel rim highlights. The editor
                            // uses one unattenuated preview light, so explicitly fade
                            // the rim as geometry recedes to prevent distant props from
                            // turning into bright silhouettes. These values are Source
                            // world units and can be tuned without changing materials.
                            const float rimFadeStart = 256.0;
                            const float rimFadeEnd = 2048.0;
                            float rimViewDistance = length(uCameraPosition - vWorldPosition);
                            float rimDistanceFade = 1.0 - smoothstep(
                                rimFadeStart, rimFadeEnd, rimViewDistance);
                            rimDistanceFade *= rimDistanceFade;

                            float rimMultiply = rimMask * rimFresnel * rimDistanceFade;
                            float rimExponent = clamp(uRimLightExponent, 1.0, 128.0);
                            float rimLobe = pow(normalHalf, rimExponent);
                            float lightVisibility = max(normalLight, 0.0);
                            vec3 directRimLighting = vec3(rimLobe * lightVisibility);

                            // Source folds direct rim into specular with max(), so
                            // it cannot add the same light twice.
                            materialHighlight = max(materialHighlight,
                                directRimLighting * rimMultiply);

                            // The real shader samples the ambient cube in the eye
                            // direction and applies $rimlightboost only here. The
                            // editor has no compiled ambient cube, so use a small,
                            // neutral approximation instead of a full white halo.
                            float upwardMask = max(normal.z, 0.0);
                            float ambientRimAmount = clamp(uRimLightBoost, 0.0, 16.0) *
                                clamp(rimMultiply * upwardMask, 0.0, 1.0);
                            materialHighlight += vec3(0.08) * ambientRimAmount;
                        }

                        // Ordinary materials use a restrained editor tonemap to
                        // prevent Phong/envmap settings from washing the albedo white.
                        // TF2 glow/Über materials deliberately output high-energy colour;
                        // retain that energy and add it directly instead of passing it
                        // through the generic highlight compressor.
                        if (uHighEnergyEffect == 0) {
                            materialHighlight = materialHighlight /
                                                (vec3(1.0) + materialHighlight);
                        } else {
                            materialHighlight = min(materialHighlight, vec3(4.0));
                        }
                        float materialEffectIntensity = max(
                            clamp(uPhongIntensity, 0.0, 4.0),
                            clamp(uSpecularIntensity, 0.0, 4.0));
                        materialHighlight *= clamp(0.80 + materialEffectIntensity * 0.12,
                                                   0.0, 1.28);
                        if (uHighEnergyEffect != 0) {
                            linearTexture += materialHighlight;
                        } else {
                            linearTexture += materialHighlight *
                                max(vec3(1.0) - linearTexture, vec3(0.12));
                        }
                        vec3 shadedColor = pow(max(linearTexture, vec3(0.0)),
                                               vec3(1.0 / 2.2));
                        fragmentColor.rgb = clamp((shadedColor - vec3(0.5)) * 1.06 +
                                            vec3(0.515), 0.0, 1.0);
                    }
                }
                // Opaque Source materials frequently use base-texture alpha as
                // a phong/specular or tint mask. Do not discard those texels:
                // force opaque alpha before applying transparency rejection.
                if (uLightmapGrid != 0) {
                #if LIGHTMAP_FWIDTH
                    vec2 lmWidth = max(fwidth(vLightmapCoord), vec2(0.000001));
                    vec2 lmDist = abs(fract(vLightmapCoord + 0.5) - 0.5) / lmWidth;
                    float lmLine = 1.0 - clamp(min(lmDist.x, lmDist.y), 0.0, 1.0);
                    // Fade the grid out when the luxels shrink below ~2 pixels
                    // so distant faces do not saturate into solid yellow.
                    lmLine *= 1.0 - clamp(max(lmWidth.x, lmWidth.y) * 0.5, 0.0, 1.0);
                #else
                    vec2 lmCell = abs(fract(vLightmapCoord + 0.5) - 0.5);
                    float lmLine = min(lmCell.x, lmCell.y) < 0.06 ? 1.0 : 0.0;
                #endif
                    fragmentColor.rgb = mix(fragmentColor.rgb,
                                            vec3(1.0, 0.8157, 0.251), lmLine * 0.59);
                }
                fragmentColor.a *= uMaterialAlpha;
                // Detail props carry the engine's per-object distance fade in
                // the blend-alpha channel, which no other draw using this path
                // consumes as an alpha.
                if (uVertexAlpha != 0) fragmentColor.a *= vBlendAlpha;
                if (uAlphaTest != 0) {
                    if (fragmentColor.a < uAlphaTestReference) discard;
                    fragmentColor.a = 1.0;
                } else if (uForceOpaque != 0) fragmentColor.a = 1.0;
                else if (fragmentColor.a < 0.01) discard;
            }
        )GLSL";

        // Only desktop OpenGL 4.6 is supported, so there is exactly one program.
        const char* vertexShader = DesktopVertexShader;
        const char* fragmentShader = DesktopFragmentShader;
        program_->bindAttributeLocation("aPosition", 0);
        program_->bindAttributeLocation("aTexCoord", 1);
        program_->bindAttributeLocation("aNormal", 2);
        program_->bindAttributeLocation("aTexCoord2", 3);
        program_->bindAttributeLocation("aBlendAlpha", 4);
        program_->bindAttributeLocation("aTangent", 5);
        program_->bindAttributeLocation("aBoneWeights", 6);
        program_->bindAttributeLocation("aBoneIndices", 7);
        program_->bindAttributeLocation("aLightmapCoord", 8);
        // Cacheable, not plain, source: Qt then stores the linked program binary
        // on disk keyed by source hash + driver, so the remaining views in this
        // document and every later launch skip compiling this shader. Each view
        // owns an unshared context, so the same program is otherwise built from
        // scratch once per viewport. Qt falls back to a normal compile when the
        // driver has no program-binary support, and QT_DISABLE_SHADER_DISK_CACHE
        // turns it off for debugging.
        if (!program_->addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
            !program_->addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader) ||
            !program_->link()) {
            error_ = program_->log();
            ready_ = false;
            return false;
        }

        glGenBuffers(1, &vbo_);
        if (useVertexArray_) {
            glGenVertexArrays(1, &vao_);
            glBindVertexArray(vao_);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            configureVertexAttributes();
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindVertexArray(0);
        }

        ready_ = vbo_ != 0 && (!useVertexArray_ || vao_ != 0);
        if (!ready_ && error_.isEmpty()) error_ = QStringLiteral("Unable to allocate OpenGL buffers");
        if (ready_) {
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            description_ = QStringLiteral("%1 %2 — %3")
                .arg(isOpenGles_ ? QStringLiteral("OpenGL ES") : QStringLiteral("OpenGL"))
                .arg(version ? QString::fromLatin1(version) : QStringLiteral("unknown"))
                .arg(renderer ? QString::fromLatin1(renderer) : QStringLiteral("unknown renderer"));
        }
        return ready_;
    }

    void release()
    {
        if (!initialized_) return;
        clearTextures();
        clearWorldCache();
        clearStudioGpuCache();
        clearOrthographicCache();
        clearOrthographicModelLineCache();
        releaseSceneCapture();
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        vbo_ = 0;
        vao_ = 0;
        program_.reset();
        ready_ = false;
        initialized_ = false;
        materialOwner_ = nullptr;
        studioModels_.reset();
    }

    void invalidateMaterialCache()
    {
        clearTexturesPending_ = true;
        clearGeometryPending_ = true;
    }

    void invalidateGeometryCache()
    {
        clearGeometryPending_ = true;
    }

    void setBakedCubemaps(std::vector<hammer::render::BakedCubemap> cubemaps)
    {
        bakedCubemaps_ = std::move(cubemaps);
        bakedCubemapTexturesPending_ = true;
        // Probe assignment is baked into the batch split, so the world geometry
        // has to be re-batched whenever the probe set changes.
        clearGeometryPending_ = true;
    }

    QString error() const { return error_; }

    QString description() const
    {
        return description_.isEmpty() ? QStringLiteral("OpenGL renderer not initialized") : description_;
    }

    void renderOrthographicScene(const hammer::vmf::Scene* scene,
                                 const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                                 MapViewWidget::Kind kind,
                                 int width, int height, float centerX, float centerY,
                                 float zoom, float deviceScale, bool gridVisible,
                                 int gridSpacing,
                                 const std::vector<hammer::vmf::ObjectRef>& selection,
                                 MapViewWidget::SelectionMode selectionMode,
                                 const std::vector<int>& editingSolidIds,
                                 const std::unordered_set<std::string>& hiddenToolTextures,
                                 const QRectF& handleBounds,
                                 const std::array<QPointF, 4>& translateHandles,
                                 bool drawHandleBox,
                                 bool showTransformHandles,
                                 MapViewWidget::TransformMode transformMode,
                                 bool creating,
                                 const QPointF& creationStart,
                                 const QPointF& creationCurrent,
                                 const std::vector<GpuVertex>& morphLines = {},
                                 const std::vector<GpuVertex>& morphDispLines = {})
    {
        if (!ready_ && !initialize()) return;

        if (clearTexturesPending_ || materialOwner_ != materials.get()) {
            clearTextures();
            clearWorldCache();
            clearStudioGpuCache();
            clearOrthographicCache();
            clearOrthographicModelLineCache();
            clearTexturesPending_ = false;
            clearGeometryPending_ = false;
            materialOwner_ = materials.get();
            studioModels_ = materials && materials->fileSystem()
                ? std::make_unique<hammer::assets::StudioModelSystem>(materials->fileSystem())
                : nullptr;
        }
        // Selection and face-selection changes alone do NOT rebuild the static
        // buffers. The solids the static buffer excluded stay recorded in
        // orthographicExcludedSolids_ and keep drawing from the small editing
        // buffer, so a click costs O(selection); classifySceneCache forces the
        // full rebuild once excluded-vs-dynamic membership actually matters
        // (the first drag step whose solids are not in the dynamic set).
        const bool orthographicSelectionChanged =
            orthographicSelection_ != selection ||
            orthographicSelectionMode_ != selectionMode ||
            orthographicEditingSolids_ != editingSolidIds;
        const SceneCacheState orthographicState =
            classifySceneCache(orthographicLineage_, scene);
        if (clearGeometryPending_ || orthographicState == SceneCacheState::Rebuild ||
            orthographicMaterialOwner_ != materials.get() ||
            orthographicHiddenToolTextures_ != hiddenToolTextures) {
            orthographicHiddenToolTextures_ = hiddenToolTextures;
            rebuildOrthographicCache(scene, materials.get(), selection, selectionMode,
                                     editingSolidIds);
            clearGeometryPending_ = false;
        } else if (orthographicState == SceneCacheState::DynamicOnly) {
            // Only the selected objects moved. Keep every static buffer and let
            // the selection and editing buffers below pick up the new geometry.
            orthographicLineage_.revision = scene->revision;
        }

        width = std::max(1, width);
        height = std::max(1, height);
        zoom = std::max(0.0001f, zoom);
        const float spacing = std::max(4.0f * deviceScale,
                                       static_cast<float>(std::max(1, gridSpacing)) * zoom * deviceScale);
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 36.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        program_->bind();
        QMatrix4x4 identity;
        program_->setUniformValue("uModel", identity);
        program_->setUniformValue("uUseTexture", 0);
        program_->setUniformValue("uHasTexture2", 0);
        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uShaded", 0);
        program_->setUniformValue("uForceOpaque", 1);
        clearMaterialEffects();
        if (useVertexArray_) glBindVertexArray(vao_);

        auto appendPixelLine = [width, height](std::vector<GpuVertex>& target,
                                               float ax, float ay, float bx, float by) {
            const auto ndcX = [width](float x) { return x * 2.0f / static_cast<float>(width) - 1.0f; };
            const auto ndcY = [height](float y) { return 1.0f - y * 2.0f / static_cast<float>(height); };
            target.push_back({ndcX(ax), ndcY(ay), 0.0f, 0.0f, 0.0f});
            target.push_back({ndcX(bx), ndcY(by), 0.0f, 0.0f, 0.0f});
        };

        // Grid and axes are generated in framebuffer coordinates. All map
        // geometry below stays in world coordinates in persistent VBOs.
        program_->setUniformValue("uViewProjection", identity);
        if (gridVisible) {
            std::vector<GpuVertex> minor;
            std::vector<GpuVertex> major;
            float startX = std::fmod(centerX, spacing);
            if (startX < 0.0f) startX += spacing;
            for (float x = startX; x < static_cast<float>(width); x += spacing) {
                const int index = static_cast<int>(std::llround((x - centerX) / spacing));
                appendPixelLine((index % 8) == 0 ? major : minor,
                                x, 0.0f, x, static_cast<float>(height));
            }
            float startY = std::fmod(centerY, spacing);
            if (startY < 0.0f) startY += spacing;
            for (float y = startY; y < static_cast<float>(height); y += spacing) {
                const int index = static_cast<int>(std::llround((y - centerY) / spacing));
                appendPixelLine((index % 8) == 0 ? major : minor,
                                0.0f, y, static_cast<float>(width), y);
            }
            glLineWidth(std::max(1.0f, deviceScale));
            program_->setUniformValue("uColor", QVector4D(22.0f / 255.0f, 32.0f / 255.0f,
                                                           62.0f / 255.0f, 1.0f));
            uploadAndDraw(minor, GL_LINES);
            program_->setUniformValue("uColor", QVector4D(45.0f / 255.0f, 58.0f / 255.0f,
                                                           92.0f / 255.0f, 1.0f));
            uploadAndDraw(major, GL_LINES);
        }

        std::vector<GpuVertex> axis;
        glLineWidth(std::max(1.0f, deviceScale));
        appendPixelLine(axis, 0.0f, centerY, static_cast<float>(width), centerY);
        program_->setUniformValue("uColor", QVector4D(130.0f / 255.0f, 36.0f / 255.0f,
                                                       36.0f / 255.0f, 1.0f));
        uploadAndDraw(axis, GL_LINES);
        axis.clear();
        appendPixelLine(axis, centerX, 0.0f, centerX, static_cast<float>(height));
        program_->setUniformValue("uColor", QVector4D(36.0f / 255.0f, 130.0f / 255.0f,
                                                       60.0f / 255.0f, 1.0f));
        uploadAndDraw(axis, GL_LINES);

        const float logicalZoom = zoom;
        const float logicalCenterX = centerX / std::max(0.0001f, deviceScale);
        const float logicalCenterY = centerY / std::max(0.0001f, deviceScale);
        const float logicalWidth = static_cast<float>(width) / std::max(0.0001f, deviceScale);
        const float logicalHeight = static_cast<float>(height) / std::max(0.0001f, deviceScale);
        const float sx = 2.0f * logicalZoom / std::max(1.0f, logicalWidth);
        const float sy = 2.0f * logicalZoom / std::max(1.0f, logicalHeight);
        const float tx = 2.0f * logicalCenterX / std::max(1.0f, logicalWidth) - 1.0f;
        const float ty = 1.0f - 2.0f * logicalCenterY / std::max(1.0f, logicalHeight);
        QMatrix4x4 projection;
        if (kind == MapViewWidget::Kind::Top) {
            projection = QMatrix4x4(sx, 0, 0, tx,
                                    0, sy, 0, ty,
                                    0, 0, 0, 0,
                                    0, 0, 0, 1);
        } else if (kind == MapViewWidget::Kind::Front) {
            projection = QMatrix4x4(0, sx, 0, tx,
                                    0, 0, sy, ty,
                                    0, 0, 0, 0,
                                    0, 0, 0, 1);
        } else {
            projection = QMatrix4x4(sx, 0, 0, tx,
                                    0, 0, sy, ty,
                                    0, 0, 0, 0,
                                    0, 0, 0, 1);
        }
        program_->setUniformValue("uViewProjection", projection);
        glLineWidth(std::max(1.0f, deviceScale));
        program_->setUniformValue("uColor", QVector4D(220.0f / 255.0f, 220.0f / 255.0f,
                                                       220.0f / 255.0f, 1.0f));
        drawOrthographicBuffer(orthographicBrushVbo_, orthographicBrushVao_,
                               orthographicBrushCount_);
        // Face-selected solids (displacement paint, texture application) change
        // per mouse-move without being in the object selection, and solids the
        // static buffer excluded for a since-changed selection still need to be
        // drawn. Both live in this small per-revision buffer so a paint drag
        // or a selection click re-uploads only them.
        if (orthographicEditingRevision_ != (scene ? scene->revision : 0) ||
            orthographicSelectionChanged) {
            rebuildOrthographicEditingCache(scene, selection, selectionMode, editingSolidIds);
        }
        drawOrthographicBuffer(orthographicEditingVbo_, orthographicEditingVao_,
                               orthographicEditingCount_);
        for (const OrthographicGpuBatch& batch : orthographicEntityBatches_) {
            program_->setUniformValue("uColor", batch.color);
            drawOrthographicBuffer(batch.vbo, batch.vao, batch.count);
        }
        for (const OrthographicGpuBatch& batch : orthographicEditingEntityBatches_) {
            program_->setUniformValue("uColor", batch.color);
            drawOrthographicBuffer(batch.vbo, batch.vao, batch.count);
        }
        drawOrthographicModelDraws(orthographicModelDraws_);
        drawOrthographicModelDraws(orthographicEditingModelDraws_);

        if (orthographicSelection_ != selection ||
            orthographicSelectionMode_ != selectionMode ||
            orthographicSelectionRevision_ != (scene ? scene->revision : 0)) {
            rebuildOrthographicSelectionCache(scene, selection, selectionMode);
        }
        // Hammer draws selected geometry red in the 2D views; the yellow stays
        // reserved for the transform bounds and handles below so a selection
        // reads distinctly from its own resize box.
        const QVector4D selectionColor(1.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f);
        glLineWidth(std::max(2.0f, 2.0f * deviceScale));
        program_->setUniformValue("uColor", selectionColor);
        drawOrthographicBuffer(orthographicSelectionVbo_, orthographicSelectionVao_,
                               orthographicSelectionCount_);
        drawOrthographicModelDraws(orthographicSelectionModelDraws_, &selectionColor);
        // Prop selection bounds: the same red corner box the perspective view
        // draws, on top of the model wireframe.
        drawOrthographicBuffer(orthographicSelectionBoundsVbo_, orthographicSelectionBoundsVao_,
                               orthographicSelectionBoundsCount_);

        // Vertex-tool mesh outlines and displacement grid edges (world space,
        // so they must draw before the screen-space overlay switches the
        // projection to identity).
        if (!morphLines.empty()) {
            program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
            glLineWidth(std::max(1.0f, deviceScale));
            uploadAndDraw(morphLines, GL_LINES);
        }
        if (!morphDispLines.empty()) {
            program_->setUniformValue("uColor", QVector4D(1.0f, 0.65f, 0.1f, 0.9f));
            glLineWidth(std::max(1.0f, deviceScale));
            uploadAndDraw(morphDispLines, GL_LINES);
        }

        // Selection bounds, transform handles, and block creation are also GPU
        // primitives. QPainter is no longer part of the orthographic scene path.
        program_->setUniformValue("uViewProjection", identity);
        std::vector<GpuVertex> screenOverlay;
        auto appendRect = [&](const QRectF& rect) {
            appendPixelLine(screenOverlay, rect.left() * deviceScale, rect.top() * deviceScale,
                            rect.right() * deviceScale, rect.top() * deviceScale);
            appendPixelLine(screenOverlay, rect.right() * deviceScale, rect.top() * deviceScale,
                            rect.right() * deviceScale, rect.bottom() * deviceScale);
            appendPixelLine(screenOverlay, rect.right() * deviceScale, rect.bottom() * deviceScale,
                            rect.left() * deviceScale, rect.bottom() * deviceScale);
            appendPixelLine(screenOverlay, rect.left() * deviceScale, rect.bottom() * deviceScale,
                            rect.left() * deviceScale, rect.top() * deviceScale);
        };
        if (showTransformHandles && handleBounds.isValid()) {
            // The box and handle positions come from the widget's
            // selectionScreenBounds()/translateHandlePositions() (logical
            // pixels), the same rects hitHandle() tests, so what is drawn is
            // exactly what is clickable — including the entity-icon box a
            // point entity's collapsed document bounds would otherwise miss.
            const QRectF& bounds = handleBounds;
            if (drawHandleBox) appendRect(bounds);
            const std::array<QPointF, 8> handles{{
                bounds.topLeft(), QPointF(bounds.center().x(), bounds.top()), bounds.topRight(),
                QPointF(bounds.right(), bounds.center().y()), bounds.bottomRight(),
                QPointF(bounds.center().x(), bounds.bottom()), bounds.bottomLeft(),
                QPointF(bounds.left(), bounds.center().y())}};
            switch (transformMode) {
            case MapViewWidget::TransformMode::Scale:
                for (const QPointF& handle : handles)
                    appendRect(QRectF(handle.x() - 3.0, handle.y() - 3.0, 7.0, 7.0));
                break;
            case MapViewWidget::TransformMode::Translate:
                for (const QPointF& handle : translateHandles)
                    appendRect(QRectF(handle.x() - 3.0, handle.y() - 3.0, 7.0, 7.0));
                break;
            case MapViewWidget::TransformMode::Rotate:
                for (int index : {0, 2, 4, 6})
                    appendRect(QRectF(handles[static_cast<std::size_t>(index)].x() - 4.0,
                                      handles[static_cast<std::size_t>(index)].y() - 4.0, 9.0, 9.0));
                appendRect(QRectF(bounds.center().x() - 2.0, bounds.center().y() - 2.0, 5.0, 5.0));
                break;
            }
        }
        if (creating) {
            const QPointF a(logicalCenterX + creationStart.x() * logicalZoom,
                            logicalCenterY - creationStart.y() * logicalZoom);
            const QPointF b(logicalCenterX + creationCurrent.x() * logicalZoom,
                            logicalCenterY - creationCurrent.y() * logicalZoom);
            const QRectF box = QRectF(a, b).normalized();
            appendRect(box);
            // The pending (unconfirmed) box is resizable, so it carries the
            // same eight handle squares the transform bounds show.
            const std::array<QPointF, 8> boxHandles{{
                box.topLeft(), QPointF(box.center().x(), box.top()), box.topRight(),
                QPointF(box.right(), box.center().y()), box.bottomRight(),
                QPointF(box.center().x(), box.bottom()), box.bottomLeft(),
                QPointF(box.left(), box.center().y())}};
            for (const QPointF& handle : boxHandles)
                appendRect(QRectF(handle.x() - 3.0, handle.y() - 3.0, 7.0, 7.0));
        }
        glLineWidth(std::max(1.0f, deviceScale));
        program_->setUniformValue("uColor", creating
            ? QVector4D(64.0f / 255.0f, 224.0f / 255.0f, 1.0f, 1.0f)
            : QVector4D(1.0f, 240.0f / 255.0f, 32.0f / 255.0f, 1.0f));
        uploadAndDraw(screenOverlay, GL_LINES);
        glLineWidth(1.0f);

        if (useVertexArray_) glBindVertexArray(0);
        program_->release();
    }

    // The widget owns the render target; the water pass needs its handle so it
    // can copy the colour and depth already drawn behind the surface.
    void setSceneTarget(GLuint framebuffer, bool hasDepthTexture, GLenum depthFormat)
    {
        sceneFramebuffer_ = framebuffer;
        sceneHasDepthTexture_ = hasDepthTexture;
        sceneDepthFormat_ = depthFormat;
    }

    // Copies the opaque scene colour and depth into private textures so the
    // water shader can read what is behind the surface. This is the editor
    // equivalent of Source's $refracttexture grab pass, and it must run after
    // every opaque depth-writing draw. Returns false when the context cannot
    // support it, in which case water keeps its cubemap-only path.
    bool captureSceneBehindWater()
    {
        if (!sceneFramebuffer_ || !sceneHasDepthTexture_ || !modernContext_) return false;
        if (renderWidth_ <= 0 || renderHeight_ <= 0) return false;

        if (sceneCaptureWidth_ != renderWidth_ || sceneCaptureHeight_ != renderHeight_) {
            releaseSceneCapture();
            glGenTextures(1, &sceneColorCopy_);
            glBindTexture(GL_TEXTURE_2D, sceneColorCopy_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, renderWidth_, renderHeight_, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            setCaptureSampling();
            glGenTextures(1, &sceneDepthCopy_);
            glBindTexture(GL_TEXTURE_2D, sceneDepthCopy_);
            // Must match the source attachment exactly or the depth blit fails.
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(sceneDepthFormat_),
                         renderWidth_, renderHeight_, 0, GL_DEPTH_COMPONENT,
                         GL_UNSIGNED_INT, nullptr);
            setCaptureSampling();
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &sceneCaptureFramebuffer_);
            glBindFramebuffer(GL_FRAMEBUFFER, sceneCaptureFramebuffer_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D, sceneDepthCopy_, 0);
            const bool complete =
                glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
            if (!complete) {
                releaseSceneCapture();
                return false;
            }
            sceneCaptureWidth_ = renderWidth_;
            sceneCaptureHeight_ = renderHeight_;
        }

        // Colour comes from the bound target; glCopyTexSubImage2D is universally
        // available. Depth cannot be copied that way, so it is blitted into a
        // companion target of the identical format.
        glBindTexture(GL_TEXTURE_2D, sceneColorCopy_);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, renderWidth_, renderHeight_);
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFramebuffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneCaptureFramebuffer_);
        glBlitFramebuffer(0, 0, renderWidth_, renderHeight_,
                          0, 0, renderWidth_, renderHeight_,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
        return true;
    }

    void releaseSceneCapture()
    {
        if (sceneCaptureFramebuffer_) glDeleteFramebuffers(1, &sceneCaptureFramebuffer_);
        if (sceneColorCopy_) glDeleteTextures(1, &sceneColorCopy_);
        if (sceneDepthCopy_) glDeleteTextures(1, &sceneDepthCopy_);
        sceneCaptureFramebuffer_ = 0;
        sceneColorCopy_ = 0;
        sceneDepthCopy_ = 0;
        sceneCaptureWidth_ = 0;
        sceneCaptureHeight_ = 0;
    }

    void setCaptureSampling()
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void render(const hammer::vmf::Scene* scene,
                const hammer::camera::State& camera,
                hammer::camera::ProjectionMode projectionMode,
                int width, int height,
                bool gridVisible,
                bool materialsEnabled,
                bool shadedTexturing,
                bool phongEnabled,
                bool specularEnabled,
                bool bumpMapsEnabled,
                bool lightWarpEnabled,
                bool selfIllumEnabled,
                bool rimLightEnabled,
                float phongIntensity,
                float specularIntensity,
                float bumpMapIntensity,
                const std::shared_ptr<hammer::assets::MaterialSystem>& materials,
                const std::unordered_set<std::string>& hiddenToolTextures,
                bool displacementSolidMask,
                const std::vector<hammer::vmf::ObjectRef>& selection,
                bool lightmapGrid,
                const std::vector<GpuVertex>& morphLines = {},
                const std::vector<GpuVertex>& morphDispLines = {},
                bool gizmoVisible = false,
                const hammer::vmf::Vec3& gizmoOrigin = {},
                double gizmoLength = 0.0,
                int gizmoActiveAxis = -1)
    {
        if (!ready_ && !initialize()) return;

        const auto now = std::chrono::steady_clock::now();
        if (animationScene_ != scene) {
            // Sequences begin when a scene/preview is installed, not at an
            // arbitrary time since the OpenGL renderer was constructed.
            animationScene_ = scene;
            animationStart_ = now;
        }

        if (clearTexturesPending_ || materialOwner_ != materials.get()) {
            clearTextures();
            clearWorldCache();
            clearStudioGpuCache();
            clearOrthographicCache();
            clearOrthographicModelLineCache();
            clearTexturesPending_ = false;
            clearGeometryPending_ = false;
            materialOwner_ = materials.get();
            studioModels_ = materials && materials->fileSystem()
                ? std::make_unique<hammer::assets::StudioModelSystem>(materials->fileSystem())
                : nullptr;
        }

        shadedTexturing_ = shadedTexturing;
        phongEnabled_ = phongEnabled;
        specularEnabled_ = specularEnabled;
        bumpMapsEnabled_ = bumpMapsEnabled;
        lightWarpEnabled_ = lightWarpEnabled;
        selfIllumEnabled_ = selfIllumEnabled;
        rimLightEnabled_ = rimLightEnabled;
        phongIntensity_ = std::clamp(phongIntensity, 0.0f, 4.0f);
        specularIntensity_ = std::clamp(specularIntensity, 0.0f, 4.0f);
        bumpMapIntensity_ = std::clamp(bumpMapIntensity, 0.0f, 4.0f);

        glViewport(0, 0, std::max(1, width), std::max(1, height));
        renderWidth_ = width;
        renderHeight_ = height;
        sceneNearPlane_ = camera.nearPlane;
        glClearColor(0.047f, 0.047f, 0.047f, 1.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glCullFace(GL_BACK);
        // VMF face loops and Source studio-model triangle strips use Source's
        // clockwise front-face convention after projection through the corrected,
        // non-reflected camera basis. The old mirrored view changed their screen
        // winding and accidentally made GL_CCW appear correct. Keep the camera
        // handedness fix and declare the actual mesh winding instead.
        glFrontFace(GL_CW);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        const QMatrix4x4 matrix = viewProjection(camera, projectionMode, width, height);
        viewProjectionMatrix_ = matrix;
        program_->bind();
        program_->setUniformValue("uViewProjection", matrix);
        program_->setUniformValue("uGpuSkinning", 0);
        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        program_->setUniformValue("uTexture", 0);
        program_->setUniformValue("uWaterFlowMap", 1);
        program_->setUniformValue("uTexture2", 2);
        program_->setUniformValue("uBumpMap", 3);
        // Dedicated units: the per-batch material setup rebinds 0-7 every draw,
        // which would clobber the capture mid-pass. GL3/ES3 both guarantee at
        // least 16 combined units.
        program_->setUniformValue("uSceneColor", 8);
        program_->setUniformValue("uSceneDepth", 9);
        program_->setUniformValue("uHasSceneCapture", 0);
        program_->setUniformValue("uEnvironmentMap", 4);
        program_->setUniformValue("uPhongExponentMap", 5);
        program_->setUniformValue("uSelfIllumMask", 6);
        program_->setUniformValue("uLightWarpTexture", 7);
        program_->setUniformValue("uHasTexture2", 0);
        program_->setUniformValue("uCameraPosition", vector(camera.position));
        program_->setUniformValue("uPhongIntensity", phongIntensity_);
        program_->setUniformValue("uSpecularIntensity", specularIntensity_);
        program_->setUniformValue("uBumpMapIntensity", bumpMapIntensity_);
        animationSeconds_ = std::chrono::duration<double>(now - animationStart_).count();
        program_->setUniformValue("uWaterScrollOffsetA", QVector2D(0.0f, 0.0f));
        program_->setUniformValue("uWaterScrollOffsetB", QVector2D(0.0f, 0.0f));
        program_->setUniformValue("uWaterScale", QVector2D(1.0f, 1.0f));
        program_->setUniformValue("uWaterMultiTexture", 0);
        program_->setUniformValue("uWaterFlowPhase", 0.0f);
        program_->setUniformValue("uWaterReflectAmount", 0.80f);
        program_->setUniformValue("uWaterRefractAmount", 0.0f);
        program_->setUniformValue("uWaterReflectBlendFactor", 1.0f);
        program_->setUniformValue("uWaterFogStart", 0.0f);
        program_->setUniformValue("uWaterFogEnd", 0.0f);
        program_->setUniformValue("uWaterNoFresnel", 0);
        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        clearMaterialEffects();
        program_->setUniformValue("uShaded", shadedTexturing ? 1 : 0);
        program_->setUniformValue("uForceOpaque", 0);
        program_->setUniformValue("uUseBumpMap", 0);
        program_->setUniformValue("uHasBumpMap", 0);
        program_->setUniformValue("uUsePhong", 0);
        program_->setUniformValue("uUseSpecular", 0);
        program_->setUniformValue("uUseSelfIllum", 0);
        program_->setUniformValue("uUseSelfIllumFresnel", 0);
        program_->setUniformValue("uUseLightWarp", 0);
        program_->setUniformValue("uHalfLambert", 0);
        program_->setUniformValue("uUseRimLight", 0);
        program_->setUniformValue("uHasSelfIllumMask", 0);
        program_->setUniformValue("uRimMaskFromExponentAlpha", 0);
        program_->setUniformValue("uPhongMaskFromBaseAlpha", 0);
        program_->setUniformValue("uSpecularMaskMode", 0);
        program_->setUniformValue("uInvertSpecularMask", 0);
        program_->setUniformValue("uHasPhongExponentMap", 0);
        program_->setUniformValue("uPhongExponentOverride", 0);
        program_->setUniformValue("uPhongAlbedoTint", 0);
        program_->setUniformValue("uPhongFresnelRanges", QVector3D(0.0f, 0.5f, 1.0f));
        program_->setUniformValue("uPhongTint", QVector3D(1.0f, 1.0f, 1.0f));
        program_->setUniformValue("uEnvMapTint", QVector3D(1.0f, 1.0f, 1.0f));
        program_->setUniformValue("uEnvMapContrast", 0.0f);
        program_->setUniformValue("uEnvMapSaturation", 1.0f);
        program_->setUniformValue("uPhongExponent", 150.0f);
        program_->setUniformValue("uPhongBoost", 1.0f);
        program_->setUniformValue("uSpecularStrength", 0.18f);
        program_->setUniformValue("uSelfIllumTint", QVector3D(1.0f, 1.0f, 1.0f));
        program_->setUniformValue("uColor2", QVector3D(1.0f, 1.0f, 1.0f));
        program_->setUniformValue("uBlendTintByBaseAlpha", 0);
        program_->setUniformValue("uBlendTintColorOverBase", 0.0f);
        program_->setUniformValue("uHighEnergyEffect", 0);
        program_->setUniformValue("uRimLightExponent", 4.0f);
        program_->setUniformValue("uRimLightBoost", 1.0f);
        program_->setUniformValue("uPhongIntensity", phongIntensity_);
        program_->setUniformValue("uSpecularIntensity", specularIntensity_);
        program_->setUniformValue("uBumpMapIntensity", bumpMapIntensity_);
        const bool hasEnvironment = scene && materials &&
            ensureEnvironmentCubeMap(scene->skyName, *materials);
        program_->setUniformValue("uHasEnvironmentMap", hasEnvironment ? 1 : 0);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, hasEnvironment ? environmentCubeMap_ : 0);
        glActiveTexture(GL_TEXTURE0);
        if (useVertexArray_) glBindVertexArray(vao_);

        if (scene && materialsEnabled && materials && !scene->skyName.empty()) {
            drawSkybox(scene->skyName, *materials, camera);
            program_->setUniformValue("uShaded", shadedTexturing ? 1 : 0);
        }
        if (gridVisible) drawGrid(camera);
        hasAnimatedWater_ = false;
        hasAnimatedModels_ = false;
        hasAnimatedMaterials_ = false;
        if (scene && materialsEnabled && materials) {
            drawMaterials(*scene, *materials, hiddenToolTextures, displacementSolidMask,
                          selection, lightmapGrid);
            program_->setUniformValue("uLightmapGrid", 0);
            drawProjectedSurfaces();
        }
        if (scene && materials) {
            drawEntityHelpers(*scene, *materials, camera, shadedTexturing,
                              HelperPhase::Models);
            // Ropes and detail props are world content, not editor aids: draw
            // them with the props so the water pass below refracts them.
            if (materialsEnabled) drawRopes(*scene, *materials, camera, shadedTexturing);
            if (materialsEnabled && detailPropsVisible_)
                drawDetailProps(*scene, *materials, camera, shadedTexturing);
        }
        // Water last among the opaque content: it samples everything above.
        if (scene && materialsEnabled && materials) drawWaterSurfaces();
        if (scene && materials) {
            drawEntityHelpers(*scene, *materials, camera, shadedTexturing,
                              HelperPhase::Helpers);
        }
        if (scene) drawDisplacementVertices(*scene, selection);
        if (scene) drawSelectedEntityBounds(*scene, selection);

        // Vertex-tool mesh outlines and displacement grid edges, always on top.
        if (!morphLines.empty() || !morphDispLines.empty()) {
            program_->setUniformValue("uUseTexture", 0);
            glDisable(GL_DEPTH_TEST);
            glLineWidth(1.0f);
            if (!morphLines.empty()) {
                program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
                uploadAndDraw(morphLines, GL_LINES);
            }
            if (!morphDispLines.empty()) {
                program_->setUniformValue("uColor", QVector4D(1.0f, 0.65f, 0.1f, 0.9f));
                uploadAndDraw(morphDispLines, GL_LINES);
            }
            glEnable(GL_DEPTH_TEST);
        }
        if (gizmoVisible) drawMoverGizmo(gizmoOrigin, gizmoLength, gizmoActiveAxis);

        // The renderer owns this private context, so textures do not need to be
        // unbound from all eight units after every frame. The next draw path
        // explicitly binds every texture it consumes; keeping the cache hot
        // avoids a large tail of redundant driver calls.
        glActiveTexture(GL_TEXTURE0);
        if (useVertexArray_) glBindVertexArray(0);
        program_->release();
        glDisable(GL_BLEND);
    }

private:
    void clearTextures()
    {
        for (auto& [name, texture] : textures_) {
            Q_UNUSED(name);
            if (texture.id) glDeleteTextures(1, &texture.id);
            if (texture.secondaryId) glDeleteTextures(1, &texture.secondaryId);
            if (texture.flowId) glDeleteTextures(1, &texture.flowId);
            if (texture.bumpId) glDeleteTextures(1, &texture.bumpId);
            for (const GLuint frame : texture.bumpFrameIds)
                if (frame && frame != texture.bumpId) glDeleteTextures(1, &frame);
            if (texture.phongExponentId) glDeleteTextures(1, &texture.phongExponentId);
            if (texture.selfIllumMaskId) glDeleteTextures(1, &texture.selfIllumMaskId);
            if (texture.lightWarpId) glDeleteTextures(1, &texture.lightWarpId);
        }
        textures_.clear();
        for (auto& [name, cubeMap] : materialEnvironmentCubeMaps_) {
            Q_UNUSED(name);
            if (cubeMap) glDeleteTextures(1, &cubeMap);
        }
        materialEnvironmentCubeMaps_.clear();
        if (environmentCubeMap_) glDeleteTextures(1, &environmentCubeMap_);
        environmentCubeMap_ = 0;
        environmentSkyName_.clear();
        releaseBakedCubemapTextures();
    }

    void releaseBakedCubemapTextures()
    {
        for (GLuint cubeMap : bakedCubemapTextures_)
            if (cubeMap) glDeleteTextures(1, &cubeMap);
        bakedCubemapTextures_.clear();
        // The faces themselves are still held, so a later frame can re-upload.
        bakedCubemapTexturesPending_ = !bakedCubemaps_.empty();
    }

    // Uploads every baked probe once, on the first frame that needs one.
    GLuint bakedCubemapTexture(int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= bakedCubemaps_.size()) return 0;
        if (bakedCubemapTexturesPending_) {
            for (GLuint cubeMap : bakedCubemapTextures_)
                if (cubeMap) glDeleteTextures(1, &cubeMap);
            bakedCubemapTextures_.assign(bakedCubemaps_.size(), 0);
            for (std::size_t probe = 0; probe < bakedCubemaps_.size(); ++probe) {
                const auto& source = bakedCubemaps_[probe].cube.faces;
                if (!bakedCubemaps_[probe].cube.valid()) continue;
                // Same right/left/back/front/up/down -> back/left/front/right/
                // up/down reorder the authored $envmap path uses.
                const std::array<const hammer::assets::Image*, 6> faces{{
                    &source[2], &source[1], &source[3], &source[0], &source[4], &source[5]}};
                bakedCubemapTextures_[probe] =
                    uploadEnvironmentCubeMap(faces, SkyCubeKind::AuthoredCubemap);
            }
            bakedCubemapTexturesPending_ = false;
        }
        if (static_cast<std::size_t>(index) >= bakedCubemapTextures_.size()) return 0;
        return bakedCubemapTextures_[static_cast<std::size_t>(index)];
    }

    const TextureRecord& textureFor(
        const std::string& name,
        const std::shared_ptr<const hammer::assets::Material>& material)
    {
        if (const auto existing = textures_.find(name); existing != textures_.end()) {
            return existing->second;
        }
        TextureRecord record;
        if (!material || !material->image.valid()) return emptyTexture_;

        auto uploadTexture = [&](const hammer::assets::Image& image,
                                 bool generateMipmaps = true,
                                 bool clampEdges = false) -> GLuint {
            if (!image.valid()) return 0;
            const std::vector<std::uint8_t> pixels = rgbaPixels(image);
            GLuint textureId = 0;
            glGenTextures(1, &textureId);
            glBindTexture(GL_TEXTURE_2D, textureId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            clampEdges ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            clampEdges ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, modernContext_ ? GL_RGBA8 : GL_RGBA,
                         image.width, image.height,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (generateMipmaps) glGenerateMipmap(GL_TEXTURE_2D);
            return textureId;
        };

        const hammer::assets::Image& sourceImage =
            material->water && material->waterNormalImage.valid()
                ? material->waterNormalImage : material->image;
        glActiveTexture(GL_TEXTURE0);
        // Water normal fields stay on their base level: automatically averaging
        // signed slopes into deep mip levels converges to (0, 0) and turns a
        // distant water face into one flat skybox sample.
        record.id = uploadTexture(sourceImage, !material->water);
        if (!material->water && material->blended && material->image2.valid()) {
            glActiveTexture(GL_TEXTURE2);
            record.secondaryId = uploadTexture(material->image2, true);
            record.hasSecondTexture = record.secondaryId != 0;
            glActiveTexture(GL_TEXTURE0);
        }
        if (material->water && material->waterHasFlowMap && material->waterFlowImage.valid()) {
            record.flowId = uploadTexture(material->waterFlowImage);
            record.hasFlowMap = record.flowId != 0;
        }
        if (!material->water && material->bumpMapped && material->bumpImage.valid()) {
            glActiveTexture(GL_TEXTURE3);
            if (material->bumpFrames.size() > 1 && material->bumpAnimationFrameRate > 0.0f) {
                record.bumpFrameIds.reserve(material->bumpFrames.size());
                for (const hammer::assets::Image& frame : material->bumpFrames) {
                    const GLuint frameId = uploadTexture(frame, true);
                    if (frameId) record.bumpFrameIds.push_back(frameId);
                }
                if (!record.bumpFrameIds.empty()) record.bumpId = record.bumpFrameIds.front();
            } else {
                record.bumpId = uploadTexture(material->bumpImage, true);
            }
            record.hasBumpMap = record.bumpId != 0;
            glActiveTexture(GL_TEXTURE0);
        }
        if (!material->water && material->hasPhongExponentTexture &&
            material->phongExponentImage.valid()) {
            glActiveTexture(GL_TEXTURE5);
            record.phongExponentId = uploadTexture(material->phongExponentImage, true);
            record.hasPhongExponentMap = record.phongExponentId != 0;
            glActiveTexture(GL_TEXTURE0);
        }
        if (!material->water && material->hasSelfIllumMask &&
            material->selfIllumMaskImage.valid()) {
            glActiveTexture(GL_TEXTURE6);
            record.selfIllumMaskId = uploadTexture(material->selfIllumMaskImage, true);
            record.hasSelfIllumMask = record.selfIllumMaskId != 0;
            glActiveTexture(GL_TEXTURE0);
        }
        if (!material->water && material->hasLightWarpTexture &&
            material->lightWarpImage.valid()) {
            glActiveTexture(GL_TEXTURE7);
            // Lightwarp textures are one-dimensional lighting ramps. Source
            // clamps their edge texels rather than wrapping back to the dark end.
            record.lightWarpId = uploadTexture(material->lightWarpImage, false, true);
            record.hasLightWarp = record.lightWarpId != 0;
            glActiveTexture(GL_TEXTURE0);
        }
        record.width = sourceImage.width;
        record.height = sourceImage.height;
        record.translucent = material->translucent;
        record.water = material->water;
        return textures_.emplace(name, std::move(record)).first->second;
    }

    GLuint uploadEnvironmentCubeMap(
        const std::array<const hammer::assets::Image*, 6>& faces, SkyCubeKind kind)
    {
        int cubeSize = 0;
        for (const hammer::assets::Image* face : faces) {
            if (!face || !face->valid()) return 0;
            cubeSize = std::max(cubeSize, std::max(face->width, face->height));
        }
        cubeSize = std::clamp(cubeSize, 16, 1024);

        GLuint cubeMap = 0;
        glActiveTexture(GL_TEXTURE4);
        glGenTextures(1, &cubeMap);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubeMap);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!isOpenGles_ || modernContext_)
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        // Reflections sample this cube along a mirror ray whose screen-space
        // derivatives explode with distance, so any mip chain collapses to the
        // sky average and the surface reads as matte. Keep level 0 only.
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        constexpr std::array<GLenum, 6> targets{{
            GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
            GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
            GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z}};
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(cubeSize * cubeSize * 4));
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            for (int row = 0; row < cubeSize; ++row) {
                const float tc = 2.0f * (static_cast<float>(row) + 0.5f) /
                                 static_cast<float>(cubeSize) - 1.0f;
                for (int column = 0; column < cubeSize; ++column) {
                    const float sc = 2.0f * (static_cast<float>(column) + 0.5f) /
                                     static_cast<float>(cubeSize) - 1.0f;
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    switch (targetIndex) {
                    case 0: x = 1.0f;  y = -tc; z = -sc; break;
                    case 1: x = -1.0f; y = -tc; z = sc;  break;
                    case 2: x = sc; y = 1.0f;  z = tc;  break;
                    case 3: x = sc; y = -1.0f; z = -tc; break;
                    case 4: x = sc; y = -tc; z = 1.0f;  break;
                    default:x = -sc; y = -tc; z = -1.0f; break;
                    }
                    const auto sample = sampleHammerSkyDirection(faces, x, y, z, kind);
                    const std::size_t offset = static_cast<std::size_t>(
                        (row * cubeSize + column) * 4);
                    std::copy(sample.begin(), sample.end(), pixels.begin() +
                              static_cast<std::ptrdiff_t>(offset));
                }
            }
            glTexImage2D(targets[targetIndex], 0, modernContext_ ? GL_RGBA8 : GL_RGBA,
                         cubeSize, cubeSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glActiveTexture(GL_TEXTURE0);
        return cubeMap;
    }

    // HAMMER_FORCE_SKYBOX replaces the six sky faces with flat, unmistakable
    // colors so which cube face a reflection lands on can be read straight off
    // the screen. Faces are in bk/lf/ft/rt/up/dn order: red, green, blue,
    // yellow, pink, black. The same override feeds the drawn sky and the
    // reflection cube, so the two can be compared against each other.
    static bool forcedSkyboxEnabled()
    {
        const char* value = std::getenv("HAMMER_FORCE_SKYBOX");
        return value && *value && std::string_view(value) != "0";
    }

    // Loaded from raw VTFs: these are textures, not materials, so they have no
    // VMT for MaterialSystem::material() to parse.
    const hammer::assets::Material* forcedSkyFace(
        std::size_t index, hammer::assets::MaterialSystem& materials)
    {
        static constexpr std::array<const char*, 6> textures{{
            "cable/red", "cable/green", "cable/blue",
            "passtime/neons/neon_yellow_solid", "xenon/pink_2", "tools/toolsblack"}};
        if (index >= textures.size()) return nullptr;
        if (forcedSkyFaces_[index]) return forcedSkyFaces_[index].get();

        const auto fileSystem = materials.fileSystem();
        if (!fileSystem) return nullptr;
        const auto bytes = fileSystem->readFile(
            std::string("materials/") + textures[index] + ".vtf");
        if (!bytes) return nullptr;
        auto decoded = hammer::assets::MaterialSystem::decodeVtf(*bytes);
        if (!decoded || !decoded->valid()) return nullptr;

        auto face = std::make_shared<hammer::assets::Material>();
        face->name = std::string("__forcedsky/") + textures[index];
        face->shader = "UnlitGeneric";
        face->image = std::move(*decoded);
        forcedSkyFaces_[index] = std::move(face);
        return forcedSkyFaces_[index].get();
    }

    bool ensureEnvironmentCubeMap(const std::string& skyName,
                                  hammer::assets::MaterialSystem& materials)
    {
        const bool forced = forcedSkyboxEnabled();
        const std::string cacheKey = forced ? "__forcedsky" : skyName;
        if (cacheKey == environmentSkyName_) return environmentCubeMap_ != 0;
        if (environmentCubeMap_) glDeleteTextures(1, &environmentCubeMap_);
        environmentCubeMap_ = 0;
        environmentSkyName_ = cacheKey;
        if (skyName.empty() && !forced) return false;

        constexpr std::array<const char*, 6> suffixes{{"bk", "lf", "ft", "rt", "up", "dn"}};
        std::array<std::shared_ptr<const hammer::assets::Material>, 6> resolved;
        std::array<const hammer::assets::Image*, 6> faces{};
        for (std::size_t index = 0; index < suffixes.size(); ++index) {
            if (forced) {
                const hammer::assets::Material* face = forcedSkyFace(index, materials);
                if (!face) return false;
                faces[index] = &face->image;
                continue;
            }
            resolved[index] = materials.material("skybox/" + skyName + suffixes[index]);
            if (!resolved[index] || resolved[index]->missing || !resolved[index]->image.valid())
                return false;
            faces[index] = &resolved[index]->image;
        }
        environmentCubeMap_ = uploadEnvironmentCubeMap(faces, SkyCubeKind::SourceSky);
        return environmentCubeMap_ != 0;
    }

    GLuint ensureMaterialEnvironmentCubeMap(
        const std::shared_ptr<const hammer::assets::Material>& material)
    {
        if (!material || material->envMap.empty() || material->envMapUsesMapCubemap ||
            !material->hasEnvMapCube || !material->envMapCube.valid()) {
            return 0;
        }
        if (const auto found = materialEnvironmentCubeMaps_.find(material->envMap);
            found != materialEnvironmentCubeMaps_.end()) {
            return found->second;
        }

        // MaterialSystem stores VTF faces as right, left, back, front, up,
        // down. Reorder them into the Hammer sky sampler's back, left, front,
        // right, up, down convention before converting to the GL cubemap.
        const auto& source = material->envMapCube.faces;
        const std::array<const hammer::assets::Image*, 6> faces{{
            &source[2], &source[1], &source[3], &source[0], &source[4], &source[5]}};
        const GLuint cubeMap = uploadEnvironmentCubeMap(faces, SkyCubeKind::AuthoredCubemap);
        if (cubeMap) materialEnvironmentCubeMaps_.emplace(material->envMap, cubeMap);
        return cubeMap;
    }

    void configureMaterialEffects(
        const std::shared_ptr<const hammer::assets::Material>& material,
        const TextureRecord& texture, int bakedCubemapIndex = -1)
    {
        program_->setUniformValue("uMaterialAlpha", material ? material->alpha : 1.0f);
        program_->setUniformValue("uAlphaTest", material && material->alphaTest ? 1 : 0);
        program_->setUniformValue("uAlphaTestReference",
                                  material ? material->alphaTestReference : 0.5f);
        const bool usable = shadedTexturing_ && material && !material->water;
        const bool hasBump = usable && material->editorBumpMapSupported &&
                             !material->ssBump && material->bumpMapped &&
                             texture.hasBumpMap && texture.bumpId;
        const bool useBump = hasBump && bumpMapsEnabled_;
        const bool usePhong = usable && phongEnabled_ &&
                              material->editorPhongSupported && material->phong;

        // $envmap "env_cubemap" explicitly requests the current map cubemap.
        // Any other $envmap names an authored VTF cubemap. Use that texture
        // first and fall back to the map skybox only when it could not be found
        // or decoded. Water always uses the map skybox approximation.
        //
        // A ray-traced env_cubemap bake supersedes the sky for exactly the
        // surfaces that asked for the map cubemap, because it is the real
        // surroundings rather than an approximation of them.
        GLuint selectedEnvironment = environmentCubeMap_;
        if (usable && material->specular && !material->envMapUsesMapCubemap) {
            const GLuint authoredEnvironment = ensureMaterialEnvironmentCubeMap(material);
            if (authoredEnvironment) selectedEnvironment = authoredEnvironment;
        } else if (usable && material->specular && material->envMapUsesMapCubemap) {
            const GLuint bakedEnvironment = bakedCubemapTexture(bakedCubemapIndex);
            if (bakedEnvironment) selectedEnvironment = bakedEnvironment;
        } else if (material && material->water) {
            // Water is deliberately outside `usable`, but it is Source's
            // canonical env_cubemap consumer: its reflection is the whole point
            // of baking one. Prefer the bake over the sky approximation.
            const GLuint bakedEnvironment = bakedCubemapTexture(bakedCubemapIndex);
            if (bakedEnvironment) selectedEnvironment = bakedEnvironment;
        }
        const bool hasSelectedEnvironment = selectedEnvironment != 0;
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, selectedEnvironment);
        glActiveTexture(GL_TEXTURE0);
        program_->setUniformValue("uHasEnvironmentMap", hasSelectedEnvironment ? 1 : 0);

        const bool useSpecular = usable && specularEnabled_ && hasSelectedEnvironment &&
                                 material->editorSpecularSupported && material->specular;

        // HAMMER_DEBUG_ENVMAP=1 reports, once per material, every input that can
        // silently switch the cubemap reflection off or scale it away. Printing
        // from the real viewport is the only way to tell an app-side
        // misconfiguration apart from a shading bug.
        if (material && std::getenv("HAMMER_DEBUG_ENVMAP")) {
            static std::unordered_set<std::string> reported;
            if (reported.insert(material->name).second) {
                std::fprintf(stderr,
                    "[envmap] %s shader=%s shadedTexturing=%d specularEnabled=%d "
                    "supported=%d specular=%d envMap='%s' usesMapCubemap=%d "
                    "skyCube=%u selected=%u useSpecular=%d strength=%.3f "
                    "intensity=%.3f maskMode=%d contrast=%.2f saturation=%.2f "
                    "tint=(%.2f %.2f %.2f) bumpMapped=%d hasBump=%d useBump=%d "
                    "bumpId=%u bumpIntensity=%.2f ssbump=%d viewport=%dx%d "
                    "gles=%d modern=%d vao=%d GL='%s' renderer='%s'\n",
                    material->name.c_str(), material->shader.c_str(),
                    shadedTexturing_ ? 1 : 0, specularEnabled_ ? 1 : 0,
                    material->editorSpecularSupported ? 1 : 0,
                    material->specular ? 1 : 0, material->envMap.c_str(),
                    material->envMapUsesMapCubemap ? 1 : 0,
                    environmentCubeMap_, selectedEnvironment, useSpecular ? 1 : 0,
                    material->specularStrength, specularIntensity_,
                    material->envMapMaskFromNormalAlpha ? 2
                        : (material->envMapMaskFromBaseAlpha ? 1 : 0),
                    material->envMapContrast, material->envMapSaturation,
                    material->envMapTint[0], material->envMapTint[1],
                    material->envMapTint[2], material->bumpMapped ? 1 : 0,
                    hasBump ? 1 : 0, useBump ? 1 : 0, texture.bumpId,
                    bumpMapIntensity_, material->ssBump ? 1 : 0,
                    renderWidth_, renderHeight_, isOpenGles_ ? 1 : 0,
                    modernContext_ ? 1 : 0, useVertexArray_ ? 1 : 0,
                    reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                    reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
            }
        }
        const bool useSelfIllum = usable && selfIllumEnabled_ &&
                                  material->editorSelfIllumSupported && material->selfIllum;
        const bool useSelfIllumFresnel = useSelfIllum && material->selfIllumFresnel;
        // Lightwarp is an independent material-preview effect. Apply it to
        // every supported material that declares $lightwarptexture, while the
        // View > Material Effects toggle controls whether the LUT is sampled.
        // Runtime proxy fallback keeps unresolved player tint variables neutral,
        // so ordinary character materials no longer need an invuln-only gate.
        const bool useLightWarp = usable && lightWarpEnabled_ &&
                                  material->hasLightWarpTexture &&
                                  texture.hasLightWarp && texture.lightWarpId;
        // Stock VertexLitGeneric only enables the rim-light shader combo when
        // Phong is active. Keep that dependency even though the editor exposes
        // a separate visibility toggle for quickly comparing the contribution.
        const bool useRimLight = usable && rimLightEnabled_ && usePhong &&
                                 material->editorRimLightSupported && material->rimLight;
        const bool useExponentMap = usePhong && material->hasPhongExponentTexture &&
                                    texture.hasPhongExponentMap && texture.phongExponentId;
        const bool useSelfIllumMask = useSelfIllum && material->hasSelfIllumMask &&
                                      texture.hasSelfIllumMask && texture.selfIllumMaskId;
        const bool zeroPhongTint = material && material->phongTintDefined &&
            std::abs(material->phongTint[0]) <= 1e-6f &&
            std::abs(material->phongTint[1]) <= 1e-6f &&
            std::abs(material->phongTint[2]) <= 1e-6f;
        const bool useAlbedoTintMap = useExponentMap && material->phongAlbedoTint &&
                                      zeroPhongTint;
        program_->setUniformValue("uUseBumpMap", useBump ? 1 : 0);
        program_->setUniformValue("uHasBumpMap", hasBump ? 1 : 0);
        program_->setUniformValue("uUsePhong", usePhong ? 1 : 0);
        program_->setUniformValue("uUseSpecular", useSpecular ? 1 : 0);
        program_->setUniformValue("uUseSelfIllum", useSelfIllum ? 1 : 0);
        program_->setUniformValue("uUseSelfIllumFresnel", useSelfIllumFresnel ? 1 : 0);
        program_->setUniformValue("uUseLightWarp", useLightWarp ? 1 : 0);
        program_->setUniformValue("uHalfLambert", material && material->halfLambert ? 1 : 0);
        program_->setUniformValue("uUseRimLight", useRimLight ? 1 : 0);
        program_->setUniformValue("uHasSelfIllumMask", useSelfIllumMask ? 1 : 0);
        program_->setUniformValue("uRimMaskFromExponentAlpha",
                                  useRimLight && material->rimMaskFromExponentAlpha &&
                                  useExponentMap ? 1 : 0);
        program_->setUniformValue("uPhongMaskFromBaseAlpha",
                                  usePhong && material->phongMaskFromBaseAlpha ? 1 : 0);
        // skin_ps20b selects base alpha for cubemap masking by default, then
        // switches to normal-map alpha for $normalmapalphaenvmapmask.
        // Non-skin materials remain unmasked unless they explicitly request an
        // alpha-mask source.
        int specularMaskMode = 0; // 0 = full, 1 = base alpha, 2 = normal alpha
        if (useSpecular) {
            if (material->envMapMaskFromNormalAlpha && hasBump) {
                specularMaskMode = 2;
            } else if (material->envMapMaskFromBaseAlpha ||
                       (usePhong && QString::fromStdString(material->shader).compare(
                           QStringLiteral("VertexLitGeneric"), Qt::CaseInsensitive) == 0)) {
                specularMaskMode = 1;
            }
        }
        program_->setUniformValue("uSpecularMaskMode", specularMaskMode);
        program_->setUniformValue("uInvertSpecularMask",
                                  useSpecular && specularMaskMode != 0 && material->invertPhongMask ? 1 : 0);
        program_->setUniformValue("uHasPhongExponentMap", useExponentMap ? 1 : 0);
        program_->setUniformValue("uPhongExponentOverride",
                                  usePhong && material->phongExponentOverride ? 1 : 0);
        program_->setUniformValue("uPhongAlbedoTint", useAlbedoTintMap ? 1 : 0);
        program_->setUniformValue("uPhongFresnelRanges", QVector3D(
            material ? material->phongFresnelRanges[0] : 0.0f,
            material ? material->phongFresnelRanges[1] : 0.5f,
            material ? material->phongFresnelRanges[2] : 1.0f));
        // Stock Source treats an all-zero constant Phong tint as a sentinel:
        // use exponent-map green for albedo tinting when available, otherwise
        // fall back to white rather than producing a black highlight.
        program_->setUniformValue("uPhongTint", zeroPhongTint
            ? QVector3D(1.0f, 1.0f, 1.0f)
            : QVector3D(material ? material->phongTint[0] : 1.0f,
                        material ? material->phongTint[1] : 1.0f,
                        material ? material->phongTint[2] : 1.0f));
        program_->setUniformValue("uEnvMapTint", QVector3D(
            material ? material->envMapTint[0] : 1.0f,
            material ? material->envMapTint[1] : 1.0f,
            material ? material->envMapTint[2] : 1.0f));
        program_->setUniformValue("uEnvMapContrast",
                                  material ? material->envMapContrast : 0.0f);
        program_->setUniformValue("uEnvMapSaturation",
                                  material ? material->envMapSaturation : 1.0f);
        program_->setUniformValue("uPhongExponent", material ? material->phongExponent : 150.0f);
        program_->setUniformValue("uPhongBoost", material ? material->phongBoost : 1.0f);
        program_->setUniformValue("uSpecularStrength",
                                  material ? material->specularStrength : 0.18f);
        program_->setUniformValue("uSelfIllumTint", QVector3D(
            material ? material->selfIllumTint[0] : 1.0f,
            material ? material->selfIllumTint[1] : 1.0f,
            material ? material->selfIllumTint[2] : 1.0f));
        program_->setUniformValue("uSelfIllumFresnelMinMaxExp", QVector3D(
            material ? material->selfIllumFresnelMinMaxExp[0] : 0.0f,
            material ? material->selfIllumFresnelMinMaxExp[1] : 1.0f,
            material ? material->selfIllumFresnelMinMaxExp[2] : 1.0f));
        const bool useColor2 = usable && material && material->color2Active;
        program_->setUniformValue("uColor2", useColor2
            ? QVector3D(material->color2[0], material->color2[1], material->color2[2])
            : QVector3D(1.0f, 1.0f, 1.0f));
        program_->setUniformValue("uBlendTintByBaseAlpha",
                                  useColor2 && material->blendTintByBaseAlpha ? 1 : 0);
        program_->setUniformValue("uBlendTintColorOverBase",
                                  material ? material->blendTintColorOverBase : 0.0f);
        program_->setUniformValue("uHighEnergyEffect",
                                  usable && material->highEnergyEffect ? 1 : 0);
        program_->setUniformValue("uRimLightExponent",
                                  material ? material->rimLightExponent : 4.0f);
        program_->setUniformValue("uRimLightBoost",
                                  material ? material->rimLightBoost : 1.0f);
        GLuint selectedBumpTexture = texture.bumpId;
        if (hasBump && material && texture.bumpFrameIds.size() > 1 &&
            material->bumpAnimationFrameRate > 0.0f) {
            const std::size_t frame = static_cast<std::size_t>(std::floor(
                animationSeconds_ * material->bumpAnimationFrameRate)) %
                texture.bumpFrameIds.size();
            selectedBumpTexture = texture.bumpFrameIds[frame];
            hasAnimatedMaterials_ = true;
        }
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, hasBump && (useBump || usePhong)
            ? selectedBumpTexture : 0);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, useExponentMap ? texture.phongExponentId : 0);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, useSelfIllumMask ? texture.selfIllumMaskId : 0);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, useLightWarp ? texture.lightWarpId : 0);
        glActiveTexture(GL_TEXTURE0);
    }

    void clearMaterialEffects()
    {
        program_->setUniformValue("uMaterialAlpha", 1.0f);
        program_->setUniformValue("uVertexAlpha", 0);
        program_->setUniformValue("uAlphaTest", 0);
        program_->setUniformValue("uAlphaTestReference", 0.5f);
        program_->setUniformValue("uUseBumpMap", 0);
        program_->setUniformValue("uHasBumpMap", 0);
        program_->setUniformValue("uUsePhong", 0);
        program_->setUniformValue("uUseSpecular", 0);
        program_->setUniformValue("uUseSelfIllum", 0);
        program_->setUniformValue("uUseSelfIllumFresnel", 0);
        program_->setUniformValue("uUseLightWarp", 0);
        program_->setUniformValue("uHalfLambert", 0);
        program_->setUniformValue("uUseRimLight", 0);
        program_->setUniformValue("uHasSelfIllumMask", 0);
        program_->setUniformValue("uRimMaskFromExponentAlpha", 0);
        program_->setUniformValue("uPhongMaskFromBaseAlpha", 0);
        program_->setUniformValue("uSpecularMaskMode", 0);
        program_->setUniformValue("uInvertSpecularMask", 0);
        program_->setUniformValue("uHasPhongExponentMap", 0);
        program_->setUniformValue("uPhongExponentOverride", 0);
        program_->setUniformValue("uPhongAlbedoTint", 0);
        program_->setUniformValue("uColor2", QVector3D(1.0f, 1.0f, 1.0f));
        program_->setUniformValue("uBlendTintByBaseAlpha", 0);
        program_->setUniformValue("uBlendTintColorOverBase", 0.0f);
        program_->setUniformValue("uHighEnergyEffect", 0);
        program_->setUniformValue("uSelfIllumFresnelMinMaxExp", QVector3D(0.0f, 1.0f, 1.0f));
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentCubeMap_);
        program_->setUniformValue("uHasEnvironmentMap", environmentCubeMap_ ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
    }

    void configureVertexAttributes()
    {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, nx)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, u2)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, blendAlpha)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, tx)));
        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                              reinterpret_cast<const void*>(offsetof(GpuVertex, lu)));
    }

    void configureStudioVertexAttributes()
    {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, nx)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, u2)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, blendAlpha)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, tx)));
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, boneWeight0)));
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(StudioGpuVertex),
                              reinterpret_cast<const void*>(offsetof(StudioGpuVertex, boneIndex0)));
    }

    void uploadAndDraw(const std::vector<GpuVertex>& vertices, GLenum primitive)
    {
        if (vertices.empty()) return;
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)),
                     vertices.data(), GL_STREAM_DRAW);
        if (!useVertexArray_) configureVertexAttributes();
        glDrawArrays(primitive, 0, static_cast<GLsizei>(vertices.size()));
        if (!useVertexArray_) {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(3);
            glDisableVertexAttribArray(4);
            glDisableVertexAttribArray(5);
            glDisableVertexAttribArray(6);
            glDisableVertexAttribArray(7);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // The 3D mover gizmo: three axis arrows drawn on top of the scene. The
    // arrowhead is a short fan of lines angled back from the tip, so the whole
    // gizmo stays in the uColor line pipeline.
    void drawMoverGizmo(const hammer::vmf::Vec3& origin, double armLength, int activeAxis)
    {
        static const hammer::vmf::Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        static const QVector4D axisColors[3] = {{0.94f, 0.25f, 0.25f, 1.0f},
                                                {0.25f, 0.82f, 0.25f, 1.0f},
                                                {0.38f, 0.5f, 1.0f, 1.0f}};
        program_->setUniformValue("uUseTexture", 0);
        glDisable(GL_DEPTH_TEST);
        for (int axis = 0; axis < 3; ++axis) {
            const hammer::vmf::Vec3& direction = axes[axis];
            const hammer::vmf::Vec3 tip{origin.x + direction.x * armLength,
                                        origin.y + direction.y * armLength,
                                        origin.z + direction.z * armLength};
            std::vector<GpuVertex> lines;
            appendWorldLine(lines, origin, tip);
            // Arrowhead: four barbs angled back along the other two axes.
            const double barb = armLength * 0.18;
            const hammer::vmf::Vec3 side1 = axes[(axis + 1) % 3];
            const hammer::vmf::Vec3 side2 = axes[(axis + 2) % 3];
            for (const double sign : {1.0, -1.0}) {
                appendWorldLine(lines, tip,
                                {tip.x - direction.x * barb + side1.x * barb * 0.5 * sign,
                                 tip.y - direction.y * barb + side1.y * barb * 0.5 * sign,
                                 tip.z - direction.z * barb + side1.z * barb * 0.5 * sign});
                appendWorldLine(lines, tip,
                                {tip.x - direction.x * barb + side2.x * barb * 0.5 * sign,
                                 tip.y - direction.y * barb + side2.y * barb * 0.5 * sign,
                                 tip.z - direction.z * barb + side2.z * barb * 0.5 * sign});
            }
            program_->setUniformValue("uColor", activeAxis == axis
                                                    ? QVector4D(1.0f, 1.0f, 0.0f, 1.0f)
                                                    : axisColors[axis]);
            glLineWidth(activeAxis == axis ? 4.0f : 3.0f);
            uploadAndDraw(lines, GL_LINES);
        }
        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
    }

    // CToolDisplace's vertex display: when a solid with displacement faces is
    // selected (or its faces are in the face selection), mark every
    // displacement vertex with a small world-space cross so the surface's
    // control points are visible while editing. The cross list is CPU-cached
    // keyed on (selection, scene revision) and streamed through the same
    // uColor line path the grid uses.
    void drawDisplacementVertices(const hammer::vmf::Scene& scene,
                                  const std::vector<hammer::vmf::ObjectRef>& selection)
    {
        if (displacementVertexScene_ != &scene ||
            displacementVertexRevision_ != scene.revision ||
            displacementVertexSelection_ != selection) {
            displacementVertexScene_ = &scene;
            displacementVertexRevision_ = scene.revision;
            displacementVertexSelection_ = selection;
            displacementVertexLines_.clear();
            const auto selected = [&selection](hammer::vmf::ObjectType type, int id) {
                return id >= 0 && std::find(selection.begin(), selection.end(),
                                            hammer::vmf::ObjectRef{type, id}) != selection.end();
            };
            constexpr double Half = 2.5;
            for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
                if (!selected(hammer::vmf::ObjectType::Solid, brush.id) &&
                    !selected(hammer::vmf::ObjectType::Entity, brush.ownerEntityId)) {
                    continue;
                }
                for (const hammer::vmf::FaceGeometry& face : brush.faces) {
                    if (!face.displacement) continue;
                    for (const auto& vertex : face.displacementVertices) {
                        const hammer::vmf::Vec3& p = vertex.position;
                        appendWorldLine(displacementVertexLines_,
                                        {p.x - Half, p.y, p.z}, {p.x + Half, p.y, p.z});
                        appendWorldLine(displacementVertexLines_,
                                        {p.x, p.y - Half, p.z}, {p.x, p.y + Half, p.z});
                        appendWorldLine(displacementVertexLines_,
                                        {p.x, p.y, p.z - Half}, {p.x, p.y, p.z + Half});
                    }
                }
            }
        }
        if (displacementVertexLines_.empty()) return;
        program_->setUniformValue("uUseTexture", 0);
        program_->setUniformValue("uColor", QVector4D(1.0f, 0.78f, 0.0f, 1.0f));
        glLineWidth(2.0f);
        uploadAndDraw(displacementVertexLines_, GL_LINES);
        glLineWidth(1.0f);
    }

    // Selected point entities with generated selection corners (props, sprite
    // helpers) get their bounds box drawn in the 3D view, always on top so the
    // selection is visible even when the model is partly occluded. Cached the
    // same way as the displacement vertex crosses.
    void drawSelectedEntityBounds(const hammer::vmf::Scene& scene,
                                  const std::vector<hammer::vmf::ObjectRef>& selection)
    {
        if (entityBoundsScene_ != &scene ||
            entityBoundsRevision_ != scene.revision ||
            entityBoundsSelection_ != selection) {
            entityBoundsScene_ = &scene;
            entityBoundsRevision_ = scene.revision;
            entityBoundsSelection_ = selection;
            entityBoundsLines_.clear();
            for (const hammer::vmf::EntityMarker& entity : scene.entities) {
                if (!entity.hasSelectionCorners) continue;
                if (std::find(selection.begin(), selection.end(), entity.object) ==
                    selection.end()) {
                    continue;
                }
                appendCornerBoxLines(entityBoundsLines_, entity.selectionCorners);
            }
        }
        if (entityBoundsLines_.empty()) return;
        program_->setUniformValue("uUseTexture", 0);
        program_->setUniformValue("uColor", QVector4D(1.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f));
        glDisable(GL_DEPTH_TEST);
        glLineWidth(2.0f);
        uploadAndDraw(entityBoundsLines_, GL_LINES);
        glLineWidth(1.0f);
        glEnable(GL_DEPTH_TEST);
    }

    void drawSkybox(const std::string& skyName,
                    hammer::assets::MaterialSystem& materials,
                    const hammer::camera::State& camera)
    {
        if (skyName.empty()) return;

        constexpr float extent = 64.0f;
        const float cx = static_cast<float>(camera.position.x);
        const float cy = static_cast<float>(camera.position.y);
        const float cz = static_cast<float>(camera.position.z);
        const auto point = [=](float x, float y, float z) {
            return QVector3D(cx + x * extent, cy + y * extent, cz + z * extent);
        };
        const auto vertex = [](const QVector3D& position, float u, float v) {
            return GpuVertex{position.x(), position.y(), position.z(), u, v,
                             0.0f, 0.0f, 1.0f, u, v, 0.0f};
        };
        struct SkyFace
        {
            const char* suffix;
            QVector3D topLeft;
            QVector3D bottomLeft;
            QVector3D bottomRight;
            QVector3D topRight;
        };

        // Correct Hammer skybox placement around the camera is BK -> LF -> FT -> RT.
        // In Source world coordinates this maps +Y, -X, -Y, +X respectively.
        // LF/BK and FT/RT are assigned by texture name rather than by the
        // conventional mathematical face labels.
        const std::array<SkyFace, 6> faces{{
            {"bk", point(-1,  1,  1), point(-1,  1, -1), point( 1,  1, -1), point( 1,  1,  1)},
            {"lf", point(-1, -1,  1), point(-1, -1, -1), point(-1,  1, -1), point(-1,  1,  1)},
            {"ft", point( 1, -1,  1), point( 1, -1, -1), point(-1, -1, -1), point(-1, -1,  1)},
            {"rt", point( 1,  1,  1), point( 1,  1, -1), point( 1, -1, -1), point( 1, -1,  1)},
            {"up", point(-1,  1,  1), point( 1,  1,  1), point( 1, -1,  1), point(-1, -1,  1)},
            {"dn", point( 1,  1, -1), point(-1,  1, -1), point(-1, -1, -1), point( 1, -1, -1)},
        }};

        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        program_->setUniformValue("uUseTexture", 1);
        program_->setUniformValue("uShaded", 0);
        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uHasTexture2", 0);
        program_->setUniformValue("uForceOpaque", 1);
        program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glActiveTexture(GL_TEXTURE0);

        const bool forced = forcedSkyboxEnabled();
        for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const SkyFace& face = faces[faceIndex];
            std::string materialName = "skybox/" + skyName + face.suffix;
            std::shared_ptr<const hammer::assets::Material> material;
            if (forced) {
                if (!forcedSkyFace(faceIndex, materials)) continue;
                material = forcedSkyFaces_[faceIndex];
                materialName = material->name;
            } else {
                material = materials.material(materialName);
            }
            if (!material || material->missing || !material->image.valid()) continue;
            const TextureRecord& texture = textureFor(materialName, material);
            if (!texture.id) continue;
            glBindTexture(GL_TEXTURE_2D, texture.id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            const std::vector<GpuVertex> vertices{
                vertex(face.topLeft, 0.0f, 0.0f),
                vertex(face.bottomLeft, 0.0f, 1.0f),
                vertex(face.bottomRight, 1.0f, 1.0f),
                vertex(face.topLeft, 0.0f, 0.0f),
                vertex(face.bottomRight, 1.0f, 1.0f),
                vertex(face.topRight, 1.0f, 0.0f),
            };
            uploadAndDraw(vertices, GL_TRIANGLES);
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        program_->setUniformValue("uForceOpaque", 0);
    }

    void drawGrid(const hammer::camera::State& camera)
    {
        constexpr double Spacing = 64.0;
        constexpr int HalfLineCount = 80;
        const double centerX = std::floor(camera.position.x / Spacing) * Spacing;
        const double centerY = std::floor(camera.position.y / Spacing) * Spacing;
        const double extent = Spacing * HalfLineCount;

        std::vector<GpuVertex> minor;
        std::vector<GpuVertex> major;
        auto appendLine = [](std::vector<GpuVertex>& target,
                             double ax, double ay, double az,
                             double bx, double by, double bz) {
            target.push_back({static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(az), 0.0f, 0.0f});
            target.push_back({static_cast<float>(bx), static_cast<float>(by), static_cast<float>(bz), 0.0f, 0.0f});
        };

        for (int index = -HalfLineCount; index <= HalfLineCount; ++index) {
            const double x = centerX + index * Spacing;
            const int absoluteIndex = static_cast<int>(std::llround(x / Spacing));
            appendLine((absoluteIndex % 8) == 0 ? major : minor,
                       x, centerY - extent, 0.0, x, centerY + extent, 0.0);
        }
        for (int index = -HalfLineCount; index <= HalfLineCount; ++index) {
            const double y = centerY + index * Spacing;
            const int absoluteIndex = static_cast<int>(std::llround(y / Spacing));
            appendLine((absoluteIndex % 8) == 0 ? major : minor,
                       centerX - extent, y, 0.0, centerX + extent, y, 0.0);
        }

        program_->setUniformValue("uUseTexture", 0);
        glDepthMask(GL_FALSE);
        program_->setUniformValue("uColor", QVector4D(0.086f, 0.125f, 0.243f, 1.0f));
        glLineWidth(1.0f);
        uploadAndDraw(minor, GL_LINES);
        program_->setUniformValue("uColor", QVector4D(0.176f, 0.227f, 0.361f, 1.0f));
        uploadAndDraw(major, GL_LINES);

        std::vector<GpuVertex> axis;
        appendLine(axis, centerX - extent, 0.0, 0.0, centerX + extent, 0.0, 0.0);
        program_->setUniformValue("uColor", QVector4D(0.588f, 0.173f, 0.173f, 1.0f));
        glLineWidth(2.0f);
        uploadAndDraw(axis, GL_LINES);
        axis.clear();
        appendLine(axis, 0.0, centerY - extent, 0.0, 0.0, centerY + extent, 0.0);
        program_->setUniformValue("uColor", QVector4D(0.173f, 0.588f, 0.267f, 1.0f));
        uploadAndDraw(axis, GL_LINES);
        glLineWidth(1.0f);
        glDepthMask(GL_TRUE);
    }

    struct OrthographicModelLines
    {
        GLuint vbo{0};
        GLuint vao{0};
        GLsizei count{0};
    };

    struct OrthographicModelDraw
    {
        std::string model;
        QMatrix4x4 transform;
        QVector4D color;
    };

    static void appendWorldLine(std::vector<GpuVertex>& target,
                                const hammer::vmf::Vec3& a,
                                const hammer::vmf::Vec3& b)
    {
        target.push_back({static_cast<float>(a.x), static_cast<float>(a.y),
                          static_cast<float>(a.z), 0.0f, 0.0f});
        target.push_back({static_cast<float>(b.x), static_cast<float>(b.y),
                          static_cast<float>(b.z), 0.0f, 0.0f});
    }

    // Prop wireframes in the 2D views. Regenerating a model's triangle lines
    // on the CPU for every entity on every cache rebuild measured ~260 ms per
    // view on a 105-entity map; instead each model's lines are uploaded once
    // in local space and each entity is a uModel-transformed draw call.
    bool entityUsesModelLines(const hammer::vmf::EntityMarker& entity)
    {
        if (!entity.projectedSurfaces.empty() || entity.model.empty() || !studioModels_)
            return false;
        const auto model = studioModels_->model(entity.model);
        return model && model->valid;
    }

    static QMatrix4x4 entityLineTransform(const hammer::vmf::EntityMarker& entity)
    {
        const hammer::camera::SourceTransform transform =
            hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
        const auto& basis = transform.basis;
        return QMatrix4x4(
            static_cast<float>(basis.forward.x), static_cast<float>(basis.left.x),
            static_cast<float>(basis.up.x), static_cast<float>(entity.origin.x),
            static_cast<float>(basis.forward.y), static_cast<float>(basis.left.y),
            static_cast<float>(basis.up.y), static_cast<float>(entity.origin.y),
            static_cast<float>(basis.forward.z), static_cast<float>(basis.left.z),
            static_cast<float>(basis.up.z), static_cast<float>(entity.origin.z),
            0.0f, 0.0f, 0.0f, 1.0f);
    }

    static QVector4D entityLineColor(const hammer::vmf::EntityMarker& entity)
    {
        return QVector4D(std::clamp(entity.displayColor[0], 0, 255) / 255.0f,
                         std::clamp(entity.displayColor[1], 0, 255) / 255.0f,
                         std::clamp(entity.displayColor[2], 0, 255) / 255.0f, 1.0f);
    }

    const OrthographicModelLines* orthographicModelLines(const std::string& path)
    {
        const auto found = orthographicModelLineCache_.find(path);
        if (found != orthographicModelLineCache_.end())
            return found->second.count > 0 ? &found->second : nullptr;
        OrthographicModelLines entry;
        const auto model = studioModels_ ? studioModels_->model(path) : nullptr;
        if (model && model->valid) {
            std::vector<GpuVertex> lines;
            for (const hammer::assets::StudioMesh& mesh : model->meshes) {
                for (std::size_t index = 0; index + 2 < mesh.vertices.size(); index += 3) {
                    const auto point = [&](const hammer::assets::StudioVertex& vertex) {
                        return hammer::vmf::Vec3{vertex.x, vertex.y, vertex.z};
                    };
                    const auto a = point(mesh.vertices[index]);
                    const auto b = point(mesh.vertices[index + 1]);
                    const auto c = point(mesh.vertices[index + 2]);
                    appendWorldLine(lines, a, b);
                    appendWorldLine(lines, b, c);
                    appendWorldLine(lines, c, a);
                }
            }
            uploadOrthographicBuffer(entry.vbo, entry.vao, entry.count, lines);
        }
        const auto [it, inserted] = orthographicModelLineCache_.emplace(path, entry);
        return it->second.count > 0 ? &it->second : nullptr;
    }

    void drawOrthographicModelDraws(const std::vector<OrthographicModelDraw>& draws,
                                    const QVector4D* overrideColor = nullptr)
    {
        if (draws.empty()) return;
        for (const OrthographicModelDraw& draw : draws) {
            const OrthographicModelLines* lines = orthographicModelLines(draw.model);
            if (!lines) continue;
            program_->setUniformValue("uModel", draw.transform);
            program_->setUniformValue("uColor", overrideColor ? *overrideColor : draw.color);
            drawOrthographicBuffer(lines->vbo, lines->vao, lines->count);
        }
        QMatrix4x4 identity;
        program_->setUniformValue("uModel", identity);
    }

    static void appendBrushLines(std::vector<GpuVertex>& target,
                                 const hammer::vmf::BrushGeometry& brush)
    {
        for (const auto& edge : brush.edges) {
            if (edge.first >= brush.vertices.size() || edge.second >= brush.vertices.size()) continue;
            appendWorldLine(target, brush.vertices[edge.first], brush.vertices[edge.second]);
        }
        for (const hammer::vmf::FaceGeometry& face : brush.faces) {
            if (!face.displacement || face.displacementPower < 1) continue;
            const int size = (1 << face.displacementPower) + 1;
            if (face.displacementVertices.size() != static_cast<std::size_t>(size * size)) continue;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const std::size_t at = static_cast<std::size_t>(y * size + x);
                    if (x + 1 < size)
                        appendWorldLine(target, face.displacementVertices[at].position,
                                        face.displacementVertices[at + 1].position);
                    if (y + 1 < size)
                        appendWorldLine(target, face.displacementVertices[at].position,
                                        face.displacementVertices[at + size].position);
                }
            }
        }
    }

    void appendEntityLines(std::vector<GpuVertex>& target,
                           const hammer::vmf::EntityMarker& entity)
    {
        if (!entity.projectedSurfaces.empty()) {
            for (const auto& surface : entity.projectedSurfaces) {
                for (std::size_t index = 0; index + 2 < surface.triangles.size(); index += 3) {
                    const auto& a = surface.triangles[index].position;
                    const auto& b = surface.triangles[index + 1].position;
                    const auto& c = surface.triangles[index + 2].position;
                    appendWorldLine(target, a, b);
                    appendWorldLine(target, b, c);
                    appendWorldLine(target, c, a);
                }
            }
            return;
        }
        if (!entity.model.empty() && studioModels_) {
            const auto model = studioModels_->model(entity.model);
            if (model && model->valid) {
                const hammer::camera::SourceTransform transform =
                    hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
                for (const hammer::assets::StudioMesh& mesh : model->meshes) {
                    for (std::size_t index = 0; index + 2 < mesh.vertices.size(); index += 3) {
                        const auto point = [&](const hammer::assets::StudioVertex& vertex) {
                            return transform.transformPoint({vertex.x, vertex.y, vertex.z});
                        };
                        const auto a = point(mesh.vertices[index]);
                        const auto b = point(mesh.vertices[index + 1]);
                        const auto c = point(mesh.vertices[index + 2]);
                        appendWorldLine(target, a, b);
                        appendWorldLine(target, b, c);
                        appendWorldLine(target, c, a);
                    }
                }
                return;
            }
        }

        std::array<hammer::vmf::Vec3, 8> corners{};
        if (entity.hasSelectionCorners) {
            corners = entity.selectionCorners;
        } else {
            for (int index = 0; index < 8; ++index) {
                corners[static_cast<std::size_t>(index)] = {
                    entity.origin.x + ((index & 1) ? entity.sizeMaximum.x : entity.sizeMinimum.x),
                    entity.origin.y + ((index & 2) ? entity.sizeMaximum.y : entity.sizeMinimum.y),
                    entity.origin.z + ((index & 4) ? entity.sizeMaximum.z : entity.sizeMinimum.z)};
            }
        }
        appendCornerBoxLines(target, corners);
    }

    static void appendCornerBoxLines(std::vector<GpuVertex>& target,
                                     const std::array<hammer::vmf::Vec3, 8>& corners)
    {
        constexpr std::array<std::pair<int, int>, 12> edges{{
            {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4},
            {0,4},{1,5},{2,6},{3,7}}};
        for (const auto& [a, b] : edges)
            appendWorldLine(target, corners[static_cast<std::size_t>(a)],
                            corners[static_cast<std::size_t>(b)]);
    }

    void uploadOrthographicBuffer(GLuint& vbo, GLuint& vao, GLsizei& count,
                                  const std::vector<GpuVertex>& vertices)
    {
        count = static_cast<GLsizei>(vertices.size());
        if (vertices.empty()) return;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)),
                     vertices.data(), GL_STATIC_DRAW);
        if (useVertexArray_) {
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            configureVertexAttributes();
            glBindVertexArray(vao_);
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    void drawOrthographicBuffer(GLuint vbo, GLuint vao, GLsizei count)
    {
        if (!vbo || count <= 0) return;
        if (useVertexArray_) glBindVertexArray(vao);
        else {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            configureVertexAttributes();
        }
        glDrawArrays(GL_LINES, 0, count);
        if (useVertexArray_) glBindVertexArray(vao_);
        else {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(3);
            glDisableVertexAttribArray(4);
            glDisableVertexAttribArray(5);
            glDisableVertexAttribArray(6);
            glDisableVertexAttribArray(7);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    void clearOrthographicModelLineCache()
    {
        for (auto& [path, lines] : orthographicModelLineCache_) {
            if (lines.vbo) glDeleteBuffers(1, &lines.vbo);
            if (lines.vao) glDeleteVertexArrays(1, &lines.vao);
        }
        orthographicModelLineCache_.clear();
    }

    void clearOrthographicCache()
    {
        if (orthographicBrushVbo_) glDeleteBuffers(1, &orthographicBrushVbo_);
        if (orthographicBrushVao_) glDeleteVertexArrays(1, &orthographicBrushVao_);
        orthographicBrushVbo_ = 0;
        orthographicBrushVao_ = 0;
        orthographicBrushCount_ = 0;
        for (OrthographicGpuBatch& batch : orthographicEntityBatches_) {
            if (batch.vbo) glDeleteBuffers(1, &batch.vbo);
            if (batch.vao) glDeleteVertexArrays(1, &batch.vao);
        }
        orthographicEntityBatches_.clear();
        if (orthographicSelectionVbo_) glDeleteBuffers(1, &orthographicSelectionVbo_);
        if (orthographicSelectionVao_) glDeleteVertexArrays(1, &orthographicSelectionVao_);
        orthographicSelectionVbo_ = 0;
        orthographicSelectionVao_ = 0;
        orthographicSelectionCount_ = 0;
        if (orthographicSelectionBoundsVbo_) glDeleteBuffers(1, &orthographicSelectionBoundsVbo_);
        if (orthographicSelectionBoundsVao_) glDeleteVertexArrays(1, &orthographicSelectionBoundsVao_);
        orthographicSelectionBoundsVbo_ = 0;
        orthographicSelectionBoundsVao_ = 0;
        orthographicSelectionBoundsCount_ = 0;
        orthographicSelection_.clear();
        orthographicSelectionMode_ = MapViewWidget::SelectionMode::Groups;
        orthographicSelectionRevision_ = 0;
        if (orthographicEditingVbo_) glDeleteBuffers(1, &orthographicEditingVbo_);
        if (orthographicEditingVao_) glDeleteVertexArrays(1, &orthographicEditingVao_);
        orthographicEditingVbo_ = 0;
        orthographicEditingVao_ = 0;
        orthographicEditingCount_ = 0;
        for (OrthographicGpuBatch& batch : orthographicEditingEntityBatches_) {
            if (batch.vbo) glDeleteBuffers(1, &batch.vbo);
            if (batch.vao) glDeleteVertexArrays(1, &batch.vao);
        }
        orthographicEditingEntityBatches_.clear();
        orthographicEditingSolids_.clear();
        orthographicExcludedSolids_.clear();
        orthographicExcludedEntities_.clear();
        orthographicEditingRevision_ = 0;
        orthographicModelDraws_.clear();
        orthographicSelectionModelDraws_.clear();
        orthographicEditingModelDraws_.clear();
        orthographicLineage_.clear();
        orthographicMaterialOwner_ = nullptr;
    }

    // The object a 2D click selects for a brush: in Groups/Objects mode a solid
    // owned by a brush entity selects the entity, so dragging that entity moves
    // every one of its solids. The static/dynamic split must use exactly this
    // rule or brush-entity drags fall off the incremental path.
    static hammer::vmf::ObjectRef effectiveOrthographicRef(
        const hammer::vmf::BrushGeometry& brush, MapViewWidget::SelectionMode selectionMode)
    {
        return selectionMode != MapViewWidget::SelectionMode::Solids && brush.ownerEntityId >= 0
            ? hammer::vmf::ObjectRef{hammer::vmf::ObjectType::Entity, brush.ownerEntityId}
            : brush.object;
    }

    void rebuildOrthographicSelectionCache(
        const hammer::vmf::Scene* scene,
        const std::vector<hammer::vmf::ObjectRef>& selection,
        MapViewWidget::SelectionMode selectionMode)
    {
        if (orthographicSelectionVbo_) glDeleteBuffers(1, &orthographicSelectionVbo_);
        if (orthographicSelectionVao_) glDeleteVertexArrays(1, &orthographicSelectionVao_);
        orthographicSelectionVbo_ = 0;
        orthographicSelectionVao_ = 0;
        orthographicSelectionCount_ = 0;
        if (orthographicSelectionBoundsVbo_) glDeleteBuffers(1, &orthographicSelectionBoundsVbo_);
        if (orthographicSelectionBoundsVao_) glDeleteVertexArrays(1, &orthographicSelectionBoundsVao_);
        orthographicSelectionBoundsVbo_ = 0;
        orthographicSelectionBoundsVao_ = 0;
        orthographicSelectionBoundsCount_ = 0;
        orthographicSelectionModelDraws_.clear();
        orthographicSelection_ = selection;
        orthographicSelectionMode_ = selectionMode;
        orthographicSelectionRevision_ = scene ? scene->revision : 0;
        if (!scene || selection.empty()) return;

        const auto selected = [&selection](const hammer::vmf::ObjectRef& object) {
            return std::find(selection.begin(), selection.end(), object) != selection.end();
        };
        std::vector<GpuVertex> lines;
        std::vector<GpuVertex> boundsLines;
        for (const hammer::vmf::BrushGeometry& brush : scene->brushes) {
            if (selected(effectiveOrthographicRef(brush, selectionMode)))
                appendBrushLines(lines, brush);
        }
        for (const hammer::vmf::EntityMarker& entity : scene->entities) {
            if (!selected(entity.object)) continue;
            if (entityUsesModelLines(entity)) {
                orthographicSelectionModelDraws_.push_back(
                    {entity.model, entityLineTransform(entity), entityLineColor(entity)});
                // The generated selection bounds are the prop's outline: draw
                // the corner box over the model wireframe so what the user
                // grabs (entityScreenBounds) is what they see. It goes in its
                // own buffer because red-on-red vanishes against the model's
                // selection wireframe; this one is drawn in the handle yellow.
                if (entity.hasSelectionCorners)
                    appendCornerBoxLines(boundsLines, entity.selectionCorners);
                continue;
            }
            appendEntityLines(lines, entity);
        }
        uploadOrthographicBuffer(orthographicSelectionVbo_, orthographicSelectionVao_,
                                  orthographicSelectionCount_, lines);
        uploadOrthographicBuffer(orthographicSelectionBoundsVbo_, orthographicSelectionBoundsVao_,
                                  orthographicSelectionBoundsCount_, boundsLines);
    }

    // Solids the static buffer does not draw but that are not currently
    // object-selected: face-selected solids (displacement paint changes their
    // geometry every mouse move) and solids excluded for a selection that has
    // since changed. Drawn in the normal wireframe color from a small buffer
    // that is cheap to rebuild per revision or per click.
    void rebuildOrthographicEditingCache(const hammer::vmf::Scene* scene,
                                         const std::vector<hammer::vmf::ObjectRef>& selection,
                                         MapViewWidget::SelectionMode selectionMode,
                                         const std::vector<int>& editingSolidIds)
    {
        if (orthographicEditingVbo_) glDeleteBuffers(1, &orthographicEditingVbo_);
        if (orthographicEditingVao_) glDeleteVertexArrays(1, &orthographicEditingVao_);
        orthographicEditingVbo_ = 0;
        orthographicEditingVao_ = 0;
        orthographicEditingCount_ = 0;
        for (OrthographicGpuBatch& batch : orthographicEditingEntityBatches_) {
            if (batch.vbo) glDeleteBuffers(1, &batch.vbo);
            if (batch.vao) glDeleteVertexArrays(1, &batch.vao);
        }
        orthographicEditingEntityBatches_.clear();
        orthographicEditingModelDraws_.clear();
        orthographicEditingRevision_ = scene ? scene->revision : 0;
        orthographicEditingSolids_ = editingSolidIds;
        if (!scene) return;

        std::vector<int> wanted = orthographicExcludedSolids_;
        wanted.insert(wanted.end(), editingSolidIds.begin(), editingSolidIds.end());
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        if (wanted.empty() && orthographicExcludedEntities_.empty()) return;

        const auto selected = [&selection](const hammer::vmf::ObjectRef& object) {
            return std::find(selection.begin(), selection.end(), object) != selection.end();
        };
        if (!wanted.empty()) {
            std::vector<GpuVertex> lines;
            for (const hammer::vmf::BrushGeometry& brush : scene->brushes) {
                if (!std::binary_search(wanted.begin(), wanted.end(), brush.id)) continue;
                // Object-selected: the selection buffer already draws it.
                if (selected(effectiveOrthographicRef(brush, selectionMode))) continue;
                if (hammer::vmf::isBrushHiddenByToolTextures(brush,
                                                             orthographicHiddenToolTextures_)) {
                    continue;
                }
                appendBrushLines(lines, brush);
            }
            uploadOrthographicBuffer(orthographicEditingVbo_, orthographicEditingVao_,
                                      orthographicEditingCount_, lines);
        }

        // Entity markers the static batches excluded for a since-changed
        // selection, in their own display colors.
        std::map<std::array<int, 3>, std::vector<GpuVertex>> entityGroups;
        for (const hammer::vmf::EntityMarker& entity : scene->entities) {
            if (!std::binary_search(orthographicExcludedEntities_.begin(),
                                    orthographicExcludedEntities_.end(), entity.id)) {
                continue;
            }
            if (selected(entity.object)) continue;
            if (entityUsesModelLines(entity)) {
                orthographicEditingModelDraws_.push_back(
                    {entity.model, entityLineTransform(entity), entityLineColor(entity)});
                continue;
            }
            const std::array<int, 3> color{{
                std::clamp(entity.displayColor[0], 0, 255),
                std::clamp(entity.displayColor[1], 0, 255),
                std::clamp(entity.displayColor[2], 0, 255)}};
            appendEntityLines(entityGroups[color], entity);
        }
        for (auto& [color, vertices] : entityGroups) {
            OrthographicGpuBatch batch;
            batch.color = QVector4D(color[0] / 255.0f, color[1] / 255.0f,
                                    color[2] / 255.0f, 1.0f);
            uploadOrthographicBuffer(batch.vbo, batch.vao, batch.count, vertices);
            orthographicEditingEntityBatches_.push_back(std::move(batch));
        }
    }

    void rebuildOrthographicCache(const hammer::vmf::Scene* scene,
                                  hammer::assets::MaterialSystem* materials,
                                  const std::vector<hammer::vmf::ObjectRef>& selection,
                                  MapViewWidget::SelectionMode selectionMode,
                                  const std::vector<int>& editingSolidIds)
    {
        const std::unordered_set<std::string> hiddenToolTextures = orthographicHiddenToolTextures_;
        clearOrthographicCache();
        orthographicHiddenToolTextures_ = hiddenToolTextures;
        orthographicMaterialOwner_ = materials;
        orthographicEditingSolids_ = editingSolidIds;
        if (!scene) return;
        orthographicLineage_.revision = scene->revision;

        const auto selected = [&selection](const hammer::vmf::ObjectRef& object) {
            return std::find(selection.begin(), selection.end(), object) != selection.end();
        };
        const auto editing = [this](int id) {
            return std::binary_search(orthographicEditingSolids_.begin(),
                                      orthographicEditingSolids_.end(), id);
        };
        // Selected geometry is drawn from the separate selection buffer, and
        // face-selected geometry from the editing buffer, so both are left out
        // here entirely. Those buffers are small, which is what makes a drag
        // cost O(selection) instead of O(map).
        for (const hammer::vmf::ObjectRef& object : selection) {
            if (object.type == hammer::vmf::ObjectType::Solid)
                orthographicLineage_.dynamicSolidIds.insert(object.id);
            else
                orthographicLineage_.dynamicEntityIds.insert(object.id);
        }
        for (int id : orthographicEditingSolids_) orthographicLineage_.dynamicSolidIds.insert(id);
        for (const hammer::vmf::BrushGeometry& brush : scene->brushes) {
            if (selected(effectiveOrthographicRef(brush, selectionMode)))
                orthographicLineage_.dynamicSolidIds.insert(brush.id);
        }

        std::vector<GpuVertex> brushes;
        orthographicExcludedSolids_.clear();
        for (const hammer::vmf::BrushGeometry& brush : scene->brushes) {
            if (selected(effectiveOrthographicRef(brush, selectionMode)) || editing(brush.id)) {
                orthographicExcludedSolids_.push_back(brush.id);
                continue;
            }
            // Tool-texture visibility: a solid made entirely of hidden tool
            // materials is not drawn in the 2D views. MapViewWidget applies the
            // same predicate when picking, so it is also unselectable there.
            if (hammer::vmf::isBrushHiddenByToolTextures(brush, orthographicHiddenToolTextures_))
                continue;
            appendBrushLines(brushes, brush);
        }
        uploadOrthographicBuffer(orthographicBrushVbo_, orthographicBrushVao_,
                                  orthographicBrushCount_, brushes);

        std::map<std::array<int, 3>, std::vector<GpuVertex>> entityGroups;
        for (const hammer::vmf::EntityMarker& entity : scene->entities) {
            if (selected(entity.object)) {
                orthographicExcludedEntities_.push_back(entity.id);
                continue;
            }
            if (entityUsesModelLines(entity)) {
                orthographicModelDraws_.push_back(
                    {entity.model, entityLineTransform(entity), entityLineColor(entity)});
                continue;
            }
            const std::array<int, 3> color{{
                std::clamp(entity.displayColor[0], 0, 255),
                std::clamp(entity.displayColor[1], 0, 255),
                std::clamp(entity.displayColor[2], 0, 255)}};
            appendEntityLines(entityGroups[color], entity);
        }
        orthographicEntityBatches_.reserve(entityGroups.size());
        for (auto& [color, vertices] : entityGroups) {
            OrthographicGpuBatch batch;
            batch.color = QVector4D(color[0] / 255.0f, color[1] / 255.0f,
                                    color[2] / 255.0f, 1.0f);
            uploadOrthographicBuffer(batch.vbo, batch.vao, batch.count, vertices);
            orthographicEntityBatches_.push_back(std::move(batch));
        }
        std::sort(orthographicExcludedSolids_.begin(), orthographicExcludedSolids_.end());
        std::sort(orthographicExcludedEntities_.begin(), orthographicExcludedEntities_.end());
    }

    void clearWorldCache()
    {
        clearWorldDynamicCache();
        worldLineage_.clear();
        if (worldVbo_) glDeleteBuffers(1, &worldVbo_);
        if (worldVao_) glDeleteVertexArrays(1, &worldVao_);
        if (projectedVbo_) glDeleteBuffers(1, &projectedVbo_);
        if (projectedVao_) glDeleteVertexArrays(1, &projectedVao_);
        worldVbo_ = 0;
        worldVao_ = 0;
        projectedVbo_ = 0;
        projectedVao_ = 0;
        worldBatches_.clear();
        projectedBatches_.clear();
        worldMaterialOwner_ = nullptr;
        worldHiddenToolTextures_.clear();
        worldDisplacementSolidMask_ = true;
        worldStaticHasAnimatedWater_ = false;
        worldHasAnimatedWater_ = false;
    }

    void clearWorldDynamicCache()
    {
        if (worldDynamicVbo_) glDeleteBuffers(1, &worldDynamicVbo_);
        if (worldDynamicVao_) glDeleteVertexArrays(1, &worldDynamicVao_);
        worldDynamicVbo_ = 0;
        worldDynamicVao_ = 0;
        worldDynamicBatches_.clear();
    }

    // Builds one textured world buffer. "wantDynamic" selects which half of the
    // map goes in: the solids listed in the lineage's dynamic set (the ones an
    // interactive edit is moving) or everything else. Splitting the map this
    // way is what lets a drag re-upload only the selection.
    void buildWorldBrushBuffer(const hammer::vmf::Scene& scene,
                               hammer::assets::MaterialSystem& materials,
                               const std::unordered_set<std::string>& hiddenToolTextures,
                               bool displacementSolidMask,
                               bool wantDynamic,
                               bool buildProjectedSurfaces,
                               GLuint& targetVbo, GLuint& targetVao,
                               std::vector<MaterialBatch>& targetBatches,
                               bool& animatedWaterOut)
    {
        // The probe index joins the key so faces reflecting different env_cubemaps
        // never share a draw. Materials that do not use the map cubemap always
        // key on -1, leaving their batching exactly as it was.
        using WorldBatchKey = std::tuple<std::string, bool, int>;
        std::map<WorldBatchKey, MaterialBatch> batches;
        bool animatedWater = false;
        for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
            if (worldLineage_.dynamicSolidIds.contains(brush.id) != wantDynamic) continue;
            for (const hammer::vmf::FaceGeometry& face : brush.faces) {
                if (face.vertices.size() < 3 && !face.displacement) continue;
                // CMapSolid::Render3D: on a solid that has a displacement, the
                // sides that are not themselves displaced are not drawn.
                if (hammer::vmf::isFaceMaskedByDisplacementSolid(brush, face,
                                                                 displacementSolidMask)) {
                    continue;
                }
                const std::string normalizedFaceMaterial =
                    hammer::vmf::normalizeMaterialPath(face.material);
                if (normalizedFaceMaterial.rfind("tools/", 0) == 0 &&
                    hiddenToolTextures.find(normalizedFaceMaterial) != hiddenToolTextures.end()) {
                    continue;
                }
                auto material = materials.material(face.material);
                if (!material || !material->image.valid()) continue;
                const bool displacementSurface =
                    face.displacement && face.displacementPower >= 1 &&
                    !face.displacementVertices.empty();
                // Source assigns a cubemap per face by nearest probe. Use the
                // face centroid, which is the same point vbsp measures from.
                int cubemapIndex = -1;
                if ((material->envMapUsesMapCubemap || material->water) &&
                    !bakedCubemaps_.empty()) {
                    hammer::vmf::Vec3 centroid{};
                    int centroidCount = 0;
                    if (displacementSurface) {
                        for (const auto& vertex : face.displacementVertices) {
                            centroid.x += vertex.position.x;
                            centroid.y += vertex.position.y;
                            centroid.z += vertex.position.z;
                            ++centroidCount;
                        }
                    } else {
                        for (const std::size_t vertexIndex : face.vertices) {
                            if (vertexIndex >= brush.vertices.size()) continue;
                            centroid.x += brush.vertices[vertexIndex].x;
                            centroid.y += brush.vertices[vertexIndex].y;
                            centroid.z += brush.vertices[vertexIndex].z;
                            ++centroidCount;
                        }
                    }
                    if (centroidCount > 0) {
                        centroid.x /= centroidCount;
                        centroid.y /= centroidCount;
                        centroid.z /= centroidCount;
                        cubemapIndex = hammer::render::nearestCubemapIndex(
                            bakedCubemaps_, centroid);
                    }
                }

                MaterialBatch& batch =
                    batches[{material->name, displacementSurface, cubemapIndex}];
                batch.name = material->name;
                batch.material = material;
                batch.displacement = displacementSurface;
                batch.cubemapIndex = cubemapIndex;
                animatedWater = animatedWater || material->water;
                const float primaryWidth = static_cast<float>(std::max(1, material->image.width));
                const float primaryHeight = static_cast<float>(std::max(1, material->image.height));
                const float secondaryWidth = static_cast<float>(
                    material->image2.valid() ? material->image2.width : material->image.width);
                const float secondaryHeight = static_cast<float>(
                    material->image2.valid() ? material->image2.height : material->image.height);

                const float lmScale = face.lightmapScale > 0
                    ? static_cast<float>(face.lightmapScale) : 16.0f;
                const auto lmAxis = [](const hammer::vmf::Vec3& axis) {
                    const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y +
                                                    axis.z * axis.z);
                    return length > 1e-9
                        ? hammer::vmf::Vec3{axis.x / length, axis.y / length, axis.z / length}
                        : hammer::vmf::Vec3{};
                };
                const hammer::vmf::Vec3 lmU = lmAxis(face.uAxis.direction);
                const hammer::vmf::Vec3 lmV = lmAxis(face.vAxis.direction);
                auto makeVertex = [&](const hammer::vmf::Vec3& position,
                                      const hammer::vmf::Vec3& normal,
                                      double blendAlpha) -> GpuVertex {
                    const float uPixels = dot(position, face.uAxis.direction) /
                                              safeScale(face.uAxis.scale) +
                                          static_cast<float>(face.uAxis.shift);
                    const float vPixels = dot(position, face.vAxis.direction) /
                                              safeScale(face.vAxis.scale) +
                                          static_cast<float>(face.vAxis.shift);
                    GpuVertex vertex{static_cast<float>(position.x), static_cast<float>(position.y),
                                     static_cast<float>(position.z),
                                     uPixels / primaryWidth, vPixels / primaryHeight,
                                     static_cast<float>(normal.x),
                                     static_cast<float>(normal.y),
                                     static_cast<float>(normal.z),
                                     uPixels / std::max(1.0f, secondaryWidth),
                                     vPixels / std::max(1.0f, secondaryHeight),
                                     static_cast<float>(std::clamp(blendAlpha, 0.0, 1.0))};
                    assignBrushTextureTangent(vertex, face.uAxis, face.vAxis);
                    vertex.lu = dot(position, lmU) / lmScale;
                    vertex.lv = dot(position, lmV) / lmScale;
                    return vertex;
                };
                auto makeDisplacementVertex = [&](
                    const hammer::vmf::DisplacementVertex& source) -> GpuVertex {
                    GpuVertex vertex = makeVertex(source.position, source.normal, source.blendAlpha);
                    vertex.u = static_cast<float>(source.textureU) / primaryWidth;
                    vertex.v = static_cast<float>(source.textureV) / primaryHeight;
                    // Source's WorldVertexTransition uses the same normalized
                    // vBaseTexCoord for both $basetexture and $basetexture2.
                    vertex.u2 = vertex.u;
                    vertex.v2 = vertex.v;
                    vertex.tx = static_cast<float>(source.tangentS.x);
                    vertex.ty = static_cast<float>(source.tangentS.y);
                    vertex.tz = static_cast<float>(source.tangentS.z);
                    float nx = static_cast<float>(source.normal.x);
                    float ny = static_cast<float>(source.normal.y);
                    float nz = static_cast<float>(source.normal.z);
                    float tx = vertex.tx, ty = vertex.ty, tz = vertex.tz;
                    float bx = static_cast<float>(source.tangentT.x);
                    float by = static_cast<float>(source.tangentT.y);
                    float bz = static_cast<float>(source.tangentT.z);
                    auto normalizeFrame = [](float& x, float& y, float& z) {
                        const float len = std::sqrt(x*x + y*y + z*z);
                        if (!std::isfinite(len) || len <= 1e-8f) return false;
                        x /= len; y /= len; z /= len;
                        return true;
                    };
                    if (normalizeFrame(nx, ny, nz) &&
                        normalizeFrame(tx, ty, tz) && normalizeFrame(bx, by, bz)) {
                        const float crossX = ny * tz - nz * ty;
                        const float crossY = nz * tx - nx * tz;
                        const float crossZ = nx * ty - ny * tx;
                        // tangentT tracks textureV, which the vertices no longer
                        // negate, so the handedness test flips with it.
                        vertex.tangentSign =
                            (crossX * bx + crossY * by + crossZ * bz) < 0.0f ? 1.0f : -1.0f;
                    }
                    return vertex;
                };

                if (displacementSurface) {
                    // Use the exact full-resolution index list generated by the
                    // Source/Hammer CCoreDispInfo path. All renderers must see
                    // the same topology or diagonals/normals diverge by mode.
                    if (face.displacementIndices.empty()) continue;
                    for (std::size_t tri = 0; tri + 2 < face.displacementIndices.size(); tri += 3) {
                        bool valid = true;
                        for (std::size_t corner = 0; corner < 3; ++corner) {
                            const std::size_t vertexIndex = face.displacementIndices[tri + corner];
                            if (vertexIndex >= face.displacementVertices.size()) {
                                valid = false;
                                break;
                            }
                        }
                        if (!valid) continue;
                        for (std::size_t corner = 0; corner < 3; ++corner) {
                            const auto& vertex = face.displacementVertices[
                                face.displacementIndices[tri + corner]];
                            batch.vertices.push_back(makeDisplacementVertex(vertex));
                        }
                    }
                } else {
                    auto makeBrushVertex = [&](std::size_t index) -> GpuVertex {
                        if (index >= brush.vertices.size()) return {};
                        return makeVertex(brush.vertices[index], face.normal, 0.0);
                    };
                    const GpuVertex first = makeBrushVertex(face.vertices.front());
                    for (std::size_t index = 1; index + 1 < face.vertices.size(); ++index) {
                        batch.vertices.push_back(first);
                        batch.vertices.push_back(makeBrushVertex(face.vertices[index]));
                        batch.vertices.push_back(makeBrushVertex(face.vertices[index + 1]));
                    }
                }
            }
        }

        // Decals and overlays used to rebuild tangents and stream a fresh VBO
        // for every projected surface on every frame. Cache their immutable
        // clipped triangles alongside world geometry and only rebuild on scene
        // invalidation.
        std::vector<GpuVertex> projectedVertices;
        static const std::vector<hammer::vmf::EntityMarker> kNoEntities;
        for (const hammer::vmf::EntityMarker& entity :
                 buildProjectedSurfaces ? scene.entities : kNoEntities) {
            for (const hammer::vmf::ProjectedSurface& surface : entity.projectedSurfaces) {
                if (surface.material.empty() || surface.triangles.empty()) continue;
                const auto material = materials.material(surface.material);
                if (!material || material->missing || !material->image.valid()) continue;

                std::vector<GpuVertex> surfaceVertices;
                surfaceVertices.reserve(surface.triangles.size());
                hammer::vmf::Vec3 minimum{
                    std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max()};
                hammer::vmf::Vec3 maximum{
                    std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
                for (const auto& source : surface.triangles) {
                    surfaceVertices.push_back({static_cast<float>(source.position.x),
                                               static_cast<float>(source.position.y),
                                               static_cast<float>(source.position.z),
                                               static_cast<float>(source.u),
                                               static_cast<float>(source.v),
                                               static_cast<float>(source.normal.x),
                                               static_cast<float>(source.normal.y),
                                               static_cast<float>(source.normal.z),
                                               static_cast<float>(source.u),
                                               static_cast<float>(source.v), 0.0f});
                    minimum.x = std::min(minimum.x, source.position.x);
                    minimum.y = std::min(minimum.y, source.position.y);
                    minimum.z = std::min(minimum.z, source.position.z);
                    maximum.x = std::max(maximum.x, source.position.x);
                    maximum.y = std::max(maximum.y, source.position.y);
                    maximum.z = std::max(maximum.z, source.position.z);
                }
                buildTriangleTangents(surfaceVertices);

                ProjectedGpuBatch batch;
                batch.material = material;
                batch.minimum = minimum;
                batch.maximum = maximum;
                batch.first = static_cast<GLint>(projectedVertices.size());
                batch.count = static_cast<GLsizei>(surfaceVertices.size());
                projectedVertices.insert(projectedVertices.end(),
                                         surfaceVertices.begin(), surfaceVertices.end());
                projectedBatches_.push_back(std::move(batch));
            }
        }

        std::size_t totalVertices = 0;
        for (const auto& [key, batch] : batches) {
            Q_UNUSED(key);
            totalVertices += batch.vertices.size();
        }
        std::vector<GpuVertex> vertices;
        vertices.reserve(totalVertices);
        targetBatches.reserve(batches.size());
        for (auto& [key, batch] : batches) {
            Q_UNUSED(key);
            batch.first = static_cast<GLint>(vertices.size());
            batch.count = static_cast<GLsizei>(batch.vertices.size());
            vertices.insert(vertices.end(), batch.vertices.begin(), batch.vertices.end());
            batch.vertices.clear();
            batch.vertices.shrink_to_fit();
            targetBatches.push_back(std::move(batch));
        }

        if (!vertices.empty()) {
            // Brush and displacement vertices already carry a tangent frame
            // derived from their VMF texture axes. Reconstructing it per
            // triangle creates seams and flat-looking normal-map highlights.
            glGenBuffers(1, &targetVbo);
            glBindBuffer(GL_ARRAY_BUFFER, targetVbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)),
                         vertices.data(), GL_STATIC_DRAW);
            if (useVertexArray_) {
                glGenVertexArrays(1, &targetVao);
                glBindVertexArray(targetVao);
                glBindBuffer(GL_ARRAY_BUFFER, targetVbo);
                configureVertexAttributes();
                glBindVertexArray(vao_);
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        }

        if (!projectedVertices.empty()) {
            glGenBuffers(1, &projectedVbo_);
            glBindBuffer(GL_ARRAY_BUFFER, projectedVbo_);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(projectedVertices.size() * sizeof(GpuVertex)),
                         projectedVertices.data(), GL_STATIC_DRAW);
            if (useVertexArray_) {
                glGenVertexArrays(1, &projectedVao_);
                glBindVertexArray(projectedVao_);
                glBindBuffer(GL_ARRAY_BUFFER, projectedVbo_);
                configureVertexAttributes();
                glBindVertexArray(vao_);
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
        }

        animatedWaterOut = animatedWater;
    }

    // Which solids the 3D view treats as "moving": the selection plus, for a
    // selected brush entity, every solid it owns. Both halves are drawn the
    // same way, so a superset here only shifts work between buffers.
    void collectWorldDynamicIds(const hammer::vmf::Scene& scene,
                                const std::vector<hammer::vmf::ObjectRef>& selection)
    {
        worldLineage_.dynamicSolidIds.clear();
        worldLineage_.dynamicEntityIds.clear();
        if (selection.empty()) return;
        const auto selected = [&selection](const hammer::vmf::ObjectRef& object) {
            return std::find(selection.begin(), selection.end(), object) != selection.end();
        };
        for (const hammer::vmf::ObjectRef& object : selection) {
            if (object.type == hammer::vmf::ObjectType::Solid)
                worldLineage_.dynamicSolidIds.insert(object.id);
            else
                worldLineage_.dynamicEntityIds.insert(object.id);
        }
        for (const hammer::vmf::BrushGeometry& brush : scene.brushes) {
            if (selected(brush.object) ||
                (brush.ownerEntityId >= 0 &&
                 selected({hammer::vmf::ObjectType::Entity, brush.ownerEntityId}))) {
                worldLineage_.dynamicSolidIds.insert(brush.id);
            }
        }
    }

    void rebuildWorldCache(const hammer::vmf::Scene& scene,
                           hammer::assets::MaterialSystem& materials,
                           const std::unordered_set<std::string>& hiddenToolTextures,
                           bool displacementSolidMask,
                           const std::vector<hammer::vmf::ObjectRef>& selection)
    {
        clearWorldCache();
        collectWorldDynamicIds(scene, selection);
        worldLineage_.revision = scene.revision;
        bool staticWater = false;
        bool dynamicWater = false;
        buildWorldBrushBuffer(scene, materials, hiddenToolTextures, displacementSolidMask,
                              false, true, worldVbo_, worldVao_, worldBatches_, staticWater);
        buildWorldBrushBuffer(scene, materials, hiddenToolTextures, displacementSolidMask,
                              true, false, worldDynamicVbo_, worldDynamicVao_,
                              worldDynamicBatches_, dynamicWater);
        worldMaterialOwner_ = &materials;
        worldHiddenToolTextures_ = hiddenToolTextures;
        worldDisplacementSolidMask_ = displacementSolidMask;
        worldStaticHasAnimatedWater_ = staticWater;
        worldHasAnimatedWater_ = staticWater || dynamicWater;
    }

    // A drag step: only the moving solids are re-assembled and re-uploaded.
    void rebuildWorldDynamicCache(const hammer::vmf::Scene& scene,
                                  hammer::assets::MaterialSystem& materials,
                                  const std::unordered_set<std::string>& hiddenToolTextures,
                                  bool displacementSolidMask)
    {
        clearWorldDynamicCache();
        bool dynamicWater = false;
        buildWorldBrushBuffer(scene, materials, hiddenToolTextures, displacementSolidMask,
                              true, false, worldDynamicVbo_, worldDynamicVao_,
                              worldDynamicBatches_, dynamicWater);
        worldHasAnimatedWater_ = worldStaticHasAnimatedWater_ || dynamicWater;
    }

    void bindGeometryBuffer(GLuint vbo, GLuint vao)
    {
        if (useVertexArray_) {
            glBindVertexArray(vao);
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            configureVertexAttributes();
        }
    }

    void unbindWorldGeometry()
    {
        if (useVertexArray_) {
            glBindVertexArray(vao_);
        } else {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(3);
            glDisableVertexAttribArray(4);
            glDisableVertexAttribArray(5);
            glDisableVertexAttribArray(6);
            glDisableVertexAttribArray(7);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    // Water is drawn in its own pass after every opaque draw, so the refraction
    // grab can contain the geometry behind the surface. Splitting the three
    // passes here keeps that ordering explicit.
    enum class WorldPass { Opaque, Water, Translucent };

    void drawWorldBatchList(GLuint vbo, GLuint vao,
                            const std::vector<MaterialBatch>& list, WorldPass pass)
    {
            if (!vbo || list.empty()) return;
            bindGeometryBuffer(vbo, vao);
            for (const MaterialBatch& batch : list) {
                const TextureRecord& texture = textureFor(batch.name, batch.material);
                if (!texture.id) continue;
                const bool water = texture.water && batch.material;
                // Alpha-tested Source materials are binary cutouts: they belong in
                // the opaque/depth-writing pass even if $alpha also modulates their
                // test coverage. Only true $translucent/$alpha surfaces blend.
                const bool alphaTest = batch.material && batch.material->alphaTest;
                // Water matches the ray-traced preview, which is opaque, so it
                // owns its pixels and its depth rather than blending.
                const bool translucent = texture.translucent && !water && !alphaTest;
                const WorldPass batchPass = water ? WorldPass::Water
                    : (translucent ? WorldPass::Translucent : WorldPass::Opaque);
                if (batchPass != pass) continue;
                glDepthMask(translucent ? GL_FALSE : GL_TRUE);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture.id);
                configureMaterialEffects(batch.material, texture, batch.cubemapIndex);
                program_->setUniformValue("uForceOpaque", (!translucent && !water) ? 1 : 0);
                const bool blended = !water && texture.hasSecondTexture;
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, blended ? texture.secondaryId : 0);
                glActiveTexture(GL_TEXTURE0);
                program_->setUniformValue("uHasTexture2", blended ? 1 : 0);

                // Water stays two-sided: the underside draw is what gives a
                // camera below the surface its underwater colour (the shader
                // flips the interface via interfaceSide).
                const bool twoSided = batch.displacement || water ||
                    (batch.material && batch.material->compileTrigger);
                if (twoSided) glDisable(GL_CULL_FACE);
                else glEnable(GL_CULL_FACE);
                program_->setUniformValue("uWater", water ? 1 : 0);
                program_->setUniformValue("uWaterHasFlowMap",
                                          water && texture.hasFlowMap ? 1 : 0);
                if (water) {
                    const auto& material = *batch.material;
                    program_->setUniformValue("uWaterFogColor", QVector3D(
                        material.waterFogColor[0], material.waterFogColor[1], material.waterFogColor[2]));
                    program_->setUniformValue("uWaterRefractTint", QVector3D(
                        material.waterRefractTint[0], material.waterRefractTint[1], material.waterRefractTint[2]));
                    program_->setUniformValue("uWaterReflectTint", QVector3D(
                        material.waterReflectTint[0], material.waterReflectTint[1], material.waterReflectTint[2]));
                    program_->setUniformValue("uWaterFresnel", material.waterFresnelReflectance);
                    program_->setUniformValue("uWaterReflectAmount", material.waterReflectAmount);
                    program_->setUniformValue("uWaterRefractAmount", material.waterRefractAmount);
                    program_->setUniformValue("uWaterReflectBlendFactor",
                                              material.waterReflectBlendFactor);
                    program_->setUniformValue("uWaterFogStart", material.waterFogStart);
                    program_->setUniformValue("uWaterFogEnd", material.waterFogEnd);
                    program_->setUniformValue("uWaterAlpha", material.waterAlpha);
                    program_->setUniformValue("uWaterNormalScale", material.waterNormalScale);
                    program_->setUniformValue("uWaterScale", QVector2D(
                        material.waterScale[0], material.waterScale[1]));
                    program_->setUniformValue("uWaterMultiTexture",
                                              material.waterMultiTexture ? 1 : 0);
                    program_->setUniformValue("uWaterNoFresnel",
                                              material.waterNoFresnel ? 1 : 0);
                    program_->setUniformValue("uWaterScrollOffsetA", QVector2D(
                        wrappedUnitPhase(animationSeconds_, material.waterScroll1[0]),
                        wrappedUnitPhase(animationSeconds_, material.waterScroll1[1])));
                    program_->setUniformValue("uWaterScrollOffsetB", QVector2D(
                        wrappedUnitPhase(animationSeconds_, material.waterScroll2[0]),
                        wrappedUnitPhase(animationSeconds_, material.waterScroll2[1])));
                    program_->setUniformValue("uWaterFlowPhase",
                        wrappedUnitPhase(animationSeconds_, material.waterFlowCycleRate));
                    program_->setUniformValue("uWaterFlowDistance", material.waterFlowDistance);
                    program_->setUniformValue("uWaterFlowMapScale", material.waterFlowMapScale);
                    program_->setUniformValue("uWaterFlowNormalUvScale", material.waterFlowNormalUvScale);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, texture.hasFlowMap ? texture.flowId : 0);
                    glActiveTexture(GL_TEXTURE0);
                }
                glDrawArrays(GL_TRIANGLES, batch.first, batch.count);
            }
            unbindWorldGeometry();
        }

    // Captures the scene behind the water, then draws every water surface. Runs
    // after all opaque geometry (world + props) so the capture is complete.
    void drawWaterSurfaces()
    {
        if ((!worldVbo_ || worldBatches_.empty()) &&
            (!worldDynamicVbo_ || worldDynamicBatches_.empty())) {
            return;
        }
        const bool captured = captureSceneBehindWater();
        program_->setUniformValue("uHasSceneCapture", captured ? 1 : 0);
        if (captured) {
            program_->setUniformValue("uSceneSize",
                QVector2D(static_cast<float>(renderWidth_),
                          static_cast<float>(renderHeight_)));
            // Linearizing the sampled depth needs the exact planes the
            // projection was built with (see viewProjection()).
            program_->setUniformValue("uDepthPlanes",
                QVector2D(static_cast<float>(std::max(0.05, sceneNearPlane_)), FarPlane));
            glActiveTexture(GL_TEXTURE8);
            glBindTexture(GL_TEXTURE_2D, sceneColorCopy_);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, sceneDepthCopy_);
            glActiveTexture(GL_TEXTURE0);
        }

        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        program_->setUniformValue("uUseTexture", 1);
        program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
        glDisable(GL_BLEND);
        drawWorldBatchList(worldVbo_, worldVao_, worldBatches_, WorldPass::Water);
        drawWorldBatchList(worldDynamicVbo_, worldDynamicVao_, worldDynamicBatches_,
                           WorldPass::Water);

        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uHasSceneCapture", 0);
        program_->setUniformValue("uForceOpaque", 0);
        clearMaterialEffects();
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);
    }

    void drawMaterials(const hammer::vmf::Scene& scene,
                       hammer::assets::MaterialSystem& materials,
                       const std::unordered_set<std::string>& hiddenToolTextures,
                       bool displacementSolidMask,
                       const std::vector<hammer::vmf::ObjectRef>& selection,
                       bool lightmapGrid)
    {
        // World brush surfaces only: models, sprites and projected surfaces
        // leave the attribute at (0,0), which would read as a solid grid line.
        program_->setUniformValue("uLightmapGrid", lightmapGrid ? 1 : 0);
        // A selection change alone does NOT rebuild these buffers: the split
        // between the static and dynamic halves only matters once selected
        // geometry actually moves, and classifySceneCache already answers
        // Rebuild on the first drag step whose changed solids are not in the
        // dynamic set. Rebuilding on click made every selection click cost a
        // full-map re-upload.
        const SceneCacheState worldState = classifySceneCache(worldLineage_, &scene);
        if (clearGeometryPending_ || worldState == SceneCacheState::Rebuild ||
            worldMaterialOwner_ != &materials ||
            worldHiddenToolTextures_ != hiddenToolTextures ||
            worldDisplacementSolidMask_ != displacementSolidMask) {
            rebuildWorldCache(scene, materials, hiddenToolTextures, displacementSolidMask,
                              selection);
            clearGeometryPending_ = false;
        } else if (worldState == SceneCacheState::DynamicOnly) {
            rebuildWorldDynamicCache(scene, materials, hiddenToolTextures, displacementSolidMask);
            worldLineage_.revision = scene.revision;
        }
        hasAnimatedWater_ = worldHasAnimatedWater_;
        if ((!worldVbo_ || worldBatches_.empty()) &&
            (!worldDynamicVbo_ || worldDynamicBatches_.empty())) {
            return;
        }

        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        program_->setUniformValue("uUseTexture", 1);
        program_->setUniformValue("uForceOpaque", 0);
        program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
        glActiveTexture(GL_TEXTURE0);

        // Opaque batches do not need blending. This avoids unnecessary
        // read/modify/write work on every world pixel and prevents accidental
        // texture alpha from making ordinary geometry look translucent.
        //
        // The static and dynamic halves are two buffers holding the same kind
        // of geometry, so each pass draws both. Their union is exactly the set
        // of batches the single world buffer used to hold.

        // Water is deliberately absent here. It is drawn by drawWaterSurfaces()
        // once props are down and the scene behind it has been captured.
        for (const WorldPass pass : {WorldPass::Opaque, WorldPass::Translucent}) {
            if (pass == WorldPass::Translucent) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            drawWorldBatchList(worldVbo_, worldVao_, worldBatches_, pass);
            drawWorldBatchList(worldDynamicVbo_, worldDynamicVao_, worldDynamicBatches_, pass);
        }
        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uWaterMultiTexture", 0);
        program_->setUniformValue("uWaterFlowPhase", 0.0f);
        program_->setUniformValue("uHasTexture2", 0);
        program_->setUniformValue("uForceOpaque", 0);
        clearMaterialEffects();
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);
    }


    static QMatrix4x4 entityTransform(const hammer::vmf::EntityMarker& entity)
    {
        const hammer::camera::SourceTransform transform =
            hammer::camera::sourceTransform(entity.origin, entity.renderAngles());
        const auto& basis = transform.basis;
        return QMatrix4x4(
            static_cast<float>(basis.forward.x), static_cast<float>(basis.left.x),
            static_cast<float>(basis.up.x), static_cast<float>(entity.origin.x),
            static_cast<float>(basis.forward.y), static_cast<float>(basis.left.y),
            static_cast<float>(basis.up.y), static_cast<float>(entity.origin.y),
            static_cast<float>(basis.forward.z), static_cast<float>(basis.left.z),
            static_cast<float>(basis.up.z), static_cast<float>(entity.origin.z),
            0.0f, 0.0f, 0.0f, 1.0f);
    }

    void clearStudioGpuCache()
    {
        studioModelLookupCache_.clear();
        for (auto& [name, model] : studioGpuModels_) {
            Q_UNUSED(name);
            for (StudioGpuMesh& mesh : model.meshes) {
                if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
                if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
            }
        }
        studioGpuModels_.clear();
    }

    std::shared_ptr<const hammer::assets::StudioModel> studioModelFor(
        const std::string& path)
    {
        if (const auto found = studioModelLookupCache_.find(path);
            found != studioModelLookupCache_.end()) {
            return found->second;
        }
        const auto model = studioModels_ ? studioModels_->model(path)
                                         : std::shared_ptr<const hammer::assets::StudioModel>{};
        studioModelLookupCache_.emplace(path, model);
        return model;
    }

    StudioGpuModel& studioGpuModel(const hammer::assets::StudioModel& model,
                                    hammer::assets::MaterialSystem& materials)
    {
        if (const auto found = studioGpuModels_.find(model.name);
            found != studioGpuModels_.end()) return found->second;

        StudioGpuModel prepared;
        auto finishBatch = [&](std::size_t sourceMeshIndex,
                               const hammer::assets::StudioMesh& sourceMesh,
                               std::vector<StudioGpuVertex>& vertices,
                               std::vector<int>& paletteBones,
                               bool allVerticesHaveAuthoredTangents) {
            if (vertices.empty()) return;
            // Missing VVD tangents are reconstructed in source/bind space once;
            // the resulting frame is then transformed by the GPU bone palette.
            if (!allVerticesHaveAuthoredTangents) buildTriangleTangents(vertices);

            StudioGpuMesh mesh;
            mesh.materialSlot = sourceMesh.materialSlot;
            mesh.sourceMeshIndex = sourceMeshIndex;
            mesh.paletteBones = paletteBones;
            mesh.skinMaterials.reserve(static_cast<std::size_t>(model.skinCount()));
            mesh.skinTextures.reserve(static_cast<std::size_t>(model.skinCount()));
            for (int skin = 0; skin < model.skinCount(); ++skin) {
                const std::string materialName = model.materialForSkin(sourceMesh.materialSlot, skin);
                const std::shared_ptr<const hammer::assets::Material> material = materialName.empty()
                    ? std::shared_ptr<const hammer::assets::Material>{}
                    : materials.material(materialName);
                mesh.skinMaterials.push_back(material);
                mesh.skinTextures.push_back(material
                    ? textureFor(material->name, material) : TextureRecord{});
            }
            mesh.count = static_cast<GLsizei>(vertices.size());
            glGenBuffers(1, &mesh.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(vertices.size() * sizeof(StudioGpuVertex)),
                         vertices.data(), GL_STATIC_DRAW);
            if (useVertexArray_) {
                glGenVertexArrays(1, &mesh.vao);
                glBindVertexArray(mesh.vao);
                glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
                configureStudioVertexAttributes();
                glBindVertexArray(vao_);
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }
            prepared.meshes.push_back(std::move(mesh));
            vertices.clear();
            paletteBones.clear();
        };

        for (std::size_t sourceMeshIndex = 0; sourceMeshIndex < model.meshes.size(); ++sourceMeshIndex) {
            const hammer::assets::StudioMesh& sourceMesh = model.meshes[sourceMeshIndex];
            if (sourceMesh.vertices.empty()) continue;

            std::vector<StudioGpuVertex> vertices;
            std::vector<int> paletteBones;
            std::unordered_map<int, int> paletteSlots;
            bool allVerticesHaveAuthoredTangents = true;
            vertices.reserve(sourceMesh.vertices.size());
            paletteBones.reserve(MaxStudioPaletteBones);

            auto flush = [&]() {
                finishBatch(sourceMeshIndex, sourceMesh, vertices, paletteBones,
                            allVerticesHaveAuthoredTangents);
                paletteSlots.clear();
                allVerticesHaveAuthoredTangents = true;
            };

            for (std::size_t triangle = 0; triangle + 2 < sourceMesh.vertices.size(); triangle += 3) {
                std::array<int, 9> triangleBones{};
                int triangleBoneCount = 0;
                for (int corner = 0; corner < 3; ++corner) {
                    const auto& source = sourceMesh.vertices[triangle + static_cast<std::size_t>(corner)];
                    for (int influence = 0; influence < std::min<int>(source.influenceCount, 3); ++influence) {
                        if (source.boneWeights[static_cast<std::size_t>(influence)] <= 0.0f) continue;
                        const int bone = source.boneIndices[static_cast<std::size_t>(influence)];
                        if (bone < 0 || bone >= static_cast<int>(model.referencePoseMatrices.size())) continue;
                        bool duplicate = false;
                        for (int existing = 0; existing < triangleBoneCount; ++existing)
                            duplicate = duplicate || triangleBones[static_cast<std::size_t>(existing)] == bone;
                        if (!duplicate && triangleBoneCount < static_cast<int>(triangleBones.size()))
                            triangleBones[static_cast<std::size_t>(triangleBoneCount++)] = bone;
                    }
                }

                int newBones = 0;
                for (int index = 0; index < triangleBoneCount; ++index)
                    if (paletteSlots.find(triangleBones[static_cast<std::size_t>(index)]) == paletteSlots.end())
                        ++newBones;
                if (!vertices.empty() &&
                    static_cast<int>(paletteBones.size()) + newBones > MaxStudioPaletteBones) {
                    flush();
                }
                for (int index = 0; index < triangleBoneCount; ++index) {
                    const int bone = triangleBones[static_cast<std::size_t>(index)];
                    if (paletteSlots.find(bone) == paletteSlots.end()) {
                        const int local = static_cast<int>(paletteBones.size());
                        paletteSlots.emplace(bone, local);
                        paletteBones.push_back(bone);
                    }
                }

                for (int corner = 0; corner < 3; ++corner) {
                    const auto& source = sourceMesh.vertices[triangle + static_cast<std::size_t>(corner)];
                    StudioGpuVertex vertex;
                    vertex.x = source.x;
                    vertex.y = source.y;
                    vertex.z = source.z;
                    vertex.nx = source.nx;
                    vertex.ny = source.ny;
                    vertex.nz = source.nz;
                    vertex.tx = source.tx;
                    vertex.ty = source.ty;
                    vertex.tz = source.tz;
                    vertex.tangentSign = source.tangentSign;
                    vertex.u = source.u;
                    vertex.v = source.v;
                    vertex.u2 = source.u;
                    vertex.v2 = source.v;
                    float* weights[3]{&vertex.boneWeight0, &vertex.boneWeight1,
                                      &vertex.boneWeight2};
                    float* indices[3]{&vertex.boneIndex0, &vertex.boneIndex1,
                                      &vertex.boneIndex2};
                    bool hasGpuInfluence = false;
                    for (int influence = 0; influence < std::min<int>(source.influenceCount, 3); ++influence) {
                        const float weight = source.boneWeights[static_cast<std::size_t>(influence)];
                        const int globalBone = source.boneIndices[static_cast<std::size_t>(influence)];
                        const auto found = paletteSlots.find(globalBone);
                        if (weight <= 0.0f || found == paletteSlots.end()) continue;
                        *weights[influence] = weight;
                        *indices[influence] = static_cast<float>(found->second);
                        hasGpuInfluence = true;
                    }
                    if (hasGpuInfluence) {
                        vertex.x = source.sourcePosition[0];
                        vertex.y = source.sourcePosition[1];
                        vertex.z = source.sourcePosition[2];
                        vertex.nx = source.sourceNormal[0];
                        vertex.ny = source.sourceNormal[1];
                        vertex.nz = source.sourceNormal[2];
                        vertex.tx = source.sourceTangent[0];
                        vertex.ty = source.sourceTangent[1];
                        vertex.tz = source.sourceTangent[2];
                    }
                    allVerticesHaveAuthoredTangents =
                        allVerticesHaveAuthoredTangents && source.hasTangent;
                    vertices.push_back(vertex);
                }
            }
            flush();
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return studioGpuModels_.emplace(model.name, std::move(prepared)).first->second;
    }

    bool resolveStudioPose(StudioGpuModel& prepared,
                           const hammer::assets::StudioModel& model,
                           int sequence, double cycle)
    {
        const std::int64_t cycleKey = sequence >= 0
            ? static_cast<std::int64_t>(std::llround(cycle * 1000000.0)) : -1;
        if (prepared.cachedSequence == sequence && prepared.cachedCycleKey == cycleKey)
            return !prepared.cachedPose.empty();

        std::vector<hammer::assets::StudioBoneMatrix> pose;
        if (sequence >= 0) {
            if (!studioModels_ ||
                !studioModels_->sampleAnimationMatrices(model, sequence, cycle, pose)) {
                sequence = -1;
            }
        }
        if (sequence < 0) pose = model.referencePoseMatrices;
        prepared.cachedSequence = sequence;
        prepared.cachedCycleKey = sequence >= 0 ? cycleKey : -1;
        prepared.cachedPose = std::move(pose);
        return !prepared.cachedPose.empty();
    }

    void uploadStudioPalette(const StudioGpuMesh& mesh,
                             const std::vector<hammer::assets::StudioBoneMatrix>& pose)
    {
        if (mesh.paletteBones.empty() || pose.empty()) {
            program_->setUniformValue("uGpuSkinning", 0);
            return;
        }
        std::array<QVector4D, MaxStudioPaletteBones> row0{};
        std::array<QVector4D, MaxStudioPaletteBones> row1{};
        std::array<QVector4D, MaxStudioPaletteBones> row2{};
        for (int index = 0; index < MaxStudioPaletteBones; ++index) {
            row0[static_cast<std::size_t>(index)] = QVector4D(1.0f, 0.0f, 0.0f, 0.0f);
            row1[static_cast<std::size_t>(index)] = QVector4D(0.0f, 1.0f, 0.0f, 0.0f);
            row2[static_cast<std::size_t>(index)] = QVector4D(0.0f, 0.0f, 1.0f, 0.0f);
        }
        for (std::size_t local = 0; local < mesh.paletteBones.size(); ++local) {
            const int global = mesh.paletteBones[local];
            if (global < 0 || global >= static_cast<int>(pose.size())) continue;
            const auto& value = pose[static_cast<std::size_t>(global)].values;
            row0[local] = QVector4D(value[0], value[1], value[2], value[3]);
            row1[local] = QVector4D(value[4], value[5], value[6], value[7]);
            row2[local] = QVector4D(value[8], value[9], value[10], value[11]);
        }
        const int count = static_cast<int>(mesh.paletteBones.size());
        program_->setUniformValueArray("uBoneRow0", row0.data(), count);
        program_->setUniformValueArray("uBoneRow1", row1.data(), count);
        program_->setUniformValueArray("uBoneRow2", row2.data(), count);
        program_->setUniformValue("uGpuSkinning", 1);
    }

    void bindStudioMesh(const StudioGpuMesh& mesh)
    {
        if (useVertexArray_) {
            glBindVertexArray(mesh.vao);
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
            configureStudioVertexAttributes();
        }
    }

    void unbindStudioMesh()
    {
        if (useVertexArray_) {
            glBindVertexArray(vao_);
        } else {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(3);
            glDisableVertexAttribArray(4);
            glDisableVertexAttribArray(5);
            glDisableVertexAttribArray(6);
            glDisableVertexAttribArray(7);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    void drawStudioModel(const hammer::vmf::EntityMarker& entity,
                         const hammer::assets::StudioModel& model,
                         hammer::assets::MaterialSystem& materials,
                         bool shadedTexturing)
    {
        StudioGpuModel& prepared = studioGpuModel(model, materials);
        if (prepared.meshes.empty()) return;

        int sequenceIndex = entity.animationSequenceIndex;
        if ((sequenceIndex < 0 || sequenceIndex >= model.sequenceCount()) &&
            !entity.animationSequence.empty())
            sequenceIndex = model.sequenceIndex(entity.animationSequence);
        if ((sequenceIndex < 0 || sequenceIndex >= model.sequenceCount()) && entity.animateModel &&
            entity.animationSequence.empty())
            sequenceIndex = model.sequenceCount() > 0 ? 0 : -1;

        double cycle = 0.0;
        if (sequenceIndex >= 0 && sequenceIndex < model.sequenceCount()) {
            const hammer::assets::StudioSequence& sequence =
                model.sequences[static_cast<std::size_t>(sequenceIndex)];
            if (entity.animationCycle >= 0.0) {
                cycle = entity.animationCycle;
            } else if (sequence.duration > 1e-6f) {
                const double rate = entity.animationPlaybackRate;
                const double elapsedCycle = animationSeconds_ * rate / sequence.duration;
                cycle = rate < 0.0 ? 1.0 + elapsedCycle : elapsedCycle;
                const bool timelineActive = sequence.looping ||
                    (rate > 0.0 && cycle < 1.0) || (rate < 0.0 && cycle > 0.0);
                if (timelineActive && std::abs(rate) > 1e-8) hasAnimatedModels_ = true;
            }
        } else {
            sequenceIndex = -1;
        }
        resolveStudioPose(prepared, model, sequenceIndex, cycle);

        program_->setUniformValue("uModel", entityTransform(entity));
        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uHasTexture2", 0);
        program_->setUniformValue("uShaded", shadedTexturing ? 1 : 0);
        // Studio models use the same clockwise Source winding as world brushes.
        // Culling hidden triangles substantially reduces prop fragment work,
        // especially on character models with several overlapping body meshes.
        glEnable(GL_CULL_FACE);

        const int selectedSkin = model.normalizedSkin(entity.skin);
        // A prop is one object, so it takes a single probe chosen at its origin
        // rather than a per-face assignment.
        const int propCubemapIndex = bakedCubemaps_.empty()
            ? -1 : hammer::render::nearestCubemapIndex(bakedCubemaps_, entity.origin);
        for (const StudioGpuMesh& mesh : prepared.meshes) {
            if (!mesh.vbo || mesh.count <= 0) continue;
            const std::shared_ptr<const hammer::assets::Material> material =
                selectedSkin >= 0 && selectedSkin < static_cast<int>(mesh.skinMaterials.size())
                    ? mesh.skinMaterials[static_cast<std::size_t>(selectedSkin)]
                    : std::shared_ptr<const hammer::assets::Material>{};
            const TextureRecord& texture =
                selectedSkin >= 0 && selectedSkin < static_cast<int>(mesh.skinTextures.size())
                    ? mesh.skinTextures[static_cast<std::size_t>(selectedSkin)]
                    : emptyTexture_;
            const bool textured = texture.id != 0;
            const bool alphaTest = material && material->alphaTest;
            const bool translucent = textured && texture.translucent && !alphaTest;

            // Opaque props must not inherit the renderer's global blend state.
            // Many VTFs contain incidental alpha even when the VMT is opaque;
            // blending those values made props look ghosted.
            if (translucent) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            glDepthMask(translucent ? GL_FALSE : GL_TRUE);
            program_->setUniformValue("uForceOpaque", translucent ? 0 : 1);
            program_->setUniformValue("uUseTexture", textured ? 1 : 0);
            configureMaterialEffects(material, texture, propCubemapIndex);
            if (material && material->uberEffect) {
                const auto color = hammer::assets::previewUberColor(*material, selectedSkin);
                program_->setUniformValue("uColor2",
                    QVector3D(color[0], color[1], color[2]));
            }
            program_->setUniformValue("uColor", textured
                ? QVector4D(1.0f, 1.0f, 1.0f, 1.0f)
                : QVector4D(entity.displayColor[0] / 255.0f,
                            entity.displayColor[1] / 255.0f,
                            entity.displayColor[2] / 255.0f, 1.0f));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture.id);
            uploadStudioPalette(mesh, prepared.cachedPose);
            bindStudioMesh(mesh);
            glDrawArrays(GL_TRIANGLES, 0, mesh.count);
            unbindStudioMesh();
        }

        program_->setUniformValue("uGpuSkinning", 0);
        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        program_->setUniformValue("uForceOpaque", 0);
        clearMaterialEffects();
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);
    }


    void drawBillboardSprite(const hammer::vmf::EntityMarker& entity,
                             const std::shared_ptr<const hammer::assets::Material>& material,
                             const hammer::camera::State& camera)
    {
        if (!material || !material->image.valid()) return;
        const TextureRecord& texture = textureFor(material->name, material);
        if (!texture.id) return;

        // Single source of truth shared with the 3D billboard ray pick and the
        // 2D helper drawing/picking, so a sprite's selection box always hugs
        // exactly the quad drawn here.
        const hammer::vmf::BillboardSize billboard = hammer::vmf::billboardSpriteSize(
            entity, material->image.width, material->image.height);
        const double width = billboard.width;
        const double height = billboard.height;

        const hammer::vmf::Vec3 right = hammer::camera::rightVector(camera);
        const hammer::vmf::Vec3 up = hammer::camera::upVector(camera);
        auto point = [&](double x, double y) {
            return hammer::vmf::Vec3{
                entity.origin.x + right.x * x + up.x * y,
                entity.origin.y + right.y * x + up.y * y,
                entity.origin.z + right.z * x + up.z * y};
        };
        const auto make = [](const hammer::vmf::Vec3& position, float u, float v) {
            return GpuVertex{static_cast<float>(position.x), static_cast<float>(position.y),
                             static_cast<float>(position.z), u, v, 0.0f, 0.0f, 1.0f,
                             u, v, 0.0f};
        };
        const double halfWidth = width * 0.5;
        const double halfHeight = height * 0.5;
        const hammer::vmf::Vec3 bottomLeft = point(-halfWidth, -halfHeight);
        const hammer::vmf::Vec3 bottomRight = point(halfWidth, -halfHeight);
        const hammer::vmf::Vec3 topRight = point(halfWidth, halfHeight);
        const hammer::vmf::Vec3 topLeft = point(-halfWidth, halfHeight);
        const std::vector<GpuVertex> vertices{
            make(bottomLeft, 0.0f, 1.0f), make(bottomRight, 1.0f, 1.0f), make(topRight, 1.0f, 0.0f),
            make(bottomLeft, 0.0f, 1.0f), make(topRight, 1.0f, 0.0f), make(topLeft, 0.0f, 0.0f)};

        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uHasTexture2", 0);
        clearMaterialEffects();
        program_->setUniformValue("uShaded", 0);
        program_->setUniformValue("uUseTexture", 1);
        program_->setUniformValue("uForceOpaque", 0);
        program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        uploadAndDraw(vertices, GL_TRIANGLES);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glDepthMask(GL_TRUE);
    }

    // Source detail props: the grass and weeds VBSP scatters over surfaces
    // whose material declares a "%detailtype". The emission is a physics-free
    // but seeded random walk over every face, so it is cached against the
    // scene revision; only the camera-facing sprite expansion is per frame.
    const hammer::assets::DetailPropEmission& detailProps(
        const hammer::vmf::Scene& scene, hammer::assets::MaterialSystem& materials)
    {
        if (detailScene_ == &scene && detailRevision_ == scene.revision) return detailProps_;
        if (detailScene_ == &scene && !solidsChangedSince(detailRevision_, scene)) {
            detailRevision_ = scene.revision;
            return detailProps_;
        }
        detailScene_ = &scene;
        detailRevision_ = scene.revision;
        detailProps_ = {};

        const std::string dictionaryName =
            scene.detailVbspName.empty() ? std::string("detail.vbsp") : scene.detailVbspName;
        if (!detailDictionaryLoaded_ || detailDictionaryName_ != dictionaryName) {
            detailDictionaryName_ = dictionaryName;
            detailDictionaryLoaded_ = true;
            detailDictionary_ = {};
            if (const auto fileSystem = materials.fileSystem()) {
                detailDictionary_ =
                    hammer::assets::loadDetailObjectDictionary(*fileSystem, dictionaryName);
            }
        }
        if (detailDictionary_.empty()) return detailProps_;

        detailProps_ = hammer::assets::emitDetailProps(
            scene, detailDictionary_, [&materials](std::string_view name) {
                const auto material = materials.material(std::string(name));
                return material ? material->detailType : std::string();
            });
        return detailProps_;
    }

    void drawDetailProps(const hammer::vmf::Scene& scene,
                         hammer::assets::MaterialSystem& materials,
                         const hammer::camera::State& camera,
                         bool shadedTexturing)
    {
        const hammer::assets::DetailPropEmission& emission = detailProps(scene, materials);
        if (emission.props.empty()) return;

        // CDetailObjectSystem draws nothing past cl_detaildist and fades the
        // last cl_detailfade units in. Without this the preview shows every
        // prop in the map at once, which is far denser than the game.
        const hammer::assets::DetailPropFade fade =
            hammer::assets::detailPropFadeForScene(scene);

        // Every detail sprite in a map is cut from one atlas - worldspawn's
        // "detailmaterial" - so they all batch together.
        const std::string spriteMaterialName =
            scene.detailMaterial.empty() ? std::string("detail/detailsprites")
                                         : scene.detailMaterial;
        const auto spriteMaterial = materials.material(spriteMaterialName);
        std::vector<GpuVertex> spriteVertices;
        const auto makeVertex = [](const hammer::vmf::Vec3& position,
                                   const hammer::vmf::Vec3& normal, float u, float v,
                                   float alpha) {
            return GpuVertex{static_cast<float>(position.x), static_cast<float>(position.y),
                             static_cast<float>(position.z), u, v,
                             static_cast<float>(normal.x), static_cast<float>(normal.y),
                             static_cast<float>(normal.z), u, v, alpha};
        };

        for (const hammer::assets::DetailPropInstance& prop : emission.props) {
            if (prop.type == hammer::assets::DetailPropType::Model) continue;
            if (!spriteMaterial || !spriteMaterial->image.valid()) break;
            const float alpha =
                hammer::assets::detailPropAlpha(prop.origin, camera.position, fade);
            if (alpha <= 0.0f) continue;
            const hammer::assets::DetailSpriteQuad quad =
                hammer::assets::detailSpriteQuad(prop, camera.position);
            const auto corner = [&](int index) {
                return makeVertex(quad.corners[static_cast<std::size_t>(index)], quad.normal,
                                  quad.texCoords[static_cast<std::size_t>(index)][0],
                                  quad.texCoords[static_cast<std::size_t>(index)][1], alpha);
            };
            spriteVertices.push_back(corner(0));
            spriteVertices.push_back(corner(1));
            spriteVertices.push_back(corner(2));
            spriteVertices.push_back(corner(0));
            spriteVertices.push_back(corner(2));
            spriteVertices.push_back(corner(3));
        }

        if (!spriteVertices.empty() && spriteMaterial) {
            const TextureRecord& texture = textureFor(spriteMaterial->name, spriteMaterial);
            if (texture.id) {
                program_->setUniformValue("uWater", 0);
                program_->setUniformValue("uWaterHasFlowMap", 0);
                program_->setUniformValue("uHasTexture2", 0);
                clearMaterialEffects();
                configureMaterialEffects(spriteMaterial, texture, -1);
                program_->setUniformValue("uShaded", shadedTexturing ? 1 : 0);
                program_->setUniformValue("uUseTexture", 1);
                program_->setUniformValue("uForceOpaque", 0);
                // The distance fade rides in each vertex's blend-alpha slot.
                program_->setUniformValue("uVertexAlpha", 1);
                program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
                QMatrix4x4 identityModel;
                program_->setUniformValue("uModel", identityModel);
                // Foliage sprites are alpha tested, not sorted: they write
                // depth so they occlude each other correctly from any angle.
                glEnable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glDepthMask(GL_TRUE);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture.id);
                uploadAndDraw(spriteVertices, GL_TRIANGLES);
                clearMaterialEffects();
            }
        }

        // Detail models are ordinary studio models placed by the same emitter.
        // They are rare (a detail type gives them a fraction of a percent of
        // its amount), so one draw each is affordable.
        if (studioModels_) {
            for (const hammer::assets::DetailPropInstance& prop : emission.props) {
                if (prop.type != hammer::assets::DetailPropType::Model || prop.model.empty())
                    continue;
                // Models are culled at the same distance. They are solid
                // geometry, so they are not faded - the original only ever
                // applies the fade alpha to sprites.
                if (hammer::assets::detailPropAlpha(prop.origin, camera.position, fade) <= 0.0f)
                    continue;
                const auto model = studioModelFor(prop.model);
                if (!model || !model->valid) continue;
                hammer::vmf::EntityMarker placement;
                placement.model = prop.model;
                placement.origin = prop.origin;
                placement.angles = prop.angles;
                if (!entityVisibleInClip(placement, viewProjectionMatrix_)) continue;
                drawStudioModel(placement, *model, materials, shadedTexturing);
            }
        }

        program_->setUniformValue("uUseTexture", 1);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
    }

    // move_rope / keyframe_rope strands, expanded into camera-facing ribbons.
    //
    // The strand polylines come from the shared builder, which reruns the
    // original editor's rope settle; that is far too expensive per frame, so
    // the strands are cached against the scene revision and only the ribbon
    // expansion (which depends on the camera) is redone each paint.
    void drawRopes(const hammer::vmf::Scene& scene,
                   hammer::assets::MaterialSystem& materials,
                   const hammer::camera::State& camera,
                   bool shadedTexturing)
    {
        if (ropeScene_ != &scene || ropeRevision_ != scene.revision) {
            ropeScene_ = &scene;
            ropeRevision_ = scene.revision;
            ropeStrands_ = hammer::vmf::buildRopeStrands(scene);
        }
        if (ropeStrands_.empty()) return;

        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uHasTexture2", 0);
        clearMaterialEffects();
        // A rope is lit world geometry, not an editor aid, so it shades with
        // the same switch the props do - which is also what the RT viewport
        // does, since rope triangles there are not tagged TriangleUnlit.
        program_->setUniformValue("uShaded", shadedTexturing ? 1 : 0);
        program_->setUniformValue("uUseTexture", 1);
        program_->setUniformValue("uForceOpaque", 0);
        program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        glEnable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0);

        for (const hammer::vmf::RopeStrand& strand : ropeStrands_) {
            const auto material = materials.material(strand.material);
            if (!material || !material->image.valid()) continue;
            const TextureRecord& texture = textureFor(material->name, material);
            if (!texture.id) continue;

            const double vPerUnit = hammer::vmf::ropeTextureVPerUnit(
                strand, material->image.height);
            const double halfWidth = strand.width * 0.5;

            // Per-point side vector: perpendicular to both the rope and the
            // direction to the eye, averaged across the two segments meeting at
            // the point so the ribbon does not crease at a node.
            const std::size_t pointCount = strand.points.size();
            std::vector<hammer::vmf::Vec3> sides(pointCount);
            std::vector<float> texV(pointCount, 0.0f);
            double travelled = 0.0;
            for (std::size_t point = 0; point < pointCount; ++point) {
                const hammer::vmf::Vec3& position = strand.points[point];
                if (point > 0) {
                    const hammer::vmf::Vec3 step{
                        position.x - strand.points[point - 1].x,
                        position.y - strand.points[point - 1].y,
                        position.z - strand.points[point - 1].z};
                    travelled += std::sqrt(step.x * step.x + step.y * step.y + step.z * step.z);
                }
                texV[point] = static_cast<float>(travelled * vPerUnit);

                const hammer::vmf::Vec3& previous = strand.points[point > 0 ? point - 1 : point];
                const hammer::vmf::Vec3& next =
                    strand.points[point + 1 < pointCount ? point + 1 : point];
                const hammer::vmf::Vec3 along{next.x - previous.x, next.y - previous.y,
                                              next.z - previous.z};
                const hammer::vmf::Vec3 toEye{camera.position.x - position.x,
                                              camera.position.y - position.y,
                                              camera.position.z - position.z};
                hammer::vmf::Vec3 side{along.y * toEye.z - along.z * toEye.y,
                                       along.z * toEye.x - along.x * toEye.z,
                                       along.x * toEye.y - along.y * toEye.x};
                double sideLength = std::sqrt(side.x * side.x + side.y * side.y +
                                              side.z * side.z);
                if (sideLength < 1e-6) {
                    // Looking straight down the rope: any perpendicular will do.
                    side = hammer::camera::rightVector(camera);
                    sideLength = 1.0;
                }
                sides[point] = {side.x / sideLength * halfWidth,
                                side.y / sideLength * halfWidth,
                                side.z / sideLength * halfWidth};
            }

            std::vector<GpuVertex> vertices;
            vertices.reserve((pointCount - 1) * 6);
            const auto make = [](const hammer::vmf::Vec3& position, const hammer::vmf::Vec3& normal,
                                 float u, float v) {
                return GpuVertex{static_cast<float>(position.x), static_cast<float>(position.y),
                                 static_cast<float>(position.z), u, v,
                                 static_cast<float>(normal.x), static_cast<float>(normal.y),
                                 static_cast<float>(normal.z), u, v, 0.0f};
            };
            for (std::size_t segment = 0; segment + 1 < pointCount; ++segment) {
                const hammer::vmf::Vec3& start = strand.points[segment];
                const hammer::vmf::Vec3& end = strand.points[segment + 1];
                const hammer::vmf::Vec3& startSide = sides[segment];
                const hammer::vmf::Vec3& endSide = sides[segment + 1];
                const hammer::vmf::Vec3 normal = hammer::vmf::Vec3{
                    camera.position.x - start.x, camera.position.y - start.y,
                    camera.position.z - start.z};
                const double normalLength = std::max(
                    std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z),
                    1e-6);
                const hammer::vmf::Vec3 unitNormal{normal.x / normalLength,
                                                   normal.y / normalLength,
                                                   normal.z / normalLength};
                const hammer::vmf::Vec3 startLeft{start.x - startSide.x, start.y - startSide.y,
                                                  start.z - startSide.z};
                const hammer::vmf::Vec3 startRight{start.x + startSide.x, start.y + startSide.y,
                                                   start.z + startSide.z};
                const hammer::vmf::Vec3 endLeft{end.x - endSide.x, end.y - endSide.y,
                                                end.z - endSide.z};
                const hammer::vmf::Vec3 endRight{end.x + endSide.x, end.y + endSide.y,
                                                 end.z + endSide.z};
                const float v0 = texV[segment];
                const float v1 = texV[segment + 1];
                vertices.push_back(make(startLeft, unitNormal, 0.0f, v0));
                vertices.push_back(make(startRight, unitNormal, 1.0f, v0));
                vertices.push_back(make(endRight, unitNormal, 1.0f, v1));
                vertices.push_back(make(startLeft, unitNormal, 0.0f, v0));
                vertices.push_back(make(endRight, unitNormal, 1.0f, v1));
                vertices.push_back(make(endLeft, unitNormal, 0.0f, v1));
            }
            if (vertices.empty()) continue;
            glBindTexture(GL_TEXTURE_2D, texture.id);
            uploadAndDraw(vertices, GL_TRIANGLES);
        }

        program_->setUniformValue("uUseTexture", 1);
        glDepthMask(GL_TRUE);
    }

    void drawProjectedSurfaces()
    {
        if (!projectedVbo_ || projectedBatches_.empty()) return;

        QMatrix4x4 identityModel;
        program_->setUniformValue("uModel", identityModel);
        program_->setUniformValue("uWater", 0);
        program_->setUniformValue("uWaterHasFlowMap", 0);
        program_->setUniformValue("uHasTexture2", 0);
        program_->setUniformValue("uShaded", 0);
        program_->setUniformValue("uForceOpaque", 0);
        program_->setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);

        if (useVertexArray_) {
            glBindVertexArray(projectedVao_);
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, projectedVbo_);
            configureVertexAttributes();
        }

        for (const ProjectedGpuBatch& batch : projectedBatches_) {
            if (!batch.material || batch.count <= 0) continue;
            if (!clipCornersVisible(viewProjectionMatrix_,
                                    boundsCorners(batch.minimum, batch.maximum))) {
                continue;
            }
            const TextureRecord& texture = textureFor(batch.material->name, batch.material);
            if (!texture.id) continue;

            program_->setUniformValue("uUseTexture", 1);
            // Decals and overlays inherit the surface they are projected onto,
            // which is already drawn with its own probe.
            configureMaterialEffects(batch.material, texture);
            if (batch.material->decalModulate)
                glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            else
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture.id);
            glDrawArrays(GL_TRIANGLES, batch.first, batch.count);
        }

        if (useVertexArray_) {
            glBindVertexArray(vao_);
        } else {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
            glDisableVertexAttribArray(3);
            glDisableVertexAttribArray(4);
            glDisableVertexAttribArray(5);
            glDisableVertexAttribArray(6);
            glDisableVertexAttribArray(7);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        glDisable(GL_POLYGON_OFFSET_FILL);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        program_->setUniformValue("uForceOpaque", 0);
        clearMaterialEffects();
    }

    void drawEntityCube(const hammer::vmf::EntityMarker& entity)
    {
        std::array<hammer::vmf::Vec3, 8> corners{};
        if (entity.hasSelectionCorners) {
            corners = entity.selectionCorners;
        } else {
            for (int index = 0; index < 8; ++index) {
                corners[static_cast<std::size_t>(index)] = {
                    entity.origin.x + ((index & 1) ? entity.sizeMaximum.x : entity.sizeMinimum.x),
                    entity.origin.y + ((index & 2) ? entity.sizeMaximum.y : entity.sizeMinimum.y),
                    entity.origin.z + ((index & 4) ? entity.sizeMaximum.z : entity.sizeMinimum.z)};
            }
        }
        // Corner bit layout: bit0 = +x, bit1 = +y, bit2 = +z.
        constexpr std::array<std::array<int, 4>, 6> faces{{
            {0, 2, 6, 4}, {1, 5, 7, 3},  // -x, +x
            {0, 4, 5, 1}, {2, 3, 7, 6},  // -y, +y
            {0, 1, 3, 2}, {4, 6, 7, 5}   // -z, +z
        }};
        std::vector<GpuVertex> triangles;
        triangles.reserve(36);
        const auto push = [&triangles, &corners](int cornerIndex) {
            const hammer::vmf::Vec3& p = corners[static_cast<std::size_t>(cornerIndex)];
            triangles.push_back({static_cast<float>(p.x), static_cast<float>(p.y),
                                 static_cast<float>(p.z), 0.0f, 0.0f});
        };
        for (const auto& face : faces) {
            push(face[0]); push(face[1]); push(face[2]);
            push(face[0]); push(face[2]); push(face[3]);
        }
        const QVector4D color(1.0f, 1.0f, 1.0f, 1.0f);
        glDepthMask(GL_TRUE);
        program_->setUniformValue("uUseTexture", 0);
        program_->setUniformValue("uColor", color);
        uploadAndDraw(triangles, GL_TRIANGLES);
        // Darkened edges so the cube reads as a volume without lighting.
        std::vector<GpuVertex> edges;
        appendCornerBoxLines(edges, corners);
        program_->setUniformValue("uColor", QVector4D(color.x() * 0.5f, color.y() * 0.5f,
                                                      color.z() * 0.5f, 1.0f));
        uploadAndDraw(edges, GL_LINES);
    }

    // Props are opaque world content and belong in the water refraction; editor
    // sprites and placeholder cubes are visual aids and must not be, so the two
    // are drawn in separate phases around the water pass.
    enum class HelperPhase { Models, Helpers };

    void drawEntityHelpers(const hammer::vmf::Scene& scene,
                           hammer::assets::MaterialSystem& materials,
                           const hammer::camera::State& camera,
                           bool shadedTexturing,
                           HelperPhase phase = HelperPhase::Helpers)
    {
        for (const hammer::vmf::EntityMarker& entity : scene.entities) {
            if (!entity.projectedSurfaces.empty()) continue;
            // Reject helpers before model-cache lookup, pose evaluation, uniform
            // uploads, and draw submission. Large maps often contain hundreds of
            // props that are completely outside the active camera frustum.
            if (!entityVisibleInClip(entity, viewProjectionMatrix_)) continue;

            bool modelDrawn = false;
            if (!entity.model.empty() && studioModels_) {
                const auto model = studioModelFor(entity.model);
                if (model && model->valid) {
                    if (phase == HelperPhase::Models)
                        drawStudioModel(entity, *model, materials, shadedTexturing);
                    modelDrawn = true;
                }
            }
            if (phase == HelperPhase::Models) continue;
            if (!modelDrawn && !entity.sprite.empty()) {
                drawBillboardSprite(entity, materials.material(entity.sprite), camera);
            } else if (!modelDrawn) {
                // Point entities with no model and no sprite helper (sky_camera,
                // logic entities without an iconsprite, ...) were invisible in
                // the 3D view; draw them as a solid cube of their FGD box in
                // the class display color, as the 2D views already imply.
                drawEntityCube(entity);
            }
        }
        program_->setUniformValue("uUseTexture", 1);
        clearMaterialEffects();
        program_->setUniformValue("uShaded", shadedTexturing ? 1 : 0);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
    }

public:
    // View > Show detail objects. Dense maps scatter tens of thousands of
    // sprites, so this is a real performance switch, not just a visual one.
    void setDetailPropsVisible(bool visible) { detailPropsVisible_ = visible; }

    bool hasAnimatedContent() const {
        return hasAnimatedWater_ || hasAnimatedModels_ || hasAnimatedMaterials_;
    }

private:
    bool initialized_{false};
    bool ready_{false};
    // Desktop OpenGL 4.6 is the only supported context, so these are fixed.
    // Kept as named constants rather than deleted so the branches they guard
    // read the same; the ES and pre-4.6 paths are now unreachable.
    static constexpr bool isOpenGles_ = false;
    static constexpr bool modernContext_ = true;
    static constexpr bool useVertexArray_ = true;
    bool clearTexturesPending_{false};
    bool clearGeometryPending_{false};
    QString error_;
    QString description_;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    GLuint vao_{0};
    GLuint vbo_{0};
    GLuint worldVao_{0};
    GLuint worldVbo_{0};
    GLuint projectedVao_{0};
    GLuint projectedVbo_{0};
    GLuint orthographicBrushVao_{0};
    GLuint orthographicBrushVbo_{0};
    GLsizei orthographicBrushCount_{0};
    std::vector<OrthographicGpuBatch> orthographicEntityBatches_;
    GLuint orthographicSelectionVao_{0};
    GLuint orthographicSelectionVbo_{0};
    GLsizei orthographicSelectionCount_{0};
    GLuint orthographicSelectionBoundsVao_{0};
    GLuint orthographicSelectionBoundsVbo_{0};
    GLsizei orthographicSelectionBoundsCount_{0};
    std::vector<hammer::vmf::ObjectRef> orthographicSelection_;
    MapViewWidget::SelectionMode orthographicSelectionMode_{MapViewWidget::SelectionMode::Groups};
    std::uint64_t orthographicSelectionRevision_{0};
    // Displacement vertex display cache (perspective view).
    const hammer::vmf::Scene* displacementVertexScene_{nullptr};
    std::uint64_t displacementVertexRevision_{0};
    std::vector<hammer::vmf::ObjectRef> displacementVertexSelection_;
    std::vector<GpuVertex> displacementVertexLines_;
    // Selected entity bounds-box cache (perspective view).
    const hammer::vmf::Scene* entityBoundsScene_{nullptr};
    std::uint64_t entityBoundsRevision_{0};
    std::vector<hammer::vmf::ObjectRef> entityBoundsSelection_;
    std::vector<GpuVertex> entityBoundsLines_;
    // Settled move_rope/keyframe_rope strands. The settle is a physics
    // simulation, so it is redone only when the scene revision changes.
    const hammer::vmf::Scene* ropeScene_{nullptr};
    std::uint64_t ropeRevision_{0};
    std::vector<hammer::vmf::RopeStrand> ropeStrands_;
    // Scattered detail props, and the detail.vbsp dictionary they came from.
    const hammer::vmf::Scene* detailScene_{nullptr};
    std::uint64_t detailRevision_{0};
    hammer::assets::DetailPropEmission detailProps_;
    hammer::assets::DetailObjectDictionary detailDictionary_;
    std::string detailDictionaryName_;
    bool detailDictionaryLoaded_{false};
    bool detailPropsVisible_{true};
    GLuint orthographicEditingVao_{0};
    GLuint orthographicEditingVbo_{0};
    GLsizei orthographicEditingCount_{0};
    // Sorted solid ids of the current face selection (see the editing buffer).
    std::vector<int> orthographicEditingSolids_;
    std::uint64_t orthographicEditingRevision_{0};
    std::vector<OrthographicGpuBatch> orthographicEditingEntityBatches_;
    // Sorted ids the static buffers excluded at their last full rebuild; until
    // the next full rebuild those objects draw from the editing buffer.
    std::vector<int> orthographicExcludedSolids_;
    std::vector<int> orthographicExcludedEntities_;
    // Per-model local-space wireframe VBOs plus the transform+color draw lists
    // each 2D cache resolved from its entities.
    std::unordered_map<std::string, OrthographicModelLines> orthographicModelLineCache_;
    std::vector<OrthographicModelDraw> orthographicModelDraws_;
    std::vector<OrthographicModelDraw> orthographicSelectionModelDraws_;
    std::vector<OrthographicModelDraw> orthographicEditingModelDraws_;
    SceneCacheLineage orthographicLineage_;
    const hammer::assets::MaterialSystem* orthographicMaterialOwner_{nullptr};
    // Tool-texture visibility the cached 2D line buffers were built for. The
    // cache is rebuilt when the View > Tool Textures set changes.
    std::unordered_set<std::string> orthographicHiddenToolTextures_;
    GLuint worldDynamicVao_{0};
    GLuint worldDynamicVbo_{0};
    std::vector<MaterialBatch> worldDynamicBatches_;
    SceneCacheLineage worldLineage_;
    bool worldStaticHasAnimatedWater_{false};
    const hammer::assets::MaterialSystem* worldMaterialOwner_{nullptr};
    std::unordered_set<std::string> worldHiddenToolTextures_;
    bool worldDisplacementSolidMask_{true};
    std::vector<MaterialBatch> worldBatches_;
    std::vector<ProjectedGpuBatch> projectedBatches_;
    bool worldHasAnimatedWater_{false};
    bool shadedTexturing_{false};
    bool phongEnabled_{true};
    bool specularEnabled_{true};
    bool bumpMapsEnabled_{true};
    bool lightWarpEnabled_{true};
    bool selfIllumEnabled_{true};
    bool rimLightEnabled_{true};
    float phongIntensity_{1.0f};
    float specularIntensity_{1.0f};
    float bumpMapIntensity_{1.0f};
    const hammer::assets::MaterialSystem* materialOwner_{nullptr};
    std::unique_ptr<hammer::assets::StudioModelSystem> studioModels_;
    std::unordered_map<std::string, TextureRecord> textures_;
    TextureRecord emptyTexture_;
    QMatrix4x4 viewProjectionMatrix_;
    int renderWidth_{0};
    int renderHeight_{0};
    // Scene-behind-water capture (Source's $refracttexture grab, editor-side).
    GLuint sceneFramebuffer_{0};
    bool sceneHasDepthTexture_{false};
    GLenum sceneDepthFormat_{GL_DEPTH_COMPONENT24};
    GLuint sceneCaptureFramebuffer_{0};
    GLuint sceneColorCopy_{0};
    GLuint sceneDepthCopy_{0};
    int sceneCaptureWidth_{0};
    int sceneCaptureHeight_{0};
    double sceneNearPlane_{0.05};
    std::array<std::shared_ptr<hammer::assets::Material>, 6> forcedSkyFaces_{};
    std::unordered_map<std::string, GLuint> materialEnvironmentCubeMaps_;
    GLuint environmentCubeMap_{0};
    // Ray-traced env_cubemap bake: source faces, and the GL cube uploaded from
    // each. The two stay index-aligned with the batch cubemapIndex.
    std::vector<hammer::render::BakedCubemap> bakedCubemaps_;
    std::vector<GLuint> bakedCubemapTextures_;
    bool bakedCubemapTexturesPending_{false};
    std::string environmentSkyName_;
    std::unordered_map<std::string, StudioGpuModel> studioGpuModels_;
    std::unordered_map<std::string,
        std::shared_ptr<const hammer::assets::StudioModel>> studioModelLookupCache_;
    std::chrono::steady_clock::time_point animationStart_{std::chrono::steady_clock::now()};
    const hammer::vmf::Scene* animationScene_{nullptr};
    double animationSeconds_{0.0};
    bool hasAnimatedWater_{false};
    bool hasAnimatedModels_{false};
    bool hasAnimatedMaterials_{false};
};

// Colour + depth render target for the 3D view. Both attachments are textures
// so the water pass can sample a copy of the scene behind the surface, which is
// what real refraction and depth-based fog need. Qt's FBO wrapper cannot do this
// because its depth attachment is always a renderbuffer of a format it picks.
class Hardware3DViewport::SceneTarget
{
public:
    SceneTarget(QOpenGLContext* context, const QSize& size) : size_(size)
    {
        QOpenGLExtraFunctions* gl = context->extraFunctions();
        const bool es = context->isOpenGLES();

        gl->glGenTextures(1, &colorTexture_);
        gl->glBindTexture(GL_TEXTURE_2D, colorTexture_);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        setSampling(gl);

        gl->glGenFramebuffers(1, &framebuffer_);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, colorTexture_, 0);

        // Prefer a sampleable depth texture, which is what unlocks refraction and
        // depth fog. ES2-class contexts may not support one; rather than fail the
        // whole viewport, fall back to a plain depth renderbuffer and let the
        // water shader keep its older depth-free path.
        if (!es || context->format().majorVersion() >= 3) {
            gl->glGenTextures(1, &depthTexture_);
            gl->glBindTexture(GL_TEXTURE_2D, depthTexture_);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(depthInternalFormat_),
                             size.width(), size.height(), 0, GL_DEPTH_COMPONENT,
                             GL_UNSIGNED_INT, nullptr);
            setSampling(gl);
            gl->glBindTexture(GL_TEXTURE_2D, 0);
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                       GL_TEXTURE_2D, depthTexture_, 0);
            valid_ = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (!valid_) {
            if (depthTexture_) {
                gl->glDeleteTextures(1, &depthTexture_);
                depthTexture_ = 0;
                gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                           GL_TEXTURE_2D, 0, 0);
            }
            gl->glGenRenderbuffers(1, &depthRenderbuffer_);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer_);
            gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                                      size.width(), size.height());
            gl->glBindRenderbuffer(GL_RENDERBUFFER, 0);
            gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, depthRenderbuffer_);
            valid_ = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl_ = gl;
    }

    ~SceneTarget()
    {
        if (!gl_) return;
        if (framebuffer_) gl_->glDeleteFramebuffers(1, &framebuffer_);
        if (colorTexture_) gl_->glDeleteTextures(1, &colorTexture_);
        if (depthTexture_) gl_->glDeleteTextures(1, &depthTexture_);
        if (depthRenderbuffer_) gl_->glDeleteRenderbuffers(1, &depthRenderbuffer_);
    }

    SceneTarget(const SceneTarget&) = delete;
    SceneTarget& operator=(const SceneTarget&) = delete;

    bool isValid() const { return valid_; }
    // False on a fallback target: the water pass must then skip refraction and
    // depth fog rather than sample a depth texture that does not exist.
    bool hasDepthTexture() const { return depthTexture_ != 0; }
    QSize size() const { return size_; }
    GLuint handle() const { return framebuffer_; }
    GLenum depthInternalFormat() const { return depthInternalFormat_; }
    void bind() { if (gl_) gl_->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_); }
    void release() { if (gl_) gl_->glBindFramebuffer(GL_FRAMEBUFFER, 0); }

private:
    static void setSampling(QOpenGLExtraFunctions* gl)
    {
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    QOpenGLExtraFunctions* gl_{nullptr};
    QSize size_;
    GLuint framebuffer_{0};
    GLuint colorTexture_{0};
    GLuint depthTexture_{0};
    GLuint depthRenderbuffer_{0};
    GLenum depthInternalFormat_{GL_DEPTH_COMPONENT24};
    bool valid_{false};
};

bool Hardware3DViewport::ensureContext()
{
    if (context_ && context_->isValid() && surface_ && surface_->isValid()) {
        return true;
    }

    releaseRenderer();
    framebuffer_.reset();
    surface_.reset();
    context_.reset();
    contextError_.clear();

    // Do not request a profile, version, multisampling, alpha, or stencil
    // configuration here. Let Qt/EGL choose the platform's compatible default.
    // The Renderer already supports desktop OpenGL 2.1+ and OpenGL ES 2.0+.
    context_ = std::make_unique<QOpenGLContext>();
    if (!context_->create() || !context_->isValid()) {
        setContextError(QStringLiteral(
            "Qt could not create an independent off-screen OpenGL context"));
        context_.reset();
        return false;
    }

    surface_ = std::make_unique<QOffscreenSurface>();
    surface_->setFormat(context_->format());
    surface_->create();
    if (!surface_->isValid()) {
        setContextError(QStringLiteral(
            "Qt created an OpenGL context but could not create its off-screen surface"));
        surface_.reset();
        context_.reset();
        return false;
    }

    if (!context_->makeCurrent(surface_.get())) {
        setContextError(QStringLiteral(
            "The off-screen OpenGL context could not be made current"));
        surface_.reset();
        context_.reset();
        return false;
    }

    if (!renderer_) renderer_ = std::make_unique<Renderer>();
    const bool initialized = renderer_->initialize();
    if (!initialized) setContextError(renderer_->error());
    context_->doneCurrent();
    return initialized;
}

bool Hardware3DViewport::renderFrame()
{
    if (!owner_ || !ensureContext()) return false;
    if (!context_->makeCurrent(surface_.get())) {
        setContextError(QStringLiteral(
            "The off-screen OpenGL context was lost while rendering"));
        return false;
    }

    owner_->applyPendingFit();
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(std::max(1, qRound(width() * dpr)),
                          std::max(1, qRound(height() * dpr)));

    if (!framebuffer_ || framebuffer_->size() != pixelSize) {
        framebuffer_.reset();
        framebuffer_ = std::make_unique<SceneTarget>(context_.get(), pixelSize);
        if (!framebuffer_->isValid()) {
            setContextError(QStringLiteral(
                "OpenGL could not allocate the 3D viewport framebuffer (%1 x %2)")
                    .arg(pixelSize.width()).arg(pixelSize.height()));
            framebuffer_.reset();
            context_->doneCurrent();
            return false;
        }
    }

    framebuffer_->bind();
    QElapsedTimer renderTimer;
    renderTimer.start();
    double renderMs = 0.0;

    // Vertex-tool overlay geometry, shared between the perspective and
    // orthographic paths. Small (one drag session's solids), rebuilt per frame
    // like the grid.
    std::vector<GpuVertex> morphLines;
    std::vector<GpuVertex> morphDispLines;
    const auto appendLine = [](std::vector<GpuVertex>& target, const hammer::vmf::Vec3& a,
                               const hammer::vmf::Vec3& b) {
        target.push_back({static_cast<float>(a.x), static_cast<float>(a.y),
                          static_cast<float>(a.z), 0.0f, 0.0f});
        target.push_back({static_cast<float>(b.x), static_cast<float>(b.y),
                          static_cast<float>(b.z), 0.0f, 0.0f});
    };
    if (owner_->tool_ == MapViewWidget::Tool::Morph && owner_->morphActive_) {
        for (const hammer::vmf::FacePolygons& solid : owner_->morphPreview_) {
            for (const std::vector<hammer::vmf::Vec3>& face : solid) {
                if (face.size() < 2) continue;
                for (std::size_t i = 0; i < face.size(); ++i)
                    appendLine(morphLines, face[i], face[(i + 1) % face.size()]);
            }
        }
        for (const hammer::vmf::MorphDispGrid& grid : owner_->morphDispGrids_) {
            const int size = (1 << grid.power) + 1;
            if (grid.positions.size() != static_cast<std::size_t>(size * size)) continue;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const std::size_t at = static_cast<std::size_t>(y * size + x);
                    if (x + 1 < size)
                        appendLine(morphDispLines, grid.positions[at], grid.positions[at + 1]);
                    if (y + 1 < size)
                        appendLine(morphDispLines, grid.positions[at], grid.positions[at + size]);
                }
            }
        }
    }

    // Mover gizmo (perspective only): morph handle centroid in the vertex
    // tool, selection bounds center in the selection tool.
    bool gizmoVisible = false;
    hammer::vmf::Vec3 gizmoOrigin{};
    double gizmoLength = 0.0;
    int gizmoActiveAxis = -1;
    if (owner_->kind_ == MapViewWidget::Kind::Perspective) {
        if (const auto origin = owner_->gizmoOrigin()) {
            gizmoVisible = true;
            gizmoOrigin = *origin;
            gizmoLength = owner_->morphGizmoWorldLength(*origin);
            gizmoActiveAxis = owner_->morphGizmoAxis_;
        }
    }

    if (owner_->kind_ == MapViewWidget::Kind::Perspective) {
        const bool shadedTexturing =
            owner_->texturedRenderMode_ != MapViewWidget::TexturedRenderMode::Unlit;
        const bool advancedMaterialPreview =
            owner_->texturedRenderMode_ == MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons;
        // Displacement paint and face edits modify solids that are in the face
        // selection but not the object selection. Treating those solids as
        // "moving" too keeps them in the dynamic buffer, so a paint drag
        // re-uploads only them instead of rebuilding the whole map's buffers
        // on every mouse move.
        std::vector<hammer::vmf::ObjectRef> dynamicSelection = owner_->selection_;
        for (const hammer::vmf::FaceRef& face : owner_->faceSelection_) {
            const hammer::vmf::ObjectRef solid{hammer::vmf::ObjectType::Solid, face.solidId};
            if (std::find(dynamicSelection.begin(), dynamicSelection.end(), solid) ==
                dynamicSelection.end()) {
                dynamicSelection.push_back(solid);
            }
        }
        renderer_->setSceneTarget(framebuffer_->handle(), framebuffer_->hasDepthTexture(),
                              framebuffer_->depthInternalFormat());
    // View > Show detail objects, read per frame like the owner's other view
    // switches.
    renderer_->setDetailPropsVisible(owner_->detailPropsVisible_);
    renderer_->render(owner_->scene_.get(), owner_->cameraState_, owner_->projectionMode_,
                          pixelSize.width(), pixelSize.height(), owner_->gridVisible_,
                          owner_->materialRenderingEnabled_, shadedTexturing,
                          advancedMaterialPreview && owner_->phongEnabled_,
                          advancedMaterialPreview && owner_->specularEnabled_,
                          advancedMaterialPreview && owner_->bumpMapsEnabled_,
                          advancedMaterialPreview && owner_->lightWarpEnabled_,
                          advancedMaterialPreview && owner_->selfIllumEnabled_,
                          advancedMaterialPreview && owner_->rimLightEnabled_,
                          owner_->phongIntensity_,
                          owner_->specularIntensity_,
                          owner_->bumpMapIntensity_,
                          owner_->materials_,
                          owner_->hiddenToolTextures_,
                          owner_->displacementSolidMaskEnabled_,
                          dynamicSelection, owner_->lightmapGridVisible_,
                          morphLines, morphDispLines,
                          gizmoVisible, gizmoOrigin, gizmoLength, gizmoActiveAxis);
    } else {
        const QPoint logicalCenter = owner_->rect().center() + owner_->pan_;
        const float deviceScale = static_cast<float>(dpr);
        // Handles show for every selection — including single entities (point
        // or brush); the software path only ever used the single-point-entity
        // case to skip the redundant outer rect and force Translate mode, not
        // to hide the handles.
        const bool showTransformHandles = !owner_->selection_.empty() &&
            owner_->tool_ == MapViewWidget::Tool::Selection;
        const QRectF handleBounds = owner_->selectionScreenBounds();
        const std::array<std::pair<MapViewWidget::Handle, QPointF>, 4> translatePairs =
            MapViewWidget::translateHandlePositions(handleBounds);
        const std::array<QPointF, 4> translateHandles{
            translatePairs[0].second, translatePairs[1].second,
            translatePairs[2].second, translatePairs[3].second};
        // Solids whose faces are in the face selection change per mouse move
        // during a displacement paint without being object-selected; the 2D
        // renderer keeps them out of its static buffers (see the editing
        // buffer in renderOrthographicScene).
        // The Block tool's pending box lives in world space now, shared by
        // every 2D view; this renderer wants it in plane coordinates.
        QPointF pendingBlockFirst;
        QPointF pendingBlockSecond;
        const bool pendingBlockVisible =
            owner_->pendingBlockPlaneRect(pendingBlockFirst, pendingBlockSecond);
        std::vector<int> editingSolidIds;
        for (const hammer::vmf::FaceRef& face : owner_->faceSelection_) {
            editingSolidIds.push_back(face.solidId);
        }
        std::sort(editingSolidIds.begin(), editingSolidIds.end());
        editingSolidIds.erase(std::unique(editingSolidIds.begin(), editingSolidIds.end()),
                              editingSolidIds.end());
        renderer_->renderOrthographicScene(
            owner_->scene_.get(), owner_->materials_, owner_->kind_,
            pixelSize.width(), pixelSize.height(),
            static_cast<float>(logicalCenter.x()) * deviceScale,
            static_cast<float>(logicalCenter.y()) * deviceScale,
            static_cast<float>(owner_->zoom_), deviceScale, owner_->gridVisible_,
            owner_->gridSpacing_,
            owner_->selection_, owner_->selectionMode_, editingSolidIds,
            owner_->hiddenToolTextures_,
            handleBounds, translateHandles,
            !owner_->isSinglePointEntitySelection(),
            showTransformHandles, owner_->effectiveTransformMode(), pendingBlockVisible,
            pendingBlockFirst, pendingBlockSecond,
            morphLines, morphDispLines);
    }
    // Reuse one RGBA image and read the FBO directly into it. Qt's
    // QOpenGLFramebufferObject::toImage() allocates a new image and mirrors the
    // entire viewport on every frame; that full-frame allocation/copy was one of
    // the largest CPU costs while flying the camera. Keep OpenGL's bottom-up row
    // order and flip only during the existing QPainter presentation step.
    const bool perfLog = qEnvironmentVariableIsSet("HAMMER_PERF");
    renderMs = renderTimer.nsecsElapsed() / 1e6;
    QElapsedTimer perfTimer;
    if (perfLog) perfTimer.start();
    if (frame_.size() != pixelSize || frame_.format() != QImage::Format_RGBX8888) {
        frame_ = QImage(pixelSize, QImage::Format_RGBX8888);
    }
    if (!frame_.isNull()) {
        frame_.setDevicePixelRatio(dpr);
        QOpenGLExtraFunctions* functions = context_->extraFunctions();
        functions->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        functions->glReadPixels(0, 0, pixelSize.width(), pixelSize.height(),
                               GL_RGBA, GL_UNSIGNED_BYTE, frame_.bits());
    }
    if (perfLog) {
        fprintf(stderr, "perf render[%d]: draw %.2f ms, readback %.2f ms (%dx%d)\n",
                owner_ ? static_cast<int>(owner_->kind_) : -1,
                renderMs, perfTimer.nsecsElapsed() / 1e6,
                pixelSize.width(), pixelSize.height());
    }
    framebuffer_->release();
    context_->doneCurrent();

    if (!renderer_->error().isEmpty()) {
        setContextError(renderer_->error());
        return false;
    }

    contextError_.clear();
    return !frame_.isNull();
}

Hardware3DViewport::Hardware3DViewport(MapViewWidget* owner)
    : QWidget(owner), owner_(owner), renderer_(std::make_unique<Renderer>())
{
    setObjectName(QStringLiteral("HammerHardware3DViewport"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);

    handleOwnerKindChanged();
}

void Hardware3DViewport::handleOwnerKindChanged()
{
    const bool perspective = owner_ && owner_->kind_ == MapViewWidget::Kind::Perspective;
    if (perspective && !waterAnimationTimer_) {
        waterAnimationTimer_ = new QTimer(this);
        waterAnimationTimer_->setTimerType(Qt::PreciseTimer);
        waterAnimationTimer_->setInterval(33);
        connect(waterAnimationTimer_, &QTimer::timeout, this, [this] {
            const int desiredInterval = owner_ && owner_->mouseCaptured() ? 16 : 33;
            if (waterAnimationTimer_->interval() != desiredInterval)
                waterAnimationTimer_->setInterval(desiredInterval);
            if (isVisible() && renderer_ && renderer_->hasAnimatedContent()) {
                frameDirty_ = true;
                update();
            }
        });
    }
    if (waterAnimationTimer_) {
        if (perspective) waterAnimationTimer_->start();
        else waterAnimationTimer_->stop();
    }
    frameDirty_ = true;
    update();
}

Hardware3DViewport::~Hardware3DViewport()
{
    releaseRenderer();
    surface_.reset();
    context_.reset();
}

void Hardware3DViewport::paintEvent(QPaintEvent*)
{
    bool hardwareFrameReady = !frame_.isNull();
    // A paused view shows its last frame and never re-renders; frameDirty_
    // stays set so the first unpaused paint catches up.
    if ((frameDirty_ || !hardwareFrameReady) && !(owner_ && owner_->renderingPaused_)) {
        hardwareFrameReady = renderFrame();
        if (hardwareFrameReady) frameDirty_ = false;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const bool perspective = owner_ && owner_->kind_ == MapViewWidget::Kind::Perspective;
    painter.fillRect(rect(), perspective ? QColor(12, 12, 12) : QColor(0, 0, 36));

    if (hardwareFrameReady && !frame_.isNull()) {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.translate(0.0, static_cast<qreal>(height()));
        painter.scale(1.0, -1.0);
        painter.drawImage(QPoint(0, 0), frame_);
        painter.restore();
    } else if (owner_ && perspective) {
        // Preserve the legacy emergency fallback only for the 3D camera. The
        // orthographic scene intentionally has no QPainter geometry fallback:
        // grid, brush, and entity primitives are OpenGL-only.
        owner_->drawPerspective(painter);
        owner_->drawSceneBase(painter);
    }

    if (owner_) owner_->paintHardwareOverlay(painter, perspective && hardwareFrameReady);

    if (!contextError_.isEmpty()) {
        painter.setPen(QColor(255, 96, 96));
        painter.drawText(rect().adjusted(12, 32, -12, -12),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("OpenGL renderer error: %1")
                             .arg(contextError_));
    }
}

void Hardware3DViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    frame_ = QImage();
    frameDirty_ = true;
    update();
}

void Hardware3DViewport::invalidateMaterialCache()
{
    if (renderer_) renderer_->invalidateMaterialCache();
    requestUpdate();
}

void Hardware3DViewport::invalidateGeometryCache()
{
    if (renderer_) renderer_->invalidateGeometryCache();
    requestUpdate();
}

void Hardware3DViewport::setBakedCubemaps(std::vector<hammer::render::BakedCubemap> cubemaps)
{
    if (renderer_) renderer_->setBakedCubemaps(std::move(cubemaps));
    requestUpdate();
}

void Hardware3DViewport::requestUpdate(bool rerender)
{
    frameDirty_ = frameDirty_ || rerender;
    update();
}

QString Hardware3DViewport::rendererDescription() const
{
    if (!contextError_.isEmpty()) {
        return QStringLiteral("Off-screen OpenGL unavailable: %1").arg(contextError_);
    }
    return renderer_ ? renderer_->description()
                     : QStringLiteral("OpenGL renderer unavailable");
}

void Hardware3DViewport::releaseRenderer()
{
    if (!renderer_) return;

    if (context_ && context_->isValid() && surface_ && surface_->isValid() &&
        context_->makeCurrent(surface_.get())) {
        framebuffer_.reset();
        renderer_->release();
        context_->doneCurrent();
    } else {
        // Avoid invoking OpenGL-backed destructors without a current context.
        // The driver will reclaim these resources when the context is destroyed.
        framebuffer_.release();
    }
    frame_ = QImage();
    frameDirty_ = true;
}

void Hardware3DViewport::setContextError(const QString& error)
{
    contextError_ = error.trimmed().isEmpty()
        ? QStringLiteral("Unknown OpenGL initialization failure")
        : error.trimmed();
}
