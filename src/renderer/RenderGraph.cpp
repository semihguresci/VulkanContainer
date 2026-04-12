#include "Container/renderer/RenderGraph.h"

#include <algorithm>

namespace container::renderer {

RenderPassNode& RenderGraph::addPass(std::string name,
                                     RenderPassNode::RecordFn fn) {
  passes_.push_back({std::move(name), true, std::move(fn)});
  return passes_.back();
}

void RenderGraph::execute(VkCommandBuffer cmd,
                          const FrameRecordParams& params) const {
  for (const auto& pass : passes_) {
    if (pass.enabled && pass.record) {
      pass.record(cmd, params);
    }
  }
}

RenderPassNode* RenderGraph::findPass(const std::string& name) {
  auto it = std::ranges::find(passes_, name, &RenderPassNode::name);
  return it != passes_.end() ? &*it : nullptr;
}

const RenderPassNode* RenderGraph::findPass(const std::string& name) const {
  auto it = std::ranges::find(passes_, name, &RenderPassNode::name);
  return it != passes_.end() ? &*it : nullptr;
}

uint32_t RenderGraph::passCount() const {
  return static_cast<uint32_t>(passes_.size());
}

uint32_t RenderGraph::enabledPassCount() const {
  return static_cast<uint32_t>(
      std::ranges::count_if(passes_, &RenderPassNode::enabled));
}

void RenderGraph::clear() { passes_.clear(); }

}  // namespace container::renderer
