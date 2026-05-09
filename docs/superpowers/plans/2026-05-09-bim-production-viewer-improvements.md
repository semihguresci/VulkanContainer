# BIM Production Viewer Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the current BIM/USD renderer from a usable model viewer into a stronger production BIM review viewer with drawing exports, precise measurement, deeper metadata browsing, review workflows, georeferencing, and coordination overlays.

**Architecture:** Keep the renderer-side BIM core in `src/renderer/bim` and keep ImGui orchestration in `src/utility/GuiManager.cpp`. Add small focused BIM service files for drawing export, relationships, snapping, schedules, compare, overlays, and georeference transforms, then route their state through `RendererFrontend` and existing frame pass planners only where they need GPU rendering.

**Tech Stack:** C++23, Vulkan, Slang shaders, ImGui, miniz for BCF archives, stb image writing for screenshot export, GTest, existing BIM managers/loaders.

---

## Scope Check

These improvements span independent subsystems. Implement them as separate workstreams:

1. Drawing/view workflow: saved drawing views, elevation/floor export, section markers.
2. BIM semantics workflow: relationship graph, search, property browser, schedules/quantities.
3. Field/review workflow: snapping measurements, BCF topics/comments/pins, issue overlays.
4. Coordination workflow: room/space overlays, MEP x-ray, clash visualization, model compare.
5. Georeference workflow: origin rebasing, project/site coordinates, coordinate readouts.

Each workstream must keep an independently testable CPU planning layer before adding renderer wiring.

## File Structure

Create:

- `include/Container/renderer/bim/BimDrawingExport.h`: serializable drawing view/export request structs and SVG export helpers.
- `src/renderer/bim/BimDrawingExport.cpp`: pure CPU SVG drawing export from BIM overlay line/cap data and saved view metadata.
- `include/Container/renderer/bim/BimRelationshipGraph.h`: IFC relationship graph, spatial tree, systems, zones, classifications, and property-set query API.
- `src/renderer/bim/BimRelationshipGraph.cpp`: graph builder from `BimElementMetadata` and loader relationship records.
- `include/Container/renderer/bim/BimMeasurementSnapping.h`: snap request/candidate/result structs for vertex, edge midpoint, face center, bounds, and floor elevation.
- `src/renderer/bim/BimMeasurementSnapping.cpp`: CPU snap ranking over selected element bounds and loaded BIM triangle metadata.
- `include/Container/renderer/bim/BimScheduleExtractor.h`: quantity/schedule aggregation structs and API.
- `src/renderer/bim/BimScheduleExtractor.cpp`: aggregate counts, materials, floor areas from metadata/bounds.
- `include/Container/renderer/bim/BimModelCompare.h`: GUID/source-id based model comparison API.
- `src/renderer/bim/BimModelCompare.cpp`: added/removed/changed classification and material/storey/type deltas.
- `include/Container/renderer/bim/BimGeoreferenceTransform.h`: project/site/survey transform settings and coordinate readout API.
- `src/renderer/bim/BimGeoreferenceTransform.cpp`: unit-aware world-to-project/site coordinate conversion and origin rebase decisions.
- `include/Container/renderer/bim/BimCoordinationOverlay.h`: room/space, x-ray, clash, issue pin overlay structs.
- `src/renderer/bim/BimCoordinationOverlay.cpp`: CPU overlay plan builder that produces draw/pin records.
- `shaders/bim_issue_pin.slang`: billboard/marker shader for issue pins and clash markers.
- `tests/renderer/bim/bim_drawing_export_tests.cpp`
- `tests/renderer/bim/bim_relationship_graph_tests.cpp`
- `tests/renderer/bim/bim_measurement_snapping_tests.cpp`
- `tests/renderer/bim/bim_schedule_extractor_tests.cpp`
- `tests/renderer/bim/bim_model_compare_tests.cpp`
- `tests/renderer/bim/bim_georeference_transform_tests.cpp`
- `tests/renderer/bim/bim_coordination_overlay_tests.cpp`

Modify:

- `include/Container/utility/SceneData.h`: add optional relationship/classification/quantity/georeference fields shared by loaders.
- `include/Container/geometry/IfcxLoader.h`, `src/geometry/IfcxLoader.cpp`: preserve IFCX relationships, classifications, space/system/zone membership.
- `include/Container/geometry/IfcTessellatedLoader.h`, `src/geometry/IfcTessellatedLoader.cpp`: preserve IFC relationship/property set records during STEP parsing.
- `include/Container/geometry/DotBimLoader.h`, `src/geometry/DotBimLoader.cpp`: pass through available classification/georeference fields.
- `include/Container/geometry/UsdLoader.h`, `src/geometry/UsdLoader.cpp`: map USD BIM custom data to the same relationship/property structures when authored.
- `include/Container/renderer/bim/BimManager.h`, `src/renderer/bim/BimManager.cpp`: own relationship graph, schedule cache, compare cache, georeference readouts, and coordination overlays.
- `include/Container/utility/GuiManager.h`, `src/utility/GuiManager.cpp`: add panels for drawing export, precise measurement, relationship tree, schedules, compare, georeference, and issue/overlay controls.
- `include/Container/renderer/core/FrameRecorder.h`, `src/renderer/core/FrameRecorder.cpp`: add optional issue/clash pin pass data.
- `src/renderer/core/RendererFrontend.cpp`: translate GUI state into BIM manager queries, drawing export requests, and frame overlay inputs.
- `include/Container/renderer/deferred/DeferredRasterLighting.h`, `src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp`: route issue/clash marker rendering after OIT and before GUI.
- `src/CMakeLists.txt`: add new renderer BIM source files and shader.
- `tests/CMakeLists.tests.cmake`: add the seven new GTest targets.
- `docs/architecture.md`: link this plan and the resulting BIM viewer architecture notes.

---

### Task 1: Drawing Views And Export

**Files:**
- Create: `include/Container/renderer/bim/BimDrawingExport.h`
- Create: `src/renderer/bim/BimDrawingExport.cpp`
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `tests/renderer/bim/bim_drawing_export_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write failing drawing export tests**

Add `tests/renderer/bim/bim_drawing_export_tests.cpp`:

```cpp
#include "Container/renderer/bim/BimDrawingExport.h"

#include <gtest/gtest.h>

using container::renderer::BimDrawingExportLine;
using container::renderer::BimDrawingExportRequest;
using container::renderer::ExportBimDrawingSvg;

TEST(BimDrawingExportTests, WritesSvgWithScaleViewAndLinework) {
  BimDrawingExportRequest request{};
  request.title = "Main floor";
  request.viewName = "Front elevation";
  request.paperWidthMm = 297.0f;
  request.paperHeightMm = 210.0f;
  request.modelUnitsPerPaperMm = 50.0f;
  request.lines.push_back(BimDrawingExportLine{
      .a = {0.0f, 0.0f, 0.0f},
      .b = {4.0f, 0.0f, 0.0f},
      .color = {0.0f, 0.0f, 0.0f},
      .lineWidthMm = 0.18f,
      .layer = "walls"});

  const std::string svg = ExportBimDrawingSvg(request);

  EXPECT_NE(svg.find("<svg"), std::string::npos);
  EXPECT_NE(svg.find("Main floor"), std::string::npos);
  EXPECT_NE(svg.find("Front elevation"), std::string::npos);
  EXPECT_NE(svg.find("data-layer=\"walls\""), std::string::npos);
  EXPECT_NE(svg.find("<line"), std::string::npos);
}

TEST(BimDrawingExportTests, RejectsEmptyPaperSize) {
  BimDrawingExportRequest request{};
  request.paperWidthMm = 0.0f;
  request.paperHeightMm = 210.0f;

  EXPECT_TRUE(ExportBimDrawingSvg(request).empty());
}
```

- [ ] **Step 2: Register and run the failing test**

Add to `tests/CMakeLists.tests.cmake`:

```cmake
add_custom_test(bim_drawing_export_tests
    ${TEST_RENDERER_BIM_DIR}/bim_drawing_export_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
```

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_drawing_export_tests --config Debug'
out\build\windows-debug\tests\bim_drawing_export_tests.exe --gtest_filter=BimDrawingExportTests.*
```

Expected: build fails because `BimDrawingExport.h` does not exist.

- [ ] **Step 3: Implement the pure export API**

Add `include/Container/renderer/bim/BimDrawingExport.h`:

```cpp
#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace container::renderer {

struct BimDrawingExportLine {
  glm::vec3 a{0.0f};
  glm::vec3 b{0.0f};
  glm::vec3 color{0.0f};
  float lineWidthMm{0.18f};
  std::string layer{};
};

struct BimDrawingExportRequest {
  std::string title{};
  std::string viewName{};
  float paperWidthMm{297.0f};
  float paperHeightMm{210.0f};
  float modelUnitsPerPaperMm{50.0f};
  std::vector<BimDrawingExportLine> lines{};
};

[[nodiscard]] std::string ExportBimDrawingSvg(
    const BimDrawingExportRequest& request);

}  // namespace container::renderer
```

Add `src/renderer/bim/BimDrawingExport.cpp`:

```cpp
#include "Container/renderer/bim/BimDrawingExport.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace container::renderer {
namespace {

std::string colorToCss(glm::vec3 color) {
  const auto toByte = [](float value) {
    return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  std::ostringstream out;
  out << "rgb(" << toByte(color.r) << "," << toByte(color.g) << ","
      << toByte(color.b) << ")";
  return out.str();
}

std::string escapeXml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

}  // namespace

std::string ExportBimDrawingSvg(const BimDrawingExportRequest& request) {
  if (request.paperWidthMm <= 0.0f || request.paperHeightMm <= 0.0f ||
      request.modelUnitsPerPaperMm <= 0.0f) {
    return {};
  }

  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""
      << request.paperWidthMm << "mm\" height=\"" << request.paperHeightMm
      << "mm\" viewBox=\"0 0 " << request.paperWidthMm << " "
      << request.paperHeightMm << "\">\n";
  out << "  <title>" << escapeXml(request.title) << "</title>\n";
  out << "  <desc>" << escapeXml(request.viewName) << "</desc>\n";
  out << "  <g fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
  const float cx = request.paperWidthMm * 0.5f;
  const float cy = request.paperHeightMm * 0.5f;
  for (const BimDrawingExportLine& line : request.lines) {
    const float x1 = cx + line.a.x / request.modelUnitsPerPaperMm;
    const float y1 = cy - line.a.z / request.modelUnitsPerPaperMm;
    const float x2 = cx + line.b.x / request.modelUnitsPerPaperMm;
    const float y2 = cy - line.b.z / request.modelUnitsPerPaperMm;
    out << "    <line data-layer=\"" << escapeXml(line.layer) << "\" x1=\""
        << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
        << "\" stroke=\"" << colorToCss(line.color) << "\" stroke-width=\""
        << std::max(line.lineWidthMm, 0.05f) << "\" />\n";
  }
  out << "  </g>\n</svg>\n";
  return out.str();
}

}  // namespace container::renderer
```

- [ ] **Step 4: Add the files to the build**

Add `renderer/bim/BimDrawingExport.cpp` to the renderer source list in `src/CMakeLists.txt`.

- [ ] **Step 5: Wire UI requests without changing rendering behavior**

In `include/Container/utility/GuiManager.h`, add:

```cpp
struct BimDrawingExportUiState {
  std::string svgPath{"bim-drawing.svg"};
  float paperWidthMm{297.0f};
  float paperHeightMm{210.0f};
  float modelUnitsPerPaperMm{50.0f};
  bool exportRequested{false};
};

[[nodiscard]] BimDrawingExportUiState consumeBimDrawingExportRequest();
```

In `src/utility/GuiManager.cpp`, add a `BIM Drawing Export` tree under the BIM panel with fields for path, paper size, scale, and an `Export SVG` button that sets `exportRequested`.

In `src/renderer/core/RendererFrontend.cpp`, consume the request and call `ExportBimDrawingSvg` with floor/elevation linework available from `BimManager`.

- [ ] **Step 6: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target VulkanSceneRenderer bim_drawing_export_tests rendering_convention_tests --config Debug'
out\build\windows-debug\tests\bim_drawing_export_tests.exe --gtest_filter=BimDrawingExportTests.*
out\build\windows-debug\tests\rendering_convention_tests.exe --gtest_filter=*Bim*
git diff --check
```

Expected: all commands pass.

---

### Task 2: Relationship Graph, Spatial Browser, And Full Property Browsing

**Files:**
- Create: `include/Container/renderer/bim/BimRelationshipGraph.h`
- Create: `src/renderer/bim/BimRelationshipGraph.cpp`
- Modify: `include/Container/utility/SceneData.h`
- Modify: `src/geometry/IfcxLoader.cpp`
- Modify: `src/geometry/IfcTessellatedLoader.cpp`
- Modify: `src/renderer/bim/BimManager.cpp`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `tests/renderer/bim/bim_relationship_graph_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write failing graph tests**

Add tests for:

```cpp
TEST(BimRelationshipGraphTests, BuildsSpatialContainmentAndPropertySets);
TEST(BimRelationshipGraphTests, FindsSystemsZonesAndClassificationsByGuid);
TEST(BimRelationshipGraphTests, SearchMatchesPropertySetNameValueAndElementName);
```

Use a small in-memory record set: building -> storey -> wall, wall -> system, wall -> zone, wall -> `Pset_WallCommon.FireRating = 2HR`.

- [ ] **Step 2: Implement graph structs and queries**

`BimRelationshipGraph.h` must expose:

```cpp
enum class BimRelationshipKind : uint32_t {
  SpatialParent,
  TypeDefinition,
  MaterialAssignment,
  SystemAssignment,
  ZoneAssignment,
  Classification,
  PropertySet,
};

struct BimRelationshipNode {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  std::string guid{};
  std::string sourceId{};
  std::string label{};
  std::string ifcClass{};
};

struct BimRelationshipEdge {
  uint32_t from{0};
  uint32_t to{0};
  BimRelationshipKind kind{BimRelationshipKind::SpatialParent};
  std::string label{};
};
```

The API must return children, parents, relationship edges by object index, and search hits with object index plus matched text.

- [ ] **Step 3: Preserve loader relationship records**

Extend `SceneData.h` with neutral records:

```cpp
struct ElementRelationship {
  std::string fromGuid{};
  std::string fromSourceId{};
  std::string toGuid{};
  std::string toSourceId{};
  std::string kind{};
  std::string label{};
};
```

Add `std::vector<ElementRelationship> relationships` to the BIM model data structure used by `IfcxLoader`, `IfcTessellatedLoader`, `UsdLoader`, and `DotBimLoader`.

- [ ] **Step 4: Build and cache graph in `BimManager`**

`BimManager::loadModel` should build `BimRelationshipGraph` after metadata is uploaded. Expose:

```cpp
[[nodiscard]] const BimRelationshipGraph& relationshipGraph() const;
```

- [ ] **Step 5: Replace flat spatial UI with graph-backed UI**

Keep current storey list, but add expandable:

- Project/Site/Building/Storey hierarchy.
- Element relationships.
- Property-set tree grouped by set name.
- Systems, zones, classifications.
- Search field that filters element name, GUID, source id, IFC class, property set, and property value.

- [ ] **Step 6: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_relationship_graph_tests ifcx_loader_tests ifc_tessellated_loader_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bim_relationship_graph_tests.exe
out\build\windows-debug\tests\ifcx_loader_tests.exe --gtest_filter=*Relationship*:*Property*
out\build\windows-debug\tests\ifc_tessellated_loader_tests.exe --gtest_filter=*Relationship*:*Property*
git diff --check
```

Expected: all commands pass.

---

### Task 3: Precise Measurement And Snapping

**Files:**
- Create: `include/Container/renderer/bim/BimMeasurementSnapping.h`
- Create: `src/renderer/bim/BimMeasurementSnapping.cpp`
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `tests/renderer/bim/bim_measurement_snapping_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write failing snapping tests**

Add tests for:

```cpp
TEST(BimMeasurementSnappingTests, PrefersClosestVertexWithinPixelRadius);
TEST(BimMeasurementSnappingTests, FallsBackToEdgeMidpointThenBoundsCenter);
TEST(BimMeasurementSnappingTests, ReportsDistanceAngleAreaAndElevationDelta);
```

Use deterministic world-space candidates and a synthetic ray/cursor.

- [ ] **Step 2: Implement snap API**

`BimMeasurementSnapping.h` must expose:

```cpp
enum class BimSnapKind : uint32_t {
  None,
  Vertex,
  EdgeMidpoint,
  FaceCenter,
  BoundsCorner,
  BoundsCenter,
  FloorElevation,
};

struct BimSnapCandidate {
  BimSnapKind kind{BimSnapKind::None};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 worldPosition{0.0f};
  float screenDistancePixels{std::numeric_limits<float>::max()};
  std::string label{};
};

struct BimMeasurementResult {
  float distance{0.0f};
  float horizontalDistance{0.0f};
  float elevationDelta{0.0f};
  float angleDegrees{0.0f};
  float polygonArea{0.0f};
};
```

- [ ] **Step 3: Extend measurement UI**

Add controls:

- Snap mode: Off, Vertex, Edge, Face, Bounds, Floor.
- Distance between arbitrary picked points.
- Angle from three points.
- Polygon area from three or more points.
- Persisted annotations shown in the BIM measurements list.

- [ ] **Step 4: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_measurement_snapping_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bim_measurement_snapping_tests.exe
git diff --check
```

Expected: all commands pass.

---

### Task 4: Section Markers And Per-Material Cut Styles

**Files:**
- Modify: `include/Container/renderer/bim/BimSectionCapBuilder.h`
- Modify: `src/renderer/bim/BimSectionCapBuilder.cpp`
- Modify: `include/Container/renderer/bim/BimSectionClipCapPassPlanner.h`
- Modify: `src/renderer/bim/BimSectionClipCapPassPlanner.cpp`
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Test: `tests/renderer/bim/bim_section_cap_builder_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Add tests for material-specific cap styles**

Create tests proving:

- Concrete cut surfaces can use denser hatch spacing than glass.
- Hatch angle is stable for Front/Back/Left/Right elevation views.
- Section marker arrow positions are generated outside model bounds.

- [ ] **Step 2: Add cap style model**

Extend `BimSectionCapBuildOptions`:

```cpp
struct BimSectionCapMaterialStyle {
  uint32_t materialIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 fillColor{0.06f, 0.08f, 0.10f};
  float fillOpacity{0.82f};
  float hatchSpacing{0.25f};
  float hatchAngleRadians{0.7853982f};
  glm::vec3 hatchColor{0.08f};
};
```

Keep existing global hatch values as fallback.

- [ ] **Step 3: Add section marker overlay records**

Add marker line/arrow records to the cap builder output and route them through the existing BIM lighting overlay line pass.

- [ ] **Step 4: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_section_cap_builder_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bim_section_cap_builder_tests.exe
git diff --check
```

Expected: all commands pass.

---

### Task 5: Room, Space, MEP X-Ray, Clash, And Issue Pin Overlays

**Files:**
- Create: `include/Container/renderer/bim/BimCoordinationOverlay.h`
- Create: `src/renderer/bim/BimCoordinationOverlay.cpp`
- Create: `shaders/bim_issue_pin.slang`
- Modify: `include/Container/renderer/bim/BimLightingOverlayPlanner.h`
- Modify: `src/renderer/bim/BimLightingOverlayPlanner.cpp`
- Modify: `include/Container/renderer/bim/BimLightingOverlayRecorder.h`
- Modify: `src/renderer/bim/BimLightingOverlayRecorder.cpp`
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/renderer/core/FrameRecorder.cpp`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Test: `tests/renderer/bim/bim_coordination_overlay_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write overlay planning tests**

Add tests for:

```cpp
TEST(BimCoordinationOverlayTests, BuildsTransparentSpaceOverlayFromIfcSpace);
TEST(BimCoordinationOverlayTests, BuildsMepXrayOverlayForPipeAndDuctClasses);
TEST(BimCoordinationOverlayTests, BuildsClashPinsFromElementPairs);
```

- [ ] **Step 2: Implement CPU overlay plan**

Expose:

```cpp
enum class BimCoordinationOverlayKind : uint32_t {
  Space,
  MepXray,
  Clash,
  IssuePin,
};

struct BimCoordinationOverlayMarker {
  BimCoordinationOverlayKind kind{BimCoordinationOverlayKind::IssuePin};
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f, 0.2f, 0.1f};
  std::string label{};
  uint32_t primaryObjectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t secondaryObjectIndex{std::numeric_limits<uint32_t>::max()};
};
```

The first implementation can consume manually supplied clash pairs and BCF pins. It must not compute heavy clash detection yet.

- [ ] **Step 3: Render markers after transparent resolve and before GUI**

Route marker pass after OIT resolve so pins are visible over glass but still depth-testable when requested.

- [ ] **Step 4: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_coordination_overlay_tests VulkanSceneRenderer rendering_convention_tests --config Debug'
out\build\windows-debug\tests\bim_coordination_overlay_tests.exe
out\build\windows-debug\tests\rendering_convention_tests.exe --gtest_filter=*Bim*Overlay*
git diff --check
```

Expected: all commands pass.

---

### Task 6: BCF Topic Board, Comments, Markups, And Pins

**Files:**
- Modify: `include/Container/utility/BcfViewpoint.h`
- Modify: `src/utility/BcfViewpoint.cpp`
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/renderer/bim/BimCoordinationOverlay.cpp`
- Test: `tests/ui/bcf_viewpoint_tests.cpp`

- [ ] **Step 1: Expand BCF tests**

Add tests:

```cpp
TEST(BcfViewpoint, AppendsCommentToLoadedArchiveAndPreservesViewpoints);
TEST(BcfViewpoint, ExportsPinsAsIssueOverlayMarkers);
TEST(BcfViewpoint, RoundTripsMarkupPolylineAndSnapshot);
```

- [ ] **Step 2: Add markup drawing primitives**

Extend `BcfViewpoint.h`:

```cpp
struct BcfMarkupLine {
  std::string guid{};
  glm::vec3 a{0.0f};
  glm::vec3 b{0.0f};
  glm::vec3 color{1.0f, 0.2f, 0.1f};
};
```

Store these on `BcfViewpointMarkup`.

- [ ] **Step 3: Add issue board UI**

The UI must show:

- Topic list.
- Topic status/priority/labels.
- Comment thread.
- Viewpoints with restore buttons.
- Pins and markup lines.
- Save/load `.bcfzip`.

- [ ] **Step 4: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bcf_viewpoint_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bcf_viewpoint_tests.exe --gtest_filter=BcfViewpoint.*
git diff --check
```

Expected: all commands pass.

---

### Task 7: Schedules, Quantities, And Model Compare

**Files:**
- Create: `include/Container/renderer/bim/BimScheduleExtractor.h`
- Create: `src/renderer/bim/BimScheduleExtractor.cpp`
- Create: `include/Container/renderer/bim/BimModelCompare.h`
- Create: `src/renderer/bim/BimModelCompare.cpp`
- Modify: `src/renderer/bim/BimManager.cpp`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `tests/renderer/bim/bim_schedule_extractor_tests.cpp`
- Test: `tests/renderer/bim/bim_model_compare_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write schedule and compare tests**

Tests must verify:

- Counts by IFC class and storey.
- Material totals by name.
- Bounding-box area/volume estimates are stable for simple boxes.
- Model compare reports added, removed, and changed elements by GUID/source id.

- [ ] **Step 2: Implement schedule extractor**

Expose:

```cpp
struct BimScheduleRow {
  std::string key{};
  std::string storey{};
  std::string material{};
  uint32_t count{0};
  double estimatedArea{0.0};
  double estimatedVolume{0.0};
};
```

- [ ] **Step 3: Implement compare API**

Expose:

```cpp
enum class BimModelChangeKind : uint32_t {
  Added,
  Removed,
  ChangedType,
  ChangedStorey,
  ChangedMaterial,
  ChangedBounds,
};
```

- [ ] **Step 4: Add UI panels**

Add:

- Schedule table with grouping selector.
- CSV export through a plain text file writer.
- Compare panel that accepts a second BIM model path and shows changes.
- Buttons to isolate added/removed/changed objects.

- [ ] **Step 5: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_schedule_extractor_tests bim_model_compare_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bim_schedule_extractor_tests.exe
out\build\windows-debug\tests\bim_model_compare_tests.exe
git diff --check
```

Expected: all commands pass.

---

### Task 8: Georeference Tools And Coordinate Readouts

**Files:**
- Create: `include/Container/renderer/bim/BimGeoreferenceTransform.h`
- Create: `src/renderer/bim/BimGeoreferenceTransform.cpp`
- Modify: `include/Container/utility/SceneData.h`
- Modify: `src/geometry/IfcTessellatedLoader.cpp`
- Modify: `src/geometry/IfcxLoader.cpp`
- Modify: `src/renderer/bim/BimManager.cpp`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `tests/renderer/bim/bim_georeference_transform_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write georeference tests**

Tests must verify:

```cpp
TEST(BimGeoreferenceTransformTests, ConvertsWorldToProjectCoordinatesWithUnits);
TEST(BimGeoreferenceTransformTests, AppliesSurveyOffsetWhenMetadataExists);
TEST(BimGeoreferenceTransformTests, RecommendsOriginRebaseForLargeCoordinates);
```

- [ ] **Step 2: Implement transform API**

Expose:

```cpp
struct BimCoordinateReadout {
  glm::dvec3 rendererWorld{0.0};
  glm::dvec3 projectCoordinates{0.0};
  glm::dvec3 surveyCoordinates{0.0};
  std::string crsLabel{};
  bool hasSurveyCoordinates{false};
};
```

- [ ] **Step 3: Add UI**

Add georeference panel:

- Source units/effective scale.
- CRS authority/code/name.
- Map conversion name.
- Origin offset source.
- Cursor/selection renderer coordinates.
- Cursor/selection project coordinates.
- Origin rebase recommendation status.

- [ ] **Step 4: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_georeference_transform_tests ifcx_loader_tests ifc_tessellated_loader_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bim_georeference_transform_tests.exe
git diff --check
```

Expected: all commands pass.

---

### Task 9: 4D Phase Timeline And Discipline Presets

**Files:**
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/renderer/bim/BimDrawFilterState.cpp`
- Modify: `src/renderer/bim/BimManager.cpp`
- Test: `tests/renderer/bim/bim_draw_filter_state_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Add filter tests**

Tests must verify:

- Phase range selection hides future/demolished elements.
- Discipline preset for Architecture hides MEP classes.
- Discipline preset for MEP x-ray shows pipe/duct/cable classes.

- [ ] **Step 2: Add phase timeline state**

Add UI state:

```cpp
struct BimPhaseTimelineUiState {
  bool enabled{false};
  int activePhaseIndex{0};
  bool showExisting{true};
  bool showNew{true};
  bool showDemolished{false};
  bool ghostFuture{true};
};
```

- [ ] **Step 3: Extend GPU visibility metadata**

Map phase/status IDs into the existing visibility object metadata path so CPU and GPU filters produce identical results.

- [ ] **Step 4: Verify**

Run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target bim_draw_filter_state_tests VulkanSceneRenderer --config Debug'
out\build\windows-debug\tests\bim_draw_filter_state_tests.exe
git diff --check
```

Expected: all commands pass.

---

## Integration Order

1. Task 1 first: drawing export gives immediate user-visible value and reuses elevation/floor plan work.
2. Task 2 next: the relationship graph becomes the data backbone for search, schedules, BCF, compare, and filters.
3. Task 3 next: precise measurement relies on stronger picking/snap semantics but can ship before advanced overlays.
4. Task 4 next: production drawing output needs cap styles and section markers.
5. Task 6 and Task 5 can run in parallel: BCF data and overlay rendering touch different primary files, then integrate through `BimCoordinationOverlay`.
6. Task 7 after Task 2: schedules and compare should use the relationship/property graph.
7. Task 8 can run alongside Task 7: georeference reads model metadata and has a narrow API.
8. Task 9 last: phase/disciplines affect all filtering and should land after relationship/search behavior is stable.

## Verification Matrix

Run the focused test for each task before integration. Before claiming the branch is complete, run:

```powershell
cmd /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out\build\windows-debug --target VulkanSceneRenderer rendering_convention_tests bcf_viewpoint_tests ifcx_loader_tests ifc_tessellated_loader_tests bim_drawing_export_tests bim_relationship_graph_tests bim_measurement_snapping_tests bim_schedule_extractor_tests bim_model_compare_tests bim_georeference_transform_tests bim_coordination_overlay_tests --config Debug'
out\build\windows-debug\tests\rendering_convention_tests.exe --gtest_filter=*Bim*
out\build\windows-debug\tests\bcf_viewpoint_tests.exe --gtest_filter=BcfViewpoint.*
out\build\windows-debug\tests\ifcx_loader_tests.exe
out\build\windows-debug\tests\ifc_tessellated_loader_tests.exe
out\build\windows-debug\tests\bim_drawing_export_tests.exe
out\build\windows-debug\tests\bim_relationship_graph_tests.exe
out\build\windows-debug\tests\bim_measurement_snapping_tests.exe
out\build\windows-debug\tests\bim_schedule_extractor_tests.exe
out\build\windows-debug\tests\bim_model_compare_tests.exe
out\build\windows-debug\tests\bim_georeference_transform_tests.exe
out\build\windows-debug\tests\bim_coordination_overlay_tests.exe
git diff --check
```

## Self-Review

Spec coverage:

- Saved BIM views and drawing export: Task 1.
- Dimension/snapping: Task 3.
- Spatial tree and property browser: Task 2.
- Search and selection set foundation: Task 2 extends searchable graph; existing selection sets remain in `GuiManager`.
- CAD hidden line, section markers, per-material cut styles: Task 4 builds on the existing elevation hidden-line work.
- Room/space overlays, MEP x-ray, clash visualization: Task 5.
- BCF topics/comments UI and pins: Task 6.
- Model compare, schedules/quantities, 4D/phase: Tasks 7 and 9.
- Georeference tools: Task 8.

Placeholder scan:

- No task uses an unresolved placeholder as acceptance criteria.
- Heavy clash detection and issue-server sync are explicitly outside the first implementation; manual clash input and local BCF archive workflow are the first shippable targets.

Type consistency:

- BIM export uses `BimDrawingExport*`.
- Relationships use `BimRelationship*`.
- Snapping uses `BimSnap*` and `BimMeasurement*`.
- Coordination overlays use `BimCoordinationOverlay*`.
- Georeference uses `BimCoordinateReadout`.

