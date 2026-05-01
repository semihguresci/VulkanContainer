#include "Container/renderer/DebugUiPresenter.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::renderer {

namespace {

container::ui::RenderPassToggle* findRenderPassToggle(
    std::vector<container::ui::RenderPassToggle>& toggles,
    RenderPassId id) {
  const std::string_view name = renderPassName(id);
  for (auto& toggle : toggles) {
    if (toggle.name == name) return &toggle;
  }
  return nullptr;
}

const container::ui::RenderPassToggle* findRenderPassToggle(
    const std::vector<container::ui::RenderPassToggle>& toggles,
    RenderPassId id) {
  const std::string_view name = renderPassName(id);
  for (const auto& toggle : toggles) {
    if (toggle.name == name) return &toggle;
  }
  return nullptr;
}

std::string dependencyNoteForPass(
    const std::vector<container::ui::RenderPassToggle>& toggles,
    RenderPassId id) {
  for (RenderPassId dependencyId : renderPassDependencies(id)) {
    const auto* dependency = findRenderPassToggle(toggles, dependencyId);
    if (dependency != nullptr && !dependency->enabled) {
      return "requires " + std::string(renderPassName(dependencyId));
    }
  }
  return {};
}

std::string executionNoteForPass(const RenderGraph& graph, RenderPassId id) {
  if (id == RenderPassId::Invalid) return {};

  const auto* status = graph.executionStatus(id);
  if (status == nullptr || status->active) return {};

  switch (status->skipReason) {
    case RenderPassSkipReason::Disabled:
    case RenderPassSkipReason::None:
      return {};
    case RenderPassSkipReason::MissingPassDependency:
      return "inactive: requires " +
             std::string(renderPassName(status->blockingPass));
    case RenderPassSkipReason::MissingResource:
      return "inactive: missing " +
             std::string(renderResourceName(status->blockingResource));
    case RenderPassSkipReason::MissingRecordCallback:
      return "inactive: no recorder";
  }

  return {};
}

bool enforceRenderPassDependencies(
    std::vector<container::ui::RenderPassToggle>& toggles) {
  bool changed = false;

  for (auto& toggle : toggles) {
    const RenderPassId id = renderPassIdFromName(toggle.name);
    if (isProtectedRenderPass(id) && !toggle.enabled) {
      toggle.enabled = true;
      changed = true;
    }
  }

  bool madeProgress = true;
  while (madeProgress) {
    madeProgress = false;

    for (auto& toggle : toggles) {
      if (!toggle.enabled) continue;
      const RenderPassId id = renderPassIdFromName(toggle.name);
      for (RenderPassId dependencyId : renderPassDependencies(id)) {
        const auto* dependency = findRenderPassToggle(toggles, dependencyId);
        if (dependency != nullptr && !dependency->enabled) {
          toggle.enabled = false;
          changed = true;
          madeProgress = true;
          break;
        }
      }
    }
  }

  return changed;
}

}  // namespace

void DebugUiPresenter::publishRenderPasses(
    container::ui::GuiManager& guiManager,
    const RenderGraph& graph) {
  std::vector<container::ui::RenderPassToggle> passList;
  passList.reserve(graph.passes().size());
  for (const auto& node : graph.passes()) {
    const std::string executionNote = executionNoteForPass(graph, node.id);
    container::ui::RenderPassToggle toggle{};
    toggle.name = node.name;
    toggle.enabled = node.enabled;
    toggle.locked = isProtectedRenderPass(node.id);
    toggle.autoDisabled = !executionNote.empty();
    toggle.dependencyNote = executionNote;
    passList.push_back(std::move(toggle));
  }
  guiManager.setRenderPassList(passList);
}

bool DebugUiPresenter::applyRenderPassToggles(
    container::ui::GuiManager& guiManager,
    RenderGraph& graph) {
  auto& toggles = guiManager.renderPassToggles();
  const bool correctedDependencies = enforceRenderPassDependencies(toggles);

  for (const auto& toggle : toggles) {
    graph.setPassEnabled(renderPassIdFromName(toggle.name), toggle.enabled);
  }
  for (auto& toggle : toggles) {
    const RenderPassId id = renderPassIdFromName(toggle.name);
    toggle.locked = isProtectedRenderPass(id);
    const std::string dependencyNote = dependencyNoteForPass(toggles, id);
    const std::string executionNote = executionNoteForPass(graph, id);
    toggle.autoDisabled = !dependencyNote.empty() || !executionNote.empty();
    toggle.dependencyNote =
        !dependencyNote.empty() ? dependencyNote : executionNote;
  }

  return correctedDependencies;
}

}  // namespace container::renderer
