#include "MaterialSystem.hpp"

#include "VmfDocument.hpp"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace hammer::assets {
namespace {
std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool iequals(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

bool endsWithCi(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           iequals(text.substr(text.size() - suffix.size()), suffix);
}

const std::string* valueCi(const hammer::vmf::Block& block, std::string_view key)
{
    for (const auto& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::KeyValue && iequals(entry.key, key)) return &entry.value;
    }
    return nullptr;
}

const std::string* nestedValueCi(const hammer::vmf::Block& block, std::string_view key)
{
    if (const std::string* value = valueCi(block, key)) return value;
    for (const auto& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::ChildBlock && entry.child) {
            if (const std::string* value = nestedValueCi(*entry.child, key)) return value;
        }
    }
    return nullptr;
}

std::string normalizeMaterialName(std::string_view input)
{
    std::string name = GameFileSystem::normalizeResourcePath(input);
    if (name.rfind("materials/", 0) == 0) name.erase(0, 10);
    if (name.size() >= 4 && (name.substr(name.size() - 4) == ".vmt" ||
                              name.substr(name.size() - 4) == ".vtf")) {
        name.resize(name.size() - 4);
    }
    return name;
}


bool isTf2InvulnerabilityMaterial(std::string_view materialName)
{
    // Classify only the logical VMT basename. A raw substring search on the
    // complete identifier can accidentally match a mounted directory such as
    // ".../uber-texture-suppression/..." and turn every material into the
    // invulnerability override. TF2's stock forced materials are named
    // invulnfx_*; custom materials can opt into the same path by using that
    // basename convention.
    const std::string normalized = lower(normalizeMaterialName(materialName));
    const std::size_t slash = normalized.find_last_of('/');
    const std::string_view basename = slash == std::string::npos
        ? std::string_view(normalized)
        : std::string_view(normalized).substr(slash + 1);
    return basename == "invulnfx" || basename.rfind("invulnfx_", 0) == 0;
}


struct EditorMaterialEffectSupport
{
    bool bumpMap{false};
    bool phong{false};
    bool specular{false};
    bool selfIllum{false};
    bool rimLight{false};
};

EditorMaterialEffectSupport editorMaterialEffectSupport(std::string_view shader)
{
    const std::string normalized = lower(shader);
    // These are the Source 1 shaders whose inputs map cleanly onto the
    // editor's deliberately small preview shader. EyeRefract, Teeth,
    // UnlitGeneric, custom character shaders, and other special-purpose
    // materials must not be fed through the generic normal/specular path.
    if (normalized == "vertexlitgeneric") return {true, true, true, true, true};
    // Brush materials use the same editor approximation in the dedicated
    // materials-polygon mode. LightmappedGeneric and WorldVertexTransition
    // expose the normal, phong, and envmap inputs needed by that preview.
    if (normalized == "lightmappedgeneric") return {true, true, true, false, false};
    if (normalized == "worldvertextransition") return {true, true, true, false, false};
    return {};
}

bool hiddenFromTextureBrowser(std::string_view input)
{
    const std::string name = normalizeMaterialName(input);
    // Model skins, VGUI artwork, and backpack inventory icons are not world
    // textures. Excluding them at index time also prevents the visual browser
    // from attempting thousands of irrelevant VMT/VTF thumbnail loads.
    return name.rfind("models/", 0) == 0 ||
           name.rfind("vgui/", 0) == 0 ||
           name.rfind("backpack/", 0) == 0;
}

std::uint16_t u16(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    if (offset + 2 > data.size()) return 0;
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

std::uint32_t u32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    if (offset + 4 > data.size()) return 0;
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

std::size_t imageSize(std::uint32_t format, int width, int height)
{
    const std::size_t pixels = static_cast<std::size_t>(std::max(1, width)) * std::max(1, height);
    switch (format) {
    case 0: case 1: case 11: case 12: case 16: return pixels * 4;
    case 2: case 3: case 9: case 10: return pixels * 3;
    case 4: case 6: case 17: case 18: case 19: case 21: case 22: return pixels * 2;
    case 5: case 8: return pixels;
    case 13: case 20: return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 8;
    case 14: case 15: return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 16;
    default: return 0;
    }
}

std::uint32_t argb(int r, int g, int b, int a = 255)
{
    return (static_cast<std::uint32_t>(a & 255) << 24) |
           (static_cast<std::uint32_t>(r & 255) << 16) |
           (static_cast<std::uint32_t>(g & 255) << 8) |
           static_cast<std::uint32_t>(b & 255);
}

std::array<int, 3> rgb565(std::uint16_t value)
{
    const int r = ((value >> 11) & 31) * 255 / 31;
    const int g = ((value >> 5) & 63) * 255 / 63;
    const int b = (value & 31) * 255 / 31;
    return {r, g, b};
}

void writePixel(Image& image, int x, int y, std::uint32_t color)
{
    if (x >= 0 && y >= 0 && x < image.width && y < image.height) {
        image.pixels[static_cast<std::size_t>(y * image.width + x)] = color;
    }
}

bool decodeDxt1(const std::uint8_t* src, std::size_t size, Image& image, bool oneBitAlpha)
{
    const int blocksX = (image.width + 3) / 4;
    const int blocksY = (image.height + 3) / 4;
    if (size < static_cast<std::size_t>(blocksX * blocksY * 8)) return false;
    for (int by = 0; by < blocksY; ++by) for (int bx = 0; bx < blocksX; ++bx) {
        const std::uint8_t* block = src + static_cast<std::size_t>((by * blocksX + bx) * 8);
        const std::uint16_t c0 = block[0] | (block[1] << 8);
        const std::uint16_t c1 = block[2] | (block[3] << 8);
        const auto a = rgb565(c0), b = rgb565(c1);
        std::array<std::uint32_t, 4> colors{argb(a[0], a[1], a[2]), argb(b[0], b[1], b[2]), 0, 0};
        if (c0 > c1 || !oneBitAlpha) {
            colors[2] = argb((2 * a[0] + b[0]) / 3, (2 * a[1] + b[1]) / 3, (2 * a[2] + b[2]) / 3);
            colors[3] = argb((a[0] + 2 * b[0]) / 3, (a[1] + 2 * b[1]) / 3, (a[2] + 2 * b[2]) / 3);
        } else {
            colors[2] = argb((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2);
            colors[3] = 0;
        }
        const std::uint32_t indices = u32(std::vector<std::uint8_t>(block + 4, block + 8), 0);
        for (int py = 0; py < 4; ++py) for (int px = 0; px < 4; ++px) {
            const int index = (indices >> (2 * (py * 4 + px))) & 3;
            writePixel(image, bx * 4 + px, by * 4 + py, colors[index]);
        }
    }
    return true;
}

bool decodeDxt3(const std::uint8_t* src, std::size_t size, Image& image)
{
    const int blocksX = (image.width + 3) / 4;
    const int blocksY = (image.height + 3) / 4;
    if (size < static_cast<std::size_t>(blocksX * blocksY * 16)) return false;
    for (int by = 0; by < blocksY; ++by) for (int bx = 0; bx < blocksX; ++bx) {
        const std::uint8_t* block = src + static_cast<std::size_t>((by * blocksX + bx) * 16);
        std::uint64_t alphaBits = 0;
        for (int i = 0; i < 8; ++i) alphaBits |= static_cast<std::uint64_t>(block[i]) << (8 * i);
        const std::uint16_t c0 = block[8] | (block[9] << 8), c1 = block[10] | (block[11] << 8);
        const auto a = rgb565(c0), b = rgb565(c1);
        std::array<std::array<int, 3>, 4> colors{a, b,
            std::array<int, 3>{(2*a[0]+b[0])/3,(2*a[1]+b[1])/3,(2*a[2]+b[2])/3},
            std::array<int, 3>{(a[0]+2*b[0])/3,(a[1]+2*b[1])/3,(a[2]+2*b[2])/3}};
        std::uint32_t indices = block[12] | (block[13]<<8) | (block[14]<<16) | (block[15]<<24);
        for (int i = 0; i < 16; ++i) {
            const int ci = (indices >> (2*i)) & 3;
            const int alpha = static_cast<int>((alphaBits >> (4*i)) & 15) * 17;
            writePixel(image, bx*4+(i%4), by*4+(i/4), argb(colors[ci][0], colors[ci][1], colors[ci][2], alpha));
        }
    }
    return true;
}

bool decodeDxt5(const std::uint8_t* src, std::size_t size, Image& image)
{
    const int blocksX = (image.width + 3) / 4;
    const int blocksY = (image.height + 3) / 4;
    if (size < static_cast<std::size_t>(blocksX * blocksY * 16)) return false;
    for (int by = 0; by < blocksY; ++by) for (int bx = 0; bx < blocksX; ++bx) {
        const std::uint8_t* block = src + static_cast<std::size_t>((by * blocksX + bx) * 16);
        std::array<int, 8> alpha{};
        alpha[0] = block[0]; alpha[1] = block[1];
        if (alpha[0] > alpha[1]) {
            for (int i = 1; i <= 6; ++i) alpha[i+1] = ((7-i)*alpha[0] + i*alpha[1]) / 7;
        } else {
            for (int i = 1; i <= 4; ++i) alpha[i+1] = ((5-i)*alpha[0] + i*alpha[1]) / 5;
            alpha[6] = 0; alpha[7] = 255;
        }
        std::uint64_t alphaIndices = 0;
        for (int i = 0; i < 6; ++i) alphaIndices |= static_cast<std::uint64_t>(block[2+i]) << (8*i);
        const std::uint16_t c0 = block[8] | (block[9] << 8), c1 = block[10] | (block[11] << 8);
        const auto a = rgb565(c0), b = rgb565(c1);
        std::array<std::array<int, 3>, 4> colors{a, b,
            std::array<int, 3>{(2*a[0]+b[0])/3,(2*a[1]+b[1])/3,(2*a[2]+b[2])/3},
            std::array<int, 3>{(a[0]+2*b[0])/3,(a[1]+2*b[1])/3,(a[2]+2*b[2])/3}};
        std::uint32_t indices = block[12] | (block[13]<<8) | (block[14]<<16) | (block[15]<<24);
        for (int i = 0; i < 16; ++i) {
            const int ci = (indices >> (2*i)) & 3;
            const int ai = static_cast<int>((alphaIndices >> (3*i)) & 7);
            writePixel(image, bx*4+(i%4), by*4+(i/4), argb(colors[ci][0], colors[ci][1], colors[ci][2], alpha[ai]));
        }
    }
    return true;
}

Image checkerboard()
{
    Image image{64, 64, std::vector<std::uint32_t>(64 * 64)};
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const bool bright = ((x / 8) ^ (y / 8)) & 1;
        image.pixels[static_cast<std::size_t>(y * 64 + x)] = bright ? argb(235, 60, 235) : argb(35, 35, 35);
    }
    return image;
}

std::array<int, 3> parseMaterialColor(const std::string* value,
                                      std::array<int, 3> fallback = {44, 88, 104})
{
    if (!value || value->empty()) return fallback;
    std::array<double, 3> components{};
    int count = 0;
    const char* cursor = value->c_str();
    while (*cursor && count < 3) {
        while (*cursor && !(std::isdigit(static_cast<unsigned char>(*cursor)) ||
                            *cursor == '-' || *cursor == '+' || *cursor == '.')) {
            ++cursor;
        }
        if (!*cursor) break;
        char* end = nullptr;
        components[count] = std::strtod(cursor, &end);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        cursor = end;
        ++count;
    }
    if (count < 3) return fallback;
    const double maximum = std::max({std::abs(components[0]), std::abs(components[1]),
                                     std::abs(components[2])});
    const double scale = maximum <= 1.001 ? 255.0 : 1.0;
    for (int i = 0; i < 3; ++i) {
        fallback[i] = std::clamp(static_cast<int>(std::lround(components[i] * scale)), 0, 255);
    }
    return fallback;
}

std::array<float, 3> normalizedColor(const std::array<int, 3>& color)
{
    return {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f};
}

float parseMaterialFloat(const std::string* value, float fallback)
{
    if (!value) return fallback;
    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(*value, &consumed);
        return consumed == 0 || !std::isfinite(parsed) ? fallback : parsed;
    } catch (...) {
        return fallback;
    }
}

std::array<float, 2> parseMaterialVector2(
    const std::string* value, std::array<float, 2> fallback)
{
    if (!value) return fallback;
    const char* cursor = value->c_str();
    int count = 0;
    while (*cursor && count < 2) {
        while (*cursor && !(std::isdigit(static_cast<unsigned char>(*cursor)) ||
                            *cursor == '-' || *cursor == '+' || *cursor == '.')) {
            ++cursor;
        }
        if (!*cursor) break;
        char* end = nullptr;
        const float parsed = std::strtof(cursor, &end);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        if (std::isfinite(parsed)) fallback[static_cast<std::size_t>(count)] = parsed;
        cursor = end;
        ++count;
    }
    return fallback;
}

std::array<float, 3> parseMaterialVector3(
    const std::string* value, std::array<float, 3> fallback)
{
    if (!value) return fallback;
    const char* cursor = value->c_str();
    int count = 0;
    while (*cursor && count < 3) {
        while (*cursor && !(std::isdigit(static_cast<unsigned char>(*cursor)) ||
                            *cursor == '-' || *cursor == '+' || *cursor == '.')) {
            ++cursor;
        }
        if (!*cursor) break;
        char* end = nullptr;
        const float parsed = std::strtof(cursor, &end);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        if (std::isfinite(parsed)) fallback[static_cast<std::size_t>(count)] = parsed;
        cursor = end;
        ++count;
    }
    return fallback;
}

bool parseMaterialBool(const std::string* value, bool fallback = false)
{
    if (!value) return fallback;
    const std::string normalized = lower(*value);
    if (normalized == "true" || normalized == "yes" || normalized == "on") return true;
    if (normalized == "false" || normalized == "no" || normalized == "off") return false;
    return std::abs(parseMaterialFloat(value, fallback ? 1.0f : 0.0f)) >= 0.5f;
}

std::array<float, 3> materialTintVector(
    const std::string* value, std::array<float, 3> fallback)
{
    auto result = parseMaterialVector3(value, fallback);
    const float maximum = std::max({std::abs(result[0]), std::abs(result[1]),
                                    std::abs(result[2])});
    // KeyValues colour literals are commonly written as either [0..1] vectors
    // or {0..255} colours. Preserve HDR-scale vectors while normalising the
    // unmistakable byte-colour form.
    if (maximum > 16.0f) {
        for (float& component : result) component /= 255.0f;
    }
    for (float& component : result) component = std::clamp(component, 0.0f, 16.0f);
    return result;
}

std::string proxyVariableName(std::string_view input)
{
    std::size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    std::size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return lower(input.substr(begin, end - begin));
}

struct ProxyTarget
{
    std::string name;
    int component{-1};
};

ProxyTarget proxyTarget(std::string_view input)
{
    ProxyTarget result;
    const std::string normalized = proxyVariableName(input);
    const std::size_t bracket = normalized.find('[');
    if (bracket == std::string::npos) {
        result.name = normalized;
        return result;
    }
    result.name = normalized.substr(0, bracket);
    const std::size_t close = normalized.find(']', bracket + 1);
    if (close != std::string::npos) {
        try {
            const int parsed = std::stoi(normalized.substr(bracket + 1, close - bracket - 1));
            if (parsed >= 0 && parsed < 3) result.component = parsed;
        } catch (...) {}
    }
    return result;
}

using ProxyValue = std::array<float, 3>;

struct ProxyDatum
{
    ProxyValue value{0.0f, 0.0f, 0.0f};
    bool known{false};
};

using ProxyVariables = std::unordered_map<std::string, ProxyDatum>;

bool proxyValueNonZero(const ProxyValue& value)
{
    return std::abs(value[0]) > 1e-6f || std::abs(value[1]) > 1e-6f ||
           std::abs(value[2]) > 1e-6f;
}

ProxyDatum proxyValue(const std::string* expression,
                      const ProxyVariables& variables)
{
    if (!expression) return {};
    const std::string normalized = proxyVariableName(*expression);
    if (!normalized.empty() && normalized.front() == '$') {
        if (const auto found = variables.find(normalized); found != variables.end())
            return found->second;
        return {};
    }

    // Material proxies accept scalar literals as well as vectors. Broadcast a
    // lone scalar so Multiply behaves like Source's vector material variables.
    const char* cursor = expression->c_str();
    ProxyValue parsed{0.0f, 0.0f, 0.0f};
    int count = 0;
    while (*cursor && count < 3) {
        while (*cursor && !(std::isdigit(static_cast<unsigned char>(*cursor)) ||
                            *cursor == '-' || *cursor == '+' || *cursor == '.')) ++cursor;
        if (!*cursor) break;
        char* tail = nullptr;
        const float component = std::strtof(cursor, &tail);
        if (tail == cursor) { ++cursor; continue; }
        if (std::isfinite(component)) parsed[static_cast<std::size_t>(count)] = component;
        cursor = tail;
        ++count;
    }
    if (count == 0) return {};
    if (count == 1) parsed = {parsed[0], parsed[0], parsed[0]};
    // Proxy variables are untyped numeric material variables, not implicitly
    // colours. Values such as $invulnfmax=18 and -31 must remain literal; tint
    // normalization is performed only when a result is finally assigned to a
    // colour parameter such as $color2 or $selfillumtint.
    for (float& component : parsed) component = std::clamp(component, -64.0f, 64.0f);
    return {parsed, true};
}

void collectBlocksCi(const hammer::vmf::Block& block, std::string_view name,
                     std::vector<const hammer::vmf::Block*>& output)
{
    for (const auto& entry : block.entries) {
        if (entry.kind != hammer::vmf::Entry::Kind::ChildBlock || !entry.child) continue;
        if (iequals(entry.child->name, name)) output.push_back(entry.child.get());
        collectBlocksCi(*entry.child, name, output);
    }
}

ProxyValue inferredModelGlowColor(std::string_view materialName,
                                  const ProxyVariables& variables,
                                  const Material& material)
{
    if (const auto found = variables.find("$glowcolor");
        found != variables.end() && found->second.known &&
        proxyValueNonZero(found->second.value)) {
        const auto& glow = found->second.value;
        const float minimum = std::min({glow[0], glow[1], glow[2]});
        const float maximum = std::max({glow[0], glow[1], glow[2]});
        // Stock TF2 materials initialize $glowcolor to scalar 1 only as a
        // runtime-proxy destination. Do not mistake that neutral placeholder
        // for the final white team colour; authored chromatic values remain valid.
        if (maximum - minimum > 0.02f) return glow;
    }
    const std::string name = lower(std::string(materialName) + " " +
                                   material.baseTexture + " " + material.envMap);
    if (name.find("blue") != std::string::npos ||
        name.find("_blu") != std::string::npos ||
        name.find("/blu") != std::string::npos) {
        return {0.12f, 0.38f, 1.0f};
    }
    if (name.find("red") != std::string::npos) return {1.0f, 0.10f, 0.06f};

    if (material.color2Active && proxyValueNonZero(material.color2) &&
        (std::abs(material.color2[0] - 1.0f) > 1e-6f ||
         std::abs(material.color2[1] - 1.0f) > 1e-6f ||
         std::abs(material.color2[2] - 1.0f) > 1e-6f)) {
        return material.color2;
    }
    return {1.0f, 1.0f, 1.0f};
}

void evaluateMaterialProxies(const hammer::vmf::Block& root,
                             std::string_view materialName,
                             Material& material)
{
    ProxyVariables variables;

    // Material proxies may reference arbitrary scalar/vector VMT variables.
    // Seed every numeric top-level $variable, while texture/string values stay
    // unresolved because proxyValue intentionally refuses non-numeric text.
    for (const auto& entry : root.entries) {
        if (entry.kind != hammer::vmf::Entry::Kind::KeyValue || entry.key.empty() ||
            entry.key.front() != '$') continue;
        const ProxyDatum parsed = proxyValue(&entry.value, variables);
        if (parsed.known) variables[proxyVariableName(entry.key)] = parsed;
    }
    if (material.color2Active)
        variables["$color2"] = {material.color2, true};
    if (const std::string* value = valueCi(root, "$selfillumtint")) {
        variables["$selfillumtint"] = {
            materialTintVector(value, material.selfIllumTint), true};
    }

    bool color2ProxyTargeted = false;
    bool color2ProxyResolved = false;
    bool selfIllumProxyTargeted = false;
    bool selfIllumProxyResolved = false;

    auto storeResult = [&](const ProxyTarget& target, ProxyDatum result) {
        if (target.name.empty() || target.name.front() != '$' || !result.known) return;
        for (float& component : result.value)
            component = std::clamp(component, -64.0f, 64.0f);
        if (target.component >= 0) {
            ProxyDatum& destination = variables[target.name];
            if (!destination.known) {
                destination.value = {0.0f, 0.0f, 0.0f};
                destination.known = true;
            }
            destination.value[static_cast<std::size_t>(target.component)] = result.value[0];
        } else {
            variables[target.name] = result;
        }
        if (target.name == "$color2") color2ProxyResolved = true;
        if (target.name == "$selfillumtint") selfIllumProxyResolved = true;
    };

    auto invalidateResult = [&](const ProxyTarget& target) {
        if (target.name.empty() || target.name.front() != '$') return;
        // A runtime proxy overwrites its destination on every bind. If the
        // editor cannot evaluate that proxy, the authored initializer is not a
        // valid substitute for the runtime result. Mark it unknown so later
        // Equals/Multiply proxies cannot accidentally consume a stale zero.
        // TF2 player materials use this exact chain for YellowLevel -> $yellow
        // -> $color2; retaining the initializer made the whole model black.
        variables[target.name] = {};
        if (target.name == "$color2") color2ProxyResolved = false;
        if (target.name == "$selfillumtint") selfIllumProxyResolved = false;
    };

    std::vector<const hammer::vmf::Block*> proxyContainers;
    collectBlocksCi(root, "proxies", proxyContainers);
    for (const hammer::vmf::Block* container : proxyContainers) {
        for (const auto& entry : container->entries) {
            if (entry.kind != hammer::vmf::Entry::Kind::ChildBlock || !entry.child) continue;
            const hammer::vmf::Block& proxy = *entry.child;
            const std::string type = lower(proxy.name);

            if (type == "animatedtexture") {
                const std::string* animatedVariable = valueCi(proxy, "animatedtexturevar");
                const std::string variable = animatedVariable
                    ? proxyVariableName(*animatedVariable) : std::string{};
                if (variable == "$bumpmap" || variable == "$normalmap") {
                    material.bumpAnimationFrameRate = std::clamp(
                        parseMaterialFloat(valueCi(proxy, "animatedtextureframerate"), 0.0f),
                        0.0f, 240.0f);
                    material.previewAnimated = material.bumpAnimationFrameRate > 0.0f;
                }
                continue;
            }

            const std::string* resultNameValue = valueCi(proxy, "resultvar");
            if (!resultNameValue) continue;
            const ProxyTarget target = proxyTarget(*resultNameValue);
            if (target.name.empty() || target.name.front() != '$') continue;

            if (target.name == "$color2") color2ProxyTargeted = true;
            if (target.name == "$selfillumtint") selfIllumProxyTargeted = true;

            ProxyDatum result;
            bool handled = true;
            if (type == "modelglowcolor") {
                result = {inferredModelGlowColor(materialName, variables, material), true};
                material.modelGlowProxy = true;
            } else if (type == "itemtintcolor") {
                material.itemTintProxy = true;
                if (const auto found = variables.find("$itemtintcolor");
                    found != variables.end() && found->second.known &&
                    proxyValueNonZero(found->second.value)) {
                    result = found->second;
                } else if (const auto found = variables.find("$colortint_base");
                           found != variables.end() && found->second.known) {
                    result = found->second;
                } else if (material.color2Active) {
                    result = {material.color2, true};
                }
            } else if (type == "invulnlevel") {
                // The editor previews the stable, fully-invulnerable state. In
                // TF2 the runtime proxy drives this through the fade transition.
                result = {{1.0f, 1.0f, 1.0f}, true};
                material.invulnLevelProxy = true;
            } else if (type == "equals") {
                result = proxyValue(valueCi(proxy, "srcvar1"), variables);
            } else if (type == "multiply") {
                const ProxyDatum a = proxyValue(valueCi(proxy, "srcvar1"), variables);
                const ProxyDatum b = proxyValue(valueCi(proxy, "srcvar2"), variables);
                if (a.known && b.known) {
                    result = {{a.value[0] * b.value[0],
                               a.value[1] * b.value[1],
                               a.value[2] * b.value[2]}, true};
                }
            } else if (type == "selectfirstifnonzero") {
                const ProxyDatum first = proxyValue(valueCi(proxy, "srcvar1"), variables);
                const ProxyDatum second = proxyValue(valueCi(proxy, "srcvar2"), variables);
                if (first.known && proxyValueNonZero(first.value)) result = first;
                else if (second.known) result = second;
            } else if (type == "lessorequal") {
                const ProxyDatum a = proxyValue(valueCi(proxy, "srcvar1"), variables);
                const ProxyDatum b = proxyValue(valueCi(proxy, "srcvar2"), variables);
                const std::string* branch = nullptr;
                if (a.known && b.known) {
                    branch = valueCi(proxy, a.value[0] <= b.value[0]
                        ? "lessequalvar" : "greatervar");
                    result = proxyValue(branch, variables);
                }
            } else if (type == "sine") {
                const ProxyDatum minimum = proxyValue(valueCi(proxy, "sinemin"), variables);
                const ProxyDatum maximum = proxyValue(valueCi(proxy, "sinemax"), variables);
                if (minimum.known && maximum.known) {
                    const float period = std::max(0.0001f,
                        std::abs(parseMaterialFloat(valueCi(proxy, "sineperiod"), 1.0f)));
                    // Sample a quarter-period: this is the bright point of the
                    // preview pulse, and equals the stable full-state values in
                    // TF2's stock invulnerability VMTs.
                    const float phase = 0.25f * period;
                    const float wave = 0.5f + 0.5f * std::sin(
                        6.28318530718f * phase / period);
                    result = {{minimum.value[0] + (maximum.value[0] - minimum.value[0]) * wave,
                               minimum.value[1] + (maximum.value[1] - minimum.value[1]) * wave,
                               minimum.value[2] + (maximum.value[2] - minimum.value[2]) * wave}, true};
                }
            } else {
                handled = false;
            }
            if (handled && result.known) storeResult(target, result);
            else invalidateResult(target);
        }
    }

    if (color2ProxyTargeted && !color2ProxyResolved) {
        material.color2 = {1.0f, 1.0f, 1.0f};
        material.color2Active = false;
    } else if (const auto found = variables.find("$color2");
               found != variables.end() && found->second.known) {
        material.color2 = found->second.value;
        const float maximum = std::max({std::abs(material.color2[0]),
                                        std::abs(material.color2[1]),
                                        std::abs(material.color2[2])});
        if (maximum > 16.0f)
            for (float& component : material.color2) component /= 255.0f;
        for (float& component : material.color2)
            component = std::clamp(component, 0.0f, 16.0f);
        material.color2Active = true;
    }

    if (selfIllumProxyTargeted && !selfIllumProxyResolved) {
        material.selfIllumTint = {1.0f, 1.0f, 1.0f};
    } else if (const auto found = variables.find("$selfillumtint");
               found != variables.end() && found->second.known) {
        material.selfIllumTint = found->second.value;
        const float maximum = std::max({std::abs(material.selfIllumTint[0]),
                                        std::abs(material.selfIllumTint[1]),
                                        std::abs(material.selfIllumTint[2])});
        if (maximum > 16.0f)
            for (float& component : material.selfIllumTint) component /= 255.0f;
        for (float& component : material.selfIllumTint)
            component = std::clamp(component, 0.0f, 16.0f);
    }
    if (const auto found = variables.find("$selfillumfresnelminmaxexp");
        found != variables.end() && found->second.known) {
        material.selfIllumFresnelMinMaxExp = found->second.value;
        material.selfIllumFresnelMinMaxExp[2] = std::clamp(
            material.selfIllumFresnelMinMaxExp[2], 0.01f, 128.0f);
    }
    if (const auto found = variables.find("$phongboost");
        found != variables.end() && found->second.known) {
        material.phongBoost = std::clamp(found->second.value[0], 0.0f, 16.0f);
    }

    // Preserve the stable 0.14.22 distinction between an authored forced
    // invulnerability material and an ordinary player material that merely
    // exposes runtime invulnerability/glow proxy variables. Player VMTs often
    // contain InvulnLevel or ModelGlowColor proxies even while the runtime value
    // is zero; those proxies must not replace the base albedo in a static editor.
    material.uberEffect = isTf2InvulnerabilityMaterial(materialName);
    // Only runtime glow/invulnerability materials bypass the editor's generic
    // highlight compressor. Self-illumination Fresnel by itself is a normal
    // VertexLitGeneric feature and must not change unrelated materials globally.
    material.highEnergyEffect = material.modelGlowProxy || material.uberEffect;
}

Image prepareWaterNormalMap(const Image& source)
{
    if (!source.valid()) return {};
    Image result{source.width, source.height,
                 std::vector<std::uint32_t>(source.pixels.size())};

    // Source commonly stores normal X in DXT5 alpha (DXT5nm) and Y in green.
    // Use alpha whenever it contains real variation; otherwise use red for
    // ordinary RGB normal maps. Reconstruct Z so every texel is normalized.
    int alphaMinimum = 255;
    int alphaMaximum = 0;
    for (const std::uint32_t pixel : source.pixels) {
        const int alpha = static_cast<int>((pixel >> 24) & 255u);
        alphaMinimum = std::min(alphaMinimum, alpha);
        alphaMaximum = std::max(alphaMaximum, alpha);
    }
    const bool alphaCarriesX = alphaMaximum - alphaMinimum > 8;
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        const std::uint32_t pixel = source.pixels[index];
        const int encodedX = alphaCarriesX
            ? static_cast<int>((pixel >> 24) & 255u)
            : static_cast<int>((pixel >> 16) & 255u);
        const int encodedY = static_cast<int>((pixel >> 8) & 255u);
        double nx = encodedX / 127.5 - 1.0;
        double ny = encodedY / 127.5 - 1.0;
        const double xyLengthSquared = nx * nx + ny * ny;
        if (xyLengthSquared > 1.0) {
            const double inverseLength = 1.0 / std::sqrt(xyLengthSquared);
            nx *= inverseLength;
            ny *= inverseLength;
        }
        const double nz = std::sqrt(std::max(0.0, 1.0 - nx * nx - ny * ny));
        const int r = std::clamp(static_cast<int>(std::lround((nx * 0.5 + 0.5) * 255.0)), 0, 255);
        const int g = std::clamp(static_cast<int>(std::lround((ny * 0.5 + 0.5) * 255.0)), 0, 255);
        const int b = std::clamp(static_cast<int>(std::lround((nz * 0.5 + 0.5) * 255.0)), 0, 255);
        result.pixels[index] = argb(r, g, b, 255);
    }
    return result;
}

Image prepareEditorNormalMap(const Image& source)
{
    if (!source.valid()) return {};
    Image result{source.width, source.height,
                 std::vector<std::uint32_t>(source.pixels.size())};
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        const std::uint32_t pixel = source.pixels[index];
        const int alpha = static_cast<int>((pixel >> 24) & 255u);
        double nx = static_cast<int>((pixel >> 16) & 255u) / 127.5 - 1.0;
        double ny = static_cast<int>((pixel >> 8) & 255u) / 127.5 - 1.0;
        double nz = static_cast<int>(pixel & 255u) / 127.5 - 1.0;
        double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!std::isfinite(length) || length < 1e-6) {
            nx = 0.0; ny = 0.0; nz = 1.0; length = 1.0;
        }
        nx /= length; ny /= length; nz /= length;
        const int r = std::clamp(static_cast<int>(std::lround((nx * 0.5 + 0.5) * 255.0)), 0, 255);
        const int g = std::clamp(static_cast<int>(std::lround((ny * 0.5 + 0.5) * 255.0)), 0, 255);
        const int b = std::clamp(static_cast<int>(std::lround((nz * 0.5 + 0.5) * 255.0)), 0, 255);
        // Unlike water's DXT5nm conversion, ordinary Source model normal maps
        // keep alpha intact because VertexLitGeneric uses it as the Phong mask.
        result.pixels[index] = argb(r, g, b, alpha);
    }
    return result;
}

Image makeProceduralWaterNormal()
{
    constexpr int width = 128;
    constexpr int height = 128;
    Image image{width, height, std::vector<std::uint32_t>(width * height)};
    auto heightAt = [](double x, double y) {
        return std::sin(x * 0.19 + y * 0.08) * 0.55 +
               std::cos(x * 0.07 - y * 0.16) * 0.32 +
               std::sin((x + y) * 0.035) * 0.20;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double dx = heightAt(x + 1.0, y) - heightAt(x - 1.0, y);
            const double dy = heightAt(x, y + 1.0) - heightAt(x, y - 1.0);
            double nx = -dx * 1.65;
            double ny = -dy * 1.65;
            double nz = 1.0;
            const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= length; ny /= length; nz /= length;
            const int r = std::clamp(static_cast<int>(std::lround((nx * 0.5 + 0.5) * 255.0)), 0, 255);
            const int g = std::clamp(static_cast<int>(std::lround((ny * 0.5 + 0.5) * 255.0)), 0, 255);
            const int b = std::clamp(static_cast<int>(std::lround((nz * 0.5 + 0.5) * 255.0)), 0, 255);
            image.pixels[static_cast<std::size_t>(y * width + x)] = argb(r, g, b, 255);
        }
    }
    return image;
}

Image makeWaterPreview(const Image* normalMap,
                       const std::array<int, 3>& tint,
                       const std::array<int, 3>& reflectionTint,
                       float materialAlpha)
{
    const int width = normalMap && normalMap->valid() ? normalMap->width : 128;
    const int height = normalMap && normalMap->valid() ? normalMap->height : 128;
    Image image{width, height, std::vector<std::uint32_t>(static_cast<std::size_t>(width * height))};
    const int alpha = std::clamp(static_cast<int>(std::lround(materialAlpha * 255.0f)), 52, 245);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double waveX = 0.0;
            double waveY = 0.0;
            double upward = 1.0;
            if (normalMap && normalMap->valid()) {
                const std::uint32_t source = normalMap->pixels[static_cast<std::size_t>(y * width + x)];
                waveX = (static_cast<double>((source >> 16) & 255u) / 255.0) * 2.0 - 1.0;
                waveY = (static_cast<double>((source >> 8) & 255u) / 255.0) * 2.0 - 1.0;
                upward = (static_cast<double>(source & 255u) / 255.0) * 2.0 - 1.0;
            }

            // Hammer's material browser represents water as its VMT fog colour
            // with a restrained fake sky sheen. Keep that colour dominant;
            // normals should break up the sheen, not repaint the whole tile.
            const double slope = std::clamp(1.0 - upward, 0.0, 1.0);
            const double crest = std::clamp(waveX * 0.55 + waveY * 0.45, -1.0, 1.0);
            const double reflection = std::clamp(0.075 + slope * 0.16 + std::max(0.0, crest) * 0.035,
                                                 0.06, 0.27);
            const double baseShade = std::clamp(0.965 + crest * 0.025, 0.92, 1.02);
            const double horizon = std::clamp(0.74 + slope * 0.16, 0.0, 1.0);
            const int r = std::clamp(static_cast<int>(std::lround(
                tint[0] * baseShade * (1.0 - reflection) + reflectionTint[0] * horizon * reflection)), 0, 255);
            const int g = std::clamp(static_cast<int>(std::lround(
                tint[1] * baseShade * (1.0 - reflection) + reflectionTint[1] * horizon * reflection)), 0, 255);
            const int b = std::clamp(static_cast<int>(std::lround(
                tint[2] * baseShade * (1.0 - reflection) + reflectionTint[2] * horizon * reflection)), 0, 255);
            image.pixels[static_cast<std::size_t>(y * width + x)] = argb(r, g, b, alpha);
        }
    }
    return image;
}

bool isWaterShader(std::string_view shader)
{
    const std::string normalized = lower(shader);
    return normalized == "water" || normalized.rfind("water_", 0) == 0;
}
} // namespace

std::array<float, 3> previewUberColor(const Material& material, int skin)
{
    auto chromatic = [](const std::array<float, 3>& value) {
        const float minimum = std::min({value[0], value[1], value[2]});
        const float maximum = std::max({value[0], value[1], value[2]});
        return maximum > 0.08f && maximum - minimum > 0.075f;
    };

    // A successfully resolved, visibly coloured proxy is the most accurate
    // preview source. Neutral white/grey is usually only a runtime placeholder
    // for ModelGlowColor and must not turn robot players into white silhouettes.
    if (material.color2Active && chromatic(material.color2)) return material.color2;
    if (chromatic(material.selfIllumTint)) return material.selfIllumTint;

    const std::string name = lower(normalizeMaterialName(material.name));
    if (name.find("blue") != std::string::npos ||
        name.find("_blu") != std::string::npos ||
        name.find("/blu") != std::string::npos) {
        return {0.12f, 0.38f, 1.0f};
    }
    if (name.find("red") != std::string::npos) return {1.0f, 0.10f, 0.06f};

    if (material.uberEffect || material.modelGlowProxy) {
        // TF2 model skin families conventionally alternate RED/BLU, including
        // extended robot and invulnerability skin tables (0/2 RED, 1/3 BLU).
        return (std::max(skin, 0) & 1) != 0
            ? std::array<float, 3>{0.12f, 0.38f, 1.0f}
            : std::array<float, 3>{1.0f, 0.10f, 0.06f};
    }

    if (material.color2Active) return material.color2;
    return {1.0f, 1.0f, 1.0f};
}

std::uint32_t Image::sampleWrapped(double uPixels, double vPixels) const
{
    if (!valid()) return 0xFFFF00FFu;
    int x = static_cast<int>(std::floor(uPixels));
    int y = static_cast<int>(std::floor(vPixels));
    x %= width; y %= height;
    if (x < 0) x += width;
    if (y < 0) y += height;
    return pixels[static_cast<std::size_t>(y * width + x)];
}

MaterialSystem::MaterialSystem(std::shared_ptr<GameFileSystem> fileSystem) : fileSystem_(std::move(fileSystem)) {}

namespace {
// Freeing a cache entry returns its pixel buffers to the allocator, but glibc
// keeps the pages in its arenas for reuse, so process RSS does not fall and every
// monitor still reports the memory as used. Measured: dropping 165 MB of previews
// only moved RSS by 35 MB until this ran, which then returned a further 153 MB.
// Only worth calling after a bulk release, never per material.
void returnFreedPagesToOs()
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}
} // namespace

namespace {

// Round a requested cap up to a power of two so a handful of thumbnail sizes and
// device pixel ratios share cache entries instead of each minting its own.
int quantizePreviewCap(int maxDimension)
{
    int quantized = 16;
    while (quantized < maxDimension && quantized < 4096) quantized *= 2;
    return quantized;
}

// Box-filtered downscale. Stepping a preview down in size re-derives it from the
// copy already in memory rather than re-reading and re-decoding the VTF.
Image downscaleImage(const Image& source, int maxDimension)
{
    const int longest = std::max(source.width, source.height);
    if (!source.valid() || longest <= maxDimension) return source;
    const double scale = static_cast<double>(maxDimension) / longest;
    const int width = std::max(1, static_cast<int>(source.width * scale));
    const int height = std::max(1, static_cast<int>(source.height * scale));

    Image result{width, height,
                 std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height)};
    for (int y = 0; y < height; ++y) {
        const int sourceTop = y * source.height / height;
        const int sourceBottom = std::max(sourceTop + 1, (y + 1) * source.height / height);
        for (int x = 0; x < width; ++x) {
            const int sourceLeft = x * source.width / width;
            const int sourceRight = std::max(sourceLeft + 1, (x + 1) * source.width / width);
            std::uint64_t a = 0, r = 0, g = 0, b = 0, samples = 0;
            for (int sy = sourceTop; sy < sourceBottom; ++sy) {
                for (int sx = sourceLeft; sx < sourceRight; ++sx) {
                    const std::uint32_t pixel =
                        source.pixels[static_cast<std::size_t>(sy) * source.width + sx];
                    a += (pixel >> 24) & 255u;
                    r += (pixel >> 16) & 255u;
                    g += (pixel >> 8) & 255u;
                    b += pixel & 255u;
                    ++samples;
                }
            }
            if (!samples) samples = 1;
            result.pixels[static_cast<std::size_t>(y) * width + x] =
                (static_cast<std::uint32_t>(a / samples) << 24) |
                (static_cast<std::uint32_t>(r / samples) << 16) |
                (static_cast<std::uint32_t>(g / samples) << 8) |
                static_cast<std::uint32_t>(b / samples);
        }
    }
    return result;
}

std::shared_ptr<Material> downscaleMaterial(const Material& source, int maxDimension)
{
    auto copy = std::make_shared<Material>(source);
    for (Image* image : {&copy->image, &copy->image2, &copy->detailImage,
            &copy->bumpImage, &copy->lightWarpImage, &copy->phongExponentImage,
            &copy->selfIllumMaskImage, &copy->waterNormalImage, &copy->waterFlowImage}) {
        if (image->valid()) *image = downscaleImage(*image, maxDimension);
    }
    for (Image& face : copy->envMapCube.faces)
        if (face.valid()) face = downscaleImage(face, maxDimension);
    for (Image& frame : copy->bumpFrames)
        if (frame.valid()) frame = downscaleImage(frame, maxDimension);
    return copy;
}

} // namespace

namespace {
// Total resident pixel bytes across every decoded surface a Material owns.
std::size_t residentBytes(const Material& material)
{
    std::size_t total = 0;
    for (const Image* image : {&material.image, &material.image2, &material.detailImage,
            &material.bumpImage, &material.lightWarpImage, &material.phongExponentImage,
            &material.selfIllumMaskImage, &material.waterNormalImage,
            &material.waterFlowImage}) {
        total += image->pixels.size() * sizeof(std::uint32_t);
    }
    for (const Image& face : material.envMapCube.faces)
        total += face.pixels.size() * sizeof(std::uint32_t);
    for (const Image& frame : material.bumpFrames)
        total += frame.pixels.size() * sizeof(std::uint32_t);
    return total;
}
} // namespace

std::size_t MaterialSystem::adjustPreviewCache(int maxDimension)
{
    if (maxDimension <= 0) return purgeUnusedMaterials();
    const int target = quantizePreviewCap(maxDimension);
    std::size_t freed = 0;
    {
    std::lock_guard lock(mutex_);

    // Entries the 3D scene (or an open preview) still references are never
    // touched here, so scene geometry always keeps the highest-quality texture
    // it was loaded with. Only entries this map alone holds are adjustable.
    std::vector<std::pair<std::string, std::shared_ptr<Material>>> rescaled;
    for (auto it = previewCache_.begin(); it != previewCache_.end();) {
        const std::size_t separator = it->first.rfind('@');
        const int cached = separator == std::string::npos
            ? 0 : std::atoi(it->first.c_str() + separator + 1);
        if (cached == target || !it->second || it->second.use_count() != 1) {
            ++it;
            continue;
        }
        const std::string name = separator == std::string::npos
            ? it->first : it->first.substr(0, separator);
        const std::size_t before = residentBytes(*it->second);
        if (cached > target) {
            // Stepping down: re-derive the smaller preview from the copy already
            // in memory instead of re-reading and re-decoding the VTF, then drop
            // the larger original.
            auto smaller = downscaleMaterial(*it->second, target);
            freed += before > residentBytes(*smaller) ? before - residentBytes(*smaller) : 0;
            rescaled.emplace_back(name + '@' + std::to_string(target), std::move(smaller));
        } else {
            // Stepping up: a smaller mip cannot be upscaled into a larger
            // preview without losing detail, so it is simply discarded and the
            // new size is decoded fresh from the appropriate mip.
            freed += before;
        }
        it = previewCache_.erase(it);
    }
    for (auto& [key, entry] : rescaled)
        previewCache_.try_emplace(key, std::move(entry));

    // Full-quality entries nothing references belong to no live scene; reclaim
    // them too rather than leaving orphans from a closed document behind.
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second && it->second.use_count() == 1) {
            freed += residentBytes(*it->second);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    }
    if (freed) returnFreedPagesToOs();
    return freed;
}

std::size_t MaterialSystem::purgeUnusedMaterials()
{
    std::size_t freed = 0;
    {
    std::lock_guard lock(mutex_);
    // use_count() == 1 means this map holds the only reference, so nothing - no
    // 3D batch, no open preview, no in-flight loader - is still reading it. The
    // 3D renderer's MaterialBatch keeps a shared_ptr per drawn material, which
    // is exactly what makes a scene-resident texture uncollectable here.
    const auto purge = [&freed](std::unordered_map<std::string,
                                                   std::shared_ptr<Material>>& cache) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (it->second && it->second.use_count() == 1) {
                freed += residentBytes(*it->second);
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    };
    purge(previewCache_);
    purge(cache_);
    }
    // Hand the emptied pages back, otherwise the release is invisible outside
    // this process.
    if (freed) returnFreedPagesToOs();
    return freed;
}

void MaterialSystem::clearCache()
{
    {
        std::lock_guard lock(mutex_);
        cache_.clear();
        previewCache_.clear();
    }
    returnFreedPagesToOs();
}

std::shared_ptr<const Material> MaterialSystem::residentMaterial(std::string_view name) const
{
    const std::string normalized = normalizeMaterialName(name);
    std::lock_guard lock(mutex_);
    if (auto it = cache_.find(normalized); it != cache_.end()) return it->second;
    return nullptr;
}

std::shared_ptr<const Material> MaterialSystem::material(std::string_view name)
{
    const std::string normalized = normalizeMaterialName(name);
    {
        std::lock_guard lock(mutex_);
        if (auto it = cache_.find(normalized); it != cache_.end()) return it->second;
    }
    auto loaded = loadMaterial(normalized);
    std::lock_guard lock(mutex_);
    const auto [it, inserted] = cache_.emplace(normalized, loaded);
    return inserted ? loaded : it->second;
}

std::shared_ptr<const Material> MaterialSystem::previewMaterial(std::string_view name,
                                                                int maxDimension)
{
    if (maxDimension <= 0) return material(name);
    const std::string normalized = normalizeMaterialName(name);
    const int quantized = quantizePreviewCap(maxDimension);
    const std::string key = normalized + '@' + std::to_string(quantized);
    {
        std::lock_guard lock(mutex_);
        if (auto it = previewCache_.find(key); it != previewCache_.end()) return it->second;
        // A full-quality load already covers any preview request.
        if (auto it = cache_.find(normalized); it != cache_.end()) return it->second;
    }
    auto loaded = loadMaterial(normalized, quantized);
    std::lock_guard lock(mutex_);
    const auto [it, inserted] = previewCache_.emplace(key, loaded);
    return inserted ? loaded : it->second;
}

std::vector<std::string> MaterialSystem::materialNames() const
{
    if (!fileSystem_) return {};
    std::vector<std::string> result;
    for (std::string path : fileSystem_->listFiles("materials/", ".vmt")) {
        if (path.rfind("materials/", 0) == 0) path.erase(0, 10);
        if (endsWithCi(path, ".vmt")) path.resize(path.size() - 4);
        if (!path.empty() && !hiddenFromTextureBrowser(path)) result.push_back(std::move(path));
    }
    return result;
}

std::shared_ptr<Material> MaterialSystem::missingMaterial(std::string normalizedName)
{
    auto result = std::make_shared<Material>();
    result->name = std::move(normalizedName);
    result->shader = "missing";
    result->image = checkerboard();
    result->missing = true;
    result->error = "Material or base texture is missing";
    return result;
}

std::shared_ptr<Material> MaterialSystem::loadMaterial(std::string normalizedName,
                                                      int maxDimension)
{
    auto fail = [&](std::string message,
                    std::string vmtSource = {},
                    std::string vtfSource = {}) {
        auto result = missingMaterial(normalizedName);
        result->error = std::move(message);
        result->vmtSource = std::move(vmtSource);
        result->vtfSource = std::move(vtfSource);
        return result;
    };

    if (!fileSystem_ || normalizedName.empty()) {
        return fail("No game filesystem is configured or the material name is empty");
    }

    const std::string vmtPath = "materials/" + normalizedName + ".vmt";
    const auto vmtLocation = fileSystem_->sourceForFile(vmtPath);
    const std::string vmtSource = vmtLocation ? vmtLocation->path.string() : std::string{};
    const auto vmtBytes = fileSystem_->readFile(vmtPath);
    if (!vmtBytes) {
        return fail(vmtLocation
            ? "The VMT is indexed but its VPK payload could not be read"
            : "VMT not found in the configured search paths",
            vmtSource);
    }

    std::string text(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
    hammer::vmf::ParseError parseError;
    auto document = hammer::vmf::Document::parse(std::move(text), &parseError);
    if (!document || document->roots().empty()) {
        return fail("VMT parse error at " + std::to_string(parseError.line) + ":" +
                    std::to_string(parseError.column) + ": " + parseError.message,
                    vmtSource);
    }
    const hammer::vmf::Block& root = document->roots().front();

    auto result = std::make_shared<Material>();
    result->name = normalizedName;
    result->shader = root.name;
    result->vmtSource = vmtSource;
    const bool patchMaterial = iequals(root.name, "patch");
    if (patchMaterial) {
        if (const std::string* include = valueCi(root, "include")) {
            const auto base = material(*include);
            if (base) *result = *base;
            result->name = normalizedName;
            result->shader = "patch";
            result->vmtSource = vmtSource;
        } else {
            return fail("Patch material has no include key", vmtSource);
        }
    } else {
        const EditorMaterialEffectSupport support = editorMaterialEffectSupport(root.name);
        result->editorBumpMapSupported = support.bumpMap;
        result->editorPhongSupported = support.phong;
        result->editorSpecularSupported = support.specular;
        result->editorSelfIllumSupported = support.selfIllum;
        result->editorRimLightSupported = support.rimLight;
    }
    result->water = result->water || isWaterShader(result->shader) || isWaterShader(root.name);
    result->decalModulate = result->decalModulate || iequals(result->shader, "DecalModulate") ||
                             iequals(root.name, "DecalModulate");
    // VBSP's EmitDetailModels reads this off the face's material to decide
    // which detail.vbsp object type is scattered across the surface.
    if (const std::string* detailType = nestedValueCi(root, "%detailtype")) {
        result->detailType = *detailType;
    }
    if (const std::string* compileTrigger = nestedValueCi(root, "%compiletrigger")) {
        result->compileTrigger = parseMaterialFloat(compileTrigger, 0.0f) >= 0.5f;
    }
    result->previewAnimated = result->previewAnimated || result->water;
    if (result->water) result->translucent = true;
    if (const std::string* base = nestedValueCi(root, "$basetexture")) {
        result->baseTexture = normalizeMaterialName(*base);
    }
    if (const std::string* base2 = nestedValueCi(root, "$basetexture2")) {
        result->baseTexture2 = normalizeMaterialName(*base2);
    }
    if (const std::string* detail = nestedValueCi(root, "$detail")) {
        result->detailTexture = normalizeMaterialName(*detail);
    }
    result->detailScale = std::clamp(std::abs(
        parseMaterialFloat(nestedValueCi(root, "$detailscale"), result->detailScale)),
        0.001f, 256.0f);
    result->detailBlendFactor = std::clamp(
        parseMaterialFloat(nestedValueCi(root, "$detailblendfactor"),
                           result->detailBlendFactor),
        0.0f, 8.0f);
    result->detailBlendMode = std::clamp(static_cast<int>(std::lround(
        parseMaterialFloat(nestedValueCi(root, "$detailblendmode"),
                           static_cast<float>(result->detailBlendMode)))), 0, 12);
    if (const std::string* alphaTest = nestedValueCi(root, "$alphatest")) {
        result->alphaTest = parseMaterialBool(alphaTest);
    }
    result->alphaTestReference = std::clamp(
        parseMaterialFloat(nestedValueCi(root, "$alphatestreference"),
                           result->alphaTestReference),
        0.0f, 1.0f);

    if (const std::string* bump = nestedValueCi(root, "$bumpmap")) {
        result->bumpMap = normalizeMaterialName(*bump);
    } else if (const std::string* normal = nestedValueCi(root, "$normalmap")) {
        result->bumpMap = normalizeMaterialName(*normal);
    }
    // Patch VMTs may explicitly disable effects inherited from their include,
    // so an authored key replaces rather than only ORs with the previous value.
    if (const std::string* phong = nestedValueCi(root, "$phong")) {
        result->phong = parseMaterialBool(phong);
    }
    if (const std::string* selfIllum = nestedValueCi(root, "$selfillum")) {
        result->selfIllum = parseMaterialBool(selfIllum);
    }
    if (const std::string* selfIllumFresnel = nestedValueCi(root, "$selfillumfresnel")) {
        result->selfIllumFresnel = parseMaterialBool(selfIllumFresnel);
    }
    if (const std::string* selfIllumFresnelParams =
            nestedValueCi(root, "$selfillumfresnelminmaxexp")) {
        result->selfIllumFresnelMinMaxExp = parseMaterialVector3(
            selfIllumFresnelParams, result->selfIllumFresnelMinMaxExp);
        result->selfIllumFresnelMinMaxExp[2] = std::clamp(
            result->selfIllumFresnelMinMaxExp[2], 0.01f, 128.0f);
    }
    if (const std::string* color2 = valueCi(root, "$color2")) {
        result->color2 = materialTintVector(color2, result->color2);
        result->color2Active = true;
    }
    if (const std::string* blendTint = valueCi(root, "$blendtintbybasealpha")) {
        result->blendTintByBaseAlpha = parseMaterialBool(blendTint);
    }
    result->blendTintColorOverBase = std::clamp(
        parseMaterialFloat(valueCi(root, "$blendtintcoloroverbase"),
                           result->blendTintColorOverBase),
        0.0f, 1.0f);
    if (const std::string* rimLight = nestedValueCi(root, "$rimlight")) {
        result->rimLight = parseMaterialBool(rimLight);
    }
    if (const std::string* ssBump = nestedValueCi(root, "$ssbump")) {
        result->ssBump = parseMaterialBool(ssBump);
    }
    if (const std::string* envMap = nestedValueCi(root, "$envmap")) {
        const std::string normalizedEnv = normalizeMaterialName(*envMap);
        const std::string loweredEnv = lower(normalizedEnv);
        result->specular = !loweredEnv.empty() && loweredEnv != "0" &&
                           loweredEnv != "none";
        result->envMap = result->specular ? normalizedEnv : std::string{};
        result->envMapUsesMapCubemap = result->specular && loweredEnv == "env_cubemap";
        // An overriding patch must not retain the included material's decoded
        // cubemap when it changes or disables $envmap.
        result->envMapCube = {};
        result->hasEnvMapCube = false;
        result->envMapSource.clear();
        result->envMapError.clear();
    }
    if (const std::string* exponent = nestedValueCi(root, "$phongexponent")) {
        const float parsed = parseMaterialFloat(exponent, result->phongExponent);
        result->phongExponentOverride = parsed > 0.0f;
        if (result->phongExponentOverride)
            result->phongExponent = std::clamp(parsed, 1.0f, 256.0f);
    }
    result->phongBoost = std::clamp(
        parseMaterialFloat(nestedValueCi(root, "$phongboost"), result->phongBoost),
        0.0f, 16.0f);
    if (const std::string* mask = nestedValueCi(root, "$basemapalphaphongmask")) {
        result->phongMaskFromBaseAlpha = parseMaterialBool(mask);
    }
    if (const std::string* mask = nestedValueCi(root, "$basealphaenvmapmask")) {
        result->envMapMaskFromBaseAlpha = parseMaterialBool(mask);
        if (result->envMapMaskFromBaseAlpha)
            result->envMapMaskFromNormalAlpha = false;
    }
    if (const std::string* mask = nestedValueCi(root, "$normalmapalphaenvmapmask")) {
        result->envMapMaskFromNormalAlpha = parseMaterialBool(mask);
        if (result->envMapMaskFromNormalAlpha)
            result->envMapMaskFromBaseAlpha = false;
    }
    if (const std::string* invert = nestedValueCi(root, "$invertphongmask")) {
        result->invertPhongMask = parseMaterialBool(invert);
    }
    if (const std::string* ranges = nestedValueCi(root, "$phongfresnelranges")) {
        result->phongFresnelRanges = parseMaterialVector3(ranges, result->phongFresnelRanges);
        for (float& component : result->phongFresnelRanges)
            component = std::clamp(component, 0.0f, 4.0f);
    }
    if (const std::string* tint = nestedValueCi(root, "$phongtint")) {
        result->phongTintDefined = true;
        result->phongTint = parseMaterialVector3(tint, result->phongTint);
        for (float& component : result->phongTint)
            component = std::clamp(component, 0.0f, 8.0f);
    }
    if (const std::string* albedoTint = nestedValueCi(root, "$phongalbedotint")) {
        result->phongAlbedoTint = parseMaterialBool(albedoTint);
    }
    if (const std::string* exponentTexture = nestedValueCi(root, "$phongexponenttexture")) {
        result->phongExponentTexture = normalizeMaterialName(*exponentTexture);
    }
    if (const std::string* selfIllumTint = valueCi(root, "$selfillumtint")) {
        result->selfIllumTint = parseMaterialVector3(selfIllumTint, result->selfIllumTint);
        for (float& component : result->selfIllumTint)
            component = std::clamp(component, 0.0f, 16.0f);
    }
    if (const std::string* selfIllumMask = nestedValueCi(root, "$selfillummask")) {
        result->selfIllumMask = normalizeMaterialName(*selfIllumMask);
    }
    result->rimLightExponent = std::clamp(
        parseMaterialFloat(nestedValueCi(root, "$rimlightexponent"), result->rimLightExponent),
        1.0f, 128.0f);
    result->rimLightBoost = std::clamp(
        parseMaterialFloat(nestedValueCi(root, "$rimlightboost"), result->rimLightBoost),
        0.0f, 16.0f);
    if (const std::string* rimMask = nestedValueCi(root, "$rimmask")) {
        result->rimMaskFromExponentAlpha = parseMaterialBool(rimMask);
    }
    if (const std::string* lightWarp = nestedValueCi(root, "$lightwarptexture")) {
        result->lightWarpTexture = normalizeMaterialName(*lightWarp);
    }
    if (const std::string* halfLambert = nestedValueCi(root, "$halflambert")) {
        result->halfLambert = parseMaterialBool(halfLambert);
    }
    if (const std::string* envContrast = nestedValueCi(root, "$envmapcontrast")) {
        result->envMapContrast = std::clamp(parseMaterialFloat(envContrast, 0.0f), 0.0f, 1.0f);
    }
    if (const std::string* envSaturation = nestedValueCi(root, "$envmapsaturation")) {
        result->envMapSaturation =
            std::clamp(parseMaterialFloat(envSaturation, 1.0f), 0.0f, 1.0f);
    }
    if (const std::string* envTint = nestedValueCi(root, "$envmaptint")) {
        result->envMapTint = parseMaterialVector3(envTint, result->envMapTint);
        for (float& component : result->envMapTint)
            component = std::clamp(component, 0.0f, 8.0f);
        const float average = (result->envMapTint[0] + result->envMapTint[1] +
                               result->envMapTint[2]) / 3.0f;
        result->specularStrength = std::clamp(0.04f + average * 0.26f, 0.0f, 2.0f);
    } else if (result->phong) {
        result->specularStrength = std::clamp(0.10f + result->phongBoost * 0.04f,
                                              0.0f, 2.0f);
    }
    // TF2 effect/paint VMTs commonly feed $color2 and $selfillumtint
    // through a short material-proxy chain. Resolve the deterministic subset
    // useful to an editor preview after all static defaults/overrides are known.
    evaluateMaterialProxies(root, normalizedName, *result);

    if (const std::string* translucent = nestedValueCi(root, "$translucent")) {
        result->translucent = *translucent != "0";
    }
    if (const std::string* alpha = nestedValueCi(root, "$alpha")) {
        try {
            result->alpha = std::clamp(static_cast<float>(std::stod(*alpha)), 0.0f, 1.0f);
            result->translucent = result->translucent || result->alpha < 0.999f;
        } catch (...) {}
    }
    result->decalScale = std::clamp(std::abs(
        parseMaterialFloat(nestedValueCi(root, "$decalscale"), result->decalScale)),
        0.001f, 128.0f);
    if (result->water) {
        const auto fog = parseMaterialColor(nestedValueCi(root, "$fogcolor"), {44, 88, 104});
        const auto refract = parseMaterialColor(nestedValueCi(root, "$refracttint"), fog);
        const auto reflect = parseMaterialColor(nestedValueCi(root, "$reflecttint"), {210, 232, 255});
        result->waterFogColor = normalizedColor(fog);
        result->waterRefractTint = normalizedColor(refract);
        result->waterReflectTint = normalizedColor(reflect);
        result->waterFresnelReflectance = std::clamp(
            parseMaterialFloat(nestedValueCi(root, "$fresnelreflection"), 0.20f), 0.0f, 1.0f);
        result->waterReflectAmount = std::clamp(std::abs(
            parseMaterialFloat(nestedValueCi(root, "$reflectamount"), 0.80f)), 0.0f, 4.0f);
        result->waterRefractAmount = std::clamp(std::abs(
            parseMaterialFloat(nestedValueCi(root, "$refractamount"), 0.0f)), 0.0f, 4.0f);
        result->waterReflectBlendFactor = std::clamp(
            parseMaterialFloat(nestedValueCi(root, "$reflectblendfactor"), 1.0f), 0.0f, 4.0f);
        result->waterFogStart = std::max(0.0f,
            parseMaterialFloat(nestedValueCi(root, "$fogstart"), 0.0f));
        result->waterFogEnd = std::max(0.0f,
            parseMaterialFloat(nestedValueCi(root, "$fogend"), 0.0f));
        result->waterNoFresnel = parseMaterialBool(nestedValueCi(root, "$nofresnel"));
        result->waterScale = parseMaterialVector2(
            nestedValueCi(root, "$scale"), result->waterScale);
        for (float& component : result->waterScale)
            component = std::clamp(std::abs(component), 0.001f, 64.0f);
        result->waterScroll1 = parseMaterialVector2(
            nestedValueCi(root, "$scroll1"), result->waterScroll1);
        result->waterScroll2 = parseMaterialVector2(
            nestedValueCi(root, "$scroll2"), result->waterScroll2);
        result->waterMultiTexture =
            std::abs(result->waterScroll1[0]) > 0.000001f ||
            std::abs(result->waterScroll1[1]) > 0.000001f ||
            std::abs(result->waterScroll2[0]) > 0.000001f ||
            std::abs(result->waterScroll2[1]) > 0.000001f;

        // Honour an explicit $alpha, but retain Hammer's useful opaque default
        // for Water materials that only declare $translucent 1.
        result->waterAlpha = std::clamp(
            parseMaterialFloat(nestedValueCi(root, "$alpha"), 0.94f), 0.02f, 1.0f);
        result->waterNormalScale = std::clamp(
            parseMaterialFloat(nestedValueCi(root, "$bumpstrength"), 1.0f), 0.0f, 4.0f);

        // Source flow-map water advances the normal texture through a vector
        // field. Honour the common timing/scale controls when present while
        // retaining conservative defaults for custom VMTs that only specify
        // $flowmap.
        const float flowInterval = std::clamp(
            parseMaterialFloat(nestedValueCi(root, "$flow_timeintervalinseconds"), 1.0f),
            0.05f, 20.0f);
        result->waterFlowCycleRate = 1.0f / flowInterval;
        result->waterFlowDistance = std::clamp(std::abs(
            parseMaterialFloat(nestedValueCi(root, "$flow_uvscrolldistance"), 0.10f)),
            0.0f, 2.0f);
        result->waterFlowMapScale = std::clamp(std::abs(
            parseMaterialFloat(nestedValueCi(root, "$flow_worlduvscale"), 1.0f)),
            0.01f, 64.0f);
        result->waterFlowNormalUvScale = std::clamp(std::abs(
            parseMaterialFloat(nestedValueCi(root, "$flow_normaluvscale"), 1.0f)),
            0.01f, 64.0f);
    }

    auto loadTexture = [&](const std::string& textureName, Image* output,
                           std::string* source, std::string* loadError) -> bool {
        const std::string normalizedTexture = normalizeMaterialName(textureName);
        if (normalizedTexture.empty()) {
            if (loadError) *loadError = "Texture name is empty";
            return false;
        }
        const std::string path = "materials/" + normalizedTexture + ".vtf";
        const auto location = fileSystem_->sourceForFile(path);
        if (source) *source = location ? location->path.string() : std::string{};
        const auto bytes = fileSystem_->readFile(path);
        if (!bytes) {
            if (loadError) {
                *loadError = location ? "The VTF is indexed but its VPK payload could not be read"
                                      : "VTF not found: " + path;
            }
            return false;
        }
        std::string decodeError;
        auto decoded = decodeVtf(*bytes, &decodeError, maxDimension);
        if (!decoded) {
            if (loadError) *loadError = "VTF decode failed: " + decodeError;
            return false;
        }
        if (output) *output = std::move(*decoded);
        return true;
    };

    // Ordinary $envmap values name a cubemap VTF. Only the special
    // "env_cubemap" token requests the map cubemap; explicit files are loaded
    // first and the renderer falls back to the map skybox only if they fail.
    if (result->specular && !result->envMapUsesMapCubemap &&
        !result->envMap.empty() && !result->hasEnvMapCube) {
        const std::string path = "materials/" + result->envMap + ".vtf";
        const auto location = fileSystem_->sourceForFile(path);
        result->envMapSource = location ? location->path.string() : std::string{};
        const auto bytes = fileSystem_->readFile(path);
        if (!bytes) {
            result->envMapError = location
                ? "The envmap VTF is indexed but its VPK payload could not be read"
                : "Envmap VTF not found: " + path;
        } else {
            std::string decodeError;
            auto decoded = decodeVtfCubemap(*bytes, &decodeError);
            if (decoded) {
                result->envMapCube = std::move(*decoded);
                result->hasEnvMapCube = true;
            } else {
                result->envMapError = "Envmap VTF decode failed: " + decodeError;
            }
        }
    }


    if (result->water) {
        const std::string* normalName = nestedValueCi(root, "$normalmap");
        if (!normalName) normalName = nestedValueCi(root, "$normalmap2");
        if (!normalName) normalName = nestedValueCi(root, "$bumpmap");
        if (!normalName) normalName = nestedValueCi(root, "$dudvmap");

        Image normal;
        std::string normalError;
        bool hasNormal = false;
        if (normalName) {
            const std::string normalizedNormal = normalizeMaterialName(*normalName);
            if (result->baseTexture.empty()) result->baseTexture = normalizedNormal;
            hasNormal = loadTexture(normalizedNormal, &normal, &result->vtfSource, &normalError);
        }

        if (!hasNormal && !result->baseTexture.empty()) {
            // A few custom Water VMTs put their distortion texture in
            // $basetexture. Accept it as a last-resort normal source.
            hasNormal = loadTexture(result->baseTexture, &normal, &result->vtfSource, &normalError);
        }

        if (!hasNormal) {
            result->waterNormalImage = makeProceduralWaterNormal();
        } else {
            result->waterNormalImage = prepareWaterNormalMap(normal);
        }

        // A flow map is a separate vector field, never a normal-map fallback.
        // Preserve its original R/G values exactly:
        //   red   0 -> right, 255 -> left
        //   green 0 -> down,  255 -> up
        std::string flowError;
        if (const std::string* flowName = nestedValueCi(root, "$flowmap")) {
            result->waterFlowMap = normalizeMaterialName(*flowName);
            if (!result->waterFlowMap.empty()) {
                result->waterHasFlowMap = loadTexture(
                    result->waterFlowMap, &result->waterFlowImage,
                    &result->waterFlowSource, &flowError);
            }
        }

        // Keep the browser/software fallback neutral. The old fog-colour preview
        // was the solid blue image that could appear beneath the hardware water
        // pass. It must never be used as a second rendered water layer.
        constexpr std::array<int, 3> neutralWater{{128, 128, 128}};
        constexpr std::array<int, 3> neutralSheen{{220, 220, 220}};
        result->image = makeWaterPreview(&result->waterNormalImage, neutralWater,
                                         neutralSheen, result->waterAlpha);
        result->translucent = true;
        result->missing = false;
        result->error.clear();
        if (result->waterHasFlowMap) {
            result->note = "Flow-mapped Water shader using " +
                (hasNormal ? result->baseTexture : std::string("procedural ripples")) +
                " with " + result->waterFlowMap;
        } else if (!flowError.empty()) {
            result->note = "Animated Water shader could not load flow map " +
                result->waterFlowMap + ": " + flowError;
        } else if (hasNormal) {
            result->note = "Animated Water shader preview using " + result->baseTexture;
        } else if (!normalError.empty()) {
            result->note = "Animated Water shader uses procedural ripples because " + normalError;
        } else {
            result->note = "Animated Water shader uses procedural ripples (no normal/distortion texture)";
        }
        return result;
    }

    if (result->baseTexture.empty()) {
        return fail("VMT has no usable $basetexture", vmtSource);
    }

    std::string loadError;
    if (!loadTexture(result->baseTexture, &result->image, &result->vtfSource, &loadError)) {
        return fail(loadError, vmtSource, result->vtfSource);
    }
    result->missing = false;
    result->error.clear();

    if (!result->detailTexture.empty()) {
        std::string detailSource;
        std::string detailError;
        if (loadTexture(result->detailTexture, &result->detailImage,
                        &detailSource, &detailError)) {
            result->hasDetailTexture = result->detailImage.valid();
        } else if (result->note.empty()) {
            result->note = "Detail texture could not be loaded: " + detailError;
        }
    }

    if (!result->phongExponentTexture.empty()) {
        std::string exponentSource;
        std::string exponentError;
        if (loadTexture(result->phongExponentTexture, &result->phongExponentImage,
                        &exponentSource, &exponentError)) {
            result->hasPhongExponentTexture = result->phongExponentImage.valid();
        } else if (result->note.empty()) {
            result->note = "Phong exponent texture could not be loaded: " + exponentError;
        }
    }

    if (result->selfIllum && !result->selfIllumMask.empty()) {
        std::string selfIllumSource;
        std::string selfIllumError;
        if (loadTexture(result->selfIllumMask, &result->selfIllumMaskImage,
                        &selfIllumSource, &selfIllumError)) {
            result->hasSelfIllumMask = result->selfIllumMaskImage.valid();
        } else if (result->note.empty()) {
            result->note = "Self-illumination mask could not be loaded: " + selfIllumError;
        }
    }

    if (!result->lightWarpTexture.empty()) {
        std::string lightWarpSource;
        std::string lightWarpError;
        if (loadTexture(result->lightWarpTexture, &result->lightWarpImage,
                        &lightWarpSource, &lightWarpError)) {
            result->hasLightWarpTexture = result->lightWarpImage.valid();
        } else if (result->note.empty()) {
            result->note = "Lightwarp texture could not be loaded: " + lightWarpError;
        }
    }

    if (!result->bumpMap.empty()) {
        std::string bumpSource;
        std::string bumpError;
        Image bumpImage;
        if (loadTexture(result->bumpMap, &bumpImage, &bumpSource, &bumpError)) {
            result->bumpImage = prepareEditorNormalMap(bumpImage);
            result->bumpMapped = result->bumpImage.valid();

            if (result->bumpAnimationFrameRate > 0.0f) {
                const std::string path = "materials/" + result->bumpMap + ".vtf";
                if (const auto bytes = fileSystem_->readFile(path)) {
                    std::string framesError;
                    if (auto decodedFrames = decodeVtfFrames(*bytes, &framesError)) {
                        result->bumpFrames.reserve(decodedFrames->size());
                        for (const Image& frame : *decodedFrames)
                            result->bumpFrames.push_back(prepareEditorNormalMap(frame));
                        result->bumpFrames.erase(std::remove_if(
                            result->bumpFrames.begin(), result->bumpFrames.end(),
                            [](const Image& frame) { return !frame.valid(); }),
                            result->bumpFrames.end());
                        if (!result->bumpFrames.empty())
                            result->bumpImage = result->bumpFrames.front();
                    }
                }
            }
            if (result->bumpMapped && result->note.empty()) {
                result->note = result->bumpFrames.size() > 1
                    ? "Animated normal map: " + result->bumpMap
                    : "Normal map: " + result->bumpMap;
            }
        } else if (result->note.empty()) {
            result->note = "Normal map could not be loaded: " + bumpError;
        }
    }

    // WorldVertexTransition and compatible shaders blend the second base
    // texture from the displacement vertex-alpha paint stored in the VMF.
    // A missing secondary map should not hide an otherwise valid material;
    // render the primary texture and retain a diagnostic note instead.
    if (!result->baseTexture2.empty()) {
        std::string secondarySource;
        std::string secondaryError;
        Image secondaryImage;
        if (loadTexture(result->baseTexture2, &secondaryImage,
                        &secondarySource, &secondaryError)) {
            result->image2 = std::move(secondaryImage);
            result->blended = true;
            result->note = "Displacement blend: " + result->baseTexture +
                           " -> " + result->baseTexture2;
        } else {
            result->image2 = {};
            result->blended = false;
            result->note = "Secondary blend texture could not be loaded: " + secondaryError;
        }
    }
    return result;
}

namespace {

struct VtfLayout
{
    int width{0};
    int height{0};
    std::uint32_t format{0};
    std::uint32_t frames{1};
    int depth{1};
    int storedFaces{1};
    std::size_t topMipOffset{0};
    std::size_t topSurfaceSize{0};
    bool cubemap{false};
    // VTFs store mips smallest-first, so the offset of the largest surface is
    // only reachable by walking the whole chain. Keeping the chain start and the
    // level count lets a caller decode a *smaller* mip instead - which is all a
    // thumbnail needs, and is orders of magnitude less memory than mip 0.
    int mipCount{1};
    std::size_t mipChainOffset{0};
};

// Re-describes a layout so it refers to one smaller mip level, and reports that
// level's byte offset. `maxDimension` is the largest edge the caller needs; the
// smallest mip that still covers it wins. Zero or negative means full size.
VtfLayout layoutForMaxDimension(const VtfLayout& base, int maxDimension,
                                std::size_t* offsetOut)
{
    int level = 0;
    if (maxDimension > 0) {
        while (level + 1 < base.mipCount &&
               std::max(1, base.width >> (level + 1)) >= maxDimension &&
               std::max(1, base.height >> (level + 1)) >= maxDimension) {
            ++level;
        }
    }
    VtfLayout chosen = base;
    chosen.width = std::max(1, base.width >> level);
    chosen.height = std::max(1, base.height >> level);
    chosen.topSurfaceSize = imageSize(base.format, chosen.width, chosen.height);

    // Mips are stored largest-last, so skip every level below the chosen one.
    std::size_t offset = base.mipChainOffset;
    for (int mip = base.mipCount - 1; mip > level; --mip) {
        offset += imageSize(base.format, std::max(1, base.width >> mip),
                            std::max(1, base.height >> mip)) *
                  base.frames * static_cast<std::size_t>(base.storedFaces) *
                  static_cast<std::size_t>(std::max(1, base.depth >> mip));
    }
    if (offsetOut) *offsetOut = offset;
    return chosen;
}

std::optional<VtfLayout> parseVtfLayout(const std::vector<std::uint8_t>& bytes,
                                        std::string* error)
{
    auto fail = [&](std::string message) -> std::optional<VtfLayout> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "VTF\0", 4) != 0)
        return fail("Invalid VTF signature");
    const std::uint32_t major = u32(bytes, 4);
    const std::uint32_t minor = u32(bytes, 8);
    const std::uint32_t headerSize = u32(bytes, 12);
    if (major != 7 || headerSize > bytes.size())
        return fail("Unsupported VTF version/header");

    VtfLayout layout;
    layout.width = u16(bytes, 16);
    layout.height = u16(bytes, 18);
    const std::uint32_t flags = u32(bytes, 20);
    layout.frames = std::max<std::uint32_t>(1, u16(bytes, 24));
    layout.format = u32(bytes, 52);
    const int mipCount = std::max<int>(1, bytes[56]);
    const std::uint32_t lowFormat = u32(bytes, 57);
    const int lowWidth = bytes[61];
    const int lowHeight = bytes[62];
    layout.depth = (minor >= 2 && bytes.size() >= 65)
        ? std::max<int>(1, u16(bytes, 63)) : 1;
    if (layout.width <= 0 || layout.height <= 0)
        return fail("Invalid VTF dimensions");

    layout.cubemap = (flags & 0x00004000u) != 0;
    // VTF versions before 7.5 store an obsolete seventh spheremap face after
    // the six real cube faces. It still contributes to mip offsets even though
    // Source no longer samples it as part of the cubemap.
    layout.storedFaces = layout.cubemap ? (minor < 5 ? 7 : 6) : 1;

    layout.topMipOffset = headerSize + imageSize(lowFormat, lowWidth, lowHeight);
    if (minor >= 3 && bytes.size() >= 80) {
        const std::uint32_t resources = u32(bytes, 68);
        for (std::uint32_t index = 0; index < resources; ++index) {
            const std::size_t entry = 80u + static_cast<std::size_t>(index) * 8u;
            if (entry + 8 > bytes.size()) break;
            if (bytes[entry] == 0x30 && bytes[entry + 1] == 0 && bytes[entry + 2] == 0) {
                layout.topMipOffset = u32(bytes, entry + 4);
                break;
            }
        }
    }

    layout.mipCount = mipCount;
    layout.mipChainOffset = layout.topMipOffset;
    for (int mip = mipCount - 1; mip > 0; --mip) {
        const int width = std::max(1, layout.width >> mip);
        const int height = std::max(1, layout.height >> mip);
        const std::size_t size = imageSize(layout.format, width, height);
        if (!size)
            return fail("Unsupported VTF image format " + std::to_string(layout.format));
        layout.topMipOffset += size * layout.frames * layout.storedFaces *
                               std::max(1, layout.depth >> mip);
    }
    layout.topSurfaceSize = imageSize(layout.format, layout.width, layout.height);
    if (!layout.topSurfaceSize)
        return fail("Unsupported VTF image format " + std::to_string(layout.format));
    const std::size_t topMipBytes = layout.topSurfaceSize * layout.frames *
        static_cast<std::size_t>(layout.storedFaces) * static_cast<std::size_t>(layout.depth);
    if (layout.topMipOffset > bytes.size() || topMipBytes > bytes.size() - layout.topMipOffset)
        return fail("Truncated or unsupported VTF image data");
    return layout;
}

std::optional<Image> decodeVtfSurface(const std::vector<std::uint8_t>& bytes,
                                      const VtfLayout& layout,
                                      std::size_t offset,
                                      std::string* error)
{
    auto fail = [&](std::string message) -> std::optional<Image> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (offset > bytes.size() || layout.topSurfaceSize > bytes.size() - offset)
        return fail("Truncated VTF surface data");

    Image image{layout.width, layout.height,
                std::vector<std::uint32_t>(static_cast<std::size_t>(layout.width * layout.height))};
    const std::uint8_t* src = bytes.data() + offset;
    const std::size_t pixels = static_cast<std::size_t>(layout.width * layout.height);
    switch (layout.format) {
    case 0: // RGBA8888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*4], src[i*4+1], src[i*4+2], src[i*4+3]);
        break;
    case 1: // ABGR8888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*4+3], src[i*4+2], src[i*4+1], src[i*4]);
        break;
    case 2: // RGB888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*3], src[i*3+1], src[i*3+2]);
        break;
    case 3: // BGR888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*3+2], src[i*3+1], src[i*3]);
        break;
    case 11: // ARGB8888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*4+1], src[i*4+2], src[i*4+3], src[i*4]);
        break;
    case 12: // BGRA8888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*4+2], src[i*4+1], src[i*4], src[i*4+3]);
        break;
    case 4: // RGB565
        for (std::size_t i = 0; i < pixels; ++i) {
            const auto rgb = rgb565(static_cast<std::uint16_t>(src[i*2] | (src[i*2+1] << 8)));
            image.pixels[i] = argb(rgb[0], rgb[1], rgb[2]);
        }
        break;
    case 5: // I8
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i], src[i], src[i]);
        break;
    case 6: // IA88
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*2], src[i*2], src[i*2], src[i*2+1]);
        break;
    case 8: // A8
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(255, 255, 255, src[i]);
        break;
    case 9: // RGB888 bluescreen
        for (std::size_t i = 0; i < pixels; ++i) {
            const int r=src[i*3], g=src[i*3+1], b=src[i*3+2];
            image.pixels[i] = argb(r,g,b,(r==0 && g==0 && b==255) ? 0 : 255);
        }
        break;
    case 10: // BGR888 bluescreen
        for (std::size_t i = 0; i < pixels; ++i) {
            const int b=src[i*3], g=src[i*3+1], r=src[i*3+2];
            image.pixels[i] = argb(r,g,b,(r==0 && g==0 && b==255) ? 0 : 255);
        }
        break;
    case 16: // BGRX8888
        for (std::size_t i = 0; i < pixels; ++i) image.pixels[i] = argb(src[i*4+2], src[i*4+1], src[i*4]);
        break;
    case 17: // BGR565
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint16_t value = static_cast<std::uint16_t>(src[i*2] | (src[i*2+1] << 8));
            const int b=((value>>11)&31)*255/31, g=((value>>5)&63)*255/63, r=(value&31)*255/31;
            image.pixels[i] = argb(r,g,b);
        }
        break;
    case 18: // BGRX5551
    case 21: // BGRA5551
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint16_t value = static_cast<std::uint16_t>(src[i*2] | (src[i*2+1] << 8));
            const int b=((value>>10)&31)*255/31, g=((value>>5)&31)*255/31, r=(value&31)*255/31;
            const int a=(layout.format==21 && !(value&0x8000)) ? 0 : 255;
            image.pixels[i] = argb(r,g,b,a);
        }
        break;
    case 19: // BGRA4444
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint16_t value = static_cast<std::uint16_t>(src[i*2] | (src[i*2+1] << 8));
            const int b=((value>>12)&15)*17, g=((value>>8)&15)*17, r=((value>>4)&15)*17, a=(value&15)*17;
            image.pixels[i] = argb(r,g,b,a);
        }
        break;
    case 13: case 20:
        if (!decodeDxt1(src, layout.topSurfaceSize, image, layout.format == 20))
            return fail("Truncated DXT1 VTF");
        break;
    case 14:
        if (!decodeDxt3(src, layout.topSurfaceSize, image))
            return fail("Truncated DXT3 VTF");
        break;
    case 15:
        if (!decodeDxt5(src, layout.topSurfaceSize, image))
            return fail("Truncated DXT5 VTF");
        break;
    default:
        return fail("Unsupported VTF image format " + std::to_string(layout.format));
    }
    return image;
}

} // namespace

std::optional<Image> MaterialSystem::decodeVtf(const std::vector<std::uint8_t>& bytes,
                                               std::string* error,
                                               int maxDimension)
{
    const auto layout = parseVtfLayout(bytes, error);
    if (!layout) return std::nullopt;
    const bool wantsSmaller = maxDimension > 0 &&
        (layout->width > maxDimension || layout->height > maxDimension);

    std::optional<Image> decoded;
    if (wantsSmaller) {
        std::size_t offset = 0;
        const VtfLayout scaled = layoutForMaxDimension(*layout, maxDimension, &offset);
        if (scaled.topSurfaceSize)
            decoded = decodeVtfSurface(bytes, scaled, offset, error);
    }
    if (!decoded) decoded = decodeVtfSurface(bytes, *layout, layout->topMipOffset, error);

    // A mip is only a starting point: plenty of VTFs stop their chain early (and
    // some store no mips at all), so the smallest stored surface can still be far
    // larger than asked for - a 2048 square texture with four mips bottoms out at
    // 256. Finish the job in software so the cap is always honoured, otherwise the
    // resident size depends on how the artist exported the file. This is what made
    // a fresh 32 px load cost 5x a 64 px load stepped down to 32.
    if (decoded && maxDimension > 0 &&
        std::max(decoded->width, decoded->height) > maxDimension) {
        *decoded = downscaleImage(*decoded, maxDimension);
    }
    return decoded;
}

std::optional<std::vector<Image>> MaterialSystem::decodeVtfFrames(
    const std::vector<std::uint8_t>& bytes, std::string* error)
{
    const auto layout = parseVtfLayout(bytes, error);
    if (!layout) return std::nullopt;
    std::vector<Image> frames;
    frames.reserve(layout->frames);
    const std::size_t frameStride = layout->topSurfaceSize *
        static_cast<std::size_t>(layout->storedFaces) *
        static_cast<std::size_t>(layout->depth);
    for (std::uint32_t frame = 0; frame < layout->frames; ++frame) {
        const std::size_t offset = layout->topMipOffset +
            static_cast<std::size_t>(frame) * frameStride;
        auto decoded = decodeVtfSurface(bytes, *layout, offset, error);
        if (!decoded) return std::nullopt;
        frames.push_back(std::move(*decoded));
    }
    return frames;
}

std::optional<CubeImage> MaterialSystem::decodeVtfCubemap(
    const std::vector<std::uint8_t>& bytes, std::string* error)
{
    const auto layout = parseVtfLayout(bytes, error);
    if (!layout) return std::nullopt;
    if (!layout->cubemap || layout->storedFaces < 6) {
        if (error) *error = "VTF is not a cubemap";
        return std::nullopt;
    }

    CubeImage cube;
    // Top mip data is ordered by frame, then face, then depth. Source envmaps
    // use frame zero and depth slice zero for the ordinary material cubemap.
    for (std::size_t face = 0; face < cube.faces.size(); ++face) {
        const std::size_t offset = layout->topMipOffset +
            face * layout->topSurfaceSize * static_cast<std::size_t>(layout->depth);
        auto decoded = decodeVtfSurface(bytes, *layout, offset, error);
        if (!decoded) return std::nullopt;
        cube.faces[face] = std::move(*decoded);
    }
    return cube.valid() ? std::optional<CubeImage>(std::move(cube)) : std::nullopt;
}

} // namespace hammer::assets
