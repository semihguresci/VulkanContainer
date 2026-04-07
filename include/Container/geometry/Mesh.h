#pragma once

#include <Container/geometry/Vertex.h>

#include <vector>

namespace geometry {

class Mesh {
 public:
  Mesh() = default;
  Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
       int32_t materialIndex = -1);

  [[nodiscard]] const std::vector<Vertex>& vertices() const { return vertices_; }
  [[nodiscard]] const std::vector<uint32_t>& indices() const { return indices_; }
  [[nodiscard]] int32_t materialIndex() const { return materialIndex_; }
  [[nodiscard]] bool empty() const { return vertices_.empty() || indices_.empty(); }

 private:
  std::vector<Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  int32_t materialIndex_{-1};
};

}  // namespace geometry

