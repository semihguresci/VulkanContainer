// lighting_structs.slang — Shared lighting / camera struct definitions.
// Must match the C++ LightingData and CameraData layouts exactly.
#pragma once

static const uint MAX_POINT_LIGHTS = 4u;
static const uint MAX_CLUSTERED_LIGHTS = 4096u;
static const uint TILE_SIZE = 16u;
static const uint MAX_LIGHTS_PER_TILE = 256u;

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
