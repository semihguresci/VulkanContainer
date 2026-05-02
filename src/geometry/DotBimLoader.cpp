#include "Container/geometry/DotBimLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace container::geometry::dotbim {
namespace {

using Json = nlohmann::json;

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

const Json &requiredMember(const Json &object, const char *key,
                           std::string_view context) {
  if (!object.is_object() || !object.contains(key)) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " is missing '" + key + "'");
  }
  return object.at(key);
}

const Json &requiredArray(const Json &object, const char *key,
                          std::string_view context) {
  const Json &value = requiredMember(object, key, context);
  if (!value.is_array()) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " '" + key + "' must be an array");
  }
  return value;
}

float numberOr(const Json &object, const char *key, float fallback) {
  if (!object.is_object() || !object.contains(key)) {
    return fallback;
  }
  const Json &value = object.at(key);
  if (!value.is_number()) {
    return fallback;
  }
  const double result = value.get<double>();
  return std::isfinite(result) ? static_cast<float>(result) : fallback;
}

uint32_t requiredMeshId(const Json &object, std::string_view context) {
  const Json &value = requiredMember(object, "mesh_id", context);
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " mesh_id must be an integer");
  }
  const int64_t id = value.get<int64_t>();
  if (id < 0 || id > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " mesh_id is outside uint32 range");
  }
  return static_cast<uint32_t>(id);
}

glm::vec3 readVector(const Json &object, const char *key, glm::vec3 fallback) {
  if (!object.is_object() || !object.contains(key)) {
    return fallback;
  }
  const Json &value = object.at(key);
  if (!value.is_object()) {
    return fallback;
  }
  return {numberOr(value, "x", fallback.x), numberOr(value, "y", fallback.y),
          numberOr(value, "z", fallback.z)};
}

glm::quat readRotation(const Json &object) {
  if (!object.is_object() || !object.contains("rotation")) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const Json &value = object.at("rotation");
  if (!value.is_object()) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  glm::quat rotation(numberOr(value, "qw", 1.0f), numberOr(value, "qx", 0.0f),
                     numberOr(value, "qy", 0.0f), numberOr(value, "qz", 0.0f));
  const float len2 = glm::dot(rotation, rotation);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  return glm::normalize(rotation);
}

float colorChannelOr(const Json &color, const char *key, float fallback) {
  if (!color.is_object() || !color.contains(key) ||
      !color.at(key).is_number()) {
    return fallback;
  }
  const double value = color.at(key).get<double>();
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(static_cast<float>(value), 0.0f, 255.0f) / 255.0f;
}

glm::vec4 readColor(const Json &element) {
  if (!element.is_object() || !element.contains("color") ||
      !element.at("color").is_object()) {
    return {0.8f, 0.82f, 0.86f, 1.0f};
  }
  const Json &color = element.at("color");
  return {
      colorChannelOr(color, "r", 204.0f), colorChannelOr(color, "g", 209.0f),
      colorChannelOr(color, "b", 219.0f), colorChannelOr(color, "a", 255.0f)};
}

std::string stringOrEmpty(const Json &object, const char *key) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_string()) {
    return {};
  }
  return object.at(key).get<std::string>();
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

Vertex makeVertex(const glm::vec3 &position, const glm::vec3 &normal,
                  const glm::vec3 &tangent) {
  Vertex vertex{};
  vertex.position = position;
  vertex.normal = normal;
  vertex.tangent = glm::vec4(tangent, 1.0f);
  return vertex;
}

std::vector<glm::vec3> readCoordinates(const Json &mesh,
                                       std::string_view context) {
  const Json *coordinates = nullptr;
  if (mesh.contains("coordinates")) {
    coordinates = &requiredArray(mesh, "coordinates", context);
  } else {
    coordinates = &requiredArray(mesh, "vertices_coordinates", context);
  }
  if ((coordinates->size() % 3u) != 0u) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " coordinates count must be divisible by 3");
  }

  std::vector<glm::vec3> result;
  result.reserve(coordinates->size() / 3u);
  for (size_t i = 0; i < coordinates->size(); i += 3u) {
    if (!(*coordinates)[i].is_number() || !(*coordinates)[i + 1].is_number() ||
        !(*coordinates)[i + 2].is_number()) {
      throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                               " coordinates must contain numbers");
    }
    result.emplace_back((*coordinates)[i].get<float>(),
                        (*coordinates)[i + 1].get<float>(),
                        (*coordinates)[i + 2].get<float>());
  }
  return result;
}

std::vector<uint32_t> readIndices(const Json &mesh, std::string_view context,
                                  size_t vertexCount) {
  const Json &indices = requiredArray(mesh, "indices", context);
  if ((indices.size() % 3u) != 0u) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " indices count must be divisible by 3");
  }

  std::vector<uint32_t> result;
  result.reserve(indices.size());
  for (const Json &value : indices) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
      throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                               " indices must contain integers");
    }
    const int64_t index = value.get<int64_t>();
    if (index < 0 || static_cast<uint64_t>(index) >= vertexCount) {
      throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                               " contains an out-of-range index");
    }
    result.push_back(static_cast<uint32_t>(index));
  }
  return result;
}

std::pair<glm::vec3, float>
computeBounds(const std::vector<glm::vec3> &coordinates) {
  if (coordinates.empty()) {
    return {{0.0f, 0.0f, 0.0f}, 0.0f};
  }

  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
  for (const auto &point : coordinates) {
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  }

  const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
  float radius = 0.0f;
  for (const auto &point : coordinates) {
    radius = std::max(radius, glm::length(point - center));
  }
  return {center, radius};
}

glm::mat4 makeTransform(const Json &element, float importScale) {
  const glm::vec3 translation = readVector(element, "vector", glm::vec3(0.0f));
  const glm::quat rotation = readRotation(element);
  return container::geometry::zUpForwardYToRendererAxes() *
         glm::scale(glm::mat4(1.0f),
                    glm::vec3(sanitizeImportScale(importScale))) *
         glm::translate(glm::mat4(1.0f), translation) *
         glm::mat4_cast(rotation);
}

} // namespace

Model LoadFromJson(std::string_view jsonText, float importScale) {
  const Json root = Json::parse(jsonText.begin(), jsonText.end());
  const Json &meshes = requiredArray(root, "meshes", "file");
  const Json &elements = requiredArray(root, "elements", "file");

  Model model{};
  model.meshRanges.reserve(meshes.size());
  std::unordered_set<uint32_t> seenMeshIds;

  for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
    const Json &mesh = meshes[meshIndex];
    const std::string context = "mesh[" + std::to_string(meshIndex) + "]";
    const uint32_t meshId = requiredMeshId(mesh, context);
    if (!seenMeshIds.insert(meshId).second) {
      throw std::runtime_error("dotbim duplicate mesh_id " +
                               std::to_string(meshId));
    }

    const auto coordinates = readCoordinates(mesh, context);
    const auto sourceIndices = readIndices(mesh, context, coordinates.size());

    MeshRange range{};
    range.meshId = meshId;
    range.firstIndex = static_cast<uint32_t>(model.indices.size());
    const auto [center, radius] = computeBounds(coordinates);
    range.boundsCenter = center;
    range.boundsRadius = radius;

    for (size_t i = 0; i < sourceIndices.size(); i += 3u) {
      const glm::vec3 &a = coordinates[sourceIndices[i]];
      const glm::vec3 &b = coordinates[sourceIndices[i + 1u]];
      const glm::vec3 &c = coordinates[sourceIndices[i + 2u]];
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
    model.meshRanges.push_back(range);
  }

  model.elements.reserve(elements.size());
  for (size_t elementIndex = 0; elementIndex < elements.size();
       ++elementIndex) {
    const Json &source = elements[elementIndex];
    const std::string context = "element[" + std::to_string(elementIndex) + "]";
    Element element{};
    element.meshId = requiredMeshId(source, context);
    element.transform = makeTransform(source, importScale);
    element.color = readColor(source);
    element.guid = stringOrEmpty(source, "guid");
    element.type = stringOrEmpty(source, "type");
    model.elements.push_back(std::move(element));
  }

  return model;
}

Model LoadFromFile(const std::filesystem::path &path, float importScale) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open dotbim file: " +
                             container::util::pathToUtf8(path));
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return LoadFromJson(text, importScale);
}

} // namespace container::geometry::dotbim
