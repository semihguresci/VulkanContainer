static const float PI = 3.14159265359;

struct CameraBuffer
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

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
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float3 SafeNormalize(float3 v, float3 fallback)
{
    float lenSq = dot(v, v);
    if (!all(isfinite(v)) || lenSq < 1e-8)
    {
        return fallback;
    }
    return v * rsqrt(lenSq);
}

float3 ReconstructWorldPosition(float2 texCoord, float depth)
{
    float2 ndcXY = texCoord * 2.0 - 1.0;
    float4 clipPosition = float4(ndcXY, depth, 1.0);
    float4 worldPosition = mul(uCamera.inverseViewProj, clipPosition);
    return worldPosition.xyz / max(worldPosition.w, 1e-6);
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
    float3 encodedNormal = gNormalTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0).xyz;
    float4 material = gMaterialTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float3 emissive = gEmissiveTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0).rgb;
    float depth = gDepthTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);

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

    float3 worldPosition = ReconstructWorldPosition(input.texCoord, depth);
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
