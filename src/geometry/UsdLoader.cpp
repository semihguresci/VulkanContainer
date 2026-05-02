#include "Container/geometry/UsdLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#if defined(CONTAINER_HAS_TINYUSDZ)
#include <composition.hh>
#include <tinyusdz.hh>
#include <tydra/render-data-converter.hh>
#include <tydra/render-data.hh>
#endif

namespace container::geometry::usd {
namespace {

enum class TokenKind { Identifier, String, Number, Symbol, End };

struct Token {
  TokenKind kind{TokenKind::End};
  std::string text{};
  double number{0.0};
};

struct MeshData {
  std::vector<glm::vec3> points{};
  std::vector<uint32_t> faceVertexCounts{};
  std::vector<uint32_t> faceVertexIndices{};
  std::vector<glm::vec3> normals{};
  std::vector<glm::vec2> texcoords0{};
  std::vector<glm::vec2> texcoords1{};
  std::vector<glm::vec3> colors{};
};

struct Node {
  std::string name{};
  std::string path{};
  std::string typeName{};
  int parent{-1};
  std::vector<int> children{};
  MeshData mesh{};
  std::unordered_map<std::string, glm::mat4> xformOps{};
  std::vector<std::string> xformOpSequence{};
  std::vector<std::string> xformOpOrder{};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  bool hasColor{false};
  bool hasOpacity{false};
  bool doubleSided{true};
  bool hasDoubleSided{false};
  bool visible{true};
  bool hasVisibility{false};
};

bool isSymbol(char c) {
  switch (c) {
  case '{':
  case '}':
  case '[':
  case ']':
  case '(':
  case ')':
  case ',':
  case '=':
  case ';':
    return true;
  default:
    return false;
  }
}

bool isNumberStart(std::string_view text, size_t offset) {
  const char c = text[offset];
  if (std::isdigit(static_cast<unsigned char>(c))) {
    return true;
  }
  if ((c == '+' || c == '-' || c == '.') && offset + 1u < text.size()) {
    return std::isdigit(static_cast<unsigned char>(text[offset + 1u]));
  }
  return false;
}

std::vector<Token> tokenize(std::string_view text) {
  std::vector<Token> tokens;
  size_t cursor = 0;
  while (cursor < text.size()) {
    const char c = text[cursor];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++cursor;
      continue;
    }
    if (c == '#') {
      while (cursor < text.size() && text[cursor] != '\n') {
        ++cursor;
      }
      continue;
    }
    if (c == '"') {
      ++cursor;
      std::string value;
      while (cursor < text.size()) {
        const char current = text[cursor++];
        if (current == '"') {
          break;
        }
        if (current == '\\' && cursor < text.size()) {
          value.push_back(text[cursor++]);
        } else {
          value.push_back(current);
        }
      }
      tokens.push_back({TokenKind::String, std::move(value), 0.0});
      continue;
    }
    if (isSymbol(c)) {
      tokens.push_back({TokenKind::Symbol, std::string(1, c), 0.0});
      ++cursor;
      continue;
    }
    if (isNumberStart(text, cursor)) {
      const char *begin = text.data() + cursor;
      char *end = nullptr;
      const double value = std::strtod(begin, &end);
      if (end != begin) {
        tokens.push_back({TokenKind::Number,
                          std::string(begin, static_cast<size_t>(end - begin)),
                          value});
        cursor += static_cast<size_t>(end - begin);
        continue;
      }
    }

    const size_t start = cursor;
    while (cursor < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[cursor])) &&
           !isSymbol(text[cursor]) && text[cursor] != '"' &&
           text[cursor] != '#') {
      ++cursor;
    }
    tokens.push_back({TokenKind::Identifier,
                      std::string(text.substr(start, cursor - start)), 0.0});
  }
  tokens.push_back({TokenKind::End, {}, 0.0});
  return tokens;
}

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool isPrimKeyword(const Token &token) {
  return token.kind == TokenKind::Identifier &&
         (token.text == "def" || token.text == "over" || token.text == "class");
}

bool tokenIsSymbol(const Token &token, char symbol) {
  return token.kind == TokenKind::Symbol && token.text.size() == 1u &&
         token.text.front() == symbol;
}

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

std::string stageUpAxisFromText(std::string_view usdText) {
  const std::vector<Token> tokens = tokenize(usdText);
  int braceDepth = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token &token = tokens[i];
    if (token.kind == TokenKind::End) {
      break;
    }
    if (tokenIsSymbol(token, '{')) {
      ++braceDepth;
      continue;
    }
    if (tokenIsSymbol(token, '}')) {
      if (braceDepth > 0) {
        --braceDepth;
      }
      continue;
    }
    if (braceDepth != 0) {
      continue;
    }
    if (isPrimKeyword(token)) {
      break;
    }
    if (token.kind == TokenKind::Identifier && token.text == "upAxis" &&
        i + 2u < tokens.size() && tokenIsSymbol(tokens[i + 1u], '=')) {
      const Token &value = tokens[i + 2u];
      if (value.kind == TokenKind::String ||
          value.kind == TokenKind::Identifier) {
        return value.text;
      }
    }
  }
  return "Y";
}

glm::vec3 safeNormal(const glm::vec3 &a, const glm::vec3 &b,
                     const glm::vec3 &c) {
  const glm::vec3 normal = glm::cross(b - a, c - a);
  const float len2 = glm::dot(normal, normal);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return {0.0f, 1.0f, 0.0f};
  }
  return normal * (1.0f / std::sqrt(len2));
}

glm::vec3 safeTangent(const glm::vec3 &a, const glm::vec3 &b,
                      const glm::vec3 &normal) {
  glm::vec3 tangent = b - a;
  tangent -= normal * glm::dot(tangent, normal);
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

container::geometry::Vertex
makeVertex(const glm::vec3 &position, const glm::vec3 &normal,
           const glm::vec4 &tangent, const glm::vec3 &color,
           const glm::vec2 &texCoord, const glm::vec2 &texCoord1) {
  container::geometry::Vertex vertex{};
  vertex.position = position;
  vertex.color = color;
  vertex.texCoord = texCoord;
  vertex.texCoord1 = texCoord1;
  vertex.normal = normal;
  vertex.tangent = tangent;
  return vertex;
}

std::pair<glm::vec3, float>
computeBounds(const std::vector<glm::vec3> &points) {
  if (points.empty()) {
    return {glm::vec3(0.0f), 0.0f};
  }

  glm::vec3 minBounds = points.front();
  glm::vec3 maxBounds = points.front();
  for (const glm::vec3 &point : points) {
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  }

  const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
  float radius = 0.0f;
  for (const glm::vec3 &point : points) {
    radius = std::max(radius, glm::length(point - center));
  }
  return {center, radius};
}

std::vector<uint32_t> triangulateIndices(const MeshData &mesh,
                                         std::string_view path) {
  if (mesh.points.empty() || mesh.faceVertexIndices.empty()) {
    return {};
  }

  for (uint32_t index : mesh.faceVertexIndices) {
    if (index >= mesh.points.size()) {
      throw std::runtime_error("USD mesh '" + std::string(path) +
                               "' has a face vertex index outside points");
    }
  }

  if (mesh.faceVertexCounts.empty()) {
    if ((mesh.faceVertexIndices.size() % 3u) != 0u) {
      throw std::runtime_error("USD mesh '" + std::string(path) +
                               "' has non-triangle indices without counts");
    }
    return mesh.faceVertexIndices;
  }

  std::vector<uint32_t> triangles;
  size_t cursor = 0;
  for (uint32_t count : mesh.faceVertexCounts) {
    if (count < 3u ||
        cursor + static_cast<size_t>(count) > mesh.faceVertexIndices.size()) {
      throw std::runtime_error("USD mesh '" + std::string(path) +
                               "' has inconsistent face vertex counts");
    }
    const uint32_t first = mesh.faceVertexIndices[cursor];
    for (uint32_t i = 1; i + 1u < count; ++i) {
      triangles.push_back(first);
      triangles.push_back(mesh.faceVertexIndices[cursor + i]);
      triangles.push_back(mesh.faceVertexIndices[cursor + i + 1u]);
    }
    cursor += count;
  }

  if (cursor != mesh.faceVertexIndices.size()) {
    throw std::runtime_error("USD mesh '" + std::string(path) +
                             "' has unused face vertex indices");
  }
  return triangles;
}

class UsdParser {
public:
  explicit UsdParser(std::string_view text) : tokens_(tokenize(text)) {}

  std::vector<Node> parse() {
    parseBlock(-1);
    return std::move(nodes_);
  }

private:
  [[nodiscard]] const Token &peek(size_t offset = 0) const {
    const size_t index = std::min(position_ + offset, tokens_.size() - 1u);
    return tokens_[index];
  }

  [[nodiscard]] bool isEnd() const { return peek().kind == TokenKind::End; }

  [[nodiscard]] bool peekSymbol(char symbol) const {
    return peek().kind == TokenKind::Symbol && peek().text.size() == 1u &&
           peek().text.front() == symbol;
  }

  bool matchSymbol(char symbol) {
    if (!peekSymbol(symbol)) {
      return false;
    }
    ++position_;
    return true;
  }

  void skipBalanced(char opening, char closing) {
    if (!matchSymbol(opening)) {
      return;
    }

    int depth = 1;
    while (!isEnd() && depth > 0) {
      if (peekSymbol(opening)) {
        ++depth;
      } else if (peekSymbol(closing)) {
        --depth;
      }
      ++position_;
    }
  }

  std::vector<double> collectNumbersFromValue() {
    std::vector<double> values;
    collectValue([&](const Token &token) {
      if (token.kind == TokenKind::Number && std::isfinite(token.number)) {
        values.push_back(token.number);
      }
    });
    return values;
  }

  std::vector<std::string> collectStringsFromValue() {
    std::vector<std::string> values;
    collectValue([&](const Token &token) {
      if (token.kind == TokenKind::String ||
          token.kind == TokenKind::Identifier) {
        values.push_back(token.text);
      }
    });
    return values;
  }

  template <typename Visitor> void collectValue(Visitor &&visitor) {
    if (isEnd()) {
      return;
    }
    if (peekSymbol('(')) {
      collectBalanced('(', ')', visitor);
      return;
    }
    if (peekSymbol('[')) {
      collectBalanced('[', ']', visitor);
      return;
    }
    visitor(peek());
    ++position_;
  }

  template <typename Visitor>
  void collectBalanced(char opening, char closing, Visitor &&visitor) {
    if (!matchSymbol(opening)) {
      return;
    }
    int depth = 1;
    while (!isEnd() && depth > 0) {
      if (peekSymbol(opening)) {
        ++depth;
        ++position_;
        continue;
      }
      if (peekSymbol(closing)) {
        --depth;
        ++position_;
        continue;
      }
      visitor(peek());
      ++position_;
    }
  }

  void skipValue() {
    collectValue([](const Token &) {});
  }

  static std::optional<glm::vec3> firstVec3(const std::vector<double> &values) {
    if (values.size() < 3u) {
      return std::nullopt;
    }
    return glm::vec3(static_cast<float>(values[0]),
                     static_cast<float>(values[1]),
                     static_cast<float>(values[2]));
  }

  static std::vector<glm::vec3>
  vec3ArrayFromNumbers(const std::vector<double> &values,
                       std::string_view attributeName) {
    if ((values.size() % 3u) != 0u) {
      throw std::runtime_error("USD attribute '" + std::string(attributeName) +
                               "' must contain vec3 values");
    }
    std::vector<glm::vec3> result;
    result.reserve(values.size() / 3u);
    for (size_t i = 0; i < values.size(); i += 3u) {
      result.emplace_back(static_cast<float>(values[i]),
                          static_cast<float>(values[i + 1u]),
                          static_cast<float>(values[i + 2u]));
    }
    return result;
  }

  static std::vector<glm::vec2>
  vec2ArrayFromNumbers(const std::vector<double> &values,
                       std::string_view attributeName) {
    if ((values.size() % 2u) != 0u) {
      throw std::runtime_error("USD attribute '" + std::string(attributeName) +
                               "' must contain vec2 values");
    }
    std::vector<glm::vec2> result;
    result.reserve(values.size() / 2u);
    for (size_t i = 0; i < values.size(); i += 2u) {
      result.emplace_back(static_cast<float>(values[i]),
                          static_cast<float>(values[i + 1u]));
    }
    return result;
  }

  static std::vector<uint32_t>
  indexArrayFromNumbers(const std::vector<double> &values,
                        std::string_view attributeName) {
    std::vector<uint32_t> result;
    result.reserve(values.size());
    for (double value : values) {
      if (!std::isfinite(value) || value < 0.0 ||
          value > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
          std::floor(value) != value) {
        throw std::runtime_error("USD attribute '" +
                                 std::string(attributeName) +
                                 "' must contain non-negative integer values");
      }
      result.push_back(static_cast<uint32_t>(value));
    }
    return result;
  }

  static glm::mat4 matrixFromNumbers(const std::vector<double> &values,
                                     std::string_view attributeName) {
    if (values.size() < 16u) {
      throw std::runtime_error("USD attribute '" + std::string(attributeName) +
                               "' must contain a 4x4 matrix");
    }

    glm::mat4 result(1.0f);
    for (size_t column = 0; column < 4u; ++column) {
      for (size_t row = 0; row < 4u; ++row) {
        result[static_cast<glm::length_t>(column)]
              [static_cast<glm::length_t>(row)] =
                  static_cast<float>(values[column * 4u + row]);
      }
    }
    return result;
  }

  static glm::mat4 rotateDegrees(float degrees, const glm::vec3 &axis) {
    return glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis);
  }

  static glm::mat4 transformOpMatrix(std::string_view name,
                                     const std::vector<double> &values) {
    if (name == "xformOp:transform") {
      return matrixFromNumbers(values, name);
    }

    const glm::vec3 value = firstVec3(values).value_or(glm::vec3(0.0f));
    if (startsWith(name, "xformOp:translate")) {
      return glm::translate(glm::mat4(1.0f), value);
    }
    if (startsWith(name, "xformOp:scale")) {
      return glm::scale(glm::mat4(1.0f), value);
    }
    if (startsWith(name, "xformOp:rotateXYZ")) {
      return rotateDegrees(value.x, {1.0f, 0.0f, 0.0f}) *
             rotateDegrees(value.y, {0.0f, 1.0f, 0.0f}) *
             rotateDegrees(value.z, {0.0f, 0.0f, 1.0f});
    }

    const float scalar =
        values.empty() ? 0.0f : static_cast<float>(values.front());
    if (startsWith(name, "xformOp:rotateX")) {
      return rotateDegrees(scalar, {1.0f, 0.0f, 0.0f});
    }
    if (startsWith(name, "xformOp:rotateY")) {
      return rotateDegrees(scalar, {0.0f, 1.0f, 0.0f});
    }
    if (startsWith(name, "xformOp:rotateZ")) {
      return rotateDegrees(scalar, {0.0f, 0.0f, 1.0f});
    }

    return glm::mat4(1.0f);
  }

  std::optional<bool> collectBoolFromValue() {
    std::optional<bool> result;
    collectValue([&](const Token &token) {
      if (result)
        return;
      if (token.kind == TokenKind::Number && std::isfinite(token.number)) {
        result = token.number != 0.0;
      } else if (token.kind == TokenKind::Identifier ||
                 token.kind == TokenKind::String) {
        const std::string value = lowerAscii(token.text);
        if (value == "true" || value == "1") {
          result = true;
        } else if (value == "false" || value == "0") {
          result = false;
        }
      }
    });
    return result;
  }

  std::optional<std::string> declarationAttributeName(size_t start,
                                                      size_t equal) const {
    for (size_t i = equal; i > start;) {
      --i;
      if (tokens_[i].kind == TokenKind::Identifier) {
        return tokens_[i].text;
      }
    }
    return std::nullopt;
  }

  std::optional<size_t> findDeclarationEquals() const {
    size_t depth = 0;
    for (size_t i = position_; i < tokens_.size(); ++i) {
      const Token &token = tokens_[i];
      if (token.kind == TokenKind::End) {
        return std::nullopt;
      }
      if (depth == 0 && token.kind == TokenKind::Symbol && token.text == "}") {
        return std::nullopt;
      }
      if (depth == 0 && i != position_ && isPrimKeyword(token)) {
        return std::nullopt;
      }
      if (token.kind == TokenKind::Symbol) {
        if (token.text == "(" || token.text == "[") {
          ++depth;
        } else if ((token.text == ")" || token.text == "]") && depth > 0) {
          --depth;
        } else if (token.text == "=" && depth == 0) {
          return i;
        }
      }
    }
    return std::nullopt;
  }

  void parseBlock(int currentNode) {
    while (!isEnd()) {
      if (matchSymbol('}')) {
        return;
      }
      if (isPrimKeyword(peek())) {
        parsePrim(currentNode);
        continue;
      }
      parseAttribute(currentNode);
    }
  }

  void parsePrim(int parent) {
    ++position_;
    std::string typeName = "Prim";
    if (peek().kind == TokenKind::Identifier) {
      typeName = peek().text;
      ++position_;
    }

    std::string name = typeName + std::to_string(nodes_.size());
    if (peek().kind == TokenKind::String ||
        peek().kind == TokenKind::Identifier) {
      name = peek().text;
      ++position_;
    }

    Node node{};
    node.name = name;
    node.typeName = typeName;
    node.parent = parent;
    if (parent >= 0 && static_cast<size_t>(parent) < nodes_.size()) {
      node.path = nodes_[static_cast<size_t>(parent)].path + "/" + name;
    } else {
      node.path = "/" + name;
    }

    const int nodeIndex = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));
    if (parent >= 0 && static_cast<size_t>(parent) < nodes_.size()) {
      nodes_[static_cast<size_t>(parent)].children.push_back(nodeIndex);
    }

    while (!isEnd() && !peekSymbol('{')) {
      if (peekSymbol('(')) {
        skipBalanced('(', ')');
      } else {
        ++position_;
      }
    }
    if (matchSymbol('{')) {
      parseBlock(nodeIndex);
    }
  }

  void parseAttribute(int currentNode) {
    const size_t start = position_;
    const std::optional<size_t> equal = findDeclarationEquals();
    if (!equal) {
      if (!isEnd()) {
        ++position_;
      }
      return;
    }

    const std::optional<std::string> attributeName =
        declarationAttributeName(start, *equal);
    position_ = *equal + 1u;

    if (currentNode < 0 || !attributeName) {
      skipValue();
      return;
    }

    Node &node = nodes_[static_cast<size_t>(currentNode)];
    const std::string &name = *attributeName;

    if (name == "faceVertexCounts") {
      node.mesh.faceVertexCounts =
          indexArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "faceVertexIndices") {
      node.mesh.faceVertexIndices =
          indexArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "points") {
      node.mesh.points = vec3ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "normals" || name == "primvars:normals") {
      node.mesh.normals = vec3ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "primvars:st" || name == "st" || name == "texcoords") {
      node.mesh.texcoords0 =
          vec2ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "primvars:st1" || name == "st1" ||
               name == "primvars:texCoord1") {
      node.mesh.texcoords1 =
          vec2ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "xformOpOrder") {
      node.xformOpOrder = collectStringsFromValue();
    } else if (startsWith(name, "xformOp:")) {
      const std::vector<double> values = collectNumbersFromValue();
      node.xformOps[name] = transformOpMatrix(name, values);
      if (std::ranges::find(node.xformOpSequence, name) ==
          node.xformOpSequence.end()) {
        node.xformOpSequence.push_back(name);
      }
    } else if (name == "visibility") {
      const auto values = collectStringsFromValue();
      if (!values.empty()) {
        node.visible = values.front() != "invisible";
        node.hasVisibility = true;
      }
    } else if (name == "doubleSided") {
      if (const auto value = collectBoolFromValue()) {
        node.doubleSided = *value;
        node.hasDoubleSided = true;
      }
    } else if (name == "primvars:displayColor" || name == "displayColor") {
      const std::vector<glm::vec3> colors =
          vec3ArrayFromNumbers(collectNumbersFromValue(), name);
      if (colors.size() > 1u) {
        node.mesh.colors = colors;
        node.color.r = 1.0f;
        node.color.g = 1.0f;
        node.color.b = 1.0f;
        node.hasColor = true;
      } else if (!colors.empty()) {
        node.color.r = std::clamp(colors.front().r, 0.0f, 1.0f);
        node.color.g = std::clamp(colors.front().g, 0.0f, 1.0f);
        node.color.b = std::clamp(colors.front().b, 0.0f, 1.0f);
        node.hasColor = true;
      }
    } else if (name == "primvars:displayOpacity" || name == "displayOpacity") {
      const std::vector<double> values = collectNumbersFromValue();
      if (!values.empty() && std::isfinite(values.front())) {
        node.color.a =
            std::clamp(static_cast<float>(values.front()), 0.0f, 1.0f);
        node.hasOpacity = true;
      }
    } else {
      skipValue();
    }

    while (peekSymbol('(')) {
      skipBalanced('(', ')');
    }
  }

  std::vector<Token> tokens_{};
  size_t position_{0};
  std::vector<Node> nodes_{};
};

glm::mat4 nodeLocalTransform(const Node &node) {
  glm::mat4 transform(1.0f);
  const std::vector<std::string> &order =
      node.xformOpOrder.empty() ? node.xformOpSequence : node.xformOpOrder;
  for (const std::string &opName : order) {
    const auto opIt = node.xformOps.find(opName);
    if (opIt != node.xformOps.end()) {
      transform *= opIt->second;
    }
  }
  return transform;
}

glm::mat4 nodeWorldTransform(int nodeIndex, const std::vector<Node> &nodes,
                             std::vector<std::optional<glm::mat4>> &cache) {
  if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.size()) {
    return glm::mat4(1.0f);
  }
  if (cache[static_cast<size_t>(nodeIndex)]) {
    return *cache[static_cast<size_t>(nodeIndex)];
  }

  const Node &node = nodes[static_cast<size_t>(nodeIndex)];
  glm::mat4 transform = nodeLocalTransform(node);
  if (node.parent >= 0) {
    transform = nodeWorldTransform(node.parent, nodes, cache) * transform;
  }
  cache[static_cast<size_t>(nodeIndex)] = transform;
  return transform;
}

bool nodeVisible(int nodeIndex, const std::vector<Node> &nodes) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasVisibility && !node.visible) {
      return false;
    }
    cursor = node.parent;
  }
  return true;
}

glm::vec4 inheritedColor(int nodeIndex, const std::vector<Node> &nodes) {
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasColor) {
      color.r = node.color.r;
      color.g = node.color.g;
      color.b = node.color.b;
      break;
    }
    cursor = node.parent;
  }
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasOpacity) {
      color.a = node.color.a;
      break;
    }
    cursor = node.parent;
  }
  return color;
}

bool inheritedDoubleSided(int nodeIndex, const std::vector<Node> &nodes) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasDoubleSided) {
      return node.doubleSided;
    }
    cursor = node.parent;
  }
  return true;
}

glm::vec3 normalizeOrFallback(const glm::vec3 &value,
                              const glm::vec3 &fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

std::optional<glm::vec3> attributeVec3(const std::vector<glm::vec3> &values,
                                       uint32_t pointIndex,
                                       size_t faceVertexOrdinal) {
  if (values.empty()) {
    return std::nullopt;
  }
  if (values.size() == 1u) {
    return values.front();
  }
  if (pointIndex < values.size()) {
    return values[pointIndex];
  }
  if (faceVertexOrdinal < values.size()) {
    return values[faceVertexOrdinal];
  }
  return std::nullopt;
}

std::optional<glm::vec2> attributeVec2(const std::vector<glm::vec2> &values,
                                       uint32_t pointIndex,
                                       size_t faceVertexOrdinal) {
  if (values.empty()) {
    return std::nullopt;
  }
  if (values.size() == 1u) {
    return values.front();
  }
  if (pointIndex < values.size()) {
    return values[pointIndex];
  }
  if (faceVertexOrdinal < values.size()) {
    return values[faceVertexOrdinal];
  }
  return std::nullopt;
}

void appendMesh(dotbim::Model &model, const MeshData &mesh, uint32_t meshId,
                std::string_view path, const glm::vec4 &color) {
  (void)color;
  const std::vector<uint32_t> sourceIndices = triangulateIndices(mesh, path);
  if (mesh.points.empty() || sourceIndices.empty()) {
    return;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(mesh.points);
  range.boundsCenter = center;
  range.boundsRadius = radius;

  for (size_t i = 0; i + 2u < sourceIndices.size(); i += 3u) {
    const glm::vec3 &a = mesh.points[sourceIndices[i]];
    const glm::vec3 &b = mesh.points[sourceIndices[i + 1u]];
    const glm::vec3 &c = mesh.points[sourceIndices[i + 2u]];
    const glm::vec3 normal = safeNormal(a, b, c);
    const glm::vec3 tangent = safeTangent(a, b, normal);
    const glm::vec4 tangent4(tangent, 1.0f);
    const uint32_t base = static_cast<uint32_t>(model.vertices.size());
    for (size_t corner = 0; corner < 3u; ++corner) {
      const uint32_t sourceIndex = sourceIndices[i + corner];
      const glm::vec3 &position = mesh.points[sourceIndex];
      const glm::vec3 vertexNormal = normalizeOrFallback(
          attributeVec3(mesh.normals, sourceIndex, i + corner).value_or(normal),
          normal);
      const glm::vec3 vertexColor =
          glm::clamp(attributeVec3(mesh.colors, sourceIndex, i + corner)
                         .value_or(glm::vec3(1.0f)),
                     glm::vec3(0.0f), glm::vec3(1.0f));
      const glm::vec2 texCoord =
          attributeVec2(mesh.texcoords0, sourceIndex, i + corner)
              .value_or(glm::vec2(0.0f));
      const glm::vec2 texCoord1 =
          attributeVec2(mesh.texcoords1, sourceIndex, i + corner)
              .value_or(glm::vec2(0.0f));
      model.vertices.push_back(makeVertex(position, vertexNormal, tangent4,
                                          vertexColor, texCoord, texCoord1));
    }
    model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.meshRanges.push_back(range);
  }
}

#if defined(CONTAINER_HAS_TINYUSDZ)

std::string pathToTinyUsdUtf8(const std::filesystem::path &path) {
  const auto value = path.lexically_normal().generic_u8string();
  std::string result;
  result.reserve(value.size());
  for (const auto ch : value) {
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

float sanitizeColorComponent(float value, float fallback) {
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(value, 0.0f, 1.0f);
}

glm::vec4 colorFromTinyUsdMaterial(const tinyusdz::tydra::RenderScene &scene,
                                   const tinyusdz::tydra::RenderMesh &mesh,
                                   int materialOverrideId) {
  glm::vec4 color{sanitizeColorComponent(mesh.displayColor.r, 0.8f),
                  sanitizeColorComponent(mesh.displayColor.g, 0.82f),
                  sanitizeColorComponent(mesh.displayColor.b, 0.86f),
                  sanitizeColorComponent(mesh.displayOpacity, 1.0f)};

  const int materialId =
      materialOverrideId >= 0 ? materialOverrideId : mesh.material_id;
  if (materialId < 0 ||
      static_cast<size_t>(materialId) >= scene.materials.size()) {
    return color;
  }

  const tinyusdz::tydra::RenderMaterial &material =
      scene.materials[static_cast<size_t>(materialId)];
  if (material.surfaceShader.has_value()) {
    const tinyusdz::tydra::PreviewSurfaceShader &shader =
        *material.surfaceShader;
    color.r = sanitizeColorComponent(shader.diffuseColor.value[0], color.r);
    color.g = sanitizeColorComponent(shader.diffuseColor.value[1], color.g);
    color.b = sanitizeColorComponent(shader.diffuseColor.value[2], color.b);
    color.a = sanitizeColorComponent(shader.opacity.value, color.a);
    return color;
  }

  if (material.openPBRShader.has_value()) {
    const tinyusdz::tydra::OpenPBRSurfaceShader &shader =
        *material.openPBRShader;
    color.r = sanitizeColorComponent(shader.base_color.value[0], color.r);
    color.g = sanitizeColorComponent(shader.base_color.value[1], color.g);
    color.b = sanitizeColorComponent(shader.base_color.value[2], color.b);
    color.a = sanitizeColorComponent(shader.opacity.value, color.a);
  }
  return color;
}

glm::mat4 matrixFromTinyUsd(const tinyusdz::value::matrix4d &matrix) {
  glm::mat4 result(1.0f);
  for (size_t column = 0; column < 4u; ++column) {
    for (size_t row = 0; row < 4u; ++row) {
      const double value = matrix.m[column][row];
      if (!std::isfinite(value)) {
        throw std::runtime_error(
            "USD node transform contains a non-finite value");
      }
      result[static_cast<glm::length_t>(column)]
            [static_cast<glm::length_t>(row)] = static_cast<float>(value);
    }
  }
  return result;
}

glm::vec2 tinyVec2(const tinyusdz::tydra::vec2 &value) {
  return {value[0], value[1]};
}

glm::vec3 tinyVec3(const tinyusdz::tydra::vec3 &value) {
  return {value[0], value[1], value[2]};
}

glm::vec4 tinyVec4(const tinyusdz::tydra::vec4 &value) {
  return {value[0], value[1], value[2], value[3]};
}

float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

uint32_t
texCoordSlotFromTinyUsdTexture(const tinyusdz::tydra::UVTexture &texture) {
  const std::string varname = lowerAscii(texture.varname_uv);
  if (varname == "st1" || varname == "uv1" || varname == "texcoord1" ||
      varname == "texcoord_1") {
    return 1u;
  }
  return 0u;
}

container::material::TextureTransform
textureTransformFromTinyUsd(const tinyusdz::tydra::RenderScene &scene,
                            int32_t textureId) {
  container::material::TextureTransform transform{};
  if (textureId < 0 ||
      static_cast<size_t>(textureId) >= scene.textures.size()) {
    return transform;
  }

  const tinyusdz::tydra::UVTexture &texture =
      scene.textures[static_cast<size_t>(textureId)];
  transform.texCoord = texCoordSlotFromTinyUsdTexture(texture);
  if (texture.has_transform2d) {
    transform.offset = tinyVec2(texture.tx_translation);
    transform.scale = tinyVec2(texture.tx_scale);
    transform.rotation = glm::radians(texture.tx_rotation);
  }
  return transform;
}

uint32_t
samplerWrapModeFromTinyUsd(tinyusdz::tydra::UVTexture::WrapMode wrapMode) {
  using WrapMode = tinyusdz::tydra::UVTexture::WrapMode;
  switch (wrapMode) {
  case WrapMode::REPEAT:
    return container::gpu::kMaterialSamplerWrapRepeat;
  case WrapMode::MIRROR:
    return container::gpu::kMaterialSamplerWrapMirroredRepeat;
  case WrapMode::CLAMP_TO_EDGE:
  case WrapMode::CLAMP_TO_BORDER:
  default:
    return container::gpu::kMaterialSamplerWrapClampToEdge;
  }
}

uint32_t
samplerIndexFromTinyUsdTexture(const tinyusdz::tydra::UVTexture &texture) {
  const uint32_t wrapS = samplerWrapModeFromTinyUsd(texture.wrapS);
  const uint32_t wrapT = samplerWrapModeFromTinyUsd(texture.wrapT);
  return wrapS + wrapT * container::gpu::kMaterialSamplerWrapModeCount;
}

void copyTinyUsdTextureBuffer(const tinyusdz::tydra::RenderScene &scene,
                              const tinyusdz::tydra::TextureImage &image,
                              dotbim::MaterialTextureAsset &asset) {
  if (image.buffer_id < 0 ||
      static_cast<size_t>(image.buffer_id) >= scene.buffers.size()) {
    return;
  }

  const auto &buffer = scene.buffers[static_cast<size_t>(image.buffer_id)];
  if (buffer.data.empty()) {
    return;
  }

  asset.encodedBytes.clear();
  asset.encodedBytes.reserve(buffer.data.size());
  for (uint8_t byte : buffer.data) {
    asset.encodedBytes.push_back(static_cast<std::byte>(byte));
  }
}

std::optional<dotbim::MaterialTextureAsset>
textureAssetFromTinyUsd(const tinyusdz::tydra::RenderScene &scene,
                        int32_t textureId,
                        const std::filesystem::path &sourceDir) {
  if (textureId < 0 ||
      static_cast<size_t>(textureId) >= scene.textures.size()) {
    return std::nullopt;
  }

  const tinyusdz::tydra::UVTexture &texture =
      scene.textures[static_cast<size_t>(textureId)];
  if (texture.texture_image_id < 0 ||
      static_cast<size_t>(texture.texture_image_id) >= scene.images.size()) {
    return std::nullopt;
  }

  const tinyusdz::tydra::TextureImage &image =
      scene.images[static_cast<size_t>(texture.texture_image_id)];

  dotbim::MaterialTextureAsset asset{};
  asset.samplerIndex = samplerIndexFromTinyUsdTexture(texture);
  asset.name = !image.asset_identifier.empty() ? image.asset_identifier
                                               : texture.abs_path;

  if (!image.asset_identifier.empty()) {
    std::filesystem::path path =
        container::util::pathFromUtf8(image.asset_identifier);
    if (path.is_relative()) {
      path = sourceDir / path;
    }
    asset.path = path.lexically_normal();
  }

  if (!image.decoded) {
    copyTinyUsdTextureBuffer(scene, image, asset);
  }

  return asset.empty()
             ? std::nullopt
             : std::optional<dotbim::MaterialTextureAsset>(std::move(asset));
}

template <typename T>
void assignTinyUsdTexture(
    const tinyusdz::tydra::RenderScene &scene,
    const tinyusdz::tydra::ShaderParam<T> &parameter,
    const std::filesystem::path &sourceDir,
    dotbim::MaterialTextureAsset &textureAsset,
    container::material::TextureTransform &textureTransform) {
  if (!parameter.is_texture()) {
    return;
  }
  if (auto resolved =
          textureAssetFromTinyUsd(scene, parameter.texture_id, sourceDir)) {
    textureAsset = std::move(*resolved);
    textureTransform = textureTransformFromTinyUsd(scene, parameter.texture_id);
  }
}

container::material::AlphaMode
alphaModeFromTinyUsdMaterial(const tinyusdz::tydra::RenderMaterial &material) {
  if (material.materialTag == tinyusdz::tydra::MaterialTag::Masked) {
    return container::material::AlphaMode::Mask;
  }
  if (material.materialTag == tinyusdz::tydra::MaterialTag::Translucent) {
    return container::material::AlphaMode::Blend;
  }
  if (material.surfaceShader.has_value()) {
    const auto &shader = *material.surfaceShader;
    if (shader.opacityThreshold.value > 0.0f) {
      return container::material::AlphaMode::Mask;
    }
    if (shader.opacity.value < 0.999f || shader.opacity.is_texture()) {
      return container::material::AlphaMode::Blend;
    }
  }
  if (material.openPBRShader.has_value()) {
    const auto &shader = *material.openPBRShader;
    if (shader.opacity.value < 0.999f || shader.opacity.is_texture() ||
        shader.transmission_weight.value > 0.001f ||
        shader.transmission_weight.is_texture()) {
      return container::material::AlphaMode::Blend;
    }
  }
  return container::material::AlphaMode::Opaque;
}

dotbim::Material
convertTinyUsdMaterial(const tinyusdz::tydra::RenderScene &scene,
                       const tinyusdz::tydra::RenderMaterial &source,
                       const std::filesystem::path &sourceDir) {
  dotbim::Material material{};
  material.pbr.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
  material.pbr.roughnessFactor = 0.5f;
  material.pbr.alphaMode = alphaModeFromTinyUsdMaterial(source);

  if (source.surfaceShader.has_value()) {
    const auto &shader = *source.surfaceShader;
    material.pbr.baseColor =
        glm::vec4(glm::clamp(tinyVec3(shader.diffuseColor.value),
                             glm::vec3(0.0f), glm::vec3(1.0f)),
                  1.0f);
    material.pbr.emissiveColor =
        glm::clamp(tinyVec3(shader.emissiveColor.value), glm::vec3(0.0f),
                   glm::vec3(std::numeric_limits<float>::max()));
    material.pbr.metallicFactor =
        std::clamp(finiteOr(shader.metallic.value, 0.0f), 0.0f, 1.0f);
    material.pbr.roughnessFactor =
        std::clamp(finiteOr(shader.roughness.value, 0.5f), 0.0f, 1.0f);
    material.pbr.opacityFactor =
        std::clamp(finiteOr(shader.opacity.value, 1.0f), 0.0f, 1.0f);
    material.pbr.alphaCutoff =
        std::clamp(finiteOr(shader.opacityThreshold.value, 0.5f), 0.0f, 1.0f);
    material.pbr.ior = std::max(finiteOr(shader.ior.value, 1.5f), 1.0f);
    material.pbr.clearcoatFactor =
        std::clamp(finiteOr(shader.clearcoat.value, 0.0f), 0.0f, 1.0f);
    material.pbr.clearcoatRoughnessFactor = std::clamp(
        finiteOr(shader.clearcoatRoughness.value, 0.01f), 0.0f, 1.0f);
    material.pbr.specularGlossinessWorkflow = shader.useSpecularWorkflow;

    assignTinyUsdTexture(scene, shader.diffuseColor, sourceDir,
                         material.texturePaths.baseColor,
                         material.pbr.baseColorTextureTransform);
    assignTinyUsdTexture(scene, shader.emissiveColor, sourceDir,
                         material.texturePaths.emissive,
                         material.pbr.emissiveTextureTransform);
    assignTinyUsdTexture(scene, shader.normal, sourceDir,
                         material.texturePaths.normal,
                         material.pbr.normalTextureTransform);
    assignTinyUsdTexture(scene, shader.occlusion, sourceDir,
                         material.texturePaths.occlusion,
                         material.pbr.occlusionTextureTransform);
    assignTinyUsdTexture(scene, shader.metallic, sourceDir,
                         material.texturePaths.metalness,
                         material.pbr.metalnessTextureTransform);
    assignTinyUsdTexture(scene, shader.roughness, sourceDir,
                         material.texturePaths.roughness,
                         material.pbr.roughnessTextureTransform);
    assignTinyUsdTexture(scene, shader.opacity, sourceDir,
                         material.texturePaths.opacity,
                         material.pbr.opacityTextureTransform);
    assignTinyUsdTexture(scene, shader.clearcoat, sourceDir,
                         material.texturePaths.clearcoat,
                         material.pbr.clearcoatTextureTransform);
    assignTinyUsdTexture(scene, shader.clearcoatRoughness, sourceDir,
                         material.texturePaths.clearcoatRoughness,
                         material.pbr.clearcoatRoughnessTextureTransform);
    return material;
  }

  if (source.openPBRShader.has_value()) {
    const auto &shader = *source.openPBRShader;
    material.pbr.baseColor =
        glm::vec4(glm::clamp(tinyVec3(shader.base_color.value), glm::vec3(0.0f),
                             glm::vec3(1.0f)),
                  1.0f);
    material.pbr.metallicFactor =
        std::clamp(finiteOr(shader.base_metalness.value, 0.0f), 0.0f, 1.0f);
    material.pbr.roughnessFactor =
        std::clamp(finiteOr(shader.base_roughness.value, 0.5f), 0.0f, 1.0f);
    material.pbr.opacityFactor =
        std::clamp(finiteOr(shader.opacity.value, 1.0f), 0.0f, 1.0f);
    material.pbr.transmissionFactor = std::clamp(
        finiteOr(shader.transmission_weight.value, 0.0f), 0.0f, 1.0f);
    material.pbr.specularFactor =
        std::clamp(finiteOr(shader.specular_weight.value, 1.0f), 0.0f, 1.0f);
    material.pbr.specularColorFactor =
        glm::clamp(tinyVec3(shader.specular_color.value), glm::vec3(0.0f),
                   glm::vec3(1.0f));
    material.pbr.ior =
        std::max(finiteOr(shader.specular_ior.value, 1.5f), 1.0f);
    material.pbr.dispersion =
        std::max(finiteOr(shader.transmission_dispersion.value, 0.0f), 0.0f);
    material.pbr.clearcoatFactor =
        std::clamp(finiteOr(shader.coat_weight.value, 0.0f), 0.0f, 1.0f);
    material.pbr.clearcoatRoughnessFactor =
        std::clamp(finiteOr(shader.coat_roughness.value, 0.0f), 0.0f, 1.0f);
    material.pbr.emissiveColor =
        glm::clamp(tinyVec3(shader.emission_color.value), glm::vec3(0.0f),
                   glm::vec3(std::numeric_limits<float>::max()));
    material.pbr.emissiveStrength =
        std::max(finiteOr(shader.emission_luminance.value, 0.0f), 0.0f);
    material.pbr.sheenColorFactor = glm::clamp(
        tinyVec3(shader.sheen_color.value) *
            std::clamp(finiteOr(shader.sheen_weight.value, 0.0f), 0.0f, 1.0f),
        glm::vec3(0.0f), glm::vec3(1.0f));
    material.pbr.sheenRoughnessFactor =
        std::clamp(finiteOr(shader.sheen_roughness.value, 0.3f), 0.0f, 1.0f);
    material.pbr.iridescenceFactor =
        std::clamp(finiteOr(shader.thin_film_weight.value, 0.0f), 0.0f, 1.0f);
    material.pbr.iridescenceIor =
        std::max(finiteOr(shader.thin_film_ior.value, 1.3f), 1.0f);
    const float thinFilmThickness =
        std::max(finiteOr(shader.thin_film_thickness.value, 400.0f), 0.0f);
    material.pbr.iridescenceThicknessMinimum = thinFilmThickness;
    material.pbr.iridescenceThicknessMaximum = thinFilmThickness;

    assignTinyUsdTexture(scene, shader.base_color, sourceDir,
                         material.texturePaths.baseColor,
                         material.pbr.baseColorTextureTransform);
    assignTinyUsdTexture(scene, shader.normal, sourceDir,
                         material.texturePaths.normal,
                         material.pbr.normalTextureTransform);
    assignTinyUsdTexture(scene, shader.opacity, sourceDir,
                         material.texturePaths.opacity,
                         material.pbr.opacityTextureTransform);
    assignTinyUsdTexture(scene, shader.transmission_weight, sourceDir,
                         material.texturePaths.transmission,
                         material.pbr.transmissionTextureTransform);
    assignTinyUsdTexture(scene, shader.specular_weight, sourceDir,
                         material.texturePaths.specular,
                         material.pbr.specularTextureTransform);
    assignTinyUsdTexture(scene, shader.specular_color, sourceDir,
                         material.texturePaths.specularColor,
                         material.pbr.specularColorTextureTransform);
    assignTinyUsdTexture(scene, shader.coat_weight, sourceDir,
                         material.texturePaths.clearcoat,
                         material.pbr.clearcoatTextureTransform);
    assignTinyUsdTexture(scene, shader.coat_roughness, sourceDir,
                         material.texturePaths.clearcoatRoughness,
                         material.pbr.clearcoatRoughnessTextureTransform);
    assignTinyUsdTexture(scene, shader.coat_normal, sourceDir,
                         material.texturePaths.clearcoatNormal,
                         material.pbr.clearcoatNormalTextureTransform);
    assignTinyUsdTexture(scene, shader.emission_color, sourceDir,
                         material.texturePaths.emissive,
                         material.pbr.emissiveTextureTransform);
    assignTinyUsdTexture(scene, shader.sheen_color, sourceDir,
                         material.texturePaths.sheenColor,
                         material.pbr.sheenColorTextureTransform);
    assignTinyUsdTexture(scene, shader.sheen_roughness, sourceDir,
                         material.texturePaths.sheenRoughness,
                         material.pbr.sheenRoughnessTextureTransform);
    assignTinyUsdTexture(scene, shader.thin_film_weight, sourceDir,
                         material.texturePaths.iridescence,
                         material.pbr.iridescenceTextureTransform);
    assignTinyUsdTexture(scene, shader.thin_film_thickness, sourceDir,
                         material.texturePaths.iridescenceThickness,
                         material.pbr.iridescenceThicknessTextureTransform);
  }
  return material;
}

bool isValidTinyUsdMaterial(const tinyusdz::tydra::RenderScene &scene,
                            int materialId) {
  return materialId >= 0 &&
         static_cast<size_t>(materialId) < scene.materials.size();
}

uint32_t materialIndexFromTinyUsd(const tinyusdz::tydra::RenderScene &scene,
                                  int materialId) {
  return isValidTinyUsdMaterial(scene, materialId)
             ? static_cast<uint32_t>(materialId)
             : std::numeric_limits<uint32_t>::max();
}

struct TinyMeshTriangle {
  uint32_t pointIndex[3]{};
  size_t faceVertexOrdinal[3]{};
};

struct TinyMeshPart {
  uint32_t meshId{0};
  int materialId{-1};
};

size_t tinyFaceVaryingOrdinal(const tinyusdz::tydra::RenderMesh &mesh,
                              size_t triangulatedOrdinal) {
  if (triangulatedOrdinal < mesh.triangulatedToOrigFaceVertexIndexMap.size()) {
    return mesh.triangulatedToOrigFaceVertexIndexMap[triangulatedOrdinal];
  }
  return triangulatedOrdinal;
}

size_t
tinyAttributeComponentCount(const tinyusdz::tydra::VertexAttribute &attribute) {
  using Format = tinyusdz::tydra::VertexAttributeFormat;
  switch (attribute.format) {
  case Format::Float:
  case Format::Double:
    return std::max<size_t>(1u, attribute.elementSize);
  case Format::Vec2:
  case Format::Dvec2:
    return 2u;
  case Format::Vec3:
  case Format::Dvec3:
    return 3u;
  case Format::Vec4:
  case Format::Dvec4:
    return 4u;
  default:
    return 0u;
  }
}

std::optional<size_t>
tinyAttributeValueIndex(const tinyusdz::tydra::VertexAttribute &attribute,
                        uint32_t pointIndex, size_t faceVertexOrdinal) {
  if (attribute.empty()) {
    return std::nullopt;
  }

  size_t sourceIndex = pointIndex;
  if (attribute.is_constant()) {
    sourceIndex = 0u;
  } else if (attribute.is_facevarying()) {
    sourceIndex = faceVertexOrdinal;
  } else if (attribute.is_uniform()) {
    return std::nullopt;
  }

  if (!attribute.indices.empty()) {
    if (sourceIndex >= attribute.indices.size()) {
      return std::nullopt;
    }
    sourceIndex = attribute.indices[sourceIndex];
  }
  if (sourceIndex >= attribute.vertex_count()) {
    return std::nullopt;
  }
  return sourceIndex;
}

std::optional<float>
tinyAttributeComponent(const tinyusdz::tydra::VertexAttribute &attribute,
                       size_t valueIndex, size_t component) {
  const size_t componentCount = tinyAttributeComponentCount(attribute);
  if (component >= componentCount) {
    return std::nullopt;
  }

  const size_t stride = attribute.stride_bytes();
  const size_t offset = valueIndex * stride;
  if (offset >= attribute.data.size()) {
    return std::nullopt;
  }

  using Format = tinyusdz::tydra::VertexAttributeFormat;
  switch (attribute.format) {
  case Format::Float:
  case Format::Vec2:
  case Format::Vec3:
  case Format::Vec4: {
    const size_t componentOffset = offset + component * sizeof(float);
    if (componentOffset + sizeof(float) > attribute.data.size()) {
      return std::nullopt;
    }
    float value = 0.0f;
    std::memcpy(&value, attribute.data.data() + componentOffset, sizeof(float));
    return std::isfinite(value) ? std::optional<float>(value) : std::nullopt;
  }
  case Format::Double:
  case Format::Dvec2:
  case Format::Dvec3:
  case Format::Dvec4: {
    const size_t componentOffset = offset + component * sizeof(double);
    if (componentOffset + sizeof(double) > attribute.data.size()) {
      return std::nullopt;
    }
    double value = 0.0;
    std::memcpy(&value, attribute.data.data() + componentOffset,
                sizeof(double));
    return std::isfinite(value)
               ? std::optional<float>(static_cast<float>(value))
               : std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

std::optional<glm::vec2>
tinyAttributeVec2(const tinyusdz::tydra::VertexAttribute &attribute,
                  uint32_t pointIndex, size_t faceVertexOrdinal) {
  const auto valueIndex =
      tinyAttributeValueIndex(attribute, pointIndex, faceVertexOrdinal);
  if (!valueIndex) {
    return std::nullopt;
  }
  const auto x = tinyAttributeComponent(attribute, *valueIndex, 0u);
  const auto y = tinyAttributeComponent(attribute, *valueIndex, 1u);
  if (!x || !y) {
    return std::nullopt;
  }
  return glm::vec2(*x, *y);
}

std::optional<glm::vec3>
tinyAttributeVec3(const tinyusdz::tydra::VertexAttribute &attribute,
                  uint32_t pointIndex, size_t faceVertexOrdinal) {
  const auto valueIndex =
      tinyAttributeValueIndex(attribute, pointIndex, faceVertexOrdinal);
  if (!valueIndex) {
    return std::nullopt;
  }
  const auto x = tinyAttributeComponent(attribute, *valueIndex, 0u);
  const auto y = tinyAttributeComponent(attribute, *valueIndex, 1u);
  const auto z = tinyAttributeComponent(attribute, *valueIndex, 2u);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

std::optional<glm::vec4>
tinyAttributeVec4(const tinyusdz::tydra::VertexAttribute &attribute,
                  uint32_t pointIndex, size_t faceVertexOrdinal) {
  const auto valueIndex =
      tinyAttributeValueIndex(attribute, pointIndex, faceVertexOrdinal);
  if (!valueIndex) {
    return std::nullopt;
  }
  const auto x = tinyAttributeComponent(attribute, *valueIndex, 0u);
  const auto y = tinyAttributeComponent(attribute, *valueIndex, 1u);
  const auto z = tinyAttributeComponent(attribute, *valueIndex, 2u);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  const auto w = tinyAttributeComponent(attribute, *valueIndex, 3u);
  return glm::vec4(*x, *y, *z, w.value_or(1.0f));
}

const tinyusdz::tydra::VertexAttribute *
tinyTexcoordAttribute(const tinyusdz::tydra::RenderMesh &mesh, uint32_t slot) {
  const auto it = mesh.texcoords.find(slot);
  return it == mesh.texcoords.end() ? nullptr : &it->second;
}

glm::vec3 tinyVertexColor(const tinyusdz::tydra::RenderMesh &mesh,
                          uint32_t pointIndex, size_t faceVertexOrdinal) {
  if (mesh.vertex_colors.vertex_count() <= 1u) {
    return glm::vec3(1.0f);
  }
  return glm::clamp(
      tinyAttributeVec3(mesh.vertex_colors, pointIndex, faceVertexOrdinal)
          .value_or(glm::vec3(1.0f)),
      glm::vec3(0.0f), glm::vec3(1.0f));
}

glm::vec4 tinyVertexTangent(const tinyusdz::tydra::RenderMesh &mesh,
                            uint32_t pointIndex, size_t faceVertexOrdinal,
                            const glm::vec3 &fallbackTangent,
                            const glm::vec3 &normal) {
  if (const auto tangent =
          tinyAttributeVec4(mesh.tangents, pointIndex, faceVertexOrdinal)) {
    glm::vec3 tangentVector =
        normalizeOrFallback(glm::vec3(*tangent), fallbackTangent);
    return glm::vec4(tangentVector, tangent->w < 0.0f ? -1.0f : 1.0f);
  }
  if (const auto tangent =
          tinyAttributeVec3(mesh.tangents, pointIndex, faceVertexOrdinal)) {
    glm::vec3 tangentVector = normalizeOrFallback(*tangent, fallbackTangent);
    float sign = 1.0f;
    if (const auto binormal =
            tinyAttributeVec3(mesh.binormals, pointIndex, faceVertexOrdinal)) {
      sign = glm::dot(glm::cross(normal, tangentVector), *binormal) < 0.0f
                 ? -1.0f
                 : 1.0f;
    }
    return glm::vec4(tangentVector, sign);
  }
  return glm::vec4(fallbackTangent, 1.0f);
}

std::vector<TinyMeshTriangle>
tinyUsdTrianglesForMaterial(const tinyusdz::tydra::RenderMesh &mesh,
                            int materialId) {
  const std::vector<uint32_t> faceVertexIndices = mesh.faceVertexIndices();
  const std::vector<uint32_t> faceVertexCounts = mesh.faceVertexCounts();
  if (faceVertexIndices.empty()) {
    return {};
  }

  std::vector<int> materialByFace;
  size_t faceCount = faceVertexCounts.empty() ? faceVertexIndices.size() / 3u
                                              : faceVertexCounts.size();
  materialByFace.assign(faceCount, mesh.material_id);
  for (const auto &[name, subset] : mesh.material_subsetMap) {
    (void)name;
    const int subsetMaterial =
        subset.material_id >= 0 ? subset.material_id : mesh.material_id;
    for (int faceIndex : subset.indices()) {
      if (faceIndex >= 0 && static_cast<size_t>(faceIndex) < faceCount) {
        materialByFace[static_cast<size_t>(faceIndex)] = subsetMaterial;
      }
    }
  }

  std::vector<TinyMeshTriangle> triangles;
  size_t cursor = 0;
  for (size_t face = 0; face < faceCount; ++face) {
    const uint32_t count =
        faceVertexCounts.empty() ? 3u : faceVertexCounts[face];
    if (count < 3u || cursor + count > faceVertexIndices.size()) {
      throw std::runtime_error("USD mesh '" + mesh.abs_path +
                               "' has inconsistent face topology");
    }
    if (materialByFace[face] == materialId) {
      for (uint32_t i = 1; i + 1u < count; ++i) {
        TinyMeshTriangle triangle{};
        triangle.pointIndex[0] = faceVertexIndices[cursor];
        triangle.pointIndex[1] = faceVertexIndices[cursor + i];
        triangle.pointIndex[2] = faceVertexIndices[cursor + i + 1u];
        triangle.faceVertexOrdinal[0] = tinyFaceVaryingOrdinal(mesh, cursor);
        triangle.faceVertexOrdinal[1] =
            tinyFaceVaryingOrdinal(mesh, cursor + i);
        triangle.faceVertexOrdinal[2] =
            tinyFaceVaryingOrdinal(mesh, cursor + i + 1u);
        triangles.push_back(triangle);
      }
    }
    cursor += count;
  }
  return triangles;
}

std::vector<int>
tinyUsdMeshMaterialIds(const tinyusdz::tydra::RenderMesh &mesh) {
  std::vector<int> materialIds;
  auto appendUnique = [&](int materialId) {
    if (std::ranges::find(materialIds, materialId) == materialIds.end()) {
      materialIds.push_back(materialId);
    }
  };

  appendUnique(mesh.material_id);
  for (const auto &[name, subset] : mesh.material_subsetMap) {
    (void)name;
    appendUnique(subset.material_id >= 0 ? subset.material_id
                                         : mesh.material_id);
  }
  return materialIds;
}

bool appendTinyUsdTrianglePart(dotbim::Model &model,
                               const tinyusdz::tydra::RenderMesh &mesh,
                               const std::vector<TinyMeshTriangle> &triangles,
                               uint32_t meshId) {
  if (mesh.points.size() >
      static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::runtime_error("USD mesh '" + mesh.abs_path +
                             "' has too many points for 32-bit indices");
  }
  if (triangles.empty()) {
    return false;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());

  std::vector<glm::vec3> rangePoints;
  rangePoints.reserve(triangles.size() * 3u);

  const tinyusdz::tydra::VertexAttribute *texcoord0 =
      tinyTexcoordAttribute(mesh, 0u);
  const tinyusdz::tydra::VertexAttribute *texcoord1 =
      tinyTexcoordAttribute(mesh, 1u);

  for (const TinyMeshTriangle &triangle : triangles) {
    glm::vec3 positions[3]{};
    for (size_t corner = 0; corner < 3u; ++corner) {
      const uint32_t pointIndex = triangle.pointIndex[corner];
      if (pointIndex >= mesh.points.size()) {
        throw std::runtime_error("USD mesh '" + mesh.abs_path +
                                 "' has a face vertex index outside points");
      }
      positions[corner] = tinyVec3(mesh.points[pointIndex]);
    }

    const glm::vec3 faceNormal =
        safeNormal(positions[0], positions[1], positions[2]);
    const glm::vec3 faceTangent =
        safeTangent(positions[0], positions[1], faceNormal);
    const uint32_t base = static_cast<uint32_t>(model.vertices.size());

    for (size_t corner = 0; corner < 3u; ++corner) {
      const uint32_t pointIndex = triangle.pointIndex[corner];
      const size_t faceVertexOrdinal = triangle.faceVertexOrdinal[corner];
      const glm::vec3 normal = normalizeOrFallback(
          tinyAttributeVec3(mesh.normals, pointIndex, faceVertexOrdinal)
              .value_or(faceNormal),
          faceNormal);
      const glm::vec4 tangent = tinyVertexTangent(
          mesh, pointIndex, faceVertexOrdinal, faceTangent, normal);
      const glm::vec2 uv0 =
          texcoord0 == nullptr
              ? glm::vec2(0.0f)
              : tinyAttributeVec2(*texcoord0, pointIndex, faceVertexOrdinal)
                    .value_or(glm::vec2(0.0f));
      const glm::vec2 uv1 =
          texcoord1 == nullptr
              ? glm::vec2(0.0f)
              : tinyAttributeVec2(*texcoord1, pointIndex, faceVertexOrdinal)
                    .value_or(glm::vec2(0.0f));
      const glm::vec3 color =
          tinyVertexColor(mesh, pointIndex, faceVertexOrdinal);
      model.vertices.push_back(
          makeVertex(positions[corner], normal, tangent, color, uv0, uv1));
      rangePoints.push_back(positions[corner]);
    }
    model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  const auto [center, radius] = computeBounds(rangePoints);
  range.boundsCenter = center;
  range.boundsRadius = radius;
  if (range.indexCount == 0u) {
    return false;
  }
  model.meshRanges.push_back(range);
  return true;
}

const std::vector<TinyMeshPart> *appendTinyUsdMeshIfNeeded(
    dotbim::Model &model, const tinyusdz::tydra::RenderScene &scene,
    int32_t meshIndex,
    std::unordered_map<int32_t, std::vector<TinyMeshPart>> &loadedMeshes) {
  if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= scene.meshes.size()) {
    return nullptr;
  }

  if (const auto existing = loadedMeshes.find(meshIndex);
      existing != loadedMeshes.end()) {
    return &existing->second;
  }

  const tinyusdz::tydra::RenderMesh &mesh =
      scene.meshes[static_cast<size_t>(meshIndex)];
  std::vector<TinyMeshPart> parts;
  for (int materialId : tinyUsdMeshMaterialIds(mesh)) {
    const std::vector<TinyMeshTriangle> triangles =
        tinyUsdTrianglesForMaterial(mesh, materialId);
    if (triangles.empty()) {
      continue;
    }
    const uint32_t meshId = static_cast<uint32_t>(model.meshRanges.size());
    if (appendTinyUsdTrianglePart(model, mesh, triangles, meshId)) {
      parts.push_back(TinyMeshPart{meshId, materialId});
    }
  }
  if (parts.empty()) {
    return nullptr;
  }
  auto [it, inserted] = loadedMeshes.emplace(meshIndex, std::move(parts));
  (void)inserted;
  return &it->second;
}

bool hasTinyUsdCompositionArcs(const tinyusdz::Layer &layer) {
  return layer.check_unresolved_inherits() ||
         layer.check_unresolved_variant() ||
         layer.check_unresolved_references() ||
         layer.check_unresolved_payload() ||
         layer.check_unresolved_specializes();
}

bool loadComposedTinyUsdStage(const std::filesystem::path &path,
                              tinyusdz::Stage &stage, std::string &warn,
                              std::string &err,
                              const tinyusdz::USDLoadOptions &options) {
  tinyusdz::Layer layer;
  const std::string filename = pathToTinyUsdUtf8(path);
  if (!tinyusdz::LoadLayerFromFile(filename, &layer, &warn, &err, options)) {
    return false;
  }

  tinyusdz::AssetResolutionResolver resolver;
  const std::string baseDir = pathToTinyUsdUtf8(path.parent_path());
  resolver.set_current_working_path(baseDir);
  resolver.set_search_paths({baseDir});

  tinyusdz::Layer working = std::move(layer);
  if (!working.metas().subLayers.empty()) {
    tinyusdz::Layer composited;
    if (!tinyusdz::CompositeSublayers(resolver, working, &composited, &warn,
                                      &err)) {
      return false;
    }
    working = std::move(composited);
  }

  constexpr int kMaxCompositionPasses = 16;
  for (int pass = 0; pass < kMaxCompositionPasses; ++pass) {
    if (!hasTinyUsdCompositionArcs(working)) {
      break;
    }

    tinyusdz::Layer composited;
    if (!tinyusdz::CompositeAllArcs(resolver, working, &composited, &warn,
                                    &err)) {
      return false;
    }
    working = std::move(composited);
  }

  if (hasTinyUsdCompositionArcs(working)) {
    err += "USD composition did not converge after " +
           std::to_string(kMaxCompositionPasses) + " passes.\n";
    return false;
  }

  return tinyusdz::LayerToStage(std::move(working), &stage, &warn, &err);
}

bool hasRenderableGeometry(const dotbim::Model &model) {
  return !model.vertices.empty() && !model.indices.empty() &&
         !model.meshRanges.empty() && !model.elements.empty();
}

void appendTinyUsdNode(
    dotbim::Model &model, const tinyusdz::tydra::RenderScene &scene,
    const tinyusdz::tydra::Node &node, const glm::mat4 &importTransform,
    std::unordered_map<int32_t, std::vector<TinyMeshPart>> &loadedMeshes,
    const std::unordered_set<std::string> &handledInstancePaths,
    bool insideHandledInstance) {
  const bool handledInstance =
      insideHandledInstance || handledInstancePaths.contains(node.abs_path);
  if (!handledInstance &&
      node.category == tinyusdz::tydra::NodeCategory::Geom && node.id >= 0) {
    const auto *parts =
        appendTinyUsdMeshIfNeeded(model, scene, node.id, loadedMeshes);
    if (parts != nullptr) {
      const tinyusdz::tydra::RenderMesh &mesh =
          scene.meshes[static_cast<size_t>(node.id)];
      for (const TinyMeshPart &part : *parts) {
        const glm::vec4 color =
            colorFromTinyUsdMaterial(scene, mesh, part.materialId);
        dotbim::Element element{};
        element.meshId = part.meshId;
        element.transform =
            importTransform * matrixFromTinyUsd(node.global_matrix);
        element.color = color;
        element.materialIndex =
            materialIndexFromTinyUsd(scene, part.materialId);
        element.doubleSided =
            static_cast<size_t>(node.id) < scene.meshes.size()
                ? scene.meshes[static_cast<size_t>(node.id)].doubleSided
                : true;
        element.guid = node.abs_path.empty()
                           ? "usd-node-" + std::to_string(node.id)
                           : node.abs_path + "#" + std::to_string(part.meshId);
        element.type = "UsdGeomMesh";
        model.elements.push_back(std::move(element));
      }
    }
  }

  for (const tinyusdz::tydra::Node &child : node.children) {
    appendTinyUsdNode(model, scene, child, importTransform, loadedMeshes,
                      handledInstancePaths, handledInstance);
  }
}

void appendTinyUsdInstances(
    dotbim::Model &model, const tinyusdz::tydra::RenderScene &scene,
    const glm::mat4 &importTransform,
    std::unordered_map<int32_t, std::vector<TinyMeshPart>> &loadedMeshes) {
  for (const tinyusdz::tydra::RenderInstance &instance : scene.instances) {
    if (!instance.visible || instance.mesh_id < 0) {
      continue;
    }

    const auto *parts =
        appendTinyUsdMeshIfNeeded(model, scene, instance.mesh_id, loadedMeshes);
    if (parts == nullptr) {
      continue;
    }

    const tinyusdz::tydra::RenderMesh &mesh =
        scene.meshes[static_cast<size_t>(instance.mesh_id)];
    for (const TinyMeshPart &part : *parts) {
      const int materialId =
          instance.material_id >= 0 ? instance.material_id : part.materialId;
      dotbim::Element element{};
      element.meshId = part.meshId;
      element.transform =
          importTransform * matrixFromTinyUsd(instance.global_matrix);
      element.color = colorFromTinyUsdMaterial(scene, mesh, materialId);
      element.materialIndex = materialIndexFromTinyUsd(scene, materialId);
      element.doubleSided =
          static_cast<size_t>(instance.mesh_id) < scene.meshes.size()
              ? scene.meshes[static_cast<size_t>(instance.mesh_id)].doubleSided
              : true;
      element.guid =
          instance.abs_path.empty()
              ? "usd-instance-" + std::to_string(instance.mesh_id)
              : instance.abs_path + "#" + std::to_string(part.meshId);
      element.type = "UsdGeomMeshInstance";
      model.elements.push_back(std::move(element));
    }
  }
}

dotbim::Model modelFromTinyUsdStage(const tinyusdz::Stage &stage,
                                    float importScale,
                                    const std::filesystem::path &sourcePath) {
  tinyusdz::tydra::RenderScene scene;
  tinyusdz::tydra::RenderSceneConverter converter;
  tinyusdz::tydra::RenderSceneConverterEnv env(stage);
  const std::string filename = pathToTinyUsdUtf8(sourcePath);
  const std::string sourceDir = pathToTinyUsdUtf8(sourcePath.parent_path());
  env.usd_filename = filename;
  env.scene_config.load_texture_assets = false;
  env.set_search_paths({sourceDir});
  if (!converter.ConvertToRenderScene(env, &scene)) {
    const std::string converterError = converter.GetError();
    throw std::runtime_error(
        "TinyUSDZ failed to convert USD scene '" + filename +
        "': " + (converterError.empty() ? "unknown error" : converterError));
  }

  dotbim::Model model{};
  model.materials.reserve(scene.materials.size());
  for (const tinyusdz::tydra::RenderMaterial &material : scene.materials) {
    model.materials.push_back(
        convertTinyUsdMaterial(scene, material, sourcePath.parent_path()));
  }

  std::unordered_map<int32_t, std::vector<TinyMeshPart>> loadedMeshes;
  loadedMeshes.reserve(scene.meshes.size());
  std::unordered_set<std::string> handledInstancePaths;
  handledInstancePaths.reserve(scene.instances.size());
  for (const tinyusdz::tydra::RenderInstance &instance : scene.instances) {
    if (instance.visible && instance.mesh_id >= 0) {
      handledInstancePaths.insert(instance.abs_path);
    }
  }

  tinyusdz::Axis upAxis{tinyusdz::Axis::Y};
  if (stage.metas().upAxis.authored()) {
    upAxis = stage.metas().upAxis.get_value();
  }
  const glm::mat4 importTransform =
      (upAxis == tinyusdz::Axis::Z
           ? container::geometry::zUpForwardYToRendererAxes()
           : glm::mat4(1.0f)) *
      glm::scale(glm::mat4(1.0f), glm::vec3(sanitizeImportScale(importScale)));
  for (const tinyusdz::tydra::Node &node : scene.nodes) {
    appendTinyUsdNode(model, scene, node, importTransform, loadedMeshes,
                      handledInstancePaths, false);
  }
  appendTinyUsdInstances(model, scene, importTransform, loadedMeshes);
  return model;
}

dotbim::Model LoadWithTinyUsd(const std::filesystem::path &path,
                              float importScale) {
  const std::string filename = pathToTinyUsdUtf8(path);
  tinyusdz::USDLoadOptions options;
  options.load_assets = false;
  options.do_composition = true;
  options.load_sublayers = true;
  options.load_references = true;
  options.load_payloads = true;

  std::optional<dotbim::Model> directModel;
  tinyusdz::Stage directStage;
  std::string directWarn;
  std::string directErr;
  if (tinyusdz::LoadUSDFromFile(filename, &directStage, &directWarn, &directErr,
                                options)) {
    directModel = modelFromTinyUsdStage(directStage, importScale, path);
    if (hasRenderableGeometry(*directModel) &&
        !directModel->materials.empty()) {
      return std::move(*directModel);
    }
  }

  tinyusdz::Stage composedStage;
  std::string composedWarn;
  std::string composedErr;
  if (!loadComposedTinyUsdStage(path, composedStage, composedWarn, composedErr,
                                options)) {
    if (directModel && hasRenderableGeometry(*directModel)) {
      return std::move(*directModel);
    }
    std::string error = composedErr.empty() ? directErr : composedErr;
    throw std::runtime_error("TinyUSDZ failed to load USD file '" + filename +
                             "': " + (error.empty() ? "unknown error" : error));
  }
  dotbim::Model composedModel =
      modelFromTinyUsdStage(composedStage, importScale, path);
  if (hasRenderableGeometry(composedModel) &&
      (!composedModel.materials.empty() || !directModel ||
       !hasRenderableGeometry(*directModel))) {
    return composedModel;
  }
  if (directModel && hasRenderableGeometry(*directModel)) {
    return std::move(*directModel);
  }
  return composedModel;
}

#endif

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open USD file: " +
                             container::util::pathToUtf8(path));
  }
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

bool fileStartsWithUsdc(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  char magic[8]{};
  file.read(magic, sizeof(magic));
  return file.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
         std::string_view(magic, sizeof(magic)) == "PXR-USDC";
}

std::string readTextFile(const std::filesystem::path &path) {
  const std::vector<uint8_t> bytes = readBinaryFile(path);
  if (bytes.size() >= 8u &&
      std::string_view(reinterpret_cast<const char *>(bytes.data()), 8u) ==
          "PXR-USDC") {
    throw std::runtime_error(
        "binary USDC payloads are not supported by the lightweight USD loader");
  }
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

uint16_t readLe16(const std::vector<uint8_t> &bytes, size_t offset) {
  if (offset + 2u > bytes.size()) {
    throw std::runtime_error("truncated USDZ archive");
  }
  return static_cast<uint16_t>(bytes[offset]) |
         (static_cast<uint16_t>(bytes[offset + 1u]) << 8u);
}

uint32_t readLe32(const std::vector<uint8_t> &bytes, size_t offset) {
  if (offset + 4u > bytes.size()) {
    throw std::runtime_error("truncated USDZ archive");
  }
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
         (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
         (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool hasUsdTextExtension(std::string_view name) {
  const std::filesystem::path path{std::string(name)};
  const std::string extension = lowerAscii(path.extension().string());
  return extension == ".usd" || extension == ".usda";
}

std::string extractFirstUsdTextFromUsdz(const std::filesystem::path &path) {
  const std::vector<uint8_t> bytes = readBinaryFile(path);
  if (bytes.size() < 22u) {
    throw std::runtime_error("USDZ archive is too small");
  }

  const size_t minOffset = bytes.size() > 66000u ? bytes.size() - 66000u : 0u;
  std::optional<size_t> eocdOffset;
  for (size_t offset = bytes.size() - 22u;; --offset) {
    if (readLe32(bytes, offset) == 0x06054b50u) {
      eocdOffset = offset;
      break;
    }
    if (offset == minOffset) {
      break;
    }
  }
  if (!eocdOffset) {
    throw std::runtime_error("USDZ archive is missing a ZIP directory");
  }

  const uint16_t entryCount = readLe16(bytes, *eocdOffset + 10u);
  const uint32_t centralDirectoryOffset = readLe32(bytes, *eocdOffset + 16u);
  size_t cursor = centralDirectoryOffset;
  for (uint16_t entry = 0; entry < entryCount; ++entry) {
    if (readLe32(bytes, cursor) != 0x02014b50u) {
      throw std::runtime_error("USDZ archive has an invalid ZIP directory");
    }

    const uint16_t method = readLe16(bytes, cursor + 10u);
    const uint32_t compressedSize = readLe32(bytes, cursor + 20u);
    const uint32_t uncompressedSize = readLe32(bytes, cursor + 24u);
    const uint16_t nameLength = readLe16(bytes, cursor + 28u);
    const uint16_t extraLength = readLe16(bytes, cursor + 30u);
    const uint16_t commentLength = readLe16(bytes, cursor + 32u);
    const uint32_t localHeaderOffset = readLe32(bytes, cursor + 42u);
    if (cursor + 46u + nameLength > bytes.size()) {
      throw std::runtime_error("USDZ archive has a truncated file name");
    }
    const std::string name(
        reinterpret_cast<const char *>(bytes.data() + cursor + 46u),
        nameLength);
    cursor += 46u + nameLength + extraLength + commentLength;

    if (!hasUsdTextExtension(name)) {
      continue;
    }
    if (method != 0u) {
      throw std::runtime_error(
          "compressed USDZ entries are not supported; USDZ files must store "
          "the root USD layer without ZIP compression");
    }
    if (compressedSize != uncompressedSize) {
      throw std::runtime_error("USDZ stored entry has mismatched sizes");
    }
    if (readLe32(bytes, localHeaderOffset) != 0x04034b50u) {
      throw std::runtime_error("USDZ archive has an invalid local header");
    }

    const uint16_t localNameLength = readLe16(bytes, localHeaderOffset + 26u);
    const uint16_t localExtraLength = readLe16(bytes, localHeaderOffset + 28u);
    const size_t dataOffset =
        localHeaderOffset + 30u + localNameLength + localExtraLength;
    if (dataOffset + compressedSize > bytes.size()) {
      throw std::runtime_error("USDZ archive entry is truncated");
    }
    if (compressedSize >= 8u && std::string_view(reinterpret_cast<const char *>(
                                                     bytes.data() + dataOffset),
                                                 8u) == "PXR-USDC") {
      throw std::runtime_error(
          "binary USDC layers inside USDZ are not supported by the lightweight "
          "USD loader");
    }
    return {reinterpret_cast<const char *>(bytes.data() + dataOffset),
            compressedSize};
  }

  throw std::runtime_error(
      "USDZ archive does not contain a .usd or .usda layer");
}

} // namespace

dotbim::Model LoadFromText(std::string_view usdText, float importScale) {
  UsdParser parser(usdText);
  const std::vector<Node> nodes = parser.parse();

  dotbim::Model model{};
  std::vector<std::optional<glm::mat4>> transformCache(nodes.size());
  const glm::mat4 importTransform =
      container::geometry::usdUpAxisToRendererAxes(
          stageUpAxisFromText(usdText)) *
      glm::scale(glm::mat4(1.0f), glm::vec3(sanitizeImportScale(importScale)));

  uint32_t nextMeshId = 0;
  for (size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
    const Node &node = nodes[nodeIndex];
    if (node.typeName != "Mesh" || node.mesh.points.empty() ||
        !nodeVisible(static_cast<int>(nodeIndex), nodes)) {
      continue;
    }

    const glm::vec4 color = inheritedColor(static_cast<int>(nodeIndex), nodes);
    const uint32_t meshId = nextMeshId++;
    const size_t meshRangeCount = model.meshRanges.size();
    appendMesh(model, node.mesh, meshId, node.path, color);
    if (model.meshRanges.size() == meshRangeCount) {
      continue;
    }

    dotbim::Element element{};
    element.meshId = meshId;
    element.transform =
        importTransform *
        nodeWorldTransform(static_cast<int>(nodeIndex), nodes, transformCache);
    element.color = color;
    element.doubleSided =
        inheritedDoubleSided(static_cast<int>(nodeIndex), nodes);
    element.guid = node.path;
    element.type = "UsdGeomMesh";
    model.elements.push_back(std::move(element));
  }

  return model;
}

dotbim::Model LoadFromFile(const std::filesystem::path &path,
                           float importScale) {
  const std::string extension = lowerAscii(path.extension().string());
#if defined(CONTAINER_HAS_TINYUSDZ)
  try {
    return LoadWithTinyUsd(path, importScale);
  } catch (const std::exception &tinyUsdError) {
    if (extension == ".usdc" || fileStartsWithUsdc(path)) {
      throw;
    }

    try {
      if (extension == ".usdz") {
        return LoadFromText(extractFirstUsdTextFromUsdz(path), importScale);
      }
      return LoadFromText(readTextFile(path), importScale);
    } catch (const std::exception &fallbackError) {
      throw std::runtime_error(
          std::string(tinyUsdError.what()) +
          "; lightweight USD fallback also failed: " + fallbackError.what());
    }
  }
#else
  if (extension == ".usdz") {
    return LoadFromText(extractFirstUsdTextFromUsdz(path), importScale);
  }
  if (extension == ".usdc") {
    throw std::runtime_error(
        "binary .usdc files are not supported by the lightweight USD loader");
  }
  return LoadFromText(readTextFile(path), importScale);
#endif
}

} // namespace container::geometry::usd
