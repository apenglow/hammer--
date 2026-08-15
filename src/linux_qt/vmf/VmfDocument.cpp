#include "VmfDocument.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace hammer::vmf {
namespace {

enum class TokenKind { Text, OpenBrace, CloseBrace, End, Invalid };

struct Token
{
    TokenKind kind{TokenKind::Invalid};
    std::string text;
    std::size_t line{1};
    std::size_t column{1};
};

class Lexer
{
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    Token next()
    {
        skipTrivia();
        const std::size_t tokenLine = line_;
        const std::size_t tokenColumn = column_;
        if (atEnd()) {
            return {TokenKind::End, {}, tokenLine, tokenColumn};
        }

        const char ch = peek();
        if (ch == '{') {
            advance();
            return {TokenKind::OpenBrace, "{", tokenLine, tokenColumn};
        }
        if (ch == '}') {
            advance();
            return {TokenKind::CloseBrace, "}", tokenLine, tokenColumn};
        }
        if (ch == '"') {
            return quoted(tokenLine, tokenColumn);
        }
        return unquoted(tokenLine, tokenColumn);
    }

private:
    bool atEnd() const { return offset_ >= input_.size(); }
    char peek(std::size_t lookahead = 0) const
    {
        return offset_ + lookahead < input_.size() ? input_[offset_ + lookahead] : '\0';
    }

    char advance()
    {
        const char ch = input_[offset_++];
        if (ch == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return ch;
    }

    void skipTrivia()
    {
        for (;;) {
            while (!atEnd()) {
                const unsigned char ch = static_cast<unsigned char>(peek());
                if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
                    break;
                }
                advance();
            }

            if (peek() == '/' && peek(1) == '/') {
                while (!atEnd() && advance() != '\n') {}
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                advance();
                advance();
                while (!atEnd() && !(peek() == '*' && peek(1) == '/')) {
                    advance();
                }
                if (!atEnd()) {
                    advance();
                    advance();
                }
                continue;
            }
            break;
        }
    }

    Token quoted(std::size_t tokenLine, std::size_t tokenColumn)
    {
        advance(); // opening quote
        std::string text;
        while (!atEnd()) {
            const char ch = advance();
            if (ch == '"') {
                return {TokenKind::Text, std::move(text), tokenLine, tokenColumn};
            }
            if (ch == '\\' && peek() == '"') {
                advance();
                text.push_back('"');
                continue;
            }
            text.push_back(ch);
        }
        return {TokenKind::Invalid, "unterminated quoted string", tokenLine, tokenColumn};
    }

    Token unquoted(std::size_t tokenLine, std::size_t tokenColumn)
    {
        std::string text;
        while (!atEnd()) {
            const char ch = peek();
            if (ch == '{' || ch == '}' || ch == '"' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                ch == '\f' || ch == '\v') {
                break;
            }
            // Comment markers are recognized by skipTrivia() only when they
            // occur between tokens. They are valid characters inside an
            // unquoted token, notably Source search paths such as custom/*.
            text.push_back(advance());
        }
        if (text.empty()) {
            return {TokenKind::Invalid, "unexpected character", tokenLine, tokenColumn};
        }
        return {TokenKind::Text, std::move(text), tokenLine, tokenColumn};
    }

    std::string_view input_;
    std::size_t offset_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};

class Parser
{
public:
    Parser(std::string_view bytes, ParseError* error) : lexer_(bytes), error_(error) {}

    bool parse(std::vector<Block>& roots)
    {
        advance();
        while (current_.kind != TokenKind::End) {
            if (current_.kind == TokenKind::CloseBrace) {
                return fail("unexpected closing brace", current_);
            }
            if (current_.kind != TokenKind::Text) {
                return fail(current_.text.empty() ? "expected top-level VMF block" : current_.text, current_);
            }
            std::string name = current_.text;
            advance();
            if (current_.kind != TokenKind::OpenBrace) {
                return fail("expected '{' after top-level block name '" + name + "'", current_);
            }
            Block block(std::move(name));
            advance();
            if (!parseBlock(block)) {
                return false;
            }
            roots.push_back(std::move(block));
        }
        return true;
    }

private:
    void advance() { current_ = lexer_.next(); }

    bool parseBlock(Block& block)
    {
        while (current_.kind != TokenKind::CloseBrace) {
            if (current_.kind == TokenKind::End) {
                return fail("unexpected end of file inside block '" + block.name + "'", current_);
            }
            if (current_.kind != TokenKind::Text) {
                return fail(current_.text.empty() ? "expected key or child block" : current_.text, current_);
            }

            std::string name = current_.text;
            advance();
            if (current_.kind == TokenKind::OpenBrace) {
                Block child(std::move(name));
                advance();
                if (!parseBlock(child)) {
                    return false;
                }
                block.entries.emplace_back(std::move(child));
                continue;
            }
            if (current_.kind != TokenKind::Text) {
                return fail("expected value or '{' after '" + name + "'", current_);
            }
            block.entries.emplace_back(std::move(name), current_.text);
            advance();
        }
        advance(); // close brace
        return true;
    }

    bool fail(std::string message, const Token& token)
    {
        if (error_) {
            error_->message = std::move(message);
            error_->line = token.line;
            error_->column = token.column;
        }
        return false;
    }

    Lexer lexer_;
    Token current_;
    ParseError* error_{nullptr};
};

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        return a == b;
    });
}

std::string quoteVmf(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char ch : value) {
        if (ch == '"') {
            result.push_back('\\');
        }
        result.push_back(ch);
    }
    result.push_back('"');
    return result;
}

bool needsQuotedBlockName(std::string_view name)
{
    return name.empty() || std::any_of(name.begin(), name.end(), [](unsigned char ch) {
        return ch <= ' ' || ch == '{' || ch == '}' || ch == '"';
    });
}

void serializeBlock(const Block& block, std::string& out, std::size_t depth)
{
    out.append(depth, '\t');
    out += needsQuotedBlockName(block.name) ? quoteVmf(block.name) : block.name;
    out += "\r\n";
    out.append(depth, '\t');
    out += "{\r\n";

    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::KeyValue) {
            out.append(depth + 1, '\t');
            out += quoteVmf(entry.key);
            out.push_back(' ');
            out += quoteVmf(entry.value);
            out += "\r\n";
        } else if (entry.child) {
            serializeBlock(*entry.child, out, depth + 1);
        }
    }

    out.append(depth, '\t');
    out += "}\r\n";
}

void accumulate(const Block& block, Statistics& stats)
{
    ++stats.totalBlocks;
    if (equalsIgnoreCase(block.name, "world")) ++stats.worlds;
    if (equalsIgnoreCase(block.name, "entity")) ++stats.entities;
    if (equalsIgnoreCase(block.name, "solid")) ++stats.solids;
    if (equalsIgnoreCase(block.name, "side")) ++stats.sides;
    if (equalsIgnoreCase(block.name, "dispinfo")) ++stats.displacements;
    if (equalsIgnoreCase(block.name, "visgroups") || equalsIgnoreCase(block.name, "visgroup")) ++stats.visgroups;
    if (equalsIgnoreCase(block.name, "cameras") || equalsIgnoreCase(block.name, "camera")) ++stats.cameras;

    for (const Entry& entry : block.entries) {
        if (entry.kind == Entry::Kind::KeyValue) {
            ++stats.keyValues;
        } else if (entry.child) {
            accumulate(*entry.child, stats);
        }
    }
}

int parseInt(const std::string* value)
{
    if (!value) return -1;
    int result = -1;
    const char* begin = value->data();
    const char* end = begin + value->size();
    const auto conversion = std::from_chars(begin, end, result);
    return conversion.ec == std::errc{} && conversion.ptr == end ? result : -1;
}

} // namespace

Entry::Entry(std::string keyName, std::string keyValue)
    : kind(Kind::KeyValue), key(std::move(keyName)), value(std::move(keyValue)) {}

Entry::Entry(Block childBlock)
    : kind(Kind::ChildBlock), child(std::make_unique<Block>(std::move(childBlock))) {}

Entry::Entry(const Entry& other)
    : kind(other.kind), key(other.key), value(other.value),
      child(other.child ? std::make_unique<Block>(*other.child) : nullptr) {}

Entry& Entry::operator=(const Entry& other)
{
    if (this != &other) {
        kind = other.kind;
        key = other.key;
        value = other.value;
        child = other.child ? std::make_unique<Block>(*other.child) : nullptr;
    }
    return *this;
}

Entry::~Entry() = default;

Block::Block(std::string blockName) : name(std::move(blockName)) {}

const std::string* Block::value(std::string_view keyName) const
{
    for (const Entry& entry : entries) {
        if (entry.kind == Entry::Kind::KeyValue && equalsIgnoreCase(entry.key, keyName)) {
            return &entry.value;
        }
    }
    return nullptr;
}

std::string* Block::value(std::string_view keyName)
{
    for (Entry& entry : entries) {
        if (entry.kind == Entry::Kind::KeyValue && equalsIgnoreCase(entry.key, keyName)) {
            return &entry.value;
        }
    }
    return nullptr;
}

void Block::setValue(std::string keyName, std::string keyValue)
{
    if (std::string* existing = value(keyName)) {
        *existing = std::move(keyValue);
        return;
    }
    entries.emplace_back(std::move(keyName), std::move(keyValue));
}

std::vector<std::string> Block::values(std::string_view keyName) const
{
    std::vector<std::string> result;
    for (const Entry& entry : entries) {
        if (entry.kind == Entry::Kind::KeyValue && equalsIgnoreCase(entry.key, keyName))
            result.push_back(entry.value);
    }
    return result;
}

void Block::appendValue(std::string keyName, std::string keyValue)
{
    entries.emplace_back(std::move(keyName), std::move(keyValue));
}

std::size_t Block::removeValues(std::string_view keyName)
{
    const std::size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [keyName](const Entry& entry) {
                                     return entry.kind == Entry::Kind::KeyValue &&
                                            equalsIgnoreCase(entry.key, keyName);
                                 }),
                  entries.end());
    return before - entries.size();
}

std::vector<const Block*> Block::children(std::string_view childName) const
{
    std::vector<const Block*> result;
    for (const Entry& entry : entries) {
        if (entry.kind == Entry::Kind::ChildBlock && entry.child &&
            (childName.empty() || equalsIgnoreCase(entry.child->name, childName))) {
            result.push_back(entry.child.get());
        }
    }
    return result;
}

std::vector<Block*> Block::children(std::string_view childName)
{
    std::vector<Block*> result;
    for (Entry& entry : entries) {
        if (entry.kind == Entry::Kind::ChildBlock && entry.child &&
            (childName.empty() || equalsIgnoreCase(entry.child->name, childName))) {
            result.push_back(entry.child.get());
        }
    }
    return result;
}

Block& Block::appendChild(std::string childName)
{
    entries.emplace_back(Block(std::move(childName)));
    return *entries.back().child;
}

std::optional<Document> Document::parse(std::string bytes, ParseError* error)
{
    if (error) *error = {};
    const std::string originalBytes = bytes;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    Document document;
    Parser parser(bytes, error);
    if (!parser.parse(document.roots_)) {
        return std::nullopt;
    }
    document.originalBytes_ = originalBytes;
    document.dirty_ = false;
    return document;
}

std::optional<Document> Document::load(const std::filesystem::path& path, ParseError* error, std::string* ioError)
{
    if (ioError) ioError->clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (ioError) *ioError = "could not open file for reading";
        return std::nullopt;
    }
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        if (ioError) *ioError = "failed while reading file";
        return std::nullopt;
    }
    return parse(bytes.str(), error);
}

Document Document::createDefault()
{
    Document document;

    Block& versionInfo = document.appendRoot("versioninfo");
    versionInfo.setValue("editorversion", "400");
    versionInfo.setValue("editorbuild", "0");
    versionInfo.setValue("mapversion", "1");
    versionInfo.setValue("formatversion", "100");
    versionInfo.setValue("prefab", "0");

    document.appendRoot("visgroups");

    Block& viewSettings = document.appendRoot("viewsettings");
    viewSettings.setValue("bSnapToGrid", "1");
    viewSettings.setValue("bShowGrid", "1");
    viewSettings.setValue("bShowLogicalGrid", "0");
    viewSettings.setValue("nGridSpacing", "16");
    viewSettings.setValue("bShow3DGrid", "0");

    Block& world = document.appendRoot("world");
    world.setValue("id", "1");
    world.setValue("mapversion", "1");
    world.setValue("classname", "worldspawn");
    world.setValue("skyname", "sky_day01_01");
    world.setValue("maxpropscreenwidth", "-1");
    world.setValue("detailvbsp", "detail.vbsp");
    world.setValue("detailmaterial", "detail/detailsprites");

    Block& cameras = document.appendRoot("cameras");
    cameras.setValue("activecamera", "-1");

    Block& cordon = document.appendRoot("cordon");
    cordon.setValue("mins", "(-1024 -1024 -1024)");
    cordon.setValue("maxs", "(1024 1024 1024)");
    cordon.setValue("active", "0");

    document.dirty_ = true;
    return document;
}

bool Document::save(const std::filesystem::path& path, std::string* error)
{
    if (error) error->clear();
    const std::filesystem::path temporary = path.string() + ".hammer-tmp";
    const std::string bytes = serialize(true);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) *error = "could not open temporary file for writing";
            return false;
        }
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            if (error) *error = "failed while writing VMF data";
            stream.close();
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        if (error) *error = "could not replace destination file: " + ec.message();
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    originalBytes_ = bytes;
    dirty_ = false;
    return true;
}

std::string Document::serialize(bool preserveOriginalWhenUnchanged) const
{
    if (preserveOriginalWhenUnchanged && !dirty_ && !originalBytes_.empty()) {
        return originalBytes_;
    }
    std::string out;
    for (const Block& block : roots_) {
        serializeBlock(block, out, 0);
    }
    return out;
}

std::vector<Block>& Document::roots()
{
    dirty_ = true;
    return roots_;
}

const Block* Document::firstRoot(std::string_view name) const
{
    for (const Block& block : roots_) {
        if (equalsIgnoreCase(block.name, name)) return &block;
    }
    return nullptr;
}

Block* Document::firstRoot(std::string_view name)
{
    dirty_ = true;
    for (Block& block : roots_) {
        if (equalsIgnoreCase(block.name, name)) return &block;
    }
    return nullptr;
}

Block& Document::appendRoot(std::string name)
{
    dirty_ = true;
    roots_.emplace_back(std::move(name));
    return roots_.back();
}

Statistics Document::statistics() const
{
    Statistics stats;
    stats.topLevelBlocks = roots_.size();
    for (const Block& block : roots_) {
        accumulate(block, stats);
    }
    if (const Block* versionInfo = firstRoot("versioninfo")) {
        stats.mapVersion = parseInt(versionInfo->value("mapversion"));
        stats.formatVersion = parseInt(versionInfo->value("formatversion"));
    }
    return stats;
}

void Document::markClean(std::string originalBytes)
{
    originalBytes_ = std::move(originalBytes);
    dirty_ = false;
}

} // namespace hammer::vmf
