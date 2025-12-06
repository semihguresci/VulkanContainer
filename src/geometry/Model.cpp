#include <Container/geometry/Model.h>

#include <tiny_gltf.h>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace geometry {
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
  const size_t stride = accessor.ByteStride(view);
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  const auto* element = reinterpret_cast<const float*>(dataStart + index * stride);
  return glm::vec3(element[0], element[1], element[2]);
}

std::vector<uint32_t> readIndices(const tinygltf::Model& model,
                                  const tinygltf::Primitive& primitive,
                                  size_t vertexCount) {
  if (primitive.indices < 0) {
    std::vector<uint32_t> sequential(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) sequential[i] = static_cast<uint32_t>(i);
    return sequential;
  }

  const auto& accessor = model.accessors[primitive.indices];
  const auto& view = model.bufferViews[accessor.bufferView];
  const auto& buffer = model.buffers[view.buffer];
  const auto* dataStart = buffer.data.data() + view.byteOffset + accessor.byteOffset;
  const size_t stride = accessor.ByteStride(view);

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

std::vector<Vertex> mergeAttributes(const tinygltf::Model& model,
                                    const tinygltf::Primitive& primitive) {
  auto positionIt = primitive.attributes.find("POSITION");
  if (positionIt == primitive.attributes.end()) {
    throw std::runtime_error("glTF primitive is missing POSITION attribute");
  }

  const auto& positionAccessor = model.accessors[positionIt->second];
  if (positionAccessor.type != TINYGLTF_TYPE_VEC3 ||
      positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
    throw std::runtime_error("POSITION must be a VEC3 float attribute");
  }

  const auto& positionView = model.bufferViews[positionAccessor.bufferView];
  const auto& positionBuffer = model.buffers[positionView.buffer];

  std::vector<Vertex> vertices(positionAccessor.count);
  for (size_t i = 0; i < positionAccessor.count; ++i) {
    vertices[i].position = readVec3(positionAccessor, positionView, positionBuffer, i);
  }

  auto colorIt = primitive.attributes.find("COLOR_0");
  if (colorIt != primitive.attributes.end()) {
    const auto& colorAccessor = model.accessors[colorIt->second];
    const auto& colorView = model.bufferViews[colorAccessor.bufferView];
    const auto& colorBuffer = model.buffers[colorView.buffer];
    const size_t stride = colorAccessor.ByteStride(colorView);
    const auto* dataStart =
        colorBuffer.data.data() + colorView.byteOffset + colorAccessor.byteOffset;

    for (size_t i = 0; i < colorAccessor.count && i < vertices.size(); ++i) {
      const auto* element = dataStart + i * stride;
      if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
          colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        const auto* value = reinterpret_cast<const float*>(element);
        vertices[i].color = glm::vec3(value[0], value[1], value[2]);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        const auto* value = reinterpret_cast<const float*>(element);
        vertices[i].color = glm::vec3(value[0], value[1], value[2]);
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

    const auto& texCoordView = model.bufferViews[texCoordAccessor.bufferView];
    const auto& texCoordBuffer = model.buffers[texCoordView.buffer];
    const size_t stride = texCoordAccessor.ByteStride(texCoordView);
    const auto* dataStart =
        texCoordBuffer.data.data() + texCoordView.byteOffset + texCoordAccessor.byteOffset;

    for (size_t i = 0; i < texCoordAccessor.count && i < vertices.size(); ++i) {
      const auto* element = dataStart + i * stride;
      const auto* value = reinterpret_cast<const float*>(element);
      vertices[i].texCoord = glm::vec2(value[0], value[1]);
    }
  }

  return vertices;
}

}  // namespace

Model::Model(std::vector<Mesh> meshes) : meshes_(std::move(meshes)) { flattenMeshes(); }

void Model::flattenMeshes() {
  vertices_.clear();
  indices_.clear();

  for (const auto& mesh : meshes_) {
    const uint32_t vertexOffset = static_cast<uint32_t>(vertices_.size());
    vertices_.insert(vertices_.end(), mesh.vertices().begin(), mesh.vertices().end());
    auto adjustedIndices = mesh.indices();
    for (auto& index : adjustedIndices) {
      index += vertexOffset;
    }
    indices_.insert(indices_.end(), adjustedIndices.begin(), adjustedIndices.end());
  }
}

Model Model::LoadFromGltf(const std::string& path) {
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

  std::vector<Mesh> meshes;
  for (const auto& mesh : model.meshes) {
    for (const auto& primitive : mesh.primitives) {
      auto vertices = mergeAttributes(model, primitive);
      auto indices = readIndices(model, primitive, vertices.size());
      meshes.emplace_back(std::move(vertices), std::move(indices));
    }
  }

  if (meshes.empty()) {
    throw std::runtime_error("No renderable primitives found in glTF file");
  }

  return Model{std::move(meshes)};
}

Model Model::MakeCube() {
  const std::vector<Vertex> cubeVertices = {
      {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
      {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
      {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
      {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
      {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
      {{0.5f, -0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
      {{0.5f, 0.5f, 0.5f}, {1.0f, 0.5f, 0.2f}, {1.0f, 1.0f}},
      {{-0.5f, 0.5f, 0.5f}, {0.2f, 0.8f, 0.5f}, {0.0f, 1.0f}}};

  const std::vector<uint32_t> cubeIndices = {4, 5, 6, 6, 7, 4, 0, 3, 2, 2, 1, 0,
                                             0, 4, 7, 7, 3, 0, 5, 1, 2, 2, 6, 5,
                                             3, 7, 6, 6, 2, 3, 0, 1, 5, 5, 4, 0};

  std::vector<Mesh> meshes;
  meshes.emplace_back(cubeVertices, cubeIndices);
  return Model{std::move(meshes)};
}

}  // namespace geometry

