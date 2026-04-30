// brdf_common.slang — Shared PBR BRDF utilities.
// Included by deferred_directional, point_light, gbuffer_composite, etc.
#pragma once

static const float PI = 3.14159265359;

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
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
    return F0 + (1.0 - F0) * Pow5(saturate(1.0 - cosTheta));
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 maxReflectivity = max((1.0 - roughness).xxx, F0);
    return F0 + (maxReflectivity - F0) * Pow5(saturate(1.0 - cosTheta));
}

float IorToF0(float ior)
{
    if (ior <= 0.0)
    {
        return 1.0;
    }
    float safeIor = max(ior, 1.0);
    float f0 = (safeIor - 1.0) / (safeIor + 1.0);
    return saturate(f0 * f0);
}

float3 EvaluatePbrF0(float3 albedo, float metallic, float specularFactor,
                     float3 specularColorFactor, float ior)
{
    float3 dielectricF0 =
        min(IorToF0(ior).xxx * max(specularColorFactor, 0.0.xxx), 1.0.xxx) *
        saturate(specularFactor);
    return lerp(dielectricF0, albedo, saturate(metallic));
}

float EvaluateDielectricF0(float specularFactor, float3 specularColorFactor,
                           float ior)
{
    float3 color = max(specularColorFactor, 0.0.xxx);
    float colorMagnitude = max(max(color.x, color.y), color.z);
    return saturate(IorToF0(ior) * saturate(specularFactor) * colorMagnitude);
}

float3 EvaluatePbrF0FromDielectricF0(float3 albedo, float metallic,
                                     float dielectricF0)
{
    return lerp(saturate(dielectricF0).xxx, albedo, saturate(metallic));
}

float3 EvaluateSpecularIblMultipleScattering(float3 prefilteredColor,
                                             float3 F0, float2 brdf)
{
    float3 singleScatter = max(F0 * brdf.x + brdf.y, 0.0.xxx);
    float singleScatterEnergy = saturate(brdf.x + brdf.y);
    float missingEnergy = saturate(1.0 - singleScatterEnergy);
    float3 averageFresnel = F0 + (1.0.xxx - F0) * (1.0 / 21.0);
    float3 multiScatter =
        singleScatter * averageFresnel * missingEnergy /
        max(1.0.xxx - averageFresnel * missingEnergy, 1e-4.xxx);
    return prefilteredColor * (singleScatter + multiScatter);
}

float SmoothRangeAttenuation(float distanceSq, float radius)
{
    float safeRadius = max(radius, 1e-3);
    float invRadiusSq = 1.0 / (safeRadius * safeRadius);
    float normalizedSq = distanceSq * invRadiusSq;
    float smoothFactor = saturate(1.0 - normalizedSq);
    return smoothFactor * smoothFactor;
}

float3 EvaluatePbrDirectLight(float3 albedo, float3 F0, float metallic,
                              float roughness, float3 N, float3 V, float3 L,
                              float3 radiance)
{
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return 0.0.xxx;
    }

    float3 H = SafeNormalize(V + L, N);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 diffuse = (1.0 - F) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * NdotL;
}

float3 ReconstructWorldPosition(float2 texCoord, float depth,
                                column_major float4x4 inverseViewProj)
{
    // texCoord is framebuffer UV from a negative-height scene viewport. Convert
    // it back to scene NDC explicitly before applying inverse view-projection.
    float2 ndcXY = texCoord * 2.0 - 1.0;
    ndcXY.y = -ndcXY.y; // Negative-height viewport: framebuffer Y is inverted relative to NDC Y.
    float4 clipPosition = float4(ndcXY, depth, 1.0);
    float4 worldPosition = mul(inverseViewProj, clipPosition);
    return worldPosition.xyz / max(worldPosition.w, 1e-6);
}
