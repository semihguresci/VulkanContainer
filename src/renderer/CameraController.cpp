#include "Container/renderer/CameraController.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <glm/gtc/quaternion.hpp>

namespace container::renderer {

using container::gpu::CameraData;

namespace {

container::ui::TransformControls decomposeTransform(const glm::mat4& matrix) {
  container::ui::TransformControls controls{};
  controls.position = glm::vec3(matrix[3]);

  glm::vec3 basisX = glm::vec3(matrix[0]);
  glm::vec3 basisY = glm::vec3(matrix[1]);
  glm::vec3 basisZ = glm::vec3(matrix[2]);
  controls.scale = glm::vec3(glm::length(basisX), glm::length(basisY),
                             glm::length(basisZ));
  controls.scale = glm::max(controls.scale, glm::vec3(0.001f));

  glm::mat3 rotationMatrix{};
  rotationMatrix[0] = basisX / controls.scale.x;
  rotationMatrix[1] = basisY / controls.scale.y;
  rotationMatrix[2] = basisZ / controls.scale.z;
  if (glm::determinant(rotationMatrix) < 0.0f) {
    controls.scale.x    *= -1.0f;
    rotationMatrix[0]   *= -1.0f;
  }

  controls.rotationDegrees = glm::degrees(
      glm::eulerAngles(glm::normalize(glm::quat_cast(rotationMatrix))));
  return controls;
}

glm::mat4 composeTransform(const container::ui::TransformControls& controls) {
  const glm::vec3 safeScale = glm::max(controls.scale, glm::vec3(0.001f));
  glm::mat4 transform =
      glm::translate(glm::mat4(1.0f), controls.position);
  transform *=
      glm::mat4_cast(glm::quat(glm::radians(controls.rotationDegrees)));
  transform = glm::scale(transform, safeScale);
  return transform;
}

}  // namespace

CameraController::CameraController(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::SwapChainManager&                     swapChainManager,
    container::scene::SceneGraph&                    sceneGraph,
    container::scene::SceneManager*                  sceneManager,
    container::window::InputManager&                  inputManager)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , swapChainManager_(swapChainManager)
    , sceneGraph_(sceneGraph)
    , sceneManager_(sceneManager)
    , inputManager_(inputManager) {
}

void CameraController::createCamera() {
  auto perspective = std::make_unique<container::scene::PerspectiveCamera>();
  camera_ = std::move(perspective);
  inputManager_.setCamera(camera_.get());
  inputManager_.setMouseSensitivity(0.10f);
  resetCameraForScene();
}

void CameraController::resetCameraForScene() {
  if (!camera_) return;
  camera_->setScale(glm::vec3(1.0f));
  inputManager_.setMoveSpeed(kDefaultMoveSpeed);

  auto* perspective =
      dynamic_cast<container::scene::PerspectiveCamera*>(camera_.get());
  if (perspective) {
    float     farPlane    = kDefaultFarPlane;
    bool      hasBounds   = false;
    glm::vec3 boundsCenter{0.0f};
    float     boundsRadius = 1.0f;

    if (sceneManager_) {
      const auto& bounds = sceneManager_->modelBounds();
      if (bounds.valid) {
        hasBounds     = true;
        boundsCenter  = bounds.center;
        boundsRadius  = bounds.radius;
        farPlane      = std::max(farPlane,
                                 glm::length(boundsCenter) + boundsRadius * 4.0f);
      }
    }
    perspective->setNearFar(kDefaultNearPlane, farPlane);

    if (hasBounds) {
      // For elongated scenes (e.g. Sponza), frame the camera at the far end of
      // the longest horizontal axis looking toward the center so the whole
      // hall is visible. For roughly cubic scenes, fall back to the default
      // three-quarter view.
      const auto& bounds = sceneManager_->modelBounds();
      const glm::vec3 sz = bounds.size;
      const float horizExtent = std::max(sz.x, sz.z);
      const bool elongated = sz.x > 2.0f * std::max(sz.y, sz.z) ||
                             sz.z > 2.0f * std::max(sz.y, sz.x);

      if (elongated) {
        const bool xIsLong = sz.x >= sz.z;
        // RH: front.x = cos(yaw)cos(pitch), front.z = -sin(yaw)cos(pitch).
        // Look +X => yaw = 180; -X => 0; -Z => 90; +Z => -90.
        const float yaw = xIsLong ? 180.0f : 90.0f;
        camera_->setYawPitch(yaw, 0.0f);
        const float eyeHeight =
            boundsCenter.y + sz.y * 0.15f;
        const float halfLong = 0.5f * (xIsLong ? sz.x : sz.z);
        // Position at the far end plus a small offset to include the end wall.
        glm::vec3 eye = boundsCenter;
        eye.y = eyeHeight;
        if (xIsLong) {
          eye.x = bounds.max.x + horizExtent * 0.05f;
        } else {
          eye.z = bounds.max.z + horizExtent * 0.05f;
        }
        camera_->setPosition(eye);
      } else {
        camera_->setYawPitch(kDefaultYaw, kDefaultPitch);
        const float    distance = boundsRadius * 2.5f + kDefaultNearPlane;
        const glm::vec3 front   = camera_->frontVector();
        camera_->setPosition(boundsCenter - front * distance);
      }
      inputManager_.setMoveSpeed(
          std::max(kDefaultMoveSpeed, boundsRadius * 0.5f));
    } else {
      camera_->setYawPitch(90.0f, 0.0f);
      camera_->setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
    }
  } else {
    camera_->setYawPitch(kDefaultYaw, kDefaultPitch);
    camera_->setPosition(glm::vec3(0.0f));
  }
}

void CameraController::updateCameraBuffer(
    container::gpu::CameraData&                              outCameraData,
    const container::gpu::AllocatedBuffer&  cameraBuffer) const {
  if (!camera_) return;
  const auto  extent = swapChainManager_.extent();
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  outCameraData.viewProj          = camera_->viewProjection(aspect);
  outCameraData.inverseViewProj   = glm::inverse(outCameraData.viewProj);
  outCameraData.cameraWorldPosition =
      glm::vec4(camera_->position(), 1.0f);

  if (cameraBuffer.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, cameraBuffer,
                                   &outCameraData, sizeof(container::gpu::CameraData));
  }
}

container::ui::TransformControls CameraController::cameraTransformControls() const {
  container::ui::TransformControls controls{};
  if (!camera_) return controls;
  controls.position       = camera_->position();
  controls.rotationDegrees = {camera_->pitchDegrees(), camera_->yawDegrees(), 0.0f};
  controls.scale           = camera_->scale();
  return controls;
}

container::ui::TransformControls
CameraController::nodeTransformControls(uint32_t nodeIndex) const {
  if (const auto* node = sceneGraph_.getNode(nodeIndex)) {
    return decomposeTransform(node->localTransform);
  }
  return {};
}

void CameraController::applyCameraTransform(
    const container::ui::TransformControls&    controls,
    container::gpu::CameraData&                              outCameraData,
    const container::gpu::AllocatedBuffer&  cameraBuffer) {
  if (!camera_) return;
  camera_->setPosition(controls.position);
  camera_->setYawPitch(controls.rotationDegrees.y, controls.rotationDegrees.x);
  camera_->setScale(controls.scale);
  updateCameraBuffer(outCameraData, cameraBuffer);
}

void CameraController::applyNodeTransform(
    uint32_t                               nodeIndex,
    uint32_t                               rootNode,
    const container::ui::TransformControls&  controls) {
  if (sceneGraph_.getNode(nodeIndex) == nullptr) return;
  sceneGraph_.setLocalTransform(nodeIndex, composeTransform(controls));
  sceneGraph_.updateWorldTransforms();
  // Caller is responsible for triggering updateLightingData when rootNode changes.
  (void)rootNode;
}

void CameraController::selectMeshNode(uint32_t nodeIndex,
                                      uint32_t& selectedMeshNode) const {
  selectedMeshNode = sceneGraph_.getNode(nodeIndex) != nullptr
                         ? nodeIndex
                         : container::scene::SceneGraph::kInvalidNode;
}

}  // namespace container::renderer
