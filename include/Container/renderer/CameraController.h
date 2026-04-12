#pragma once

#include "Container/utility/Camera.h"
#include "Container/utility/GuiManager.h"

#include <cstdint>
#include <memory>

namespace container::gpu {
class AllocationManager;
class SwapChainManager;
class VulkanDevice;
struct AllocatedBuffer;
struct CameraData;
}  // namespace container::gpu

namespace container::scene {
class SceneGraph;
class SceneManager;
}  // namespace container::scene

namespace container::window {
class InputManager;
}  // namespace container::window

namespace container::renderer {

// Manages the application camera: creation, reset, update, and transform
// helpers for both camera and scene-graph nodes.
class CameraController {
 public:
  CameraController(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::SwapChainManager&                     swapChainManager,
      container::scene::SceneGraph&                    sceneGraph,
      container::scene::SceneManager*                  sceneManager,
      container::window::InputManager&                  inputManager);

  ~CameraController() = default;
  CameraController(const CameraController&) = delete;
  CameraController& operator=(const CameraController&) = delete;

  // ---- Lifecycle ----------------------------------------------------------

  // Creates PerspectiveCamera and wires it to the input manager.
  void createCamera();

  // Positions the camera relative to the current scene bounds.
  void resetCameraForScene();

  // ---- Per-frame ----------------------------------------------------------

  // Updates cameraData and uploads to cameraBuffer.
  void updateCameraBuffer(
      container::gpu::CameraData&                              outCameraData,
      const container::gpu::AllocatedBuffer&  cameraBuffer) const;

  // ---- Transform helpers --------------------------------------------------

  container::ui::TransformControls cameraTransformControls() const;
  container::ui::TransformControls nodeTransformControls(uint32_t nodeIndex) const;

  void applyCameraTransform(
      const container::ui::TransformControls&    controls,
      container::gpu::CameraData&                              outCameraData,
      const container::gpu::AllocatedBuffer&  cameraBuffer);

  void applyNodeTransform(
      uint32_t                               nodeIndex,
      uint32_t                               rootNode,
      const container::ui::TransformControls&  controls);

  void selectMeshNode(uint32_t nodeIndex, uint32_t& selectedMeshNode) const;

  // ---- Accessors ----------------------------------------------------------

  container::scene::BaseCamera* camera() const { return camera_.get(); }

 private:
  static constexpr float kDefaultYaw         = -135.0f;
  static constexpr float kDefaultPitch        = -35.0f;
  static constexpr float kDefaultMoveSpeed    = 1.0f;
  static constexpr float kDefaultNearPlane    = 0.05f;
  static constexpr float kDefaultFarPlane     = 500.0f;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::SwapChainManager&                     swapChainManager_;
  container::scene::SceneGraph&                    sceneGraph_;
  container::scene::SceneManager*                  sceneManager_{nullptr};
  container::window::InputManager&                  inputManager_;

  std::unique_ptr<container::scene::BaseCamera> camera_;
};

}  // namespace container::renderer
