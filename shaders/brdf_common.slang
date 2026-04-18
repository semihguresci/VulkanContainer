// brdf_common.slang — Shared PBR BRDF utilities.
// Included by deferred_directional, point_light, gbuffer_composite, etc.
#pragma once

static const float PI = 3.14159265359;

float3 SafeNormalize(float3 v, float3 fallback)
{
    float lenSq = dot(v, v);
    if (!all(isfinite(v)) || lenSq < 1e-8)
    {
        return fallback;
    }
    return v * rsqrt(lenSq);
}

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

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 maxReflectivity = max((1.0 - roughness).xxx, F0);
    return F0 + (maxReflectivity - F0) * pow(1.0 - cosTheta, 5.0);
}

float3 ReconstructWorldPosition(float2 texCoord, float depth,
                                column_major float4x4 inverseViewProj)
{
    float2 ndcXY = texCoord * 2.0 - 1.0;
    ndcXY.y = -ndcXY.y; // Negative-height viewport: framebuffer Y is inverted relative to NDC Y.
    float4 clipPosition = float4(ndcXY, depth, 1.0);
    float4 worldPosition = mul(inverseViewProj, clipPosition);
    return worldPosition.xyz / max(worldPosition.w, 1e-6);
}
