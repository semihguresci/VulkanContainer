// tile_light_cull.slang — Compute shader for tiled light culling.
// One workgroup per screen tile (TILE_SIZE × TILE_SIZE pixels).
// Each workgroup tests all lights against the tile frustum and builds a
// compact light index list.

#include "lighting_structs.slang"

struct TileCullPushConstants
{
    uint tileCountX;
    uint tileCountY;
    uint totalLights;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<TileCullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] Texture2D<float>             gDepthTexture;
[[vk::binding(2, 0)]] SamplerState                 gDepthSampler;
[[vk::binding(0, 1)]] StructuredBuffer<PointLightData> uLights;
[[vk::binding(0, 2)]] RWStructuredBuffer<TileLightGrid> uTileGrid;
[[vk::binding(1, 2)]] RWStructuredBuffer<uint>          uLightIndexList;

groupshared uint sharedMinDepthBits;
groupshared uint sharedMaxDepthBits;
groupshared uint sharedLightCount;
groupshared uint sharedLightIndices[MAX_LIGHTS_PER_TILE];
groupshared uint sharedGlobalOffset;

// Reconstruct view-space Z from reverse-Z depth (near=1, far=0).
// We linearize by applying the inverse projection.
float LinearizeDepth(float depth, float4x4 invViewProj)
{
    float4 clip = float4(0.0, 0.0, depth, 1.0);
    float4 world = mul(invViewProj, clip);
    return world.z / max(world.w, 1e-6);
}

// Build a frustum plane from two screen-space points and the camera origin.
// The plane normal points inward (towards the tile interior).
float4 BuildPlane(float3 p0, float3 p1, float3 eye)
{
    float3 n = normalize(cross(p1 - eye, p0 - eye));
    return float4(n, -dot(n, eye));
}

// Test a sphere against a plane. Returns true if the sphere is on the
// positive (inside) half-space or intersects the plane.
bool SpherePlaneTest(float4 plane, float3 center, float radius)
{
    return dot(plane.xyz, center) + plane.w > -radius;
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

    // Screen dimensions from depth texture.
    uint screenWidth = 0u;
    uint screenHeight = 0u;
    gDepthTexture.GetDimensions(screenWidth, screenHeight);

    // Pixel coordinate for this thread.
    uint2 pixel = uint2(tileX * TILE_SIZE + localId.x,
                        tileY * TILE_SIZE + localId.y);

    // Initialize shared memory.
    if (localIndex == 0)
    {
        sharedMinDepthBits = 0x7f7fffff; // FLT_MAX as uint
        sharedMaxDepthBits = 0;
        sharedLightCount   = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Sample depth and compute tile min/max depth range.
    float depth = 0.0;
    if (pixel.x < screenWidth && pixel.y < screenHeight)
    {
        float2 uv = (float2(pixel) + 0.5) / float2(screenWidth, screenHeight);
        depth = gDepthTexture.SampleLevel(gDepthSampler, uv, 0.0);
    }

    // Reverse-Z: near = 1.0, far = 0.0. Skip background pixels (depth == 0).
    if (depth > 0.0)
    {
        uint depthBits = asuint(depth);
        InterlockedMin(sharedMinDepthBits, depthBits);
        InterlockedMax(sharedMaxDepthBits, depthBits);
    }
    GroupMemoryBarrierWithGroupSync();

    float tileMinDepth = asfloat(sharedMinDepthBits);
    float tileMaxDepth = asfloat(sharedMaxDepthBits);

    // If no geometry in this tile, no lights to cull.
    if (tileMinDepth > tileMaxDepth)
    {
        if (localIndex == 0)
        {
            uint tileIndex = tileY * pc.tileCountX + tileX;
            uTileGrid[tileIndex].offset = 0;
            uTileGrid[tileIndex].count  = 0;
        }
        return;
    }

    // Build tile frustum planes in NDC space for a simple AABB test.
    // Tile boundaries in NDC [-1, 1].
    // Negative-height viewport: framebuffer Y is inverted relative to NDC Y.
    float tileLeft   = float(tileX * TILE_SIZE) / float(screenWidth)  * 2.0 - 1.0;
    float tileRight  = float(min((tileX + 1) * TILE_SIZE, screenWidth)) / float(screenWidth) * 2.0 - 1.0;
    float tileTop    = -(float(tileY * TILE_SIZE) / float(screenHeight) * 2.0 - 1.0);
    float tileBottom = -(float(min((tileY + 1) * TILE_SIZE, screenHeight)) / float(screenHeight) * 2.0 - 1.0);

    // Reconstruct tile frustum corners in world space.
    // Use min/max depth values for near/far planes of the tile.
    float4 corners[8];
    corners[0] = float4(tileLeft,  tileTop,    tileMinDepth, 1.0);
    corners[1] = float4(tileRight, tileTop,    tileMinDepth, 1.0);
    corners[2] = float4(tileLeft,  tileBottom, tileMinDepth, 1.0);
    corners[3] = float4(tileRight, tileBottom, tileMinDepth, 1.0);
    corners[4] = float4(tileLeft,  tileTop,    tileMaxDepth, 1.0);
    corners[5] = float4(tileRight, tileTop,    tileMaxDepth, 1.0);
    corners[6] = float4(tileLeft,  tileBottom, tileMaxDepth, 1.0);
    corners[7] = float4(tileRight, tileBottom, tileMaxDepth, 1.0);

    float3 worldCorners[8];
    for (int c = 0; c < 8; ++c)
    {
        float4 w = mul(uCamera.inverseViewProj, corners[c]);
        worldCorners[c] = w.xyz / max(w.w, 1e-6);
    }

    // Build an AABB around the world-space frustum corners for a fast
    // sphere-vs-AABB test.
    float3 aabbMin = worldCorners[0];
    float3 aabbMax = worldCorners[0];
    for (int c = 1; c < 8; ++c)
    {
        aabbMin = min(aabbMin, worldCorners[c]);
        aabbMax = max(aabbMax, worldCorners[c]);
    }

    // Each thread tests a subset of lights.
    uint threadsPerTile = TILE_SIZE * TILE_SIZE;
    for (uint i = localIndex; i < pc.totalLights; i += threadsPerTile)
    {
        float3 lightPos = uLights[i].positionRadius.xyz;
        float  lightRadius = uLights[i].positionRadius.w;

        // Conservative padding: expand light radius slightly to prevent
        // light popping at tile boundaries due to AABB approximation.
        float conservativePadding = lightRadius * 0.05;
        float testRadius = lightRadius + conservativePadding;

        // Sphere-vs-AABB test.
        float3 closest = clamp(lightPos, aabbMin, aabbMax);
        float3 diff = lightPos - closest;
        float distSq = dot(diff, diff);

        if (distSq <= testRadius * testRadius)
        {
            uint idx;
            InterlockedAdd(sharedLightCount, 1, idx);
            if (idx < MAX_LIGHTS_PER_TILE)
            {
                sharedLightIndices[idx] = i;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint lightCount = min(sharedLightCount, MAX_LIGHTS_PER_TILE);

    // Thread 0 allocates space in the global light index list.
    if (localIndex == 0)
    {
        // Simple allocation via atomic counter at uLightIndexList[0].
        // We store a running counter in the first element of the grid buffer
        // as a side channel. Actually, we use an atomic on a dedicated counter.
        // For simplicity, use offset = tileIndex * MAX_LIGHTS_PER_TILE
        // (fixed stride per tile — wastes memory but avoids atomic contention).
        uint tileIndex = tileY * pc.tileCountX + tileX;
        sharedGlobalOffset = tileIndex * MAX_LIGHTS_PER_TILE;
        uTileGrid[tileIndex].offset = sharedGlobalOffset;
        uTileGrid[tileIndex].count  = lightCount;
    }
    GroupMemoryBarrierWithGroupSync();

    // Write light indices to global list.
    for (uint i = localIndex; i < lightCount; i += threadsPerTile)
    {
        uLightIndexList[sharedGlobalOffset + i] = sharedLightIndices[i];
    }
}
