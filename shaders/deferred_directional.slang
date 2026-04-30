#include "brdf_common.slang"
#include "shadow_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
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
    // Use integer pixel loads for the G-buffer to avoid filtering material IDs,
    // normal encodings, and depth across geometry edges.
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));
    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));

    float depth = gDepthTexture.Load(int3(pixelCoord, 0));

    if (depth == 0.0)
    {
        // Background pixel: no opaque geometry (sky, wall holes, frame cutouts,
        // etc.). Sample the environment cubemap along the view ray so we get a
        // proper sky color instead of a black hole. Using mip 0 of the
        // prefiltered cubemap gives the sharpest (lowest-roughness) sky.
        float3 nearWorld = ReconstructWorldPosition(uv, 1.0, uCamera.inverseViewProj);
        float3 farWorld  = ReconstructWorldPosition(uv, 1e-4, uCamera.inverseViewProj);
        float3 viewDir = SafeNormalize(farWorld - nearWorld, float3(0.0, 0.0, -1.0));
        float3 skyColor = gPrefilteredCubemap.SampleLevel(envSampler, viewDir, 0.0).rgb;
        return float4(skyColor + gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb, 1.0);
    }

    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    if (albedo.a <= 0.0)
    {
        float3 nearWorld = ReconstructWorldPosition(uv, 1.0, uCamera.inverseViewProj);
        float3 farWorld  = ReconstructWorldPosition(uv, 1e-4, uCamera.inverseViewProj);
        float3 viewDir = SafeNormalize(farWorld - nearWorld, float3(0.0, 0.0, -1.0));
        float3 skyColor = gPrefilteredCubemap.SampleLevel(envSampler, viewDir, 0.0).rgb;
        return float4(skyColor + gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb, 1.0);
    }

    float3 normal = SafeNormalize(gNormalTexture.Load(int3(pixelCoord, 0)).xyz * 2.0 - 1.0,
                                  float3(0.0, 1.0, 0.0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float3 emissive = gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb;

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float occlusion = saturate(material.z);
    bool thinSurface = material.w > 0.5;
    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float3 L = SafeNormalize(-uLighting.directionalDirection.xyz, float3(0.0, 1.0, 0.0));
    float3 lightColor =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;

    // Positive view-axis depth selects the cascade. Camera forward is uploaded
    // once per frame; reconstructing it here would cost two matrix transforms
    // for every lit pixel.
    float3 cameraForward =
        SafeNormalize(uCamera.cameraForward.xyz, float3(0.0, 0.0, -1.0));
    float viewDepth =
        max(dot(worldPosition - uCamera.cameraWorldPosition.xyz, cameraForward), 0.0);

    // Shadow evaluation.
    float shadowFactor = 1.0;
    float NdotLForShadow = dot(normal, L);
    if (uLighting.featureFlags.x != 0u &&
        (NdotLForShadow > 0.0 || thinSurface))
    {
        shadowFactor = ComputeShadowFactor(
            worldPosition, normal, uLighting.directionalDirection.xyz,
            viewDepth, uShadow, gShadowAtlas, gShadowSampler);
    }

    float3 F0 = lerp(0.04.xxx, albedo.rgb, metallic);
    float3 directLighting = EvaluatePbrDirectLight(
        albedo.rgb, F0, metallic, roughness, normal, V, L, lightColor) * shadowFactor;
    if (thinSurface)
    {
        float wrappedDiffuse = saturate((dot(normal, L) + 0.45) / 1.45);
        float backLight = saturate(dot(-normal, L));
        float viewThrough = pow(saturate(dot(-L, V)), 2.0);
        float transmission = max(wrappedDiffuse * 0.3, backLight * viewThrough * 0.7);
        directLighting += albedo.rgb * lightColor * transmission * shadowFactor;
    }

    // IBL ambient lighting (split-sum approximation).
    float NdotV = max(dot(normal, V), 0.0);
    float3 kS_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - kS_ibl) * (1.0 - metallic);

    float3 irradiance = gIrradianceCubemap.Sample(envSampler, normal).rgb;
    float3 diffuseIBL = irradiance * albedo.rgb;

    float3 R = reflect(-V, normal);
    float maxMipLevel = max(float(uLighting.prefilteredMipCount) - 1.0, 0.0);
    float3 prefilteredColor = gPrefilteredCubemap.SampleLevel(envSampler, R, roughness * maxMipLevel).rgb;
    float2 brdf = gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness));
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

    // Screen-space AO (GTAO).
    float ssao = 1.0;
    if (uLighting.featureFlags.y != 0u)
    {
        ssao = gAOTexture.Sample(aoSampler, uv);
    }

    float diffuseAO = occlusion * ssao;
    float specularOcclusion = saturate(pow(NdotV + diffuseAO, exp2(-16.0 * roughness - 1.0)) - 1.0 + diffuseAO);
    specularOcclusion = lerp(1.0, specularOcclusion, saturate(1.0 - roughness));
    float3 ambientLighting = kD_ibl * diffuseIBL * diffuseAO + specularIBL * specularOcclusion;
    float3 finalColor = ambientLighting + directLighting + emissive;

    return float4(finalColor, 1.0);
}
