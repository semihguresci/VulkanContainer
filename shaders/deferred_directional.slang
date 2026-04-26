#include "brdf_common.slang"
#include "shadow_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState gBufferSampler;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
[[vk::binding(7, 0)]] ConstantBuffer<ShadowBuffer> uShadow;
[[vk::binding(8, 0)]] Texture2DArray<float> gShadowAtlas;
[[vk::binding(9, 0)]] SamplerComparisonState gShadowSampler;
[[vk::binding(10, 0)]] TextureCube<float4> gIrradianceCubemap;
[[vk::binding(11, 0)]] TextureCube<float4> gPrefilteredCubemap;
[[vk::binding(12, 0)]] Texture2D<float2> gBrdfLUT;
[[vk::binding(13, 0)]] SamplerState envSampler;
[[vk::binding(14, 0)]] SamplerState brdfSampler;
[[vk::binding(15, 0)]] Texture2D<float> gAOTexture;
[[vk::binding(16, 0)]] SamplerState aoSampler;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;

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
    float3 emissive = gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb;

    if (depth == 0.0 || albedo.a <= 0.0)
    {
        // Background pixel: no opaque geometry (sky, wall holes, frame cutouts,
        // etc.). Sample the environment cubemap along the view ray so we get a
        // proper sky color instead of a black hole. Using mip 0 of the
        // prefiltered cubemap gives the sharpest (lowest-roughness) sky.
        float3 nearWorld = ReconstructWorldPosition(uv, 1.0, uCamera.inverseViewProj);
        float3 farWorld  = ReconstructWorldPosition(uv, 1e-4, uCamera.inverseViewProj);
        float3 viewDir = SafeNormalize(farWorld - nearWorld, float3(0.0, 0.0, -1.0));
        float3 skyColor = gPrefilteredCubemap.SampleLevel(envSampler, viewDir, 0.0).rgb;
        return float4(skyColor + emissive, 1.0);
    }

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float occlusion = saturate(material.z);
    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float3 L = SafeNormalize(-uLighting.directionalDirection.xyz, float3(0.0, 1.0, 0.0));
    float3 H = SafeNormalize(V + L, normal);
    float3 lightColor =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;

    // Compute view-space depth for cascade selection.
    // For RH perspective, clip.w = -viewZ = positive linear distance from camera.
    float4 clipPos = mul(uCamera.viewProj, float4(worldPosition, 1.0));
    float viewDepth = clipPos.w;

    // Shadow evaluation.
    float shadowFactor = 1.0;
    if (uLighting.featureFlags.x != 0u)
    {
        shadowFactor = ComputeShadowFactor(
            worldPosition, normal, uLighting.directionalDirection.xyz,
            viewDepth, uShadow, gShadowAtlas, gShadowSampler);
    }

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
    float3 directLighting = (kD * albedo.rgb / PI + specular) * lightColor * NdotL * shadowFactor;

    // IBL ambient lighting (split-sum approximation).
    float NdotV = max(dot(normal, V), 0.0);
    float3 kS_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - kS_ibl) * (1.0 - metallic);

    float3 irradiance = gIrradianceCubemap.Sample(envSampler, normal).rgb;
    float3 diffuseIBL = irradiance * albedo.rgb;

    float3 R = reflect(-V, normal);
    float maxMipLevel = 4.0; // 5 mip levels: 0..4
    float3 prefilteredColor = gPrefilteredCubemap.SampleLevel(envSampler, R, roughness * maxMipLevel).rgb;
    float2 brdf = gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness));
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    // Screen-space AO (GTAO).
    float ssao = 1.0;
    if (uLighting.featureFlags.y != 0u)
    {
        ssao = gAOTexture.Sample(aoSampler, uv);
    }

    float3 ambientLighting = (kD_ibl * diffuseIBL + specularIBL) * occlusion * ssao;
    float3 finalColor = ambientLighting + directLighting + emissive;

    return float4(finalColor, 1.0);
}
