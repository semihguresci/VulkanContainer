#include <Container/geometry/Model.h>

#include <Container/geometry/GltfModelLoader.h>

#include <glm/geometric.hpp>

#include <utility>
#include <vector>

namespace container::geometry {
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

Vertex makeFaceVertex(const glm::vec3& position, const glm::vec2& texCoord,
                      const glm::vec3& normal, const glm::vec3& tangent) {
  Vertex vertex{};
  const glm::vec3 safeNormal = glm::normalize(normal);
  glm::vec3 safeTangent = tangent - safeNormal * glm::dot(safeNormal, tangent);
  if (glm::dot(safeTangent, safeTangent) < 1e-8f) {
    const glm::vec3 axis = std::abs(safeNormal.y) < 0.999f
                               ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
    safeTangent = glm::cross(axis, safeNormal);
  }
  safeTangent = glm::normalize(safeTangent);
  vertex.position = position;
  vertex.color = glm::vec3(1.0f);
  vertex.texCoord = texCoord;
  vertex.normal = safeNormal;
  vertex.tangent = glm::vec4(safeTangent, 1.0f);
  return vertex;
}

void appendQuadFace(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                    const glm::vec3& p0, const glm::vec3& p1,
                    const glm::vec3& p2, const glm::vec3& p3,
                    const glm::vec3& normal, const glm::vec3& tangent) {
  const uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
  vertices.push_back(
      makeFaceVertex(p0, glm::vec2(0.0f, 0.0f), normal, tangent));
  vertices.push_back(
      makeFaceVertex(p1, glm::vec2(1.0f, 0.0f), normal, tangent));
  vertices.push_back(
      makeFaceVertex(p2, glm::vec2(1.0f, 1.0f), normal, tangent));
  vertices.push_back(
      makeFaceVertex(p3, glm::vec2(0.0f, 1.0f), normal, tangent));

  indices.insert(indices.end(),
                 {baseIndex, baseIndex + 1, baseIndex + 2, baseIndex,
                  baseIndex + 2, baseIndex + 3});
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

Model::Model(std::vector<Mesh> meshes) : meshes_(std::move(meshes)) {
  flattenMeshes();
}

void Model::flattenMeshes() {
  vertices_.clear();
  indices_.clear();
  primitiveRanges_.clear();

  vertices_.reserve(countVertices(meshes_));
  indices_.reserve(countIndices(meshes_));
  primitiveRanges_.reserve(meshes_.size());

  for (const auto& mesh : meshes_) {
    const uint32_t firstIndex = static_cast<uint32_t>(indices_.size());
    appendFlattenedMesh(mesh, vertices_, indices_);
    primitiveRanges_.push_back(
        {firstIndex, static_cast<uint32_t>(mesh.indices().size()),
         mesh.materialIndex()});
  }
}

Model Model::LoadFromGltf(const std::string& path) {
  return gltf::LoadModelFromFile(path);
}

Model Model::MakeCube() {
  std::vector<Vertex> verts;
  verts.reserve(24);
  std::vector<uint32_t> idx;
  idx.reserve(36);

  // Right-handed cube with outward normals and CCW winding when viewed from
  // outside. The runtime's Vulkan Y-flip is handled by the camera projection,
  // not by rewriting mesh winding here.
  appendQuadFace(verts, idx,
                 {.5f, -.5f, .5f}, {.5f, -.5f, -.5f},
                 {.5f, .5f, -.5f}, {.5f, .5f, .5f},
                 {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  appendQuadFace(verts, idx,
                 {-.5f, -.5f, -.5f}, {-.5f, -.5f, .5f},
                 {-.5f, .5f, .5f}, {-.5f, .5f, -.5f},
                 {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
  appendQuadFace(verts, idx,
                 {-.5f, .5f, .5f}, {.5f, .5f, .5f},
                 {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                 {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(verts, idx,
                 {-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f},
                 {.5f, -.5f, .5f}, {-.5f, -.5f, .5f},
                 {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(verts, idx,
                 {-.5f, -.5f, .5f}, {.5f, -.5f, .5f},
                 {.5f, .5f, .5f}, {-.5f, .5f, .5f},
                 {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(verts, idx,
                 {.5f, -.5f, -.5f}, {-.5f, -.5f, -.5f},
                 {-.5f, .5f, -.5f}, {.5f, .5f, -.5f},
                 {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f});

  std::vector<Mesh> meshes;
  meshes.emplace_back(verts, idx);
  return Model{std::move(meshes)};
}

}  // namespace container::geometry
