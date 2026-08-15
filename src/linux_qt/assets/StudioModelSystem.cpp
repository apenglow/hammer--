#include "StudioModelSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <charconv>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_set>

namespace hammer::assets {

struct StudioAnimationData
{
    struct Bone
    {
        std::string name;
        int parent{-1};
        std::array<float, 3> basePosition{};
        std::array<float, 4> baseQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 3> baseRotation{};
        std::array<float, 3> positionScale{};
        std::array<float, 3> rotationScale{};
        std::array<float, 4> alignment{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 12> poseToBone{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f};
        int flags{0};
        bool hasPoseToBone{false};
    };

    // Source's $includemodel mechanism appends sequences from animation-only
    // MDLs to the mesh MDL. Each source keeps its own compressed channels,
    // animation blocks, and local bone table, while boneToRoot maps those local
    // channels onto the render model's skeleton by bone name.
    struct Source
    {
        std::string path;
        std::shared_ptr<const std::vector<std::uint8_t>> mdl;
        std::shared_ptr<const std::vector<std::uint8_t>> ani;
        std::vector<Bone> bones;
        std::vector<int> boneToRoot;
        int localSequenceCount{0};
        int localSequenceIndex{0};
        int localAnimationCount{0};
        int localAnimationIndex{0};
        int animationBlockCount{0};
        int animationBlockIndex{0};
        // Equivalent to virtualgroup_t::masterAnim/masterSeq: local sequence
        // descriptors and animation numbers resolve through the shared virtual
        // model tables rather than being appended blindly.
        std::vector<int> masterAnimations;
        std::vector<int> masterSequences;
    };

    struct AnimationBinding
    {
        std::size_t sourceIndex{0};
        int localAnimation{0};
    };

    struct SequenceBinding
    {
        std::size_t sourceIndex{0};
        int localSequence{0};
    };

    std::vector<Bone> bones; // master/render-model skeleton
    std::vector<Source> sources;
    std::vector<AnimationBinding> animations;
    std::vector<SequenceBinding> sequences;
};

namespace {

constexpr std::size_t StudioHeaderBoneCount = 156;
constexpr std::size_t StudioHeaderBoneIndex = 160;
constexpr std::size_t StudioHeaderLocalAnimationCount = 180;
constexpr std::size_t StudioHeaderLocalAnimationIndex = 184;
constexpr std::size_t StudioHeaderLocalSequenceCount = 188;
constexpr std::size_t StudioHeaderLocalSequenceIndex = 192;
constexpr std::size_t StudioHeaderIncludeModelCount = 336;
constexpr std::size_t StudioHeaderIncludeModelIndex = 340;
constexpr std::size_t StudioHeaderAnimationBlockNameIndex = 348;
constexpr std::size_t StudioHeaderAnimationBlockCount = 352;
constexpr std::size_t StudioHeaderAnimationBlockIndex = 356;
constexpr std::size_t StudioHeader2Index = 400;
constexpr std::size_t StudioHeader2LinearBoneIndex = 16;
constexpr std::size_t StudioHeaderBodyPartCount = 232;
constexpr std::size_t StudioHeaderBodyPartIndex = 236;
constexpr std::size_t StudioHeaderTextureCount = 204;
constexpr std::size_t StudioHeaderTextureIndex = 208;
constexpr std::size_t StudioHeaderCdTextureCount = 212;
constexpr std::size_t StudioHeaderCdTextureIndex = 216;
constexpr std::size_t StudioHeaderSkinReferenceCount = 220;
constexpr std::size_t StudioHeaderSkinFamilyCount = 224;
constexpr std::size_t StudioHeaderSkinIndex = 228;
constexpr std::size_t StudioModelGroupSize = 8;
constexpr std::size_t StudioModelGroupNameIndex = 4;
constexpr std::size_t StudioBoneSize = 216;
constexpr std::size_t StudioBoneNameIndex = 0;
constexpr std::size_t StudioBonePosition = 32;
constexpr std::size_t StudioBoneQuaternion = 44;
constexpr std::size_t StudioBoneRotation = 60;
constexpr std::size_t StudioBonePositionScale = 72;
constexpr std::size_t StudioBoneRotationScale = 84;
constexpr std::size_t StudioBonePoseToBone = 96;
constexpr std::size_t StudioBoneAlignment = 144;
constexpr std::size_t StudioBoneFlags = 160;
constexpr std::size_t StudioLinearBoneSize = 64;
constexpr std::size_t StudioLinearBoneFlagsIndex = 4;
constexpr std::size_t StudioLinearBoneParentIndex = 8;
constexpr std::size_t StudioLinearBonePositionIndex = 12;
constexpr std::size_t StudioLinearBoneQuaternionIndex = 16;
constexpr std::size_t StudioLinearBoneRotationIndex = 20;
constexpr std::size_t StudioLinearBonePoseToBoneIndex = 24;
constexpr std::size_t StudioLinearBonePositionScaleIndex = 28;
constexpr std::size_t StudioLinearBoneRotationScaleIndex = 32;
constexpr std::size_t StudioLinearBoneAlignmentIndex = 36;
constexpr std::size_t StudioAnimationDescriptionSize = 100;
constexpr std::size_t StudioAnimationNameIndex = 4;
constexpr std::size_t StudioSequenceDescriptionSize = 212;
constexpr std::size_t StudioSequenceLabelIndex = 4;
constexpr std::size_t StudioSequenceActivityNameIndex = 8;
constexpr std::size_t StudioSequenceFlags = 12;
constexpr std::size_t StudioSequenceMinimum = 32;
constexpr std::size_t StudioSequenceMaximum = 44;
constexpr std::size_t StudioSequenceAnimationIndex = 60;
constexpr std::size_t StudioSequenceGroupSize = 68;
constexpr std::size_t StudioSequenceParameterStart = 84;
constexpr std::size_t StudioSequenceParameterEnd = 92;
constexpr std::size_t StudioSequenceWeightListIndex = 156;
constexpr std::size_t StudioAnimationFps = 8;
constexpr std::size_t StudioAnimationFlags = 12;
constexpr std::size_t StudioAnimationFrameCount = 16;
constexpr std::size_t StudioAnimationBlock = 52;
constexpr std::size_t StudioAnimationDataIndex = 56;
constexpr std::size_t StudioAnimationSectionIndex = 80;
constexpr std::size_t StudioAnimationSectionFrames = 84;
constexpr std::size_t StudioBodyPartSize = 16;
constexpr std::size_t StudioModelSize = 148;
constexpr std::size_t StudioMeshSize = 116;
constexpr std::size_t StudioTextureSize = 64;
constexpr std::size_t StudioVertexSize = 48;
constexpr std::size_t VvdFixupCount = 48;
constexpr std::size_t VvdFixupTableStart = 52;
// vertexFileHeader_t stores vertexDataStart at byte 56. Byte 60 is the
// tangent-data offset. The v0.11.7 reader used byte 60, so real Source models
// usually read tangent bytes as vertices and produced no renderable props.
constexpr std::size_t VvdVertexDataStart = 56;
constexpr std::size_t VvdTangentDataStart = 60;
constexpr std::size_t VtxBodyPartOffset = 32;
constexpr std::size_t VtxBodyPartSize = 8;
constexpr std::size_t VtxModelSize = 8;
constexpr std::size_t VtxLodSize = 12;
constexpr std::size_t VtxMeshSize = 9;
constexpr std::size_t VtxStripGroupSize = 25;
constexpr std::size_t VtxVertexSize = 9;
constexpr std::size_t VtxStripSize = 27;

bool range(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t size)
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

std::int32_t i32(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t fallback = 0)
{
    if (!range(bytes, offset, 4)) return fallback;
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    return static_cast<std::int32_t>(value);
}

std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (!range(bytes, offset, 2)) return 0;
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::int16_t i16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(u16(bytes, offset));
}

std::uint64_t u64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (!range(bytes, offset, 8)) return 0;
    std::uint64_t value = 0;
    for (int byte = 0; byte < 8; ++byte)
        value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(byte)]) << (byte * 8);
    return value;
}

float f32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    const std::uint32_t bits = static_cast<std::uint32_t>(i32(bytes, offset));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) ? value : 0.0f;
}

std::string cstring(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                    std::size_t maximum = 4096)
{
    if (offset >= bytes.size()) return {};
    const std::size_t endLimit = std::min(bytes.size(), offset + maximum);
    std::size_t end = offset;
    while (end < endLimit && bytes[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), end - offset);
}

std::string normalized(std::string_view input)
{
    std::string path = GameFileSystem::normalizeResourcePath(input);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.rfind("models/", 0) != 0) path.insert(0, "models/");
    if (path.size() < 4 || path.substr(path.size() - 4) != ".mdl") path += ".mdl";
    return path;
}

std::string withoutExtension(std::string path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) path.resize(dot);
    return path;
}

std::string materialName(std::string path)
{
    path = GameFileSystem::normalizeResourcePath(path);
    if (path.rfind("materials/", 0) == 0) path.erase(0, 10);
    if (path.size() >= 4 && (path.substr(path.size() - 4) == ".vmt" ||
                             path.substr(path.size() - 4) == ".vtf")) {
        path.resize(path.size() - 4);
    }
    return path;
}

std::string joinMaterial(std::string directory, std::string texture)
{
    directory = materialName(directory);
    texture = materialName(texture);
    if (!directory.empty() && directory.back() != '/') directory.push_back('/');
    return materialName(directory + texture);
}

bool signature(const std::vector<std::uint8_t>& bytes, const char* expected)
{
    return range(bytes, 0, 4) && bytes[0] == static_cast<std::uint8_t>(expected[0]) &&
           bytes[1] == static_cast<std::uint8_t>(expected[1]) &&
           bytes[2] == static_cast<std::uint8_t>(expected[2]) &&
           bytes[3] == static_cast<std::uint8_t>(expected[3]);
}

struct Matrix3x4
{
    float m[3][4]{
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f}
    };
};

Matrix3x4 quaternionMatrix(float x, float y, float z, float w,
                           float px, float py, float pz)
{
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 1e-8f) {
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    } else {
        x = y = z = 0.0f;
        w = 1.0f;
    }

    Matrix3x4 result;
    result.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    result.m[0][1] = 2.0f * (x * y - z * w);
    result.m[0][2] = 2.0f * (x * z + y * w);
    result.m[1][0] = 2.0f * (x * y + z * w);
    result.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    result.m[1][2] = 2.0f * (y * z - x * w);
    result.m[2][0] = 2.0f * (x * z - y * w);
    result.m[2][1] = 2.0f * (y * z + x * w);
    result.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    result.m[0][3] = px;
    result.m[1][3] = py;
    result.m[2][3] = pz;
    return result;
}

Matrix3x4 concatenate(const Matrix3x4& parent, const Matrix3x4& local)
{
    Matrix3x4 result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = parent.m[row][0] * local.m[0][column] +
                                    parent.m[row][1] * local.m[1][column] +
                                    parent.m[row][2] * local.m[2][column];
        }
        result.m[row][3] = parent.m[row][0] * local.m[0][3] +
                           parent.m[row][1] * local.m[1][3] +
                           parent.m[row][2] * local.m[2][3] +
                           parent.m[row][3];
    }
    return result;
}

Matrix3x4 readMatrix3x4(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    Matrix3x4 result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.m[row][column] = f32(bytes, offset +
                static_cast<std::size_t>(row * 4 + column) * sizeof(float));
        }
    }
    return result;
}

bool usableAffineMatrix(const Matrix3x4& matrix)
{
    const float determinant =
        matrix.m[0][0] * (matrix.m[1][1] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][1]) -
        matrix.m[0][1] * (matrix.m[1][0] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][0]) +
        matrix.m[0][2] * (matrix.m[1][0] * matrix.m[2][1] - matrix.m[1][1] * matrix.m[2][0]);
    return std::isfinite(determinant) && std::abs(determinant) > 1e-6f;
}

struct QuaternionValue
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
};

float halfFloat(std::uint16_t value)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1fu;
    std::uint32_t mantissa = value & 0x03ffu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float output = 0.0f;
    std::memcpy(&output, &bits, sizeof(output));
    return std::isfinite(output) ? output : 0.0f;
}

QuaternionValue normalizeQuaternion(QuaternionValue quaternion)
{
    const float length = std::sqrt(quaternion.x * quaternion.x + quaternion.y * quaternion.y +
                                   quaternion.z * quaternion.z + quaternion.w * quaternion.w);
    if (length <= 1e-8f) return {};
    quaternion.x /= length;
    quaternion.y /= length;
    quaternion.z /= length;
    quaternion.w /= length;
    return quaternion;
}

QuaternionValue quaternion48(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (!range(bytes, offset, 6)) return {};
    const std::uint16_t encodedX = u16(bytes, offset);
    const std::uint16_t encodedY = u16(bytes, offset + 2);
    const std::uint16_t encodedZW = u16(bytes, offset + 4);
    QuaternionValue output;
    output.x = (static_cast<int>(encodedX) - 32768) / 32768.0f;
    output.y = (static_cast<int>(encodedY) - 32768) / 32768.0f;
    output.z = (static_cast<int>(encodedZW & 0x7fffu) - 16384) / 16384.0f;
    const float remaining = std::max(0.0f, 1.0f - output.x * output.x -
                                     output.y * output.y - output.z * output.z);
    output.w = std::sqrt(remaining);
    if ((encodedZW & 0x8000u) != 0) output.w = -output.w;
    return normalizeQuaternion(output);
}

QuaternionValue quaternion64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    const std::uint64_t encoded = u64(bytes, offset);
    constexpr std::uint64_t Mask = (1ull << 21) - 1ull;
    QuaternionValue output;
    output.x = (static_cast<int>(encoded & Mask) - 1048576) / 1048576.5f;
    output.y = (static_cast<int>((encoded >> 21) & Mask) - 1048576) / 1048576.5f;
    output.z = (static_cast<int>((encoded >> 42) & Mask) - 1048576) / 1048576.5f;
    const float remaining = std::max(0.0f, 1.0f - output.x * output.x -
                                     output.y * output.y - output.z * output.z);
    output.w = std::sqrt(remaining);
    if ((encoded >> 63) != 0) output.w = -output.w;
    return normalizeQuaternion(output);
}

QuaternionValue radianEulerQuaternion(const std::array<float, 3>& angles)
{
    const float sx = std::sin(angles[0] * 0.5f);
    const float cx = std::cos(angles[0] * 0.5f);
    const float sy = std::sin(angles[1] * 0.5f);
    const float cy = std::cos(angles[1] * 0.5f);
    const float sz = std::sin(angles[2] * 0.5f);
    const float cz = std::cos(angles[2] * 0.5f);
    return normalizeQuaternion({sx * cy * cz - cx * sy * sz,
                                cx * sy * cz + sx * cy * sz,
                                cx * cy * sz - sx * sy * cz,
                                cx * cy * cz + sx * sy * sz});
}

float frameZeroAnimationValue(const std::vector<std::uint8_t>& bytes,
                              std::size_t pointer, int axis, float scale)
{
    if (!range(bytes, pointer, 6) || axis < 0 || axis >= 3) return 0.0f;
    const std::int16_t relative = i16(bytes, pointer + static_cast<std::size_t>(axis) * 2u);
    if (relative <= 0) return 0.0f;
    const std::size_t stream = pointer + static_cast<std::size_t>(relative);
    if (!range(bytes, stream, 4)) return 0.0f;
    const std::uint8_t valid = bytes[stream];
    const std::uint8_t total = bytes[stream + 1];
    if (total == 0 || valid == 0) return 0.0f;
    return static_cast<float>(i16(bytes, stream + 2)) * scale;
}

QuaternionValue quaternionMultiply(const QuaternionValue& left,
                                   const QuaternionValue& right)
{
    return normalizeQuaternion({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z});
}

QuaternionValue quaternionSlerp(QuaternionValue first, QuaternionValue second, float amount)
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    float dot = first.x * second.x + first.y * second.y + first.z * second.z + first.w * second.w;
    if (dot < 0.0f) {
        second.x = -second.x;
        second.y = -second.y;
        second.z = -second.z;
        second.w = -second.w;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        return normalizeQuaternion({first.x + (second.x - first.x) * amount,
                                    first.y + (second.y - first.y) * amount,
                                    first.z + (second.z - first.z) * amount,
                                    first.w + (second.w - first.w) * amount});
    }
    const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sine = std::sin(angle);
    if (std::abs(sine) < 1e-7f) return first;
    const float firstWeight = std::sin((1.0f - amount) * angle) / sine;
    const float secondWeight = std::sin(amount * angle) / sine;
    return normalizeQuaternion({first.x * firstWeight + second.x * secondWeight,
                                first.y * firstWeight + second.y * secondWeight,
                                first.z * firstWeight + second.z * secondWeight,
                                first.w * firstWeight + second.w * secondWeight});
}

Matrix3x4 matrixFromArray(const std::array<float, 12>& values)
{
    Matrix3x4 matrix;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            matrix.m[row][column] = values[static_cast<std::size_t>(row * 4 + column)];
    return matrix;
}

std::array<float, 12> matrixToArray(const Matrix3x4& matrix)
{
    std::array<float, 12> values{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            values[static_cast<std::size_t>(row * 4 + column)] = matrix.m[row][column];
    return values;
}

QuaternionValue quaternionFromArray(const std::array<float, 4>& value)
{
    return normalizeQuaternion({value[0], value[1], value[2], value[3]});
}

std::array<float, 4> quaternionToArray(const QuaternionValue& value)
{
    return {value.x, value.y, value.z, value.w};
}

std::string lowerAscii(std::string_view value)
{
    std::string output(value);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return output;
}

std::vector<std::string> includedModelPaths(const std::vector<std::uint8_t>& mdl)
{
    std::vector<std::string> paths;
    const int count = std::clamp(i32(mdl, StudioHeaderIncludeModelCount), 0, 256);
    const int index = i32(mdl, StudioHeaderIncludeModelIndex);
    if (count <= 0 || index <= 0) return paths;
    paths.reserve(static_cast<std::size_t>(count));
    for (int include = 0; include < count; ++include) {
        const std::size_t entry = static_cast<std::size_t>(index) +
            static_cast<std::size_t>(include) * StudioModelGroupSize;
        if (!range(mdl, entry, StudioModelGroupSize)) break;
        const int nameRelative = i32(mdl, entry + StudioModelGroupNameIndex);
        if (nameRelative <= 0) continue;
        const std::string name = cstring(mdl, entry + static_cast<std::size_t>(nameRelative));
        if (!name.empty()) paths.push_back(normalized(name));
    }
    return paths;
}

StudioAnimationData::Source makeAnimationSource(
    std::string path,
    std::shared_ptr<const std::vector<std::uint8_t>> mdl,
    const std::shared_ptr<GameFileSystem>& fileSystem,
    const std::vector<StudioAnimationData::Bone>& rootBones)
{
    StudioAnimationData::Source source;
    source.path = std::move(path);
    source.mdl = std::move(mdl);
    if (!source.mdl || !signature(*source.mdl, "IDST")) return source;
    const auto& bytes = *source.mdl;
    source.localSequenceCount = std::clamp(i32(bytes, StudioHeaderLocalSequenceCount), 0, 4096);
    source.localSequenceIndex = i32(bytes, StudioHeaderLocalSequenceIndex);
    source.localAnimationCount = std::clamp(i32(bytes, StudioHeaderLocalAnimationCount), 0, 4096);
    source.localAnimationIndex = i32(bytes, StudioHeaderLocalAnimationIndex);
    source.animationBlockCount = std::clamp(i32(bytes, StudioHeaderAnimationBlockCount), 0, 65536);
    source.animationBlockIndex = i32(bytes, StudioHeaderAnimationBlockIndex);

    const int boneCount = std::clamp(i32(bytes, StudioHeaderBoneCount), 0, 256);
    const int boneIndex = i32(bytes, StudioHeaderBoneIndex);
    source.bones.resize(static_cast<std::size_t>(boneCount));
    auto vector3 = [&](std::size_t offset) {
        return std::array<float, 3>{f32(bytes, offset), f32(bytes, offset + 4), f32(bytes, offset + 8)};
    };
    auto quaternion = [&](std::size_t offset) {
        return quaternionToArray(normalizeQuaternion({f32(bytes, offset), f32(bytes, offset + 4),
                                                       f32(bytes, offset + 8), f32(bytes, offset + 12)}));
    };

    bool loadedLinear = false;
    const int header2Index = i32(bytes, StudioHeader2Index);
    if (boneCount > 0 && header2Index > 0 &&
        range(bytes, static_cast<std::size_t>(header2Index),
              StudioHeader2LinearBoneIndex + sizeof(std::int32_t))) {
        const std::size_t header2 = static_cast<std::size_t>(header2Index);
        const int linearRelative = i32(bytes, header2 + StudioHeader2LinearBoneIndex);
        if (linearRelative > 0) {
            const std::size_t linear = header2 + static_cast<std::size_t>(linearRelative);
            if (range(bytes, linear, StudioLinearBoneSize) && i32(bytes, linear) == boneCount) {
                const int flagsRelative = i32(bytes, linear + StudioLinearBoneFlagsIndex);
                const int parentRelative = i32(bytes, linear + StudioLinearBoneParentIndex);
                const int positionRelative = i32(bytes, linear + StudioLinearBonePositionIndex);
                const int quaternionRelative = i32(bytes, linear + StudioLinearBoneQuaternionIndex);
                const int rotationRelative = i32(bytes, linear + StudioLinearBoneRotationIndex);
                const int positionScaleRelative = i32(bytes, linear + StudioLinearBonePositionScaleIndex);
                const int rotationScaleRelative = i32(bytes, linear + StudioLinearBoneRotationScaleIndex);
                const int alignmentRelative = i32(bytes, linear + StudioLinearBoneAlignmentIndex);
                const auto validArray = [&](int relative, std::size_t stride) {
                    return relative > 0 && range(bytes, linear + static_cast<std::size_t>(relative),
                        static_cast<std::size_t>(boneCount) * stride);
                };
                if (validArray(parentRelative, sizeof(std::int32_t)) &&
                    validArray(positionRelative, 3u * sizeof(float)) &&
                    validArray(quaternionRelative, 4u * sizeof(float))) {
                    const bool hasFlags = validArray(flagsRelative, sizeof(std::int32_t));
                    const bool hasRotation = validArray(rotationRelative, 3u * sizeof(float));
                    const bool hasPositionScale = validArray(positionScaleRelative, 3u * sizeof(float));
                    const bool hasRotationScale = validArray(rotationScaleRelative, 3u * sizeof(float));
                    const bool hasAlignment = validArray(alignmentRelative, 4u * sizeof(float));
                    for (int index = 0; index < boneCount; ++index) {
                        auto& bone = source.bones[static_cast<std::size_t>(index)];
                        if (boneIndex > 0) {
                            const std::size_t legacy = static_cast<std::size_t>(boneIndex) +
                                static_cast<std::size_t>(index) * StudioBoneSize;
                            if (range(bytes, legacy, StudioBoneSize)) {
                                const int relative = i32(bytes, legacy + StudioBoneNameIndex);
                                if (relative > 0) bone.name = cstring(bytes, legacy + static_cast<std::size_t>(relative));
                            }
                        }
                        bone.parent = i32(bytes, linear + static_cast<std::size_t>(parentRelative) +
                            static_cast<std::size_t>(index) * sizeof(std::int32_t));
                        bone.basePosition = vector3(linear + static_cast<std::size_t>(positionRelative) +
                            static_cast<std::size_t>(index) * 3u * sizeof(float));
                        bone.baseQuaternion = quaternion(linear + static_cast<std::size_t>(quaternionRelative) +
                            static_cast<std::size_t>(index) * 4u * sizeof(float));
                        if (hasRotation) bone.baseRotation = vector3(linear + static_cast<std::size_t>(rotationRelative) +
                            static_cast<std::size_t>(index) * 3u * sizeof(float));
                        if (hasPositionScale) bone.positionScale = vector3(linear + static_cast<std::size_t>(positionScaleRelative) +
                            static_cast<std::size_t>(index) * 3u * sizeof(float));
                        if (hasRotationScale) bone.rotationScale = vector3(linear + static_cast<std::size_t>(rotationScaleRelative) +
                            static_cast<std::size_t>(index) * 3u * sizeof(float));
                        if (hasAlignment) bone.alignment = quaternion(linear + static_cast<std::size_t>(alignmentRelative) +
                            static_cast<std::size_t>(index) * 4u * sizeof(float));
                        if (hasFlags) bone.flags = i32(bytes, linear + static_cast<std::size_t>(flagsRelative) +
                            static_cast<std::size_t>(index) * sizeof(std::int32_t));
                    }
                    loadedLinear = true;
                }
            }
        }
    }

    if (!loadedLinear && boneCount > 0 && boneIndex > 0 &&
        range(bytes, static_cast<std::size_t>(boneIndex),
              static_cast<std::size_t>(boneCount) * StudioBoneSize)) {
        for (int index = 0; index < boneCount; ++index) {
            const std::size_t base = static_cast<std::size_t>(boneIndex) +
                static_cast<std::size_t>(index) * StudioBoneSize;
            auto& bone = source.bones[static_cast<std::size_t>(index)];
            const int nameRelative = i32(bytes, base + StudioBoneNameIndex);
            if (nameRelative > 0) bone.name = cstring(bytes, base + static_cast<std::size_t>(nameRelative));
            bone.parent = i32(bytes, base + 4);
            bone.basePosition = vector3(base + StudioBonePosition);
            bone.baseQuaternion = quaternion(base + StudioBoneQuaternion);
            bone.baseRotation = vector3(base + StudioBoneRotation);
            bone.positionScale = vector3(base + StudioBonePositionScale);
            bone.rotationScale = vector3(base + StudioBoneRotationScale);
            bone.alignment = quaternion(base + StudioBoneAlignment);
            bone.flags = i32(bytes, base + StudioBoneFlags);
        }
    }

    std::unordered_map<std::string, int> rootByName;
    for (int index = 0; index < static_cast<int>(rootBones.size()); ++index) {
        if (!rootBones[static_cast<std::size_t>(index)].name.empty())
            rootByName.emplace(lowerAscii(rootBones[static_cast<std::size_t>(index)].name), index);
    }
    const bool sourceHasBoneNames = std::any_of(source.bones.begin(), source.bones.end(),
        [](const StudioAnimationData::Bone& bone) { return !bone.name.empty(); });
    const bool rootHasBoneNames = !rootByName.empty();

    // Source's virtualgroup_t carries a per-include masterBone mapping, so an
    // animation group's local skeleton is not required to have exactly the
    // same number of bones as the render model. TF2's human player models add
    // class/model-specific bones after the shared animation skeleton, while
    // several animation-only MDLs omit their bone names. Robot player models
    // commonly mask this because their render and animation skeleton sizes
    // happen to match.
    //
    // We cannot read the runtime-only masterBone vector directly from disk, so
    // use authored local indices only when names are unavailable and the
    // smaller skeleton is a hierarchy-compatible prefix of the render one.
    // This preserves the old equal-size fallback while avoiding an unsafe map
    // between unrelated differently-sized skeletons.
    bool hierarchyCompatiblePrefix = source.bones.size() <= rootBones.size();
    if (hierarchyCompatiblePrefix) {
        for (std::size_t index = 0; index < source.bones.size(); ++index) {
            const int sourceParent = source.bones[index].parent;
            const int rootParent = rootBones[index].parent;
            if (sourceParent != rootParent) {
                hierarchyCompatiblePrefix = false;
                break;
            }
        }
    }
    const bool missingNames = !sourceHasBoneNames || !rootHasBoneNames;
    const bool equalSizeIndexFallback = source.bones.size() == rootBones.size() && missingNames;
    const bool sharedPrefixIndexFallback = source.bones.size() < rootBones.size() &&
                                           !sourceHasBoneNames &&
                                           hierarchyCompatiblePrefix;
    const bool useIndexFallback = equalSizeIndexFallback || sharedPrefixIndexFallback;
    source.boneToRoot.assign(source.bones.size(), -1);
    for (int index = 0; index < static_cast<int>(source.bones.size()); ++index) {
        const auto& bone = source.bones[static_cast<std::size_t>(index)];
        if (!bone.name.empty()) {
            if (const auto found = rootByName.find(lowerAscii(bone.name)); found != rootByName.end())
                source.boneToRoot[static_cast<std::size_t>(index)] = found->second;
        }
        if (source.boneToRoot[static_cast<std::size_t>(index)] < 0 && useIndexFallback) {
            // Some animation-only compilers omit all bone names. Source maps
            // these local bones through virtualgroup_t::masterBone; the
            // hierarchy-compatible prefix above is the disk-only equivalent
            // for shared player skeletons with extra render-model bones.
            source.boneToRoot[static_cast<std::size_t>(index)] = index;
        }
    }

    if (source.animationBlockCount > 1 && fileSystem) {
        std::string authoredBlockName;
        const int nameIndex = i32(bytes, StudioHeaderAnimationBlockNameIndex);
        if (nameIndex > 0) authoredBlockName = cstring(bytes, static_cast<std::size_t>(nameIndex));

        // studiomdl output is not consistent about this string. Some games
        // store "models/player/scout_animations.ani", while TF2 class animation
        // MDLs commonly store only a basename. Resolve both the authored path
        // and a path relative to the MDL that owns the animation blocks.
        std::vector<std::string> candidates;
        auto addCandidate = [&](std::string candidate) {
            candidate = GameFileSystem::normalizeResourcePath(candidate);
            while (!candidate.empty() && candidate.front() == '/') candidate.erase(candidate.begin());
            if (candidate.empty()) return;
            if (candidate.rfind("models/", 0) != 0) candidate.insert(0, "models/");
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(std::move(candidate));
        };

        if (!authoredBlockName.empty()) {
            addCandidate(authoredBlockName);
            std::string sourceDirectory = source.path;
            const std::size_t slash = sourceDirectory.find_last_of('/');
            sourceDirectory = slash == std::string::npos ? std::string{} : sourceDirectory.substr(0, slash + 1);
            std::string relativeName = GameFileSystem::normalizeResourcePath(authoredBlockName);
            const std::size_t authoredSlash = relativeName.find_last_of('/');
            if (authoredSlash != std::string::npos) relativeName.erase(0, authoredSlash + 1);
            addCandidate(sourceDirectory + relativeName);
        }
        addCandidate(withoutExtension(source.path) + ".ani");

        for (const std::string& candidate : candidates) {
            if (auto ani = fileSystem->readFile(candidate)) {
                source.ani = std::make_shared<const std::vector<std::uint8_t>>(std::move(*ani));
                break;
            }
        }
    }
    return source;
}

bool extractAnimationValue(const std::vector<std::uint8_t>& bytes,
                           std::size_t stream, int frame, float scale,
                           float& first, float& second)
{
    first = second = 0.0f;
    if (frame < 0 || !range(bytes, stream, 2)) return false;
    int remaining = frame;
    for (int block = 0; block < 65536 && range(bytes, stream, 2); ++block) {
        const int valid = bytes[stream];
        const int total = bytes[stream + 1];
        if (total <= 0 || valid > total) return false;
        const std::size_t blockBytes = static_cast<std::size_t>(valid + 1) * 2u;
        if (!range(bytes, stream, blockBytes)) return false;
        if (remaining >= total) {
            remaining -= total;
            stream += blockBytes;
            continue;
        }

        auto valueAt = [&](int index, float fallback) {
            if (index <= 0 || !range(bytes, stream + static_cast<std::size_t>(index) * 2u, 2))
                return fallback;
            return static_cast<float>(i16(bytes, stream + static_cast<std::size_t>(index) * 2u)) * scale;
        };

        // Valve special-cases a one-frame constant block so interpolation does
        // not read the first value of whatever record happens to follow it.
        if (valid == 1 && total == 1) {
            first = second = valueAt(1, 0.0f);
            return true;
        }
        if (valid == 0) return true;
        if (valid > remaining) {
            first = valueAt(remaining + 1, 0.0f);
            if (valid > remaining + 1) {
                second = valueAt(remaining + 2, first);
            } else if (total > remaining + 1) {
                second = first;
            } else {
                second = valueAt(valid + 2, first);
            }
        } else {
            first = valueAt(valid, 0.0f);
            second = total > remaining + 1 ? first : valueAt(valid + 2, first);
        }
        return true;
    }
    return false;
}

void extractAnimationAxis(const std::vector<std::uint8_t>& bytes,
                          std::size_t valuePointers, int axis, int frame,
                          float scale, float& first, float& second)
{
    first = second = 0.0f;
    if (axis < 0 || axis >= 3 || !range(bytes, valuePointers, 6)) return;
    const std::int16_t relative = i16(bytes, valuePointers + static_cast<std::size_t>(axis) * 2u);
    if (relative <= 0) return;
    extractAnimationValue(bytes, valuePointers + static_cast<std::size_t>(relative),
                          frame, scale, first, second);
}

struct AnimationChunk
{
    const std::vector<std::uint8_t>* bytes{nullptr};
    std::size_t offset{0};
    int localFrame{0};
};

AnimationChunk animationChunk(const StudioAnimationData::Source& source,
                              std::size_t animationDescription, int frame)
{
    AnimationChunk chunk;
    if (!source.mdl || !range(*source.mdl, animationDescription, StudioAnimationDescriptionSize))
        return chunk;
    const auto& mdl = *source.mdl;
    int block = i32(mdl, animationDescription + StudioAnimationBlock);
    int index = i32(mdl, animationDescription + StudioAnimationDataIndex);
    int localFrame = std::max(0, frame);
    const int frameCount = std::max(1, i32(mdl, animationDescription + StudioAnimationFrameCount));
    const int sectionFrames = i32(mdl, animationDescription + StudioAnimationSectionFrames);
    if (sectionFrames > 0) {
        int section = 0;
        if (frameCount > sectionFrames && localFrame == frameCount - 1) {
            localFrame = 0;
            section = frameCount / sectionFrames + 1;
        } else {
            section = localFrame / sectionFrames;
            localFrame -= section * sectionFrames;
        }
        const int sectionRelative = i32(mdl, animationDescription + StudioAnimationSectionIndex);
        const std::size_t sectionEntry = sectionRelative > 0
            ? animationDescription + static_cast<std::size_t>(sectionRelative) +
                static_cast<std::size_t>(section) * 8u
            : 0u;
        if (sectionRelative <= 0 || !range(mdl, sectionEntry, 8)) return chunk;
        block = i32(mdl, sectionEntry);
        index = i32(mdl, sectionEntry + 4);
    }
    if (block < 0) return chunk;
    if (block == 0) {
        // Inline animindex is relative to mstudioanimdesc_t and zero means no
        // data. External animation-block offsets, however, are allowed to be
        // exactly zero and are handled below.
        if (index <= 0) return chunk;
        const std::size_t offset = animationDescription + static_cast<std::size_t>(index);
        if (!range(mdl, offset, 4)) return chunk;
        chunk.bytes = &mdl;
        chunk.offset = offset;
        chunk.localFrame = localFrame;
        return chunk;
    }
    if (index < 0 || !source.ani || block >= source.animationBlockCount ||
        source.animationBlockIndex <= 0)
        return chunk;
    const std::size_t blockEntry = static_cast<std::size_t>(source.animationBlockIndex) +
                                   static_cast<std::size_t>(block) * 8u;
    if (!range(mdl, blockEntry, 8)) return chunk;
    const int dataStart = i32(mdl, blockEntry);
    if (dataStart < 0) return chunk;
    const std::size_t offset = static_cast<std::size_t>(dataStart) + static_cast<std::size_t>(index);
    if (!range(*source.ani, offset, 4)) return chunk;
    chunk.bytes = source.ani.get();
    chunk.offset = offset;
    chunk.localFrame = localFrame;
    return chunk;
}

const StudioAnimationData::AnimationBinding* resolveAnimationBinding(
    const StudioAnimationData& data, std::size_t sequenceSourceIndex, int localAnimation)
{
    if (sequenceSourceIndex >= data.sources.size() || localAnimation < 0) return nullptr;
    const auto& sequenceSource = data.sources[sequenceSourceIndex];
    if (localAnimation >= static_cast<int>(sequenceSource.masterAnimations.size())) return nullptr;
    const int globalAnimation = sequenceSource.masterAnimations[static_cast<std::size_t>(localAnimation)];
    if (globalAnimation < 0 || globalAnimation >= static_cast<int>(data.animations.size())) return nullptr;
    return &data.animations[static_cast<std::size_t>(globalAnimation)];
}

std::size_t animationDescriptionOffset(const StudioAnimationData::Source& source,
                                       int localAnimation)
{
    if (!source.mdl || source.localAnimationIndex <= 0 || localAnimation < 0 ||
        localAnimation >= source.localAnimationCount) return 0;
    const std::size_t offset = static_cast<std::size_t>(source.localAnimationIndex) +
        static_cast<std::size_t>(localAnimation) * StudioAnimationDescriptionSize;
    return range(*source.mdl, offset, StudioAnimationDescriptionSize) ? offset : 0;
}

bool evaluateSequence(const StudioAnimationData& data, int requestedSequence,
                      double requestedCycle, std::vector<Matrix3x4>& skinMatrices)
{
    if (data.bones.empty() || data.sources.empty() || data.sequences.empty()) return false;
    const int globalSequence = requestedSequence >= 0 &&
        requestedSequence < static_cast<int>(data.sequences.size()) ? requestedSequence : 0;
    const auto& binding = data.sequences[static_cast<std::size_t>(globalSequence)];
    if (binding.sourceIndex >= data.sources.size()) return false;
    const auto& sequenceSource = data.sources[binding.sourceIndex];
    if (!sequenceSource.mdl || sequenceSource.localSequenceCount <= 0 ||
        sequenceSource.localSequenceIndex <= 0 || binding.localSequence < 0 ||
        binding.localSequence >= sequenceSource.localSequenceCount) return false;
    const auto& mdl = *sequenceSource.mdl;
    const std::size_t sequence = static_cast<std::size_t>(sequenceSource.localSequenceIndex) +
        static_cast<std::size_t>(binding.localSequence) * StudioSequenceDescriptionSize;
    if (!range(mdl, sequence, StudioSequenceDescriptionSize)) return false;
    const int sequenceFlags = i32(mdl, sequence + StudioSequenceFlags);
    double cycle = std::isfinite(requestedCycle) ? requestedCycle : 0.0;
    if ((sequenceFlags & 0x0001) != 0) {
        cycle -= std::floor(cycle);
        if (cycle < 0.0) cycle += 1.0;
    } else {
        cycle = std::clamp(cycle, 0.0, 1.0);
    }

    const int groupX = std::clamp(i32(mdl, sequence + StudioSequenceGroupSize), 1, 256);
    const int groupY = std::clamp(i32(mdl, sequence + StudioSequenceGroupSize + 4), 1, 256);
    const int animationIndexRelative = i32(mdl, sequence + StudioSequenceAnimationIndex);
    const std::size_t blendTable = animationIndexRelative > 0
        ? sequence + static_cast<std::size_t>(animationIndexRelative) : 0u;
    if (animationIndexRelative <= 0 || !range(mdl, blendTable,
        static_cast<std::size_t>(groupX) * static_cast<std::size_t>(groupY) * 2u)) return false;

    // The editor has no gameplay pose-parameter state. Select the animation
    // nearest each sequence parameter's neutral value (zero) rather than the
    // first blend corner. This is crucial for player aim/movement matrices,
    // whose (0,0) entry may be a reference or extreme directional pose.
    auto neutralBlend = [&](int dimension, int size) {
        if (size <= 1) return 0;
        const float start = f32(mdl, sequence + StudioSequenceParameterStart +
            static_cast<std::size_t>(dimension) * sizeof(float));
        const float end = f32(mdl, sequence + StudioSequenceParameterEnd +
            static_cast<std::size_t>(dimension) * sizeof(float));
        float normalizedValue = 0.5f;
        if (std::isfinite(start) && std::isfinite(end) && std::abs(end - start) > 1e-6f)
            normalizedValue = std::clamp((0.0f - start) / (end - start), 0.0f, 1.0f);
        return std::clamp(static_cast<int>(std::lround(normalizedValue * static_cast<float>(size - 1))),
                          0, size - 1);
    };
    const int blendX = neutralBlend(0, groupX);
    const int blendY = neutralBlend(1, groupY);
    const std::size_t blendOffset = static_cast<std::size_t>(blendY * groupX + blendX) * 2u;
    const int animationNumber = i16(mdl, blendTable + blendOffset);
    const auto* animationBinding = resolveAnimationBinding(data, binding.sourceIndex, animationNumber);
    if (!animationBinding || animationBinding->sourceIndex >= data.sources.size()) return false;
    const auto& animationSource = data.sources[animationBinding->sourceIndex];
    const std::size_t animationDescription = animationDescriptionOffset(
        animationSource, animationBinding->localAnimation);
    if (animationDescription == 0 || !animationSource.mdl) return false;
    const auto& animationMdl = *animationSource.mdl;

    const int frameCount = std::max(1, i32(animationMdl,
        animationDescription + StudioAnimationFrameCount));
    const double frameValue = cycle * static_cast<double>(std::max(0, frameCount - 1));
    const int frame = std::clamp(static_cast<int>(std::floor(frameValue)), 0, frameCount - 1);
    const float fraction = static_cast<float>(std::clamp(frameValue - frame, 0.0, 1.0));
    const AnimationChunk chunk = animationChunk(animationSource, animationDescription, frame);

    std::vector<std::array<float, 3>> positions(data.bones.size());
    std::vector<QuaternionValue> quaternions(data.bones.size());
    for (std::size_t index = 0; index < data.bones.size(); ++index) {
        positions[index] = data.bones[index].basePosition;
        quaternions[index] = quaternionFromArray(data.bones[index].baseQuaternion);
    }

    constexpr std::uint8_t AnimRawPosition = 0x01u;
    constexpr std::uint8_t AnimRawRotation = 0x02u;
    constexpr std::uint8_t AnimPosition = 0x04u;
    constexpr std::uint8_t AnimRotation = 0x08u;
    constexpr std::uint8_t AnimDelta = 0x10u;
    constexpr std::uint8_t AnimRawRotation2 = 0x20u;
    constexpr int SequenceDelta = 0x0004;
    constexpr int BoneFixedAlignment = 0x00100000;
    const int animationFlags = i32(animationMdl, animationDescription + StudioAnimationFlags);
    const int weightListRelative = i32(mdl, sequence + StudioSequenceWeightListIndex);

    // Source evaluates a virtual sequence in the sequence-owning group's base
    // pose, not the render model's reference pose. The two are often identical
    // for props and robots, but TF2's human class models use forward/shared
    // animation skeletons whose local base transforms differ from the mesh MDL.
    // Build the inverse boneMap (root -> sequence-local) and initialize only
    // bones represented by that sequence group.
    std::vector<int> sequenceBoneForRoot(data.bones.size(), -1);
    for (std::size_t localBone = 0; localBone < sequenceSource.boneToRoot.size(); ++localBone) {
        const int rootBone = sequenceSource.boneToRoot[localBone];
        if (rootBone >= 0 && rootBone < static_cast<int>(sequenceBoneForRoot.size()) &&
            localBone < sequenceSource.bones.size()) {
            sequenceBoneForRoot[static_cast<std::size_t>(rootBone)] = static_cast<int>(localBone);
        }
    }

    std::vector<float> rootWeights(data.bones.size(), 0.0f);
    const std::size_t weights = weightListRelative > 0
        ? sequence + static_cast<std::size_t>(weightListRelative) : 0u;
    for (std::size_t rootBone = 0; rootBone < sequenceBoneForRoot.size(); ++rootBone) {
        const int localBone = sequenceBoneForRoot[rootBone];
        if (localBone < 0 || localBone >= static_cast<int>(sequenceSource.bones.size())) continue;
        float weight = 1.0f;
        if (weightListRelative > 0) {
            const std::size_t weightOffset = weights + static_cast<std::size_t>(localBone) * sizeof(float);
            weight = range(mdl, weightOffset, sizeof(float)) ? f32(mdl, weightOffset) : 0.0f;
        }
        rootWeights[rootBone] = weight;
        if (weight > 0.0f) {
            const auto& sequenceBone = sequenceSource.bones[static_cast<std::size_t>(localBone)];
            positions[rootBone] = sequenceBone.basePosition;
            quaternions[rootBone] = quaternionFromArray(sequenceBone.baseQuaternion);
        }
    }

    if (chunk.bytes) {
        const auto& bytes = *chunk.bytes;
        std::size_t animation = chunk.offset;
        for (int record = 0; record < 4096 && range(bytes, animation, 4); ++record) {
            const std::uint8_t boneIndex = bytes[animation];
            if (boneIndex == 255u) break;
            const std::uint8_t flags = bytes[animation + 1];
            if (boneIndex < animationSource.bones.size() &&
                boneIndex < animationSource.boneToRoot.size()) {
                const int rootBoneIndex = animationSource.boneToRoot[boneIndex];
                const bool weighted = rootBoneIndex >= 0 &&
                    rootBoneIndex < static_cast<int>(data.bones.size()) &&
                    rootWeights[static_cast<std::size_t>(rootBoneIndex)] > 0.0f;
                if (weighted) {
                    const auto& bone = animationSource.bones[boneIndex];
                    const std::size_t payload = animation + 4;
                    const bool delta = ((flags & AnimDelta) != 0) ||
                                       ((animationFlags | sequenceFlags) & SequenceDelta) != 0;

                    QuaternionValue rotation = delta ? QuaternionValue{}
                                                     : quaternionFromArray(bone.baseQuaternion);
                    if ((flags & AnimRawRotation) != 0 && range(bytes, payload, 6)) {
                        rotation = quaternion48(bytes, payload);
                    } else if ((flags & AnimRawRotation2) != 0 && range(bytes, payload, 8)) {
                        rotation = quaternion64(bytes, payload);
                    } else if ((flags & AnimRotation) != 0 && range(bytes, payload, 6)) {
                        std::array<float, 3> firstAngles{};
                        std::array<float, 3> secondAngles{};
                        for (int axis = 0; axis < 3; ++axis) {
                            extractAnimationAxis(bytes, payload, axis, chunk.localFrame,
                                bone.rotationScale[static_cast<std::size_t>(axis)],
                                firstAngles[static_cast<std::size_t>(axis)],
                                secondAngles[static_cast<std::size_t>(axis)]);
                            if (!delta) {
                                firstAngles[static_cast<std::size_t>(axis)] += bone.baseRotation[static_cast<std::size_t>(axis)];
                                secondAngles[static_cast<std::size_t>(axis)] += bone.baseRotation[static_cast<std::size_t>(axis)];
                            }
                        }
                        rotation = quaternionSlerp(radianEulerQuaternion(firstAngles),
                                                   radianEulerQuaternion(secondAngles), fraction);
                        if (!delta && (bone.flags & BoneFixedAlignment) != 0) {
                            const QuaternionValue alignment = quaternionFromArray(bone.alignment);
                            const float dot = rotation.x * alignment.x + rotation.y * alignment.y +
                                              rotation.z * alignment.z + rotation.w * alignment.w;
                            if (dot < 0.0f) {
                                rotation.x = -rotation.x;
                                rotation.y = -rotation.y;
                                rotation.z = -rotation.z;
                                rotation.w = -rotation.w;
                            }
                        }
                    }

                    std::array<float, 3> position = delta ? std::array<float, 3>{}
                                                          : bone.basePosition;
                    std::size_t rawPosition = payload;
                    if ((flags & AnimRawRotation) != 0) rawPosition += 6;
                    if ((flags & AnimRawRotation2) != 0) rawPosition += 8;
                    if ((flags & AnimRawPosition) != 0 && range(bytes, rawPosition, 6)) {
                        position = {halfFloat(u16(bytes, rawPosition)),
                                    halfFloat(u16(bytes, rawPosition + 2)),
                                    halfFloat(u16(bytes, rawPosition + 4))};
                    } else if ((flags & AnimPosition) != 0) {
                        const std::size_t positionPointers = payload +
                            (((flags & AnimRotation) != 0) ? 6u : 0u);
                        if (range(bytes, positionPointers, 6)) {
                            for (int axis = 0; axis < 3; ++axis) {
                                float firstValue = 0.0f;
                                float secondValue = 0.0f;
                                extractAnimationAxis(bytes, positionPointers, axis, chunk.localFrame,
                                    bone.positionScale[static_cast<std::size_t>(axis)],
                                    firstValue, secondValue);
                                position[static_cast<std::size_t>(axis)] =
                                    firstValue * (1.0f - fraction) + secondValue * fraction +
                                    (delta ? 0.0f : bone.basePosition[static_cast<std::size_t>(axis)]);
                            }
                        }
                    }

                    if (delta) {
                        for (int axis = 0; axis < 3; ++axis)
                            position[static_cast<std::size_t>(axis)] += bone.basePosition[static_cast<std::size_t>(axis)];
                        rotation = quaternionMultiply(quaternionFromArray(bone.baseQuaternion), rotation);
                    }
                    positions[static_cast<std::size_t>(rootBoneIndex)] = position;
                    quaternions[static_cast<std::size_t>(rootBoneIndex)] = rotation;
                }
            }
            const std::int16_t next = i16(bytes, animation + 2);
            if (next <= 0 || !range(bytes, animation + static_cast<std::size_t>(next), 4)) break;
            animation += static_cast<std::size_t>(next);
        }
    }

    std::vector<Matrix3x4> boneToPose(data.bones.size());
    skinMatrices.assign(data.bones.size(), Matrix3x4{});
    std::vector<std::uint8_t> state(data.bones.size(), 0u);
    std::function<void(int)> resolve = [&](int index) {
        if (index < 0 || index >= static_cast<int>(data.bones.size())) return;
        if (state[static_cast<std::size_t>(index)] == 2u) return;
        if (state[static_cast<std::size_t>(index)] == 1u) {
            boneToPose[static_cast<std::size_t>(index)] = Matrix3x4{};
            skinMatrices[static_cast<std::size_t>(index)] = Matrix3x4{};
            state[static_cast<std::size_t>(index)] = 2u;
            return;
        }
        state[static_cast<std::size_t>(index)] = 1u;
        const Matrix3x4 local = quaternionMatrix(
            quaternions[static_cast<std::size_t>(index)].x,
            quaternions[static_cast<std::size_t>(index)].y,
            quaternions[static_cast<std::size_t>(index)].z,
            quaternions[static_cast<std::size_t>(index)].w,
            positions[static_cast<std::size_t>(index)][0],
            positions[static_cast<std::size_t>(index)][1],
            positions[static_cast<std::size_t>(index)][2]);
        const int parent = data.bones[static_cast<std::size_t>(index)].parent;
        if (parent >= 0 && parent < static_cast<int>(data.bones.size()) && parent != index) {
            resolve(parent);
            boneToPose[static_cast<std::size_t>(index)] = concatenate(
                boneToPose[static_cast<std::size_t>(parent)], local);
        } else {
            boneToPose[static_cast<std::size_t>(index)] = local;
        }
        skinMatrices[static_cast<std::size_t>(index)] =
            data.bones[static_cast<std::size_t>(index)].hasPoseToBone
                ? concatenate(boneToPose[static_cast<std::size_t>(index)],
                              matrixFromArray(data.bones[static_cast<std::size_t>(index)].poseToBone))
                : Matrix3x4{};
        state[static_cast<std::size_t>(index)] = 2u;
    };
    for (int index = 0; index < static_cast<int>(data.bones.size()); ++index) resolve(index);
    return true;
}

std::array<float, 3> transformPoint(const Matrix3x4& matrix,
                                    const std::array<float, 3>& point);
std::array<float, 3> rotateVector(const Matrix3x4& matrix,
                                  const std::array<float, 3>& vector);

void skinStudioVertex(StudioVertex& vertex, const std::vector<Matrix3x4>& matrices)
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 3> tangent{};
    float totalWeight = 0.0f;
    const int count = std::min<int>(vertex.influenceCount, 3);
    for (int influence = 0; influence < count; ++influence) {
        const float weight = vertex.boneWeights[static_cast<std::size_t>(influence)];
        const int bone = vertex.boneIndices[static_cast<std::size_t>(influence)];
        if (weight <= 0.0f || bone < 0 || bone >= static_cast<int>(matrices.size())) continue;
        const auto transformedPosition = transformPoint(matrices[static_cast<std::size_t>(bone)],
                                                        vertex.sourcePosition);
        const auto transformedNormal = rotateVector(matrices[static_cast<std::size_t>(bone)],
                                                     vertex.sourceNormal);
        const auto transformedTangent = rotateVector(matrices[static_cast<std::size_t>(bone)],
                                                      vertex.sourceTangent);
        for (int axis = 0; axis < 3; ++axis) {
            position[static_cast<std::size_t>(axis)] += transformedPosition[static_cast<std::size_t>(axis)] * weight;
            normal[static_cast<std::size_t>(axis)] += transformedNormal[static_cast<std::size_t>(axis)] * weight;
            tangent[static_cast<std::size_t>(axis)] += transformedTangent[static_cast<std::size_t>(axis)] * weight;
        }
        totalWeight += weight;
    }
    if (totalWeight <= 1e-6f) {
        position = vertex.sourcePosition;
        normal = vertex.sourceNormal;
        tangent = vertex.sourceTangent;
    } else {
        for (int axis = 0; axis < 3; ++axis)
            position[static_cast<std::size_t>(axis)] /= totalWeight;
    }
    auto normalizeVector = [](std::array<float, 3>& value) {
        const float magnitude = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        if (magnitude > 1e-7f && std::isfinite(magnitude))
            for (float& component : value) component /= magnitude;
    };
    normalizeVector(normal);
    const float tangentDot = tangent[0] * normal[0] + tangent[1] * normal[1] + tangent[2] * normal[2];
    for (int axis = 0; axis < 3; ++axis)
        tangent[static_cast<std::size_t>(axis)] -= normal[static_cast<std::size_t>(axis)] * tangentDot;
    normalizeVector(tangent);

    vertex.x = position[0];
    vertex.y = position[1];
    vertex.z = position[2];
    vertex.nx = normal[0];
    vertex.ny = normal[1];
    vertex.nz = normal[2];
    vertex.tx = tangent[0];
    vertex.ty = tangent[1];
    vertex.tz = tangent[2];
}

std::array<float, 3> transformPoint(const Matrix3x4& matrix,
                                    const std::array<float, 3>& point)
{
    return {
        matrix.m[0][0] * point[0] + matrix.m[0][1] * point[1] +
            matrix.m[0][2] * point[2] + matrix.m[0][3],
        matrix.m[1][0] * point[0] + matrix.m[1][1] * point[1] +
            matrix.m[1][2] * point[2] + matrix.m[1][3],
        matrix.m[2][0] * point[0] + matrix.m[2][1] * point[1] +
            matrix.m[2][2] * point[2] + matrix.m[2][3]
    };
}

std::array<float, 3> rotateVector(const Matrix3x4& matrix,
                                  const std::array<float, 3>& vector)
{
    return {
        matrix.m[0][0] * vector[0] + matrix.m[0][1] * vector[1] + matrix.m[0][2] * vector[2],
        matrix.m[1][0] * vector[0] + matrix.m[1][1] * vector[1] + matrix.m[1][2] * vector[2],
        matrix.m[2][0] * vector[0] + matrix.m[2][1] * vector[1] + matrix.m[2][2] * vector[2]
    };
}

StudioBoneMatrix publicMatrix(const Matrix3x4& matrix)
{
    StudioBoneMatrix result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.values[static_cast<std::size_t>(row * 4 + column)] =
                matrix.m[row][column];
        }
    }
    return result;
}

std::vector<StudioBoneMatrix> publicMatrices(const std::vector<Matrix3x4>& matrices)
{
    std::vector<StudioBoneMatrix> result;
    result.reserve(matrices.size());
    for (const Matrix3x4& matrix : matrices) result.push_back(publicMatrix(matrix));
    return result;
}

void expand(StudioModel& model, const StudioVertex& vertex, bool& hasBounds)
{
    if (!hasBounds) {
        model.minimum = model.maximum = {vertex.x, vertex.y, vertex.z};
        hasBounds = true;
        return;
    }
    model.minimum[0] = std::min(model.minimum[0], vertex.x);
    model.minimum[1] = std::min(model.minimum[1], vertex.y);
    model.minimum[2] = std::min(model.minimum[2], vertex.z);
    model.maximum[0] = std::max(model.maximum[0], vertex.x);
    model.maximum[1] = std::max(model.maximum[1], vertex.y);
    model.maximum[2] = std::max(model.maximum[2], vertex.z);
}

} // namespace

StudioModelSystem::StudioModelSystem(std::shared_ptr<GameFileSystem> fileSystem)
    : fileSystem_(std::move(fileSystem))
{
}

std::shared_ptr<const StudioModel> StudioModelSystem::model(std::string_view path)
{
    const std::string key = normalized(path);
    {
        std::lock_guard lock(mutex_);
        if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
    }
    auto loaded = load(key);
    std::lock_guard lock(mutex_);
    const auto [it, inserted] = cache_.emplace(key, loaded);
    return inserted ? loaded : it->second;
}

void StudioModelSystem::clearCache()
{
    std::lock_guard lock(mutex_);
    cache_.clear();
}

int StudioModel::sequenceIndex(std::string_view labelOrActivity) const
{
    std::string value(labelOrActivity);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return -1;
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);

    int numeric = -1;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, numeric);
    if (parsed.ec == std::errc{} && parsed.ptr == end)
        return numeric >= 0 && numeric < sequenceCount() ? numeric : -1;

    const std::string wanted = lowerAscii(value);
    for (int index = 0; index < sequenceCount(); ++index) {
        const StudioSequence& sequence = sequences[static_cast<std::size_t>(index)];
        if (lowerAscii(sequence.label) == wanted ||
            (!sequence.activityName.empty() && lowerAscii(sequence.activityName) == wanted))
            return index;
    }
    return -1;
}

bool StudioModelSystem::sampleAnimationMatrices(const StudioModel& source, int sequence, double cycle,
                                                std::vector<StudioBoneMatrix>& output) const
{
    output.clear();
    if (!source.animationData) return false;
    std::vector<Matrix3x4> matrices;
    if (!evaluateSequence(*source.animationData, source.normalizedSequence(sequence), cycle, matrices))
        return false;
    output = publicMatrices(matrices);
    return !output.empty();
}

bool StudioModelSystem::sampleAnimation(const StudioModel& source, int sequence, double cycle,
                                        std::vector<std::vector<StudioVertex>>& output) const
{
    output.clear();
    if (!source.animationData || source.meshes.empty()) return false;
    std::vector<Matrix3x4> matrices;
    if (!evaluateSequence(*source.animationData, source.normalizedSequence(sequence), cycle, matrices))
        return false;
    output.reserve(source.meshes.size());
    for (const StudioMesh& mesh : source.meshes) {
        std::vector<StudioVertex> vertices = mesh.vertices;
        for (StudioVertex& vertex : vertices) skinStudioVertex(vertex, matrices);
        output.push_back(std::move(vertices));
    }
    return true;
}

std::shared_ptr<StudioModel> StudioModelSystem::load(std::string modelPath) const
{
    auto output = std::make_shared<StudioModel>();
    output->name = modelPath;
    if (!fileSystem_) {
        output->error = "No game filesystem is configured";
        return output;
    }

    auto mdlFile = fileSystem_->readFile(modelPath);
    if (!mdlFile || !signature(*mdlFile, "IDST")) {
        output->error = "Missing or unsupported MDL: " + modelPath;
        return output;
    }
    auto mdl = std::make_shared<std::vector<std::uint8_t>>(std::move(*mdlFile));
    const std::string stem = withoutExtension(modelPath);
    const auto vvd = fileSystem_->readFile(stem + ".vvd");
    if (!vvd || !signature(*vvd, "IDSV")) {
        output->error = "Missing VVD for " + modelPath;
        return output;
    }
    std::optional<std::vector<std::uint8_t>> vtx;
    for (const std::string suffix : {".dx90.vtx", ".vtx", ".dx80.vtx", ".sw.vtx"}) {
        vtx = fileSystem_->readFile(stem + suffix);
        if (vtx && range(*vtx, 0, 36)) break;
    }
    if (!vtx) {
        output->error = "Missing VTX for " + modelPath;
        return output;
    }

    const std::int32_t checksum = i32(*mdl, 8);
    if (i32(*vvd, 8) != checksum || i32(*vtx, 16) != checksum) {
        output->error = "MDL/VVD/VTX checksum mismatch for " + modelPath;
        return output;
    }
    const std::int32_t vertexDataStart = i32(*vvd, VvdVertexDataStart);
    const std::int32_t tangentDataStart = i32(*vvd, VvdTangentDataStart);
    if (vertexDataStart <= 0 || static_cast<std::size_t>(vertexDataStart) >= vvd->size()) {
        output->error = "Invalid VVD vertex data for " + modelPath;
        return output;
    }

    // Hammer does not draw studio meshes using mstudiobone_t::pos/quat alone.
    // StudioModel::SetUpBones first calls InitPose and then evaluates sequence 0
    // at the current cycle. Static previews begin at cycle zero, so reconstruct
    // that reference pose before composing the hierarchy. This is especially
    // important for root bones: studiomdl commonly stores the visible reference
    // alignment in the first animation while poseToBone was generated from that
    // evaluated pose. Using only the base root values leaves a residual transform.
    struct BonePose {
        std::string name;
        int parent{-1};
        std::array<float, 3> basePosition{};
        QuaternionValue baseQuaternion{};
        std::array<float, 3> baseRotation{};
        std::array<float, 3> positionScale{};
        std::array<float, 3> rotationScale{};
        QuaternionValue alignment{};
        int flags{0};
        std::array<float, 3> posePosition{};
        QuaternionValue poseQuaternion{};
        Matrix3x4 local;
        Matrix3x4 boneToPose;
        Matrix3x4 poseToBone;
        Matrix3x4 skin;
        bool hasPoseToBone{false};
        int state{0};
    };
    std::vector<BonePose> bones;
    const int declaredBoneCount = std::clamp(i32(*mdl, StudioHeaderBoneCount), 0, 256);
    const int boneIndex = i32(*mdl, StudioHeaderBoneIndex);
    if (declaredBoneCount > 0) {
        bones.resize(static_cast<std::size_t>(declaredBoneCount));

        auto readVector3 = [&](std::size_t offset) {
            return std::array<float, 3>{f32(*mdl, offset), f32(*mdl, offset + 4),
                                        f32(*mdl, offset + 8)};
        };
        auto readQuaternion = [&](std::size_t offset) {
            return normalizeQuaternion({f32(*mdl, offset), f32(*mdl, offset + 4),
                                        f32(*mdl, offset + 8), f32(*mdl, offset + 12)});
        };

        bool loadedLinearBones = false;
        const int header2Index = i32(*mdl, StudioHeader2Index);
        if (header2Index > 0 && range(*mdl, static_cast<std::size_t>(header2Index),
                                      StudioHeader2LinearBoneIndex + sizeof(std::int32_t))) {
            const std::size_t header2 = static_cast<std::size_t>(header2Index);
            const int linearRelative = i32(*mdl, header2 + StudioHeader2LinearBoneIndex);
            if (linearRelative > 0) {
                const std::size_t linear = header2 + static_cast<std::size_t>(linearRelative);
                if (range(*mdl, linear, StudioLinearBoneSize) &&
                    i32(*mdl, linear) == declaredBoneCount) {
                    const int flagsRelative = i32(*mdl, linear + StudioLinearBoneFlagsIndex);
                    const int parentRelative = i32(*mdl, linear + StudioLinearBoneParentIndex);
                    const int positionRelative = i32(*mdl, linear + StudioLinearBonePositionIndex);
                    const int quaternionRelative = i32(*mdl, linear + StudioLinearBoneQuaternionIndex);
                    const int rotationRelative = i32(*mdl, linear + StudioLinearBoneRotationIndex);
                    const int poseToBoneRelative = i32(*mdl, linear + StudioLinearBonePoseToBoneIndex);
                    const int positionScaleRelative = i32(*mdl, linear + StudioLinearBonePositionScaleIndex);
                    const int rotationScaleRelative = i32(*mdl, linear + StudioLinearBoneRotationScaleIndex);
                    const int alignmentRelative = i32(*mdl, linear + StudioLinearBoneAlignmentIndex);
                    const bool arraysValid = parentRelative > 0 && positionRelative > 0 &&
                        quaternionRelative > 0 && poseToBoneRelative > 0 &&
                        range(*mdl, linear + static_cast<std::size_t>(parentRelative),
                              static_cast<std::size_t>(declaredBoneCount) * sizeof(std::int32_t)) &&
                        range(*mdl, linear + static_cast<std::size_t>(positionRelative),
                              static_cast<std::size_t>(declaredBoneCount) * 3u * sizeof(float)) &&
                        range(*mdl, linear + static_cast<std::size_t>(quaternionRelative),
                              static_cast<std::size_t>(declaredBoneCount) * 4u * sizeof(float)) &&
                        range(*mdl, linear + static_cast<std::size_t>(poseToBoneRelative),
                              static_cast<std::size_t>(declaredBoneCount) * 12u * sizeof(float));
                    if (arraysValid) {
                        auto optionalArray = [&](int relative, std::size_t stride) {
                            return relative > 0 &&
                                range(*mdl, linear + static_cast<std::size_t>(relative),
                                      static_cast<std::size_t>(declaredBoneCount) * stride);
                        };
                        const bool hasFlags = optionalArray(flagsRelative, sizeof(std::int32_t));
                        const bool hasRotation = optionalArray(rotationRelative, 3u * sizeof(float));
                        const bool hasPositionScale = optionalArray(positionScaleRelative, 3u * sizeof(float));
                        const bool hasRotationScale = optionalArray(rotationScaleRelative, 3u * sizeof(float));
                        const bool hasAlignment = optionalArray(alignmentRelative, 4u * sizeof(float));
                        for (int index = 0; index < declaredBoneCount; ++index) {
                            BonePose& bone = bones[static_cast<std::size_t>(index)];
                            if (boneIndex > 0) {
                                const std::size_t legacy = static_cast<std::size_t>(boneIndex) +
                                    static_cast<std::size_t>(index) * StudioBoneSize;
                                const int nameRelative = i32(*mdl, legacy + StudioBoneNameIndex);
                                if (nameRelative > 0) bone.name = cstring(*mdl, legacy + static_cast<std::size_t>(nameRelative));
                            }
                            const std::size_t parent = linear + static_cast<std::size_t>(parentRelative) +
                                static_cast<std::size_t>(index) * sizeof(std::int32_t);
                            const std::size_t position = linear + static_cast<std::size_t>(positionRelative) +
                                static_cast<std::size_t>(index) * 3u * sizeof(float);
                            const std::size_t quaternion = linear + static_cast<std::size_t>(quaternionRelative) +
                                static_cast<std::size_t>(index) * 4u * sizeof(float);
                            const std::size_t poseToBone = linear + static_cast<std::size_t>(poseToBoneRelative) +
                                static_cast<std::size_t>(index) * 12u * sizeof(float);
                            bone.parent = i32(*mdl, parent);
                            bone.basePosition = readVector3(position);
                            bone.baseQuaternion = readQuaternion(quaternion);
                            bone.baseRotation = hasRotation ? readVector3(linear + static_cast<std::size_t>(rotationRelative) +
                                static_cast<std::size_t>(index) * 3u * sizeof(float)) : std::array<float, 3>{};
                            bone.positionScale = hasPositionScale ? readVector3(linear + static_cast<std::size_t>(positionScaleRelative) +
                                static_cast<std::size_t>(index) * 3u * sizeof(float)) : std::array<float, 3>{};
                            bone.rotationScale = hasRotationScale ? readVector3(linear + static_cast<std::size_t>(rotationScaleRelative) +
                                static_cast<std::size_t>(index) * 3u * sizeof(float)) : std::array<float, 3>{};
                            bone.alignment = hasAlignment ? readQuaternion(linear + static_cast<std::size_t>(alignmentRelative) +
                                static_cast<std::size_t>(index) * 4u * sizeof(float)) : QuaternionValue{};
                            bone.flags = hasFlags ? i32(*mdl, linear + static_cast<std::size_t>(flagsRelative) +
                                static_cast<std::size_t>(index) * sizeof(std::int32_t)) : 0;
                            bone.poseToBone = readMatrix3x4(*mdl, poseToBone);
                            bone.hasPoseToBone = usableAffineMatrix(bone.poseToBone);
                        }
                        loadedLinearBones = true;
                    }
                }
            }
        }

        if (!loadedLinearBones && boneIndex > 0 &&
            range(*mdl, static_cast<std::size_t>(boneIndex),
                  static_cast<std::size_t>(declaredBoneCount) * StudioBoneSize)) {
            for (int index = 0; index < declaredBoneCount; ++index) {
                const std::size_t base = static_cast<std::size_t>(boneIndex) +
                                         static_cast<std::size_t>(index) * StudioBoneSize;
                BonePose& bone = bones[static_cast<std::size_t>(index)];
                const int nameRelative = i32(*mdl, base + StudioBoneNameIndex);
                if (nameRelative > 0) bone.name = cstring(*mdl, base + static_cast<std::size_t>(nameRelative));
                bone.parent = i32(*mdl, base + 4);
                bone.basePosition = readVector3(base + StudioBonePosition);
                bone.baseQuaternion = readQuaternion(base + StudioBoneQuaternion);
                bone.baseRotation = readVector3(base + StudioBoneRotation);
                bone.positionScale = readVector3(base + StudioBonePositionScale);
                bone.rotationScale = readVector3(base + StudioBoneRotationScale);
                bone.poseToBone = readMatrix3x4(*mdl, base + StudioBonePoseToBone);
                bone.hasPoseToBone = usableAffineMatrix(bone.poseToBone);
                bone.alignment = readQuaternion(base + StudioBoneAlignment);
                bone.flags = i32(*mdl, base + StudioBoneFlags);
            }
        }

        for (BonePose& bone : bones) {
            bone.posePosition = bone.basePosition;
            bone.poseQuaternion = bone.baseQuaternion;
        }

        // Reproduce the initial Hammer StudioModel pose: sequence 0, blend 0,
        // cycle 0. Local inline animation data is sufficient for ordinary prop
        // models; unsupported external animation blocks safely leave InitPose.
        constexpr std::uint8_t AnimRawPosition = 0x01u;
        constexpr std::uint8_t AnimRawRotation = 0x02u;
        constexpr std::uint8_t AnimPosition = 0x04u;
        constexpr std::uint8_t AnimRotation = 0x08u;
        constexpr std::uint8_t AnimDelta = 0x10u;
        constexpr std::uint8_t AnimRawRotation2 = 0x20u;
        constexpr int SequenceDelta = 0x0004;
        constexpr int BoneFixedAlignment = 0x00100000;

        const int localSequenceCount = std::max(0, i32(*mdl, StudioHeaderLocalSequenceCount));
        const int localSequenceIndex = i32(*mdl, StudioHeaderLocalSequenceIndex);
        const int localAnimationCount = std::max(0, i32(*mdl, StudioHeaderLocalAnimationCount));
        const int localAnimationIndex = i32(*mdl, StudioHeaderLocalAnimationIndex);
        if (localSequenceCount > 0 && localAnimationCount > 0 &&
            localSequenceIndex > 0 && localAnimationIndex > 0 &&
            range(*mdl, static_cast<std::size_t>(localSequenceIndex), StudioSequenceDescriptionSize)) {
            const std::size_t sequence = static_cast<std::size_t>(localSequenceIndex);
            const int groupX = std::max(1, i32(*mdl, sequence + StudioSequenceGroupSize));
            const int groupY = std::max(1, i32(*mdl, sequence + StudioSequenceGroupSize + 4));
            const int animationIndexRelative = i32(*mdl, sequence + StudioSequenceAnimationIndex);
            const int weightListRelative = i32(*mdl, sequence + StudioSequenceWeightListIndex);
            if (groupX > 0 && groupY > 0 && animationIndexRelative > 0 &&
                range(*mdl, sequence + static_cast<std::size_t>(animationIndexRelative), sizeof(std::int16_t))) {
                const int animationNumber = i16(*mdl, sequence + static_cast<std::size_t>(animationIndexRelative));
                if (animationNumber >= 0 && animationNumber < localAnimationCount) {
                    const std::size_t animationDescription = static_cast<std::size_t>(localAnimationIndex) +
                        static_cast<std::size_t>(animationNumber) * StudioAnimationDescriptionSize;
                    if (range(*mdl, animationDescription, StudioAnimationDescriptionSize) &&
                        i32(*mdl, animationDescription + StudioAnimationFrameCount) > 0 &&
                        (i32(*mdl, animationDescription + StudioAnimationFlags) & SequenceDelta) == 0) {
                        int block = i32(*mdl, animationDescription + StudioAnimationBlock);
                        int dataRelative = i32(*mdl, animationDescription + StudioAnimationDataIndex);
                        const int sectionFrames = i32(*mdl, animationDescription + StudioAnimationSectionFrames);
                        if (sectionFrames > 0) {
                            const int sectionRelative = i32(*mdl, animationDescription + StudioAnimationSectionIndex);
                            if (sectionRelative > 0 && range(*mdl, animationDescription +
                                    static_cast<std::size_t>(sectionRelative), 8)) {
                                const std::size_t section = animationDescription +
                                    static_cast<std::size_t>(sectionRelative);
                                block = i32(*mdl, section);
                                dataRelative = i32(*mdl, section + 4);
                            }
                        }
                        if (block == 0 && dataRelative > 0 &&
                            range(*mdl, animationDescription + static_cast<std::size_t>(dataRelative), 4)) {
                            std::size_t animation = animationDescription + static_cast<std::size_t>(dataRelative);
                            for (int record = 0; record < 1024 && range(*mdl, animation, 4); ++record) {
                                const std::uint8_t boneIndexInAnimation = (*mdl)[animation];
                                if (boneIndexInAnimation == 255u) break;
                                const std::uint8_t flags = (*mdl)[animation + 1];
                                if (boneIndexInAnimation < bones.size()) {
                                    bool weighted = true;
                                    if (weightListRelative > 0 && range(*mdl, sequence +
                                            static_cast<std::size_t>(weightListRelative) +
                                            static_cast<std::size_t>(boneIndexInAnimation) * sizeof(float), sizeof(float))) {
                                        weighted = f32(*mdl, sequence + static_cast<std::size_t>(weightListRelative) +
                                            static_cast<std::size_t>(boneIndexInAnimation) * sizeof(float)) > 0.0f;
                                    }
                                    if (weighted) {
                                        BonePose& bone = bones[boneIndexInAnimation];
                                        const std::size_t data = animation + 4;
                                        const bool delta = (flags & AnimDelta) != 0;
                                        QuaternionValue quaternion = delta ? QuaternionValue{} : bone.baseQuaternion;
                                        if ((flags & AnimRawRotation) != 0 && range(*mdl, data, 6)) {
                                            quaternion = quaternion48(*mdl, data);
                                        } else if ((flags & AnimRawRotation2) != 0 && range(*mdl, data, 8)) {
                                            quaternion = quaternion64(*mdl, data);
                                        } else if ((flags & AnimRotation) != 0 && range(*mdl, data, 6)) {
                                            std::array<float, 3> rotation{};
                                            for (int axis = 0; axis < 3; ++axis) {
                                                rotation[static_cast<std::size_t>(axis)] =
                                                    frameZeroAnimationValue(*mdl, data, axis,
                                                        bone.rotationScale[static_cast<std::size_t>(axis)]) +
                                                    (delta ? 0.0f : bone.baseRotation[static_cast<std::size_t>(axis)]);
                                            }
                                            quaternion = radianEulerQuaternion(rotation);
                                            if (!delta && (bone.flags & BoneFixedAlignment) != 0) {
                                                const float dot = quaternion.x * bone.alignment.x +
                                                    quaternion.y * bone.alignment.y +
                                                    quaternion.z * bone.alignment.z +
                                                    quaternion.w * bone.alignment.w;
                                                if (dot < 0.0f) {
                                                    quaternion.x = -quaternion.x;
                                                    quaternion.y = -quaternion.y;
                                                    quaternion.z = -quaternion.z;
                                                    quaternion.w = -quaternion.w;
                                                }
                                            }
                                        }

                                        std::array<float, 3> position = delta ? std::array<float, 3>{}
                                                                               : bone.basePosition;
                                        std::size_t rawPosition = data;
                                        if ((flags & AnimRawRotation) != 0) rawPosition += 6;
                                        if ((flags & AnimRawRotation2) != 0) rawPosition += 8;
                                        if ((flags & AnimRawPosition) != 0 && range(*mdl, rawPosition, 6)) {
                                            position = {halfFloat(u16(*mdl, rawPosition)),
                                                        halfFloat(u16(*mdl, rawPosition + 2)),
                                                        halfFloat(u16(*mdl, rawPosition + 4))};
                                        } else if ((flags & AnimPosition) != 0) {
                                            const std::size_t positionValues = data +
                                                (((flags & AnimRotation) != 0) ? 6u : 0u);
                                            if (range(*mdl, positionValues, 6)) {
                                                for (int axis = 0; axis < 3; ++axis) {
                                                    position[static_cast<std::size_t>(axis)] =
                                                        frameZeroAnimationValue(*mdl, positionValues, axis,
                                                            bone.positionScale[static_cast<std::size_t>(axis)]) +
                                                        (delta ? 0.0f : bone.basePosition[static_cast<std::size_t>(axis)]);
                                                }
                                            }
                                        }
                                        bone.posePosition = position;
                                        bone.poseQuaternion = quaternion;
                                    }
                                }
                                const std::int16_t next = i16(*mdl, animation + 2);
                                if (next <= 0 || !range(*mdl, animation + static_cast<std::size_t>(next), 4)) break;
                                animation += static_cast<std::size_t>(next);
                            }
                        }
                    }
                }
            }
        }

        for (BonePose& bone : bones) {
            bone.local = quaternionMatrix(bone.poseQuaternion.x, bone.poseQuaternion.y,
                                          bone.poseQuaternion.z, bone.poseQuaternion.w,
                                          bone.posePosition[0], bone.posePosition[1],
                                          bone.posePosition[2]);
        }

        std::function<void(int)> resolveBone = [&](int index) {
            BonePose& bone = bones[static_cast<std::size_t>(index)];
            if (bone.state == 2) return;
            if (bone.state == 1) {
                bone.boneToPose = Matrix3x4{};
                bone.skin = Matrix3x4{};
                bone.state = 2;
                return;
            }
            bone.state = 1;
            if (bone.parent >= 0 && bone.parent < declaredBoneCount && bone.parent != index) {
                resolveBone(bone.parent);
                bone.boneToPose = concatenate(
                    bones[static_cast<std::size_t>(bone.parent)].boneToPose, bone.local);
            } else {
                bone.boneToPose = bone.local;
            }

            // The VVD is authored in model/reference-pose space. Source's
            // studio renderer applies the evaluated bone-to-pose matrix and the
            // stored inverse bind matrix. At the reconstructed reference pose
            // this product is identity, including for animated root alignment.
            bone.skin = bone.hasPoseToBone
                ? concatenate(bone.boneToPose, bone.poseToBone)
                : Matrix3x4{};
            bone.state = 2;
        };
        for (int index = 0; index < declaredBoneCount; ++index) resolveBone(index);
    }

    auto animationData = std::make_shared<StudioAnimationData>();
    animationData->bones.reserve(bones.size());
    for (const BonePose& sourceBone : bones) {
        StudioAnimationData::Bone bone;
        bone.name = sourceBone.name;
        bone.parent = sourceBone.parent;
        bone.basePosition = sourceBone.basePosition;
        bone.baseQuaternion = quaternionToArray(sourceBone.baseQuaternion);
        bone.baseRotation = sourceBone.baseRotation;
        bone.positionScale = sourceBone.positionScale;
        bone.rotationScale = sourceBone.rotationScale;
        bone.alignment = quaternionToArray(sourceBone.alignment);
        bone.poseToBone = matrixToArray(sourceBone.poseToBone);
        bone.flags = sourceBone.flags;
        bone.hasPoseToBone = sourceBone.hasPoseToBone;
        animationData->bones.push_back(std::move(bone));
    }

    auto readSequenceMetadata = [&](std::size_t sourceIndex, int localSequence,
                                    StudioSequence& sequence) -> bool {
        if (sourceIndex >= animationData->sources.size()) return false;
        const auto& source = animationData->sources[sourceIndex];
        if (!source.mdl || source.localSequenceCount <= 0 || source.localSequenceIndex <= 0 ||
            localSequence < 0 || localSequence >= source.localSequenceCount) return false;
        const auto& sourceMdl = *source.mdl;
        const std::size_t sequenceOffset = static_cast<std::size_t>(source.localSequenceIndex) +
            static_cast<std::size_t>(localSequence) * StudioSequenceDescriptionSize;
        if (!range(sourceMdl, sequenceOffset, StudioSequenceDescriptionSize)) return false;

        const int labelRelative = i32(sourceMdl, sequenceOffset + StudioSequenceLabelIndex);
        const int activityRelative = i32(sourceMdl, sequenceOffset + StudioSequenceActivityNameIndex);
        if (labelRelative > 0)
            sequence.label = cstring(sourceMdl, sequenceOffset + static_cast<std::size_t>(labelRelative));
        if (activityRelative > 0)
            sequence.activityName = cstring(sourceMdl, sequenceOffset + static_cast<std::size_t>(activityRelative));
        if (sequence.label.empty())
            sequence.label = "Sequence " + std::to_string(sourceIndex) + ":" + std::to_string(localSequence);
        const int flags = i32(sourceMdl, sequenceOffset + StudioSequenceFlags);
        sequence.looping = (flags & 0x0001) != 0;
        sequence.delta = (flags & 0x0004) != 0;
        sequence.minimum = {f32(sourceMdl, sequenceOffset + StudioSequenceMinimum),
                            f32(sourceMdl, sequenceOffset + StudioSequenceMinimum + 4),
                            f32(sourceMdl, sequenceOffset + StudioSequenceMinimum + 8)};
        sequence.maximum = {f32(sourceMdl, sequenceOffset + StudioSequenceMaximum),
                            f32(sourceMdl, sequenceOffset + StudioSequenceMaximum + 4),
                            f32(sourceMdl, sequenceOffset + StudioSequenceMaximum + 8)};

        const int groupX = std::clamp(i32(sourceMdl, sequenceOffset + StudioSequenceGroupSize), 1, 256);
        const int groupY = std::clamp(i32(sourceMdl, sequenceOffset + StudioSequenceGroupSize + 4), 1, 256);
        const int animationRelative = i32(sourceMdl, sequenceOffset + StudioSequenceAnimationIndex);
        if (animationRelative > 0) {
            const std::size_t table = sequenceOffset + static_cast<std::size_t>(animationRelative);
            const std::size_t blendCount = static_cast<std::size_t>(groupX) * static_cast<std::size_t>(groupY);
            if (range(sourceMdl, table, blendCount * 2u)) {
                auto neutralBlend = [&](int dimension, int size) {
                    if (size <= 1) return 0;
                    const float start = f32(sourceMdl, sequenceOffset + StudioSequenceParameterStart +
                        static_cast<std::size_t>(dimension) * sizeof(float));
                    const float end = f32(sourceMdl, sequenceOffset + StudioSequenceParameterEnd +
                        static_cast<std::size_t>(dimension) * sizeof(float));
                    float normalizedValue = 0.5f;
                    if (std::isfinite(start) && std::isfinite(end) && std::abs(end - start) > 1e-6f)
                        normalizedValue = std::clamp((0.0f - start) / (end - start), 0.0f, 1.0f);
                    return std::clamp(static_cast<int>(std::lround(
                        normalizedValue * static_cast<float>(size - 1))), 0, size - 1);
                };
                const std::size_t neutral = static_cast<std::size_t>(
                    neutralBlend(1, groupY) * groupX + neutralBlend(0, groupX));
                int animationNumber = i16(sourceMdl, table + neutral * 2u);
                if (animationNumber < 0 || animationNumber >= source.localAnimationCount) {
                    animationNumber = -1;
                    for (std::size_t blend = 0; blend < blendCount; ++blend) {
                        const int candidate = i16(sourceMdl, table + blend * 2u);
                        if (candidate >= 0 && candidate < source.localAnimationCount) {
                            animationNumber = candidate;
                            break;
                        }
                    }
                }
                if (animationNumber >= 0) {
                    const auto* resolved = resolveAnimationBinding(
                        *animationData, sourceIndex, animationNumber);
                    if (resolved && resolved->sourceIndex < animationData->sources.size()) {
                        const auto& animationSource = animationData->sources[resolved->sourceIndex];
                        const std::size_t animationOffset = animationDescriptionOffset(
                            animationSource, resolved->localAnimation);
                        if (animationOffset != 0 && animationSource.mdl) {
                            const auto& animationMdl = *animationSource.mdl;
                            sequence.fps = std::max(0.0f,
                                f32(animationMdl, animationOffset + StudioAnimationFps));
                            sequence.frameCount = std::max(1, i32(animationMdl,
                                animationOffset + StudioAnimationFrameCount));
                            if (sequence.fps > 0.0f && sequence.frameCount > 1)
                                sequence.duration = static_cast<float>(sequence.frameCount - 1) / sequence.fps;
                        }
                    }
                }
            }
        }
        return true;
    };

    std::unordered_set<std::string> visitedAnimationModels;
    visitedAnimationModels.insert(modelPath);
    std::function<void(std::string, std::shared_ptr<const std::vector<std::uint8_t>>, int)> appendSource;
    appendSource = [&](std::string sourcePath,
                       std::shared_ptr<const std::vector<std::uint8_t>> sourceMdl,
                       int depth) {
        if (!sourceMdl || depth > 16) return;
        StudioAnimationData::Source source = makeAnimationSource(
            sourcePath, std::move(sourceMdl), fileSystem_, animationData->bones);
        const std::size_t sourceIndex = animationData->sources.size();
        animationData->sources.push_back(std::move(source));

        const auto& stored = animationData->sources[sourceIndex];
        if (!stored.mdl || depth == 16) return;
        // Copy the include list before recursing: appendSource grows the source
        // vector and may reallocate it, so no references into that vector may
        // survive a recursive call.
        const std::vector<std::string> includedModels = includedModelPaths(*stored.mdl);
        for (const std::string& included : includedModels) {
            if (!visitedAnimationModels.insert(included).second) continue;
            auto includeFile = fileSystem_->readFile(included);
            if (!includeFile || !signature(*includeFile, "IDST")) continue;
            appendSource(included,
                std::make_shared<const std::vector<std::uint8_t>>(std::move(*includeFile)), depth + 1);
        }
    };

    appendSource(modelPath, std::shared_ptr<const std::vector<std::uint8_t>>(mdl), 0);

    // Build the virtual model's global animation table. Source sequences store
    // animation numbers local to their owning group and iRelativeAnim resolves
    // those through virtualgroup_t::masterAnim. Animation-only include models
    // may redeclare an animation name while the actual channel data lives in a
    // different group, which is how TF2's class animation sets are assembled.
    std::unordered_map<std::string, int> animationByName;
    std::vector<int> animationPayloadScores;
    auto payloadScore = [&](const StudioAnimationData::AnimationBinding& binding) {
        if (binding.sourceIndex >= animationData->sources.size()) return 0;
        const auto& source = animationData->sources[binding.sourceIndex];
        const std::size_t description = animationDescriptionOffset(source, binding.localAnimation);
        if (description == 0 || !source.mdl) return 0;
        if (animationChunk(source, description, 0).bytes) return 2;
        return std::max(1, i32(*source.mdl, description + StudioAnimationFrameCount)) == 1 ? 1 : 0;
    };

    for (std::size_t sourceIndex = 0; sourceIndex < animationData->sources.size(); ++sourceIndex) {
        auto& source = animationData->sources[sourceIndex];
        source.masterAnimations.assign(static_cast<std::size_t>(source.localAnimationCount), -1);
        for (int localAnimation = 0; localAnimation < source.localAnimationCount; ++localAnimation) {
            const std::size_t description = animationDescriptionOffset(source, localAnimation);
            if (description == 0 || !source.mdl) continue;
            const int nameRelative = i32(*source.mdl, description + StudioAnimationNameIndex);
            const std::string name = nameRelative > 0
                ? lowerAscii(cstring(*source.mdl, description + static_cast<std::size_t>(nameRelative)))
                : std::string{};
            const StudioAnimationData::AnimationBinding candidate{sourceIndex, localAnimation};
            int globalAnimation = -1;
            if (!name.empty()) {
                if (const auto found = animationByName.find(name); found != animationByName.end()) {
                    globalAnimation = found->second;
                    const int score = payloadScore(candidate);
                    if (globalAnimation >= 0 &&
                        globalAnimation < static_cast<int>(animationPayloadScores.size()) &&
                        score > animationPayloadScores[static_cast<std::size_t>(globalAnimation)]) {
                        animationData->animations[static_cast<std::size_t>(globalAnimation)] = candidate;
                        animationPayloadScores[static_cast<std::size_t>(globalAnimation)] = score;
                    }
                }
            }
            if (globalAnimation < 0) {
                globalAnimation = static_cast<int>(animationData->animations.size());
                animationData->animations.push_back(candidate);
                animationPayloadScores.push_back(payloadScore(candidate));
                if (!name.empty()) animationByName.emplace(name, globalAnimation);
            }
            source.masterAnimations[static_cast<std::size_t>(localAnimation)] = globalAnimation;
        }
    }

    // Build Source's virtual sequence table instead of exposing every local
    // descriptor independently. Human TF2 player MDLs use $declaresequence,
    // which compiles to an empty STUDIO_OVERRIDE descriptor in the mesh MDL;
    // the included animation model later replaces that same-named entry. If the
    // forward declaration wins lookup, sampling succeeds structurally but has
    // no animation payload and the player remains in reference pose.
    constexpr int SequenceOverride = 0x0800;
    std::unordered_map<std::string, int> sequenceByLabel;
    auto sequenceFlagsForBinding = [&](const StudioAnimationData::SequenceBinding& binding) {
        if (binding.sourceIndex >= animationData->sources.size()) return 0;
        const auto& source = animationData->sources[binding.sourceIndex];
        if (!source.mdl || source.localSequenceIndex <= 0 || binding.localSequence < 0 ||
            binding.localSequence >= source.localSequenceCount) return 0;
        const std::size_t offset = static_cast<std::size_t>(source.localSequenceIndex) +
            static_cast<std::size_t>(binding.localSequence) * StudioSequenceDescriptionSize;
        return range(*source.mdl, offset, StudioSequenceDescriptionSize)
            ? i32(*source.mdl, offset + StudioSequenceFlags) : 0;
    };

    for (std::size_t sourceIndex = 0; sourceIndex < animationData->sources.size(); ++sourceIndex) {
        auto& source = animationData->sources[sourceIndex];
        source.masterSequences.assign(static_cast<std::size_t>(source.localSequenceCount), -1);
        const int previousGroupCount = static_cast<int>(animationData->sequences.size());
        std::vector<std::pair<std::string, int>> labelsAddedByGroup;
        for (int localSequence = 0; localSequence < source.localSequenceCount; ++localSequence) {
            StudioSequence metadata;
            if (!readSequenceMetadata(sourceIndex, localSequence, metadata)) continue;
            const std::string key = lowerAscii(metadata.label);
            int globalSequence = -1;
            if (!key.empty()) {
                // Valve compares a group's local sequences only against groups
                // already present when AppendSequences began. Same-group labels
                // remain distinct and enter the lookup table after the group.
                if (const auto found = sequenceByLabel.find(key); found != sequenceByLabel.end())
                    globalSequence = found->second;
            }

            const StudioAnimationData::SequenceBinding candidate{sourceIndex, localSequence};
            if (globalSequence < 0) {
                globalSequence = static_cast<int>(animationData->sequences.size());
                animationData->sequences.push_back(candidate);
                output->sequences.push_back(std::move(metadata));
                if (!key.empty()) labelsAddedByGroup.emplace_back(key, globalSequence);
            } else if (globalSequence < previousGroupCount &&
                       (sequenceFlagsForBinding(
                            animationData->sequences[static_cast<std::size_t>(globalSequence)]) &
                        SequenceOverride) != 0) {
                // Match virtualmodel_t::AppendSequences: an included sequence
                // replaces a previously stored forward declaration in-place so
                // sequence indices and entity-authored DefaultAnim values remain
                // stable.
                animationData->sequences[static_cast<std::size_t>(globalSequence)] = candidate;
                output->sequences[static_cast<std::size_t>(globalSequence)] = std::move(metadata);
            }
            source.masterSequences[static_cast<std::size_t>(localSequence)] = globalSequence;
        }
        for (const auto& [label, globalSequence] : labelsAddedByGroup)
            sequenceByLabel.emplace(label, globalSequence);
    }

    if (!output->sequences.empty() && !animationData->bones.empty() &&
        !animationData->animations.empty())
        output->animationData = animationData;
    output->referencePoseMatrices.clear();
    output->referencePoseMatrices.reserve(bones.size());
    for (const BonePose& bone : bones)
        output->referencePoseMatrices.push_back(publicMatrix(bone.skin));

    std::vector<std::string> textureNames;
    const int textureCount = std::max(0, i32(*mdl, StudioHeaderTextureCount));
    const int textureIndex = i32(*mdl, StudioHeaderTextureIndex);
    for (int index = 0; index < textureCount; ++index) {
        const std::size_t base = static_cast<std::size_t>(textureIndex) +
                                 static_cast<std::size_t>(index) * StudioTextureSize;
        if (!range(*mdl, base, StudioTextureSize)) break;
        const int nameOffset = i32(*mdl, base);
        textureNames.push_back(materialName(cstring(*mdl, base + std::max(0, nameOffset))));
    }

    std::vector<std::string> textureDirectories;
    const int directoryCount = std::max(0, i32(*mdl, StudioHeaderCdTextureCount));
    const int directoryIndex = i32(*mdl, StudioHeaderCdTextureIndex);
    for (int index = 0; index < directoryCount; ++index) {
        const std::size_t entry = static_cast<std::size_t>(directoryIndex) +
                                  static_cast<std::size_t>(index) * 4u;
        const int nameOffset = i32(*mdl, entry);
        if (nameOffset > 0) textureDirectories.push_back(materialName(cstring(*mdl, nameOffset)));
    }
    if (textureDirectories.empty()) textureDirectories.push_back({});

    auto resolvedTextureMaterial = [&](int texture) {
        if (texture < 0 || texture >= static_cast<int>(textureNames.size())) return std::string{};
        for (const std::string& directory : textureDirectories) {
            const std::string candidate = joinMaterial(directory, textureNames[static_cast<std::size_t>(texture)]);
            if (fileSystem_->exists("materials/" + candidate + ".vmt")) return candidate;
        }
        return joinMaterial(textureDirectories.front(), textureNames[static_cast<std::size_t>(texture)]);
    };

    const int skinReferenceCount = std::max(0, i32(*mdl, StudioHeaderSkinReferenceCount));
    const int skinFamilyCount = std::max(0, i32(*mdl, StudioHeaderSkinFamilyCount));
    const int skinIndex = i32(*mdl, StudioHeaderSkinIndex);
    if (skinFamilyCount > 0 && skinReferenceCount > 0 && skinIndex > 0) {
        output->skinFamilies.reserve(static_cast<std::size_t>(skinFamilyCount));
        for (int familyIndex = 0; familyIndex < skinFamilyCount; ++familyIndex) {
            std::vector<std::string> family;
            family.reserve(static_cast<std::size_t>(skinReferenceCount));
            for (int referenceIndex = 0; referenceIndex < skinReferenceCount; ++referenceIndex) {
                const std::size_t tableIndex = static_cast<std::size_t>(skinIndex) +
                    (static_cast<std::size_t>(familyIndex) * static_cast<std::size_t>(skinReferenceCount) +
                     static_cast<std::size_t>(referenceIndex)) * 2u;
                const int texture = range(*mdl, tableIndex, 2)
                    ? static_cast<int>(u16(*mdl, tableIndex)) : referenceIndex;
                family.push_back(resolvedTextureMaterial(texture));
            }
            output->skinFamilies.push_back(std::move(family));
        }
    }

    // Models without an explicit skin table still address texture slots directly.
    if (output->skinFamilies.empty()) {
        std::vector<std::string> family;
        family.reserve(textureNames.size());
        for (int texture = 0; texture < static_cast<int>(textureNames.size()); ++texture)
            family.push_back(resolvedTextureMaterial(texture));
        output->skinFamilies.push_back(std::move(family));
    }

    const int mdlBodyPartCount = std::max(0, i32(*mdl, StudioHeaderBodyPartCount));
    const int mdlBodyPartIndex = i32(*mdl, StudioHeaderBodyPartIndex);
    const int vtxBodyPartCount = std::max(0, i32(*vtx, 28));
    const int vtxBodyPartIndex = i32(*vtx, VtxBodyPartOffset);
    const int bodyPartCount = std::min(mdlBodyPartCount, vtxBodyPartCount);
    bool hasBounds = false;

    std::vector<std::size_t> fixedVertexOrder;
    const int fixupCount = std::max(0, i32(*vvd, VvdFixupCount));
    const int fixupTableStart = i32(*vvd, VvdFixupTableStart);
    if (fixupCount > 0 && fixupTableStart > 0) {
        for (int fixup = 0; fixup < fixupCount; ++fixup) {
            const std::size_t entry = static_cast<std::size_t>(fixupTableStart) +
                                      static_cast<std::size_t>(fixup) * 12u;
            if (!range(*vvd, entry, 12)) break;
            const int lod = i32(*vvd, entry);
            const int source = std::max(0, i32(*vvd, entry + 4));
            const int count = std::max(0, i32(*vvd, entry + 8));
            // Root LOD zero includes every fixup range. VTX indices address
            // this fixed-up logical order rather than the raw VVD order.
            if (lod < 0) continue;
            for (int index = 0; index < count; ++index)
                fixedVertexOrder.push_back(static_cast<std::size_t>(source + index));
        }
    }

    auto readVertex = [&](std::size_t index, StudioVertex& vertex) -> bool {
        const std::size_t rawIndex = fixedVertexOrder.empty()
            ? index : (index < fixedVertexOrder.size() ? fixedVertexOrder[index]
                                                       : std::numeric_limits<std::size_t>::max());
        if (rawIndex == std::numeric_limits<std::size_t>::max()) return false;
        const std::size_t base = static_cast<std::size_t>(vertexDataStart) + rawIndex * StudioVertexSize;
        if (!range(*vvd, base, StudioVertexSize)) return false;
        const std::array<float, 3> sourcePosition{
            f32(*vvd, base + 16), f32(*vvd, base + 20), f32(*vvd, base + 24)};
        const std::array<float, 3> sourceNormal{
            f32(*vvd, base + 28), f32(*vvd, base + 32), f32(*vvd, base + 36)};
        std::array<float, 3> sourceTangent{1.0f, 0.0f, 0.0f};
        float tangentSign = 1.0f;
        bool hasTangent = false;
        if (tangentDataStart > 0) {
            const std::size_t tangentBase = static_cast<std::size_t>(tangentDataStart) +
                                            rawIndex * 16u;
            if (range(*vvd, tangentBase, 16u)) {
                sourceTangent = {f32(*vvd, tangentBase), f32(*vvd, tangentBase + 4),
                                 f32(*vvd, tangentBase + 8)};
                tangentSign = f32(*vvd, tangentBase + 12) < 0.0f ? -1.0f : 1.0f;
                const float tangentLengthSquared = sourceTangent[0] * sourceTangent[0] +
                                                   sourceTangent[1] * sourceTangent[1] +
                                                   sourceTangent[2] * sourceTangent[2];
                hasTangent = std::isfinite(tangentLengthSquared) && tangentLengthSquared > 1e-8f;
            }
        }

        std::array<float, 3> skinnedPosition{};
        std::array<float, 3> skinnedNormal{};
        std::array<float, 3> skinnedTangent{};
        vertex.sourcePosition = sourcePosition;
        vertex.sourceNormal = sourceNormal;
        vertex.sourceTangent = sourceTangent;
        float totalWeight = 0.0f;
        const int influenceCount = std::min<int>((*vvd)[base + 15], 3);
        vertex.influenceCount = static_cast<std::uint8_t>(influenceCount);
        for (int influence = 0; influence < influenceCount; ++influence) {
            const float weight = f32(*vvd, base + static_cast<std::size_t>(influence) * 4u);
            const int boneIndex = static_cast<int>((*vvd)[base + 12 + static_cast<std::size_t>(influence)]);
            vertex.boneWeights[static_cast<std::size_t>(influence)] = weight;
            vertex.boneIndices[static_cast<std::size_t>(influence)] = static_cast<std::uint8_t>(boneIndex);
            if (weight <= 0.0f || boneIndex < 0 || boneIndex >= static_cast<int>(bones.size())) continue;
            const Matrix3x4& matrix = bones[static_cast<std::size_t>(boneIndex)].skin;
            const auto position = transformPoint(matrix, sourcePosition);
            const auto normal = rotateVector(matrix, sourceNormal);
            const auto tangent = rotateVector(matrix, sourceTangent);
            for (int component = 0; component < 3; ++component) {
                skinnedPosition[static_cast<std::size_t>(component)] +=
                    position[static_cast<std::size_t>(component)] * weight;
                skinnedNormal[static_cast<std::size_t>(component)] +=
                    normal[static_cast<std::size_t>(component)] * weight;
                if (hasTangent) {
                    skinnedTangent[static_cast<std::size_t>(component)] +=
                        tangent[static_cast<std::size_t>(component)] * weight;
                }
            }
            totalWeight += weight;
        }

        if (totalWeight > 1e-6f) {
            for (float& component : skinnedPosition) component /= totalWeight;
            const float normalLength = std::sqrt(
                skinnedNormal[0] * skinnedNormal[0] +
                skinnedNormal[1] * skinnedNormal[1] +
                skinnedNormal[2] * skinnedNormal[2]);
            if (normalLength > 1e-6f)
                for (float& component : skinnedNormal) component /= normalLength;
        } else {
            skinnedPosition = sourcePosition;
            skinnedNormal = sourceNormal;
            skinnedTangent = sourceTangent;
        }

        if (hasTangent) {
            // Gram-Schmidt the authored tangent against the final skinned
            // normal. This preserves Source's smooth tangent frame through the
            // bind pose while rejecting corrupt/degenerate VVD tangent data.
            const float normalDot = skinnedTangent[0] * skinnedNormal[0] +
                                    skinnedTangent[1] * skinnedNormal[1] +
                                    skinnedTangent[2] * skinnedNormal[2];
            for (int component = 0; component < 3; ++component) {
                skinnedTangent[static_cast<std::size_t>(component)] -=
                    skinnedNormal[static_cast<std::size_t>(component)] * normalDot;
            }
            const float tangentLength = std::sqrt(
                skinnedTangent[0] * skinnedTangent[0] +
                skinnedTangent[1] * skinnedTangent[1] +
                skinnedTangent[2] * skinnedTangent[2]);
            if (tangentLength > 1e-6f && std::isfinite(tangentLength)) {
                for (float& component : skinnedTangent) component /= tangentLength;
            } else {
                hasTangent = false;
            }
        }

        vertex.x = skinnedPosition[0];
        vertex.y = skinnedPosition[1];
        vertex.z = skinnedPosition[2];
        vertex.nx = skinnedNormal[0];
        vertex.ny = skinnedNormal[1];
        vertex.nz = skinnedNormal[2];
        vertex.u = f32(*vvd, base + 40);
        vertex.v = f32(*vvd, base + 44);
        vertex.tx = skinnedTangent[0];
        vertex.ty = skinnedTangent[1];
        vertex.tz = skinnedTangent[2];
        vertex.tangentSign = tangentSign;
        vertex.hasTangent = hasTangent;
        return true;
    };

    for (int bodyPart = 0; bodyPart < bodyPartCount; ++bodyPart) {
        const std::size_t mdlBody = static_cast<std::size_t>(mdlBodyPartIndex) +
                                    static_cast<std::size_t>(bodyPart) * StudioBodyPartSize;
        const std::size_t vtxBody = static_cast<std::size_t>(vtxBodyPartIndex) +
                                    static_cast<std::size_t>(bodyPart) * VtxBodyPartSize;
        if (!range(*mdl, mdlBody, StudioBodyPartSize) || !range(*vtx, vtxBody, VtxBodyPartSize)) continue;
        const int modelCount = std::min(std::max(0, i32(*mdl, mdlBody + 4)),
                                        std::max(0, i32(*vtx, vtxBody)));
        const int mdlModelOffset = i32(*mdl, mdlBody + 12);
        const int vtxModelOffset = i32(*vtx, vtxBody + 4);
        for (int modelIndex = 0; modelIndex < std::min(modelCount, 1); ++modelIndex) {
            const std::size_t mdlModel = mdlBody + static_cast<std::size_t>(mdlModelOffset) +
                                         static_cast<std::size_t>(modelIndex) * StudioModelSize;
            const std::size_t vtxModel = vtxBody + static_cast<std::size_t>(vtxModelOffset) +
                                         static_cast<std::size_t>(modelIndex) * VtxModelSize;
            if (!range(*mdl, mdlModel, StudioModelSize) || !range(*vtx, vtxModel, VtxModelSize)) continue;
            const int meshCount = std::max(0, i32(*mdl, mdlModel + 72));
            const int mdlMeshOffset = i32(*mdl, mdlModel + 76);
            const int modelVertexBase = i32(*mdl, mdlModel + 84) / static_cast<int>(StudioVertexSize);
            const int lodCount = std::max(0, i32(*vtx, vtxModel));
            const int lodOffset = i32(*vtx, vtxModel + 4);
            if (lodCount <= 0) continue;
            const std::size_t lod = vtxModel + static_cast<std::size_t>(lodOffset);
            if (!range(*vtx, lod, VtxLodSize)) continue;
            const int vtxMeshCount = std::max(0, i32(*vtx, lod));
            const int vtxMeshOffset = i32(*vtx, lod + 4);
            for (int meshIndex = 0; meshIndex < std::min(meshCount, vtxMeshCount); ++meshIndex) {
                const std::size_t mdlMesh = mdlModel + static_cast<std::size_t>(mdlMeshOffset) +
                                            static_cast<std::size_t>(meshIndex) * StudioMeshSize;
                const std::size_t vtxMesh = lod + static_cast<std::size_t>(vtxMeshOffset) +
                                            static_cast<std::size_t>(meshIndex) * VtxMeshSize;
                if (!range(*mdl, mdlMesh, StudioMeshSize) || !range(*vtx, vtxMesh, VtxMeshSize)) continue;
                StudioMesh mesh;
                mesh.materialSlot = i32(*mdl, mdlMesh);
                mesh.material = output->materialForSkin(mesh.materialSlot, 0);
                const int meshVertexOffset = i32(*mdl, mdlMesh + 12);
                const int stripGroupCount = std::max(0, i32(*vtx, vtxMesh));
                const int stripGroupOffset = i32(*vtx, vtxMesh + 4);
                for (int groupIndex = 0; groupIndex < stripGroupCount; ++groupIndex) {
                    const std::size_t group = vtxMesh + static_cast<std::size_t>(stripGroupOffset) +
                                              static_cast<std::size_t>(groupIndex) * VtxStripGroupSize;
                    if (!range(*vtx, group, VtxStripGroupSize)) continue;
                    const int optimizedVertexCount = std::max(0, i32(*vtx, group));
                    const int optimizedVertexOffset = i32(*vtx, group + 4);
                    const int indexCount = std::max(0, i32(*vtx, group + 8));
                    const int indexOffset = i32(*vtx, group + 12);
                    const int stripCount = std::max(0, i32(*vtx, group + 16));
                    const int stripOffset = i32(*vtx, group + 20);
                    auto appendIndex = [&](int localIndex) -> bool {
                        if (localIndex < 0 || localIndex >= indexCount) return false;
                        const std::uint16_t optimizedIndex = u16(*vtx, group +
                            static_cast<std::size_t>(indexOffset) + static_cast<std::size_t>(localIndex) * 2u);
                        if (optimizedIndex >= optimizedVertexCount) return false;
                        const std::size_t optimized = group + static_cast<std::size_t>(optimizedVertexOffset) +
                                                      static_cast<std::size_t>(optimizedIndex) * VtxVertexSize;
                        // OptimizedModel::Vertex_t::origMeshVertID maps the
                        // strip-group vertex back into this MDL mesh.
                        const std::uint16_t original = u16(*vtx, optimized + 4);
                        StudioVertex vertex;
                        if (!readVertex(static_cast<std::size_t>(std::max(0, modelVertexBase +
                                               meshVertexOffset + static_cast<int>(original))), vertex)) return false;
                        mesh.vertices.push_back(vertex);
                        expand(*output, vertex, hasBounds);
                        return true;
                    };
                    for (int stripIndex = 0; stripIndex < stripCount; ++stripIndex) {
                        const std::size_t strip = group + static_cast<std::size_t>(stripOffset) +
                                                  static_cast<std::size_t>(stripIndex) * VtxStripSize;
                        if (!range(*vtx, strip, VtxStripSize)) continue;
                        const int stripIndices = std::max(0, i32(*vtx, strip));
                        const int firstIndex = i32(*vtx, strip + 4);
                        const std::uint8_t flags = (*vtx)[strip + 18];
                        if ((flags & 0x01u) != 0) {
                            for (int index = 0; index + 2 < stripIndices; index += 3) {
                                const std::size_t before = mesh.vertices.size();
                                if (!appendIndex(firstIndex + index) ||
                                    !appendIndex(firstIndex + index + 1) ||
                                    !appendIndex(firstIndex + index + 2)) {
                                    mesh.vertices.resize(before);
                                }
                            }
                        } else if ((flags & 0x02u) != 0) {
                            for (int index = 0; index + 2 < stripIndices; ++index) {
                                const int a = firstIndex + index + ((index & 1) ? 1 : 0);
                                const int b = firstIndex + index + ((index & 1) ? 0 : 1);
                                const int c = firstIndex + index + 2;
                                const std::size_t before = mesh.vertices.size();
                                if (!appendIndex(a) || !appendIndex(b) || !appendIndex(c))
                                    mesh.vertices.resize(before);
                            }
                        }
                    }
                }
                if (!mesh.vertices.empty()) output->meshes.push_back(std::move(mesh));
            }
        }
    }

    // Re-apply sequence zero through the same general evaluator used at
    // runtime. Besides keeping the reference mesh and animated path identical,
    // this also handles reference poses stored in an external .ani block. The
    // older inline frame-zero reconstruction above remains the safe fallback
    // for malformed or unsupported animation data.
    if (output->animationData && !output->sequences.empty() && !output->meshes.empty() &&
        !output->animationData->sequences.empty() &&
        output->animationData->sequences.front().sourceIndex == 0u &&
        output->animationData->sequences.front().localSequence == 0) {
        std::vector<Matrix3x4> referenceMatrices;
        if (evaluateSequence(*output->animationData, 0, 0.0, referenceMatrices)) {
            output->referencePoseMatrices = publicMatrices(referenceMatrices);
            hasBounds = false;
            for (StudioMesh& mesh : output->meshes) {
                for (StudioVertex& vertex : mesh.vertices) {
                    skinStudioVertex(vertex, referenceMatrices);
                    expand(*output, vertex, hasBounds);
                }
            }
        }
    }

    output->valid = !output->meshes.empty();
    if (!output->valid && output->error.empty())
        output->error = "No renderable bind-pose meshes in " + modelPath;
    return output;
}

} // namespace hammer::assets
