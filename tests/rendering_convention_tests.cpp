// rendering_convention_tests.cpp
//
// Validates the full winding + projection + depth convention chain used by the
// Vulkan renderer.  All tests are CPU-only (no GPU required) and operate on the
// same math the renderer uses at runtime.
//
// Convention contract being tested
// ---------------------------------
// World space  : right-handed, glTF native (+Y up, -Z forward for camera)
// View matrix  : glm::lookAt   (RH, camera looks down -Z)
// Projection   : glm::perspectiveRH_ZO(fov, aspect, FAR, NEAR)  <- near/far
//                swapped to get reverse-Z: near→1, far→0
// Y-flip       : proj[1][1] *= -1  (Vulkan NDC has Y-down)
// Front-face   : VK_FRONT_FACE_CLOCKWISE
//   Reason     : the Y-flip mirrors Y in NDC, reversing CCW→CW; CW is
//                therefore "outward-facing" in screen space.
// Depth        : cleared to 0.0f, compare GREATER_OR_EQUAL (reverse-Z)

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include "Container/common/CommonMath.h"
#include "Container/geometry/Model.h"
#include "Container/utility/Camera.h"

namespace {

// Tolerance for floating-point comparisons.
constexpr float kEps = 1e-4f;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Transform a world-space triangle through full VP and return the 2-D signed
// area in NDC (positive = CCW, negative = CW).
float ndcSignedArea(glm::vec3 w0, glm::vec3 w1, glm::vec3 w2,
                    const glm::mat4& viewProj) {
    auto project = [&](glm::vec3 w) -> glm::vec2 {
        glm::vec4 clip = viewProj * glm::vec4(w, 1.0f);
        return glm::vec2(clip) / clip.w;  // perspective divide → NDC xy
    };
    glm::vec2 p0 = project(w0);
    glm::vec2 p1 = project(w1);
    glm::vec2 p2 = project(w2);
    // 2D cross product (signed area * 2)
    return (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
}

// Build the view-projection matrix exactly as the renderer does.
glm::mat4 makeTestViewProj(glm::vec3 eye, glm::vec3 target,
                            float fovDeg = 60.0f, float aspect = 1.0f,
                            float zNear = 0.05f, float zFar = 500.0f) {
    glm::mat4 view = common::math::lookAt(eye, target, {0, 1, 0});
    // Reverse-Z: swap near/far so near→1, far→0
    glm::mat4 proj = common::math::perspectiveRH_ReverseZ(
        glm::radians(fovDeg), aspect, zNear, zFar);
    proj[1][1] *= -1.0f;  // Vulkan Y-flip
    return proj * view;
}

glm::vec4 simulateShaderRowVectorMul(const glm::mat4& uploadedMatrix,
                                     const glm::vec4& vector) {
    // In column-vector GLM notation, shader-side row-vector multiplication
    // v * M is equivalent to transpose(M) * v.
    return glm::transpose(uploadedMatrix) * vector;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Projection depth range
//    near plane vertex must map to NDC z ≈ 1 (reverse-Z)
//    far  plane vertex must map to NDC z ≈ 0 (reverse-Z)
// ---------------------------------------------------------------------------
TEST(RenderingConvention, ReverseZ_NearMapsToOne) {
    constexpr float zNear = 0.05f;
    constexpr float zFar  = 500.0f;

    glm::mat4 proj = common::math::perspectiveRH_ReverseZ(
        glm::radians(60.0f), 1.0f, zNear, zFar);

    // A point exactly at the near plane in RH view space has view_z = -zNear
    glm::vec4 nearClip = proj * glm::vec4(0.0f, 0.0f, -zNear, 1.0f);
    float ndcZ = nearClip.z / nearClip.w;
    EXPECT_NEAR(ndcZ, 1.0f, kEps) << "Near plane should map to NDC z=1 (reverse-Z)";
}

TEST(RenderingConvention, ReverseZ_FarMapsToZero) {
    constexpr float zNear = 0.05f;
    constexpr float zFar  = 500.0f;

    glm::mat4 proj = common::math::perspectiveRH_ReverseZ(
        glm::radians(60.0f), 1.0f, zNear, zFar);

    // A point at the far plane in RH view space has view_z = -zFar
    glm::vec4 farClip = proj * glm::vec4(0.0f, 0.0f, -zFar, 1.0f);
    float ndcZ = farClip.z / farClip.w;
    EXPECT_NEAR(ndcZ, 0.0f, kEps) << "Far plane should map to NDC z=0 (reverse-Z)";
}

TEST(RenderingConvention, CloserObjectHasHigherDepth) {
    constexpr float zNear = 0.05f;
    constexpr float zFar  = 500.0f;

    glm::mat4 proj = common::math::perspectiveRH_ReverseZ(
        glm::radians(60.0f), 1.0f, zNear, zFar);

    // Near object at view_z = -1
    glm::vec4 nearClip = proj * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f);
    float nearNdcZ = nearClip.z / nearClip.w;

    // Far object at view_z = -100
    glm::vec4 farClip = proj * glm::vec4(0.0f, 0.0f, -100.0f, 1.0f);
    float farNdcZ = farClip.z / farClip.w;

    EXPECT_GT(nearNdcZ, farNdcZ)
        << "Closer object must have higher depth value (GREATER_OR_EQUAL wins)";
}

// ---------------------------------------------------------------------------
// 2. Winding convention
//    A CCW-in-world-space triangle must appear CW in NDC after the Y-flip,
//    meaning the front-face convention VK_FRONT_FACE_CLOCKWISE is correct.
// ---------------------------------------------------------------------------
TEST(RenderingConvention, CCWWorldTriangle_IsCWInNDC_AfterYFlip) {
    // Camera at (0,0,3) looking at origin (down -Z).
    glm::mat4 vp = makeTestViewProj({0, 0, 3}, {0, 0, 0});

    // A simple CCW triangle in world space (XY plane, facing +Z toward camera).
    // Vertices ordered CCW when viewed from +Z:
    glm::vec3 v0(-1, -1, 0);
    glm::vec3 v1( 1, -1, 0);
    glm::vec3 v2( 0,  1, 0);

    float area = ndcSignedArea(v0, v1, v2, vp);

    // After proj[1][1]*=-1 the Y-flip reverses winding: CCW → CW (negative area).
    // VK_FRONT_FACE_CLOCKWISE treats negative-area (CW) as front-facing.
    EXPECT_LT(area, 0.0f)
        << "CCW world-space triangle must be CW (negative signed area) in NDC "
           "after Vulkan Y-flip. Front-face should be VK_FRONT_FACE_CLOCKWISE.";
}

TEST(RenderingConvention, CWWorldTriangle_IsCCWInNDC_AfterYFlip) {
    glm::mat4 vp = makeTestViewProj({0, 0, 3}, {0, 0, 0});

    // Same triangle, reversed winding (CW in world space = back-facing outward).
    glm::vec3 v0(-1, -1, 0);
    glm::vec3 v1( 0,  1, 0);
    glm::vec3 v2( 1, -1, 0);

    float area = ndcSignedArea(v0, v1, v2, vp);

    EXPECT_GT(area, 0.0f)
        << "CW world-space triangle must be CCW (positive signed area) in NDC "
           "after Vulkan Y-flip → back-facing with VK_FRONT_FACE_CLOCKWISE.";
}

// ---------------------------------------------------------------------------
// 3. Cube geometry
//    For each face: geometric normal from vertex order must match the assigned
//    normal (outward-pointing).
// ---------------------------------------------------------------------------
TEST(RenderingConvention, CubeFaces_GeometricNormalMatchesAssignedNormal) {
    const geometry::Model cube = geometry::Model::MakeCube();
    const auto& verts   = cube.vertices();
    const auto& indices = cube.indices();

    ASSERT_EQ(verts.size(), 24u)   << "Expected 24 vertices (4 per face)";
    ASSERT_EQ(indices.size(), 36u) << "Expected 36 indices (6 per face)";

    constexpr float kDotThreshold = 0.99f;  // must be nearly identical direction

    for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
        const auto& v0 = verts[indices[tri]];
        const auto& v1 = verts[indices[tri + 1]];
        const auto& v2 = verts[indices[tri + 2]];

        glm::vec3 geomNormal = glm::normalize(
            glm::cross(v1.position - v0.position,
                       v2.position - v0.position));

        // All three vertices of a flat face share the same assigned normal.
        glm::vec3 assignedNormal = glm::normalize(v0.normal);

        float dot = glm::dot(geomNormal, assignedNormal);
        EXPECT_GT(dot, kDotThreshold)
            << "Triangle " << tri / 3
            << ": geometric normal " << geomNormal.x << "," << geomNormal.y << "," << geomNormal.z
            << " does not match assigned normal " << assignedNormal.x << "," << assignedNormal.y << "," << assignedNormal.z
            << " (dot=" << dot << "). Face is inside-out or normal is wrong.";
    }
}

// ---------------------------------------------------------------------------
// 4. Cube face normals point away from origin (outward)
// ---------------------------------------------------------------------------
TEST(RenderingConvention, CubeFaces_NormalsPointOutward) {
    const geometry::Model cube = geometry::Model::MakeCube();
    const auto& verts   = cube.vertices();
    const auto& indices = cube.indices();

    for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
        const auto& v0 = verts[indices[tri]];
        const auto& v1 = verts[indices[tri + 1]];
        const auto& v2 = verts[indices[tri + 2]];

        glm::vec3 centroid = (v0.position + v1.position + v2.position) / 3.0f;
        glm::vec3 normal   = glm::normalize(v0.normal);

        // The centroid of an outward face is on the same side as its normal
        // (dot > 0 when measured from origin).
        float dot = glm::dot(normal, centroid);
        EXPECT_GT(dot, 0.0f)
            << "Triangle " << tri / 3
            << ": face normal points inward (dot=" << dot << ").";
    }
}

// ---------------------------------------------------------------------------
// 5. Cube front-face visible from outside
//    With the renderer's VP, a vertex in front of the +Z face must have
//    a positive (non-zero) clip-w and produce a CW triangle in NDC.
// ---------------------------------------------------------------------------
TEST(RenderingConvention, CubePlusZFace_IsVisibleAndCWInNDC) {
    // Camera at (0,0,3) looking toward origin — exactly the diagnostic setup.
    glm::mat4 vp = makeTestViewProj({0, 0, 3}, {0, 0, 0});

    // +Z face: v0=(-0.5,-0.5,0.5), v1=(0.5,-0.5,0.5), v2=(0.5,0.5,0.5)
    // (first triangle of the +Z face as authored in MakeCube)
    glm::vec3 v0(-0.5f, -0.5f, 0.5f);
    glm::vec3 v1( 0.5f, -0.5f, 0.5f);
    glm::vec3 v2( 0.5f,  0.5f, 0.5f);

    // Vertices must be in front of the camera (positive clip-w).
    auto clipW = [&](glm::vec3 w) {
        return (vp * glm::vec4(w, 1.0f)).w;
    };
    EXPECT_GT(clipW(v0), 0.0f) << "+Z face v0 behind camera";
    EXPECT_GT(clipW(v1), 0.0f) << "+Z face v1 behind camera";
    EXPECT_GT(clipW(v2), 0.0f) << "+Z face v2 behind camera";

    // Must be CW in NDC (front-facing with VK_FRONT_FACE_CLOCKWISE).
    float area = ndcSignedArea(v0, v1, v2, vp);
    EXPECT_LT(area, 0.0f)
        << "+Z face must be CW (negative signed area) in NDC — "
           "front-facing for VK_FRONT_FACE_CLOCKWISE.";
}

// ---------------------------------------------------------------------------
// 6. Depth clear value is correct for reverse-Z
// ---------------------------------------------------------------------------
TEST(RenderingConvention, DepthClear_IsZero_ForReverseZ) {
    // In reverse-Z: near=1, far=0. An empty pixel that was never drawn has
    // depth=0 (far). The clear value must be 0.0f.
    constexpr float kExpectedClearDepth = 0.0f;
    // This test documents the required constant — if someone changes the clear
    // value the companion pipeline change (LESS vs GREATER) must also change.
    EXPECT_FLOAT_EQ(kExpectedClearDepth, 0.0f)
        << "Reverse-Z depth clear must be 0.0f (far=0 convention).";
}

// ---------------------------------------------------------------------------
// 7. Camera::viewMatrix() basis vectors (RH)
//    With yaw=90/pitch=0 the camera looks straight down -Z.
//    Cube at origin from (0,0,3) must project symmetrically to NDC.
// ---------------------------------------------------------------------------
TEST(RenderingConvention, Camera_FrontVector_Yaw90_Pitch0_IsNegativeZ) {
    utility::camera::PerspectiveCamera cam;
    cam.setYawPitch(90.0f, 0.0f);
    glm::vec3 front = cam.frontVector();
    EXPECT_NEAR(front.x, 0.0f,  kEps) << "front.x should be 0 for yaw=90";
    EXPECT_NEAR(front.y, 0.0f,  kEps) << "front.y should be 0 for pitch=0";
    EXPECT_NEAR(front.z, -1.0f, kEps) << "front.z should be -1 (RH -Z forward)";
}

TEST(RenderingConvention, Camera_UpVector_IsWorldUp_WhenLookingDownNegZ) {
    utility::camera::PerspectiveCamera cam;
    cam.setYawPitch(90.0f, 0.0f);
    glm::vec3 front = cam.frontVector();
    glm::vec3 up    = cam.upVector(front);
    EXPECT_NEAR(up.x, 0.0f, kEps) << "up.x should be 0";
    EXPECT_NEAR(up.y, 1.0f, kEps) << "up.y should be 1 (world up)";
    EXPECT_NEAR(up.z, 0.0f, kEps) << "up.z should be 0";
}

TEST(RenderingConvention, Camera_CubeAtOrigin_ProjectsSymmetrically) {
    // Camera at (0,0,3) looking at origin — diagnostic cube setup.
    utility::camera::PerspectiveCamera cam;
    cam.setYawPitch(90.0f, 0.0f);
    cam.setPosition({0.0f, 0.0f, 3.0f});

    constexpr float aspect = 16.0f / 9.0f;
    glm::mat4 vp = cam.viewProjection(aspect);

    // Cube corners at x=+/-0.5, y=+/-0.5 must be symmetric in NDC.
    auto ndcXY = [&](glm::vec3 w) -> glm::vec2 {
        glm::vec4 clip = vp * glm::vec4(w, 1.0f);
        return glm::vec2(clip) / clip.w;
    };

    glm::vec2 ppp = ndcXY({ 0.5f,  0.5f, 0.0f});
    glm::vec2 pmm = ndcXY({ 0.5f, -0.5f, 0.0f});
    glm::vec2 mpp = ndcXY({-0.5f,  0.5f, 0.0f});

    // x must be symmetric: (+0.5,y,z) -> +x_ndc, (-0.5,y,z) -> -x_ndc
    EXPECT_NEAR(ppp.x, -mpp.x, kEps) << "Cube should be centred: +x and -x NDC must be symmetric";
    // y must be symmetric: (x,+0.5,z) -> y_ndc, (x,-0.5,z) -> -y_ndc (Y-flipped)
    EXPECT_NEAR(ppp.y, -pmm.y, kEps) << "Cube should be centred: +y and -y NDC must be symmetric";
    // Centre of cube (0,0,0) must project to NDC (0,0)
    glm::vec2 centre = ndcXY({0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(centre.x, 0.0f, kEps) << "Cube centre must project to NDC x=0";
    EXPECT_NEAR(centre.y, 0.0f, kEps) << "Cube centre must project to NDC y=0";
}

TEST(RenderingConvention, ShaderBoundary_TransposedUploadsMatchCpuColumnVectorMath) {
    const glm::mat4 model =
        glm::translate(glm::mat4(1.0f), {1.25f, -0.75f, 0.5f}) *
        glm::rotate(glm::mat4(1.0f), glm::radians(37.0f),
                    glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))) *
        glm::scale(glm::mat4(1.0f), {1.5f, 0.75f, 2.0f});
    const glm::mat4 viewProj =
        makeTestViewProj({1.5f, 2.0f, 4.0f}, {0.25f, -0.1f, 0.0f}, 57.0f, 16.0f / 9.0f);
    const glm::vec4 position{0.4f, -0.2f, 0.7f, 1.0f};

    const glm::vec4 expectedClip = viewProj * model * position;

    const glm::mat4 uploadedModel = glm::transpose(model);
    const glm::mat4 uploadedViewProj = glm::transpose(viewProj);
    const glm::vec4 shaderWorld =
        simulateShaderRowVectorMul(uploadedModel, position);
    const glm::vec4 shaderClip =
        simulateShaderRowVectorMul(uploadedViewProj, shaderWorld);

    EXPECT_NEAR(shaderClip.x, expectedClip.x, kEps);
    EXPECT_NEAR(shaderClip.y, expectedClip.y, kEps);
    EXPECT_NEAR(shaderClip.z, expectedClip.z, kEps);
    EXPECT_NEAR(shaderClip.w, expectedClip.w, kEps);
}

TEST(RenderingConvention, ShaderBoundary_UntransposedUploadsDoNotMatchCpuMath) {
    const glm::mat4 model =
        glm::translate(glm::mat4(1.0f), {1.25f, -0.75f, 0.5f}) *
        glm::rotate(glm::mat4(1.0f), glm::radians(37.0f),
                    glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))) *
        glm::scale(glm::mat4(1.0f), {1.5f, 0.75f, 2.0f});
    const glm::mat4 viewProj =
        makeTestViewProj({1.5f, 2.0f, 4.0f}, {0.25f, -0.1f, 0.0f}, 57.0f, 16.0f / 9.0f);
    const glm::vec4 position{0.4f, -0.2f, 0.7f, 1.0f};

    const glm::vec4 expectedClip = viewProj * model * position;
    const glm::vec4 shaderWorld =
        simulateShaderRowVectorMul(model, position);
    const glm::vec4 shaderClip =
        simulateShaderRowVectorMul(viewProj, shaderWorld);

    const float delta = glm::length(shaderClip - expectedClip);
    EXPECT_GT(delta, 1.0f)
        << "Uploading CPU column-vector matrices without transposition should "
           "not match the compiled shader boundary math.";
}
