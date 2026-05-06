#include "Container/renderer/bim/BimMetadataIndex.h"

#include <limits>

namespace container::renderer {

void BimMetadataIndex::clear() {
  objectIndicesByGuid_.clear();
  objectIndicesBySourceId_.clear();
}

void BimMetadataIndex::reserve(size_t objectCount) {
  objectIndicesByGuid_.reserve(objectCount);
  objectIndicesBySourceId_.reserve(objectCount);
}

void BimMetadataIndex::index(const BimElementMetadata &metadata) {
  if (metadata.objectIndex == std::numeric_limits<uint32_t>::max()) {
    return;
  }
  if (!metadata.guid.empty()) {
    objectIndicesByGuid_[metadata.guid].push_back(metadata.objectIndex);
  }
  if (!metadata.sourceId.empty()) {
    objectIndicesBySourceId_[metadata.sourceId].push_back(metadata.objectIndex);
  }
}

std::span<const uint32_t>
BimMetadataIndex::objectIndicesForGuid(std::string_view guid) const {
  if (guid.empty()) {
    return {};
  }
  const auto it = objectIndicesByGuid_.find(guid);
  if (it == objectIndicesByGuid_.end()) {
    return {};
  }
  return {it->second.data(), it->second.size()};
}

std::span<const uint32_t>
BimMetadataIndex::objectIndicesForSourceId(std::string_view sourceId) const {
  if (sourceId.empty()) {
    return {};
  }
  const auto it = objectIndicesBySourceId_.find(sourceId);
  if (it == objectIndicesBySourceId_.end()) {
    return {};
  }
  return {it->second.data(), it->second.size()};
}

} // namespace container::renderer
