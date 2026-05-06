#pragma once

#include "Container/renderer/bim/BimManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::renderer {

enum class BimMetadataSemanticCategory : uint8_t {
  Type,
  Storey,
  Material,
  Discipline,
  Phase,
  FireRating,
  LoadBearing,
  Status,
};

class BimMetadataCatalog {
public:
  void clear();
  void clearLabels();
  void reserve(size_t objectCount);

  void setModelUnitMetadata(BimModelUnitMetadata metadata);
  void setModelGeoreferenceMetadata(BimModelGeoreferenceMetadata metadata);

  [[nodiscard]] const BimModelUnitMetadata &modelUnitMetadata() const;
  [[nodiscard]] const BimModelGeoreferenceMetadata &
  modelGeoreferenceMetadata() const;

  [[nodiscard]] const std::vector<std::string> &types() const;
  [[nodiscard]] const std::vector<std::string> &storeys() const;
  [[nodiscard]] const std::vector<std::string> &materials() const;
  [[nodiscard]] const std::vector<std::string> &disciplines() const;
  [[nodiscard]] const std::vector<std::string> &phases() const;
  [[nodiscard]] const std::vector<std::string> &fireRatings() const;
  [[nodiscard]] const std::vector<std::string> &loadBearingValues() const;
  [[nodiscard]] const std::vector<std::string> &statuses() const;
  [[nodiscard]] const std::vector<BimStoreyRange> &storeyRanges() const;

  [[nodiscard]] uint32_t registerType(std::string label);
  void registerStorey(const BimElementMetadata &metadata,
                      const BimElementBounds &bounds);
  void registerMaterial(const BimElementMetadata &metadata);
  void registerDiscipline(std::string label);
  void registerPhase(std::string label);
  void registerFireRating(std::string label);
  void registerLoadBearing(std::string label);
  void registerStatus(std::string label);
  void sortStoreyRanges();

  [[nodiscard]] uint32_t
  semanticIdForCategory(BimMetadataSemanticCategory category,
                        std::string_view label) const;
  [[nodiscard]] uint32_t
  semanticIdForMetadata(const BimElementMetadata &metadata,
                        BimSemanticColorMode mode) const;
  [[nodiscard]] BimVisibilityGpuObjectMetadata
  visibilityGpuMetadata(const BimElementMetadata &metadata) const;

private:
  uint32_t registerUniqueLabel(std::unordered_map<std::string, uint32_t> &ids,
                               std::vector<std::string> &values,
                               std::string label);

  std::vector<std::string> types_{};
  std::vector<std::string> storeys_{};
  std::vector<std::string> materials_{};
  std::vector<std::string> disciplines_{};
  std::vector<std::string> phases_{};
  std::vector<std::string> fireRatings_{};
  std::vector<std::string> loadBearingValues_{};
  std::vector<std::string> statuses_{};
  std::vector<BimStoreyRange> storeyRanges_{};

  std::unordered_map<std::string, uint32_t> typeIds_{};
  std::unordered_map<std::string, uint32_t> storeyIds_{};
  std::unordered_map<std::string, uint32_t> materialIds_{};
  std::unordered_map<std::string, uint32_t> disciplineIds_{};
  std::unordered_map<std::string, uint32_t> phaseIds_{};
  std::unordered_map<std::string, uint32_t> fireRatingIds_{};
  std::unordered_map<std::string, uint32_t> loadBearingIds_{};
  std::unordered_map<std::string, uint32_t> statusIds_{};
  std::unordered_map<std::string, size_t> storeyRangeIndices_{};

  BimModelUnitMetadata modelUnitMetadata_{};
  BimModelGeoreferenceMetadata modelGeoreferenceMetadata_{};
};

[[nodiscard]] std::string
bimMetadataStoreyLabel(const BimElementMetadata &metadata);
[[nodiscard]] std::string
bimMetadataMaterialLabel(const BimElementMetadata &metadata);

} // namespace container::renderer
