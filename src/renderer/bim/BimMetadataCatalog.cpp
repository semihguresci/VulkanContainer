#include "Container/renderer/bim/BimMetadataCatalog.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace container::renderer {
namespace {

uint32_t semanticIdFromZeroBased(uint32_t id) {
  return id == std::numeric_limits<uint32_t>::max() ? 0u : id + 1u;
}

uint32_t semanticIdFromLabel(std::string_view label,
                             const std::vector<std::string> &values) {
  if (label.empty()) {
    return 0u;
  }
  const auto it = std::ranges::find(values, label);
  if (it == values.end()) {
    return 0u;
  }
  const size_t offset = static_cast<size_t>(std::distance(values.begin(), it));
  if (offset >= std::numeric_limits<uint32_t>::max()) {
    return 0u;
  }
  return static_cast<uint32_t>(offset + 1u);
}

} // namespace

std::string bimMetadataStoreyLabel(const BimElementMetadata &metadata) {
  if (!metadata.storeyName.empty()) {
    return metadata.storeyName;
  }
  return metadata.storeyId;
}

std::string bimMetadataMaterialLabel(const BimElementMetadata &metadata) {
  if (!metadata.materialName.empty()) {
    return metadata.materialName;
  }
  return metadata.materialCategory;
}

void BimMetadataCatalog::clear() {
  clearLabels();
  modelUnitMetadata_ = {};
  modelGeoreferenceMetadata_ = {};
}

void BimMetadataCatalog::clearLabels() {
  types_.clear();
  storeys_.clear();
  materials_.clear();
  disciplines_.clear();
  phases_.clear();
  fireRatings_.clear();
  loadBearingValues_.clear();
  statuses_.clear();
  storeyRanges_.clear();

  typeIds_.clear();
  storeyIds_.clear();
  materialIds_.clear();
  disciplineIds_.clear();
  phaseIds_.clear();
  fireRatingIds_.clear();
  loadBearingIds_.clear();
  statusIds_.clear();
  storeyRangeIndices_.clear();
}

void BimMetadataCatalog::reserve(size_t objectCount) {
  typeIds_.reserve(objectCount);
  storeyIds_.reserve(objectCount);
  materialIds_.reserve(objectCount);
  disciplineIds_.reserve(objectCount);
  phaseIds_.reserve(objectCount);
  fireRatingIds_.reserve(objectCount);
  loadBearingIds_.reserve(objectCount);
  statusIds_.reserve(objectCount);
  storeyRangeIndices_.reserve(objectCount);
}

void BimMetadataCatalog::setModelUnitMetadata(BimModelUnitMetadata metadata) {
  modelUnitMetadata_ = std::move(metadata);
}

void BimMetadataCatalog::setModelGeoreferenceMetadata(
    BimModelGeoreferenceMetadata metadata) {
  modelGeoreferenceMetadata_ = std::move(metadata);
}

const BimModelUnitMetadata &BimMetadataCatalog::modelUnitMetadata() const {
  return modelUnitMetadata_;
}

const BimModelGeoreferenceMetadata &
BimMetadataCatalog::modelGeoreferenceMetadata() const {
  return modelGeoreferenceMetadata_;
}

const std::vector<std::string> &BimMetadataCatalog::types() const {
  return types_;
}

const std::vector<std::string> &BimMetadataCatalog::storeys() const {
  return storeys_;
}

const std::vector<std::string> &BimMetadataCatalog::materials() const {
  return materials_;
}

const std::vector<std::string> &BimMetadataCatalog::disciplines() const {
  return disciplines_;
}

const std::vector<std::string> &BimMetadataCatalog::phases() const {
  return phases_;
}

const std::vector<std::string> &BimMetadataCatalog::fireRatings() const {
  return fireRatings_;
}

const std::vector<std::string> &BimMetadataCatalog::loadBearingValues() const {
  return loadBearingValues_;
}

const std::vector<std::string> &BimMetadataCatalog::statuses() const {
  return statuses_;
}

const std::vector<BimStoreyRange> &BimMetadataCatalog::storeyRanges() const {
  return storeyRanges_;
}

uint32_t BimMetadataCatalog::registerUniqueLabel(
    std::unordered_map<std::string, uint32_t> &ids,
    std::vector<std::string> &values, std::string label) {
  if (label.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }
  auto [it, inserted] =
      ids.try_emplace(label, static_cast<uint32_t>(ids.size()));
  if (inserted) {
    values.push_back(std::move(label));
  }
  return it->second;
}

uint32_t BimMetadataCatalog::registerType(std::string label) {
  return registerUniqueLabel(typeIds_, types_, std::move(label));
}

void BimMetadataCatalog::registerStorey(const BimElementMetadata &metadata,
                                        const BimElementBounds &bounds) {
  const std::string label = bimMetadataStoreyLabel(metadata);
  if (label.empty()) {
    return;
  }
  registerUniqueLabel(storeyIds_, storeys_, label);
  if (!bounds.valid) {
    return;
  }

  const auto [rangeIt, insertedRange] =
      storeyRangeIndices_.try_emplace(label, storeyRanges_.size());
  if (insertedRange) {
    storeyRanges_.push_back(BimStoreyRange{
        .label = label,
        .minElevation = bounds.min.y,
        .maxElevation = bounds.max.y,
        .objectCount = 1u,
    });
    return;
  }

  BimStoreyRange &range = storeyRanges_[rangeIt->second];
  range.minElevation = std::min(range.minElevation, bounds.min.y);
  range.maxElevation = std::max(range.maxElevation, bounds.max.y);
  ++range.objectCount;
}

void BimMetadataCatalog::registerMaterial(const BimElementMetadata &metadata) {
  registerUniqueLabel(materialIds_, materials_,
                      bimMetadataMaterialLabel(metadata));
}

void BimMetadataCatalog::registerDiscipline(std::string label) {
  registerUniqueLabel(disciplineIds_, disciplines_, std::move(label));
}

void BimMetadataCatalog::registerPhase(std::string label) {
  registerUniqueLabel(phaseIds_, phases_, std::move(label));
}

void BimMetadataCatalog::registerFireRating(std::string label) {
  registerUniqueLabel(fireRatingIds_, fireRatings_, std::move(label));
}

void BimMetadataCatalog::registerLoadBearing(std::string label) {
  registerUniqueLabel(loadBearingIds_, loadBearingValues_, std::move(label));
}

void BimMetadataCatalog::registerStatus(std::string label) {
  registerUniqueLabel(statusIds_, statuses_, std::move(label));
}

void BimMetadataCatalog::sortStoreyRanges() {
  std::ranges::sort(storeyRanges_,
                    [](const BimStoreyRange &lhs, const BimStoreyRange &rhs) {
                      if (lhs.minElevation == rhs.minElevation) {
                        return lhs.label < rhs.label;
                      }
                      return lhs.minElevation < rhs.minElevation;
                    });
}

uint32_t
BimMetadataCatalog::semanticIdForCategory(BimMetadataSemanticCategory category,
                                          std::string_view label) const {
  switch (category) {
  case BimMetadataSemanticCategory::Type:
    return semanticIdFromLabel(label, types_);
  case BimMetadataSemanticCategory::Storey:
    return semanticIdFromLabel(label, storeys_);
  case BimMetadataSemanticCategory::Material:
    return semanticIdFromLabel(label, materials_);
  case BimMetadataSemanticCategory::Discipline:
    return semanticIdFromLabel(label, disciplines_);
  case BimMetadataSemanticCategory::Phase:
    return semanticIdFromLabel(label, phases_);
  case BimMetadataSemanticCategory::FireRating:
    return semanticIdFromLabel(label, fireRatings_);
  case BimMetadataSemanticCategory::LoadBearing:
    return semanticIdFromLabel(label, loadBearingValues_);
  case BimMetadataSemanticCategory::Status:
    return semanticIdFromLabel(label, statuses_);
  }
  return 0u;
}

uint32_t
BimMetadataCatalog::semanticIdForMetadata(const BimElementMetadata &metadata,
                                          BimSemanticColorMode mode) const {
  switch (mode) {
  case BimSemanticColorMode::Type:
  case BimSemanticColorMode::Off:
    return semanticIdFromZeroBased(metadata.semanticTypeId);
  case BimSemanticColorMode::Storey:
    return semanticIdForCategory(BimMetadataSemanticCategory::Storey,
                                 bimMetadataStoreyLabel(metadata));
  case BimSemanticColorMode::Material:
    return semanticIdForCategory(BimMetadataSemanticCategory::Material,
                                 bimMetadataMaterialLabel(metadata));
  case BimSemanticColorMode::FireRating:
    return semanticIdForCategory(BimMetadataSemanticCategory::FireRating,
                                 metadata.fireRating);
  case BimSemanticColorMode::LoadBearing:
    return semanticIdForCategory(BimMetadataSemanticCategory::LoadBearing,
                                 metadata.loadBearing);
  case BimSemanticColorMode::Status:
    return semanticIdForCategory(BimMetadataSemanticCategory::Status,
                                 metadata.status);
  }
  return 0u;
}

BimVisibilityGpuObjectMetadata BimMetadataCatalog::visibilityGpuMetadata(
    const BimElementMetadata &metadata) const {
  BimVisibilityGpuObjectMetadata gpuMetadata{};
  gpuMetadata.semanticIds = {
      semanticIdFromZeroBased(metadata.semanticTypeId),
      semanticIdForCategory(BimMetadataSemanticCategory::Storey,
                            bimMetadataStoreyLabel(metadata)),
      semanticIdForCategory(BimMetadataSemanticCategory::Material,
                            bimMetadataMaterialLabel(metadata)),
      semanticIdForCategory(BimMetadataSemanticCategory::Discipline,
                            metadata.discipline),
  };
  gpuMetadata.propertyIds = {
      semanticIdForCategory(BimMetadataSemanticCategory::Phase, metadata.phase),
      semanticIdForCategory(BimMetadataSemanticCategory::FireRating,
                            metadata.fireRating),
      semanticIdForCategory(BimMetadataSemanticCategory::LoadBearing,
                            metadata.loadBearing),
      semanticIdForCategory(BimMetadataSemanticCategory::Status,
                            metadata.status),
  };
  gpuMetadata.identity = {
      metadata.productIdentityId,
      metadata.objectIndex,
      static_cast<uint32_t>(metadata.geometryKind),
      0u,
  };
  return gpuMetadata;
}

} // namespace container::renderer
