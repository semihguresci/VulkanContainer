#pragma once

#include <Container/geometry/Model.h>

#include <tiny_gltf.h>

#include <string>

namespace geometry {

struct GltfLoadResult {
  Model model;
  tinygltf::Model gltfModel;
};

namespace gltf {

// Responsible solely for reading glTF assets and converting them into
// runtime geometry structures. Keeps file I/O away from Model.
Model LoadModelFromFile(const std::string& path);

GltfLoadResult LoadModelWithSource(const std::string& path);

}  // namespace gltf
}  // namespace geometry

