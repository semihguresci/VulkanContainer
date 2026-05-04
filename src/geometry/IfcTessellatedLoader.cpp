#include "Container/geometry/IfcTessellatedLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace container::geometry::ifc {
namespace {

struct StepValue {
  enum class Kind { Omitted, Number, String, Enum, Ref, Text, List };

  Kind kind{Kind::Omitted};
  double number{0.0};
  uint32_t ref{0};
  std::string text{};
  std::vector<StepValue> list{};
};

struct Entity {
  uint32_t id{0};
  std::string type{};
  StepValue args{};
};

struct MeshGroup {
  uint32_t meshId{0};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
};

struct BoxSolid {
  glm::vec3 minBounds{0.0f};
  glm::vec3 maxBounds{0.0f};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
};

struct GeometryInstance {
  uint32_t geometryId{0};
  glm::mat4 transform{1.0f};
};

struct StoreyMetadata {
  std::string id{};
  std::string name{};
};

struct SemanticMaterialMetadata {
  std::string name{};
  std::string category{};

  [[nodiscard]] bool empty() const noexcept {
    return name.empty() && category.empty();
  }
};

struct ProductMetadata {
  std::string guid{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::vector<container::geometry::dotbim::ElementProperty> properties{};
};

struct LengthUnitMetadata {
  bool authored{false};
  float metersPerUnit{1.0f};
  std::string sourceUnits{};
};

std::string upperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

std::string semanticPropertyKey(std::string_view value) {
  std::string key;
  key.reserve(value.size());
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      key.push_back(static_cast<char>(
          std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return key;
}

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

std::string ifcSiLengthUnitLabel(const std::optional<std::string> &prefix) {
  if (!prefix.has_value()) {
    return "metre";
  }
  if (*prefix == "MILLI") {
    return "millimetre";
  }
  if (*prefix == "CENTI") {
    return "centimetre";
  }
  if (*prefix == "DECI") {
    return "decimetre";
  }
  if (*prefix == "KILO") {
    return "kilometre";
  }
  return "metre";
}

container::geometry::dotbim::ModelUnitMetadata
makeUnitMetadata(const LengthUnitMetadata &lengthUnit, float importScale) {
  const float sanitizedImportScale = sanitizeImportScale(importScale);
  container::geometry::dotbim::ModelUnitMetadata metadata{};
  if (lengthUnit.authored) {
    metadata.hasSourceUnits = true;
    metadata.sourceUnits = lengthUnit.sourceUnits;
    metadata.hasMetersPerUnit = true;
    metadata.metersPerUnit = lengthUnit.metersPerUnit;
  }
  metadata.hasImportScale = true;
  metadata.importScale = sanitizedImportScale;
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale =
      sanitizedImportScale *
      (lengthUnit.authored ? lengthUnit.metersPerUnit : 1.0f);
  return metadata;
}

class ValueParser {
public:
  explicit ValueParser(std::string_view text) : text_(text) {}

  StepValue parseArguments() {
    StepValue result{};
    result.kind = StepValue::Kind::List;

    while (true) {
      skipWhitespace();
      if (atEnd()) {
        break;
      }

      result.list.push_back(parseValue());
      skipWhitespace();
      if (atEnd()) {
        break;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      throw std::runtime_error("unexpected IFC argument token near offset " +
                               std::to_string(pos_));
    }

    return result;
  }

private:
  [[nodiscard]] bool atEnd() const { return pos_ >= text_.size(); }

  void skipWhitespace() {
    while (!atEnd() &&
           std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
  }

  StepValue parseValue() {
    skipWhitespace();
    if (atEnd()) {
      return {};
    }

    const char c = text_[pos_];
    if (c == '(') {
      return parseList();
    }
    if (c == '\'') {
      return parseString();
    }
    if (c == '#') {
      return parseRef();
    }
    if (c == '.') {
      return parseEnum();
    }
    if (c == '$' || c == '*') {
      ++pos_;
      return {};
    }
    return parseTokenOrTypedValue();
  }

  StepValue parseList() {
    StepValue result{};
    result.kind = StepValue::Kind::List;
    ++pos_;

    while (true) {
      skipWhitespace();
      if (atEnd()) {
        throw std::runtime_error("unterminated IFC list value");
      }
      if (text_[pos_] == ')') {
        ++pos_;
        break;
      }

      result.list.push_back(parseValue());
      skipWhitespace();
      if (atEnd()) {
        throw std::runtime_error("unterminated IFC list value");
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ')') {
        ++pos_;
        break;
      }
      throw std::runtime_error("unexpected IFC list token near offset " +
                               std::to_string(pos_));
    }

    return result;
  }

  StepValue parseString() {
    StepValue result{};
    result.kind = StepValue::Kind::String;
    ++pos_;

    while (!atEnd()) {
      const char c = text_[pos_++];
      if (c == '\'') {
        if (!atEnd() && text_[pos_] == '\'') {
          result.text.push_back('\'');
          ++pos_;
          continue;
        }
        return result;
      }
      result.text.push_back(c);
    }

    throw std::runtime_error("unterminated IFC string value");
  }

  StepValue parseRef() {
    StepValue result{};
    result.kind = StepValue::Kind::Ref;
    ++pos_;
    const size_t start = pos_;
    while (!atEnd() &&
           std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
    if (start == pos_) {
      throw std::runtime_error("IFC reference is missing an id");
    }
    const std::string digits(text_.substr(start, pos_ - start));
    const auto parsed = std::stoull(digits);
    if (parsed > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("IFC reference id exceeds uint32 range");
    }
    result.ref = static_cast<uint32_t>(parsed);
    return result;
  }

  StepValue parseEnum() {
    StepValue result{};
    result.kind = StepValue::Kind::Enum;
    ++pos_;
    const size_t start = pos_;
    while (!atEnd() && text_[pos_] != '.') {
      ++pos_;
    }
    if (atEnd()) {
      throw std::runtime_error("unterminated IFC enum value");
    }
    result.text = upperAscii(std::string(text_.substr(start, pos_ - start)));
    ++pos_;
    return result;
  }

  StepValue parseTokenOrTypedValue() {
    const size_t start = pos_;
    while (!atEnd()) {
      const char c = text_[pos_];
      if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',' ||
          c == '(' || c == ')') {
        break;
      }
      ++pos_;
    }

    if (start == pos_) {
      throw std::runtime_error("unexpected IFC value token near offset " +
                               std::to_string(pos_));
    }

    const std::string token(text_.substr(start, pos_ - start));
    skipWhitespace();
    if (!atEnd() && text_[pos_] == '(') {
      return parseList();
    }

    char *end = nullptr;
    const double number = std::strtod(token.c_str(), &end);
    if (end != token.c_str() && end != nullptr && *end == '\0' &&
        std::isfinite(number)) {
      StepValue result{};
      result.kind = StepValue::Kind::Number;
      result.number = number;
      return result;
    }

    StepValue result{};
    result.kind = StepValue::Kind::Text;
    result.text = upperAscii(token);
    return result;
  }

  std::string_view text_{};
  size_t pos_{0};
};

bool isIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

size_t findMatchingParen(std::string_view text, size_t openPos) {
  size_t depth = 0;
  bool inString = false;
  for (size_t i = openPos; i < text.size(); ++i) {
    const char c = text[i];
    if (inString) {
      if (c == '\'') {
        if (i + 1u < text.size() && text[i + 1u] == '\'') {
          ++i;
        } else {
          inString = false;
        }
      }
      continue;
    }

    if (c == '\'') {
      inString = true;
      continue;
    }
    if (c == '(') {
      ++depth;
      continue;
    }
    if (c == ')') {
      if (depth == 0u) {
        throw std::runtime_error("IFC parser encountered unmatched ')'");
      }
      --depth;
      if (depth == 0u) {
        return i;
      }
    }
  }
  throw std::runtime_error("IFC entity has unterminated argument list");
}

std::unordered_map<uint32_t, Entity> parseEntities(std::string_view text) {
  std::unordered_map<uint32_t, Entity> entities;

  size_t pos = 0;
  while (pos < text.size()) {
    const size_t hash = text.find('#', pos);
    if (hash == std::string_view::npos) {
      break;
    }

    size_t cursor = hash + 1u;
    if (cursor >= text.size() ||
        std::isdigit(static_cast<unsigned char>(text[cursor])) == 0) {
      pos = cursor;
      continue;
    }

    const size_t idStart = cursor;
    while (cursor < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    const size_t idEnd = cursor;

    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '=') {
      pos = cursor;
      continue;
    }
    ++cursor;
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }

    const size_t typeStart = cursor;
    while (cursor < text.size() && isIdentChar(text[cursor])) {
      ++cursor;
    }
    if (typeStart == cursor) {
      pos = cursor;
      continue;
    }

    const std::string type =
        upperAscii(std::string(text.substr(typeStart, cursor - typeStart)));
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '(') {
      pos = cursor;
      continue;
    }

    const size_t close = findMatchingParen(text, cursor);
    const std::string digits(text.substr(idStart, idEnd - idStart));
    const auto parsedId = std::stoull(digits);
    if (parsedId > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("IFC entity id exceeds uint32 range");
    }

    const std::string_view argsText =
        text.substr(cursor + 1u, close - cursor - 1u);
    Entity entity{};
    entity.id = static_cast<uint32_t>(parsedId);
    entity.type = type;
    entity.args = ValueParser(argsText).parseArguments();
    entities[entity.id] = std::move(entity);
    pos = close + 1u;
  }

  return entities;
}

const StepValue *argAt(const Entity &entity, size_t index) {
  if (entity.args.kind != StepValue::Kind::List ||
      index >= entity.args.list.size()) {
    return nullptr;
  }
  return &entity.args.list[index];
}

std::span<const StepValue> asList(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::List) {
    return {};
  }
  return value->list;
}

std::optional<uint32_t> refValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::Ref) {
    return std::nullopt;
  }
  return value->ref;
}

std::optional<double> numberValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::Number) {
    return std::nullopt;
  }
  return value->number;
}

std::optional<std::string> stringValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::String) {
    return std::nullopt;
  }
  return value->text;
}

std::optional<std::string> scalarLabelValue(const StepValue *value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  switch (value->kind) {
  case StepValue::Kind::String:
    return value->text;
  case StepValue::Kind::Enum:
  case StepValue::Kind::Text:
    if (value->text == "T") {
      return std::string("true");
    }
    if (value->text == "F") {
      return std::string("false");
    }
    return value->text;
  case StepValue::Kind::Number:
    if (std::isfinite(value->number)) {
      return std::to_string(value->number);
    }
    return std::nullopt;
  case StepValue::Kind::List:
    for (const StepValue &item : value->list) {
      if (auto nested = scalarLabelValue(&item); nested && !nested->empty()) {
        return nested;
      }
    }
    return std::nullopt;
  case StepValue::Kind::Omitted:
  case StepValue::Kind::Ref:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> enumValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::Enum) {
    return std::nullopt;
  }
  return value->text;
}

std::vector<uint32_t> refList(const StepValue *value) {
  std::vector<uint32_t> refs;
  for (const StepValue &item : asList(value)) {
    if (item.kind == StepValue::Kind::Ref) {
      refs.push_back(item.ref);
    }
  }
  return refs;
}

std::optional<uint32_t> firstRef(const Entity &entity, size_t index) {
  return refValue(argAt(entity, index));
}

class IfcModelBuilder {
public:
  IfcModelBuilder(std::unordered_map<uint32_t, Entity> entities,
                  float importScale)
      : entities_(std::move(entities)),
        importScale_(sanitizeImportScale(importScale)) {
    sortedEntityIds_.reserve(entities_.size());
    for (const auto &[id, _] : entities_) {
      sortedEntityIds_.push_back(id);
    }
    std::ranges::sort(sortedEntityIds_);
    unitMetadata_ = detectLengthUnitMetadata();
    unitScale_ = unitMetadata_.authored ? unitMetadata_.metersPerUnit : 1.0f;
    model_.unitMetadata = makeUnitMetadata(unitMetadata_, importScale_);
    pointCache_.reserve(entities_.size() / 2u);
    directionCache_.reserve(entities_.size() / 8u);
    axisPlacementCache_.reserve(entities_.size() / 8u);
    localPlacementCache_.reserve(entities_.size() / 8u);
    groupsByItem_.reserve(entities_.size() / 8u);
    boxSolidsByItem_.reserve(entities_.size() / 16u);
    openingsByHostProduct_.reserve(entities_.size() / 32u);
    hostByOpeningProduct_.reserve(entities_.size() / 32u);
    storeyByProduct_.reserve(entities_.size() / 32u);
    materialByProduct_.reserve(entities_.size() / 32u);
    semanticPropertiesByProduct_.reserve(entities_.size() / 32u);
  }

  Model build() {
    cacheStyleColors();
    cacheVoidRelations();
    cacheFillRelations();
    cacheSpatialContainmentRelations();
    cacheMaterialRelations();
    cachePropertyRelations();
    cacheClassificationRelations();
    cacheGroupRelations();
    cacheHierarchyRelations();
    cacheTypeRelations();
    appendTessellatedGeometry();
    appendSweptSolidGeometry();
    appendProductElements();
    appendFallbackElements();
    return std::move(model_);
  }

private:
  const Entity *entity(uint32_t id) const {
    const auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
  }

  glm::vec3 readDirection(uint32_t ref, glm::vec3 fallback) const {
    if (const auto cached = directionCache_.find(ref);
        cached != directionCache_.end()) {
      return cached->second;
    }

    const Entity *direction = entity(ref);
    if (direction == nullptr || direction->type != "IFCDIRECTION") {
      return fallback;
    }
    const auto coords = readNumberList(argAt(*direction, 0));
    if (coords.size() < 2u) {
      return fallback;
    }
    glm::vec3 value(static_cast<float>(coords[0]),
                    static_cast<float>(coords[1]),
                    coords.size() > 2u ? static_cast<float>(coords[2]) : 0.0f);
    if (glm::dot(value, value) <= 1.0e-12f) {
      return fallback;
    }
    value = glm::normalize(value);
    directionCache_[ref] = value;
    return value;
  }

  glm::vec3 readPoint(uint32_t ref, glm::vec3 fallback) const {
    if (const auto cached = pointCache_.find(ref);
        cached != pointCache_.end()) {
      return cached->second;
    }

    const Entity *point = entity(ref);
    if (point == nullptr || point->type != "IFCCARTESIANPOINT") {
      return fallback;
    }
    const auto coords = readNumberList(argAt(*point, 0));
    if (coords.size() < 2u) {
      return fallback;
    }
    const glm::vec3 value{
        static_cast<float>(coords[0]), static_cast<float>(coords[1]),
        coords.size() > 2u ? static_cast<float>(coords[2]) : 0.0f};
    pointCache_[ref] = value;
    return value;
  }

  std::vector<glm::vec3> readPolylinePoints(uint32_t ref) const {
    const Entity *polyline = entity(ref);
    if (polyline == nullptr || polyline->type != "IFCPOLYLINE") {
      return {};
    }

    std::vector<glm::vec3> points;
    for (const auto pointRef : refList(argAt(*polyline, 0))) {
      points.push_back(readPoint(pointRef, glm::vec3(0.0f)));
    }
    if (points.size() > 2u &&
        glm::length(points.front() - points.back()) <= 1.0e-5f) {
      points.pop_back();
    }
    return points;
  }

  std::vector<glm::vec3> readArbitraryClosedProfilePoints(uint32_t ref) const {
    const Entity *profile = entity(ref);
    if (profile == nullptr) {
      return {};
    }
    if (profile->type == "IFCARBITRARYCLOSEDPROFILEDEF") {
      const auto curveRef = firstRef(*profile, 2);
      return curveRef.has_value() ? readPolylinePoints(*curveRef)
                                  : std::vector<glm::vec3>{};
    }
    if (profile->type == "IFCARBITRARYPROFILEDEFWITHVOIDS") {
      const auto curveRef = firstRef(*profile, 2);
      return curveRef.has_value() ? readPolylinePoints(*curveRef)
                                  : std::vector<glm::vec3>{};
    }
    return {};
  }

  std::vector<glm::vec3> readPointList3D(uint32_t ref) const {
    const Entity *pointList = entity(ref);
    if (pointList == nullptr || pointList->type != "IFCCARTESIANPOINTLIST3D") {
      return {};
    }

    std::vector<glm::vec3> points;
    for (const StepValue &tuple : asList(argAt(*pointList, 0))) {
      const auto coords = readNumberList(&tuple);
      if (coords.size() < 3u) {
        continue;
      }
      points.emplace_back(static_cast<float>(coords[0]),
                          static_cast<float>(coords[1]),
                          static_cast<float>(coords[2]));
    }
    return points;
  }

  static std::vector<double> readNumberList(const StepValue *value) {
    std::vector<double> numbers;
    for (const StepValue &item : asList(value)) {
      if (item.kind == StepValue::Kind::Number && std::isfinite(item.number)) {
        numbers.push_back(item.number);
      }
    }
    return numbers;
  }

  static std::vector<uint32_t> readPositiveIndexList(const StepValue *value) {
    std::vector<uint32_t> indices;
    for (const StepValue &item : asList(value)) {
      if (item.kind != StepValue::Kind::Number || item.number < 1.0 ||
          item.number >
              static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        continue;
      }
      const double rounded = std::round(item.number);
      if (std::abs(item.number - rounded) <= 1.0e-6) {
        indices.push_back(static_cast<uint32_t>(rounded));
      }
    }
    return indices;
  }

  struct TriangleVertices {
    std::array<glm::vec3, 3> positions{};
  };
  using AxisIndex = glm::vec3::length_type;

  static glm::vec3 safeNormal(const glm::vec3 &a, const glm::vec3 &b,
                              const glm::vec3 &c) {
    const glm::vec3 normal = glm::cross(b - a, c - a);
    const float len2 = glm::dot(normal, normal);
    if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
      return {0.0f, 1.0f, 0.0f};
    }
    return normal * (1.0f / std::sqrt(len2));
  }

  static glm::vec3 safeTangent(const glm::vec3 &a, const glm::vec3 &b,
                               const glm::vec3 &normal) {
    glm::vec3 tangent = b - a;
    tangent -= normal * glm::dot(normal, tangent);
    float len2 = glm::dot(tangent, tangent);
    if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
      const glm::vec3 axis = std::abs(normal.y) < 0.999f
                                 ? glm::vec3(0.0f, 1.0f, 0.0f)
                                 : glm::vec3(1.0f, 0.0f, 0.0f);
      tangent = glm::cross(axis, normal);
      len2 = glm::dot(tangent, tangent);
    }
    if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
      return {1.0f, 0.0f, 0.0f};
    }
    return tangent * (1.0f / std::sqrt(len2));
  }

  static Vertex makeVertex(const glm::vec3 &position, const glm::vec3 &normal,
                           const glm::vec3 &tangent) {
    Vertex vertex{};
    vertex.position = position;
    vertex.normal = normal;
    vertex.tangent = glm::vec4(tangent, 1.0f);
    return vertex;
  }

  static uint32_t packColor(glm::vec4 color) {
    auto pack = [](float component) {
      return static_cast<uint32_t>(
          std::clamp(std::lround(component * 255.0f), 0l, 255l));
    };
    return pack(color.r) | (pack(color.g) << 8u) | (pack(color.b) << 16u) |
           (pack(color.a) << 24u);
  }

  static glm::vec4 defaultColor() { return {0.8f, 0.82f, 0.86f, 1.0f}; }

  static glm::vec4 sanitizeColor(glm::vec4 color) {
    color.r = std::clamp(color.r, 0.0f, 1.0f);
    color.g = std::clamp(color.g, 0.0f, 1.0f);
    color.b = std::clamp(color.b, 0.0f, 1.0f);
    color.a = std::clamp(color.a, 0.0f, 1.0f);
    return color;
  }

  LengthUnitMetadata detectLengthUnitMetadata() const {
    for (const auto id : sortedEntityIds_) {
      const Entity *unit = entity(id);
      if (unit == nullptr || unit->type != "IFCSIUNIT") {
        continue;
      }
      const auto unitType = enumValue(argAt(*unit, 1));
      const auto unitName = enumValue(argAt(*unit, 3));
      if (!unitType.has_value() || *unitType != "LENGTHUNIT" ||
          !unitName.has_value() || *unitName != "METRE") {
        continue;
      }
      const auto prefix = enumValue(argAt(*unit, 2));
      LengthUnitMetadata metadata{};
      metadata.authored = true;
      metadata.sourceUnits = ifcSiLengthUnitLabel(prefix);
      if (!prefix.has_value()) {
        metadata.metersPerUnit = 1.0f;
        return metadata;
      }
      if (*prefix == "MILLI") {
        metadata.metersPerUnit = 0.001f;
        return metadata;
      }
      if (*prefix == "CENTI") {
        metadata.metersPerUnit = 0.01f;
        return metadata;
      }
      if (*prefix == "DECI") {
        metadata.metersPerUnit = 0.1f;
        return metadata;
      }
      if (*prefix == "KILO") {
        metadata.metersPerUnit = 1000.0f;
        return metadata;
      }
      metadata.metersPerUnit = 1.0f;
      return metadata;
    }
    return {};
  }

  glm::mat4 importUnitTransform() const {
    return container::geometry::zUpForwardYToRendererAxes() *
           glm::scale(glm::mat4(1.0f), glm::vec3(importScale_ * unitScale_));
  }

  static glm::mat4 basisTransform(const glm::vec3 &location,
                                  const glm::vec3 &xAxisHint,
                                  const glm::vec3 &zAxisHint) {
    glm::vec3 zAxis = zAxisHint;
    if (glm::dot(zAxis, zAxis) <= 1.0e-12f) {
      zAxis = {0.0f, 0.0f, 1.0f};
    }
    zAxis = glm::normalize(zAxis);

    glm::vec3 xAxis = xAxisHint;
    if (glm::dot(xAxis, xAxis) <= 1.0e-12f ||
        std::abs(glm::dot(glm::normalize(xAxis), zAxis)) > 0.999f) {
      xAxis = std::abs(zAxis.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                       : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    xAxis = glm::normalize(xAxis - zAxis * glm::dot(zAxis, xAxis));
    glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
    xAxis = glm::normalize(glm::cross(yAxis, zAxis));

    glm::mat4 result(1.0f);
    result[0] = glm::vec4(xAxis, 0.0f);
    result[1] = glm::vec4(yAxis, 0.0f);
    result[2] = glm::vec4(zAxis, 0.0f);
    result[3] = glm::vec4(location, 1.0f);
    return result;
  }

  glm::mat4 axis2Placement3D(uint32_t ref) const {
    if (const auto cached = axisPlacementCache_.find(ref);
        cached != axisPlacementCache_.end()) {
      return cached->second;
    }

    const Entity *placement = entity(ref);
    if (placement == nullptr || placement->type != "IFCAXIS2PLACEMENT3D") {
      return glm::mat4(1.0f);
    }

    glm::vec3 location(0.0f);
    if (const auto pointRef = firstRef(*placement, 0); pointRef.has_value()) {
      location = readPoint(*pointRef, glm::vec3(0.0f));
    }
    glm::vec3 zAxis(0.0f, 0.0f, 1.0f);
    if (const auto directionRef = firstRef(*placement, 1);
        directionRef.has_value()) {
      zAxis = readDirection(*directionRef, {0.0f, 0.0f, 1.0f});
    }
    glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
    if (const auto directionRef = firstRef(*placement, 2);
        directionRef.has_value()) {
      xAxis = readDirection(*directionRef, {1.0f, 0.0f, 0.0f});
    }
    const glm::mat4 result = basisTransform(location, xAxis, zAxis);
    axisPlacementCache_[ref] = result;
    return result;
  }

  glm::mat4 localPlacement(uint32_t ref) const {
    if (const auto cached = localPlacementCache_.find(ref);
        cached != localPlacementCache_.end()) {
      return cached->second;
    }

    std::unordered_set<uint32_t> visiting;
    return localPlacementRecursive(ref, visiting);
  }

  glm::mat4
  localPlacementRecursive(uint32_t ref,
                          std::unordered_set<uint32_t> &visiting) const {
    if (const auto cached = localPlacementCache_.find(ref);
        cached != localPlacementCache_.end()) {
      return cached->second;
    }
    if (!visiting.insert(ref).second) {
      return glm::mat4(1.0f);
    }

    const Entity *placement = entity(ref);
    if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT") {
      visiting.erase(ref);
      return glm::mat4(1.0f);
    }

    glm::mat4 parent(1.0f);
    if (const auto parentRef = firstRef(*placement, 0); parentRef.has_value()) {
      parent = localPlacementRecursive(*parentRef, visiting);
    }
    glm::mat4 relative(1.0f);
    if (const auto relativeRef = firstRef(*placement, 1);
        relativeRef.has_value()) {
      relative = axis2Placement3D(*relativeRef);
    }
    const glm::mat4 result = parent * relative;
    visiting.erase(ref);
    localPlacementCache_[ref] = result;
    return result;
  }

  glm::mat4 cartesianTransformationOperator3D(uint32_t ref) const {
    const Entity *op = entity(ref);
    if (op == nullptr ||
        (op->type != "IFCCARTESIANTRANSFORMATIONOPERATOR3D" &&
         op->type != "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM")) {
      return glm::mat4(1.0f);
    }

    glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
    if (const auto directionRef = firstRef(*op, 0); directionRef.has_value()) {
      xAxis = readDirection(*directionRef, {1.0f, 0.0f, 0.0f});
    }
    glm::vec3 yAxis(0.0f, 1.0f, 0.0f);
    if (const auto directionRef = firstRef(*op, 1); directionRef.has_value()) {
      yAxis = readDirection(*directionRef, {0.0f, 1.0f, 0.0f});
    }
    glm::vec3 origin(0.0f);
    if (const auto pointRef = firstRef(*op, 2); pointRef.has_value()) {
      origin = readPoint(*pointRef, glm::vec3(0.0f));
    }
    const float scale1 =
        static_cast<float>(numberValue(argAt(*op, 3)).value_or(1.0));
    glm::vec3 zHint = glm::cross(xAxis, yAxis);
    if (glm::dot(zHint, zHint) <= 1.0e-12f) {
      zHint = {0.0f, 0.0f, 1.0f};
    }
    glm::vec3 zAxis = glm::normalize(zHint);
    if (const auto directionRef = firstRef(*op, 4); directionRef.has_value()) {
      zAxis = readDirection(*directionRef, zHint);
    }
    const float scale2 =
        op->type == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM"
            ? static_cast<float>(numberValue(argAt(*op, 5)).value_or(scale1))
            : scale1;
    const float scale3 =
        op->type == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM"
            ? static_cast<float>(numberValue(argAt(*op, 6)).value_or(scale1))
            : scale1;

    glm::mat4 result(1.0f);
    result[0] = glm::vec4(glm::normalize(xAxis) * scale1, 0.0f);
    result[1] = glm::vec4(glm::normalize(yAxis) * scale2, 0.0f);
    result[2] = glm::vec4(glm::normalize(zAxis) * scale3, 0.0f);
    result[3] = glm::vec4(origin, 1.0f);
    return result;
  }

  std::optional<glm::vec4> colorFromRef(uint32_t ref) const {
    return colorFromRef(ref, {});
  }

  std::optional<glm::vec4>
  colorFromRef(uint32_t ref, std::unordered_set<uint32_t> visiting) const {
    if (!visiting.insert(ref).second) {
      return std::nullopt;
    }

    const Entity *colorEntity = entity(ref);
    if (colorEntity == nullptr) {
      return std::nullopt;
    }

    if (colorEntity->type == "IFCCOLOURRGB") {
      const auto r = numberValue(argAt(*colorEntity, 1));
      const auto g = numberValue(argAt(*colorEntity, 2));
      const auto b = numberValue(argAt(*colorEntity, 3));
      if (r.has_value() && g.has_value() && b.has_value()) {
        return sanitizeColor({static_cast<float>(*r), static_cast<float>(*g),
                              static_cast<float>(*b), 1.0f});
      }
      return std::nullopt;
    }

    if (colorEntity->type == "IFCSURFACESTYLERENDERING" ||
        colorEntity->type == "IFCSURFACESTYLESHADING") {
      const auto baseColorRef = firstRef(*colorEntity, 0);
      if (!baseColorRef.has_value()) {
        return std::nullopt;
      }
      auto color = colorFromRef(*baseColorRef, visiting);
      if (!color.has_value()) {
        return std::nullopt;
      }
      const float transparency =
          static_cast<float>(numberValue(argAt(*colorEntity, 1)).value_or(0.0));
      color->a = std::clamp(1.0f - transparency, 0.0f, 1.0f);
      return sanitizeColor(*color);
    }

    if (colorEntity->type == "IFCSURFACESTYLE") {
      for (const auto styleRef : refList(argAt(*colorEntity, 2))) {
        if (auto color = colorFromRef(styleRef, visiting); color.has_value()) {
          return color;
        }
      }
      return std::nullopt;
    }

    if (colorEntity->type == "IFCPRESENTATIONSTYLEASSIGNMENT") {
      for (const auto styleRef : refList(argAt(*colorEntity, 0))) {
        if (auto color = colorFromRef(styleRef, visiting); color.has_value()) {
          return color;
        }
      }
    }

    return std::nullopt;
  }

  std::vector<glm::vec4> colorList(uint32_t ref) const {
    const Entity *list = entity(ref);
    if (list == nullptr || list->type != "IFCCOLOURRGBLIST") {
      return {};
    }
    std::vector<glm::vec4> colors;
    for (const StepValue &tuple : asList(argAt(*list, 0))) {
      const auto values = readNumberList(&tuple);
      if (values.size() < 3u) {
        continue;
      }
      colors.push_back(sanitizeColor({static_cast<float>(values[0]),
                                      static_cast<float>(values[1]),
                                      static_cast<float>(values[2]), 1.0f}));
    }
    return colors;
  }

  void cacheStyleColors() {
    for (const auto id : sortedEntityIds_) {
      const Entity *styledItem = entity(id);
      if (styledItem == nullptr || styledItem->type != "IFCSTYLEDITEM") {
        continue;
      }
      const auto itemRef = firstRef(*styledItem, 0);
      if (!itemRef.has_value()) {
        continue;
      }
      for (const auto styleRef : refList(argAt(*styledItem, 1))) {
        if (auto color = colorFromRef(styleRef); color.has_value()) {
          styleColorByItem_[*itemRef] = *color;
          break;
        }
      }
    }

    for (const auto id : sortedEntityIds_) {
      const Entity *colorMap = entity(id);
      if (colorMap == nullptr || colorMap->type != "IFCINDEXEDCOLOURMAP") {
        continue;
      }
      const auto mappedTo = firstRef(*colorMap, 0);
      const auto colorListRef = firstRef(*colorMap, 2);
      if (!mappedTo.has_value() || !colorListRef.has_value()) {
        continue;
      }
      const auto colors = colorList(*colorListRef);
      if (colors.empty()) {
        continue;
      }

      std::vector<glm::vec4> faceColors;
      for (const StepValue &colorIndex : asList(argAt(*colorMap, 3))) {
        if (colorIndex.kind != StepValue::Kind::Number ||
            colorIndex.number < 1.0) {
          continue;
        }
        const auto index =
            static_cast<size_t>(std::lround(colorIndex.number)) - 1u;
        if (index < colors.size()) {
          faceColors.push_back(colors[index]);
        }
      }
      if (!faceColors.empty()) {
        faceColorsByFaceSet_[*mappedTo] = std::move(faceColors);
      }
    }
  }

  void cacheVoidRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELVOIDSELEMENT") {
        continue;
      }

      const auto hostRef = firstRef(*relation, 4);
      const auto openingRef = firstRef(*relation, 5);
      if (!hostRef.has_value() || !openingRef.has_value()) {
        continue;
      }
      openingsByHostProduct_[*hostRef].push_back(*openingRef);
      hostByOpeningProduct_[*openingRef] = *hostRef;
      appendRelationshipReference(*hostRef, "IFCRELVOIDSELEMENT", "Opening",
                                  *openingRef);
    }
  }

  void cacheFillRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELFILLSELEMENT") {
        continue;
      }

      const auto openingRef = firstRef(*relation, 4);
      const auto fillerRef = firstRef(*relation, 5);
      if (!openingRef.has_value() || !fillerRef.has_value()) {
        continue;
      }

      appendRelationshipReference(*fillerRef, "IFCRELFILLSELEMENT", "Opening",
                                  *openingRef);
      appendRelationshipReference(*openingRef, "IFCRELFILLSELEMENT",
                                  "FilledBy", *fillerRef);
      if (const auto hostIt = hostByOpeningProduct_.find(*openingRef);
          hostIt != hostByOpeningProduct_.end()) {
        appendRelationshipReference(*fillerRef, "IFCRELFILLSELEMENT",
                                    "VoidsElement", hostIt->second);
        appendRelationshipReference(hostIt->second, "IFCRELFILLSELEMENT",
                                    "FilledBy", *fillerRef);
      }
    }
  }

  void cacheSpatialContainmentRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
        continue;
      }

      const auto structureRef = firstRef(*relation, 5);
      if (!structureRef.has_value()) {
        continue;
      }
      const Entity *structure = entity(*structureRef);
      if (structure == nullptr ||
          (structure->type != "IFCBUILDINGSTOREY" &&
           structure->type != "IFCSPACE")) {
        continue;
      }

      StoreyMetadata storey{};
      storey.id = stringValue(argAt(*structure, 0)).value_or("");
      if (storey.id.empty()) {
        storey.id = "#" + std::to_string(structure->id);
      }
      storey.name = stringValue(argAt(*structure, 2)).value_or("");
      for (const auto productRef : refList(argAt(*relation, 4))) {
        if (structure->type == "IFCBUILDINGSTOREY") {
          storeyByProduct_[productRef] = storey;
        }
        ProductMetadata &metadata = semanticPropertiesByProduct_[productRef];
        appendProperty(metadata,
                       makeProperty("IFCRELCONTAINEDINSPATIALSTRUCTURE",
                                    structure->type == "IFCSPACE" ? "Space"
                                                                  : "BuildingStorey",
                                    storey.name.empty() ? storey.id : storey.name,
                                    "reference"));
      }
    }
  }

  SemanticMaterialMetadata materialMetadata(uint32_t ref) const {
    std::unordered_set<uint32_t> visiting;
    return materialMetadata(ref, visiting);
  }

  SemanticMaterialMetadata
  materialMetadata(uint32_t ref, std::unordered_set<uint32_t> &visiting) const {
    if (!visiting.insert(ref).second) {
      return {};
    }

    const Entity *source = entity(ref);
    if (source == nullptr) {
      return {};
    }

    if (source->type == "IFCMATERIAL") {
      return SemanticMaterialMetadata{
          .name = stringValue(argAt(*source, 0)).value_or(""),
          .category = stringValue(argAt(*source, 2)).value_or(""),
      };
    }

    auto fromRef = [&](size_t index) {
      if (const auto nestedRef = firstRef(*source, index);
          nestedRef.has_value()) {
        return materialMetadata(*nestedRef, visiting);
      }
      return SemanticMaterialMetadata{};
    };

    if (source->type == "IFCMATERIALLAYER") {
      SemanticMaterialMetadata material = fromRef(0);
      if (material.name.empty()) {
        material.name = stringValue(argAt(*source, 3)).value_or("");
      }
      if (material.category.empty()) {
        material.category = stringValue(argAt(*source, 5)).value_or("");
      }
      return material;
    }

    if (source->type == "IFCMATERIALLAYERSET") {
      for (const auto layerRef : refList(argAt(*source, 0))) {
        SemanticMaterialMetadata material = materialMetadata(layerRef, visiting);
        if (!material.empty()) {
          if (material.category.empty()) {
            material.category = stringValue(argAt(*source, 1)).value_or("");
          }
          return material;
        }
      }
      return SemanticMaterialMetadata{
          .name = stringValue(argAt(*source, 1)).value_or(""),
      };
    }

    if (source->type == "IFCMATERIALLAYERSETUSAGE") {
      return fromRef(0);
    }

    if (source->type == "IFCMATERIALPROFILE") {
      SemanticMaterialMetadata material = fromRef(2);
      if (material.name.empty()) {
        material.name = stringValue(argAt(*source, 0)).value_or("");
      }
      if (material.category.empty()) {
        material.category = stringValue(argAt(*source, 5)).value_or("");
      }
      return material;
    }

    if (source->type == "IFCMATERIALCONSTITUENT") {
      SemanticMaterialMetadata material = fromRef(2);
      if (material.name.empty()) {
        material.name = stringValue(argAt(*source, 0)).value_or("");
      }
      if (material.category.empty()) {
        material.category = stringValue(argAt(*source, 4)).value_or("");
      }
      return material;
    }

    if (source->type == "IFCMATERIALPROFILESET" ||
        source->type == "IFCMATERIALCONSTITUENTSET" ||
        source->type == "IFCMATERIALLIST") {
      for (size_t i = 0; i < source->args.list.size(); ++i) {
        for (const auto nestedRef : refList(argAt(*source, i))) {
          SemanticMaterialMetadata material =
              materialMetadata(nestedRef, visiting);
          if (!material.empty()) {
            return material;
          }
        }
        SemanticMaterialMetadata material = fromRef(i);
        if (!material.empty()) {
          return material;
        }
      }
    }

    return {};
  }

  void cacheMaterialRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELASSOCIATESMATERIAL") {
        continue;
      }

      const auto materialRef = firstRef(*relation, 5);
      if (!materialRef.has_value()) {
        continue;
      }
      const SemanticMaterialMetadata material = materialMetadata(*materialRef);
      ProductMetadata materialProperties = materialBrowsingMetadata(*materialRef);
      const std::string materialValue =
          material.name.empty() ? materialLabel(*materialRef) : material.name;
      appendProperty(materialProperties,
                     makeProperty("IFCRELASSOCIATESMATERIAL", "Material",
                                  materialValue, "relationship"));
      appendProperty(materialProperties,
                     makeProperty("IFCRELASSOCIATESMATERIAL", "MaterialId",
                                  fallbackRefValue(*materialRef),
                                  "relationship"));
      for (const auto productRef : refList(argAt(*relation, 4))) {
        if (!material.empty()) {
          materialByProduct_[productRef] = material;
        }
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, materialProperties);
      }
    }
  }

  static void appendProperty(
      ProductMetadata &metadata,
      container::geometry::dotbim::ElementProperty property) {
    if (property.name.empty() || property.value.empty()) {
      return;
    }
    const auto duplicate = std::ranges::find_if(
        metadata.properties,
        [&](const container::geometry::dotbim::ElementProperty &existing) {
          return existing.set == property.set &&
                 existing.name == property.name &&
                 existing.category == property.category;
        });
    if (duplicate == metadata.properties.end()) {
      metadata.properties.push_back(std::move(property));
    }
  }

  static std::string joinedScalarValues(const StepValue *value) {
    if (value == nullptr) {
      return {};
    }
    if (value->kind != StepValue::Kind::List) {
      return scalarLabelValue(value).value_or("");
    }

    std::string result;
    for (const StepValue &item : value->list) {
      const std::string itemValue = joinedScalarValues(&item);
      if (itemValue.empty()) {
        continue;
      }
      if (!result.empty()) {
        result += ", ";
      }
      result += itemValue;
    }
    return result;
  }

  static container::geometry::dotbim::ElementProperty
  makeProperty(std::string set, std::string name, std::string value,
               std::string category) {
    container::geometry::dotbim::ElementProperty property{};
    property.set = std::move(set);
    property.name = std::move(name);
    property.value = std::move(value);
    property.category = std::move(category);
    return property;
  }

  static std::string fallbackRefValue(uint32_t ref) {
    return "#" + std::to_string(ref);
  }

  std::string entityNameOrId(uint32_t ref) const {
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return fallbackRefValue(ref);
    }
    std::string value = stringValue(argAt(*source, 2)).value_or("");
    if (value.empty()) {
      value = stringValue(argAt(*source, 0)).value_or("");
    }
    return value.empty() ? fallbackRefValue(ref) : value;
  }

  void appendRelationshipReference(ProductMetadata &metadata,
                                   std::string_view set, std::string name,
                                   uint32_t relatedRef) const {
    appendProperty(metadata,
                   makeProperty(std::string(set), name, entityNameOrId(relatedRef),
                                "relationship"));
    appendProperty(metadata,
                   makeProperty(std::string(set), name + "Id",
                                fallbackRefValue(relatedRef), "relationship"));
  }

  void appendRelationshipReference(uint32_t productRef, std::string_view set,
                                   std::string name, uint32_t relatedRef) {
    appendRelationshipReference(semanticPropertiesByProduct_[productRef], set,
                                std::move(name), relatedRef);
  }

  static void appendMaterialProperty(ProductMetadata &metadata,
                                     std::string set, std::string name,
                                     std::string value) {
    appendProperty(metadata,
                   makeProperty(std::move(set), std::move(name),
                                std::move(value), "material"));
  }

  static std::vector<uint32_t> refsAt(const Entity &source, size_t index) {
    std::vector<uint32_t> refs = refList(argAt(source, index));
    if (refs.empty()) {
      if (const auto ref = refValue(argAt(source, index)); ref.has_value()) {
        refs.push_back(*ref);
      }
    }
    return refs;
  }

  std::string materialLabel(uint32_t ref) const {
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return fallbackRefValue(ref);
    }

    if (source->type == "IFCMATERIAL") {
      return stringValue(argAt(*source, 0)).value_or(fallbackRefValue(ref));
    }
    if (source->type == "IFCMATERIALLAYER") {
      std::string value = stringValue(argAt(*source, 3)).value_or("");
      if (!value.empty()) {
        return value;
      }
    }
    if (source->type == "IFCMATERIALLAYERSET") {
      return stringValue(argAt(*source, 1)).value_or(fallbackRefValue(ref));
    }
    if (source->type == "IFCMATERIALLAYERSETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        return materialLabel(*setRef);
      }
    }
    if (source->type == "IFCMATERIALPROFILE" ||
        source->type == "IFCMATERIALCONSTITUENT") {
      std::string value = stringValue(argAt(*source, 0)).value_or("");
      if (!value.empty()) {
        return value;
      }
    }
    if (source->type == "IFCMATERIALPROFILESET" ||
        source->type == "IFCMATERIALCONSTITUENTSET") {
      return stringValue(argAt(*source, 0)).value_or(fallbackRefValue(ref));
    }
    if (source->type == "IFCMATERIALPROFILESETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        return materialLabel(*setRef);
      }
    }

    const SemanticMaterialMetadata material = materialMetadata(ref);
    if (!material.name.empty()) {
      return material.name;
    }
    return entityNameOrId(ref);
  }

  ProductMetadata materialBrowsingMetadata(uint32_t ref) const {
    std::unordered_set<uint32_t> visiting;
    return materialBrowsingMetadata(ref, visiting);
  }

  ProductMetadata
  materialBrowsingMetadata(uint32_t ref,
                           std::unordered_set<uint32_t> &visiting) const {
    ProductMetadata metadata{};
    if (!visiting.insert(ref).second) {
      return metadata;
    }

    const Entity *source = entity(ref);
    if (source == nullptr) {
      return metadata;
    }

    auto mergeNested = [&](uint32_t nestedRef) {
      mergeSemanticProperties(metadata,
                              materialBrowsingMetadata(nestedRef, visiting));
    };

    if (source->type == "IFCMATERIAL") {
      appendMaterialProperty(metadata, "IFCMATERIAL", "Name",
                             stringValue(argAt(*source, 0)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIAL", "Description",
                             stringValue(argAt(*source, 1)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIAL", "Category",
                             stringValue(argAt(*source, 2)).value_or(""));
      return metadata;
    }

    if (source->type == "IFCMATERIALLAYER") {
      const SemanticMaterialMetadata material = materialMetadata(ref);
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Layer",
                             materialLabel(ref));
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Material",
                             material.name);
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Thickness",
                             joinedScalarValues(argAt(*source, 1)));
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Category",
                             stringValue(argAt(*source, 5))
                                 .value_or(material.category));
      if (const auto materialRef = firstRef(*source, 0);
          materialRef.has_value()) {
        mergeNested(*materialRef);
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALLAYERSET") {
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSET", "LayerSet",
                             materialLabel(ref));
      size_t layerIndex = 1u;
      for (const auto layerRef : refsAt(*source, 0)) {
        const Entity *layer = entity(layerRef);
        const SemanticMaterialMetadata material = materialMetadata(layerRef);
        const std::string prefix = "Layer." + std::to_string(layerIndex);
        appendMaterialProperty(metadata, "IFCMATERIALLAYERSET", prefix,
                               material.name.empty() ? materialLabel(layerRef)
                                                     : material.name);
        if (layer != nullptr) {
          appendMaterialProperty(
              metadata, "IFCMATERIALLAYERSET", prefix + ".Name",
              stringValue(argAt(*layer, 3)).value_or(""));
          appendMaterialProperty(metadata, "IFCMATERIALLAYERSET",
                                 prefix + ".Thickness",
                                 joinedScalarValues(argAt(*layer, 1)));
          appendMaterialProperty(
              metadata, "IFCMATERIALLAYERSET", prefix + ".Category",
              stringValue(argAt(*layer, 5)).value_or(material.category));
        }
        mergeNested(layerRef);
        ++layerIndex;
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALLAYERSETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                               "LayerSet", materialLabel(*setRef));
        mergeNested(*setRef);
      }
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                             "LayerSetDirection",
                             enumValue(argAt(*source, 1)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                             "DirectionSense",
                             enumValue(argAt(*source, 2)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                             "OffsetFromReferenceLine",
                             joinedScalarValues(argAt(*source, 3)));
      return metadata;
    }

    if (source->type == "IFCMATERIALPROFILE") {
      const SemanticMaterialMetadata material = materialMetadata(ref);
      appendMaterialProperty(metadata, "IFCMATERIALPROFILE", "Profile",
                             materialLabel(ref));
      appendMaterialProperty(metadata, "IFCMATERIALPROFILE", "Material",
                             material.name);
      appendMaterialProperty(metadata, "IFCMATERIALPROFILE", "Category",
                             stringValue(argAt(*source, 5))
                                 .value_or(material.category));
      if (const auto profileRef = firstRef(*source, 3);
          profileRef.has_value()) {
        appendMaterialProperty(metadata, "IFCMATERIALPROFILE",
                               "ProfileDefinition",
                               entityNameOrId(*profileRef));
      }
      if (const auto materialRef = firstRef(*source, 2);
          materialRef.has_value()) {
        mergeNested(*materialRef);
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALPROFILESET") {
      appendMaterialProperty(metadata, "IFCMATERIALPROFILESET", "ProfileSet",
                             materialLabel(ref));
      size_t profileIndex = 1u;
      for (const auto profileRef : refsAt(*source, 2)) {
        const SemanticMaterialMetadata material = materialMetadata(profileRef);
        const std::string prefix = "Profile." + std::to_string(profileIndex);
        appendMaterialProperty(metadata, "IFCMATERIALPROFILESET", prefix,
                               materialLabel(profileRef));
        appendMaterialProperty(metadata, "IFCMATERIALPROFILESET",
                               prefix + ".Material", material.name);
        mergeNested(profileRef);
        ++profileIndex;
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALPROFILESETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        appendMaterialProperty(metadata, "IFCMATERIALPROFILESETUSAGE",
                               "ProfileSet", materialLabel(*setRef));
        mergeNested(*setRef);
      }
      appendMaterialProperty(metadata, "IFCMATERIALPROFILESETUSAGE",
                             "CardinalPoint",
                             joinedScalarValues(argAt(*source, 1)));
      appendMaterialProperty(metadata, "IFCMATERIALPROFILESETUSAGE",
                             "ReferenceExtent",
                             joinedScalarValues(argAt(*source, 2)));
      return metadata;
    }

    if (source->type == "IFCMATERIALCONSTITUENT") {
      const SemanticMaterialMetadata material = materialMetadata(ref);
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENT",
                             "Constituent", materialLabel(ref));
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENT", "Material",
                             material.name);
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENT", "Category",
                             stringValue(argAt(*source, 4))
                                 .value_or(material.category));
      if (const auto materialRef = firstRef(*source, 2);
          materialRef.has_value()) {
        mergeNested(*materialRef);
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALCONSTITUENTSET") {
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENTSET",
                             "ConstituentSet", materialLabel(ref));
      size_t constituentIndex = 1u;
      for (const auto constituentRef : refsAt(*source, 2)) {
        const SemanticMaterialMetadata material =
            materialMetadata(constituentRef);
        const std::string prefix =
            "Constituent." + std::to_string(constituentIndex);
        appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENTSET", prefix,
                               materialLabel(constituentRef));
        appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENTSET",
                               prefix + ".Material", material.name);
        mergeNested(constituentRef);
        ++constituentIndex;
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALLIST") {
      size_t materialIndex = 1u;
      for (const auto nestedRef : refsAt(*source, 0)) {
        appendMaterialProperty(metadata, "IFCMATERIALLIST",
                               "Material." + std::to_string(materialIndex),
                               materialLabel(nestedRef));
        mergeNested(nestedRef);
        ++materialIndex;
      }
    }
    return metadata;
  }

  static void assignSemanticProperty(ProductMetadata &metadata,
                                     std::string_view name,
                                     std::string value) {
    if (value.empty()) {
      return;
    }
    const std::string key = semanticPropertyKey(name);
    if ((key == "DISCIPLINE" || key == "IFCDISCIPLINE") &&
        metadata.discipline.empty()) {
      metadata.discipline = std::move(value);
    } else if ((key == "PHASE" || key == "PHASENAME" ||
                key == "CONSTRUCTIONPHASE") &&
               metadata.phase.empty()) {
      metadata.phase = std::move(value);
    } else if ((key == "FIRERATING" || key == "FIRERESISTANCERATING" ||
                key == "FIRECLASSIFICATION") &&
               metadata.fireRating.empty()) {
      metadata.fireRating = std::move(value);
    } else if ((key == "LOADBEARING" || key == "ISLOADBEARING") &&
               metadata.loadBearing.empty()) {
      metadata.loadBearing = std::move(value);
    } else if ((key == "STATUS" || key == "ELEMENTSTATUS") &&
               metadata.status.empty()) {
      metadata.status = std::move(value);
    }
  }

  void mergeSemanticProperties(ProductMetadata &target,
                               const ProductMetadata &source) const {
    if (target.discipline.empty()) {
      target.discipline = source.discipline;
    }
    if (target.phase.empty()) {
      target.phase = source.phase;
    }
    if (target.fireRating.empty()) {
      target.fireRating = source.fireRating;
    }
    if (target.loadBearing.empty()) {
      target.loadBearing = source.loadBearing;
    }
    if (target.status.empty()) {
      target.status = source.status;
    }
    for (const auto &property : source.properties) {
      appendProperty(target, property);
    }
  }

  ProductMetadata propertySetMetadata(uint32_t ref) const {
    ProductMetadata metadata{};
    const Entity *propertySet = entity(ref);
    if (propertySet == nullptr) {
      return metadata;
    }

    if (propertySet->type == "IFCPROPERTYSET") {
      const std::string setName =
          stringValue(argAt(*propertySet, 2)).value_or(fallbackRefValue(ref));
      for (const auto propertyRef : refList(argAt(*propertySet, 4))) {
        mergeSemanticProperties(metadata,
                                propertyMetadata(propertyRef, setName, "pset"));
      }
    } else if (propertySet->type == "IFCELEMENTQUANTITY") {
      const std::string setName =
          stringValue(argAt(*propertySet, 2)).value_or("BaseQuantities");
      for (const auto quantityRef : refList(argAt(*propertySet, 5))) {
        mergeSemanticProperties(metadata, quantityMetadata(quantityRef, setName));
      }
    } else if (propertySet->type == "IFCCOMPLEXPROPERTY") {
      mergeSemanticProperties(metadata, propertyMetadata(ref, {}, "pset"));
    } else {
      mergeSemanticProperties(metadata, propertyMetadata(ref, {}, "pset"));
    }
    return metadata;
  }

  ProductMetadata propertyMetadata(uint32_t ref, std::string_view setName,
                                   std::string_view category) const {
    ProductMetadata metadata{};
    const Entity *property = entity(ref);
    if (property == nullptr) {
      return metadata;
    }

    if (property->type == "IFCPROPERTYSINGLEVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      const std::string value = joinedScalarValues(argAt(*property, 2));
      assignSemanticProperty(metadata, name, value);
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYENUMERATEDVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      const std::string value = joinedScalarValues(argAt(*property, 2));
      assignSemanticProperty(metadata, name, value);
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYLISTVALUE" ||
               property->type == "IFCPROPERTYTABLEVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      const std::string value = joinedScalarValues(argAt(*property, 2));
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYREFERENCEVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      std::string value;
      if (const auto reference = refValue(argAt(*property, 3));
          reference.has_value()) {
        value = entityNameOrId(*reference);
      } else {
        value = joinedScalarValues(argAt(*property, 3));
      }
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYBOUNDEDVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      appendProperty(metadata,
                     makeProperty(std::string(setName), name + ".UpperBound",
                                  joinedScalarValues(argAt(*property, 2)),
                                  std::string(category)));
      appendProperty(metadata,
                     makeProperty(std::string(setName), name + ".LowerBound",
                                  joinedScalarValues(argAt(*property, 3)),
                                  std::string(category)));
    } else if (property->type == "IFCCOMPLEXPROPERTY") {
      const std::string complexName =
          stringValue(argAt(*property, 0)).value_or(fallbackRefValue(ref));
      std::string nestedSet = std::string(setName);
      if (nestedSet.empty()) {
        nestedSet = complexName;
      } else if (!complexName.empty()) {
        nestedSet += "." + complexName;
      }
      for (const auto propertyRef : refList(argAt(*property, 3))) {
        mergeSemanticProperties(
            metadata, propertyMetadata(propertyRef, nestedSet, category));
      }
    }
    return metadata;
  }

  ProductMetadata quantityMetadata(uint32_t ref, std::string_view setName) const {
    ProductMetadata metadata{};
    const Entity *quantity = entity(ref);
    if (quantity == nullptr) {
      return metadata;
    }

    if (quantity->type == "IFCPHYSICALCOMPLEXQUANTITY") {
      const std::string complexName =
          stringValue(argAt(*quantity, 0)).value_or(fallbackRefValue(ref));
      std::string nestedSet = std::string(setName);
      if (!complexName.empty()) {
        nestedSet = nestedSet.empty() ? complexName : nestedSet + "." + complexName;
      }
      for (const auto quantityRef : refList(argAt(*quantity, 2))) {
        mergeSemanticProperties(metadata,
                                quantityMetadata(quantityRef, nestedSet));
      }
      return metadata;
    }

    if (!quantity->type.starts_with("IFCQUANTITY")) {
      return metadata;
    }

    const std::string name = stringValue(argAt(*quantity, 0)).value_or("");
    const std::string value = joinedScalarValues(argAt(*quantity, 3));
    appendProperty(metadata,
                   makeProperty(std::string(setName), name, value, "quantity"));
    return metadata;
  }

  void cachePropertyRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELDEFINESBYPROPERTIES") {
        continue;
      }

      const auto propertySetRef = firstRef(*relation, 5);
      if (!propertySetRef.has_value()) {
        continue;
      }
      const ProductMetadata properties = propertySetMetadata(*propertySetRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, properties);
      }
    }
  }

  std::string classificationSourceName(uint32_t ref) const {
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return {};
    }
    if (source->type == "IFCCLASSIFICATIONREFERENCE") {
      if (const auto sourceRef = firstRef(*source, 3); sourceRef.has_value()) {
        const std::string nested = classificationSourceName(*sourceRef);
        if (!nested.empty()) {
          return nested;
        }
      }
      return stringValue(argAt(*source, 2)).value_or("");
    }
    if (source->type == "IFCCLASSIFICATION") {
      for (const size_t index : {3u, 0u, 1u, 4u}) {
        const std::string value = stringValue(argAt(*source, index)).value_or("");
        if (!value.empty()) {
          return value;
        }
      }
    }
    return {};
  }

  ProductMetadata classificationMetadata(uint32_t ref) const {
    ProductMetadata metadata{};
    const Entity *classification = entity(ref);
    if (classification == nullptr) {
      return metadata;
    }

    if (classification->type == "IFCCLASSIFICATIONREFERENCE") {
      std::string value = stringValue(argAt(*classification, 1)).value_or("");
      if (value.empty()) {
        value = stringValue(argAt(*classification, 0)).value_or("");
      }
      std::string name =
          stringValue(argAt(*classification, 2)).value_or("Classification");
      if (name.empty()) {
        name = "Classification";
      }
      std::string set = classificationSourceName(ref);
      if (set.empty()) {
        set = "Classification";
      }
      appendProperty(metadata,
                     makeProperty(std::move(set), std::move(name),
                                  std::move(value), "classification"));
    } else if (classification->type == "IFCCLASSIFICATION") {
      std::string value = classificationSourceName(ref);
      appendProperty(metadata,
                     makeProperty("Classification", "Classification",
                                  std::move(value), "classification"));
    }
    return metadata;
  }

  void cacheClassificationRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELASSOCIATESCLASSIFICATION") {
        continue;
      }
      const auto classificationRef = firstRef(*relation, 5);
      if (!classificationRef.has_value()) {
        continue;
      }
      const ProductMetadata classification =
          classificationMetadata(*classificationRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, classification);
      }
    }
  }

  ProductMetadata groupMetadata(uint32_t ref) const {
    ProductMetadata metadata{};
    const Entity *group = entity(ref);
    if (group == nullptr) {
      return metadata;
    }

    std::string name = "Group";
    if (group->type == "IFCSYSTEM") {
      name = "System";
    } else if (group->type == "IFCZONE") {
      name = "Zone";
    } else if (group->type == "IFCSPACE") {
      name = "Space";
    }
    appendProperty(metadata,
                   makeProperty(group->type, name, entityNameOrId(ref),
                                "reference"));
    return metadata;
  }

  void cacheGroupRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELASSIGNSTOGROUP") {
        continue;
      }
      const auto groupRef = firstRef(*relation, 6);
      if (!groupRef.has_value()) {
        continue;
      }
      const ProductMetadata group = groupMetadata(*groupRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, group);
      }
    }
  }

  void cacheHierarchyRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          (relation->type != "IFCRELAGGREGATES" &&
           relation->type != "IFCRELNESTS")) {
        continue;
      }

      const auto parentRef = firstRef(*relation, 4);
      if (!parentRef.has_value()) {
        continue;
      }

      size_t childIndex = 1u;
      for (const auto childRef : refList(argAt(*relation, 5))) {
        appendRelationshipReference(childRef, relation->type, "Parent",
                                    *parentRef);
        appendRelationshipReference(*parentRef, relation->type,
                                    "Child." + std::to_string(childIndex),
                                    childRef);
        ++childIndex;
      }
    }
  }

  ProductMetadata typeObjectMetadata(uint32_t typeRef) const {
    ProductMetadata metadata{};
    const Entity *typeObject = entity(typeRef);
    if (typeObject == nullptr) {
      return metadata;
    }

    appendProperty(metadata,
                   makeProperty("IFCRELDEFINESBYTYPE", "Type",
                                entityNameOrId(typeRef), "relationship"));
    appendProperty(metadata,
                   makeProperty("IFCRELDEFINESBYTYPE", "TypeId",
                                fallbackRefValue(typeRef), "relationship"));
    appendProperty(metadata,
                   makeProperty("IFCRELDEFINESBYTYPE", "TypeEntity",
                                typeObject->type, "relationship"));

    for (const size_t propertySetIndex : {5u, 6u}) {
      for (const auto propertySetRef : refsAt(*typeObject, propertySetIndex)) {
        mergeSemanticProperties(metadata, propertySetMetadata(propertySetRef));
      }
    }

    if (const auto typeSemanticIt = semanticPropertiesByProduct_.find(typeRef);
        typeSemanticIt != semanticPropertiesByProduct_.end()) {
      mergeSemanticProperties(metadata, typeSemanticIt->second);
    }
    return metadata;
  }

  void cacheTypeRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELDEFINESBYTYPE") {
        continue;
      }

      const auto typeRef = firstRef(*relation, 5);
      if (!typeRef.has_value()) {
        continue;
      }

      const ProductMetadata typeMetadata = typeObjectMetadata(*typeRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, typeMetadata);
        if (!materialByProduct_.contains(productRef)) {
          if (const auto materialIt = materialByProduct_.find(*typeRef);
              materialIt != materialByProduct_.end()) {
            materialByProduct_[productRef] = materialIt->second;
          }
        }
      }
    }
  }

  ProductMetadata productMetadata(const Entity &product) const {
    ProductMetadata metadata{};
    metadata.guid = stringValue(argAt(product, 0)).value_or("");
    metadata.displayName = stringValue(argAt(product, 2)).value_or("");
    metadata.objectType = stringValue(argAt(product, 4)).value_or("");
    metadata.sourceId = "#" + std::to_string(product.id);

    if (const auto storeyIt = storeyByProduct_.find(product.id);
        storeyIt != storeyByProduct_.end()) {
      metadata.storeyId = storeyIt->second.id;
      metadata.storeyName = storeyIt->second.name;
    }
    if (const auto materialIt = materialByProduct_.find(product.id);
        materialIt != materialByProduct_.end()) {
      metadata.materialName = materialIt->second.name;
      metadata.materialCategory = materialIt->second.category;
    }
    if (const auto semanticIt = semanticPropertiesByProduct_.find(product.id);
        semanticIt != semanticPropertiesByProduct_.end()) {
      mergeSemanticProperties(metadata, semanticIt->second);
    }
    return metadata;
  }

  void applyProductMetadata(container::geometry::dotbim::Element &element,
                            const ProductMetadata &metadata) const {
    element.guid = metadata.guid;
    element.displayName = metadata.displayName;
    element.objectType = metadata.objectType;
    element.storeyName = metadata.storeyName;
    element.storeyId = metadata.storeyId;
    element.materialName = metadata.materialName;
    element.materialCategory = metadata.materialCategory;
    element.discipline = metadata.discipline;
    element.phase = metadata.phase;
    element.fireRating = metadata.fireRating;
    element.loadBearing = metadata.loadBearing;
    element.status = metadata.status;
    element.sourceId = metadata.sourceId;
    element.properties = metadata.properties;
  }

  void appendTessellatedGeometry() {
    for (const auto id : sortedEntityIds_) {
      const Entity *faceSet = entity(id);
      if (faceSet == nullptr || faceSet->type != "IFCTRIANGULATEDFACESET") {
        continue;
      }
      auto groups = appendFaceSetGeometry(*faceSet);
      if (!groups.empty()) {
        groupsByItem_[id] = std::move(groups);
      }
    }
  }

  std::optional<MeshGroup>
  appendTriangleMesh(std::span<const TriangleVertices> triangles,
                     glm::vec4 color) {
    if (triangles.empty()) {
      return std::nullopt;
    }

    const uint32_t firstIndex = static_cast<uint32_t>(model_.indices.size());
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (const auto &triangle : triangles) {
      const auto &positions = triangle.positions;
      const glm::vec3 normal =
          safeNormal(positions[0], positions[1], positions[2]);
      const glm::vec3 tangent = safeTangent(positions[0], positions[1], normal);
      const uint32_t base = static_cast<uint32_t>(model_.vertices.size());
      model_.vertices.push_back(makeVertex(positions[0], normal, tangent));
      model_.vertices.push_back(makeVertex(positions[1], normal, tangent));
      model_.vertices.push_back(makeVertex(positions[2], normal, tangent));
      model_.indices.insert(model_.indices.end(), {base, base + 1u, base + 2u});

      for (const auto &position : positions) {
        minBounds = glm::min(minBounds, position);
        maxBounds = glm::max(maxBounds, position);
      }
    }

    const uint32_t indexCount =
        static_cast<uint32_t>(model_.indices.size()) - firstIndex;
    if (indexCount == 0u) {
      return std::nullopt;
    }

    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    float radius = 0.0f;
    for (const auto &triangle : triangles) {
      for (const auto &position : triangle.positions) {
        radius = std::max(radius, glm::length(position - center));
      }
    }

    const uint32_t meshId = nextMeshId_++;
    model_.meshRanges.push_back(
        {meshId, firstIndex, indexCount, center, radius});
    return MeshGroup{meshId, sanitizeColor(color)};
  }

  std::vector<MeshGroup> appendFaceSetGeometry(const Entity &faceSet) {
    const auto pointsRef = firstRef(faceSet, 0);
    if (!pointsRef.has_value()) {
      return {};
    }
    const auto points = readPointList3D(*pointsRef);
    if (points.empty()) {
      return {};
    }

    const auto faceColorsIt = faceColorsByFaceSet_.find(faceSet.id);
    const std::vector<glm::vec4> *faceColors =
        faceColorsIt == faceColorsByFaceSet_.end() ? nullptr
                                                   : &faceColorsIt->second;
    const glm::vec4 baseColor = styleColorByItem_.contains(faceSet.id)
                                    ? styleColorByItem_.at(faceSet.id)
                                    : defaultColor();

    struct Triangle {
      std::array<uint32_t, 3> indices{};
    };
    struct Group {
      glm::vec4 color{defaultColor()};
      std::vector<Triangle> triangles{};
    };
    std::map<uint32_t, Group> groups;

    size_t faceIndex = 0;
    for (const StepValue &faceValue : asList(argAt(faceSet, 3))) {
      const auto indices = readPositiveIndexList(&faceValue);
      if (indices.size() < 3u) {
        ++faceIndex;
        continue;
      }
      const glm::vec4 color =
          faceColors != nullptr && faceIndex < faceColors->size()
              ? (*faceColors)[faceIndex]
              : baseColor;
      const uint32_t colorKey = packColor(color);
      auto &group = groups[colorKey];
      group.color = color;
      for (size_t i = 1; i + 1u < indices.size(); ++i) {
        group.triangles.push_back(
            Triangle{{indices[0], indices[i], indices[i + 1u]}});
      }
      ++faceIndex;
    }

    std::vector<MeshGroup> meshGroups;
    for (const auto &[_, group] : groups) {
      if (group.triangles.empty()) {
        continue;
      }

      std::vector<TriangleVertices> triangles;
      triangles.reserve(group.triangles.size());
      for (const Triangle &triangle : group.triangles) {
        std::array<glm::vec3, 3> positions{};
        bool valid = true;
        for (size_t i = 0; i < triangle.indices.size(); ++i) {
          const uint32_t oneBasedIndex = triangle.indices[i];
          if (oneBasedIndex == 0u || oneBasedIndex > points.size()) {
            valid = false;
            break;
          }
          positions[i] = points[oneBasedIndex - 1u];
        }
        if (!valid) {
          continue;
        }
        triangles.push_back({positions});
      }

      auto meshGroup = appendTriangleMesh(triangles, group.color);
      if (!meshGroup.has_value()) {
        continue;
      }
      meshGroups.push_back(*meshGroup);
    }

    return meshGroups;
  }

  void appendSweptSolidGeometry() {
    for (const auto id : sortedEntityIds_) {
      const Entity *solid = entity(id);
      if (solid == nullptr || solid->type != "IFCEXTRUDEDAREASOLID") {
        continue;
      }
      if (auto group = appendExtrudedAreaSolidGeometry(*solid);
          group.has_value()) {
        groupsByItem_[id] = {*group};
      }
    }
  }

  std::optional<MeshGroup>
  appendExtrudedAreaSolidGeometry(const Entity &solid) {
    const auto profileRef = firstRef(solid, 0);
    if (!profileRef.has_value()) {
      return std::nullopt;
    }

    const auto profilePoints = readArbitraryClosedProfilePoints(*profileRef);
    if (profilePoints.size() < 3u) {
      return std::nullopt;
    }

    const auto depth = numberValue(argAt(solid, 3));
    if (!depth.has_value() || !std::isfinite(*depth) || *depth <= 0.0) {
      return std::nullopt;
    }

    glm::mat4 solidPlacement(1.0f);
    if (const auto positionRef = firstRef(solid, 1); positionRef.has_value()) {
      solidPlacement = axis2Placement3D(*positionRef);
    }

    glm::vec3 direction(0.0f, 0.0f, 1.0f);
    if (const auto directionRef = firstRef(solid, 2);
        directionRef.has_value()) {
      direction = readDirection(*directionRef, direction);
    }
    const glm::vec3 extrusionVector = direction * static_cast<float>(*depth);

    std::vector<glm::vec3> bottom;
    std::vector<glm::vec3> top;
    bottom.reserve(profilePoints.size());
    top.reserve(profilePoints.size());
    for (const auto &point : profilePoints) {
      const glm::vec3 base = glm::vec3(solidPlacement * glm::vec4(point, 1.0f));
      const glm::vec3 cap =
          glm::vec3(solidPlacement * glm::vec4(point + extrusionVector, 1.0f));
      bottom.push_back(base);
      top.push_back(cap);
    }

    glm::vec3 extrusionWorld = top.front() - bottom.front();
    if (glm::dot(extrusionWorld, extrusionWorld) <= 1.0e-12f) {
      return std::nullopt;
    }
    extrusionWorld = glm::normalize(extrusionWorld);

    std::vector<TriangleVertices> triangles;
    triangles.reserve((profilePoints.size() - 2u) * 2u +
                      profilePoints.size() * 2u);

    for (size_t i = 1; i + 1u < profilePoints.size(); ++i) {
      appendCapTriangle(triangles, {bottom[0], bottom[i], bottom[i + 1u]},
                        -extrusionWorld);
      appendCapTriangle(triangles, {top[0], top[i], top[i + 1u]},
                        extrusionWorld);
    }

    for (size_t i = 0; i < profilePoints.size(); ++i) {
      const size_t next = (i + 1u) % profilePoints.size();
      triangles.push_back({{bottom[i], bottom[next], top[next]}});
      triangles.push_back({{bottom[i], top[next], top[i]}});
    }

    const glm::vec4 color = styleColorByItem_.contains(solid.id)
                                ? styleColorByItem_.at(solid.id)
                                : defaultColor();
    if (auto boxSolid = makeBoxSolid(bottom, top, color);
        boxSolid.has_value()) {
      boxSolidsByItem_[solid.id] = *boxSolid;
    }
    return appendTriangleMesh(triangles, color);
  }

  static void appendCapTriangle(std::vector<TriangleVertices> &triangles,
                                std::array<glm::vec3, 3> positions,
                                const glm::vec3 &expectedNormal) {
    const glm::vec3 normal =
        safeNormal(positions[0], positions[1], positions[2]);
    if (glm::dot(normal, expectedNormal) < 0.0f) {
      std::swap(positions[1], positions[2]);
    }
    triangles.push_back({positions});
  }

  static bool hasPositiveSpan(const BoxSolid &box) {
    for (AxisIndex axis = 0; axis < 3; ++axis) {
      if (box.maxBounds[axis] - box.minBounds[axis] <= 1.0e-4f) {
        return false;
      }
    }
    return true;
  }

  static std::optional<BoxSolid> makeBoxSolid(std::span<const glm::vec3> bottom,
                                              std::span<const glm::vec3> top,
                                              glm::vec4 color) {
    if (bottom.size() != 4u || top.size() != 4u) {
      return std::nullopt;
    }

    BoxSolid box{};
    box.minBounds = glm::vec3(std::numeric_limits<float>::max());
    box.maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    box.color = sanitizeColor(color);

    for (const auto points : {bottom, top}) {
      for (const auto &point : points) {
        for (AxisIndex axis = 0; axis < 3; ++axis) {
          if (!std::isfinite(point[axis])) {
            return std::nullopt;
          }
        }
        box.minBounds = glm::min(box.minBounds, point);
        box.maxBounds = glm::max(box.maxBounds, point);
      }
    }
    if (!hasPositiveSpan(box)) {
      return std::nullopt;
    }

    std::array<bool, 8> seenCorners{};
    for (const auto points : {bottom, top}) {
      for (const auto &point : points) {
        uint32_t cornerMask = 0;
        for (AxisIndex axis = 0; axis < 3; ++axis) {
          const float toMin = std::abs(point[axis] - box.minBounds[axis]);
          const float toMax = std::abs(point[axis] - box.maxBounds[axis]);
          if (toMin <= 1.0e-4f) {
            continue;
          }
          if (toMax <= 1.0e-4f) {
            cornerMask |= (1u << static_cast<uint32_t>(axis));
            continue;
          }
          return std::nullopt;
        }
        seenCorners[cornerMask] = true;
      }
    }

    if (!std::ranges::all_of(seenCorners, [](bool seen) { return seen; })) {
      return std::nullopt;
    }
    return box;
  }

  static std::array<glm::vec3, 8> boxCorners(const BoxSolid &box) {
    std::array<glm::vec3, 8> corners{};
    for (uint32_t mask = 0; mask < 8u; ++mask) {
      glm::vec3 corner{};
      for (AxisIndex axis = 0; axis < 3; ++axis) {
        const uint32_t axisBit = 1u << static_cast<uint32_t>(axis);
        corner[axis] =
            (mask & axisBit) != 0u ? box.maxBounds[axis] : box.minBounds[axis];
      }
      corners[mask] = corner;
    }
    return corners;
  }

  static BoxSolid transformBox(const BoxSolid &box,
                               const glm::mat4 &transform) {
    BoxSolid result{};
    result.minBounds = glm::vec3(std::numeric_limits<float>::max());
    result.maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    result.color = box.color;
    for (const auto &corner : boxCorners(box)) {
      const glm::vec3 transformed =
          glm::vec3(transform * glm::vec4(corner, 1.0f));
      result.minBounds = glm::min(result.minBounds, transformed);
      result.maxBounds = glm::max(result.maxBounds, transformed);
    }
    return result;
  }

  static std::optional<BoxSolid> intersectBoxes(const BoxSolid &a,
                                                const BoxSolid &b) {
    BoxSolid result{};
    result.minBounds = glm::max(a.minBounds, b.minBounds);
    result.maxBounds = glm::min(a.maxBounds, b.maxBounds);
    result.color = a.color;
    if (!hasPositiveSpan(result)) {
      return std::nullopt;
    }
    return result;
  }

  static glm::vec3 axisDirection(AxisIndex axis, float sign) {
    glm::vec3 direction(0.0f);
    direction[axis] = sign;
    return direction;
  }

  static std::array<AxisIndex, 2> remainingAxes(AxisIndex axis) {
    if (axis == 0) {
      return {1, 2};
    }
    if (axis == 1) {
      return {0, 2};
    }
    return {0, 1};
  }

  static std::optional<AxisIndex> throughAxisForCut(const BoxSolid &host,
                                                    const BoxSolid &cut) {
    constexpr float kEpsilon = 1.0e-4f;
    for (AxisIndex axis = 0; axis < 3; ++axis) {
      if (std::abs(cut.minBounds[axis] - host.minBounds[axis]) > kEpsilon ||
          std::abs(cut.maxBounds[axis] - host.maxBounds[axis]) > kEpsilon) {
        continue;
      }

      const auto [firstAxis, secondAxis] = remainingAxes(axis);
      if (cut.minBounds[firstAxis] <= host.minBounds[firstAxis] + kEpsilon ||
          cut.maxBounds[firstAxis] >= host.maxBounds[firstAxis] - kEpsilon ||
          cut.minBounds[secondAxis] <= host.minBounds[secondAxis] + kEpsilon ||
          cut.maxBounds[secondAxis] >= host.maxBounds[secondAxis] - kEpsilon) {
        continue;
      }
      return axis;
    }
    return std::nullopt;
  }

  static glm::vec3 pointOnPlane(AxisIndex fixedAxis, float fixedValue,
                                AxisIndex axisA, float valueA, AxisIndex axisB,
                                float valueB) {
    glm::vec3 point(0.0f);
    point[fixedAxis] = fixedValue;
    point[axisA] = valueA;
    point[axisB] = valueB;
    return point;
  }

  static void appendRectOnPlane(std::vector<TriangleVertices> &triangles,
                                AxisIndex fixedAxis, float fixedValue,
                                AxisIndex axisA, float minA, float maxA,
                                AxisIndex axisB, float minB, float maxB,
                                const glm::vec3 &expectedNormal) {
    if (maxA - minA <= 1.0e-4f || maxB - minB <= 1.0e-4f) {
      return;
    }

    const glm::vec3 p00 =
        pointOnPlane(fixedAxis, fixedValue, axisA, minA, axisB, minB);
    const glm::vec3 p10 =
        pointOnPlane(fixedAxis, fixedValue, axisA, maxA, axisB, minB);
    const glm::vec3 p11 =
        pointOnPlane(fixedAxis, fixedValue, axisA, maxA, axisB, maxB);
    const glm::vec3 p01 =
        pointOnPlane(fixedAxis, fixedValue, axisA, minA, axisB, maxB);

    appendCapTriangle(triangles, {p00, p10, p11}, expectedNormal);
    appendCapTriangle(triangles, {p00, p11, p01}, expectedNormal);
  }

  static void appendThroughCapWithHole(std::vector<TriangleVertices> &triangles,
                                       const BoxSolid &host,
                                       const BoxSolid &cut,
                                       AxisIndex throughAxis,
                                       float throughValue,
                                       const glm::vec3 &expectedNormal) {
    const auto [axisA, axisB] = remainingAxes(throughAxis);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      host.minBounds[axisA], cut.minBounds[axisA], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      expectedNormal);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      cut.maxBounds[axisA], host.maxBounds[axisA], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      expectedNormal);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      cut.minBounds[axisA], cut.maxBounds[axisA], axisB,
                      host.minBounds[axisB], cut.minBounds[axisB],
                      expectedNormal);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      cut.minBounds[axisA], cut.maxBounds[axisA], axisB,
                      cut.maxBounds[axisB], host.maxBounds[axisB],
                      expectedNormal);
  }

  std::optional<MeshGroup> appendBoxWithSingleThroughVoid(const BoxSolid &host,
                                                          const BoxSolid &cut) {
    const auto throughAxis = throughAxisForCut(host, cut);
    if (!throughAxis.has_value()) {
      return std::nullopt;
    }

    const auto [axisA, axisB] = remainingAxes(*throughAxis);
    std::vector<TriangleVertices> triangles;
    triangles.reserve(32u);

    appendThroughCapWithHole(triangles, host, cut, *throughAxis,
                             host.minBounds[*throughAxis],
                             axisDirection(*throughAxis, -1.0f));
    appendThroughCapWithHole(triangles, host, cut, *throughAxis,
                             host.maxBounds[*throughAxis],
                             axisDirection(*throughAxis, 1.0f));

    appendRectOnPlane(triangles, axisA, host.minBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      axisDirection(axisA, -1.0f));
    appendRectOnPlane(triangles, axisA, host.maxBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      axisDirection(axisA, 1.0f));
    appendRectOnPlane(triangles, axisB, host.minBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA,
                      host.minBounds[axisA], host.maxBounds[axisA],
                      axisDirection(axisB, -1.0f));
    appendRectOnPlane(triangles, axisB, host.maxBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA,
                      host.minBounds[axisA], host.maxBounds[axisA],
                      axisDirection(axisB, 1.0f));

    appendRectOnPlane(triangles, axisA, cut.minBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB, cut.minBounds[axisB],
                      cut.maxBounds[axisB], axisDirection(axisA, 1.0f));
    appendRectOnPlane(triangles, axisA, cut.maxBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB, cut.minBounds[axisB],
                      cut.maxBounds[axisB], axisDirection(axisA, -1.0f));
    appendRectOnPlane(triangles, axisB, cut.minBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA, cut.minBounds[axisA],
                      cut.maxBounds[axisA], axisDirection(axisB, 1.0f));
    appendRectOnPlane(triangles, axisB, cut.maxBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA, cut.minBounds[axisA],
                      cut.maxBounds[axisA], axisDirection(axisB, -1.0f));

    return appendTriangleMesh(triangles, host.color);
  }

  void
  collectGeometryInstances(uint32_t ref, const glm::mat4 &transform,
                           std::vector<GeometryInstance> &instances) const {
    collectGeometryInstances(ref, transform, instances, {});
  }

  void collectGeometryInstances(uint32_t ref, const glm::mat4 &transform,
                                std::vector<GeometryInstance> &instances,
                                std::unordered_set<uint32_t> visiting) const {
    if (!visiting.insert(ref).second) {
      return;
    }
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return;
    }

    if (source->type == "IFCTRIANGULATEDFACESET" ||
        source->type == "IFCEXTRUDEDAREASOLID") {
      instances.push_back({ref, transform});
      return;
    }

    if (source->type == "IFCPRODUCTDEFINITIONSHAPE") {
      for (const auto repRef : refList(argAt(*source, 2))) {
        collectGeometryInstances(repRef, transform, instances, visiting);
      }
      return;
    }

    if (source->type == "IFCSHAPEREPRESENTATION") {
      for (const auto itemRef : refList(argAt(*source, 3))) {
        collectGeometryInstances(itemRef, transform, instances, visiting);
      }
      return;
    }

    if (source->type == "IFCMAPPEDITEM") {
      const auto sourceRef = firstRef(*source, 0);
      const auto targetRef = firstRef(*source, 1);
      if (!sourceRef.has_value()) {
        return;
      }
      const glm::mat4 target =
          targetRef.has_value() ? cartesianTransformationOperator3D(*targetRef)
                                : glm::mat4(1.0f);
      collectGeometryInstances(*sourceRef, transform * target, instances,
                               visiting);
      return;
    }

    if (source->type == "IFCREPRESENTATIONMAP") {
      const auto mappedRepRef = firstRef(*source, 1);
      if (!mappedRepRef.has_value()) {
        return;
      }
      glm::mat4 origin(1.0f);
      if (const auto originRef = firstRef(*source, 0); originRef.has_value()) {
        origin = axis2Placement3D(*originRef);
      }
      collectGeometryInstances(*mappedRepRef, transform * glm::inverse(origin),
                               instances, visiting);
      return;
    }

    if (source->type == "IFCSTYLEDITEM") {
      if (const auto itemRef = firstRef(*source, 0); itemRef.has_value()) {
        collectGeometryInstances(*itemRef, transform, instances, visiting);
      }
    }
  }

  bool appendVoidedProductElement(const Entity &product,
                                  const glm::mat4 &placement,
                                  std::span<const GeometryInstance> instances,
                                  const ProductMetadata &metadata,
                                  const glm::mat4 &unitTransform) {
    const auto openingIt = openingsByHostProduct_.find(product.id);
    if (openingIt == openingsByHostProduct_.end() ||
        openingIt->second.size() != 1u || instances.size() != 1u) {
      return false;
    }

    const GeometryInstance &hostInstance = instances.front();
    const auto hostBoxIt = boxSolidsByItem_.find(hostInstance.geometryId);
    if (hostBoxIt == boxSolidsByItem_.end()) {
      return false;
    }
    const BoxSolid &hostBox = hostBoxIt->second;

    const Entity *opening = entity(openingIt->second.front());
    if (opening == nullptr || opening->type != "IFCOPENINGELEMENT") {
      return false;
    }
    const auto openingPlacementRef = firstRef(*opening, 5);
    const auto openingRepresentationRef = firstRef(*opening, 6);
    if (!openingRepresentationRef.has_value()) {
      return false;
    }

    std::vector<GeometryInstance> openingInstances;
    collectGeometryInstances(*openingRepresentationRef, glm::mat4(1.0f),
                             openingInstances);
    if (openingInstances.size() != 1u) {
      return false;
    }
    const auto openingBoxIt =
        boxSolidsByItem_.find(openingInstances.front().geometryId);
    if (openingBoxIt == boxSolidsByItem_.end()) {
      return false;
    }

    const glm::mat4 openingPlacement =
        openingPlacementRef.has_value() ? localPlacement(*openingPlacementRef)
                                        : glm::mat4(1.0f);
    const glm::mat4 openingToHostGeometry =
        glm::inverse(hostInstance.transform) * glm::inverse(placement) *
        openingPlacement * openingInstances.front().transform;
    const BoxSolid openingBox =
        transformBox(openingBoxIt->second, openingToHostGeometry);
    const auto cutBox = intersectBoxes(hostBox, openingBox);
    if (!cutBox.has_value()) {
      return false;
    }

    auto group = appendBoxWithSingleThroughVoid(hostBox, *cutBox);
    if (!group.has_value()) {
      return false;
    }

    container::geometry::dotbim::Element element{};
    element.meshId = group->meshId;
    element.transform = unitTransform * placement * hostInstance.transform;
    element.color = group->color;
    element.type = product.type;
    applyProductMetadata(element, metadata);
    model_.elements.push_back(std::move(element));
    return true;
  }

  void appendProductElements() {
    const glm::mat4 unitTransform = importUnitTransform();
    for (const auto id : sortedEntityIds_) {
      const Entity *product = entity(id);
      if (product == nullptr || product->args.kind != StepValue::Kind::List ||
          product->args.list.size() < 7u) {
        continue;
      }

      const auto placementRef = firstRef(*product, 5);
      const auto representationRef = firstRef(*product, 6);
      if (!representationRef.has_value()) {
        continue;
      }
      const Entity *representation = entity(*representationRef);
      if (representation == nullptr ||
          representation->type != "IFCPRODUCTDEFINITIONSHAPE") {
        continue;
      }

      const glm::mat4 placement = placementRef.has_value()
                                      ? localPlacement(*placementRef)
                                      : glm::mat4(1.0f);
      std::vector<GeometryInstance> instances;
      collectGeometryInstances(*representationRef, glm::mat4(1.0f), instances);

      const ProductMetadata metadata = productMetadata(*product);
      if (product->type == "IFCOPENINGELEMENT") {
        continue;
      }
      if (appendVoidedProductElement(*product, placement, instances, metadata,
                                     unitTransform)) {
        continue;
      }
      for (const auto &instance : instances) {
        const auto groupIt = groupsByItem_.find(instance.geometryId);
        if (groupIt == groupsByItem_.end()) {
          continue;
        }
        for (const auto &group : groupIt->second) {
          container::geometry::dotbim::Element element{};
          element.meshId = group.meshId;
          element.transform = unitTransform * placement * instance.transform;
          element.color = group.color;
          element.type = product->type;
          applyProductMetadata(element, metadata);
          model_.elements.push_back(std::move(element));
        }
      }
    }
  }

  void appendFallbackElements() {
    if (!model_.elements.empty()) {
      return;
    }

    const glm::mat4 transform = importUnitTransform();
    for (const auto id : sortedEntityIds_) {
      const auto groupIt = groupsByItem_.find(id);
      if (groupIt == groupsByItem_.end()) {
        continue;
      }
      for (const auto &group : groupIt->second) {
        container::geometry::dotbim::Element element{};
        element.meshId = group.meshId;
        element.transform = transform;
        element.color = group.color;
        element.type = "IFCTRIANGULATEDFACESET";
        element.sourceId = "#" + std::to_string(id);
        model_.elements.push_back(std::move(element));
      }
    }
  }

  std::unordered_map<uint32_t, Entity> entities_;
  std::vector<uint32_t> sortedEntityIds_{};
  float importScale_{1.0f};
  LengthUnitMetadata unitMetadata_{};
  float unitScale_{1.0f};
  uint32_t nextMeshId_{1};
  Model model_{};
  mutable std::unordered_map<uint32_t, glm::vec3> pointCache_{};
  mutable std::unordered_map<uint32_t, glm::vec3> directionCache_{};
  mutable std::unordered_map<uint32_t, glm::mat4> axisPlacementCache_{};
  mutable std::unordered_map<uint32_t, glm::mat4> localPlacementCache_{};
  std::unordered_map<uint32_t, glm::vec4> styleColorByItem_{};
  std::unordered_map<uint32_t, std::vector<glm::vec4>> faceColorsByFaceSet_{};
  std::unordered_map<uint32_t, std::vector<MeshGroup>> groupsByItem_{};
  std::unordered_map<uint32_t, BoxSolid> boxSolidsByItem_{};
  std::unordered_map<uint32_t, std::vector<uint32_t>> openingsByHostProduct_{};
  std::unordered_map<uint32_t, uint32_t> hostByOpeningProduct_{};
  std::unordered_map<uint32_t, StoreyMetadata> storeyByProduct_{};
  std::unordered_map<uint32_t, SemanticMaterialMetadata> materialByProduct_{};
  std::unordered_map<uint32_t, ProductMetadata> semanticPropertiesByProduct_{};
};

} // namespace

Model LoadFromStep(std::string_view stepText, float importScale) {
  return IfcModelBuilder(parseEntities(stepText), importScale).build();
}

Model LoadFromFile(const std::filesystem::path &path, float importScale) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open IFC file: " +
                             container::util::pathToUtf8(path));
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return LoadFromStep(text, importScale);
}

} // namespace container::geometry::ifc
