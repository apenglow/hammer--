#include "MaterialRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace HammerMaterialRenderer {
namespace {
struct Vertex
{
    hammer::camera::CameraPoint camera;
    double u{0.0};
    double v{0.0};
    double u2{0.0};
    double v2{0.0};
    double blendAlpha{0.0};
};

struct ScreenVertex
{
    double x{0.0};
    double y{0.0};
    double depth{0.0};
    double u{0.0};
    double v{0.0};
    double u2{0.0};
    double v2{0.0};
    double blendAlpha{0.0};
};

double dot(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b)
{
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

Vertex interpolate(const Vertex& a, const Vertex& b, double t)
{
    return {
        {a.camera.right + (b.camera.right-a.camera.right)*t,
         a.camera.up + (b.camera.up-a.camera.up)*t,
         a.camera.forward + (b.camera.forward-a.camera.forward)*t},
        a.u + (b.u-a.u)*t,
        a.v + (b.v-a.v)*t,
        a.u2 + (b.u2-a.u2)*t,
        a.v2 + (b.v2-a.v2)*t,
        a.blendAlpha + (b.blendAlpha-a.blendAlpha)*t
    };
}

std::vector<Vertex> clipNear(const std::vector<Vertex>& polygon, double nearPlane)
{
    std::vector<Vertex> output;
    if (polygon.empty()) return output;
    Vertex previous = polygon.back();
    bool previousInside = previous.camera.forward >= nearPlane;
    for (const Vertex& current : polygon) {
        const bool currentInside = current.camera.forward >= nearPlane;
        if (currentInside != previousInside) {
            const double denominator = current.camera.forward - previous.camera.forward;
            if (std::abs(denominator) > 1e-12) {
                const double t = std::clamp((nearPlane - previous.camera.forward) / denominator, 0.0, 1.0);
                output.push_back(interpolate(previous, current, t));
            }
        }
        if (currentInside) output.push_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return output;
}

ScreenVertex project(const Vertex& vertex, const hammer::camera::State& state,
                     hammer::camera::ProjectionMode projection,
                     double logicalWidth, double logicalHeight, double dpr)
{
    if (projection == hammer::camera::ProjectionMode::Perspective) {
        const double focal = logicalHeight * dpr / (2.0 * std::tan(state.verticalFovRadians * 0.5));
        return {logicalWidth*dpr*0.5 + vertex.camera.right*focal/vertex.camera.forward,
                logicalHeight*dpr*0.5 - vertex.camera.up*focal/vertex.camera.forward,
                vertex.camera.forward, vertex.u, vertex.v, vertex.u2, vertex.v2, vertex.blendAlpha};
    }
    const double pixelsPerUnit = logicalHeight*dpr / std::max(1.0, state.orthographicHeight);
    return {logicalWidth*dpr*0.5 + vertex.camera.right*pixelsPerUnit,
            logicalHeight*dpr*0.5 - vertex.camera.up*pixelsPerUnit,
            vertex.camera.forward, vertex.u, vertex.v, vertex.u2, vertex.v2, vertex.blendAlpha};
}

double edge(double ax, double ay, double bx, double by, double px, double py)
{
    return (px-ax)*(by-ay) - (py-ay)*(bx-ax);
}

std::uint32_t blendOpaque(std::uint32_t source, std::uint32_t destination)
{
    const int alpha = (source >> 24) & 255;
    if (alpha >= 255) return source | 0xFF000000u;
    if (alpha <= 0) return destination;
    const int inverse = 255-alpha;
    const int sr=(source>>16)&255, sg=(source>>8)&255, sb=source&255;
    const int dr=(destination>>16)&255, dg=(destination>>8)&255, db=destination&255;
    return 0xFF000000u |
           static_cast<std::uint32_t>((sr*alpha+dr*inverse)/255)<<16 |
           static_cast<std::uint32_t>((sg*alpha+dg*inverse)/255)<<8 |
           static_cast<std::uint32_t>((sb*alpha+db*inverse)/255);
}

std::uint32_t mixPixels(std::uint32_t first, std::uint32_t second, double amount)
{
    const int blend = std::clamp(static_cast<int>(std::lround(amount * 255.0)), 0, 255);
    const int inverse = 255 - blend;
    auto channel = [&](int shift) {
        return ((((first >> shift) & 255u) * inverse) +
                 (((second >> shift) & 255u) * blend)) / 255;
    };
    return static_cast<std::uint32_t>(channel(24)) << 24 |
           static_cast<std::uint32_t>(channel(16)) << 16 |
           static_cast<std::uint32_t>(channel(8)) << 8 |
           static_cast<std::uint32_t>(channel(0));
}

void triangle(QImage& target, std::vector<double>& depthBuffer,
              const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
              const hammer::assets::Image& texture,
              const hammer::assets::Image* texture2,
              hammer::camera::ProjectionMode projection)
{
    const double area = edge(a.x,a.y,b.x,b.y,c.x,c.y);
    if (std::abs(area) < 1e-8) return;
    const int minX = std::max(0, static_cast<int>(std::floor(std::min({a.x,b.x,c.x}))));
    const int maxX = std::min(target.width()-1, static_cast<int>(std::ceil(std::max({a.x,b.x,c.x}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min({a.y,b.y,c.y}))));
    const int maxY = std::min(target.height()-1, static_cast<int>(std::ceil(std::max({a.y,b.y,c.y}))));
    if (minX > maxX || minY > maxY) return;

    const double invZa = 1.0/a.depth, invZb=1.0/b.depth, invZc=1.0/c.depth;
    for (int y=minY; y<=maxY; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(target.scanLine(y));
        for (int x=minX; x<=maxX; ++x) {
            const double px=x+0.5, py=y+0.5;
            double wa=edge(b.x,b.y,c.x,c.y,px,py)/area;
            double wb=edge(c.x,c.y,a.x,a.y,px,py)/area;
            double wc=1.0-wa-wb;
            if (wa < -1e-8 || wb < -1e-8 || wc < -1e-8) continue;
            double depth=0.0,u=0.0,v=0.0,u2=0.0,v2=0.0,blendAlpha=0.0;
            if (projection == hammer::camera::ProjectionMode::Perspective) {
                const double invZ=wa*invZa+wb*invZb+wc*invZc;
                if (invZ <= 0.0) continue;
                depth=1.0/invZ;
                u=(wa*a.u*invZa+wb*b.u*invZb+wc*c.u*invZc)*depth;
                v=(wa*a.v*invZa+wb*b.v*invZb+wc*c.v*invZc)*depth;
                u2=(wa*a.u2*invZa+wb*b.u2*invZb+wc*c.u2*invZc)*depth;
                v2=(wa*a.v2*invZa+wb*b.v2*invZb+wc*c.v2*invZc)*depth;
                blendAlpha=(wa*a.blendAlpha*invZa+wb*b.blendAlpha*invZb+
                            wc*c.blendAlpha*invZc)*depth;
            } else {
                depth=wa*a.depth+wb*b.depth+wc*c.depth;
                u=wa*a.u+wb*b.u+wc*c.u;
                v=wa*a.v+wb*b.v+wc*c.v;
                u2=wa*a.u2+wb*b.u2+wc*c.u2;
                v2=wa*a.v2+wb*b.v2+wc*c.v2;
                blendAlpha=wa*a.blendAlpha+wb*b.blendAlpha+wc*c.blendAlpha;
            }
            const std::size_t index=static_cast<std::size_t>(y*target.width()+x);
            if (depth >= depthBuffer[index]) continue;
            depthBuffer[index]=depth;
            std::uint32_t source = texture.sampleWrapped(u,v);
            if (texture2 && texture2->valid()) {
                source = mixPixels(source, texture2->sampleWrapped(u2,v2), blendAlpha);
            }
            row[x]=blendOpaque(source,row[x]);
        }
    }
}
} // namespace

void render(QImage& target, const hammer::vmf::Scene& scene,
            const hammer::camera::State& camera,
            hammer::camera::ProjectionMode projection,
            double logicalWidth, double logicalHeight,
            hammer::assets::MaterialSystem& materials,
            bool displacementSolidMask)
{
    if (target.isNull() || logicalWidth <= 0 || logicalHeight <= 0) return;
    const double dpr=target.devicePixelRatio();
    std::vector<double> depth(static_cast<std::size_t>(target.width()*target.height()),
                              std::numeric_limits<double>::infinity());
    for (const auto& brush : scene.brushes) {
        for (const auto& face : brush.faces) {
            if (face.vertices.size() < 3 && !face.displacement) continue;
            // CMapSolid::Render3D hides the non-displaced sides of a
            // displacement solid in the 3D views.
            if (hammer::vmf::isFaceMaskedByDisplacementSolid(brush, face,
                                                             displacementSolidMask)) {
                continue;
            }
            const auto material=materials.material(face.material);
            if (!material || !material->image.valid()) continue;
            const hammer::assets::Image* secondary =
                material->blended && material->image2.valid() ? &material->image2 : nullptr;
            const double uScale = std::abs(face.uAxis.scale) < 1e-9 ? 0.25 : face.uAxis.scale;
            const double vScale = std::abs(face.vAxis.scale) < 1e-9 ? 0.25 : face.vAxis.scale;

            const double primaryWidth = std::max(1, material->image.width);
            const double primaryHeight = std::max(1, material->image.height);
            const double secondaryWidth = secondary ? std::max(1, secondary->width) : primaryWidth;
            const double secondaryHeight = secondary ? std::max(1, secondary->height) : primaryHeight;
            auto makeVertex = [&](const hammer::vmf::Vec3& world, double blendAlpha) {
                const double u=(dot(world,face.uAxis.direction)/uScale)+face.uAxis.shift;
                const double v=(dot(world,face.vAxis.direction)/vScale)+face.vAxis.shift;
                return Vertex{hammer::camera::toCamera(camera,world),u,v,
                              u * secondaryWidth / primaryWidth,
                              v * secondaryHeight / primaryHeight, blendAlpha};
            };
            auto makeDisplacementVertex = [&](
                const hammer::vmf::DisplacementVertex& source) {
                return Vertex{hammer::camera::toCamera(camera, source.position),
                              source.textureU, source.textureV,
                              source.textureU * secondaryWidth / primaryWidth,
                              source.textureV * secondaryHeight / primaryHeight,
                              source.blendAlpha};
            };
            auto renderPolygon = [&](std::vector<Vertex> polygon) {
                polygon=clipNear(polygon,camera.nearPlane);
                if (polygon.size() < 3) return;
                std::vector<ScreenVertex> screen;
                screen.reserve(polygon.size());
                for (const auto& vertex : polygon) {
                    screen.push_back(project(vertex,camera,projection,
                                             logicalWidth,logicalHeight,dpr));
                }
                for (std::size_t i=1;i+1<screen.size();++i) {
                    triangle(target,depth,screen[0],screen[i],screen[i+1],
                             material->image,secondary,projection);
                }
            };

            if (face.displacement && !face.displacementIndices.empty()) {
                for (std::size_t index = 0; index + 2 < face.displacementIndices.size(); index += 3) {
                    std::vector<Vertex> polygon;
                    polygon.reserve(3);
                    bool valid = true;
                    for (std::size_t corner = 0; corner < 3; ++corner) {
                        const std::size_t vertexIndex = face.displacementIndices[index + corner];
                        if (vertexIndex >= face.displacementVertices.size()) {
                            valid = false;
                            break;
                        }
                        const auto& vertex = face.displacementVertices[vertexIndex];
                        polygon.push_back(makeDisplacementVertex(vertex));
                    }
                    if (valid) renderPolygon(std::move(polygon));
                }
            } else {
                std::vector<Vertex> polygon;
                polygon.reserve(face.vertices.size());
                for (std::size_t index : face.vertices) {
                    if (index >= brush.vertices.size()) continue;
                    polygon.push_back(makeVertex(brush.vertices[index], 0.0));
                }
                renderPolygon(std::move(polygon));
            }
        }
    }
}

} // namespace HammerMaterialRenderer
