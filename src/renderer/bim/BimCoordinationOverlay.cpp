#include "Container/renderer/bim/BimCoordinationOverlay.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace container::renderer {

namespace {

constexpr glm::vec3 kSpaceOverlayColor{0.18f, 0.58f, 0.95f};
constexpr glm::vec3 kMepXrayOverlayColor{0.95f, 0.56f, 0.14f};
constexpr glm::vec3 kClashMarkerColor{1.0f, 0.16f, 0.05f};

[[nodiscard]] std::string normalizedCoordinationText(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character) != 0) {
      normalized.push_back(static_cast<char>(std::tolower(character)));
    }
  }
  return normalized;
}

[[nodiscard]] std::string combinedElementClassificationText(
    const BimCoordinationOverlayElement &element) {
  std::string text;
  text.reserve(element.ifcClass.size() + element.type.size() + 1u);
  text.append(element.ifcClass);
  text.push_back(' ');
  text.append(element.type);
  return text;
}

[[nodiscard]] bool finiteVec3(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool validBounds(const BimCoordinationOverlayBounds &bounds) {
  return bounds.valid && finiteVec3(bounds.min) && finiteVec3(bounds.max) &&
         bounds.max.x >= bounds.min.x && bounds.max.y >= bounds.min.y &&
         bounds.max.z >= bounds.min.z;
}

[[nodiscard]] glm::vec3 boundsCenter(
    const BimCoordinationOverlayBounds &bounds) {
  return (bounds.min + bounds.max) * 0.5f;
}

[[nodiscard]] const std::string &firstNonEmptyLabel(
    const BimCoordinationOverlayElement &element) {
  if (!element.name.empty()) {
    return element.name;
  }
  if (!element.type.empty()) {
    return element.type;
  }
  return element.ifcClass;
}

[[nodiscard]] std::string fallbackElementLabel(
    const BimCoordinationOverlayElement &element,
    std::string_view fallbackPrefix) {
  const std::string &label = firstNonEmptyLabel(element);
  if (!label.empty()) {
    return label;
  }
  return std::string(fallbackPrefix) + " " +
         std::to_string(element.objectIndex);
}

[[nodiscard]] const BimCoordinationOverlayElement *findElement(
    std::span<const BimCoordinationOverlayElement> elements,
    uint32_t objectIndex) {
  if (objectIndex == kInvalidBimCoordinationOverlayObjectIndex) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      elements, [objectIndex](const BimCoordinationOverlayElement &element) {
        return element.objectIndex == objectIndex;
      });
  return found == elements.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<glm::vec3> clashPosition(
    const BimCoordinationOverlayClashPair &clash,
    std::span<const BimCoordinationOverlayElement> elements) {
  if (clash.hasPosition && finiteVec3(clash.position)) {
    return clash.position;
  }

  const auto *primary = findElement(elements, clash.primaryObjectIndex);
  const auto *secondary = findElement(elements, clash.secondaryObjectIndex);
  if (primary != nullptr && secondary != nullptr &&
      validBounds(primary->bounds) && validBounds(secondary->bounds)) {
    return (boundsCenter(primary->bounds) + boundsCenter(secondary->bounds)) *
           0.5f;
  }
  if (primary != nullptr && validBounds(primary->bounds)) {
    return boundsCenter(primary->bounds);
  }
  if (secondary != nullptr && validBounds(secondary->bounds)) {
    return boundsCenter(secondary->bounds);
  }
  return std::nullopt;
}

[[nodiscard]] std::string clashLabel(
    const BimCoordinationOverlayClashPair &clash) {
  if (!clash.label.empty()) {
    return clash.label;
  }
  return "Clash " + std::to_string(clash.primaryObjectIndex) + "/" +
         std::to_string(clash.secondaryObjectIndex);
}

[[nodiscard]] std::string issuePinLabel(
    const BimCoordinationOverlayIssuePin &pin) {
  if (!pin.label.empty()) {
    return pin.label;
  }
  return "Issue pin";
}

[[nodiscard]] float sanitizedMarkerRadius(float markerRadius) {
  if (!std::isfinite(markerRadius) || markerRadius <= 0.0f) {
    return 0.25f;
  }
  return std::clamp(markerRadius, 0.01f, 1000.0f);
}

} // namespace

bool isBimCoordinationSpaceClass(std::string_view value) {
  const std::string normalized = normalizedCoordinationText(value);
  return normalized == "ifcspace" || normalized == "space" ||
         normalized.find("space") != std::string::npos;
}

bool isBimCoordinationMepXrayClass(std::string_view value) {
  const std::string normalized = normalizedCoordinationText(value);
  return normalized.find("pipe") != std::string::npos ||
         normalized.find("duct") != std::string::npos ||
         normalized.find("cable") != std::string::npos ||
         normalized.find("service") != std::string::npos ||
         normalized.find("mep") != std::string::npos ||
         normalized.find("flowsegment") != std::string::npos ||
         normalized.find("flowfitting") != std::string::npos ||
         normalized.find("distribution") != std::string::npos;
}

BimCoordinationOverlayResult
buildBimCoordinationOverlay(const BimCoordinationOverlayInputs &inputs) {
  BimCoordinationOverlayResult result{};

  if (inputs.options.spacesEnabled || inputs.options.mepXrayEnabled) {
    result.elementOverlays.reserve(inputs.elements.size());
    for (const BimCoordinationOverlayElement &element : inputs.elements) {
      const std::string classificationText =
          combinedElementClassificationText(element);

      if (inputs.options.spacesEnabled &&
          isBimCoordinationSpaceClass(classificationText) &&
          validBounds(element.bounds)) {
        result.elementOverlays.push_back(
            {.kind = BimCoordinationOverlayKind::Space,
             .objectIndex = element.objectIndex,
             .bounds = element.bounds,
             .color = kSpaceOverlayColor,
             .opacity = 0.22f,
             .transparent = true,
             .label = fallbackElementLabel(element, "Space")});
      }

      if (inputs.options.mepXrayEnabled &&
          isBimCoordinationMepXrayClass(classificationText) &&
          validBounds(element.bounds)) {
        result.elementOverlays.push_back(
            {.kind = BimCoordinationOverlayKind::MepXray,
             .objectIndex = element.objectIndex,
             .bounds = element.bounds,
             .color = kMepXrayOverlayColor,
             .opacity = 0.42f,
             .transparent = true,
             .label = fallbackElementLabel(element, "MEP")});
      }
    }
  }

  if (inputs.options.clashesEnabled) {
    result.markers.reserve(result.markers.size() + inputs.clashPairs.size());
    for (const BimCoordinationOverlayClashPair &clash : inputs.clashPairs) {
      if (clash.primaryObjectIndex ==
              kInvalidBimCoordinationOverlayObjectIndex ||
          clash.secondaryObjectIndex ==
              kInvalidBimCoordinationOverlayObjectIndex) {
        continue;
      }
      const std::optional<glm::vec3> position =
          clashPosition(clash, inputs.elements);
      if (!position) {
        continue;
      }
      result.markers.push_back(
          {.kind = BimCoordinationOverlayKind::Clash,
           .position = *position,
           .color = kClashMarkerColor,
           .label = clashLabel(clash),
           .primaryObjectIndex = clash.primaryObjectIndex,
           .secondaryObjectIndex = clash.secondaryObjectIndex});
    }
  }

  if (inputs.options.issuePinsEnabled) {
    result.markers.reserve(result.markers.size() + inputs.issuePins.size());
    for (const BimCoordinationOverlayIssuePin &pin : inputs.issuePins) {
      if (!finiteVec3(pin.position) || !finiteVec3(pin.color)) {
        continue;
      }
      result.markers.push_back(
          {.kind = BimCoordinationOverlayKind::IssuePin,
           .position = pin.position,
           .color = pin.color,
           .label = issuePinLabel(pin),
           .ifcGuid = pin.ifcGuid,
           .sourceId = pin.sourceId,
           .primaryObjectIndex = pin.primaryObjectIndex,
           .secondaryObjectIndex = pin.secondaryObjectIndex});
    }
  }

  return result;
}

std::vector<BimCoordinationOverlayMarkerWirePrimitive>
buildBimCoordinationOverlayMarkerWirePrimitives(
    std::span<const BimCoordinationOverlayMarker> markers,
    float markerRadius) {
  const float radius = sanitizedMarkerRadius(markerRadius);
  std::vector<BimCoordinationOverlayMarkerWirePrimitive> primitives;
  primitives.reserve(markers.size());

  for (const BimCoordinationOverlayMarker &marker : markers) {
    if (!finiteVec3(marker.position) || !finiteVec3(marker.color)) {
      continue;
    }

    BimCoordinationOverlayMarkerWirePrimitive primitive{};
    primitive.kind = marker.kind;
    primitive.color = marker.color;
    primitive.label = marker.label;
    primitive.primaryObjectIndex = marker.primaryObjectIndex;
    primitive.secondaryObjectIndex = marker.secondaryObjectIndex;
    primitive.positions = {
        marker.position + glm::vec3{0.0f, radius, 0.0f},
        marker.position - glm::vec3{0.0f, radius, 0.0f},
        marker.position + glm::vec3{radius, 0.0f, 0.0f},
        marker.position - glm::vec3{radius, 0.0f, 0.0f},
        marker.position + glm::vec3{0.0f, 0.0f, radius},
        marker.position - glm::vec3{0.0f, 0.0f, radius},
    };
    primitive.indices = {
        0u, 2u, 4u, 0u, 4u, 3u, 0u, 3u, 5u, 0u, 5u, 2u,
        1u, 4u, 2u, 1u, 3u, 4u, 1u, 5u, 3u, 1u, 2u, 5u,
    };
    primitives.push_back(std::move(primitive));
  }

  return primitives;
}

} // namespace container::renderer
