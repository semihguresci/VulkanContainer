// lighting_structs.slang — Shared lighting / camera struct definitions.
// Must match the C++ LightingData and CameraData layouts exactly.
#pragma once

static const uint MAX_POINT_LIGHTS = 12u;
static const uint MAX_CLUSTERED_LIGHTS = 8192u;
static const uint TILE_SIZE = 16u;
static const uint CLUSTER_DEPTH_SLICES = 16u;
static const uint MAX_LIGHTS_PER_TILE = 128u;

struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct PointLightData
{
    float4 positionRadius;
    float4 colorIntensity;
};

struct LightingBuffer
{
    float4 directionalDirection;
    float4 directionalColorIntensity;
    uint pointLightCount;
    uint3 featureFlags;
    PointLightData pointLights[MAX_POINT_LIGHTS];
};

struct TileLightGrid
{
    uint offset;
    uint count;
};

float SanitizeClusterNear(float nearPlane)
{
    return max(nearPlane, 1e-4);
}

float SanitizeClusterFar(float nearPlane, float farPlane)
{
    return max(farPlane, nearPlane + 1e-3);
}

float LinearizeReverseZDepth(float depth, float nearPlane, float farPlane)
{
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    return (n * f) / (saturate(depth) * (f - n) + n);
}

float ViewDepthToReverseZ(float viewDepth, float nearPlane, float farPlane)
{
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    float z = clamp(viewDepth, n, f);
    return saturate(n * (f / z - 1.0) / (f - n));
}

uint DepthToClusterSlice(float depth, float nearPlane, float farPlane, uint depthSliceCount)
{
    uint safeSliceCount = max(depthSliceCount, 1u);
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    float viewDepth = clamp(LinearizeReverseZDepth(depth, n, f), n, f);
    float logRange = max(log2(f / n), 1e-6);
    float normalized = log2(viewDepth / n) / logRange;
    uint slice = uint(saturate(normalized) * float(safeSliceCount));
    return min(slice, safeSliceCount - 1u);
}

float ClusterSliceBoundaryDepth(uint sliceBoundary, float nearPlane, float farPlane, uint depthSliceCount)
{
    uint safeSliceCount = max(depthSliceCount, 1u);
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    uint safeBoundary = min(sliceBoundary, safeSliceCount);
    float t = float(safeBoundary) / float(safeSliceCount);
    float viewDepth = n * exp2(log2(f / n) * t);
    return ViewDepthToReverseZ(viewDepth, n, f);
}

static const uint SHADOW_CASCADE_COUNT = 4u;

struct ShadowCascadeData
{
    column_major float4x4 viewProj;
    float splitDepth;
    float3 padding;
};

struct ShadowBuffer
{
    ShadowCascadeData cascades[SHADOW_CASCADE_COUNT];
};

struct ShadowCascadeCullData
{
    column_major float4x4 viewProj;
    column_major float4x4 lightView;
    float4 receiverMinBounds;
    float4 receiverMaxBounds;
    float4 casterMinBounds;
    float4 casterMaxBounds;
};

struct ShadowCullBuffer
{
    ShadowCascadeCullData cascades[SHADOW_CASCADE_COUNT];
};

struct ShadowCullPushConstants
{
    uint drawCount;
    uint cascadeIndex;
    uint outputOffset;
    uint pad0;
};
