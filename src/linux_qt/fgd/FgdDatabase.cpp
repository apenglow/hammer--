#include "FgdDatabase.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <regex>
#include <unordered_set>

namespace hammer::fgd {
namespace {

enum class TokenKind { Text, String, Symbol, Newline, End, Invalid };

struct Token {
    TokenKind kind{TokenKind::Invalid};
    std::string text;
    std::size_t line{1};
    std::size_t column{1};
};

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    Token next()
    {
        skipHorizontalTrivia();
        const std::size_t line = line_;
        const std::size_t column = column_;
        if (offset_ >= input_.size()) return {TokenKind::End, {}, line, column};

        const char ch = peek();
        if (ch == '\r' || ch == '\n') {
            consumeNewline();
            return {TokenKind::Newline, "\n", line, column};
        }
        if (ch == '"') return quoted(line, column);
        if (std::string_view("@=:[](),").find(ch) != std::string_view::npos) {
            advance();
            return {TokenKind::Symbol, std::string(1, ch), line, column};
        }
        return text(line, column);
    }

private:
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

    void consumeNewline()
    {
        if (peek() == '\r') advance();
        if (peek() == '\n') advance();
        else {
            ++line_;
            column_ = 1;
        }
    }

    void skipHorizontalTrivia()
    {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\f' || peek() == '\v') advance();
            if (peek() == '/' && peek(1) == '/') {
                while (offset_ < input_.size() && peek() != '\r' && peek() != '\n') advance();
                continue;
            }
            break;
        }
    }

    Token quoted(std::size_t line, std::size_t column)
    {
        advance();
        std::string value;
        while (offset_ < input_.size()) {
            const char ch = advance();
            if (ch == '"') return {TokenKind::String, std::move(value), line, column};
            if (ch == '\\' && peek() == '"') {
                advance();
                value.push_back('"');
            } else {
                value.push_back(ch);
            }
        }
        return {TokenKind::Invalid, "unterminated string", line, column};
    }

    Token text(std::size_t line, std::size_t column)
    {
        std::string value;
        while (offset_ < input_.size()) {
            const char ch = peek();
            if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v' || ch == '"' ||
                std::string_view("@=:[](),").find(ch) != std::string_view::npos) break;
            if (ch == '/' && peek(1) == '/') break;
            value.push_back(advance());
        }
        if (value.empty()) {
            value.push_back(advance());
            return {TokenKind::Invalid, std::move(value), line, column};
        }
        return {TokenKind::Text, std::move(value), line, column};
    }

    std::string_view input_;
    std::size_t offset_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};

class Parser {
public:
    Parser(std::string_view input, std::vector<EntityClass>& classes, ParseError* error)
        : lexer_(input), classes_(classes), error_(error)
    {
        advance();
    }

    bool parse()
    {
        while (current_.kind != TokenKind::End) {
            skipNewlines();
            if (current_.kind == TokenKind::End) break;
            if (!isSymbol("@")) {
                advance();
                continue;
            }
            advance();
            if (!isText()) return fail("expected FGD directive after '@'");
            const std::string directive = lower(current_.text);
            advance();
            if (directive == "include") {
                skipToNewline();
                continue;
            }
            ClassKind kind;
            if (directive == "baseclass") kind = ClassKind::Base;
            else if (directive == "pointclass") kind = ClassKind::Point;
            else if (directive == "solidclass") kind = ClassKind::Solid;
            // GameData.cpp treats these as point classes with extra editor
            // behaviour (NPC previews, keyframe animation, movement linking,
            // filter targeting); for visualization and lookup they are points.
            else if (directive == "npcclass" || directive == "keyframeclass" ||
                     directive == "moveclass" || directive == "filterclass") {
                kind = ClassKind::Point;
            }
            else {
                skipToNewline();
                continue;
            }
            EntityClass parsed;
            parsed.kind = kind;
            parsed.npc = directive == "npcclass";
            if (!parseClass(parsed)) return false;
            classes_.push_back(std::move(parsed));
        }
        return true;
    }

private:
    void advance() { current_ = lexer_.next(); }
    bool isText() const { return current_.kind == TokenKind::Text || current_.kind == TokenKind::String; }
    bool isSymbol(std::string_view symbol) const { return current_.kind == TokenKind::Symbol && current_.text == symbol; }
    void skipNewlines() { while (current_.kind == TokenKind::Newline) advance(); }
    void skipToNewline()
    {
        while (current_.kind != TokenKind::Newline && current_.kind != TokenKind::End) advance();
        skipNewlines();
    }

    bool parseClass(EntityClass& entityClass)
    {
        while (current_.kind != TokenKind::End && !isSymbol("=")) {
            if (isText()) {
                const std::string modifier = lower(current_.text);
                advance();
                if (!isSymbol("(")) continue;
                const std::vector<std::string> arguments = parseModifierArguments();
                if (modifier == "base") {
                    entityClass.bases.insert(entityClass.bases.end(), arguments.begin(), arguments.end());
                } else if (modifier == "color" && arguments.size() >= 3) {
                    std::array<int, 3> color{};
                    if (parseInteger(arguments[0], color[0]) &&
                        parseInteger(arguments[1], color[1]) &&
                        parseInteger(arguments[2], color[2])) {
                        for (int& component : color) component = std::clamp(component, 0, 255);
                        entityClass.displayColor = color;
                    }
                } else if (modifier == "size" && arguments.size() >= 6) {
                    std::array<double, 3> minimum{};
                    std::array<double, 3> maximum{};
                    bool valid = true;
                    for (int index = 0; index < 3; ++index) {
                        valid = parseNumber(arguments[static_cast<std::size_t>(index)], minimum[static_cast<std::size_t>(index)]) && valid;
                        valid = parseNumber(arguments[static_cast<std::size_t>(index + 3)], maximum[static_cast<std::size_t>(index)]) && valid;
                    }
                    if (valid) {
                        entityClass.sizeMinimum = minimum;
                        entityClass.sizeMaximum = maximum;
                    }
                } else if (modifier == "studio" || modifier == "studioprop" ||
                           modifier == "lightprop" || modifier == "model") {
                    if (modifier == "studioprop")
                        entityClass.modelHelper = ModelHelperKind::StudioProp;
                    else if (modifier == "lightprop")
                        entityClass.modelHelper = ModelHelperKind::LightProp;
                    else
                        entityClass.modelHelper = ModelHelperKind::Studio;
                    // An omitted helper parameter means the entity's model key,
                    // just like Hammer's CMapStudioModel helper factory.
                    entityClass.model = arguments.empty() ? std::string{} : arguments.front();
                } else if ((modifier == "iconsprite" || modifier == "sprite") && !arguments.empty()) {
                    entityClass.sprite = arguments.front();
                }
                continue;
            }
            advance();
        }
        if (!isSymbol("=")) return fail("expected '=' in FGD class declaration");
        advance();
        skipNewlines();
        if (!isText()) return fail("expected FGD class name");
        entityClass.name = current_.text;
        advance();

        skipNewlines();
        if (isSymbol(":")) {
            advance();
            skipNewlines();
            if (isText()) {
                entityClass.description = current_.text;
                advance();
            }
        }
        while (current_.kind != TokenKind::End && !isSymbol("[")) advance();
        if (!isSymbol("[")) return fail("expected '[' for FGD class properties");
        advance();
        skipNewlines();
        while (current_.kind != TokenKind::End && !isSymbol("]")) {
            if (!parseProperty(entityClass)) return false;
            skipNewlines();
        }
        if (!isSymbol("]")) return fail("unterminated FGD class property block");
        advance();
        return true;
    }

    std::vector<std::string> parseModifierArguments()
    {
        std::vector<std::string> arguments;
        if (!isSymbol("(")) return arguments;
        int depth = 0;
        do {
            if (isSymbol("(")) {
                ++depth;
            } else if (isSymbol(")")) {
                --depth;
                if (depth == 0) {
                    advance();
                    break;
                }
            } else if (depth == 1 && isText()) {
                arguments.push_back(current_.text);
            }
            advance();
        } while (current_.kind != TokenKind::End && depth > 0);
        return arguments;
    }

    static bool parseInteger(const std::string& text, int& value)
    {
        try {
            std::size_t consumed = 0;
            const int parsed = std::stoi(text, &consumed);
            if (consumed != text.size()) return false;
            value = parsed;
            return true;
        } catch (...) {
            return false;
        }
    }

    static bool parseNumber(const std::string& text, double& value)
    {
        try {
            std::size_t consumed = 0;
            const double parsed = std::stod(text, &consumed);
            if (consumed != text.size()) return false;
            value = parsed;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parseProperty(EntityClass& entityClass)
    {
        if (current_.kind == TokenKind::Newline) {
            advance();
            return true;
        }
        if (!isText()) {
            advance();
            return true;
        }

        const std::string first = lower(current_.text);
        if (first == "input" || first == "output") {
            const bool isInput = first == "input";
            advance();
            if (!isText()) { skipToNewline(); return true; }
            IoDefinition io;
            io.name = current_.text;
            advance();
            if (isSymbol("(")) {
                advance();
                if (isText()) {
                    io.valueType = current_.text;
                    advance();
                }
                while (current_.kind != TokenKind::End && !isSymbol(")")) advance();
                if (isSymbol(")")) advance();
            }
            if (isSymbol(":")) {
                advance();
                skipNewlines();
                if (isText()) {
                    io.description = current_.text;
                    advance();
                }
            }
            (isInput ? entityClass.inputs : entityClass.outputs).push_back(std::move(io));
            if (current_.kind != TokenKind::Newline && !isSymbol("]")) skipToNewline();
            return true;
        }

        PropertyDefinition property;
        property.key = current_.text;
        advance();
        if (!isSymbol("(")) {
            skipToNewline();
            return true;
        }
        advance();
        if (!isText()) return fail("expected FGD property type");
        property.rawType = current_.text;
        property.type = Database::propertyType(property.rawType);
        advance();
        while (current_.kind != TokenKind::End && !isSymbol(")")) advance();
        if (!isSymbol(")")) return fail("unterminated FGD property type");
        advance();

        while (isText() && lower(current_.text) == "readonly") {
            property.readOnly = true;
            advance();
        }
        if (isSymbol(":")) {
            advance();
            skipNewlines();
            if (isText()) {
                property.displayName = current_.text;
                advance();
            }
            if (isSymbol(":")) {
                advance();
                skipNewlines();
                if (isText()) {
                    property.defaultValue = current_.text;
                    advance();
                }
                if (isSymbol(":")) {
                    advance();
                    skipNewlines();
                    if (isText()) {
                        property.description = current_.text;
                        advance();
                    }
                }
            }
        }
        if (property.displayName.empty()) property.displayName = property.key;

        if (isSymbol("=")) {
            advance();
            skipNewlines();
            if (isSymbol("[")) {
                advance();
                if (!parseChoices(property)) return false;
            }
        }
        entityClass.properties.push_back(std::move(property));
        if (current_.kind != TokenKind::Newline && !isSymbol("]")) skipToNewline();
        return true;
    }

    bool parseChoices(PropertyDefinition& property)
    {
        skipNewlines();
        while (current_.kind != TokenKind::End && !isSymbol("]")) {
            if (!isText()) {
                advance();
                continue;
            }
            Choice choice;
            choice.value = current_.text;
            advance();
            if (!isSymbol(":")) {
                skipToNewline();
                continue;
            }
            advance();
            skipNewlines();
            if (isText()) {
                choice.label = current_.text;
                advance();
            }
            if (isSymbol(":")) {
                advance();
                skipNewlines();
                if (isText()) {
                    const std::string enabled = lower(current_.text);
                    choice.defaultOn = enabled == "1" || enabled == "yes" || enabled == "true";
                    advance();
                }
            }
            property.choices.push_back(std::move(choice));
            skipNewlines();
        }
        if (!isSymbol("]")) return fail("unterminated FGD choices/flags block");
        advance();
        return true;
    }

    bool fail(std::string message)
    {
        if (error_) {
            error_->message = std::move(message);
            error_->line = current_.line;
            error_->column = current_.column;
        }
        return false;
    }

    Lexer lexer_;
    Token current_;
    std::vector<EntityClass>& classes_;
    ParseError* error_{nullptr};
};

} // namespace

bool Database::loadText(std::string_view text, ParseError* error)
{
    if (error) *error = {};
    std::vector<EntityClass> parsed;
    Parser parser(text, parsed, error);
    if (!parser.parse()) return false;
    for (EntityClass& entityClass : parsed) {
        const std::string key = lower(entityClass.name);
        const bool isBase = entityClass.kind == ClassKind::Base;
        // A redefinition replaces the earlier class of the same kind, but a
        // base class and an entity class that share a name are two different
        // things and both have to survive: base.fgd declares @BaseClass "Light"
        // (brightness, style, falloff) and @PointClass "light", and FGD names
        // are case insensitive. Replacing one with the other left every light
        // entity inheriting nothing.
        auto existing = std::find_if(classes_.begin(), classes_.end(), [&](const EntityClass& item) {
            return lower(item.name) == key && (item.kind == ClassKind::Base) == isBase;
        });
        if (existing == classes_.end()) classes_.push_back(std::move(entityClass));
        else *existing = std::move(entityClass);
    }
    rebuildIndex();
    return true;
}

bool Database::loadFile(const std::filesystem::path& path, ParseError* error, std::string* ioError)
{
    if (ioError) ioError->clear();
    std::unordered_set<std::string> visited;
    const auto loadRecursive = [&](auto&& self, const std::filesystem::path& input) -> bool {
        std::error_code canonicalError;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(input, canonicalError);
        const std::string key = (canonicalError ? input.lexically_normal() : canonical).string();
        if (!visited.insert(key).second) return true;

        std::ifstream stream(input, std::ios::binary);
        if (!stream) {
            if (ioError) *ioError = "could not open FGD file: " + input.string();
            return false;
        }
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        if (!stream.good() && !stream.eof()) {
            if (ioError) *ioError = "could not read FGD file: " + input.string();
            return false;
        }
        const std::string text = bytes.str();
        const std::regex includePattern(R"FGD(@include\s+"([^"]+)")FGD", std::regex::icase);
        for (std::sregex_iterator it(text.begin(), text.end(), includePattern), end; it != end; ++it) {
            if (!self(self, input.parent_path() / (*it)[1].str())) return false;
        }
        return loadText(text, error);
    };
    return loadRecursive(loadRecursive, path);
}

void Database::clear()
{
    classes_.clear();
    index_.clear();
}

const EntityClass* Database::findClass(std::string_view name) const
{
    const auto it = index_.find(lower(name));
    return it == index_.end() ? nullptr : &classes_[it->second];
}

const EntityClass* Database::findBaseClass(std::string_view name) const
{
    const auto it = baseIndex_.find(lower(name));
    return it == baseIndex_.end() ? nullptr : &classes_[it->second];
}

std::vector<const EntityClass*> Database::pointClasses() const
{
    std::vector<const EntityClass*> result;
    for (const EntityClass& entityClass : classes_) if (entityClass.kind == ClassKind::Point) result.push_back(&entityClass);
    std::sort(result.begin(), result.end(), [](const EntityClass* a, const EntityClass* b) { return lower(a->name) < lower(b->name); });
    return result;
}

std::vector<const EntityClass*> Database::solidClasses() const
{
    std::vector<const EntityClass*> result;
    for (const EntityClass& entityClass : classes_) if (entityClass.kind == ClassKind::Solid) result.push_back(&entityClass);
    std::sort(result.begin(), result.end(), [](const EntityClass* a, const EntityClass* b) { return lower(a->name) < lower(b->name); });
    return result;
}

namespace {
// The cycle guard has to tell a base class from an entity class of the same
// name, or "light" inheriting base.fgd's "Light" looks like self-recursion.
std::string visitKey(const EntityClass& entityClass)
{
    return (entityClass.kind == ClassKind::Base ? "base:" : "class:") + lower(entityClass.name);
}
} // namespace

std::vector<PropertyDefinition> Database::effectiveProperties(std::string_view className) const
{
    std::vector<PropertyDefinition> result;
    std::vector<std::string> visiting;
    if (const EntityClass* entityClass = findClass(className)) appendEffectiveProperties(*entityClass, result, visiting);
    return result;
}

namespace {
void appendEffectiveIo(const Database& database, const EntityClass& entityClass,
                       bool inputs, std::vector<IoDefinition>& output,
                       std::vector<std::string>& visiting)
{
    const std::string current = visitKey(entityClass);
    if (std::find(visiting.begin(), visiting.end(), current) != visiting.end()) return;
    visiting.push_back(current);
    for (const std::string& baseName : entityClass.bases) {
        if (const EntityClass* base = database.findBaseClass(baseName)) {
            appendEffectiveIo(database, *base, inputs, output, visiting);
        }
    }
    for (const IoDefinition& io : inputs ? entityClass.inputs : entityClass.outputs) {
        const auto existing = std::find_if(output.begin(), output.end(),
            [&](const IoDefinition& item) { return lower(item.name) == lower(io.name); });
        if (existing != output.end()) *existing = io;
        else output.push_back(io);
    }
}
} // namespace

std::vector<IoDefinition> Database::effectiveOutputs(std::string_view className) const
{
    std::vector<IoDefinition> result;
    std::vector<std::string> visiting;
    if (const EntityClass* entityClass = findClass(className)) {
        appendEffectiveIo(*this, *entityClass, false, result, visiting);
    }
    return result;
}

std::vector<IoDefinition> Database::effectiveInputs(std::string_view className) const
{
    std::vector<IoDefinition> result;
    std::vector<std::string> visiting;
    if (const EntityClass* entityClass = findClass(className)) {
        appendEffectiveIo(*this, *entityClass, true, result, visiting);
    }
    return result;
}

EntityVisualization Database::effectiveVisualization(std::string_view className) const
{
    EntityVisualization result;
    std::vector<std::string> visiting;
    if (const EntityClass* entityClass = findClass(className)) {
        appendEffectiveVisualization(*entityClass, result, visiting);
    }
    return result;
}

void Database::appendEffectiveProperties(const EntityClass& entityClass,
                                         std::vector<PropertyDefinition>& output,
                                         std::vector<std::string>& visiting) const
{
    const std::string current = visitKey(entityClass);
    if (std::find(visiting.begin(), visiting.end(), current) != visiting.end()) return;
    visiting.push_back(current);
    for (const std::string& baseName : entityClass.bases) {
        if (const EntityClass* base = findBaseClass(baseName)) appendEffectiveProperties(*base, output, visiting);
    }
    for (const PropertyDefinition& property : entityClass.properties) {
        const std::string propertyKey = lower(property.key);
        auto existing = std::find_if(output.begin(), output.end(), [&](const PropertyDefinition& item) {
            return lower(item.key) == propertyKey;
        });
        if (existing == output.end()) output.push_back(property);
        else *existing = property;
    }
    visiting.pop_back();
}

void Database::appendEffectiveVisualization(const EntityClass& entityClass,
                                            EntityVisualization& output,
                                            std::vector<std::string>& visiting) const
{
    const std::string current = visitKey(entityClass);
    if (std::find(visiting.begin(), visiting.end(), current) != visiting.end()) return;
    visiting.push_back(current);
    for (const std::string& baseName : entityClass.bases) {
        if (const EntityClass* base = findBaseClass(baseName)) {
            appendEffectiveVisualization(*base, output, visiting);
        }
    }
    if (entityClass.displayColor) output.displayColor = *entityClass.displayColor;
    if (entityClass.sizeMinimum && entityClass.sizeMaximum) {
        output.sizeMinimum = *entityClass.sizeMinimum;
        output.sizeMaximum = *entityClass.sizeMaximum;
    }
    if (!entityClass.description.empty()) output.description = entityClass.description;
    if (entityClass.modelHelper != ModelHelperKind::None) {
        output.modelHelper = entityClass.modelHelper;
        // Clear an inherited literal path when a derived class declares a
        // parameterless studio/studioprop/lightprop helper.
        output.model = entityClass.model;
    }
    if (!entityClass.sprite.empty()) output.sprite = entityClass.sprite;
    visiting.pop_back();
}

void Database::rebuildIndex()
{
    index_.clear();
    baseIndex_.clear();
    // Two indices over one class list: one that prefers real entity classes and
    // one that prefers base classes, so a name used by both (base.fgd's "Light"
    // and "light") resolves correctly from either side. Within a kind the last
    // definition wins, which is how a later FGD redefines an earlier one.
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        const std::string name = lower(classes_[i].name);
        const bool isBase = classes_[i].kind == ClassKind::Base;
        const auto prefer = [&](std::unordered_map<std::string, std::size_t>& index,
                                bool wantBase) {
            const auto existing = index.find(name);
            if (existing == index.end()) {
                index[name] = i;
                return;
            }
            const bool existingMatches = (classes_[existing->second].kind == ClassKind::Base) == wantBase;
            const bool candidateMatches = isBase == wantBase;
            if (candidateMatches || !existingMatches) existing->second = i;
        };
        prefer(index_, false);
        prefer(baseIndex_, true);
    }
}

PropertyType Database::propertyType(std::string_view rawType)
{
    const std::string type = lower(rawType);
    if (type == "integer" || type == "int") return PropertyType::Integer;
    if (type == "float") return PropertyType::Float;
    if (type == "boolean" || type == "bool") return PropertyType::Boolean;
    if (type == "choices") return PropertyType::Choices;
    if (type == "flags") return PropertyType::Flags;
    if (type == "color255") return PropertyType::Color255;
    if (type == "color1") return PropertyType::Color1;
    if (type == "vector" || type == "origin") return PropertyType::Vector;
    if (type == "angle" || type == "angles") return PropertyType::Angle;
    if (type == "target_source" || type == "targetname") return PropertyType::TargetSource;
    if (type == "target_destination" || type == "target") return PropertyType::TargetDestination;
    if (type == "material") return PropertyType::Material;
    if (type == "studio" || type == "model") return PropertyType::Model;
    if (type == "sound") return PropertyType::Sound;
    if (type == "sprite") return PropertyType::Sprite;
    if (type == "string") return PropertyType::String;
    return PropertyType::Unknown;
}

std::string_view Database::propertyTypeName(PropertyType type)
{
    switch (type) {
    case PropertyType::String: return "string";
    case PropertyType::Integer: return "integer";
    case PropertyType::Float: return "float";
    case PropertyType::Boolean: return "boolean";
    case PropertyType::Choices: return "choices";
    case PropertyType::Flags: return "flags";
    case PropertyType::Color255: return "color255";
    case PropertyType::Color1: return "color1";
    case PropertyType::Vector: return "vector";
    case PropertyType::Angle: return "angle";
    case PropertyType::TargetSource: return "target_source";
    case PropertyType::TargetDestination: return "target_destination";
    case PropertyType::Material: return "material";
    case PropertyType::Model: return "model";
    case PropertyType::Sound: return "sound";
    case PropertyType::Sprite: return "sprite";
    case PropertyType::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace hammer::fgd
