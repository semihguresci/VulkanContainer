#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declaration — no Vulkan header needed here.
struct VkCommandBuffer_T;
using VkCommandBuffer = VkCommandBuffer_T*;

namespace container::renderer {

struct FrameRecordParams;

// A single node in the render graph.
// Each node wraps a callable that records commands for one logical pass.
struct RenderPassNode {
  std::string name;
  bool        enabled{true};

  // The recording callback.  Receives the command buffer and the current
  // per-frame params built by RendererFrontend.
  using RecordFn = std::function<void(VkCommandBuffer, const FrameRecordParams&)>;
  RecordFn record;
};

// Ordered sequence of render pass nodes.
// Nodes are executed front-to-back; disabled nodes are skipped.
//
// The graph does **not** perform topological sorting or automatic barrier
// insertion — the caller (FrameRecorder) registers passes in the correct
// dependency order and relies on Vulkan render-pass subpass dependencies
// for synchronisation between attachments.
class RenderGraph {
 public:
  RenderGraph() = default;

  // Append a pass to the end of the execution list.
  // Returns a reference to the newly added node.  Note: the reference is
  // invalidated by subsequent addPass() calls (vector reallocation).
  // Use findPass() for deferred access.
  RenderPassNode& addPass(std::string name, RenderPassNode::RecordFn fn);

  // Execute all enabled passes in registration order.
  void execute(VkCommandBuffer cmd, const FrameRecordParams& params) const;

  // Access a pass by name (linear search — graph is small).
  // Returns nullptr if not found.
  RenderPassNode*       findPass(const std::string& name);
  const RenderPassNode* findPass(const std::string& name) const;

  // Number of registered passes.
  [[nodiscard]] uint32_t passCount() const;

  // Number of currently enabled passes.
  [[nodiscard]] uint32_t enabledPassCount() const;

  // Direct access to the pass list for iteration.
  [[nodiscard]] std::vector<RenderPassNode>&       passes()       { return passes_; }
  [[nodiscard]] const std::vector<RenderPassNode>& passes() const { return passes_; }

  // Remove all passes.
  void clear();

 private:
  std::vector<RenderPassNode> passes_;
};

}  // namespace container::renderer
