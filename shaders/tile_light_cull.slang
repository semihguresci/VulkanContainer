// Clustered light cull.
// One workgroup per screen tile and depth slice. Each group builds a compact
// light list for a 16x16xZ screen-space cluster.

#include "lighting_structs.slang"

struct TileCullPushConstants
{
    uint tileCountX;
    uint tileCountY;
    uint depthSliceCount;
    uint totalLights;
    float cameraNear;
    float cameraFar;
};

[[vk::push_constant]]
ConstantBuffer<TileCullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] Texture2D<float>             gDepthTexture;
[[vk::binding(2, 0)]] SamplerState                 gDepthSampler;
[[vk::binding(0, 1)]] StructuredBuffer<PointLightData> uLights;
[[vk::binding(0, 2)]] RWStructuredBuffer<TileLightGrid> uTileGrid;
[[vk::binding(1, 2)]] RWStructuredBuffer<uint>          uLightIndexList;
[[vk::binding(2, 2)]] RWStructuredBuffer<uint>          uLightStats;

groupshared uint sharedVisiblePixelCount;
groupshared uint sharedLightCount;
groupshared uint sharedLightIndices[MAX_LIGHTS_PER_TILE];
groupshared uint sharedGlobalOffset;

bool SphereIntersectsAabb(float3 center, float radius, float3 aabbMin, float3 aabbMax)
{
    float3 closest = clamp(center, aabbMin, aabbMax);
    float3 diff = center - closest;
    return dot(diff, diff) <= radius * radius;
}

[shader("compute")]
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void computeMain(
    uint3 groupId    : SV_GroupID,
    uint3 localId    : SV_GroupThreadID,
    uint  localIndex : SV_GroupIndex)
{
    uint tileX = groupId.x;
    uint tileY = groupId.y;
    uint sliceZ = groupId.z;

    uint screenWidth = 0u;
    uint screenHeight = 0u;
    gDepthTexture.GetDimensions(screenWidth, screenHeight);

    uint safeDepthSliceCount = max(pc.depthSliceCount, 1u);
    uint clusterIndex =
        (sliceZ * pc.tileCountY + tileY) * pc.tileCountX + tileX;

    if (localIndex == 0)
    {
        sharedVisiblePixelCount = 0u;
        sharedLightCount = 0u;
        sharedGlobalOffset = clusterIndex * MAX_LIGHTS_PER_TILE;
        uTileGrid[clusterIndex].offset = sharedGlobalOffset;
        uTileGrid[clusterIndex].count = 0u;
        if (clusterIndex == 0u)
        {
            uLightStats[3] = pc.totalLights;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 pixel = uint2(tileX * TILE_SIZE + localId.x,
                        tileY * TILE_SIZE + localId.y);

    if (pixel.x < screenWidth && pixel.y < screenHeight)
    {
        float2 uv = (float2(pixel) + 0.5.xx) /
                    float2(max(screenWidth, 1u), max(screenHeight, 1u));
        float depth = gDepthTexture.SampleLevel(gDepthSampler, uv, 0.0);
        if (depth > 0.0 &&
            DepthToClusterSlice(depth, pc.cameraNear, pc.cameraFar,
                                safeDepthSliceCount) == sliceZ)
        {
            uint ignored;
            InterlockedAdd(sharedVisiblePixelCount, 1u, ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (sharedVisiblePixelCount == 0u)
    {
        return;
    }

    float tileLeft =
        float(tileX * TILE_SIZE) / float(max(screenWidth, 1u)) * 2.0 - 1.0;
    float tileRight =
        float(min((tileX + 1u) * TILE_SIZE, screenWidth)) /
            float(max(screenWidth, 1u)) * 2.0 - 1.0;
    float tileTop =
        -(float(tileY * TILE_SIZE) / float(max(screenHeight, 1u)) * 2.0 - 1.0);
    float tileBottom =
        -(float(min((tileY + 1u) * TILE_SIZE, screenHeight)) /
              float(max(screenHeight, 1u)) * 2.0 - 1.0);

    float sliceNearDepth = ClusterSliceBoundaryDepth(
        sliceZ, pc.cameraNear, pc.cameraFar, safeDepthSliceCount);
    float sliceFarDepth = ClusterSliceBoundaryDepth(
        sliceZ + 1u, pc.cameraNear, pc.cameraFar, safeDepthSliceCount);

    float4 corners[8];
    corners[0] = float4(tileLeft,  tileTop,    sliceNearDepth, 1.0);
    corners[1] = float4(tileRight, tileTop,    sliceNearDepth, 1.0);
    corners[2] = float4(tileLeft,  tileBottom, sliceNearDepth, 1.0);
    corners[3] = float4(tileRight, tileBottom, sliceNearDepth, 1.0);
    corners[4] = float4(tileLeft,  tileTop,    sliceFarDepth, 1.0);
    corners[5] = float4(tileRight, tileTop,    sliceFarDepth, 1.0);
    corners[6] = float4(tileLeft,  tileBottom, sliceFarDepth, 1.0);
    corners[7] = float4(tileRight, tileBottom, sliceFarDepth, 1.0);

    float3 worldCorners[8];
    for (int c = 0; c < 8; ++c)
    {
        float4 w = mul(uCamera.inverseViewProj, corners[c]);
        worldCorners[c] = w.xyz / max(w.w, 1e-6);
    }

    float3 aabbMin = worldCorners[0];
    float3 aabbMax = worldCorners[0];
    for (int c = 1; c < 8; ++c)
    {
        aabbMin = min(aabbMin, worldCorners[c]);
        aabbMax = max(aabbMax, worldCorners[c]);
    }

    uint threadsPerCluster = TILE_SIZE * TILE_SIZE;
    for (uint i = localIndex; i < pc.totalLights; i += threadsPerCluster)
    {
        float3 lightPos = uLights[i].positionRadius.xyz;
        float lightRadius = uLights[i].positionRadius.w * 1.03;

        if (SphereIntersectsAabb(lightPos, lightRadius, aabbMin, aabbMax))
        {
            uint idx;
            InterlockedAdd(sharedLightCount, 1u, idx);
            if (idx < MAX_LIGHTS_PER_TILE)
            {
                sharedLightIndices[idx] = i;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint lightCount = min(sharedLightCount, MAX_LIGHTS_PER_TILE);
    if (localIndex == 0)
    {
        uTileGrid[clusterIndex].count = lightCount;
        uint dropped = sharedLightCount > MAX_LIGHTS_PER_TILE
                           ? sharedLightCount - MAX_LIGHTS_PER_TILE
                           : 0u;
        uint ignored;
        InterlockedMax(uLightStats[0], lightCount, ignored);
        InterlockedAdd(uLightStats[1], dropped, ignored);
        InterlockedAdd(uLightStats[2], 1u, ignored);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint i = localIndex; i < lightCount; i += threadsPerCluster)
    {
        uLightIndexList[sharedGlobalOffset + i] = sharedLightIndices[i];
    }
}
