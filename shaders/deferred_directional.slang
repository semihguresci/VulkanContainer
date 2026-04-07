static const float PI = 3.14159265359;
static const uint MAX_POINT_LIGHTS = 4u;

struct CameraBuffer
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
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
    float3 padding;
    PointLightData pointLights[MAX_POINT_LIGHTS];
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState gBufferSampler;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float4> gPositionTexture;
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
};

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-4);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(N, V)), roughness) *
           GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

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
    float4 albedo = gAlbedoTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float3 normal = normalize(gNormalTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0).xyz * 2.0 - 1.0);
    float4 material = gMaterialTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float3 emissive = gEmissiveTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0).rgb;
    float4 positionSample = gPositionTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);

    if (positionSample.w <= 0.0 || albedo.a <= 0.0)
    {
        return float4(emissive, 1.0);
    }

    if (!all(isfinite(normal)) || length(normal) < 1e-4)
    {
        normal = float3(0.0, 1.0, 0.0);
    }

    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.045, 1.0);
    float occlusion = saturate(material.z);
    float3 worldPosition = positionSample.xyz;
    float3 V = normalize(uCamera.cameraWorldPosition.xyz - worldPosition);
    float3 L = normalize(-uLighting.directionalDirection.xyz);
    float3 H = normalize(V + L);
    float3 lightColor =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;

    float3 F0 = lerp(0.04.xxx, albedo.rgb, metallic);
    float NDF = DistributionGGX(normal, H, roughness);
    float G = GeometrySmith(normal, V, L, roughness);
    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float3 numerator = NDF * G * F;
    float denominator =
        max(4.0 * saturate(dot(normal, V)) * saturate(dot(normal, L)), 1e-4);
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float NdotL = saturate(dot(normal, L));

    float3 directLighting =
        (kD * albedo.rgb / PI + specular) * lightColor * NdotL;
    float3 ambientLighting = albedo.rgb * 0.03 * occlusion;
    float3 finalColor = ambientLighting + directLighting + emissive;

    return float4(finalColor, 1.0);
}
