#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hammer::fgd {

enum class ClassKind { Base, Point, Solid };
enum class ModelHelperKind { None, Studio, StudioProp, LightProp };
enum class PropertyType {
    String,
    Integer,
    Float,
    Boolean,
    Choices,
    Flags,
    Color255,
    Color1,
    Vector,
    Angle,
    TargetSource,
    TargetDestination,
    Material,
    Model,
    Sound,
    Sprite,
    Unknown
};

struct Choice {
    std::string value;
    std::string label;
    bool defaultOn{false};

    bool operator==(const Choice&) const = default;
};

struct PropertyDefinition {
    std::string key;
    PropertyType type{PropertyType::String};
    std::string rawType;
    std::string displayName;
    std::string defaultValue;
    std::string description;
    std::vector<Choice> choices;
    bool readOnly{false};

    bool operator==(const PropertyDefinition&) const = default;
};

// One "input" or "output" declaration: name(valuetype) : "description".
struct IoDefinition {
    std::string name;
    std::string valueType;
    std::string description;

    bool operator==(const IoDefinition&) const = default;
};

struct EntityClass {
    std::string name;
    ClassKind kind{ClassKind::Point};
    // @NPCClass. Collapsed into Point for lookup and visualization, but kept
    // separately because the "NPCs" auto visgroup is keyed on it
    // (GDclass::IsNPCClass, used by CMapDoc::AddToAutoVisGroup).
    bool npc{false};
    std::string description;
    std::vector<std::string> bases;
    std::vector<PropertyDefinition> properties;
    std::vector<IoDefinition> inputs;
    std::vector<IoDefinition> outputs;
    std::optional<std::array<int, 3>> displayColor;
    std::optional<std::array<double, 3>> sizeMinimum;
    std::optional<std::array<double, 3>> sizeMaximum;
    std::string model;
    ModelHelperKind modelHelper{ModelHelperKind::None};
    std::string sprite;

    bool operator==(const EntityClass&) const = default;
};

struct EntityVisualization {
    std::array<int, 3> displayColor{108, 220, 108};
    std::array<double, 3> sizeMinimum{-8.0, -8.0, -8.0};
    std::array<double, 3> sizeMaximum{8.0, 8.0, 8.0};
    std::string description;
    std::string model;
    ModelHelperKind modelHelper{ModelHelperKind::None};
    std::string sprite;
};

struct ParseError {
    std::string message;
    std::size_t line{1};
    std::size_t column{1};
};

class Database {
public:
    bool loadText(std::string_view text, ParseError* error = nullptr);
    bool loadFile(const std::filesystem::path& path, ParseError* error = nullptr,
                  std::string* ioError = nullptr);
    void clear();

    const std::vector<EntityClass>& classes() const { return classes_; }
    const EntityClass* findClass(std::string_view name) const;
    // Resolution for base(...) references. base.fgd names a @BaseClass "Light"
    // and a @PointClass "light"; FGD names are case insensitive, so the two
    // collide. A base() reference means the base class, an entity's classname
    // means the point/solid class, and each lookup has to prefer its own kind
    // or the light classes silently inherit nothing.
    const EntityClass* findBaseClass(std::string_view name) const;
    std::vector<const EntityClass*> pointClasses() const;
    std::vector<const EntityClass*> solidClasses() const;
    std::vector<PropertyDefinition> effectiveProperties(std::string_view className) const;
    // Entity IO declarations, base classes resolved (op_output.cpp reads the
    // same data from GDclass).
    std::vector<IoDefinition> effectiveOutputs(std::string_view className) const;
    std::vector<IoDefinition> effectiveInputs(std::string_view className) const;
    EntityVisualization effectiveVisualization(std::string_view className) const;
    bool empty() const { return classes_.empty(); }

    static PropertyType propertyType(std::string_view rawType);
    static std::string_view propertyTypeName(PropertyType type);

private:
    void rebuildIndex();
    void appendEffectiveProperties(const EntityClass& entityClass,
                                   std::vector<PropertyDefinition>& output,
                                   std::vector<std::string>& visiting) const;
    void appendEffectiveVisualization(const EntityClass& entityClass,
                                      EntityVisualization& output,
                                      std::vector<std::string>& visiting) const;

    std::vector<EntityClass> classes_;
    std::unordered_map<std::string, std::size_t> index_;
    std::unordered_map<std::string, std::size_t> baseIndex_;
};

} // namespace hammer::fgd
