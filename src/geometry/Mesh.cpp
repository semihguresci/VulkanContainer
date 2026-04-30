#include <Container/geometry/Mesh.h>

namespace container::geometry {

Mesh::Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
           int32_t materialIndex, bool disableBackfaceCulling)
    : vertices_(std::move(vertices)),
      indices_(std::move(indices)),
      materialIndex_(materialIndex),
      disableBackfaceCulling_(disableBackfaceCulling) {}

}  // namespace container::geometry

