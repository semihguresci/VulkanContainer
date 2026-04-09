#include <Container/geometry/Model.h>

#include <Container/geometry/GltfModelLoader.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <cmath>
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

bool isFiniteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
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

Model Model::LoadFromGltf(const std::string& path) { return gltf::LoadModelFromFile(path); }

Model Model::MakeCube() {
  // 24 vertices (4 per face) with correct per-face world normals and tangents.
  // Winding order: CCW when viewed from outside (standard glTF / OpenGL).
  // Tangent w=+1 throughout (no mirror faces).
  //
  // Expected normal-visualisation colors (normal*0.5+0.5):
  //   +X face → (1.0, 0.5, 0.5)  reddish
  //   -X face → (0.0, 0.5, 0.5)  cyan
  //   +Y face → (0.5, 1.0, 0.5)  green
  //   -Y face → (0.5, 0.0, 0.5)  magenta
  //   +Z face → (0.5, 0.5, 1.0)  blue
  //   -Z face → (0.5, 0.5, 0.0)  yellow

  using V = Vertex;
  // Helper: build one face vertex
  // pos, uv, normal, tangent are all in RH/glTF convention;
  // the LH conversion (toLeftHandedTransform on the scene node) will handle
  // the coordinate flip at the scene-graph level.
  std::vector<Vertex> verts;
  verts.reserve(24);

  const auto face = [&](glm::vec3 n, glm::vec3 t,
                         glm::vec3 p0, glm::vec3 p1,
                         glm::vec3 p2, glm::vec3 p3) {
    glm::vec4 tan{t, 1.0f};
    verts.push_back({p0, {1,1,1}, {0,0}, n, tan});
    verts.push_back({p1, {1,1,1}, {1,0}, n, tan});
    verts.push_back({p2, {1,1,1}, {1,1}, n, tan});
    verts.push_back({p3, {1,1,1}, {0,1}, n, tan});
  };

  // +X  normal=(1,0,0)  tangent=(0,0,-1)
  face({1,0,0}, {0,0,-1},
       { .5f,-.5f, .5f}, { .5f,-.5f,-.5f},
       { .5f, .5f,-.5f}, { .5f, .5f, .5f});

  // -X  normal=(-1,0,0)  tangent=(0,0,1)
  face({-1,0,0}, {0,0,1},
       {-.5f,-.5f,-.5f}, {-.5f,-.5f, .5f},
       {-.5f, .5f, .5f}, {-.5f, .5f,-.5f});

  // +Y  normal=(0,1,0)  tangent=(1,0,0)
  face({0,1,0}, {1,0,0},
       {-.5f, .5f, .5f}, { .5f, .5f, .5f},
       { .5f, .5f,-.5f}, {-.5f, .5f,-.5f});

  // -Y  normal=(0,-1,0)  tangent=(1,0,0)
  face({0,-1,0}, {1,0,0},
       {-.5f,-.5f,-.5f}, { .5f,-.5f,-.5f},
       { .5f,-.5f, .5f}, {-.5f,-.5f, .5f});

  // +Z  normal=(0,0,1)  tangent=(1,0,0)
  face({0,0,1}, {1,0,0},
       {-.5f,-.5f, .5f}, { .5f,-.5f, .5f},
       { .5f, .5f, .5f}, {-.5f, .5f, .5f});

  // -Z  normal=(0,0,-1)  tangent=(-1,0,0)
  face({0,0,-1}, {-1,0,0},
       { .5f,-.5f,-.5f}, {-.5f,-.5f,-.5f},
       {-.5f, .5f,-.5f}, { .5f, .5f,-.5f});

  // Two triangles per face, CCW
  std::vector<uint32_t> idx;
  idx.reserve(36);
  for (uint32_t f = 0; f < 6; ++f) {
    const uint32_t b = f * 4;
    idx.insert(idx.end(), {b,b+1,b+2, b,b+2,b+3});
  }

  alignNormalsAndTangentsToWinding(verts, idx);

  std::vector<Mesh> meshes;
  meshes.emplace_back(verts, idx);
  return Model{std::move(meshes)};
}

}  // namespace geometry

