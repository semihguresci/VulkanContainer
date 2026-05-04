#pragma once

#include "Container/scene/SceneProvider.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct RasterDrawBatchDesc {
  container::scene::SceneProviderId providerId{};
  std::size_t opaqueBatchCount{0};
  std::size_t transparentBatchCount{0};
  bool usesGpuVisibility{false};
};

struct RayTracingGeometryBuildInput {
  container::scene::SceneProviderId providerId{};
  std::size_t triangleGeometryCount{0};
  std::size_t instanceCount{0};
  uint64_t geometryRevision{0};
  bool hasOpaqueGeometry{false};
};

struct SplattingDispatchInput {
  container::scene::SceneProviderId providerId{};
  std::size_t splatCount{0};
  uint64_t geometryRevision{0};
  bool requiresDepthComposite{true};
};

struct RadianceFieldDispatchInput {
  container::scene::SceneProviderId providerId{};
  uint32_t fieldCount{0};
  uint64_t geometryRevision{0};
  bool usesOccupancyData{false};
};

class MeshRasterExtractor {
 public:
  virtual ~MeshRasterExtractor() = default;

  [[nodiscard]] virtual RasterDrawBatchDesc extract(
      const container::scene::SceneProviderSnapshot& provider) const = 0;
};

class BimRasterExtractor {
 public:
  virtual ~BimRasterExtractor() = default;

  [[nodiscard]] virtual RasterDrawBatchDesc extract(
      const container::scene::SceneProviderSnapshot& provider) const = 0;
};

class RayTracingSceneExtractor {
 public:
  virtual ~RayTracingSceneExtractor() = default;

  [[nodiscard]] virtual std::vector<RayTracingGeometryBuildInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const = 0;
};

class SplattingExtractor {
 public:
  virtual ~SplattingExtractor() = default;

  [[nodiscard]] virtual std::vector<SplattingDispatchInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const = 0;
};

class RadianceFieldExtractor {
 public:
  virtual ~RadianceFieldExtractor() = default;

  [[nodiscard]] virtual std::vector<RadianceFieldDispatchInput> extract(
      const std::vector<container::scene::SceneProviderSnapshot>& providers)
      const = 0;
};

}  // namespace container::renderer
