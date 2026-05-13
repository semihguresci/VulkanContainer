#include "Container/renderer/bim/BimModelCompare.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {

[[nodiscard]] bool finiteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool validBounds(const BimModelCompareBounds& bounds) {
  return bounds.valid && finiteVec3(bounds.min) && finiteVec3(bounds.max) &&
         bounds.max.x >= bounds.min.x && bounds.max.y >= bounds.min.y &&
         bounds.max.z >= bounds.min.z;
}

[[nodiscard]] std::string identityKey(const BimModelCompareElement& element) {
  return !element.guid.empty() ? element.guid : element.sourceId;
}

[[nodiscard]] std::string identityValue(
    const BimModelCompareElement& element) {
  return identityKey(element);
}

[[nodiscard]] std::string typeValue(const BimModelCompareElement& element) {
  if (!element.ifcClass.empty() && !element.type.empty()) {
    return element.ifcClass + " / " + element.type;
  }
  if (!element.type.empty()) {
    return element.type;
  }
  return element.ifcClass;
}

struct IndexedElement {
  size_t index{0};
  const BimModelCompareElement* element{nullptr};
};

struct MatchedPair {
  IndexedElement before{};
  IndexedElement after{};
  std::string identity{};
  size_t occurrence{1};
};

struct MatchPlan {
  std::vector<MatchedPair> pairs{};
  std::vector<IndexedElement> unmatchedBefore{};
  std::vector<IndexedElement> unmatchedAfter{};
};

using IdentityGroups = std::map<std::string, std::vector<size_t>>;

[[nodiscard]] IdentityGroups groupByGuid(
    std::span<const BimModelCompareElement> elements) {
  IdentityGroups groups;
  for (size_t index = 0; index < elements.size(); ++index) {
    if (elements[index].guid.empty()) {
      continue;
    }
    groups[elements[index].guid].push_back(index);
  }
  return groups;
}

void appendIndexedElement(std::vector<IndexedElement>& output,
                          std::span<const BimModelCompareElement> elements,
                          size_t index) {
  output.push_back({.index = index, .element = &elements[index]});
}

void appendOccurrenceMatches(MatchPlan& plan,
                             std::span<const BimModelCompareElement> before,
                             std::span<const BimModelCompareElement> after,
                             std::string_view identity,
                             const std::vector<size_t>& beforeIndices,
                             const std::vector<size_t>& afterIndices,
                             std::vector<bool>& beforeMatched,
                             std::vector<bool>& afterMatched) {
  const size_t matchedCount = std::min(beforeIndices.size(),
                                       afterIndices.size());
  for (size_t occurrence = 0; occurrence < matchedCount; ++occurrence) {
    const size_t beforeIndex = beforeIndices[occurrence];
    const size_t afterIndex = afterIndices[occurrence];
    beforeMatched[beforeIndex] = true;
    afterMatched[afterIndex] = true;
    plan.pairs.push_back({.before = {.index = beforeIndex,
                                     .element = &before[beforeIndex]},
                          .after = {.index = afterIndex,
                                    .element = &after[afterIndex]},
                          .identity = std::string(identity),
                          .occurrence = occurrence + 1u});
  }
}

void matchByGuid(MatchPlan& plan,
                 std::span<const BimModelCompareElement> before,
                 std::span<const BimModelCompareElement> after,
                 std::vector<bool>& beforeMatched,
                 std::vector<bool>& afterMatched) {
  const IdentityGroups beforeGroups = groupByGuid(before);
  const IdentityGroups afterGroups = groupByGuid(after);
  for (const auto& [guid, beforeIndices] : beforeGroups) {
    const auto afterFound = afterGroups.find(guid);
    if (afterFound == afterGroups.end()) {
      continue;
    }
    appendOccurrenceMatches(plan, before, after, guid, beforeIndices,
                            afterFound->second, beforeMatched, afterMatched);
  }
}

[[nodiscard]] IdentityGroups groupUnmatchedBySourceId(
    std::span<const BimModelCompareElement> elements,
    const std::vector<bool>& matched) {
  IdentityGroups groups;
  for (size_t index = 0; index < elements.size(); ++index) {
    if (matched[index] || elements[index].sourceId.empty()) {
      continue;
    }
    groups[elements[index].sourceId].push_back(index);
  }
  return groups;
}

void matchBySourceId(MatchPlan& plan,
                     std::span<const BimModelCompareElement> before,
                     std::span<const BimModelCompareElement> after,
                     std::vector<bool>& beforeMatched,
                     std::vector<bool>& afterMatched) {
  const IdentityGroups beforeGroups =
      groupUnmatchedBySourceId(before, beforeMatched);
  const IdentityGroups afterGroups = groupUnmatchedBySourceId(after, afterMatched);
  for (const auto& [sourceId, beforeIndices] : beforeGroups) {
    const auto afterFound = afterGroups.find(sourceId);
    if (afterFound == afterGroups.end()) {
      continue;
    }
    appendOccurrenceMatches(plan, before, after, sourceId, beforeIndices,
                            afterFound->second, beforeMatched, afterMatched);
  }
}

[[nodiscard]] MatchPlan matchElements(
    std::span<const BimModelCompareElement> before,
    std::span<const BimModelCompareElement> after) {
  MatchPlan plan;
  std::vector<bool> beforeMatched(before.size(), false);
  std::vector<bool> afterMatched(after.size(), false);
  matchByGuid(plan, before, after, beforeMatched, afterMatched);
  matchBySourceId(plan, before, after, beforeMatched, afterMatched);

  for (size_t index = 0; index < before.size(); ++index) {
    if (!beforeMatched[index] && !identityValue(before[index]).empty()) {
      appendIndexedElement(plan.unmatchedBefore, before, index);
    }
  }
  for (size_t index = 0; index < after.size(); ++index) {
    if (!afterMatched[index] && !identityValue(after[index]).empty()) {
      appendIndexedElement(plan.unmatchedAfter, after, index);
    }
  }
  return plan;
}

[[nodiscard]] size_t occurrenceInSpan(
    std::span<const BimModelCompareElement> elements, size_t index) {
  size_t occurrence = 0;
  const std::string identity = identityValue(elements[index]);
  for (size_t candidate = 0; candidate <= index; ++candidate) {
    if (identityValue(elements[candidate]) == identity) {
      ++occurrence;
    }
  }
  return occurrence;
}

[[nodiscard]] std::string occurrenceIdentity(std::string identity,
                                             size_t occurrence) {
  if (occurrence <= 1u) {
    return identity;
  }
  return identity + "#" + std::to_string(occurrence);
}

[[nodiscard]] bool componentChanged(float before, float after,
                                    float tolerance) {
  return std::abs(before - after) > tolerance;
}

[[nodiscard]] bool boundsChanged(const BimModelCompareBounds& before,
                                 const BimModelCompareBounds& after,
                                 float tolerance) {
  const bool beforeValid = validBounds(before);
  const bool afterValid = validBounds(after);
  if (beforeValid != afterValid) {
    return true;
  }
  if (!beforeValid) {
    return false;
  }
  const float clampedTolerance = std::max(tolerance, 0.0f);
  return componentChanged(before.min.x, after.min.x, clampedTolerance) ||
         componentChanged(before.min.y, after.min.y, clampedTolerance) ||
         componentChanged(before.min.z, after.min.z, clampedTolerance) ||
         componentChanged(before.max.x, after.max.x, clampedTolerance) ||
         componentChanged(before.max.y, after.max.y, clampedTolerance) ||
         componentChanged(before.max.z, after.max.z, clampedTolerance);
}

void appendChange(BimModelCompareResult& result,
                  BimModelCompareChangeKind kind, std::string identity,
                  size_t beforeIndex, size_t afterIndex,
                  std::string beforeValue = {}, std::string afterValue = {}) {
  result.changes.push_back({.kind = kind,
                            .identity = std::move(identity),
                            .beforeIndex = beforeIndex,
                            .afterIndex = afterIndex,
                            .beforeValue = std::move(beforeValue),
                            .afterValue = std::move(afterValue)});
}

} // namespace

BimModelCompareResult compareBimModels(
    std::span<const BimModelCompareElement> before,
    std::span<const BimModelCompareElement> after,
    const BimModelCompareOptions& options) {
  const MatchPlan matchPlan = matchElements(before, after);
  BimModelCompareResult result{};
  for (const MatchedPair& pair : matchPlan.pairs) {
    const BimModelCompareElement& oldElement = *pair.before.element;
    const BimModelCompareElement& newElement = *pair.after.element;
    const std::string identity =
        occurrenceIdentity(pair.identity, pair.occurrence);
    if (typeValue(oldElement) != typeValue(newElement)) {
      appendChange(result, BimModelCompareChangeKind::ChangedType, identity,
                   pair.before.index, pair.after.index, typeValue(oldElement),
                   typeValue(newElement));
    }
    if (oldElement.storey != newElement.storey) {
      appendChange(result, BimModelCompareChangeKind::ChangedStorey, identity,
                   pair.before.index, pair.after.index, oldElement.storey,
                   newElement.storey);
    }
    if (oldElement.material != newElement.material) {
      appendChange(result, BimModelCompareChangeKind::ChangedMaterial, identity,
                   pair.before.index, pair.after.index, oldElement.material,
                   newElement.material);
    }
    if (boundsChanged(oldElement.bounds, newElement.bounds,
                      options.boundsTolerance)) {
      appendChange(result, BimModelCompareChangeKind::ChangedBounds, identity,
                   pair.before.index, pair.after.index);
    }
  }

  for (const IndexedElement& beforeElement : matchPlan.unmatchedBefore) {
    appendChange(result, BimModelCompareChangeKind::Removed,
                 occurrenceIdentity(identityValue(*beforeElement.element),
                                    occurrenceInSpan(before,
                                                     beforeElement.index)),
                 beforeElement.index, std::numeric_limits<size_t>::max());
  }

  for (const IndexedElement& afterElement : matchPlan.unmatchedAfter) {
    appendChange(result, BimModelCompareChangeKind::Added,
                 occurrenceIdentity(identityValue(*afterElement.element),
                                    occurrenceInSpan(after, afterElement.index)),
                 std::numeric_limits<size_t>::max(), afterElement.index);
  }

  return result;
}

} // namespace container::renderer
