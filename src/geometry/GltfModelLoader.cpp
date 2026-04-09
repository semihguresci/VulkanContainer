#include <Container/geometry/GltfModelLoader.h>

#include <Container/geometry/Mesh.h>

#include <tiny_gltf.h>

#include <glm/geometric.hpp>

#include <cctype>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace geometry {
namespace gltf {
namespace {

bool isBinaryGltf(const std::string& path) {
  const auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos || dotPos + 1 >= path.size()) return false;

  std::string ext = path.substr(dotPos + 1);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == "glb";
}

glm::vec3 readVec3(const tinygltf::Accessor& accessor,
                   const tinygltf::BufferView& view,
                   const tinygltf::Buffer& buffer, size_t index) {
  size_t stride = accessor.ByteStride(view);
  if (stride == 0) {
    stride = sizeof(float) * 3;
  }
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  const auto* element = reinterpret_cast<const float*>(dataStart + index * stride);
  return glm::vec3(element[0], element[1], element[2]);
}

glm::vec4 readVec4(const tinygltf::Accessor& accessor,
                   const tinygltf::BufferView& view,
                   const tinygltf::Buffer& buffer, size_t index) {
  size_t stride = accessor.ByteStride(view);
  if (stride == 0) {
    stride = sizeof(float) * 4;
  }
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  const auto* element = reinterpret_cast<const float*>(dataStart + index * stride);
  return glm::vec4(element[0], element[1], element[2], element[3]);
}

glm::vec3 fallbackTangentForNormal(const glm::vec3& normal) {
  glm::vec3 axis = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                               : glm::vec3(1.0f, 0.0f, 0.0f);
  glm::vec3 tangent = glm::cross(axis, normal);
  if (glm::dot(tangent, tangent) < 1e-8f) {
    tangent = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), normal);
  }
  if (glm::dot(tangent, tangent) < 1e-8f) {
    tangent = glm::vec3(1.0f, 0.0f, 0.0f);
  }
  return glm::normalize(tangent);
}

bool isFiniteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

void generateMissingNormals(std::vector<Vertex>& vertices,
                            const std::vector<uint32_t>& indices) {
  for (auto& vertex : vertices) {
    vertex.normal = glm::vec3(0.0f);
  }

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1];
    const uint32_t i2 = indices[i + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
      continue;
    }

    const glm::vec3 edge01 = vertices[i1].position - vertices[i0].position;
    const glm::vec3 edge02 = vertices[i2].position - vertices[i0].position;
    const glm::vec3 faceNormal = glm::cross(edge01, edge02);
    if (glm::dot(faceNormal, faceNormal) < 1e-8f) {
      continue;
    }

    vertices[i0].normal += faceNormal;
    vertices[i1].normal += faceNormal;
    vertices[i2].normal += faceNormal;
  }

  for (auto& vertex : vertices) {
    if (glm::dot(vertex.normal, vertex.normal) < 1e-8f) {
      vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
      continue;
    }
    vertex.normal = glm::normalize(vertex.normal);
  }
}

void normalizeLoadedNormals(std::vector<Vertex>& vertices) {
  for (auto& vertex : vertices) {
    if (glm::dot(vertex.normal, vertex.normal) < 1e-8f ||
        !isFiniteVec3(vertex.normal)) {
      vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
      continue;
    }
    vertex.normal = glm::normalize(vertex.normal);
  }
}

void sanitizeTangents(std::vector<Vertex>& vertices) {
  for (auto& vertex : vertices) {
    glm::vec3 tangent(vertex.tangent);
    if (!isFiniteVec3(tangent) ||
        glm::dot(tangent, tangent) < 1e-8f) {
      tangent = fallbackTangentForNormal(vertex.normal);
    } else {
      tangent = glm::normalize(tangent);
    }

    float handedness = std::isfinite(vertex.tangent.w) ? vertex.tangent.w : 1.0f;
    handedness = handedness < 0.0f ? -1.0f : 1.0f;
    vertex.tangent = glm::vec4(tangent, handedness);
  }
}

void alignNormalsAndTangentsToWinding(std::vector<Vertex>& vertices,
                                      const std::vector<uint32_t>& indices) {
  std::vector<glm::vec3> windingSums(vertices.size(), glm::vec3(0.0f));

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1];
    const uint32_t i2 = indices[i + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
      continue;
    }

    const glm::vec3 edge01 = vertices[i1].position - vertices[i0].position;
    const glm::vec3 edge02 = vertices[i2].position - vertices[i0].position;
    const glm::vec3 faceNormal = glm::cross(edge01, edge02);
    if (!isFiniteVec3(faceNormal) ||
        glm::dot(faceNormal, faceNormal) < 1e-8f) {
      continue;
    }

    windingSums[i0] += faceNormal;
    windingSums[i1] += faceNormal;
    windingSums[i2] += faceNormal;
  }

  for (size_t i = 0; i < vertices.size(); ++i) {
    const glm::vec3 reference = windingSums[i];
    if (!isFiniteVec3(reference) ||
        glm::dot(reference, reference) < 1e-8f) {
      continue;
    }

    glm::vec3 normal = vertices[i].normal;
    if (!isFiniteVec3(normal) || glm::dot(normal, normal) < 1e-8f) {
      vertices[i].normal = glm::normalize(reference);
      continue;
    }

    normal = glm::normalize(normal);
    if (glm::dot(normal, reference) < 0.0f) {
      vertices[i].normal = -normal;
      vertices[i].tangent.w = -vertices[i].tangent.w;
    } else {
      vertices[i].normal = normal;
    }
  }
}

void generateMissingTangents(std::vector<Vertex>& vertices,
                             const std::vector<uint32_t>& indices) {
  std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
  std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));

  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const uint32_t i0 = indices[i];
    const uint32_t i1 = indices[i + 1];
    const uint32_t i2 = indices[i + 2];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
      continue;
    }

    const glm::vec3& p0 = vertices[i0].position;
    const glm::vec3& p1 = vertices[i1].position;
    const glm::vec3& p2 = vertices[i2].position;
    const glm::vec2& uv0 = vertices[i0].texCoord;
    const glm::vec2& uv1 = vertices[i1].texCoord;
    const glm::vec2& uv2 = vertices[i2].texCoord;

    const glm::vec3 edge01 = p1 - p0;
    const glm::vec3 edge02 = p2 - p0;
    const glm::vec2 deltaUv01 = uv1 - uv0;
    const glm::vec2 deltaUv02 = uv2 - uv0;

    const float determinant =
        deltaUv01.x * deltaUv02.y - deltaUv01.y * deltaUv02.x;
    if (std::abs(determinant) < 1e-8f) {
      continue;
    }

    const float invDeterminant = 1.0f / determinant;
    const glm::vec3 triangleTangent =
        (edge01 * deltaUv02.y - edge02 * deltaUv01.y) * invDeterminant;
    const glm::vec3 triangleBitangent =
        (edge02 * deltaUv01.x - edge01 * deltaUv02.x) * invDeterminant;

    tangents[i0] += triangleTangent;
    tangents[i1] += triangleTangent;
    tangents[i2] += triangleTangent;
    bitangents[i0] += triangleBitangent;
    bitangents[i1] += triangleBitangent;
    bitangents[i2] += triangleBitangent;
  }

  for (size_t i = 0; i < vertices.size(); ++i) {
    glm::vec3 normal = vertices[i].normal;
    if (glm::dot(normal, normal) < 1e-8f) {
      normal = glm::vec3(0.0f, 1.0f, 0.0f);
    } else {
      normal = glm::normalize(normal);
    }

    glm::vec3 tangent = tangents[i] - normal * glm::dot(normal, tangents[i]);
    if (glm::dot(tangent, tangent) < 1e-8f) {
      tangent = fallbackTangentForNormal(normal);
    } else {
      tangent = glm::normalize(tangent);
    }

    float handedness = 1.0f;
    if (glm::dot(bitangents[i], bitangents[i]) >= 1e-8f) {
      handedness =
          glm::dot(glm::cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f
                                                                       : 1.0f;
    }

    vertices[i].tangent = glm::vec4(tangent, handedness);
  }
}

struct PrimitiveVertexData {
  std::vector<Vertex> vertices{};
  bool hasNormals{false};
  bool hasTangents{false};
};

std::vector<uint32_t> readIndices(const tinygltf::Model& model,
                                  const tinygltf::Primitive& primitive,
                                  size_t vertexCount) {
  if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
    throw std::runtime_error("Only glTF triangle primitives are supported");
  }

  if (primitive.indices < 0) {
    if (vertexCount % 3 != 0) {
      throw std::runtime_error("Non-indexed glTF primitive must contain triangles");
    }
    std::vector<uint32_t> sequential(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
      sequential[i] = static_cast<uint32_t>(i);
    }
    return sequential;
  }

  const auto& accessor = model.accessors[primitive.indices];
  if (accessor.type != TINYGLTF_TYPE_SCALAR) {
    throw std::runtime_error("glTF indices accessor must be scalar");
  }
  if (accessor.bufferView < 0 || accessor.bufferView >= model.bufferViews.size()) {
    throw std::runtime_error("glTF indices accessor missing buffer view");
  }

  const auto& view = model.bufferViews[accessor.bufferView];
  if (view.buffer < 0 || view.buffer >= model.buffers.size()) {
    throw std::runtime_error("glTF indices buffer view missing buffer");
  }
  const auto& buffer = model.buffers[view.buffer];
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  size_t stride = accessor.ByteStride(view);
  if (stride == 0) {
    stride = tinygltf::GetComponentSizeInBytes(accessor.componentType);
  }

  std::vector<uint32_t> indices(accessor.count);
  for (size_t i = 0; i < accessor.count; ++i) {
    const auto* element = dataStart + i * stride;
    switch (accessor.componentType) {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        indices[i] = static_cast<uint32_t>(*reinterpret_cast<const uint16_t*>(element));
        break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        indices[i] = static_cast<uint32_t>(*reinterpret_cast<const uint32_t*>(element));
        break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        indices[i] = static_cast<uint32_t>(*reinterpret_cast<const uint8_t*>(element));
        break;
      default:
        throw std::runtime_error("Unsupported glTF index component type");
    }
  }
  return indices;
}

PrimitiveVertexData mergeAttributes(const tinygltf::Model& model,
                                    const tinygltf::Primitive& primitive) {
  auto positionIt = primitive.attributes.find("POSITION");
  if (positionIt == primitive.attributes.end()) {
    throw std::runtime_error("glTF primitive is missing POSITION attribute");
  }

  const auto& positionAccessor = model.accessors[positionIt->second];
  if (positionAccessor.bufferView < 0 ||
      positionAccessor.bufferView >= model.bufferViews.size()) {
    throw std::runtime_error("POSITION accessor missing buffer view");
  }
  if (positionAccessor.type != TINYGLTF_TYPE_VEC3 ||
      positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
    throw std::runtime_error("POSITION must be a VEC3 float attribute");
  }

  const auto& positionView = model.bufferViews[positionAccessor.bufferView];
  if (positionView.buffer < 0 || positionView.buffer >= model.buffers.size()) {
    throw std::runtime_error("POSITION buffer view missing buffer");
  }
  const auto& positionBuffer = model.buffers[positionView.buffer];

  PrimitiveVertexData primitiveData{};
  primitiveData.vertices.resize(positionAccessor.count);
  for (size_t i = 0; i < positionAccessor.count; ++i) {
    primitiveData.vertices[i].position =
        readVec3(positionAccessor, positionView, positionBuffer, i);
  }

  auto colorIt = primitive.attributes.find("COLOR_0");
  if (colorIt != primitive.attributes.end()) {
    const auto& colorAccessor = model.accessors[colorIt->second];
    if (colorAccessor.bufferView < 0 ||
        colorAccessor.bufferView >= model.bufferViews.size()) {
      throw std::runtime_error("COLOR_0 accessor missing buffer view");
    }
    const auto& colorView = model.bufferViews[colorAccessor.bufferView];
    if (colorView.buffer < 0 || colorView.buffer >= model.buffers.size()) {
      throw std::runtime_error("COLOR_0 buffer view missing buffer");
    }
    const auto& colorBuffer = model.buffers[colorView.buffer];
    size_t stride = colorAccessor.ByteStride(colorView);
    if (stride == 0) {
      stride = tinygltf::GetNumComponentsInType(colorAccessor.type) *
               tinygltf::GetComponentSizeInBytes(colorAccessor.componentType);
    }
    const auto* dataStart =
        colorBuffer.data.data() + colorView.byteOffset + colorAccessor.byteOffset;

    if (colorAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("COLOR_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      const auto* element = dataStart + i * stride;
      if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
          colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        const auto* value = reinterpret_cast<const float*>(element);
        primitiveData.vertices[i].color = glm::vec3(value[0], value[1], value[2]);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        const auto* value = reinterpret_cast<const float*>(element);
        primitiveData.vertices[i].color = glm::vec3(value[0], value[1], value[2]);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint8_t*>(element);
        primitiveData.vertices[i].color =
            glm::vec3(value[0], value[1], value[2]) / 255.0f;
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint8_t*>(element);
        primitiveData.vertices[i].color =
            glm::vec3(value[0], value[1], value[2]) / 255.0f;
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint16_t*>(element);
        primitiveData.vertices[i].color =
            glm::vec3(value[0], value[1], value[2]) / glm::vec3(65535.0f);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint16_t*>(element);
        primitiveData.vertices[i].color =
            glm::vec3(value[0], value[1], value[2]) / glm::vec3(65535.0f);
      }
    }
  }

  auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
  if (texCoordIt != primitive.attributes.end()) {
    const auto& texCoordAccessor = model.accessors[texCoordIt->second];
    if (texCoordAccessor.type != TINYGLTF_TYPE_VEC2 ||
        texCoordAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
      throw std::runtime_error("TEXCOORD_0 must be a VEC2 float attribute");
    }

    if (texCoordAccessor.bufferView < 0 ||
        texCoordAccessor.bufferView >= model.bufferViews.size()) {
      throw std::runtime_error("TEXCOORD_0 accessor missing buffer view");
    }
    const auto& texCoordView = model.bufferViews[texCoordAccessor.bufferView];
    if (texCoordView.buffer < 0 || texCoordView.buffer >= model.buffers.size()) {
      throw std::runtime_error("TEXCOORD_0 buffer view missing buffer");
    }
    const auto& texCoordBuffer = model.buffers[texCoordView.buffer];
    size_t stride = texCoordAccessor.ByteStride(texCoordView);
    if (stride == 0) {
      stride = sizeof(float) * 2;
    }
    const auto* dataStart =
        texCoordBuffer.data.data() + texCoordView.byteOffset + texCoordAccessor.byteOffset;

    if (texCoordAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TEXCOORD_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      const auto* element = dataStart + i * stride;
      const auto* value = reinterpret_cast<const float*>(element);
      primitiveData.vertices[i].texCoord = glm::vec2(value[0], value[1]);
    }
  }

  auto normalIt = primitive.attributes.find("NORMAL");
  if (normalIt != primitive.attributes.end()) {
    const auto& normalAccessor = model.accessors[normalIt->second];
    if (normalAccessor.type != TINYGLTF_TYPE_VEC3 ||
        normalAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
      throw std::runtime_error("NORMAL must be a VEC3 float attribute");
    }
    if (normalAccessor.bufferView < 0 ||
        normalAccessor.bufferView >= model.bufferViews.size()) {
      throw std::runtime_error("NORMAL accessor missing buffer view");
    }
    const auto& normalView = model.bufferViews[normalAccessor.bufferView];
    if (normalView.buffer < 0 || normalView.buffer >= model.buffers.size()) {
      throw std::runtime_error("NORMAL buffer view missing buffer");
    }
    const auto& normalBuffer = model.buffers[normalView.buffer];
    if (normalAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("NORMAL attribute count is smaller than POSITION count");
    }

    primitiveData.hasNormals = true;
    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      primitiveData.vertices[i].normal =
          readVec3(normalAccessor, normalView, normalBuffer, i);
    }
  }

  auto tangentIt = primitive.attributes.find("TANGENT");
  if (tangentIt != primitive.attributes.end()) {
    const auto& tangentAccessor = model.accessors[tangentIt->second];
    if (tangentAccessor.type != TINYGLTF_TYPE_VEC4 ||
        tangentAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
      throw std::runtime_error("TANGENT must be a VEC4 float attribute");
    }
    if (tangentAccessor.bufferView < 0 ||
        tangentAccessor.bufferView >= model.bufferViews.size()) {
      throw std::runtime_error("TANGENT accessor missing buffer view");
    }
    const auto& tangentView = model.bufferViews[tangentAccessor.bufferView];
    if (tangentView.buffer < 0 || tangentView.buffer >= model.buffers.size()) {
      throw std::runtime_error("TANGENT buffer view missing buffer");
    }
    const auto& tangentBuffer = model.buffers[tangentView.buffer];
    if (tangentAccessor.count < primitiveData.vertices.size()) {
      throw std::runtime_error("TANGENT attribute count is smaller than POSITION count");
    }

    primitiveData.hasTangents = true;
    for (size_t i = 0; i < primitiveData.vertices.size(); ++i) {
      primitiveData.vertices[i].tangent =
          readVec4(tangentAccessor, tangentView, tangentBuffer, i);
    }
  }

  return primitiveData;
}

std::vector<Mesh> parseMeshes(const tinygltf::Model& model) {
  std::vector<Mesh> meshes;
  for (const auto& mesh : model.meshes) {
    for (const auto& primitive : mesh.primitives) {
      auto primitiveData = mergeAttributes(model, primitive);
      auto indices = readIndices(model, primitive, primitiveData.vertices.size());
      if (!primitiveData.hasNormals) {
        generateMissingNormals(primitiveData.vertices, indices);
      } else {
        normalizeLoadedNormals(primitiveData.vertices);
      }
      if (!primitiveData.hasTangents) {
        generateMissingTangents(primitiveData.vertices, indices);
      }
      sanitizeTangents(primitiveData.vertices);
      alignNormalsAndTangentsToWinding(primitiveData.vertices, indices);
      meshes.emplace_back(std::move(primitiveData.vertices), std::move(indices),
                          primitive.material);
    }
  }
  return meshes;
}

tinygltf::Model loadGltfModel(const std::string& path) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err;
  std::string warn;

  bool loaded = false;
  if (isBinaryGltf(path)) {
    loaded = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  } else {
    loaded = loader.LoadASCIIFromFile(&model, &err, &warn, path);
  }

  if (!warn.empty()) {
    std::clog << "glTF warning: " << warn << std::endl;
  }
  if (!loaded) {
    throw std::runtime_error("Failed to load glTF file: " + err);
  }

  return model;
}

}  // namespace

Model LoadModelFromFile(const std::string& path) {
  auto model = loadGltfModel(path);
  auto meshes = parseMeshes(model);
  if (meshes.empty()) {
    throw std::runtime_error("No renderable primitives found in glTF file");
  }

  return Model{std::move(meshes)};
}

GltfLoadResult LoadModelWithSource(const std::string& path) {
  auto gltfModel = loadGltfModel(path);

  auto meshes = parseMeshes(gltfModel);
  if (meshes.empty()) {
    throw std::runtime_error("No renderable primitives found in glTF file");
  }

  return GltfLoadResult{Model{std::move(meshes)}, std::move(gltfModel)};
}

}  // namespace gltf
}  // namespace geometry
