#include "CollabAssets.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string_view>

namespace hammer::collab {

namespace {

std::string normalizePath(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\') c = '/';
        out += char(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool isCustomFile(const hammer::assets::GameFileSystem& fs, const std::string& path)
{
    const auto source = fs.sourceForFile(path);
    return source && source->kind == hammer::assets::SearchLocation::Kind::Directory &&
           source->pathId == "custom";
}

// The VMT keys whose values name another texture file. Sourced from the
// material features this editor actually renders plus the common authoring
// keys; an unknown key at worst means one texture is not shared.
constexpr std::string_view kTextureKeys[] = {
    "$basetexture",  "$basetexture2", "$bumpmap",       "$bumpmap2",
    "$normalmap",    "$envmapmask",   "$detail",        "$selfillummask",
    "$selfillumtexture", "$phongexponenttexture", "$lightwarptexture",
    "$flowmap",      "$flow_noise_texture", "$texture2",
};

bool isTextureKey(std::string_view key)
{
    return std::any_of(std::begin(kTextureKeys), std::end(kTextureKeys),
                       [key](std::string_view candidate) { return candidate == key; });
}

void collectVmtValues(const hammer::vmf::Block& block, std::vector<std::string>& textures,
                      std::vector<std::string>& includes)
{
    for (const hammer::vmf::Entry& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::KeyValue) {
            const std::string key = normalizePath(entry.key);
            if (isTextureKey(key)) textures.push_back(normalizePath(entry.value));
            else if (key == "include") includes.push_back(normalizePath(entry.value));
        } else if (entry.child) {
            collectVmtValues(*entry.child, textures, includes);
        }
    }
}

struct Collector
{
    const hammer::assets::GameFileSystem& fs;
    std::set<std::string> result;

    void addIfCustom(const std::string& path)
    {
        if (isCustomFile(fs, path)) result.insert(path);
    }

    // A material by short name ("brick/wall01") or a full VMT path from an
    // "include". Adds the VMT and its textures; follows patch includes one
    // extra level so PATCH-over-PATCH stops rather than recursing forever.
    void addMaterial(const std::string& nameOrPath, int depth)
    {
        std::string vmt = normalizePath(nameOrPath);
        if (vmt.rfind("materials/", 0) != 0) vmt = "materials/" + vmt;
        if (vmt.size() < 4 || vmt.substr(vmt.size() - 4) != ".vmt") vmt += ".vmt";
        addIfCustom(vmt);

        const auto bytes = fs.readFile(vmt);
        if (!bytes) return;
        const std::string text(bytes->begin(), bytes->end());
        const auto parsed = hammer::vmf::Document::parse(text);
        if (!parsed) return;

        std::vector<std::string> textures;
        std::vector<std::string> includes;
        for (const hammer::vmf::Block& root : parsed->roots())
            collectVmtValues(root, textures, includes);
        for (const std::string& texture : textures) {
            std::string vtf = texture;
            if (vtf.rfind("materials/", 0) != 0) vtf = "materials/" + vtf;
            if (vtf.size() < 4 || vtf.substr(vtf.size() - 4) != ".vtf") vtf += ".vtf";
            addIfCustom(vtf);
        }
        if (depth > 0) {
            for (const std::string& include : includes) addMaterial(include, depth - 1);
        }
    }

    void addModel(const std::string& modelPath)
    {
        std::string mdl = normalizePath(modelPath);
        if (mdl.size() < 4 || mdl.substr(mdl.size() - 4) != ".mdl") return;
        const std::string stem = mdl.substr(0, mdl.size() - 4);
        addIfCustom(mdl);
        for (const char* suffix : {".vvd", ".phy", ".dx90.vtx", ".dx80.vtx", ".sw.vtx"})
            addIfCustom(stem + suffix);
    }
};

void walkDocument(const hammer::vmf::Block& block, Collector& collector)
{
    const bool isSide = block.name == "side";
    for (const hammer::vmf::Entry& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::KeyValue) {
            const std::string key = normalizePath(entry.key);
            if (entry.value.empty()) continue;
            if ((isSide && key == "material") || key == "texture") {
                collector.addMaterial(entry.value, 1);
            } else if (!isSide && key == "material") {
                // Entity $material keys (decals, sprites) name materials too.
                collector.addMaterial(entry.value, 1);
            } else if (key == "model") {
                collector.addModel(entry.value);
            }
        } else if (entry.child) {
            walkDocument(*entry.child, collector);
        }
    }
}

// The keys walkDocument treats as asset references, without resolving them.
void walkReferences(const hammer::vmf::Block& block, std::vector<std::string>& out)
{
    const bool isSide = block.name == "side";
    for (const hammer::vmf::Entry& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::KeyValue) {
            if (entry.value.empty()) continue;
            const std::string key = normalizePath(entry.key);
            if (key == "material" || key == "texture")
                out.push_back("m:" + normalizePath(entry.value));
            else if (key == "model")
                out.push_back("d:" + normalizePath(entry.value));
            (void)isSide;
        } else if (entry.child) {
            walkReferences(*entry.child, out);
        }
    }
}

} // namespace

std::vector<std::string> collectAssetReferences(const hammer::vmf::Document& document)
{
    std::vector<std::string> references;
    for (const hammer::vmf::Block& root : document.roots()) walkReferences(root, references);
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()), references.end());
    return references;
}

bool isSafeAssetPath(const std::string& path)
{
    if (path.empty() || path.size() > 512) return false;
    if (path.find('\\') != std::string::npos) return false;
    if (path.front() == '/' || path.find(':') != std::string::npos) return false;
    for (char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return false;
    }
    // Segment check: no "", ".", "..".
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view segment(path.data() + start,
                                       (slash == std::string::npos ? path.size() : slash) - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return path.rfind("materials/", 0) == 0 || path.rfind("models/", 0) == 0;
}

std::vector<std::string> collectCustomAssetPaths(const hammer::vmf::Document& document,
                                                 const hammer::assets::GameFileSystem& fs)
{
    Collector collector{fs, {}};
    for (const hammer::vmf::Block& root : document.roots()) walkDocument(root, collector);
    std::vector<std::string> paths;
    for (const std::string& path : collector.result) {
        if (isSafeAssetPath(path)) paths.push_back(path);
    }
    return paths;
}

} // namespace hammer::collab
