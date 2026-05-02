#include "Container/geometry/IfcxLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace container::geometry::ifcx {
namespace {

using Json = nlohmann::json;

struct Node {
  std::string path{};
  Json attributes{Json::object()};
  std::unordered_map<std::string, std::string> children{};
};

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

const Json &requiredArray(const Json &object, const char *key,
                          std::string_view context) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_array()) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " is missing array member '" + key + "'");
  }
  return object.at(key);
}

std::optional<float> numberAsFloat(const Json &value) {
  if (!value.is_number()) {
    return std::nullopt;
  }
  const double parsed = value.get<double>();
  if (!std::isfinite(parsed)) {
    return std::nullopt;
  }
  return static_cast<float>(parsed);
}

std::optional<glm::vec3> readVec3(const Json &value) {
  if (!value.is_array() || value.size() < 3u) {
    return std::nullopt;
  }

  const auto x = numberAsFloat(value[0]);
  const auto y = numberAsFloat(value[1]);
  const auto z = numberAsFloat(value[2]);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

std::optional<uint32_t> readIndex(const Json &value, size_t vertexCount) {
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    return std::nullopt;
  }
  const auto parsed = value.get<int64_t>();
  if (parsed < 0 ||
      static_cast<uint64_t>(parsed) >= static_cast<uint64_t>(vertexCount) ||
      static_cast<uint64_t>(parsed) >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(parsed);
}

glm::vec3 safeNormal(const glm::vec3 &a, const glm::vec3 &b,
                     const glm::vec3 &c) {
  const glm::vec3 n = glm::cross(b - a, c - a);
  const float length = glm::length(n);
  if (length <= 1.0e-8f) {
    return {0.0f, 1.0f, 0.0f};
  }
  return n / length;
}

glm::vec3 safeTangent(const glm::vec3 &a, const glm::vec3 &b,
                      const glm::vec3 &normal) {
  glm::vec3 tangent = b - a;
  tangent -= normal * glm::dot(tangent, normal);
  const float length = glm::length(tangent);
  if (length <= 1.0e-8f) {
    const glm::vec3 axis = std::abs(normal.y) < 0.9f
                               ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
    tangent = glm::cross(axis, normal);
    const float fallbackLength = glm::length(tangent);
    return fallbackLength > 1.0e-8f ? tangent / fallbackLength
                                    : glm::vec3(1.0f, 0.0f, 0.0f);
  }
  return tangent / length;
}

container::geometry::Vertex makeVertex(const glm::vec3 &position,
                                       const glm::vec3 &normal,
                                       const glm::vec3 &tangent) {
  container::geometry::Vertex vertex{};
  vertex.position = position;
  vertex.color = {1.0f, 1.0f, 1.0f};
  vertex.normal = normal;
  vertex.tangent = glm::vec4(tangent, 1.0f);
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

std::vector<glm::vec3> readPoints(const Json &mesh, std::string_view context) {
  const Json &points = requiredArray(mesh, "points", context);
  std::vector<glm::vec3> result;
  result.reserve(points.size());
  for (const Json &point : points) {
    if (auto parsed = readVec3(point)) {
      result.push_back(*parsed);
    } else {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid mesh point");
    }
  }
  return result;
}

std::vector<uint32_t> readTriangulatedIndices(const Json &mesh,
                                              std::string_view context,
                                              size_t vertexCount) {
  const Json &sourceIndices = requiredArray(mesh, "faceVertexIndices", context);
  const Json *faceVertexCounts = nullptr;
  if (mesh.contains("faceVertexCounts") &&
      mesh.at("faceVertexCounts").is_array()) {
    faceVertexCounts = &mesh.at("faceVertexCounts");
  }

  std::vector<uint32_t> rawIndices;
  rawIndices.reserve(sourceIndices.size());
  for (const Json &value : sourceIndices) {
    if (auto index = readIndex(value, vertexCount)) {
      rawIndices.push_back(*index);
    } else {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid face vertex index");
    }
  }

  if (faceVertexCounts == nullptr) {
    if ((rawIndices.size() % 3u) != 0u) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has non-triangle indices without counts");
    }
    return rawIndices;
  }

  std::vector<uint32_t> triangles;
  size_t cursor = 0;
  for (const Json &countValue : *faceVertexCounts) {
    if (!countValue.is_number_integer() && !countValue.is_number_unsigned()) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid face vertex count");
    }
    const auto count = countValue.get<int64_t>();
    if (count < 3 || cursor + static_cast<size_t>(count) > rawIndices.size()) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has inconsistent face vertex counts");
    }

    const uint32_t first = rawIndices[cursor];
    for (int64_t i = 1; i + 1 < count; ++i) {
      triangles.push_back(first);
      triangles.push_back(rawIndices[cursor + static_cast<size_t>(i)]);
      triangles.push_back(rawIndices[cursor + static_cast<size_t>(i + 1)]);
    }
    cursor += static_cast<size_t>(count);
  }

  if (cursor != rawIndices.size()) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " has unused face vertex indices");
  }
  return triangles;
}

glm::mat4 readUsdTransform(const Json &attributes) {
  if (!attributes.is_object() || !attributes.contains("usd::xformop")) {
    return glm::mat4(1.0f);
  }
  const Json &xform = attributes.at("usd::xformop");
  if (!xform.is_object() || !xform.contains("transform") ||
      !xform.at("transform").is_array() || xform.at("transform").size() < 4u) {
    return glm::mat4(1.0f);
  }

  glm::mat4 result(1.0f);
  const Json &rows = xform.at("transform");
  for (size_t column = 0; column < 4u; ++column) {
    if (!rows[column].is_array() || rows[column].size() < 4u) {
      return glm::mat4(1.0f);
    }
    for (size_t row = 0; row < 4u; ++row) {
      const auto value = numberAsFloat(rows[column][row]);
      if (!value) {
        return glm::mat4(1.0f);
      }
      result[static_cast<glm::length_t>(column)]
            [static_cast<glm::length_t>(row)] = *value;
    }
  }
  return result;
}

void mergeAttributes(Node &node, const Json &attributes) {
  if (!attributes.is_object()) {
    return;
  }
  for (auto it = attributes.begin(); it != attributes.end(); ++it) {
    node.attributes[it.key()] = it.value();
  }
}

void collectChildReferences(const Json &value,
                            std::vector<std::string> &children) {
  if (value.is_string()) {
    children.push_back(value.get<std::string>());
    return;
  }
  if (value.is_array()) {
    for (const Json &item : value) {
      collectChildReferences(item, children);
    }
    return;
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      collectChildReferences(it.value(), children);
    }
  }
}

std::unordered_map<std::string, Node>
buildNodes(const Json &data,
           std::unordered_map<std::string, std::string> &parentByChild) {
  std::unordered_map<std::string, Node> nodes;
  for (const Json &item : data) {
    if (!item.is_object() || !item.contains("path") ||
        !item.at("path").is_string()) {
      continue;
    }

    const std::string path = item.at("path").get<std::string>();
    Node &node = nodes[path];
    node.path = path;
    if (item.contains("attributes")) {
      mergeAttributes(node, item.at("attributes"));
    }
    if (item.contains("children") && item.at("children").is_object()) {
      for (auto it = item.at("children").begin();
           it != item.at("children").end(); ++it) {
        std::vector<std::string> childPaths;
        collectChildReferences(it.value(), childPaths);
        for (const std::string &childPath : childPaths) {
          node.children.emplace(it.key(), childPath);
          parentByChild.emplace(childPath, path);
        }
      }
    }
  }
  return nodes;
}

glm::mat4
nodeTransform(const std::string &path,
              const std::unordered_map<std::string, Node> &nodes,
              const std::unordered_map<std::string, std::string> &parentByChild,
              std::unordered_map<std::string, glm::mat4> &cache,
              std::unordered_set<std::string> &visiting) {
  if (auto cached = cache.find(path); cached != cache.end()) {
    return cached->second;
  }
  if (!visiting.insert(path).second) {
    return glm::mat4(1.0f);
  }

  glm::mat4 transform(1.0f);
  if (auto parentIt = parentByChild.find(path);
      parentIt != parentByChild.end()) {
    transform =
        nodeTransform(parentIt->second, nodes, parentByChild, cache, visiting);
  }
  if (auto nodeIt = nodes.find(path); nodeIt != nodes.end()) {
    transform *= readUsdTransform(nodeIt->second.attributes);
  }

  visiting.erase(path);
  cache.emplace(path, transform);
  return transform;
}

const Node *nearestNodeWithAttribute(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    const std::unordered_map<std::string, std::string> &parentByChild,
    std::string_view key) {
  std::string cursor = path;
  std::unordered_set<std::string> visited;
  while (!cursor.empty() && visited.insert(cursor).second) {
    const auto nodeIt = nodes.find(cursor);
    if (nodeIt != nodes.end() && nodeIt->second.attributes.contains(key)) {
      return &nodeIt->second;
    }
    const auto parentIt = parentByChild.find(cursor);
    if (parentIt == parentByChild.end()) {
      break;
    }
    cursor = parentIt->second;
  }
  return nullptr;
}

bool isInvisible(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    const std::unordered_map<std::string, std::string> &parentByChild) {
  const Node *node = nearestNodeWithAttribute(path, nodes, parentByChild,
                                              "usd::usdgeom::visibility");
  if (node == nullptr) {
    return false;
  }
  const Json &value = node->attributes.at("usd::usdgeom::visibility");
  if (value.is_object() && value.contains("visibility") &&
      value.at("visibility").is_string()) {
    return value.at("visibility").get<std::string>() == "invisible";
  }
  if (value.is_string()) {
    return value.get<std::string>() == "invisible";
  }
  return false;
}

glm::vec4 inheritedColor(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    const std::unordered_map<std::string, std::string> &parentByChild) {
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  if (const Node *node = nearestNodeWithAttribute(
          path, nodes, parentByChild, "bsi::ifc::presentation::diffuseColor")) {
    const Json &diffuse =
        node->attributes.at("bsi::ifc::presentation::diffuseColor");
    if (diffuse.is_array() && diffuse.size() >= 3u) {
      const auto r = numberAsFloat(diffuse[0]);
      const auto g = numberAsFloat(diffuse[1]);
      const auto b = numberAsFloat(diffuse[2]);
      if (r && g && b) {
        color.r = std::clamp(*r, 0.0f, 1.0f);
        color.g = std::clamp(*g, 0.0f, 1.0f);
        color.b = std::clamp(*b, 0.0f, 1.0f);
      }
    }
  }
  if (const Node *node = nearestNodeWithAttribute(
          path, nodes, parentByChild, "bsi::ifc::presentation::opacity")) {
    const auto opacity =
        numberAsFloat(node->attributes.at("bsi::ifc::presentation::opacity"));
    if (opacity) {
      color.a = std::clamp(*opacity, 0.0f, 1.0f);
    }
  }
  return color;
}

std::string inheritedType(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    const std::unordered_map<std::string, std::string> &parentByChild) {
  const Node *node =
      nearestNodeWithAttribute(path, nodes, parentByChild, "bsi::ifc::class");
  if (node == nullptr) {
    return {};
  }

  const Json &value = node->attributes.at("bsi::ifc::class");
  if (value.is_object() && value.contains("code") &&
      value.at("code").is_string()) {
    return value.at("code").get<std::string>();
  }
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return {};
}

void appendMesh(Model &model, const Json &mesh, uint32_t meshId,
                std::string_view context) {
  const std::vector<glm::vec3> points = readPoints(mesh, context);
  const std::vector<uint32_t> sourceIndices =
      readTriangulatedIndices(mesh, context, points.size());
  if (points.empty() || sourceIndices.empty()) {
    return;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(points);
  range.boundsCenter = center;
  range.boundsRadius = radius;

  for (size_t i = 0; i + 2u < sourceIndices.size(); i += 3u) {
    const glm::vec3 &a = points[sourceIndices[i]];
    const glm::vec3 &b = points[sourceIndices[i + 1u]];
    const glm::vec3 &c = points[sourceIndices[i + 2u]];
    const glm::vec3 normal = safeNormal(a, b, c);
    const glm::vec3 tangent = safeTangent(a, b, normal);
    const uint32_t base = static_cast<uint32_t>(model.vertices.size());
    model.vertices.push_back(makeVertex(a, normal, tangent));
    model.vertices.push_back(makeVertex(b, normal, tangent));
    model.vertices.push_back(makeVertex(c, normal, tangent));
    model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.meshRanges.push_back(range);
  }
}

} // namespace

Model LoadFromJson(std::string_view jsonText, float importScale) {
  const Json root = Json::parse(jsonText.begin(), jsonText.end());
  const Json &data = requiredArray(root, "data", "file");

  std::unordered_map<std::string, std::string> parentByChild;
  const auto nodes = buildNodes(data, parentByChild);

  Model model{};
  std::unordered_map<std::string, glm::mat4> transformCache;
  uint32_t nextMeshId = 0;
  const glm::mat4 importTransform =
      container::geometry::zUpForwardYToRendererAxes() *
      glm::scale(glm::mat4(1.0f), glm::vec3(sanitizeImportScale(importScale)));

  for (const auto &[path, node] : nodes) {
    if (!node.attributes.contains("usd::usdgeom::mesh") ||
        isInvisible(path, nodes, parentByChild)) {
      continue;
    }

    const Json &mesh = node.attributes.at("usd::usdgeom::mesh");
    if (!mesh.is_object()) {
      continue;
    }

    const uint32_t meshId = nextMeshId++;
    const uint32_t meshRangeCount =
        static_cast<uint32_t>(model.meshRanges.size());
    appendMesh(model, mesh, meshId, "mesh[" + path + "]");
    if (model.meshRanges.size() == meshRangeCount) {
      continue;
    }

    std::unordered_set<std::string> visiting;
    dotbim::Element element{};
    element.meshId = meshId;
    element.transform =
        importTransform *
        nodeTransform(path, nodes, parentByChild, transformCache, visiting);
    element.color = inheritedColor(path, nodes, parentByChild);
    element.guid = path;
    element.type = inheritedType(path, nodes, parentByChild);
    model.elements.push_back(std::move(element));
  }

  return model;
}

Model LoadFromFile(const std::filesystem::path &path, float importScale) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open IFCX file: " +
                             container::util::pathToUtf8(path));
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return LoadFromJson(text, importScale);
}

} // namespace container::geometry::ifcx
