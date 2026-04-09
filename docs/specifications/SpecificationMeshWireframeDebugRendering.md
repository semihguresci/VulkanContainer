Specification: Mesh Wireframe Debug Rendering
1. Overview

This specification defines a wireframe debug rendering feature for the Vulkan-based renderer.
The feature enables visualization of mesh triangle topology by rendering triangle edges as lines.

It is intended as a developer-facing diagnostic tool for inspecting:

Mesh triangulation
Topology correctness
Winding order issues
Vertex/index buffer integrity
glTF import results
2. Motivation

Current shaded rendering does not expose underlying triangle structure.
This limits the ability to debug:

Incorrect normals or shading artifacts
Broken or non-manifold geometry
Incorrect index buffers
Face orientation (inside-out meshes)
Tessellation density and distribution

Given that the renderer already loads indexed triangle meshes via glTF and builds draw commands from them, a wireframe mode provides direct insight into the actual GPU geometry being rendered.

3. Scope
In Scope
Wireframe rendering of all scene meshes
Runtime toggle via GUI
Overlay and full wireframe modes
Integration with existing rendering pipeline
Reuse of existing vertex/index buffers and draw commands
Out of Scope
Editing or modifying mesh topology
CPU-side wireframe mesh generation
Non-triangle primitives
Production rendering (debug-only feature)
4. Definitions
Wireframe Rendering: Rendering only triangle edges instead of filled surfaces
Overlay Mode: Wireframe drawn on top of shaded scene
Full Wireframe Mode: Scene rendered only as wireframe
Topology Visualization: Representation of triangle connectivity via edges
5. User Stories
As a rendering engineer, I want to toggle wireframe mode to inspect triangle topology.
As a developer, I want to verify that glTF meshes are correctly triangulated.
As a technical artist, I want to visualize mesh density and structure.
As a debugger, I want to detect inside-out faces and winding issues.
6. Functional Requirements
FR-1: Wireframe Toggle

The renderer shall provide a runtime toggle:

Wireframe: ON / OFF
FR-2: Rendering Modes

The renderer shall support:

Disabled
Full Wireframe
Overlay Wireframe
FR-3: Geometry Source

Wireframe rendering shall:

Use existing vertex and index buffers
Use existing draw command lists
Reflect actual GPU triangle topology
FR-4: Mesh Compatibility

The feature shall support:

All indexed triangle meshes
All materials (opaque + transparent in later phase)
FR-5: Visual Controls

The renderer shall allow configuration of:

Wireframe color
Line width (if supported)
Depth test (enabled / disabled)
Overlay intensity
FR-6: Debug Integration

The feature shall integrate with existing debug visualization system:

Similar to geometry debug and normal visualization passes
Controlled via GUI manager
7. Technical Requirements
TR-1: Pipeline Creation

A dedicated Vulkan graphics pipeline shall be created:

Wireframe Pipeline

Key states:

polygonMode = VK_POLYGON_MODE_LINE;
topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
TR-2: Device Feature Requirement

The renderer shall verify support for:

VkPhysicalDeviceFeatures::fillModeNonSolid

If unsupported:

Disable wireframe feature in UI
Log warning
TR-3: Pipeline Configuration
Rasterization
polygonMode = LINE
cullMode = BACK or NONE
frontFace = consistent with existing pipeline
Depth
Overlay mode: depth test ON, depth write OFF
Debug mode: optional depth test OFF
Multisampling
Same as scene pipeline
TR-4: Command Buffer Integration

Wireframe rendering shall be integrated into existing command recording:

Overlay Mode
Executed after lighting pass
Similar to existing debug overlays
Full Wireframe Mode
Replace opaque rendering pass OR
Skip shading pipelines and render only wireframe
TR-5: Geometry Reuse

Wireframe pass shall reuse:

bindSceneGeometryBuffers()
drawSceneGeometry()
Existing object buffers and push constants

No duplication of mesh data is allowed.

TR-6: Shader

Minimal shader requirements:

Vertex Shader
Same as scene vertex shader
Outputs clip-space position
Fragment Shader
Constant color output (wireframe color)
TR-7: Performance Constraints
Must not introduce additional CPU-side mesh processing
GPU cost should scale with existing draw calls
Should be optional and disabled by default
8. UI / UX Requirements

Add controls to GUI:

[ ] Wireframe Enabled
Mode: (Overlay / Full)
Depth Test: (On / Off)
Line Color: [RGB picker]
Line Width: [if supported]
9. Acceptance Criteria
Wireframe mode correctly displays triangle edges
Overlay mode preserves shaded rendering
Full wireframe mode renders only edges
Edges match actual mesh triangulation
Feature works across all loaded glTF models
No regression in existing rendering pipeline
Toggle works in real-time without restart
10. Implementation Plan
Phase 1 (Core)
Add pipeline with VK_POLYGON_MODE_LINE
Add GUI toggle
Integrate overlay rendering
Support opaque meshes
Phase 2 (Extension)
Transparent meshes
Per-object wireframe
Depth bias tuning
Phase 3 (Advanced)
Barycentric wireframe (shader-based)
Edge highlighting (silhouette emphasis)
Thickness control via shader
11. Risks & Considerations
R1: Hardware Support
Some GPUs have limited line rendering support
Line width > 1.0 may not be supported
R2: Visual Quality
Hardware wireframe can look thin or inconsistent
May require shader-based fallback later
R3: Performance
Additional draw pass increases GPU cost
Should remain debug-only
12. Future Enhancements
Barycentric coordinate wireframe (no reliance on polygon mode)
Edge classification (silhouette vs internal edges)
Mesh analysis tools (degenerate triangles, non-manifold edges)
Integration with selection system
13. Summary

This feature introduces a wireframe debug rendering mode that:

Reuses existing rendering infrastructure
Provides critical visibility into mesh topology
Fits naturally into the renderer’s debug pipeline architecture
Enables faster debugging and validation of geometry