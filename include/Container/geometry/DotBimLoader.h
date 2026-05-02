#pragma once

#include "Container/common/CommonMath.h"
#include "Container/geometry/Vertex.h"
#include "Container/utility/Material.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::dotbim {

struct MeshRange {
  uint32_t meshId{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};
};

struct Element {
  uint32_t meshId{0};
  glm::mat4 transform{1.0f};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  std::string guid{};
  std::string type{};
  bool doubleSided{true};
  uint32_t materialIndex{std::numeric_limits<uint32_t>::max()};
};

struct MaterialTextureAsset {
  std::filesystem::path path{};
  std::string name{};
  std::vector<std::byte> encodedBytes{};
  uint32_t samplerIndex{0};

  [[nodiscard]] bool empty() const noexcept {
    return path.empty() && encodedBytes.empty();
  }
};

struct MaterialTexturePaths {
  MaterialTextureAsset baseColor{};
  MaterialTextureAsset normal{};
  MaterialTextureAsset occlusion{};
  MaterialTextureAsset emissive{};
  MaterialTextureAsset metallicRoughness{};
  MaterialTextureAsset roughness{};
  MaterialTextureAsset metalness{};
  MaterialTextureAsset specular{};
  MaterialTextureAsset specularColor{};
  MaterialTextureAsset opacity{};
  MaterialTextureAsset transmission{};
  MaterialTextureAsset clearcoat{};
  MaterialTextureAsset clearcoatRoughness{};
  MaterialTextureAsset clearcoatNormal{};
  MaterialTextureAsset sheenColor{};
  MaterialTextureAsset sheenRoughness{};
  MaterialTextureAsset iridescence{};
  MaterialTextureAsset iridescenceThickness{};
};

struct Material {
  container::material::Material pbr{};
  MaterialTexturePaths texturePaths{};
};

struct Model {
  std::vector<Vertex> vertices{};
  std::vector<uint32_t> indices{};
  std::vector<MeshRange> meshRanges{};
  std::vector<Element> elements{};
  std::vector<Material> materials{};
};

[[nodiscard]] Model LoadFromFile(const std::filesystem::path& path,
                                 float importScale = 1.0f);
[[nodiscard]] Model LoadFromJson(std::string_view jsonText,
                                 float importScale = 1.0f);

}  // namespace container::geometry::dotbim
