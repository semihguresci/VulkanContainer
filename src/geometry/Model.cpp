#include <Container/geometry/Model.h>

#include <Container/geometry/GltfModelLoader.h>

#include <utility>
#include <vector>

namespace geometry {
namespace {

uint32_t countVertices(const std::vector<Mesh>& meshes) {
  uint32_t total = 0;
  for (const auto& mesh : meshes) {
    total += static_cast<uint32_t>(mesh.vertices().size());
  }
  return total;
}

uint32_t countIndices(const std::vector<Mesh>& meshes) {
  uint32_t total = 0;
  for (const auto& mesh : meshes) {
    total += static_cast<uint32_t>(mesh.indices().size());
  }
  return total;
}

void appendFlattenedMesh(const Mesh& mesh, std::vector<Vertex>& vertices,
                         std::vector<uint32_t>& indices) {
  const uint32_t vertexOffset = static_cast<uint32_t>(vertices.size());

  vertices.insert(vertices.end(), mesh.vertices().begin(), mesh.vertices().end());

  indices.reserve(indices.size() + mesh.indices().size());
  for (const auto index : mesh.indices()) {
    indices.push_back(index + vertexOffset);
  }
}

}  // namespace

Model::Model(std::vector<Mesh> meshes) : meshes_(std::move(meshes)) { flattenMeshes(); }

void Model::flattenMeshes() {
  vertices_.clear();
  indices_.clear();

  vertices_.reserve(countVertices(meshes_));
  indices_.reserve(countIndices(meshes_));

  for (const auto& mesh : meshes_) {
    appendFlattenedMesh(mesh, vertices_, indices_);
  }
}

Model Model::LoadFromGltf(const std::string& path) { return gltf::LoadModelFromFile(path); }

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

