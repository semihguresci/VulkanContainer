Specification: Triangle Normal Validation Debug Rendering
1. Overview

The Triangle Normal Validation Debug Rendering feature is a diagnostic visualization mode used to verify the correctness of triangle orientation and normal direction within the rendering pipeline.

This mode highlights triangles based on whether their geometric normals are consistent with expected orientation rules.

Correct triangles → rendered in blue
Incorrect triangles → rendered in red

This feature is intended for debugging:

Mesh winding issues
Normal generation errors
Coordinate system mismatches (LH vs RH)
Inverted faces (inside-out geometry)
2. Motivation

From your renderer behavior and loader:

Normals may be:
Loaded from glTF
Generated via cross product in loader
Errors can come from:
Incorrect winding (CW vs CCW)
Wrong handedness conversion
Negative scale transforms
Incorrect normal matrix

This debug mode provides immediate visual feedback at triangle level.

3. Functional Requirements
3.1 Activation
Toggle via GUI (e.g. Debug Panel)
Optional hotkey (e.g. F9)
Mutually exclusive with other debug modes (wireframe, normal lines)
3.2 Visualization Rule

For each triangle:

Step 1 — Compute geometric normal
vec3 e1 = p1 - p0;
vec3 e2 = p2 - p0;
vec3 faceNormal = normalize(cross(e1, e2));
Step 2 — Determine expected normal

Options depending on mode:

Option A (Recommended): View-based validation

vec3 viewDir = normalize(cameraPos - triangleCenter);
bool correct = dot(faceNormal, viewDir) > 0;

Option B: Vertex normal agreement

vec3 avgNormal = normalize(n0 + n1 + n2);
bool correct = dot(faceNormal, avgNormal) > threshold;
3.3 Color Output
Condition	Color
Correct	Blue (0, 0, 1)
Incorrect	Red (1, 0, 0)

Optional:

Slight shading for depth perception
Flat shading (no interpolation)
4. Rendering Pipeline Integration
4.1 Pipeline

Use a dedicated debug pipeline (you already have one):

VkPipeline geometryDebugPipeline;

Or create:

normal_validation_pipeline
4.2 Shader Behavior
Vertex Shader
Pass world-space position
Pass triangle vertices if needed (or use flat shading)
Fragment Shader (core logic)
vec3 e1 = p1 - p0;
vec3 e2 = p2 - p0;
vec3 faceNormal = normalize(cross(e1, e2));

vec3 viewDir = normalize(cameraPos - center);

bool correct = dot(faceNormal, viewDir) > 0.0;

vec3 color = correct ? vec3(0,0,1) : vec3(1,0,0);
4.3 Geometry Handling

Two approaches:

Option 1 (Fast, recommended)
Compute face normal in fragment shader using derivatives:
vec3 faceNormal = normalize(cross(dFdx(worldPos), dFdy(worldPos)));
Option 2 (Explicit, debug-heavy)
Use geometry shader to:
Access all 3 vertices
Compute per-triangle normal
5. UI Integration

Add to existing debug controls:

bool showNormalValidation = false;

GUI:

[ ] Wireframe
[ ] Normal Lines
[ ] Normal Validation  <-- NEW
6. Performance Considerations
Very low overhead (pure shader logic)
No additional buffers required
Can run in real-time even on large meshes

Optional:

Disable in release builds
7. Edge Cases
7.1 Degenerate triangles
If |cross(e1,e2)| < epsilon
→ mark as invalid (RED)
7.2 Double-sided materials
Skip validation OR treat both directions as valid
7.3 Negative scale transforms
Must use correct normal matrix:
normalMatrix = transpose(inverse(model))
8. Expected Visual Result
Clean blue mesh → correct pipeline
Red patches → issues in:
winding
normal generation
coordinate system

Exactly like your reference image:

alternating patches indicate inconsistent normals
full red mesh → fully inverted model
9. Debug Value

This feature directly helps diagnose:

Issue	Symptom
Wrong winding	checkerboard red/blue
Flipped normals	mostly red
LH/RH mismatch	systematic inversion
Bad normal generation	noisy patterns
10. Future Extensions
Add yellow for borderline cases (dot ≈ 0)
Visualize:
backface culling errors
per-vertex vs per-face mismatch
Combine with:
wireframe overlay
normal line rendering
Summary

This feature is a high-signal, low-cost debugging tool that fits perfectly into your renderer:

Uses existing pipeline structure
Requires minimal code changes
Gives immediate insight into geometry correctness