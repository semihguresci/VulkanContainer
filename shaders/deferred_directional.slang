#include "brdf_common.slang"
#include "area_light_common.slang"
#include "material_data_common.slang"
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
[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(2, 1)]] StructuredBuffer<AreaLightData> uAreaLights;
[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials;

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

float3 EvaluateDeferredAreaLight(AreaLightData light, float3 albedo,
                                 float3 F0, float metallic, float roughness,
                                 float3 normal, float3 V,
                                 float3 worldPosition,
                                 DeferredMaterialLayerTerms layerTerms,
                                 bool thinSurface, float alpha)
{
    AreaLightFrame frame = BuildAreaLightFrame(light);
    float3 directLighting = 0.0.xxx;

    float transmissionWeight =
        saturate(layerTerms.transmissionFactor * (1.0 - metallic));

    [unroll]
    for (uint sampleIndex = 0u;
         sampleIndex < AREA_LIGHT_INTEGRATION_SAMPLE_COUNT; ++sampleIndex)
    {
        float3 L;
        float3 radiance = EvaluateAreaLightSampleRadiance(
            light, frame, worldPosition, sampleIndex, L);
        directLighting += EvaluateDeferredLayeredDirectLight(
            albedo, F0, metallic, roughness, normal, V, L, radiance,
            layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
            layerTerms.sheenColor, layerTerms.sheenRoughness);

        if (thinSurface || transmissionWeight > 0.0)
        {
            float transmissionAlpha =
                thinSurface ? lerp(alpha, 0.0, transmissionWeight) : 0.0;
            float transmissionScale = thinSurface ? 1.0 : transmissionWeight;
            directLighting += EvaluateThinSurfaceTransmission(
                albedo, normal, V, L, radiance, transmissionAlpha, false) *
                transmissionScale;
        }
    }

    return directLighting;
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
    float4 emissiveSample = gEmissiveTexture.Load(int3(pixelCoord, 0));
    float4 specularSample = gSpecularTexture.Load(int3(pixelCoord, 0));
    float3 emissive = emissiveSample.rgb;
    uint materialIndex = DecodeGBufferMaterialIndex(material.w);
    GpuMaterial materialData = uMaterials[materialIndex];
    // Resolve materialData.clearcoatFactor,
    // materialData.clearcoatRoughnessFactor, materialData.sheenColorFactor.rgb,
    // materialData.sheenRoughnessFactor, materialData.iridescenceFactor, and
    // materialData.transmissionFactor with texture-only deferred fallbacks.
    DeferredMaterialLayerTerms layerTerms =
        ResolveDeferredMaterialLayerTerms(materialData);

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float occlusion = saturate(material.z);
    bool thinSurface = IsThinSurfaceGpuMaterial(materialData.flags);
    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float3 L = SafeNormalize(-uLighting.directionalDirection.xyz, float3(0.0, 1.0, 0.0));
    float3 lightColor =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;
    float NdotV = max(dot(normal, V), 0.0);

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
    if (uLighting.shadowEnabled != 0u &&
        (NdotLForShadow > 0.0 || thinSurface))
    {
        shadowFactor = ComputeShadowFactor(
            worldPosition, normal, uLighting.directionalDirection.xyz,
            viewDepth, uShadow, gShadowAtlas, gShadowSampler);
    }

    float iridescenceThickness = EstimateDeferredIridescenceThickness(
        materialData.iridescenceThicknessMinimum,
        materialData.iridescenceThicknessMaximum);
    float3 F0 = EvaluateDeferredPbrF0FromDielectricColor(
        albedo.rgb, metallic, specularSample.rgb,
        layerTerms.iridescenceFactor, materialData.iridescenceIor,
        iridescenceThickness, NdotV);
    float3 directLighting = EvaluateDeferredLayeredDirectLight(
        albedo.rgb, F0, metallic, roughness, normal, V, L, lightColor,
        layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
        layerTerms.sheenColor, layerTerms.sheenRoughness) * shadowFactor;

    float transmissionWeight =
        saturate(layerTerms.transmissionFactor * (1.0 - metallic));
    if (thinSurface || transmissionWeight > 0.0)
    {
        float transmissionAlpha =
            thinSurface ? lerp(albedo.a, 0.0, transmissionWeight) : 0.0;
        float transmissionScale = thinSurface ? 1.0 : transmissionWeight;
        directLighting += EvaluateThinSurfaceTransmission(
            albedo.rgb, normal, V, L, lightColor, transmissionAlpha, false) *
            transmissionScale * shadowFactor;
    }

    uint areaLightCount = min(uLighting.areaLightCount, MAX_AREA_LIGHTS);
    for (uint areaIndex = 0u; areaIndex < areaLightCount; ++areaIndex)
    {
        directLighting += EvaluateDeferredAreaLight(
            uAreaLights[areaIndex], albedo.rgb, F0, metallic, roughness,
            normal, V, worldPosition, layerTerms, thinSurface, albedo.a);
    }

    // IBL ambient lighting (split-sum approximation).
    float3 kS_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - kS_ibl) * (1.0 - metallic);

    float environmentIntensity = max(uLighting.environmentIntensity, 0.0);
    float3 irradiance =
        gIrradianceCubemap.Sample(envSampler, normal).rgb *
        environmentIntensity;
    float3 diffuseIBL = irradiance * albedo.rgb;

    float3 R = reflect(-V, normal);
    float maxMipLevel = max(float(uLighting.prefilteredMipCount) - 1.0, 0.0);
    float3 prefilteredColor =
        gPrefilteredCubemap.SampleLevel(envSampler, R,
                                        roughness * maxMipLevel).rgb *
        environmentIntensity;
    float2 brdf = gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness));
    float3 specularIBL =
        EvaluateSpecularIblMultipleScattering(prefilteredColor, F0, brdf);

    // Screen-space AO (GTAO).
    float ssao = 1.0;
    if (uLighting.gtaoEnabled != 0u)
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
