#include "brdf_common.slang"
#include "lighting_structs.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState gBufferSampler;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;

struct PushConstants
{
    uint outputMode;
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

    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    float3 encodedNormal = gNormalTexture.Load(int3(pixelCoord, 0)).xyz;
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float3 emissive = gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb;
    float depth = gDepthTexture.Load(int3(pixelCoord, 0));

    float3 normal = SafeNormalize(encodedNormal * 2.0 - 1.0, float3(0.0, 1.0, 0.0));

    if (pc.outputMode == 1)
    {
        return albedo;
    }

    if (pc.outputMode == 2)
    {
        return float4(normal * 0.5 + 0.5, 1.0);
    }

    if (pc.outputMode == 3)
    {
        return float4(material.xyz, 1.0);
    }

    if (pc.outputMode == 4)
    {
        float depthVisual = 1.0 - pow(saturate(depth), 48.0);
        return float4(depthVisual.xxx, 1.0);
    }

    if (pc.outputMode == 5)
    {
        return float4(emissive, 1.0);
    }

    if (depth <= 1e-6 || albedo.a <= 0.0)
    {
        return float4(emissive, 1.0);
    }

    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float3 L = SafeNormalize(float3(0.45, 0.65, 1.0), float3(0.0, 1.0, 0.0));
    float3 H = SafeNormalize(V + L, normal);
    float3 lightColor = float3(4.0, 3.8, 3.5);

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float occlusion = saturate(material.z);

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
    float3 directLighting = (kD * albedo.rgb / PI + specular) * lightColor * NdotL;
    float3 ambientLighting = albedo.rgb * 0.03 * occlusion;
    float3 finalColor = ambientLighting + directLighting + emissive;

    return float4(finalColor, 1.0);
}
