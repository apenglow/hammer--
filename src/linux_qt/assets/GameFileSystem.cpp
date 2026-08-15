#include "GameFileSystem.hpp"

#include "VmfDocument.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace hammer::assets {
namespace {
constexpr std::uint32_t VpkSignature = 0x55AA1234u;
constexpr std::uint16_t VpkDirectoryArchive = 0x7FFFu;
constexpr std::uint16_t VpkEntryTerminator = 0xFFFFu;

std::string lower(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool iequals(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

const hammer::vmf::Block* childCi(const hammer::vmf::Block& block, std::string_view name)
{
    for (const hammer::vmf::Entry& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::ChildBlock && entry.child && iequals(entry.child->name, name)) {
            return entry.child.get();
        }
    }
    return nullptr;
}

const std::string* valueCi(const hammer::vmf::Block& block, std::string_view key)
{
    for (const hammer::vmf::Entry& entry : block.entries) {
        if (entry.kind == hammer::vmf::Entry::Kind::KeyValue && iequals(entry.key, key)) return &entry.value;
    }
    return nullptr;
}

int intValueCi(const hammer::vmf::Block& block, std::string_view key)
{
    const std::string* value = valueCi(block, key);
    if (!value) return 0;
    try { return std::stoi(*value); } catch (...) { return 0; }
}

std::optional<std::vector<std::uint8_t>> readBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) return std::nullopt;
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream && !bytes.empty()) return std::nullopt;
    return bytes;
}

template<typename T>
bool readLittle(std::istream& stream, T& value)
{
    std::array<unsigned char, sizeof(T)> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) return false;
    value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<T>(bytes[i]) << (i * 8);
    }
    return true;
}

bool readCString(std::istream& stream, std::string& value)
{
    value.clear();
    char ch = 0;
    while (stream.get(ch)) {
        if (ch == '\0') return true;
        value.push_back(ch);
        if (value.size() > 65535) return false;
    }
    return false;
}

std::filesystem::path weakCanonical(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

std::optional<std::filesystem::path> findCaseInsensitive(const std::filesystem::path& root,
                                                         std::string_view relative)
{
    std::filesystem::path current = root;
    std::string normalized(relative);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::stringstream parts(normalized);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            current = current.parent_path();
            continue;
        }
        const std::filesystem::path direct = current / part;
        std::error_code ec;
        if (std::filesystem::exists(direct, ec)) {
            current = direct;
            continue;
        }
        bool found = false;
        if (!std::filesystem::is_directory(current, ec)) return std::nullopt;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
            if (ec) break;
            if (iequals(entry.path().filename().string(), part)) {
                current = entry.path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return current;
}

bool endsWithCi(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           iequals(text.substr(text.size() - suffix.size()), suffix);
}

bool sourcePathExists(const std::filesystem::path& path)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return true;
    if (endsWithCi(path.filename().string(), ".vpk") &&
        !endsWithCi(path.filename().string(), "_dir.vpk")) {
        const auto alternate = path.parent_path() / (path.stem().string() + "_dir.vpk");
        ec.clear();
        return std::filesystem::is_regular_file(alternate, ec);
    }
    return false;
}

std::string joinResource(std::string_view path, std::string_view file, std::string_view extension)
{
    std::string result;
    if (!path.empty() && path != " ") {
        result.assign(path);
        result.push_back('/');
    }
    result.append(file);
    if (!extension.empty() && extension != " ") {
        result.push_back('.');
        result.append(extension);
    }
    return GameFileSystem::normalizeResourcePath(result);
}

std::filesystem::path resolveSearchPath(std::string location,
                                        const std::filesystem::path& gameDir,
                                        const std::filesystem::path& primaryRoot,
                                        const std::vector<std::filesystem::path>& dependencyRoots,
                                        const SteamLibraries& steam,
                                        int* resolvedAppId)
{
    *resolvedAppId = 0;
    std::replace(location.begin(), location.end(), '\\', '/');
    const std::string lowerLocation = lower(location);
    constexpr std::string_view GameInfoToken = "|gameinfo_path|";
    constexpr std::string_view AllSourceToken = "|all_source_engine_paths|";
    constexpr std::string_view AppIdToken = "|appid_";

    if (lowerLocation.rfind(GameInfoToken, 0) == 0) {
        return weakCanonical(gameDir / location.substr(GameInfoToken.size()));
    }
    if (lowerLocation.rfind(AllSourceToken, 0) == 0) {
        return weakCanonical(primaryRoot / location.substr(AllSourceToken.size()));
    }
    if (lowerLocation.rfind(AppIdToken, 0) == 0) {
        const std::size_t end = lowerLocation.find('|', AppIdToken.size());
        if (end != std::string::npos) {
            try {
                *resolvedAppId = std::stoi(lowerLocation.substr(AppIdToken.size(), end - AppIdToken.size()));
            } catch (...) { *resolvedAppId = 0; }
            if (*resolvedAppId > 0) {
                if (auto app = steam.resolveApp(*resolvedAppId)) {
                    return weakCanonical(*app / location.substr(end + 1));
                }
            }
        }
    }

    const std::filesystem::path raw(location);
    if (raw.is_absolute()) return weakCanonical(raw);

    std::vector<std::filesystem::path> bases;
    if (!primaryRoot.empty()) bases.push_back(primaryRoot);
    bases.insert(bases.end(), dependencyRoots.begin(), dependencyRoots.end());
    bases.push_back(gameDir.parent_path());
    bases.push_back(gameDir);
    for (const auto& base : bases) {
        const auto candidate = weakCanonical(base / raw);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return weakCanonical((!primaryRoot.empty() ? primaryRoot : gameDir.parent_path()) / raw);
}

struct ResolvedSearchPath
{
    std::filesystem::path path;
    int appId{0};
    bool optional{false};
};

std::vector<ResolvedSearchPath> resolveSearchPaths(std::string location,
                                                   const std::filesystem::path& gameDir,
                                                   const std::filesystem::path& primaryRoot,
                                                   const std::vector<std::filesystem::path>& dependencyRoots,
                                                   const SteamLibraries& steam)
{
    std::replace(location.begin(), location.end(), '\\', '/');
    const std::string lowerLocation = lower(location);
    constexpr std::string_view AppIdToken = "|appid_";

    std::vector<ResolvedSearchPath> results;
    auto append = [&](std::filesystem::path path, int appId, bool optional) {
        path = weakCanonical(path);
        const auto existing = std::find_if(results.begin(), results.end(), [&](const ResolvedSearchPath& item) {
            return item.path == path;
        });
        if (existing != results.end()) {
            // A later required resolution upgrades an earlier optional probe.
            existing->optional = existing->optional && optional;
            if (existing->appId == 0) existing->appId = appId;
            return;
        }
        results.push_back({std::move(path), appId, optional});
    };

    if (lowerLocation.rfind(AppIdToken, 0) == 0) {
        const std::size_t end = lowerLocation.find('|', AppIdToken.size());
        if (end != std::string::npos) {
            int appId = 0;
            try {
                appId = std::stoi(lowerLocation.substr(AppIdToken.size(), end - AppIdToken.size()));
            } catch (...) { appId = 0; }

            if (appId > 0) {
                const std::filesystem::path suffix(location.substr(end + 1));

                // Standalone Source mods and SDK games often ship local copies of
                // app content (for example tf/, hl2/, or platform/) next to or
                // inside the selected game directory. Mount those loose roots
                // before external Steam content so local files can override it.
                append(gameDir / suffix, appId, true);
                append(gameDir.parent_path() / suffix, appId, true);

                // Do not collapse an AppID to a single Steam library. Steam can
                // leave an older manifest/install behind after a library move, and
                // Source mods frequently depend on content split across libraries.
                // Keep every valid install candidate and prefer candidates that
                // actually contain this specific suffix (including name_dir.vpk).
                const auto appRoots = steam.resolveApps(appId);
                bool foundRequestedContent = false;
                for (const auto& appRoot : appRoots) {
                    const auto candidate = appRoot / suffix;
                    const bool present = sourcePathExists(candidate);
                    append(candidate, appId, !present);
                    foundRequestedContent = foundRequestedContent || present;
                }

                // If manifests are absent or stale, make one conservative fallback
                // pass over installed Steam applications and accept only an app
                // root that contains the exact requested Source content path.
                if (!foundRequestedContent) {
                    for (const auto& library : steam.libraries()) {
                        const auto common = library / "steamapps/common";
                        std::error_code ec;
                        if (!std::filesystem::is_directory(common, ec)) continue;
                        for (const auto& installed : std::filesystem::directory_iterator(common, ec)) {
                            if (ec) break;
                            if (!installed.is_directory(ec)) continue;
                            const auto candidate = installed.path() / suffix;
                            if (sourcePathExists(candidate)) {
                                append(candidate, appId, false);
                                foundRequestedContent = true;
                            }
                        }
                    }
                }

                // Preserve a useful warning when no candidate contains the path.
                if (!foundRequestedContent && !appRoots.empty()) {
                    append(appRoots.front() / suffix, appId, false);
                }
                return results;
            }
        }
    }

    int appId = 0;
    append(resolveSearchPath(std::move(location), gameDir, primaryRoot, dependencyRoots, steam, &appId),
           appId, false);
    return results;
}
} // namespace

VpkArchive::VpkArchive(std::filesystem::path directoryFile) { open(directoryFile); }

bool VpkArchive::open(const std::filesystem::path& directoryFile, AssetError* error)
{
    entries_.clear();
    directoryFile_ = directoryFile;
    std::ifstream stream(directoryFile_, std::ios::binary);
    if (!stream) {
        if (error) error->message = "Unable to open VPK directory: " + directoryFile_.string();
        return false;
    }

    std::uint32_t signature = 0;
    if (!readLittle(stream, signature) || !readLittle(stream, version_) || !readLittle(stream, treeSize_) ||
        signature != VpkSignature || (version_ != 1 && version_ != 2)) {
        if (error) error->message = "Invalid or unsupported VPK header: " + directoryFile_.string();
        return false;
    }
    headerSize_ = version_ == 1 ? 12u : 28u;
    if (version_ == 2) {
        std::uint32_t ignored = 0;
        for (int i = 0; i < 4; ++i) if (!readLittle(stream, ignored)) return false;
    }

    const std::streamoff treeEnd = static_cast<std::streamoff>(headerSize_ + treeSize_);
    while (stream && stream.tellg() < treeEnd) {
        std::string extension;
        if (!readCString(stream, extension)) break;
        if (extension.empty()) break;
        while (stream && stream.tellg() < treeEnd) {
            std::string path;
            if (!readCString(stream, path)) break;
            if (path.empty()) break;
            while (stream && stream.tellg() < treeEnd) {
                std::string filename;
                if (!readCString(stream, filename)) break;
                if (filename.empty()) break;
                Entry entry;
                std::uint16_t terminator = 0;
                if (!readLittle(stream, entry.crc) || !readLittle(stream, entry.preloadBytes) ||
                    !readLittle(stream, entry.archiveIndex) || !readLittle(stream, entry.offset) ||
                    !readLittle(stream, entry.length) || !readLittle(stream, terminator) ||
                    terminator != VpkEntryTerminator) {
                    if (error) error->message = "Malformed VPK tree entry in " + directoryFile_.string();
                    entries_.clear();
                    return false;
                }
                entry.preload.resize(entry.preloadBytes);
                if (entry.preloadBytes) {
                    stream.read(reinterpret_cast<char*>(entry.preload.data()), entry.preloadBytes);
                    if (!stream) return false;
                }
                entries_[joinResource(path, filename, extension)] = std::move(entry);
            }
        }
    }
    if (entries_.empty()) {
        if (error) error->message = "VPK contains no readable directory entries: " + directoryFile_.string();
        return false;
    }
    return true;
}

std::filesystem::path VpkArchive::archivePath(std::uint16_t archiveIndex) const
{
    std::string stem = directoryFile_.filename().string();
    const std::string suffix = "_dir.vpk";
    if (endsWithCi(stem, suffix)) stem.resize(stem.size() - suffix.size());
    std::ostringstream name;
    name << stem << '_' << std::setfill('0') << std::setw(3) << archiveIndex << ".vpk";
    return directoryFile_.parent_path() / name.str();
}

std::optional<std::vector<std::uint8_t>> VpkArchive::read(std::string_view relativePath) const
{
    const auto found = entries_.find(GameFileSystem::normalizeResourcePath(relativePath));
    if (found == entries_.end()) return std::nullopt;
    const Entry& entry = found->second;
    std::vector<std::uint8_t> result = entry.preload;
    if (!entry.length) return result;

    const std::filesystem::path dataPath = entry.archiveIndex == VpkDirectoryArchive
        ? directoryFile_ : archivePath(entry.archiveIndex);
    std::ifstream stream(dataPath, std::ios::binary);
    if (!stream) return std::nullopt;
    const std::uint64_t base = entry.archiveIndex == VpkDirectoryArchive
        ? static_cast<std::uint64_t>(headerSize_) + treeSize_ : 0u;
    stream.seekg(static_cast<std::streamoff>(base + entry.offset), std::ios::beg);
    if (!stream) return std::nullopt;
    const std::size_t oldSize = result.size();
    result.resize(oldSize + entry.length);
    stream.read(reinterpret_cast<char*>(result.data() + oldSize), entry.length);
    if (!stream) return std::nullopt;
    return result;
}

bool VpkArchive::contains(std::string_view relativePath) const
{
    return entries_.contains(GameFileSystem::normalizeResourcePath(relativePath));
}

std::vector<std::string> VpkArchive::files() const
{
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& [path, entry] : entries_) {
        (void)entry;
        result.push_back(path);
    }
    std::sort(result.begin(), result.end());
    return result;
}

SteamLibraries::SteamLibraries() : roots_(defaultRoots()) { discover(); }
SteamLibraries::SteamLibraries(std::vector<std::filesystem::path> roots) : roots_(std::move(roots)) { discover(); }

std::vector<std::filesystem::path> SteamLibraries::defaultRoots()
{
    std::vector<std::filesystem::path> result;
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path h(home);
        result.push_back(h / ".local/share/Steam");
        result.push_back(h / ".steam/steam");
        result.push_back(h / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
    }
    return result;
}

void SteamLibraries::discover()
{
    std::set<std::filesystem::path> unique;
    for (const auto& root : roots_) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root / "steamapps", ec)) continue;
        unique.insert(weakCanonical(root));
        const auto file = root / "steamapps/libraryfolders.vdf";
        hammer::vmf::ParseError parseError;
        std::string ioError;
        const auto document = hammer::vmf::Document::load(file, &parseError, &ioError);
        if (!document) continue;
        const hammer::vmf::Block* libraries = nullptr;
        for (const auto& candidate : document->roots()) {
            if (iequals(candidate.name, "libraryfolders")) { libraries = &candidate; break; }
        }
        if (!libraries) continue;
        for (const hammer::vmf::Entry& entry : libraries->entries) {
            std::string path;
            if (entry.kind == hammer::vmf::Entry::Kind::ChildBlock && entry.child) {
                if (const std::string* value = valueCi(*entry.child, "path")) path = *value;
            } else if (entry.kind == hammer::vmf::Entry::Kind::KeyValue) {
                if (std::all_of(entry.key.begin(), entry.key.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    path = entry.value;
                }
            }
            if (path.empty()) continue;
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::filesystem::path library(path);
            if (std::filesystem::is_directory(library / "steamapps", ec)) unique.insert(weakCanonical(library));
        }
    }
    libraries_.assign(unique.begin(), unique.end());
}

std::vector<std::filesystem::path> SteamLibraries::resolveApps(int appId) const
{
    std::vector<std::filesystem::path> results;
    if (appId <= 0) return results;

    auto append = [&](const std::filesystem::path& candidate) {
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec)) return;
        const auto canonical = weakCanonical(candidate);
        if (std::find(results.begin(), results.end(), canonical) == results.end()) {
            results.push_back(canonical);
        }
    };

    // Collect every install directory named by an AppID manifest. Then test
    // each name against every Steam library, which recovers cleanly from a
    // moved installation with a stale manifest left in another library.
    std::vector<std::string> installDirs;
    for (const auto& library : libraries_) {
        const auto manifest = library / "steamapps" / ("appmanifest_" + std::to_string(appId) + ".acf");
        hammer::vmf::ParseError parseError;
        std::string ioError;
        const auto document = hammer::vmf::Document::load(manifest, &parseError, &ioError);
        if (!document || document->roots().empty()) continue;
        const hammer::vmf::Block& appState = document->roots().front();
        const std::string* installDir = valueCi(appState, "installdir");
        if (!installDir || installDir->empty()) continue;
        if (std::find(installDirs.begin(), installDirs.end(), *installDir) == installDirs.end()) {
            installDirs.push_back(*installDir);
        }
        append(library / "steamapps/common" / *installDir);
    }

    for (const std::string& installDir : installDirs) {
        for (const auto& library : libraries_) {
            append(library / "steamapps/common" / installDir);
        }
    }
    return results;
}

std::optional<std::filesystem::path> SteamLibraries::resolveApp(int appId) const
{
    const auto apps = resolveApps(appId);
    if (apps.empty()) return std::nullopt;
    return apps.front();
}

std::string GameFileSystem::normalizeResourcePath(std::string_view path)
{
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    while (result.rfind("./", 0) == 0) result.erase(0, 2);
    while (!result.empty() && result.front() == '/') result.erase(result.begin());
    std::string collapsed;
    bool slash = false;
    for (unsigned char ch : result) {
        if (ch == '/') {
            if (!slash) collapsed.push_back('/');
            slash = true;
        } else {
            collapsed.push_back(static_cast<char>(std::tolower(ch)));
            slash = false;
        }
    }
    return collapsed;
}

void GameFileSystem::addDirectory(const std::filesystem::path& directory, std::string pathId, int appId)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return;
    const auto path = weakCanonical(directory);
    const bool duplicate = std::any_of(locations_.begin(), locations_.end(), [&](const SearchLocation& item) {
        return item.kind == SearchLocation::Kind::Directory && item.path == path;
    });
    if (!duplicate) locations_.push_back({SearchLocation::Kind::Directory, path, std::move(pathId), appId});
}

bool GameFileSystem::addVpk(const std::filesystem::path& path, std::string pathId, int appId,
                            AssetError* /*error*/, bool optional)
{
    std::error_code ec;
    std::filesystem::path directoryFile = path;

    // Source gameinfo files conventionally name a VPK as "name.vpk", while
    // the archive directory on disk is "name_dir.vpk". Accept both forms.
    if (!std::filesystem::is_regular_file(directoryFile, ec) &&
        endsWithCi(directoryFile.filename().string(), ".vpk") &&
        !endsWithCi(directoryFile.filename().string(), "_dir.vpk")) {
        const std::filesystem::path alternate =
            directoryFile.parent_path() /
            (directoryFile.stem().string() + "_dir.vpk");
        ec.clear();
        if (std::filesystem::is_regular_file(alternate, ec)) directoryFile = alternate;
    }

    ec.clear();
    if (!std::filesystem::is_regular_file(directoryFile, ec)) {
        if (!optional) {
            std::string warning = "VPK search path was not found and was skipped: " + path.string();
            if (appId > 0) warning += " (Steam AppID " + std::to_string(appId) + ")";
            warnings_.push_back(std::move(warning));
        }
        return true;
    }

    const auto canonical = weakCanonical(directoryFile);
    if (std::any_of(vpks_.begin(), vpks_.end(), [&](const auto& vpk) { return vpk->path() == canonical; })) return true;
    auto archive = std::make_shared<VpkArchive>();
    AssetError localError;
    if (!archive->open(canonical, &localError)) {
        std::string warning = localError.message.empty()
            ? "Unable to mount VPK archive: " + canonical.string()
            : localError.message;
        warnings_.push_back(std::move(warning));
        return true;
    }
    vpks_.push_back(archive);
    locations_.push_back({SearchLocation::Kind::Vpk, canonical, std::move(pathId), appId});
    return true;
}

void GameFileSystem::addDirectoryVpks(const std::filesystem::path& directory, const std::string& pathId,
                                      int appId, AssetError* error)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return;
    std::vector<std::filesystem::path> vpks;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (endsWithCi(name, "_dir.vpk")) vpks.push_back(entry.path());
    }
    std::sort(vpks.begin(), vpks.end(), [](const auto& a, const auto& b) {
        return lower(a.filename().string()) < lower(b.filename().string());
    });
    for (const auto& vpk : vpks) addVpk(vpk, pathId, appId, error, true);
}

bool GameFileSystem::configure(const std::filesystem::path& input, AssetError* error,
                               const SteamLibraries* steamOverride)
{
    if (error) error->message.clear();
    locations_.clear();
    vpks_.clear();
    warnings_.clear();
    steamAppId_ = 0;
    gameInfoFile_ = std::filesystem::is_directory(input) ? input / "gameinfo.txt" : input;
    gameInfoFile_ = weakCanonical(gameInfoFile_);
    gameDirectory_ = gameInfoFile_.parent_path();

    hammer::vmf::ParseError parseError;
    std::string ioError;
    const auto document = hammer::vmf::Document::load(gameInfoFile_, &parseError, &ioError);
    if (!document) {
        if (error) error->message = !ioError.empty() ? ioError :
            "gameinfo.txt parse error at " + std::to_string(parseError.line) + ":" +
            std::to_string(parseError.column) + ": " + parseError.message;
        return false;
    }
    const hammer::vmf::Block* gameInfo = nullptr;
    for (const auto& root : document->roots()) if (iequals(root.name, "gameinfo")) { gameInfo = &root; break; }
    if (!gameInfo) {
        if (error) error->message = "gameinfo.txt has no GameInfo root block";
        return false;
    }
    const hammer::vmf::Block* fileSystem = childCi(*gameInfo, "filesystem");
    const hammer::vmf::Block* searchPaths = fileSystem ? childCi(*fileSystem, "searchpaths") : nullptr;
    if (!fileSystem || !searchPaths) {
        if (error) error->message = "gameinfo.txt is missing FileSystem/SearchPaths";
        return false;
    }

    SteamLibraries discovered;
    const SteamLibraries& steam = steamOverride ? *steamOverride : discovered;
    steamAppId_ = intValueCi(*fileSystem, "steamappid");
    std::filesystem::path primaryRoot;
    if (auto app = steam.resolveApp(steamAppId_)) primaryRoot = *app;

    std::vector<int> dependencyIds;
    std::set<int> referencedAppIds;
    if (steamAppId_ > 0) referencedAppIds.insert(steamAppId_);
    const int dependsOnAppId = intValueCi(*gameInfo, "dependsonappid");
    if (dependsOnAppId > 0) {
        referencedAppIds.insert(dependsOnAppId);
        if (dependsOnAppId != steamAppId_) dependencyIds.push_back(dependsOnAppId);
    }
    for (const hammer::vmf::Entry& entry : fileSystem->entries) {
        if (entry.kind != hammer::vmf::Entry::Kind::KeyValue) continue;
        const std::string key = lower(entry.key);
        if (key.rfind("additionalcontentid", 0) == 0 || key.rfind("toolsappid", 0) == 0) {
            try {
                const int id = std::stoi(entry.value);
                if (id > 0) {
                    referencedAppIds.insert(id);
                    if (id != steamAppId_) dependencyIds.push_back(id);
                }
            } catch (...) {}
        }
    }
    std::sort(dependencyIds.begin(), dependencyIds.end());
    dependencyIds.erase(std::unique(dependencyIds.begin(), dependencyIds.end()), dependencyIds.end());
    std::vector<std::filesystem::path> dependencyRoots;
    for (int id : dependencyIds) if (auto app = steam.resolveApp(id)) dependencyRoots.push_back(*app);

    for (const hammer::vmf::Entry& entry : searchPaths->entries) {
        if (entry.kind != hammer::vmf::Entry::Kind::KeyValue) continue;
        const std::string pathId = entry.key;
        std::string location = entry.value;
        const std::string loweredLocation = lower(location);
        constexpr std::string_view AppIdToken = "|appid_";
        if (loweredLocation.rfind(AppIdToken, 0) == 0) {
            const std::size_t end = loweredLocation.find('|', AppIdToken.size());
            if (end != std::string::npos) {
                try {
                    const int id = std::stoi(loweredLocation.substr(AppIdToken.size(), end - AppIdToken.size()));
                    if (id > 0) referencedAppIds.insert(id);
                } catch (...) {}
            }
        }
        const bool wildcard = endsWithCi(location, "/*") || endsWithCi(location, "\\*");
        if (wildcard) location.resize(location.size() - 2);
        const auto resolvedPaths = resolveSearchPaths(location, gameDirectory_, primaryRoot,
                                                      dependencyRoots, steam);

        for (const ResolvedSearchPath& candidate : resolvedPaths) {
            const int appId = candidate.appId > 0 ? candidate.appId : steamAppId_;
            const std::filesystem::path& resolved = candidate.path;

            if (wildcard) {
                std::error_code ec;
                std::vector<std::filesystem::path> children;
                if (std::filesystem::is_directory(resolved, ec)) {
                    for (const auto& child : std::filesystem::directory_iterator(resolved, ec)) {
                        if (ec) break;
                        if (child.is_directory(ec) ||
                            (child.is_regular_file(ec) &&
                             endsWithCi(child.path().filename().string(), "_dir.vpk"))) {
                            children.push_back(child.path());
                        }
                    }
                }
                std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
                    return lower(a.filename().string()) < lower(b.filename().string());
                });
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    if (std::filesystem::is_directory(*it, ec)) {
                        addDirectory(*it, pathId, appId);
                        addDirectoryVpks(*it, pathId, appId, error);
                    } else {
                        addVpk(*it, pathId, appId, error, true);
                    }
                }
                continue;
            }

            if (endsWithCi(resolved.filename().string(), ".vpk")) {
                if (!addVpk(resolved, pathId, appId, error, candidate.optional)) return false;
            } else {
                std::error_code ec;
                if (!std::filesystem::is_directory(resolved, ec)) {
                    if (!candidate.optional) {
                        std::string warning = "Directory search path was not found and was skipped: " + resolved.string();
                        if (appId > 0) warning += " (Steam AppID " + std::to_string(appId) + ")";
                        warnings_.push_back(std::move(warning));
                    }
                    continue;
                }
                addDirectory(resolved, pathId, appId);
                addDirectoryVpks(resolved, pathId, appId, error);
            }
        }
    }

    // TF2 ships the Half-Life 2 material scripts and textures as a paired
    // set of VPKs below its hl2/ directory. Some installations have stale or
    // incomplete app manifests even though the depots are present. If AppID
    // 440 is referenced, make one final content-based pass and mount the
    // actual pair at low priority. This is deliberately appended after the
    // declared SearchPaths so it never overrides mod content.
    if (referencedAppIds.contains(440)) {
        std::vector<std::filesystem::path> tfRoots = steam.resolveApps(440);
        for (const auto& library : steam.libraries()) {
            const std::filesystem::path common = library / "steamapps/common";
            std::error_code ec;
            const std::filesystem::path conventional = common / "Team Fortress 2";
            if (std::filesystem::is_directory(conventional, ec)) tfRoots.push_back(conventional);
            ec.clear();
            if (!std::filesystem::is_directory(common, ec)) continue;
            for (const auto& installed : std::filesystem::directory_iterator(common, ec)) {
                if (ec) break;
                if (!installed.is_directory(ec)) continue;
                const auto hl2 = installed.path() / "hl2";
                if (sourcePathExists(hl2 / "hl2_misc.vpk") ||
                    sourcePathExists(hl2 / "hl2_textures.vpk")) {
                    tfRoots.push_back(installed.path());
                }
            }
        }
        std::set<std::filesystem::path> seenRoots;
        for (const auto& root : tfRoots) {
            const auto canonicalRoot = weakCanonical(root);
            if (!seenRoots.insert(canonicalRoot).second) continue;
            const auto hl2 = canonicalRoot / "hl2";
            addVpk(hl2 / "hl2_misc.vpk", "game", 440, error, true);
            addVpk(hl2 / "hl2_textures.vpk", "game", 440, error, true);
            addDirectory(hl2, "game", 440);
        }
        if (!exists("materials/brick/brickwall001a.vmt")) {
            warnings_.push_back(
                "TF2 AppID 440 was referenced, but the HL2 material probe "
                "materials/brick/brickwall001a.vmt was not found after mounting "
                "hl2_misc_dir.vpk and hl2_textures_dir.vpk");
        }
    }

    // A selected game directory is always a valid loose-content root. Most
    // gameinfo files list |gameinfo_path|., but mounting it defensively also
    // supports older or customized files that omit the explicit entry. It is
    // appended only when it was not already mounted, so declared search order
    // remains authoritative.
    addDirectory(gameDirectory_, "game+mod", steamAppId_);
    addDirectoryVpks(gameDirectory_, "game+mod", steamAppId_, error);

    if (locations_.empty()) {
        if (error) error->message = "No usable game search paths were resolved from " + gameInfoFile_.string();
        return false;
    }
    return true;
}

bool GameFileSystem::mountOverrideDirectory(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return false;
    const auto path = weakCanonical(directory);
    locations_.erase(std::remove_if(locations_.begin(), locations_.end(),
                                    [&](const SearchLocation& item) {
                                        return item.kind == SearchLocation::Kind::Directory &&
                                               item.path == path;
                                    }),
                     locations_.end());
    locations_.insert(locations_.begin(),
                      {SearchLocation::Kind::Directory, path, "custom", steamAppId_});
    return true;
}

std::optional<std::vector<std::uint8_t>> GameFileSystem::readFile(std::string_view relativePath) const
{
    const std::string normalized = normalizeResourcePath(relativePath);
    for (const SearchLocation& location : locations_) {
        if (location.kind == SearchLocation::Kind::Directory) {
            const auto found = findCaseInsensitive(location.path, normalized);
            if (found) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(*found, ec)) {
                    if (auto bytes = readBytes(*found)) return bytes;
                }
            }
        } else {
            const auto archive = std::find_if(vpks_.begin(), vpks_.end(), [&](const auto& vpk) {
                return vpk->path() == location.path;
            });
            if (archive != vpks_.end()) if (auto bytes = (*archive)->read(normalized)) return bytes;
        }
    }
    return std::nullopt;
}

std::optional<SearchLocation> GameFileSystem::sourceForFile(std::string_view relativePath) const
{
    const std::string normalized = normalizeResourcePath(relativePath);
    for (const SearchLocation& location : locations_) {
        if (location.kind == SearchLocation::Kind::Directory) {
            const auto found = findCaseInsensitive(location.path, normalized);
            if (!found) continue;
            std::error_code ec;
            if (std::filesystem::is_regular_file(*found, ec)) return location;
        } else {
            const auto archive = std::find_if(vpks_.begin(), vpks_.end(), [&](const auto& vpk) {
                return vpk->path() == location.path;
            });
            if (archive != vpks_.end() && (*archive)->contains(normalized)) return location;
        }
    }
    return std::nullopt;
}

std::vector<std::string> GameFileSystem::listFiles(std::string_view prefix,
                                                    std::string_view extension) const
{
    const std::string normalizedPrefix = normalizeResourcePath(prefix);
    std::string normalizedExtension = lower(extension);
    if (!normalizedExtension.empty() && normalizedExtension.front() != '.') {
        normalizedExtension.insert(normalizedExtension.begin(), '.');
    }

    auto matches = [&](const std::string& path) {
        if (!normalizedPrefix.empty() && path.rfind(normalizedPrefix, 0) != 0) return false;
        if (!normalizedExtension.empty() && !endsWithCi(path, normalizedExtension)) return false;
        return true;
    };

    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const SearchLocation& location : locations_) {
        if (location.kind == SearchLocation::Kind::Vpk) {
            const auto archive = std::find_if(vpks_.begin(), vpks_.end(), [&](const auto& vpk) {
                return vpk->path() == location.path;
            });
            if (archive == vpks_.end()) continue;
            for (const std::string& file : (*archive)->files()) {
                if (matches(file) && seen.insert(file).second) result.push_back(file);
            }
            continue;
        }

        std::error_code ec;
        std::filesystem::path scanRoot = location.path;
        std::string scanPrefix;
        if (!normalizedPrefix.empty()) {
            const auto prefixed = findCaseInsensitive(location.path, normalizedPrefix);
            if (!prefixed || !std::filesystem::is_directory(*prefixed, ec)) continue;
            scanRoot = *prefixed;
            scanPrefix = normalizedPrefix;
            if (!scanPrefix.empty() && scanPrefix.back() != '/') scanPrefix.push_back('/');
        }
        if (!std::filesystem::is_directory(scanRoot, ec)) continue;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(scanRoot, options, ec), end;
             it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            const auto relative = std::filesystem::relative(it->path(), scanRoot, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const std::string file = scanPrefix + normalizeResourcePath(relative.string());
            if (matches(file) && seen.insert(file).second) result.push_back(file);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool GameFileSystem::exists(std::string_view relativePath) const { return readFile(relativePath).has_value(); }

std::string GameFileSystem::summary() const
{
    std::size_t directories = 0;
    for (const auto& location : locations_) if (location.kind == SearchLocation::Kind::Directory) ++directories;
    std::string result = std::to_string(directories) + " directories, " +
        std::to_string(vpks_.size()) + " VPKs, Steam AppID " + std::to_string(steamAppId_);
    if (!warnings_.empty()) result += ", " + std::to_string(warnings_.size()) + " warnings";
    return result;
}

} // namespace hammer::assets
