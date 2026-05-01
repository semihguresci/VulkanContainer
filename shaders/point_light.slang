#include "brdf_common.slang"
#include "lighting_structs.slang"
#include "material_data_common.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture;
[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials;

[[vk::push_constant]]
ConstantBuffer<LightPushConstants> pc;

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

    float depth = gDepthTexture.Load(int3(pixelCoord, 0));
    if (depth == 0.0)
    {
        return 0.0.xxxx;
    }

    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    if (albedo.a <= 0.0)
    {
        return 0.0.xxxx;
    }

    float3 normal = SafeNormalize(gNormalTexture.Load(int3(pixelCoord, 0)).xyz * 2.0 - 1.0,
                                  float3(0.0, 1.0, 0.0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float4 specularSample = gSpecularTexture.Load(int3(pixelCoord, 0));
    uint materialIndex = DecodeGBufferMaterialIndex(material.w);
    GpuMaterial materialData = uMaterials[materialIndex];
    // Resolve materialData.clearcoatFactor,
    // materialData.clearcoatRoughnessFactor, materialData.sheenColorFactor.rgb,
    // materialData.sheenRoughnessFactor, materialData.iridescenceFactor, and
    // materialData.transmissionFactor with texture-only deferred fallbacks.
    DeferredMaterialLayerTerms layerTerms =
        ResolveDeferredMaterialLayerTerms(materialData);
    bool thinSurface = IsThinSurfaceGpuMaterial(materialData.flags);

    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));
    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float3 lightVector = pc.positionRadius.xyz - worldPosition;
    float lightRadius = pc.positionRadius.w;
    float distanceSq = dot(lightVector, lightVector);
    if (PointLightHasFiniteRange(lightRadius) &&
        distanceSq >= lightRadius * lightRadius)
    {
        return 0.0.xxxx;
    }

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float invDistance = rsqrt(max(distanceSq, 1e-8));
    float3 L = lightVector * invDistance;
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float attenuation = PointLightAttenuation(distanceSq, lightRadius);
    float coneAttenuation =
        SpotLightConeAttenuation(pc.directionInnerCos, pc.coneOuterCosType, L);
    float3 radiance =
        pc.colorIntensity.rgb * pc.colorIntensity.a * attenuation *
        coneAttenuation;
    float NdotV = max(dot(normal, V), 0.0);
    float iridescenceThickness = EstimateDeferredIridescenceThickness(
        materialData.iridescenceThicknessMinimum,
        materialData.iridescenceThicknessMaximum);
    float3 F0 = EvaluateDeferredPbrF0FromDielectricColor(
        albedo.rgb, metallic, specularSample.rgb,
        layerTerms.iridescenceFactor, materialData.iridescenceIor,
        iridescenceThickness, NdotV);

    float3 directLighting =
        EvaluateDeferredLayeredDirectLight(
            albedo.rgb, F0, metallic, roughness, normal, V, L, radiance,
            layerTerms.clearcoatFactor, layerTerms.clearcoatRoughness,
            layerTerms.sheenColor, layerTerms.sheenRoughness);

    float transmissionWeight =
        saturate(layerTerms.transmissionFactor * (1.0 - metallic));
    if (thinSurface || transmissionWeight > 0.0)
    {
        float transmissionAlpha =
            thinSurface ? lerp(albedo.a, 0.0, transmissionWeight) : 0.0;
        float transmissionScale = thinSurface ? 1.0 : transmissionWeight;
        directLighting += EvaluateThinSurfaceTransmission(
            albedo.rgb, normal, V, L, radiance, transmissionAlpha, false) *
            transmissionScale;
    }

    return float4(directLighting, 1.0);
}
