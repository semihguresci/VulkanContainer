// tiled_lighting.slang — Fullscreen pass that accumulates point light
// contributions using the tile light grid built by tile_light_cull.slang.
// Replaces the per-light stencil draw loop.

#include "brdf_common.slang"
#include "lighting_structs.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState gBufferSampler;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
// Bindings 7-9 unused (shadow — not needed for point lights).

// Set 1: tile resources.
[[vk::binding(0, 1)]] StructuredBuffer<PointLightData> uLights;
[[vk::binding(1, 1)]] StructuredBuffer<TileLightGrid> uTileGrid;
[[vk::binding(2, 1)]] StructuredBuffer<uint> uLightIndexList;

struct PushConstants
{
    uint tileCountX;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
};

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 kPositions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0),
    };

    static const float2 kTexCoords[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };

    VSOutput output;
    output.pos = float4(kPositions[vertexIndex], 0.0, 1.0);
    output.texCoord = kTexCoords[vertexIndex];
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    uint width = 0u;
    uint height = 0u;
    gDepthTexture.GetDimensions(width, height);
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));
    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));

    float depth = gDepthTexture.Load(int3(pixelCoord, 0));

    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    float3 normal = SafeNormalize(gNormalTexture.Load(int3(pixelCoord, 0)).xyz * 2.0 - 1.0,
                                  float3(0.0, 1.0, 0.0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));

    if (depth == 0.0 || albedo.a <= 0.0)
    {
        return 0.0.xxxx;
    }

    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));

    // Look up the tile for this pixel.
    uint2 tileCoord = pixelCoord / TILE_SIZE;
    uint tileIndex = tileCoord.y * pc.tileCountX + tileCoord.x;
    TileLightGrid tile = uTileGrid[tileIndex];

    float3 totalLighting = 0.0.xxx;

    for (uint i = 0; i < tile.count; ++i)
    {
        uint lightIdx = uLightIndexList[tile.offset + i];
        PointLightData light = uLights[lightIdx];

        float3 lightVector = light.positionRadius.xyz - worldPosition;
        float distanceToLight = length(lightVector);
        float lightRadius = max(light.positionRadius.w, 1e-3);
        if (distanceToLight >= lightRadius)
        {
            continue;
        }

        float attenuation = pow(saturate(1.0 - distanceToLight / lightRadius), 2.0);
        float3 L = SafeNormalize(lightVector, float3(0.0, 1.0, 0.0));
        float3 H = SafeNormalize(V + L, normal);
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;

        float3 F0 = lerp(0.04.xxx, albedo.rgb, metallic);
        float NDF = DistributionGGX(normal, H, roughness);
        float G = GeometrySmith(normal, V, L, roughness);
        float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
        float3 numerator = NDF * G * F;
        float denominator = max(4.0 * saturate(dot(normal, V)) * saturate(dot(normal, L)), 1e-4);
        float3 specular = numerator / denominator;

        float3 kS = F;
        float3 kD = (1.0 - kS) * (1.0 - metallic);
        float NdotL = saturate(dot(normal, L));
        totalLighting += (kD * albedo.rgb / PI + specular) * radiance * NdotL;
    }

    return float4(totalLighting, 1.0);
}
