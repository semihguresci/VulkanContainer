#include "Container/renderer/CameraController.h"
#include "Container/ecs/World.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace container::renderer {

using container::gpu::CameraData;

namespace {

container::ui::TransformControls decomposeTransform(const glm::mat4 &matrix) {
  container::ui::TransformControls controls{};
  controls.position = glm::vec3(matrix[3]);

  glm::vec3 basisX = glm::vec3(matrix[0]);
  glm::vec3 basisY = glm::vec3(matrix[1]);
  glm::vec3 basisZ = glm::vec3(matrix[2]);
  controls.scale =
      glm::vec3(glm::length(basisX), glm::length(basisY), glm::length(basisZ));
  controls.scale = glm::max(controls.scale, glm::vec3(0.001f));

  glm::mat3 rotationMatrix{};
  rotationMatrix[0] = basisX / controls.scale.x;
  rotationMatrix[1] = basisY / controls.scale.y;
  rotationMatrix[2] = basisZ / controls.scale.z;
  if (glm::determinant(rotationMatrix) < 0.0f) {
    controls.scale.x *= -1.0f;
    rotationMatrix[0] *= -1.0f;
  }

  controls.rotationDegrees = glm::degrees(
      glm::eulerAngles(glm::normalize(glm::quat_cast(rotationMatrix))));
  return controls;
}

glm::mat4 composeTransform(const container::ui::TransformControls &controls) {
  const glm::vec3 safeScale = glm::max(controls.scale, glm::vec3(0.001f));
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), controls.position);
  transform *=
      glm::mat4_cast(glm::quat(glm::radians(controls.rotationDegrees)));
  transform = glm::scale(transform, safeScale);
  return transform;
}

float selectedNodeRadius(const container::scene::SceneManager *sceneManager) {
  if (sceneManager && sceneManager->modelBounds().valid) {
    return std::max(0.25f, sceneManager->modelBounds().radius * 0.10f);
  }
  return 1.0f;
}

} // namespace

CameraController::CameraController(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager &allocationManager,
    container::gpu::SwapChainManager &swapChainManager,
    container::scene::SceneGraph &sceneGraph,
    container::scene::SceneManager *sceneManager, container::ecs::World &world,
    container::window::InputManager &inputManager)
    : device_(std::move(device)), allocationManager_(allocationManager),
      swapChainManager_(swapChainManager), sceneGraph_(sceneGraph),
      sceneManager_(sceneManager), world_(world), inputManager_(inputManager) {}

void CameraController::createCamera() {
  auto perspective = std::make_unique<container::scene::PerspectiveCamera>();
  camera_ = std::move(perspective);
  inputManager_.setCamera(camera_.get());
  inputManager_.setMouseSensitivity(0.10f);
  resetCameraForScene();
}

void CameraController::resetCameraForScene() {
  if (!camera_)
    return;
  camera_->setScale(glm::vec3(1.0f));
  inputManager_.setMoveSpeed(kDefaultMoveSpeed);

  auto *perspective =
      dynamic_cast<container::scene::PerspectiveCamera *>(camera_.get());
  if (perspective) {
    float farPlane = kDefaultFarPlane;
    bool hasBounds = false;
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius = 1.0f;

    if (sceneManager_) {
      const auto &bounds = sceneManager_->modelBounds();
      if (bounds.valid) {
        hasBounds = true;
        boundsCenter = bounds.center;
        boundsRadius = bounds.radius;
        farPlane =
            std::max(farPlane, glm::length(boundsCenter) + boundsRadius * 4.0f);
      }
    }
    perspective->setNearFar(kDefaultNearPlane, farPlane);

    if (hasBounds) {
      // For hall-like scenes (e.g. Sponza), start inside the volume looking
      // down the longest horizontal axis. For roughly cubic scenes, fall back
      // to the default three-quarter overview.
      const auto &bounds = sceneManager_->modelBounds();
      const glm::vec3 sz = bounds.size;
      const bool xIsLong = sz.x >= sz.z;
      const float longExtent = xIsLong ? sz.x : sz.z;
      const float crossExtent = xIsLong ? sz.z : sz.x;
      const bool hallLike =
          longExtent > 1.25f * crossExtent && longExtent > 1.5f * sz.y;

      if (hallLike) {
        // RH: front.x = cos(yaw)cos(pitch), front.z = -sin(yaw)cos(pitch).
        // Look +X => yaw = 0; -X => 180; -Z => 90; +Z => -90.
        const float yaw = xIsLong ? 180.0f : 90.0f;
        camera_->setYawPitch(yaw, 0.0f);
        const float eyeHeight = bounds.min.y + sz.y * 0.32f;
        const float endInset = longExtent * 0.10f;
        glm::vec3 eye = boundsCenter;
        eye.y = eyeHeight;
        if (xIsLong) {
          eye.x = bounds.max.x - endInset;
        } else {
          eye.z = bounds.max.z - endInset;
        }
        camera_->setPosition(eye);
      } else {
        camera_->setYawPitch(kDefaultYaw, kDefaultPitch);
        const float distance = boundsRadius * 2.5f + kDefaultNearPlane;
        const glm::vec3 front = camera_->frontVector();
        camera_->setPosition(boundsCenter - front * distance);
      }
      inputManager_.setMoveSpeed(
          std::max(kDefaultMoveSpeed,
                   hallLike ? longExtent * 0.08f : boundsRadius * 0.5f));
    } else {
      camera_->setYawPitch(90.0f, 0.0f);
      camera_->setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
    }
  } else {
    camera_->setYawPitch(kDefaultYaw, kDefaultPitch);
    camera_->setPosition(glm::vec3(0.0f));
  }
}

CameraController::ViewPivot
CameraController::resolveViewPivot(uint32_t nodeIndex) const {
  if (nodeIndex != container::scene::SceneGraph::kInvalidNode) {
    if (const auto *node = sceneGraph_.getNode(nodeIndex)) {
      return ViewPivot{glm::vec3(node->worldTransform[3]),
                       selectedNodeRadius(sceneManager_), true};
    }
  }

  if (sceneManager_ && sceneManager_->modelBounds().valid) {
    const auto &bounds = sceneManager_->modelBounds();
    return ViewPivot{bounds.center, std::max(bounds.radius, 0.25f), true};
  }

  if (camera_) {
    return ViewPivot{camera_->position() + camera_->frontVector() * 5.0f, 1.0f,
                     true};
  }

  return {};
}

void CameraController::lookAt(const glm::vec3 &target) {
  if (!camera_) {
    return;
  }

  glm::vec3 forward = target - camera_->position();
  const float length = glm::length(forward);
  if (length <= 0.0001f) {
    return;
  }
  forward /= length;

  const float pitchDegrees =
      glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  const float yawDegrees = glm::degrees(std::atan2(-forward.z, forward.x));
  camera_->setYawPitch(yawDegrees, pitchDegrees);
}

void CameraController::frameNodeOrScene(uint32_t nodeIndex) {
  if (!camera_) {
    return;
  }

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  if (!pivot.valid) {
    resetCameraForScene();
    return;
  }

  const float distance = std::max(kDefaultNearPlane * 4.0f,
                                  pivot.radius * 2.5f + kDefaultNearPlane);
  camera_->setPosition(pivot.center - camera_->frontVector() * distance);
  lookAt(pivot.center);
  inputManager_.setMoveSpeed(std::max(kDefaultMoveSpeed, pivot.radius * 0.5f));
}

void CameraController::orbit(uint32_t nodeIndex, float deltaX, float deltaY,
                             float sensitivityScale) {
  if (!camera_ || (deltaX == 0.0f && deltaY == 0.0f)) {
    return;
  }

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  if (!pivot.valid) {
    return;
  }

  const float distance =
      std::max(glm::length(camera_->position() - pivot.center),
               kDefaultNearPlane * 4.0f);
  const float sensitivity = 0.20f * std::max(sensitivityScale, 0.01f);
  camera_->setYawPitch(camera_->yawDegrees() + deltaX * sensitivity,
                       camera_->pitchDegrees() + deltaY * sensitivity);
  camera_->setPosition(pivot.center - camera_->frontVector() * distance);
  lookAt(pivot.center);
}

void CameraController::pan(uint32_t nodeIndex, float deltaX, float deltaY,
                           float speedScale) {
  if (!camera_ || (deltaX == 0.0f && deltaY == 0.0f)) {
    return;
  }

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  const float distance =
      pivot.valid ? glm::length(camera_->position() - pivot.center)
                  : std::max(inputManager_.moveSpeed(), kDefaultMoveSpeed);
  const float speed = std::max(distance, 1.0f) * 0.0015f *
                      std::max(speedScale, 0.01f);
  const glm::vec3 front = camera_->frontVector();
  const glm::vec3 up = camera_->upVector(front);
  const glm::vec3 right = camera_->rightVector(front, up);
  camera_->move(-right, deltaX * speed);
  camera_->move(-up, deltaY * speed);
}

void CameraController::dolly(uint32_t nodeIndex, float wheelSteps,
                             float speedScale) {
  if (!camera_ || wheelSteps == 0.0f) {
    return;
  }

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  if (pivot.valid) {
    const glm::vec3 toPivot = pivot.center - camera_->position();
    const float distance = glm::length(toPivot);
    if (distance > kDefaultNearPlane * 4.0f) {
      const float exponent = wheelSteps * std::max(speedScale, 0.01f);
      const float nextDistance =
          std::clamp(distance * std::pow(0.85f, exponent),
                     kDefaultNearPlane * 4.0f, kDefaultFarPlane);
      camera_->setPosition(pivot.center - (toPivot / distance) * nextDistance);
      return;
    }
  }

  float navigationScale = kDefaultMoveSpeed;
  if (sceneManager_ && sceneManager_->modelBounds().valid) {
    navigationScale =
        std::max(navigationScale, sceneManager_->modelBounds().radius * 0.15f);
  }
  const float distance =
      wheelSteps * std::max(navigationScale, inputManager_.moveSpeed()) *
      std::max(speedScale, 0.01f) * 0.25f;
  camera_->move(camera_->frontVector(), distance);
}

void CameraController::adjustMoveSpeed(float wheelSteps) {
  if (wheelSteps == 0.0f) {
    return;
  }

  const float multiplier = std::pow(1.15f, wheelSteps);
  const float nextSpeed =
      std::clamp(inputManager_.moveSpeed() * multiplier, 0.001f, 100000.0f);
  inputManager_.setMoveSpeed(nextSpeed);
}

float CameraController::moveSpeed() const { return inputManager_.moveSpeed(); }

void CameraController::updateCameraBuffer(
    container::gpu::CameraData &outCameraData,
    const container::gpu::AllocatedBuffer &cameraBuffer) const {
  if (!camera_)
    return;
  const auto extent = swapChainManager_.extent();
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  outCameraData.viewProj = camera_->viewProjection(aspect);
  outCameraData.inverseViewProj = glm::inverse(outCameraData.viewProj);
  outCameraData.cameraWorldPosition = glm::vec4(camera_->position(), 1.0f);
  outCameraData.cameraForward = glm::vec4(camera_->frontVector(), 0.0f);

  float nearPlane = kDefaultNearPlane;
  float farPlane = kDefaultFarPlane;
  if (const auto *perspective =
          dynamic_cast<const container::scene::PerspectiveCamera *>(
              camera_.get())) {
    nearPlane = perspective->nearPlane();
    farPlane = perspective->farPlane();
  } else if (const auto *orthographic =
                 dynamic_cast<const container::scene::OrthographicCamera *>(
                     camera_.get())) {
    nearPlane = orthographic->nearPlane();
    farPlane = orthographic->farPlane();
  }
  (void)world_.setActiveCamera(outCameraData, nearPlane, farPlane);

  if (cameraBuffer.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, cameraBuffer,
                                   &outCameraData,
                                   sizeof(container::gpu::CameraData));
  }
}

container::ui::TransformControls
CameraController::cameraTransformControls() const {
  container::ui::TransformControls controls{};
  if (!camera_)
    return controls;
  controls.position = camera_->position();
  controls.rotationDegrees = {camera_->pitchDegrees(), camera_->yawDegrees(),
                              0.0f};
  controls.scale = camera_->scale();
  return controls;
}

container::ui::TransformControls
CameraController::nodeTransformControls(uint32_t nodeIndex) const {
  if (const auto *node = sceneGraph_.getNode(nodeIndex)) {
    return decomposeTransform(node->localTransform);
  }
  return {};
}

void CameraController::applyCameraTransform(
    const container::ui::TransformControls &controls,
    container::gpu::CameraData &outCameraData,
    const container::gpu::AllocatedBuffer &cameraBuffer) {
  if (!camera_)
    return;
  camera_->setPosition(controls.position);
  camera_->setYawPitch(controls.rotationDegrees.y, controls.rotationDegrees.x);
  camera_->setScale(controls.scale);
  updateCameraBuffer(outCameraData, cameraBuffer);
}

void CameraController::applyNodeTransform(
    uint32_t nodeIndex, uint32_t rootNode,
    const container::ui::TransformControls &controls) {
  if (sceneGraph_.getNode(nodeIndex) == nullptr)
    return;
  sceneGraph_.setLocalTransform(nodeIndex, composeTransform(controls));
  sceneGraph_.updateWorldTransforms();
  // Caller is responsible for triggering updateLightingData when rootNode
  // changes.
  (void)rootNode;
}

void CameraController::selectMeshNode(uint32_t nodeIndex,
                                      uint32_t &selectedMeshNode) const {
  selectedMeshNode = sceneGraph_.getNode(nodeIndex) != nullptr
                         ? nodeIndex
                         : container::scene::SceneGraph::kInvalidNode;
}

} // namespace container::renderer
