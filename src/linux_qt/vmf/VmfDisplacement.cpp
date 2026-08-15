#include "VmfDisplacement.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

// hammer/noise.cpp, compiled into hammer_original_portable. CMapDisp::ApplyNoise
// calls it directly, so the port uses the very same generator.
extern float PerlinNoise2D(float x, float y, float rockiness);

namespace hammer::vmf {
namespace {

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 multiply(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double lengthSquared(const Vec3& a) { return a.x * a.x + a.y * a.y + a.z * a.z; }

// Vector::NormalizeInPlace: returns the old length and leaves a zero vector
// alone, which is what CMapDisp::PaintPosition_Update relies on.
double normalizeInPlace(Vec3& value)
{
    const double length = std::sqrt(lengthSquared(value));
    if (length > 0.0) value = multiply(value, 1.0 / length);
    return length;
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

bool parseNumbers(const std::string& text, double* output, std::size_t count)
{
    std::string cleaned = text;
    for (char& character : cleaned) {
        if (character == '[' || character == ']' || character == '(' || character == ')' ||
            character == ',') {
            character = ' ';
        }
    }
    std::istringstream stream(cleaned);
    for (std::size_t index = 0; index < count; ++index) {
        if (!(stream >> output[index]) || !std::isfinite(output[index])) return false;
    }
    return true;
}

double parseDouble(const std::string* text, double fallback)
{
    double value = fallback;
    if (text && parseNumbers(*text, &value, 1)) return value;
    return fallback;
}

const Block* firstChild(const Block& block, std::string_view name)
{
    const auto children = block.children(name);
    return children.empty() ? nullptr : children.front();
}

Block* firstChild(Block& block, std::string_view name)
{
    const auto children = block.children(name);
    return children.empty() ? nullptr : children.front();
}

std::string rowKey(int row) { return "row" + std::to_string(row); }

// One "row<N>" per grid row, each holding gridSize * components numbers, which
// is the layout CMapDisp::SaveDispData writes.
bool readRows(const Block* block, int gridSize, int components, std::vector<double>& output)
{
    output.assign(static_cast<std::size_t>(gridSize) * gridSize * components, 0.0);
    if (!block) return false;
    const std::size_t stride = static_cast<std::size_t>(gridSize) * components;
    std::vector<double> row(stride, 0.0);
    for (int y = 0; y < gridSize; ++y) {
        const std::string* text = block->value(rowKey(y));
        if (!text || !parseNumbers(*text, row.data(), stride)) return false;
        std::copy(row.begin(), row.end(), output.begin() + static_cast<std::ptrdiff_t>(y * stride));
    }
    return true;
}

void writeRows(Block& parent, std::string_view name, int gridSize, int components,
               const std::vector<double>& values)
{
    Block* block = firstChild(parent, name);
    if (!block) block = &parent.appendChild(std::string(name));
    block->entries.clear();
    const std::size_t stride = static_cast<std::size_t>(gridSize) * components;
    for (int y = 0; y < gridSize; ++y) {
        std::string text;
        for (std::size_t index = 0; index < stride; ++index) {
            if (index != 0) text += ' ';
            text += formatNumber(values[static_cast<std::size_t>(y) * stride + index]);
        }
        block->setValue(rowKey(y), text);
    }
}

std::vector<double> flatten(const std::vector<Vec3>& values)
{
    std::vector<double> output;
    output.reserve(values.size() * 3);
    for (const Vec3& value : values) {
        output.push_back(value.x);
        output.push_back(value.y);
        output.push_back(value.z);
    }
    return output;
}

std::vector<Vec3> unflatten(const std::vector<double>& values)
{
    std::vector<Vec3> output;
    output.reserve(values.size() / 3);
    for (std::size_t index = 0; index + 2 < values.size(); index += 3) {
        output.push_back({values[index], values[index + 1], values[index + 2]});
    }
    return output;
}

// CDispPaintMgr::IsInSphereRadius: strictly inside, and the squared distance is
// handed to the falloff.
bool isInSphereRadius(const Vec3& center, double radiusSquared, const Vec3& position,
                      double& distanceSquared)
{
    distanceSquared = lengthSquared(subtract(position, center));
    return distanceSquared < radiusSquared;
}

// CDispPaintMgr::DoPaintOneOverR / DoPaintOne, factored to the scalar they
// scale the paint axis by.
double paintFalloff(const SpatialPaintData& paint, double distanceSquared)
{
    if (paint.brushType == PaintBrushType::Hard) return paint.scalar;
    const double radiusSquared = paint.radius * paint.radius;
    if (radiusSquared <= 0.0) return paint.scalar;
    return (1.0 - distanceSquared / radiusSquared) * paint.scalar;
}

} // namespace

bool DisplacementInfo::valid() const
{
    if (power < MinDisplacementPower || power > MaxDisplacementPower) return false;
    const std::size_t count = static_cast<std::size_t>(vertexCount());
    return normals.size() == count && distances.size() == count && offsets.size() == count &&
           offsetNormals.size() == count && alphas.size() == count;
}

DisplacementInfo makeDisplacement(int power, const Vec3& startPosition, const Vec3& faceNormal)
{
    DisplacementInfo info;
    info.power = std::clamp(power, MinDisplacementPower, MaxDisplacementPower);
    info.startPosition = startPosition;
    info.elevation = 0.0;
    info.subdiv = false;
    const std::size_t count = static_cast<std::size_t>(info.vertexCount());
    info.normals.assign(count, Vec3{});
    info.distances.assign(count, 0.0);
    info.offsets.assign(count, Vec3{});
    // CMapDisp::ResetFieldData leaves the subdivision normals at the surface
    // normal; those are what the VMF "offset_normals" rows carry.
    info.offsetNormals.assign(count, faceNormal);
    info.alphas.assign(count, 0.0);
    return info;
}

std::optional<DisplacementInfo> readDisplacement(const Block& side)
{
    const Block* disp = firstChild(side, "dispinfo");
    if (!disp) return std::nullopt;

    DisplacementInfo info;
    double power = 0.0;
    const std::string* powerText = disp->value("power");
    if (!powerText || !parseNumbers(*powerText, &power, 1)) return std::nullopt;
    info.power = static_cast<int>(power);
    if (info.power < MinDisplacementPower || info.power > MaxDisplacementPower) return std::nullopt;

    const int gridSize = info.gridSize();
    double start[3] = {0.0, 0.0, 0.0};
    if (const std::string* text = disp->value("startposition")) {
        parseNumbers(*text, start, 3);
    }
    info.startPosition = {start[0], start[1], start[2]};
    info.elevation = parseDouble(disp->value("elevation"), 0.0);
    info.subdiv = parseDouble(disp->value("subdiv"), 0.0) != 0.0;

    std::vector<double> values;
    if (!readRows(firstChild(*disp, "normals"), gridSize, 3, values)) return std::nullopt;
    info.normals = unflatten(values);
    if (!readRows(firstChild(*disp, "distances"), gridSize, 1, values)) return std::nullopt;
    info.distances = values;
    // Offsets, offset normals and alphas are optional in the wild; a missing
    // block reads as zero, exactly as VmfScene::buildDisplacement treats it.
    readRows(firstChild(*disp, "offsets"), gridSize, 3, values);
    info.offsets = unflatten(values);
    readRows(firstChild(*disp, "offset_normals"), gridSize, 3, values);
    info.offsetNormals = unflatten(values);
    readRows(firstChild(*disp, "alphas"), gridSize, 1, values);
    info.alphas = values;

    return info.valid() ? std::optional<DisplacementInfo>(info) : std::nullopt;
}

void writeDisplacement(Block& side, const DisplacementInfo& info)
{
    if (!info.valid()) return;
    Block* disp = firstChild(side, "dispinfo");
    if (!disp) disp = &side.appendChild("dispinfo");

    const int gridSize = info.gridSize();
    disp->setValue("power", std::to_string(info.power));
    // Hammer writes startposition in the bracketed vector form.
    disp->setValue("startposition", "[" + formatNumber(info.startPosition.x) + " " +
                                        formatNumber(info.startPosition.y) + " " +
                                        formatNumber(info.startPosition.z) + "]");
    disp->setValue("elevation", formatNumber(info.elevation));
    disp->setValue("subdiv", info.subdiv ? "1" : "0");

    writeRows(*disp, "normals", gridSize, 3, flatten(info.normals));
    writeRows(*disp, "distances", gridSize, 1, info.distances);
    writeRows(*disp, "offsets", gridSize, 3, flatten(info.offsets));
    writeRows(*disp, "offset_normals", gridSize, 3, flatten(info.offsetNormals));
    writeRows(*disp, "alphas", gridSize, 1, info.alphas);
    // "triangle_tags" and "allowed_verts" are written by vbsp-facing code paths
    // the port does not have; Hammer reloads a dispinfo without them fine.
}

bool removeDisplacement(Block& side)
{
    const auto before = side.entries.size();
    side.entries.erase(std::remove_if(side.entries.begin(), side.entries.end(),
                                      [](const Entry& entry) {
                                          return entry.kind == Entry::Kind::ChildBlock &&
                                                 entry.child && entry.child->name == "dispinfo";
                                      }),
                       side.entries.end());
    return side.entries.size() != before;
}

bool hasDisplacement(const Block& side) { return firstChild(side, "dispinfo") != nullptr; }

std::array<Vec3, 4> orientDisplacementCorners(const std::array<Vec3, 4>& corners,
                                              const Vec3& startPosition)
{
    std::array<Vec3, 4> oriented = corners;
    std::size_t startCorner = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < oriented.size(); ++index) {
        const double candidate = lengthSquared(subtract(oriented[index], startPosition));
        if (candidate < bestDistance) {
            bestDistance = candidate;
            startCorner = index;
        }
    }
    std::rotate(oriented.begin(), oriented.begin() + static_cast<std::ptrdiff_t>(startCorner),
                oriented.end());
    return oriented;
}

std::vector<Vec3> displacementFlatVertices(const std::array<Vec3, 4>& corners, int power)
{
    const int gridSize = displacementGridSize(power);
    std::vector<Vec3> flat(static_cast<std::size_t>(gridSize) * gridSize, Vec3{});
    const double inverseIntervals = 1.0 / static_cast<double>(gridSize - 1);
    const Vec3 edgeInterval0 = multiply(subtract(corners[1], corners[0]), inverseIntervals);
    const Vec3 edgeInterval1 = multiply(subtract(corners[2], corners[3]), inverseIntervals);
    for (int row = 0; row < gridSize; ++row) {
        const Vec3 end0 = add(corners[0], multiply(edgeInterval0, static_cast<double>(row)));
        const Vec3 end1 = add(corners[3], multiply(edgeInterval1, static_cast<double>(row)));
        const Vec3 segmentInterval = multiply(subtract(end1, end0), inverseIntervals);
        for (int column = 0; column < gridSize; ++column) {
            flat[static_cast<std::size_t>(row * gridSize + column)] =
                add(end0, multiply(segmentInterval, static_cast<double>(column)));
        }
    }
    return flat;
}

std::vector<Vec3> displacementVertexPositions(const DisplacementInfo& info,
                                              const std::array<Vec3, 4>& corners,
                                              const Vec3& faceNormal)
{
    if (!info.valid()) return {};
    const std::array<Vec3, 4> oriented = orientDisplacementCorners(corners, info.startPosition);
    std::vector<Vec3> positions = displacementFlatVertices(oriented, info.power);
    const Vec3 elevationVector = multiply(faceNormal, info.elevation);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        positions[index] = add(add(add(positions[index], elevationVector), info.offsets[index]),
                               multiply(info.normals[index], info.distances[index]));
    }
    return positions;
}

bool paintDisplacement(DisplacementInfo& info, const std::array<Vec3, 4>& corners,
                       const Vec3& faceNormal, const SpatialPaintData& paint)
{
    if (!info.valid()) return false;
    const std::array<Vec3, 4> oriented = orientDisplacementCorners(corners, info.startPosition);
    const std::vector<Vec3> flat = displacementFlatVertices(oriented, info.power);
    const std::vector<Vec3> positions = displacementVertexPositions(info, corners, faceNormal);
    if (positions.size() != flat.size()) return false;

    const Vec3 elevationVector = multiply(faceNormal, info.elevation);
    const double radiusSquared = paint.radius * paint.radius;
    bool changed = false;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        double distanceSquared = 0.0;
        if (!isInSphereRadius(paint.center, radiusSquared, positions[index], distanceSquared)) {
            continue;
        }
        const Vec3 painted =
            add(positions[index], multiply(paint.paintAxis, paintFalloff(paint, distanceSquared)));

        // The inverse of the port's own renderer; see the header note.
        Vec3 segment = subtract(subtract(subtract(painted, flat[index]), info.offsets[index]),
                                elevationVector);
        const double distance = normalizeInPlace(segment);
        info.normals[index] = segment;
        info.distances[index] = distance;
        changed = true;
    }
    return changed;
}

bool paintDisplacementAlpha(DisplacementInfo& info, const std::array<Vec3, 4>& corners,
                            const Vec3& faceNormal, const SpatialPaintData& paint)
{
    if (!info.valid()) return false;
    const std::vector<Vec3> positions = displacementVertexPositions(info, corners, faceNormal);
    if (positions.size() != info.alphas.size()) return false;

    const double radiusSquared = paint.radius * paint.radius;
    bool changed = false;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        double distanceSquared = 0.0;
        if (!isInSphereRadius(paint.center, radiusSquared, positions[index], distanceSquared)) {
            continue;
        }
        // CMapDisp::PaintAlpha_Update stores the painted value directly; the
        // canvas value for the alpha channel is the previous alpha plus the
        // falloff-scaled scalar, and CMapDisp clamps the channel to [0..255].
        const double value =
            std::clamp(info.alphas[index] + paintFalloff(paint, distanceSquared), 0.0, 255.0);
        if (value != info.alphas[index]) {
            info.alphas[index] = value;
            changed = true;
        }
    }
    return changed;
}

// -----------------------------------------------------------------------------
// Attribute editing: Resample / Elevate / Scale / Noise.
// -----------------------------------------------------------------------------
namespace {

// Every per-vertex channel the port keeps, packed into one stride so the
// original's componentwise averaging applies uniformly to all of them (the
// original averages field vectors componentwise too, without renormalizing:
// mapdisp.cpp "( dVector[0] + dVector[1] ) * 0.5f").
inline constexpr std::size_t ChannelStride = 11;

std::vector<double> packChannels(const DisplacementInfo& info)
{
    const std::size_t count = info.distances.size();
    std::vector<double> packed(count * ChannelStride, 0.0);
    for (std::size_t index = 0; index < count; ++index) {
        double* slot = packed.data() + index * ChannelStride;
        slot[0] = info.distances[index];
        slot[1] = info.alphas[index];
        slot[2] = info.normals[index].x;
        slot[3] = info.normals[index].y;
        slot[4] = info.normals[index].z;
        slot[5] = info.offsets[index].x;
        slot[6] = info.offsets[index].y;
        slot[7] = info.offsets[index].z;
        slot[8] = info.offsetNormals[index].x;
        slot[9] = info.offsetNormals[index].y;
        slot[10] = info.offsetNormals[index].z;
    }
    return packed;
}

void unpackChannels(const std::vector<double>& packed, DisplacementInfo& info)
{
    const std::size_t count = packed.size() / ChannelStride;
    info.distances.assign(count, 0.0);
    info.alphas.assign(count, 0.0);
    info.normals.assign(count, Vec3{});
    info.offsets.assign(count, Vec3{});
    info.offsetNormals.assign(count, Vec3{});
    for (std::size_t index = 0; index < count; ++index) {
        const double* slot = packed.data() + index * ChannelStride;
        info.distances[index] = slot[0];
        info.alphas[index] = slot[1];
        info.normals[index] = {slot[2], slot[3], slot[4]};
        info.offsets[index] = {slot[5], slot[6], slot[7]};
        info.offsetNormals[index] = {slot[8], slot[9], slot[10]};
    }
}

void copySlot(std::vector<double>& into, std::size_t target, const std::vector<double>& from,
              std::size_t source)
{
    std::copy_n(from.begin() + static_cast<std::ptrdiff_t>(source * ChannelStride), ChannelStride,
                into.begin() + static_cast<std::ptrdiff_t>(target * ChannelStride));
}

void averageSlots(std::vector<double>& into, std::size_t target, const std::vector<double>& from,
                  std::size_t a, std::size_t b)
{
    for (std::size_t channel = 0; channel < ChannelStride; ++channel) {
        into[target * ChannelStride + channel] =
            (from[a * ChannelStride + channel] + from[b * ChannelStride + channel]) * 0.5;
    }
}

// CMapDisp::UpSample( oldPower ). "grid" is the NEW grid size, as the original's
// GetWidth()/GetHeight() are after SetPower.
std::vector<double> upSampleChannels(const std::vector<double>& source, int oldGrid, int grid)
{
    std::vector<double> output(static_cast<std::size_t>(grid) * grid * ChannelStride, 0.0);
    for (int oh = 0, nh = 0; oh < oldGrid; ++oh, nh += 2) {
        for (int ow = 0, nw = 0; ow < oldGrid; ++ow, nw += 2) {
            const auto oldIndex = static_cast<std::size_t>(oh * oldGrid + ow);
            const auto newIndex = static_cast<std::size_t>(nh * grid + nw);

            copySlot(output, newIndex, source, oldIndex);

            const bool right = (ow + 1) < oldGrid;
            const bool up = (oh + 1) < oldGrid;
            if (right) {
                averageSlots(output, newIndex + 1, source, oldIndex, oldIndex + 1);
            }
            if (up) {
                averageSlots(output, newIndex + static_cast<std::size_t>(grid), source, oldIndex,
                             oldIndex + static_cast<std::size_t>(oldGrid));
            }
            if (right && up) {
                // The original averages the RIGHT and UP neighbours here, not
                // the two diagonal corners (mapdisp.cpp lines 616-635). Kept as
                // is so the port resamples identically.
                averageSlots(output, newIndex + static_cast<std::size_t>(grid) + 1, source,
                             oldIndex + 1, oldIndex + static_cast<std::size_t>(oldGrid));
            }
        }
    }
    return output;
}

// CMapDisp::DownSample( oldPower ) + SamplePoints / GetValidSamplePoints.
std::vector<double> downSampleChannels(const std::vector<double>& source, int oldGrid, int grid)
{
    std::vector<double> output(static_cast<std::size_t>(grid) * grid * ChannelStride, 0.0);
    for (int oh = 0, nh = 0; oh < oldGrid; oh += 2, ++nh) {
        for (int ow = 0, nw = 0; ow < oldGrid; ow += 2, ++nw) {
            const int oldIndex = oh * oldGrid + ow;
            const auto newIndex = static_cast<std::size_t>(nh * grid + nw);

            const int x = ow;
            const int y = oh;
            // GetValidSamplePoints' eight neighbours, in its own order.
            const int neighbours[8] = {
                oldIndex - oldGrid - 1, oldIndex - 1, oldIndex + oldGrid - 1, oldIndex + oldGrid,
                oldIndex + oldGrid + 1, oldIndex + 1, oldIndex - oldGrid + 1, oldIndex - oldGrid,
            };
            const bool valid[8] = {
                (x - 1) >= 0 && (y - 1) >= 0,
                (x - 1) >= 0,
                (x - 1) >= 0 && (y + 1) < oldGrid,
                (y + 1) < oldGrid,
                (x + 1) < oldGrid && (y + 1) < oldGrid,
                (x + 1) < oldGrid,
                (x + 1) < oldGrid && (y - 1) >= 0,
                (y - 1) >= 0,
            };

            double accumulator[ChannelStride];
            for (std::size_t channel = 0; channel < ChannelStride; ++channel) {
                accumulator[channel] =
                    source[static_cast<std::size_t>(oldIndex) * ChannelStride + channel];
            }
            int count = 1;
            for (int i = 0; i < 8; ++i) {
                if (!valid[i]) continue;
                const auto index = static_cast<std::size_t>(neighbours[i]);
                for (std::size_t channel = 0; channel < ChannelStride; ++channel) {
                    accumulator[channel] += source[index * ChannelStride + channel];
                }
                ++count;
            }
            for (std::size_t channel = 0; channel < ChannelStride; ++channel) {
                output[newIndex * ChannelStride + channel] =
                    accumulator[channel] / static_cast<double>(count);
            }
        }
    }
    return output;
}

} // namespace

bool resampleDisplacement(DisplacementInfo& info, int power)
{
    if (!info.valid()) return false;
    if (power < MinDisplacementPower || power > MaxDisplacementPower) return false;
    if (power == info.power) return false;

    // CMapDisp::Resample walks one power at a time, sampling against the
    // previous grid on each step.
    std::vector<double> channels = packChannels(info);
    int current = info.power;
    while (current != power) {
        const int next = current + (power > current ? 1 : -1);
        const int currentGrid = displacementGridSize(current);
        const int nextGrid = displacementGridSize(next);
        channels = power > current ? upSampleChannels(channels, currentGrid, nextGrid)
                                   : downSampleChannels(channels, currentGrid, nextGrid);
        current = next;
    }

    info.power = power;
    unpackChannels(channels, info);
    return info.valid();
}

bool elevateDisplacement(DisplacementInfo& info, double elevation)
{
    if (!info.valid() || info.elevation == elevation) return false;
    info.elevation = elevation;
    return true;
}

bool scaleDisplacement(DisplacementInfo& info, double previousScale, double scale)
{
    if (!info.valid() || scale == previousScale) return false;
    // CMapDisp::Scale: un-scale by 1/m_Scale first when the surface is already
    // scaled, then apply the new scale. Only the field distances survive into
    // the VMF, so only they are touched here.
    const double adjust = (previousScale != 0.0 && previousScale != 1.0) ? 1.0 / previousScale : 1.0;
    for (double& distance : info.distances) distance = distance * adjust * scale;
    return true;
}

bool applyDisplacementNoise(DisplacementInfo& info, const std::array<Vec3, 4>& corners,
                            const Vec3& faceNormal, double minimum, double maximum,
                            double rockiness)
{
    // "if( min == max ) return;"
    if (!info.valid() || minimum == maximum) return false;

    const std::array<Vec3, 4> oriented = orientDisplacementCorners(corners, info.startPosition);
    const std::vector<Vec3> flat = displacementFlatVertices(oriented, info.power);
    const std::vector<Vec3> positions = displacementVertexPositions(info, corners, faceNormal);
    if (positions.size() != flat.size()) return false;

    rockiness = std::clamp(rockiness, 0.0, 1.0);
    const double delta = maximum - minimum;
    const double deltaBy2 = delta / 2.0;
    const Vec3 elevationVector = multiply(faceNormal, info.elevation);

    for (std::size_t index = 0; index < positions.size(); ++index) {
        const Vec3& vertex = positions[index];
        double noise = PerlinNoise2D(static_cast<float>(vertex.x + vertex.z),
                                     static_cast<float>(vertex.y + vertex.z),
                                     static_cast<float>(rockiness));
        noise = std::clamp(noise, -1.0, 1.0);
        noise = noise * deltaBy2 + (deltaBy2 + minimum);

        // "apply noise to the subdivision normal direction": the field vector,
        // or - when it is still zero - the offset normal, which is where the
        // port keeps the surface normal a fresh displacement starts with.
        Vec3 direction = info.normals[index];
        if (direction.x == 0.0 && direction.y == 0.0 && direction.z == 0.0) {
            direction = info.offsetNormals[index];
        }
        const Vec3 painted = add(vertex, multiply(direction, noise));

        Vec3 segment = subtract(subtract(subtract(painted, flat[index]), info.offsets[index]),
                                elevationVector);
        const double distance = normalizeInPlace(segment);
        info.normals[index] = segment;
        info.distances[index] = distance;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Sewing (hammer/dispsew.cpp).
// -----------------------------------------------------------------------------

int displacementCornerPointIndex(int gridSize, int corner)
{
    switch (corner) {
    case 0: return 0;
    case 1: return (gridSize - 1) * gridSize;
    case 2: return gridSize * gridSize - 1;
    case 3: return gridSize - 1;
    default: return -1;
    }
}

int displacementEdgePointIndex(int gridSize, int edge, int pointIndex, bool counterClockwise)
{
    if (counterClockwise) {
        switch (edge) {
        case 0: return pointIndex * gridSize;
        case 1: return ((gridSize - 1) * gridSize) + pointIndex;
        case 2: return (gridSize * gridSize - 1) - (pointIndex * gridSize);
        case 3: return (gridSize - 1) - pointIndex;
        default: return -1;
        }
    }
    switch (edge) {
    case 0: return ((gridSize - 1) * gridSize) - (pointIndex * gridSize);
    case 1: return (gridSize * gridSize - 1) - pointIndex;
    case 2: return (gridSize - 1) + (pointIndex * gridSize);
    case 3: return pointIndex;
    default: return -1;
    }
}

namespace {

// dispsew.cpp PointCompareWithTolerance.
bool pointCompareWithTolerance(const Vec3& a, const Vec3& b, double tolerance)
{
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

// dispsew.cpp EdgeCompare, reduced to the answer the port acts on: the two
// edges share both endpoints and neither is a T-junction (the midpoint of one
// edge falling on an endpoint of the other).
bool edgesAreNormalMatch(const Vec3& a0, const Vec3& a1, const Vec3& b0, const Vec3& b1)
{
    const Vec3 edgeA[3] = {a0, multiply(add(a0, a1), 0.5), a1};
    const Vec3 edgeB[3] = {b0, multiply(add(b0, b1), 0.5), b1};

    int overlapCount = 0;
    int indexA[2] = {0, 0};
    int indexB[2] = {0, 0};
    for (int i = 0; i < 3 && overlapCount < 2; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (!pointCompareWithTolerance(edgeA[i], edgeB[j], SewPointTolerance)) continue;
            // "no midpoint to midpoint sharing allowed"
            if ((i % 2 != 0) && (j % 2 != 0)) continue;
            indexA[overlapCount] = i;
            indexB[overlapCount] = j;
            ++overlapCount;
            break;
        }
    }
    if (overlapCount != 2) return false;
    // A midpoint taking part in the overlap is a T-junction on that side; the
    // port does not sew those (see the header note).
    return (indexA[0] % 2 == 0) && (indexA[1] % 2 == 0) && (indexB[0] % 2 == 0) &&
           (indexB[1] % 2 == 0);
}

bool materialsMatch(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(a[index])) !=
            std::tolower(static_cast<unsigned char>(b[index]))) {
            return false;
        }
    }
    return true;
}

// dispsew.cpp AverageVectorFieldData, minus the subdivision rows the port does
// not keep.
void averageVectorFieldData(SewSurface& first, std::size_t indexFirst, SewSurface& second,
                            std::size_t indexSecond)
{
    DisplacementInfo& a = *first.info;
    DisplacementInfo& b = *second.info;

    Vec3 average = multiply(add(multiply(a.normals[indexFirst], a.distances[indexFirst]),
                                multiply(b.normals[indexSecond], b.distances[indexSecond])),
                            0.5);
    const double distance = normalizeInPlace(average);
    a.distances[indexFirst] = distance;
    b.distances[indexSecond] = distance;
    a.normals[indexFirst] = average;
    b.normals[indexSecond] = average;

    if (materialsMatch(first.material, second.material)) {
        // NOTE: the original writes ( flAlpha1 + flAlpha1 ) * 0.5f here, i.e.
        // the first surface's alpha is copied onto both. Replicated so a sewn
        // seam blends the same way it does in Hammer.
        const double blended = (a.alphas[indexFirst] + a.alphas[indexFirst]) * 0.5;
        a.alphas[indexFirst] = blended;
        b.alphas[indexSecond] = blended;
    }
}

// dispsew.cpp BlendVectorFieldData, for the single-surface case the normal-edge
// resolve uses (both sides are the larger displacement).
void blendVectorFieldData(SewSurface& surface, std::size_t sourceStart, std::size_t sourceEnd,
                          std::size_t destination, double blendFactor)
{
    DisplacementInfo& info = *surface.info;
    const Vec3 start = multiply(info.normals[sourceStart], info.distances[sourceStart]);
    const Vec3 end = multiply(info.normals[sourceEnd], info.distances[sourceEnd]);
    Vec3 blended = add(start, multiply(subtract(end, start), blendFactor));
    const double distance = normalizeInPlace(blended);
    info.distances[destination] = distance;
    info.normals[destination] = blended;
}

// dispsew.cpp SewEdge_ResolveDispNormal.
void resolveEdgeDispNormal(SewSurface& first, int edgeFirst, SewSurface& second, int edgeSecond)
{
    const int gridFirst = first.info->gridSize();
    const int gridSecond = second.info->gridSize();

    SewSurface* small = &first;
    SewSurface* large = &second;
    int edgeSmall = edgeFirst;
    int edgeLarge = edgeSecond;
    int smallInterval = gridFirst;
    int largeInterval = gridSecond;
    if (gridFirst > gridSecond) {
        small = &second;
        large = &first;
        edgeSmall = edgeSecond;
        edgeLarge = edgeFirst;
        smallInterval = gridSecond;
        largeInterval = gridFirst;
    }

    const int ratio = (largeInterval - 1) / (smallInterval - 1);
    if (ratio < 1) return;

    // Average the "like" points. The end points are left to corner sewing.
    for (int indexSmall = 1, indexLarge = ratio; indexSmall < (smallInterval - 1);
         ++indexSmall, indexLarge += ratio) {
        const int pointSmall =
            displacementEdgePointIndex(smallInterval, edgeSmall, indexSmall, true);
        const int pointLarge =
            displacementEdgePointIndex(largeInterval, edgeLarge, indexLarge, false);
        if (pointSmall < 0 || pointLarge < 0) continue;
        averageVectorFieldData(*small, static_cast<std::size_t>(pointSmall), *large,
                               static_cast<std::size_t>(pointLarge));
    }

    // Linearly interpolate the "unlike" points across the larger surface.
    const double blendRatio = 1.0 / static_cast<double>(ratio);
    for (int start = 0; start < (largeInterval - 1); start += ratio) {
        const int end = start + ratio;
        const int startPoint = displacementEdgePointIndex(largeInterval, edgeLarge, start, true);
        const int endPoint = displacementEdgePointIndex(largeInterval, edgeLarge, end, true);
        if (startPoint < 0 || endPoint < 0) continue;
        for (int index = start + 1; index < end; ++index) {
            const double blend = blendRatio * static_cast<double>(index - start);
            const int destination =
                displacementEdgePointIndex(largeInterval, edgeLarge, index, true);
            if (destination < 0) continue;
            blendVectorFieldData(*large, static_cast<std::size_t>(startPoint),
                                 static_cast<std::size_t>(endPoint),
                                 static_cast<std::size_t>(destination), blend);
        }
    }
}

// dispsew.cpp SewCorner_ResolveDisp over one cluster of coincident corners.
void resolveCornerDisp(std::vector<SewSurface*>& surfaces, std::vector<int>& corners)
{
    const auto faceCount = static_cast<double>(surfaces.size());
    if (faceCount < 2.0) return;

    bool blendAlpha = true;
    for (std::size_t index = 1; index < surfaces.size(); ++index) {
        if (!materialsMatch(surfaces[0]->material, surfaces[index]->material)) {
            blendAlpha = false;
            break;
        }
    }

    double averageDistance = 0.0;
    Vec3 averageField{};
    double averageAlpha = 0.0;
    std::vector<std::size_t> points(surfaces.size(), 0);
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        DisplacementInfo& info = *surfaces[index]->info;
        const int point = displacementCornerPointIndex(info.gridSize(), corners[index]);
        if (point < 0) return;
        points[index] = static_cast<std::size_t>(point);
        averageDistance += info.distances[points[index]];
        averageField = add(averageField, info.normals[points[index]]);
        if (blendAlpha) averageAlpha += info.alphas[points[index]];
    }
    averageDistance /= faceCount;
    averageField = multiply(averageField, 1.0 / faceCount);
    averageAlpha /= faceCount;

    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        DisplacementInfo& info = *surfaces[index]->info;
        info.distances[points[index]] = averageDistance;
        info.normals[points[index]] = averageField;
        if (blendAlpha) info.alphas[points[index]] = averageAlpha;
    }
}

} // namespace

bool sewDisplacements(std::vector<SewSurface>& surfaces)
{
    if (surfaces.size() < 2) return false;
    for (const SewSurface& surface : surfaces) {
        if (!surface.info || !surface.info->valid()) return false;
    }

    bool changed = false;

    // SewCorner_Build / SewCorner_Resolve: cluster every base-surface corner and
    // average the field data of the displacements that meet there.
    struct CornerEntry
    {
        std::size_t surface;
        int corner;
        Vec3 point;
        bool used{false};
    };
    std::vector<CornerEntry> entries;
    entries.reserve(surfaces.size() * 4);
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        for (int corner = 0; corner < 4; ++corner) {
            entries.push_back({index, corner, surfaces[index].corners[static_cast<std::size_t>(corner)]});
        }
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].used) continue;
        std::vector<SewSurface*> cluster{&surfaces[entries[index].surface]};
        std::vector<int> clusterCorners{entries[index].corner};
        entries[index].used = true;
        for (std::size_t other = index + 1; other < entries.size(); ++other) {
            if (entries[other].used) continue;
            if (entries[other].surface == entries[index].surface) continue;
            if (!pointCompareWithTolerance(entries[index].point, entries[other].point,
                                           SewPointTolerance)) {
                continue;
            }
            entries[other].used = true;
            cluster.push_back(&surfaces[entries[other].surface]);
            clusterCorners.push_back(entries[other].corner);
        }
        if (cluster.size() > 1) {
            resolveCornerDisp(cluster, clusterCorners);
            changed = true;
        }
    }

    // SewEdge_Build / SewEdge_Resolve, normal (non T-junction) displacement
    // pairs only.
    for (std::size_t first = 0; first < surfaces.size(); ++first) {
        for (std::size_t second = first + 1; second < surfaces.size(); ++second) {
            for (int edgeFirst = 0; edgeFirst < 4; ++edgeFirst) {
                const Vec3& a0 = surfaces[first].corners[static_cast<std::size_t>(edgeFirst)];
                const Vec3& a1 = surfaces[first].corners[static_cast<std::size_t>((edgeFirst + 1) % 4)];
                for (int edgeSecond = 0; edgeSecond < 4; ++edgeSecond) {
                    const Vec3& b0 = surfaces[second].corners[static_cast<std::size_t>(edgeSecond)];
                    const Vec3& b1 =
                        surfaces[second].corners[static_cast<std::size_t>((edgeSecond + 1) % 4)];
                    if (!edgesAreNormalMatch(a0, a1, b0, b1)) continue;
                    resolveEdgeDispNormal(surfaces[first], edgeFirst, surfaces[second], edgeSecond);
                    changed = true;
                }
            }
        }
    }

    return changed;
}

} // namespace hammer::vmf
