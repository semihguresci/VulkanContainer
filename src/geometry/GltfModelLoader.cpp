#include <Container/geometry/GltfModelLoader.h>

#include <Container/geometry/Mesh.h>

#include <tiny_gltf.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cctype>
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

std::vector<uint32_t> readIndices(const tinygltf::Model& model,
                                  const tinygltf::Primitive& primitive,
                                  size_t vertexCount) {
  if (primitive.indices < 0) {
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

std::vector<Vertex> mergeAttributes(const tinygltf::Model& model,
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

  std::vector<Vertex> vertices(positionAccessor.count);
  for (size_t i = 0; i < positionAccessor.count; ++i) {
    vertices[i].position = readVec3(positionAccessor, positionView, positionBuffer, i);
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

    if (colorAccessor.count < vertices.size()) {
      throw std::runtime_error("COLOR_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
      const auto* element = dataStart + i * stride;
      if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
          colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        const auto* value = reinterpret_cast<const float*>(element);
        vertices[i].color = glm::vec3(value[0], value[1], value[2]);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        const auto* value = reinterpret_cast<const float*>(element);
        vertices[i].color = glm::vec3(value[0], value[1], value[2]);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint8_t*>(element);
        vertices[i].color = glm::vec3(value[0], value[1], value[2]) / 255.0f;
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint8_t*>(element);
        vertices[i].color = glm::vec3(value[0], value[1], value[2]) / 255.0f;
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC3 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint16_t*>(element);
        vertices[i].color =
            glm::vec3(value[0], value[1], value[2]) / glm::vec3(65535.0f);
      } else if (colorAccessor.type == TINYGLTF_TYPE_VEC4 &&
                 colorAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                 colorAccessor.normalized) {
        const auto* value = reinterpret_cast<const uint16_t*>(element);
        vertices[i].color =
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

    if (texCoordAccessor.count < vertices.size()) {
      throw std::runtime_error("TEXCOORD_0 attribute count is smaller than POSITION count");
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
      const auto* element = dataStart + i * stride;
      const auto* value = reinterpret_cast<const float*>(element);
      vertices[i].texCoord = glm::vec2(value[0], value[1]);
    }
  }

  return vertices;
}

std::vector<Mesh> parseMeshes(const tinygltf::Model& model) {
  std::vector<Mesh> meshes;
  for (const auto& mesh : model.meshes) {
    for (const auto& primitive : mesh.primitives) {
      auto vertices = mergeAttributes(model, primitive);
      auto indices = readIndices(model, primitive, vertices.size());
      meshes.emplace_back(std::move(vertices), std::move(indices));
    }
  }
  return meshes;
}

}  // namespace

Model LoadModelFromFile(const std::string& path) {
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

  auto meshes = parseMeshes(model);
  if (meshes.empty()) {
    throw std::runtime_error("No renderable primitives found in glTF file");
  }

  return Model{std::move(meshes)};
}

}  // namespace gltf
}  // namespace geometry

