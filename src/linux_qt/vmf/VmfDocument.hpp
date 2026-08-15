#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hammer::vmf {

struct Block;

struct Entry
{
    enum class Kind { KeyValue, ChildBlock };

    Kind kind{Kind::KeyValue};
    std::string key;
    std::string value;
    std::unique_ptr<Block> child;

    Entry() = default;
    Entry(std::string keyName, std::string keyValue);
    explicit Entry(Block childBlock);
    Entry(const Entry& other);
    Entry& operator=(const Entry& other);
    Entry(Entry&&) noexcept = default;
    Entry& operator=(Entry&&) noexcept = default;
    ~Entry();
};

struct Block
{
    std::string name;
    std::vector<Entry> entries;

    Block() = default;
    explicit Block(std::string blockName);

    const std::string* value(std::string_view key) const;
    std::string* value(std::string_view key);
    void setValue(std::string key, std::string value);
    // Keys a VMF may legitimately repeat within one block. CMapClass::SaveVMF
    // writes one "visgroupid" per visgroup an object belongs to, so the single
    // -value accessors above cannot represent membership on their own.
    std::vector<std::string> values(std::string_view key) const;
    void appendValue(std::string key, std::string value);
    // Removes every entry with this key. Returns how many were dropped.
    std::size_t removeValues(std::string_view key);
    std::vector<const Block*> children(std::string_view childName = {}) const;
    std::vector<Block*> children(std::string_view childName = {});
    Block& appendChild(std::string childName);
};

struct ParseError
{
    std::string message;
    std::size_t line{1};
    std::size_t column{1};
};

struct Statistics
{
    std::size_t topLevelBlocks{0};
    std::size_t totalBlocks{0};
    std::size_t keyValues{0};
    std::size_t worlds{0};
    std::size_t entities{0};
    std::size_t solids{0};
    std::size_t sides{0};
    std::size_t displacements{0};
    std::size_t visgroups{0};
    std::size_t cameras{0};
    int mapVersion{-1};
    int formatVersion{-1};
};

class Document
{
public:
    Document() = default;
    Document(const Document&) = default;
    Document& operator=(const Document&) = default;
    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;

    static std::optional<Document> parse(std::string bytes, ParseError* error = nullptr);
    static std::optional<Document> load(const std::filesystem::path& path, ParseError* error = nullptr,
                                        std::string* ioError = nullptr);
    static Document createDefault();

    bool save(const std::filesystem::path& path, std::string* error = nullptr);
    std::string serialize(bool preserveOriginalWhenUnchanged = true) const;

    const std::vector<Block>& roots() const { return roots_; }
    std::vector<Block>& roots();

    const Block* firstRoot(std::string_view name) const;
    Block* firstRoot(std::string_view name);
    Block& appendRoot(std::string name);

    Statistics statistics() const;
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markClean(std::string originalBytes = {});

private:
    std::vector<Block> roots_;
    std::string originalBytes_;
    bool dirty_{true};
};

} // namespace hammer::vmf
