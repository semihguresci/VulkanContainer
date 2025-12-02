#pragma once

#include <Container/geometry/Vertex.h>

#include <vector>

namespace geometry {

class Mesh {
 public:
  Mesh() = default;
  Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);

  const std::vector<Vertex>& vertices() const { return vertices_; }
  const std::vector<uint32_t>& indices() const { return indices_; }
  bool empty() const { return vertices_.empty() || indices_.empty(); }

 private:
  std::vector<Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
};

}  // namespace geometry

