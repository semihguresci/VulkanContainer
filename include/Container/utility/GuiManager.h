#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/renderer/BimSemanticColorMode.h"
#include "Container/renderer/RendererTelemetry.h"
#include "Container/utility/SceneData.h"

struct GLFWwindow;

namespace container::scene {
class SceneGraph;
} // namespace container::scene

namespace container::renderer {
struct BimElementProperty;
struct BimStoreyRange;
struct CullStats;
} // namespace container::renderer

namespace container::ui {

enum class GBufferViewMode : uint32_t {
  Lit = 0,
  Albedo = 1,
  Normals = 2,
  Material = 3,
  Depth = 4,
  Emissive = 5,
  Transparency = 6,
  Revealage = 7,
  Overview = 8,
  SurfaceNormals = 9,
  ObjectSpaceNormals = 10,
  ShadowCascades = 11,
  TileLightHeatMap = 12,
  ShadowTexelDensity = 13,
};

enum class WireframeMode : uint32_t {
  Overlay = 0,
  Full = 1,
};

enum class ViewportTool : uint32_t {
  Select = 0,
  Translate = 1,
  Rotate = 2,
  Scale = 3,
};

enum class TransformSpace : uint32_t {
  Local = 0,
  World = 1,
};

enum class TransformAxis : uint32_t {
  Free = 0,
  X = 1,
  Y = 2,
  Z = 3,
};

enum class ViewportGesture : uint32_t {
  None = 0,
  FlyLook = 1,
  Orbit = 2,
  Pan = 3,
  TransformDrag = 4,
};

enum class ViewportNavigationStyle : uint32_t {
  Revit = 0,
  Blender = 1,
};

enum class CameraProjectionMode : uint32_t {
  Perspective = 0,
  Orthographic = 1,
};

enum class CameraViewPreset : uint32_t {
  Front = 0,
  Back = 1,
  Right = 2,
  Left = 3,
  Top = 4,
  Bottom = 5,
};

struct ViewportInteractionState {
  ViewportTool tool{ViewportTool::Select};
  TransformSpace transformSpace{TransformSpace::Local};
  TransformAxis transformAxis{TransformAxis::Free};
  TransformAxis hoverTransformAxis{TransformAxis::Free};
  ViewportNavigationStyle navigationStyle{ViewportNavigationStyle::Revit};
  ViewportGesture gesture{ViewportGesture::None};
  bool transformSnapEnabled{false};
};

struct ViewportNavigationState {
  CameraProjectionMode projectionMode{CameraProjectionMode::Perspective};
  glm::vec3 cameraRight{1.0f, 0.0f, 0.0f};
  glm::vec3 cameraUp{0.0f, 1.0f, 0.0f};
  glm::vec3 cameraForward{0.0f, 0.0f, -1.0f};
};

struct WireframeSettings {
  bool enabled{false};
  WireframeMode mode{WireframeMode::Overlay};
  bool depthTest{true};
  glm::vec3 color{0.0f, 1.0f, 0.0f};
  float lineWidth{1.0f};
  float overlayIntensity{0.85f};
};

struct TransformControls {
  glm::vec3 position{0.0f, 0.0f, 0.0f};
  glm::vec3 rotationDegrees{0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f, 1.0f, 1.0f};
};

struct RenderPassToggle {
  std::string name;
  bool enabled{true};
  bool locked{false};
  bool autoDisabled{false};
  std::string dependencyNote{};
};

struct BimFilterState {
  bool typeFilterEnabled{false};
  std::string type{};
  bool storeyFilterEnabled{false};
  std::string storey{};
  bool materialFilterEnabled{false};
  std::string material{};
  bool disciplineFilterEnabled{false};
  std::string discipline{};
  bool phaseFilterEnabled{false};
  std::string phase{};
  bool fireRatingFilterEnabled{false};
  std::string fireRating{};
  bool loadBearingFilterEnabled{false};
  std::string loadBearing{};
  bool statusFilterEnabled{false};
  std::string status{};
  bool drawBudgetEnabled{false};
  uint32_t drawBudgetMaxObjects{0};
  bool isolateSelection{false};
  bool hideSelection{false};
};

enum class BimFloorPlanElevationMode : uint32_t {
  ProjectedGround = 0,
  SourceElevation = 1,
};

struct BimFloorPlanOverlayState {
  bool enabled{false};
  bool depthTest{false};
  BimFloorPlanElevationMode elevationMode{
      BimFloorPlanElevationMode::ProjectedGround};
  glm::vec3 color{0.02f, 0.02f, 0.02f};
  float opacity{0.85f};
  float lineWidth{1.0f};
};

struct BimLayerVisibilityState {
  bool pointCloudVisible{true};
  bool curvesVisible{true};
  bool xrayLayerVisible{false};
  bool clashLayerVisible{false};
  bool markupLayerVisible{false};
};

struct BimLodStreamingUiState {
  bool autoLod{true};
  bool drawBudgetEnabled{false};
  int drawBudgetMaxObjects{10000};
  int lodBias{0};
  float screenErrorPixels{2.0f};
  bool pauseStreamingRequest{false};
  bool keepVisibleStoreysResident{false};
};

struct SectionPlaneState {
  bool enabled{false};
  glm::vec3 normal{0.0f, 1.0f, 0.0f};
  float offset{0.0f};
};

struct BimBoxClipUiState {
  bool enabled{false};
  bool invert{false};
  glm::vec3 min{-10.0f, -10.0f, -10.0f};
  glm::vec3 max{10.0f, 10.0f, 10.0f};
};

struct BimClipCapHatchingUiState {
  bool capPreview{false};
  bool hatchingPreview{false};
  glm::vec3 capColor{0.06f, 0.08f, 0.10f};
  float capOpacity{0.82f};
  float hatchSpacing{0.25f};
  float hatchAngleDegrees{45.0f};
  float hatchLineWidth{1.0f};
  glm::vec3 hatchColor{0.08f, 0.08f, 0.08f};
};

struct BimInspectionState {
  bool hasScene{false};
  std::string modelPath{};
  size_t objectCount{0};
  size_t meshObjectCount{0};
  size_t pointObjectCount{0};
  size_t curveObjectCount{0};
  size_t opaqueDrawCount{0};
  size_t transparentDrawCount{0};
  size_t pointOpaqueDrawCount{0};
  size_t pointTransparentDrawCount{0};
  size_t curveOpaqueDrawCount{0};
  size_t curveTransparentDrawCount{0};
  size_t nativePointOpaqueDrawCount{0};
  size_t nativePointTransparentDrawCount{0};
  size_t nativeCurveOpaqueDrawCount{0};
  size_t nativeCurveTransparentDrawCount{0};
  size_t meshletClusterCount{0};
  size_t meshletSourceClusterCount{0};
  size_t meshletEstimatedClusterCount{0};
  size_t meshletObjectReferenceCount{0};
  size_t meshletGpuResidentObjectCount{0};
  size_t meshletGpuResidentClusterCount{0};
  size_t meshletGpuBufferBytes{0};
  bool meshletGpuComputeReady{false};
  bool meshletGpuDispatchPending{false};
  uint32_t meshletMaxLodLevel{0};
  bool optimizedModelMetadataCacheable{false};
  bool optimizedModelMetadataCacheHit{false};
  bool optimizedModelMetadataCacheStale{false};
  bool optimizedModelMetadataCacheWritten{false};
  std::string optimizedModelMetadataCacheKey{};
  std::string optimizedModelMetadataCachePath{};
  std::string optimizedModelMetadataCacheStatus{};
  size_t drawBudgetVisibleObjectCount{0};
  size_t drawBudgetVisibleMeshObjectCount{0};
  size_t drawBudgetVisibleMeshletClusterCount{0};
  uint32_t drawBudgetVisibleMaxLodLevel{0};
  size_t floorPlanDrawCount{0};
  size_t uniqueTypeCount{0};
  size_t uniqueStoreyCount{0};
  size_t uniqueMaterialCount{0};
  size_t uniqueDisciplineCount{0};
  size_t uniquePhaseCount{0};
  size_t uniqueFireRatingCount{0};
  size_t uniqueLoadBearingCount{0};
  size_t uniqueStatusCount{0};
  bool hasSourceUnits{false};
  std::string sourceUnits{};
  bool hasMetersPerUnit{false};
  float metersPerUnit{1.0f};
  bool hasImportScale{false};
  float importScale{1.0f};
  bool hasEffectiveImportScale{false};
  float effectiveImportScale{1.0f};
  bool hasSourceUpAxis{false};
  std::string sourceUpAxis{};
  bool hasCoordinateOffset{false};
  glm::dvec3 coordinateOffset{0.0};
  std::string coordinateOffsetSource{};
  std::string crsName{};
  std::string crsAuthority{};
  std::string crsCode{};
  std::string mapConversionName{};
  std::span<const std::string> elementTypes{};
  std::span<const std::string> elementStoreys{};
  std::span<const std::string> elementMaterials{};
  std::span<const std::string> elementDisciplines{};
  std::span<const std::string> elementPhases{};
  std::span<const std::string> elementFireRatings{};
  std::span<const std::string> elementLoadBearingValues{};
  std::span<const std::string> elementStatuses{};
  std::span<const container::renderer::BimStoreyRange> elementStoreyRanges{};

  bool hasSelection{false};
  uint32_t selectedObjectIndex{0};
  uint32_t sourceElementIndex{0};
  uint32_t meshId{0};
  uint32_t sourceMaterialIndex{0};
  uint32_t materialIndex{0};
  uint32_t semanticTypeId{0};
  glm::vec4 sourceColor{0.8f, 0.82f, 0.86f, 1.0f};
  std::string guid{};
  std::string type{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::string geometryKind{};
  std::span<const container::renderer::BimElementProperty> properties{};
  bool transparent{false};
  bool doubleSided{false};
  bool hasSelectionBounds{false};
  glm::vec3 selectionBoundsMin{0.0f};
  glm::vec3 selectionBoundsMax{0.0f};
  glm::vec3 selectionBoundsCenter{0.0f};
  glm::vec3 selectionBoundsSize{0.0f};
  float selectionBoundsRadius{0.0f};
  float selectionFloorElevation{0.0f};
};

struct ViewpointSnapshotState {
  std::string label{};
  TransformControls camera{};
  uint32_t selectedMeshNode{std::numeric_limits<uint32_t>::max()};
  uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
  std::string selectedBimGuid{};
  std::string selectedBimType{};
  std::string selectedBimSourceId{};
  std::string bimModelPath{};
  BimFilterState bimFilter{};
};

class GuiManager {
public:
  GuiManager() = default;
  ~GuiManager();

  void initialize(VkInstance instance, VkDevice device,
                  VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
                  uint32_t graphicsQueueFamily, VkRenderPass renderPass,
                  uint32_t imageCount, GLFWwindow *window,
                  const std::string &defaultModelPath,
                  float defaultImportScale);

  void shutdown(VkDevice device);
  void updateSwapchainImageCount(uint32_t imageCount);

  void startFrame();
  void render(VkCommandBuffer commandBuffer);

  void drawViewportInteractionControls(
      const ViewportInteractionState &state,
      const std::function<void(ViewportTool)> &setTool,
      const std::function<void(TransformSpace)> &setTransformSpace,
      const std::function<void(TransformAxis)> &setTransformAxis,
      const std::function<void(ViewportNavigationStyle)> &setNavigationStyle,
      const std::function<void(bool)> &setTransformSnapEnabled);

  void drawViewportNavigationOverlay(
      const ViewportNavigationState &state,
      const std::function<void(CameraViewPreset)> &setViewPreset,
      const std::function<void(float, float)> &freeRotate,
      const std::function<void(float, float)> &panView,
      const std::function<void()> &toggleProjectionMode);

  void drawSceneControls(
      const container::scene::SceneGraph &sceneGraph,
      const std::function<bool(const std::string &, float)> &reloadModel,
      const std::function<bool(float)> &reloadDefault,
      const TransformControls &cameraTransform,
      const std::function<void(const TransformControls &)>
          &applyCameraTransform,
      const TransformControls &sceneTransform,
      const std::function<void(const TransformControls &)> &applySceneTransform,
      const glm::vec3 &directionalLightPosition,
      const container::gpu::LightingData &lightingData,
      const std::vector<container::gpu::PointLightData> &pointLights,
      uint32_t selectedMeshNode, const BimInspectionState &bimInspection,
      const ViewpointSnapshotState &currentViewpoint,
      const std::function<bool(const ViewpointSnapshotState &)>
          &restoreViewpoint,
      const std::function<void(uint32_t)> &selectMeshNode,
      const TransformControls &meshTransform,
      const std::function<void(uint32_t, const TransformControls &)>
          &applyMeshTransform);

  [[nodiscard]] bool isCapturingInput() const;
  [[nodiscard]] bool isCapturingMouse() const;
  [[nodiscard]] bool isCapturingKeyboard() const;
  [[nodiscard]] bool showGeometryOverlay() const {
    return showGeometryOverlay_;
  }
  [[nodiscard]] bool showLightGizmos() const { return showLightGizmos_; }
  [[nodiscard]] bool showNormalDiagCube() const { return showNormalDiagCube_; }
  [[nodiscard]] bool showNormalValidation() const {
    return normalValidationSettings_.enabled;
  }
  [[nodiscard]] const container::gpu::NormalValidationSettings &
  normalValidationSettings() const {
    return normalValidationSettings_;
  }
  [[nodiscard]] GBufferViewMode gBufferViewMode() const {
    return gBufferViewMode_;
  }
  [[nodiscard]] const WireframeSettings &wireframeSettings() const {
    return wireframeSettings_;
  }
  [[nodiscard]] const BimFilterState &bimFilterState() const {
    return bimFilterState_;
  }
  [[nodiscard]] const BimFloorPlanOverlayState &bimFloorPlanOverlayState()
      const {
    return bimFloorPlanOverlayState_;
  }
  [[nodiscard]] const BimLayerVisibilityState &bimLayerVisibilityState()
      const {
    return bimLayerVisibilityState_;
  }
  [[nodiscard]] container::renderer::BimSemanticColorMode
  bimSemanticColorMode() const {
    return bimSemanticColorMode_;
  }
  [[nodiscard]] const SectionPlaneState &sectionPlaneState() const {
    return sectionPlaneState_;
  }
  [[nodiscard]] const BimBoxClipUiState& bimBoxClipState() const {
    return bimBoxClipState_;
  }
  [[nodiscard]] const BimLodStreamingUiState& bimLodStreamingUiState() const {
    return bimLodStreamingUiState_;
  }
  [[nodiscard]] const BimClipCapHatchingUiState&
  bimClipCapHatchingUiState() const {
    return bimClipCapHatchingUiState_;
  }
  [[nodiscard]] bool wireframeSupported() const { return wireframeSupported_; }
  [[nodiscard]] const std::string &statusMessage() const {
    return statusMessage_;
  }
  [[nodiscard]] const std::string &environmentStatus() const {
    return environmentStatus_;
  }
  void setStatusMessage(std::string status) {
    statusMessage_ = std::move(status);
  }
  void setEnvironmentStatus(std::string status) {
    environmentStatus_ = std::move(status);
  }
  void setWireframeCapabilities(bool supported, bool rasterModeSupported,
                                bool wideLineSupported);
  void setCullStats(uint32_t total, uint32_t frustumPassed,
                    uint32_t occlusionPassed);
  void setLightCullingStats(const container::gpu::LightCullingStats &stats);
  void setRendererTelemetry(
      const container::renderer::RendererTelemetryView &telemetry);
  void setLightingSettings(const container::gpu::LightingSettings &settings);
  [[nodiscard]] const container::gpu::LightingSettings &
  lightingSettings() const {
    return lightingSettings_;
  }
  [[nodiscard]] const container::gpu::ShadowSettings& shadowSettings() const {
    return shadowSettings_;
  }
  void setFreezeCulling(bool frozen);
  [[nodiscard]] bool freezeCullingRequested() const { return freezeCulling_; }

  // Bloom settings (bidirectional sync with BloomManager).
  void setBloomSettings(bool enabled, float threshold, float knee,
                        float intensity, float radius);
  [[nodiscard]] bool bloomEnabled() const { return bloomEnabled_; }
  [[nodiscard]] float bloomThreshold() const { return bloomThreshold_; }
  [[nodiscard]] float bloomKnee() const { return bloomKnee_; }
  [[nodiscard]] float bloomIntensity() const { return bloomIntensity_; }
  [[nodiscard]] float bloomRadius() const { return bloomRadius_; }
  [[nodiscard]] const container::gpu::ExposureSettings& exposureSettings() const {
    return exposureSettings_;
  }
  [[nodiscard]] float postProcessExposure() const {
    return exposureSettings_.manualExposure;
  }

  // Render pass toggles (bidirectional sync with RenderGraph).
  void setRenderPassList(const std::vector<RenderPassToggle> &passes);
  [[nodiscard]] std::vector<RenderPassToggle> &renderPassToggles() {
    return renderPassToggles_;
  }
  [[nodiscard]] const std::vector<RenderPassToggle> &renderPassToggles() const {
    return renderPassToggles_;
  }

private:
  void ensureInitialized() const;
  void discoverSampleModels();
  void drawRendererTelemetryWindow();
  [[nodiscard]] int sampleModelIndexForPath(const std::string &path) const;

  struct SampleModelOption {
    std::string label;
    std::string path;
  };

  struct BimMeasurementPointState {
    bool captured{false};
    glm::vec3 center{0.0f};
    uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
    std::string label{};
    std::string modelPath{};
  };

  struct BimMeasurementAnnotationState {
    uint32_t id{0};
    std::string label{};
    BimMeasurementPointState pointA{};
    BimMeasurementPointState pointB{};
    float distance{0.0f};
    float horizontalDistance{0.0f};
    float elevationDelta{0.0f};
    float slopeAngleDegrees{0.0f};
    float elevationAxisAngleDegrees{0.0f};
  };

  struct BimSelectionSetMemberState {
    std::string label{};
    std::string type{};
    std::string storey{};
    std::string material{};
    ViewpointSnapshotState snapshot{};
  };

  struct BimSelectionSetState {
    uint32_t id{0};
    std::string label{};
    std::string modelPath{};
    std::vector<BimSelectionSetMemberState> members{};
  };

  struct BcfTopicArchiveEntryState {
    uint32_t id{0};
    std::string label{};
    std::string status{};
    std::string priority{};
    std::string path{};
    bool hasSnapshot{false};
    ViewpointSnapshotState snapshot{};
  };

  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  bool initialized_{false};
  bool showGeometryOverlay_{false};
  bool showLightGizmos_{true};
  bool showNormalDiagCube_{false};
  bool wireframeSupported_{false};
  bool wireframeRasterModeSupported_{false};
  bool wireframeWideLineSupported_{false};
  GBufferViewMode gBufferViewMode_{GBufferViewMode::Overview};
  WireframeSettings wireframeSettings_{};
  container::gpu::NormalValidationSettings normalValidationSettings_{};
  std::string modelPathInput_{};
  std::string defaultModelPath_{};
  std::vector<SampleModelOption> sampleModelOptions_;
  int selectedSampleModelIndex_{-1};
  int importScaleIndex_{0};
  float importScale_{1.0f};
  std::string statusMessage_{};
  std::string environmentStatus_{};
  uint32_t cullStatsTotal_{0};
  uint32_t cullStatsFrustum_{0};
  uint32_t cullStatsOcclusion_{0};
  container::gpu::LightCullingStats lightCullingStats_{};
  container::renderer::RendererTelemetryView rendererTelemetry_{};
  container::gpu::LightingSettings lightingSettings_{};
  container::gpu::ShadowSettings shadowSettings_{};
  bool freezeCulling_{false};
  bool bloomEnabled_{true};
  float bloomThreshold_{1.0f};
  float bloomKnee_{0.1f};
  float bloomIntensity_{0.3f};
  float bloomRadius_{1.0f};
  container::gpu::ExposureSettings exposureSettings_{};
  std::vector<RenderPassToggle> renderPassToggles_;
  BimFilterState bimFilterState_{};
  BimFloorPlanOverlayState bimFloorPlanOverlayState_{};
  BimLayerVisibilityState bimLayerVisibilityState_{};
  std::string bimQuickFilterSearch_{};
  std::string bimPropertySearch_{};
  container::renderer::BimSemanticColorMode bimSemanticColorMode_{
      container::renderer::BimSemanticColorMode::Off};
  SectionPlaneState sectionPlaneState_{};
  BimBoxClipUiState bimBoxClipState_{};
  int sectionPlaneAxis_{1};
  int selectedBimStoreyRangeIndex_{-1};
  std::string bimSelectionSetNameInput_{"Selection Set"};
  std::vector<BimSelectionSetState> bimSelectionSets_{};
  int selectedBimSelectionSetIndex_{-1};
  uint32_t nextBimSelectionSetId_{1};
  std::vector<ViewpointSnapshotState> viewpointSnapshots_{};
  int selectedViewpointSnapshotIndex_{-1};
  uint32_t nextViewpointSnapshotId_{1};
  std::string bcfViewpointPath_{"container-viewpoint.bcfv"};
  std::string bcfTopicFolderPath_{"container-bcf-topic"};
  std::string bcfTopicArchivePath_{"container-topic.bcfzip"};
  std::string bcfTopicTitleInput_{"BIM issue"};
  std::string bcfTopicStatusInput_{"Open"};
  std::string bcfTopicPriorityInput_{};
  std::string bcfTopicLabelsInput_{};
  std::string bcfTopicCommentInput_{};
  std::vector<BcfTopicArchiveEntryState> bcfTopicArchiveEntries_{};
  int selectedBcfTopicArchiveIndex_{-1};
  uint32_t nextBcfTopicArchiveId_{1};
  BimLodStreamingUiState bimLodStreamingUiState_{};
  BimClipCapHatchingUiState bimClipCapHatchingUiState_{};
  std::string bimMeasurementModelPath_{};
  float bimMeasurementEffectiveImportScale_{1.0f};
  size_t bimMeasurementObjectCount_{0};
  BimMeasurementPointState bimMeasurementPointA_{};
  BimMeasurementPointState bimMeasurementPointB_{};
  std::vector<BimMeasurementAnnotationState> bimMeasurementAnnotations_{};
  uint32_t nextBimMeasurementAnnotationId_{1};
  int selectedBimMeasurementAnnotationIndex_{-1};
  bool viewportNavFreeRotateActive_{false};
  bool viewportNavPanActive_{false};
};

} // namespace container::ui
