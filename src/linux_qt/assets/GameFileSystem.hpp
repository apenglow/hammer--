#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hammer::assets {

struct AssetError
{
    std::string message;
};

struct SearchLocation
{
    enum class Kind { Directory, Vpk };
    Kind kind{Kind::Directory};
    std::filesystem::path path;
    std::string pathId;
    int appId{0};
};

class VpkArchive
{
public:
    VpkArchive() = default;
    explicit VpkArchive(std::filesystem::path directoryFile);

    bool open(const std::filesystem::path& directoryFile, AssetError* error = nullptr);
    std::optional<std::vector<std::uint8_t>> read(std::string_view relativePath) const;
    bool contains(std::string_view relativePath) const;
    std::vector<std::string> files() const;
    const std::filesystem::path& path() const { return directoryFile_; }
    std::size_t fileCount() const { return entries_.size(); }

private:
    struct Entry
    {
        std::uint32_t crc{0};
        std::uint16_t preloadBytes{0};
        std::uint16_t archiveIndex{0};
        std::uint32_t offset{0};
        std::uint32_t length{0};
        std::vector<std::uint8_t> preload;
    };

    std::filesystem::path archivePath(std::uint16_t archiveIndex) const;
    std::filesystem::path directoryFile_;
    std::uint32_t version_{0};
    std::uint32_t headerSize_{0};
    std::uint32_t treeSize_{0};
    std::unordered_map<std::string, Entry> entries_;
};

class SteamLibraries
{
public:
    SteamLibraries();
    explicit SteamLibraries(std::vector<std::filesystem::path> roots);

    static std::vector<std::filesystem::path> defaultRoots();
    std::optional<std::filesystem::path> resolveApp(int appId) const;
    std::vector<std::filesystem::path> resolveApps(int appId) const;
    const std::vector<std::filesystem::path>& libraries() const { return libraries_; }

private:
    void discover();
    std::vector<std::filesystem::path> roots_;
    std::vector<std::filesystem::path> libraries_;
};

class GameFileSystem
{
public:
    bool configure(const std::filesystem::path& gameInfoFile, AssetError* error = nullptr,
                   const SteamLibraries* steamOverride = nullptr);

    // Mounts a loose directory ahead of every configured search path, so its
    // files override identically named game content. Used for per-map custom
    // content when gameinfo.txt does not already mount it via custom/*.
    bool mountOverrideDirectory(const std::filesystem::path& directory);

    std::optional<std::vector<std::uint8_t>> readFile(std::string_view relativePath) const;
    bool exists(std::string_view relativePath) const;
    std::optional<SearchLocation> sourceForFile(std::string_view relativePath) const;
    std::vector<std::string> listFiles(std::string_view prefix = {},
                                       std::string_view extension = {}) const;

    const std::filesystem::path& gameInfoFile() const { return gameInfoFile_; }
    const std::filesystem::path& gameDirectory() const { return gameDirectory_; }
    int steamAppId() const { return steamAppId_; }
    const std::vector<SearchLocation>& locations() const { return locations_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    std::size_t vpkCount() const { return vpks_.size(); }
    std::string summary() const;

    static std::string normalizeResourcePath(std::string_view path);

private:
    void addDirectory(const std::filesystem::path& directory, std::string pathId, int appId);
    bool addVpk(const std::filesystem::path& path, std::string pathId, int appId,
                AssetError* error, bool optional = true);
    void addDirectoryVpks(const std::filesystem::path& directory, const std::string& pathId,
                          int appId, AssetError* error);

    std::filesystem::path gameInfoFile_;
    std::filesystem::path gameDirectory_;
    int steamAppId_{0};
    std::vector<SearchLocation> locations_;
    std::vector<std::shared_ptr<VpkArchive>> vpks_;
    std::vector<std::string> warnings_;
};

} // namespace hammer::assets
