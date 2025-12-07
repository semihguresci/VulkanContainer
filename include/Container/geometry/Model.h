#pragma once

#include <Container/geometry/Mesh.h>

#include <string>
#include <vector>

namespace geometry {

class Model;

namespace gltf {
struct GltfLoadResult;
Model LoadModelFromFile(const std::string& path);
GltfLoadResult LoadModelWithSource(const std::string& path);
}

class Model {
 public:
  Model() = default;
  Model(const Model&) = default;
  Model(Model&&) noexcept = default;
  Model& operator=(const Model&) = default;
  Model& operator=(Model&&) noexcept = default;

  static Model LoadFromGltf(const std::string& path);
  static Model MakeCube();

  const std::vector<Mesh>& meshes() const { return meshes_; }
  const std::vector<Vertex>& vertices() const { return vertices_; }
  const std::vector<uint32_t>& indices() const { return indices_; }
 bool empty() const { return vertices_.empty() || indices_.empty(); }

 private:
  friend Model gltf::LoadModelFromFile(const std::string& path);
  friend gltf::GltfLoadResult gltf::LoadModelWithSource(const std::string& path);

  explicit Model(std::vector<Mesh> meshes);
  void flattenMeshes();

  std::vector<Mesh> meshes_{};
  std::vector<Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
};

}  // namespace geometry

