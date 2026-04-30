#include "brdf_common.slang"
#include "lighting_structs.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;

struct PushConstants
{
    float4 positionRadius;
    float4 colorIntensity;
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
    float dielectricF0 = saturate(gEmissiveTexture.Load(int3(pixelCoord, 0)).a);

    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));
    float3 worldPosition = ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
    float3 lightVector = pc.positionRadius.xyz - worldPosition;
    float lightRadius = max(pc.positionRadius.w, 1e-3);
    float distanceSq = dot(lightVector, lightVector);
    if (distanceSq >= lightRadius * lightRadius)
    {
        return 0.0.xxxx;
    }

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float invDistance = rsqrt(max(distanceSq, 1e-8));
    float3 L = lightVector * invDistance;
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - worldPosition, float3(0.0, 0.0, 1.0));
    float attenuation = SmoothRangeAttenuation(distanceSq, lightRadius);
    float3 radiance = pc.colorIntensity.rgb * pc.colorIntensity.a * attenuation;
    float3 F0 = EvaluatePbrF0FromDielectricF0(
        albedo.rgb, metallic, dielectricF0);

    float3 directLighting =
        EvaluatePbrDirectLight(albedo.rgb, F0, metallic, roughness,
                               normal, V, L, radiance);

    return float4(directLighting, 1.0);
}
