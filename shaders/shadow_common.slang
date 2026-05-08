// shadow_common.slang — Shared shadow sampling functions.
// Included by deferred_directional.slang for CSM shadow evaluation.
#pragma once

#include "lighting_structs.slang"

float ShadowFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 ShadowNdcToUv(float2 shadowNdcXY)
{
    // Shadow cascades render with a positive-height viewport, so the atlas
    // keeps Vulkan's default framebuffer orientation and does not need the
    // scene pass Y inversion used for negative-height viewports.
    return shadowNdcXY * 0.5 + 0.5;
}

float ComputeReceiverPlaneDepthBias(float3 shadowNDC, ShadowBuffer shadowData)
{
    float2 dz = float2(ShadowFiniteOr(ddx(shadowNDC.z), 0.0),
                       ShadowFiniteOr(ddy(shadowNDC.z), 0.0));
    float scale = max(ShadowFiniteOr(shadowData.biasSettings.w, 0.0), 0.0);
    float maxBias = max(ShadowFiniteOr(shadowData.filterSettings.w, 0.0), 0.0);
    return min(dot(abs(dz), 1.0.xx) * scale, maxBias);
}

float ComputeSlopeScaledBias(float3 normal, float3 lightDir,
                             ShadowBuffer shadowData)
{
    float NdotL = saturate(ShadowFiniteOr(dot(normal, -lightDir), 0.0));
    float slope = sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.12);
    float constantBias = max(ShadowFiniteOr(shadowData.filterSettings.z, 0.0),
                             0.0);
    float slopeScale = max(ShadowFiniteOr(shadowData.biasSettings.z, 0.0),
                           0.0);
    float maxBias =
        max(ShadowFiniteOr(shadowData.filterSettings.w, constantBias), 0.0);
    return min(constantBias + slope * slopeScale, maxBias);
}

float3 OffsetShadowSamplePosition(
    float3 worldPosition,
    float3 normal,
    float3 lightDir,
    uint cascadeIndex,
    ShadowBuffer shadowData)
{
    float minNormalBiasTexels =
        max(ShadowFiniteOr(shadowData.biasSettings.x, 0.0), 0.0);
    float maxNormalBiasTexels =
        max(ShadowFiniteOr(shadowData.biasSettings.y, minNormalBiasTexels),
            minNormalBiasTexels);
    float normalBiasTexels = lerp(
        minNormalBiasTexels,
        maxNormalBiasTexels,
        saturate(1.0 - ShadowFiniteOr(dot(normal, -lightDir), 0.0)));
    float normalBias =
        max(ShadowFiniteOr(shadowData.cascades[cascadeIndex].texelSize, 0.0),
            0.0) *
        normalBiasTexels;
    return worldPosition + normal * normalBias;
}

bool ProjectShadowCascade(
    float3 samplePos,
    uint cascadeIndex,
    ShadowBuffer shadowData,
    out float3 shadowNDC)
{
    shadowNDC = 0.0.xxx;

    float4 shadowClip =
        mul(shadowData.cascades[cascadeIndex].viewProj, float4(samplePos, 1.0));
    if (!all(isfinite(shadowClip)) || shadowClip.w <= 1e-6)
    {
        return false;
    }

    float3 projectedNDC = shadowClip.xyz / shadowClip.w;
    if (!all(isfinite(projectedNDC)))
    {
        return false;
    }

    shadowNDC = projectedNDC;
    return true;
}

float SampleWeightedPcf(
    float2 shadowUV,
    uint cascadeIndex,
    float compareDepth,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    if (!isfinite(compareDepth))
    {
        return 1.0;
    }

    compareDepth = saturate(compareDepth);
    float filterRadiusTexels =
        max(ShadowFiniteOr(shadowData.filterSettings.x, 0.0), 0.0);
    ShadowCascadeData cascade = shadowData.cascades[cascadeIndex];
    float cascadeExtent =
        max(ShadowFiniteOr(cascade.worldRadius, 0.0) * 2.0, 1e-4);
    float cascadeTexelSize = max(ShadowFiniteOr(cascade.texelSize, 0.0), 0.0);
    float2 texelSize = (cascadeTexelSize * filterRadiusTexels / cascadeExtent).xx;
    float shadow = 0.0;
    float totalWeight = 0.0;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 o = float2(float(x), float(y));
            float weight = exp2(-dot(o, o) * 0.55);
            float2 tapUv = shadowUV + o * texelSize;
            if (all(isfinite(tapUv)) &&
                all(tapUv >= 0.0.xx) && all(tapUv <= 1.0.xx))
            {
                shadow += shadowAtlas.SampleCmpLevelZero(
                    shadowSampler,
                    float3(tapUv, float(cascadeIndex)),
                    compareDepth) * weight;
            }
            else
            {
                shadow += weight;
            }
            totalWeight += weight;
        }
    }

    return shadow / max(totalWeight, 1e-4);
}

// Select the cascade index based on the fragment's view-space depth.
// splitDepths are in linear view-space depth, compared with reverse-Z convention
// where near=1 and far=0 in NDC but splitDepths use positive linear distances.
uint SelectCascade(float viewDepth, ShadowBuffer shadowData)
{
    if (!isfinite(viewDepth) || viewDepth <= 0.0)
    {
        return SHADOW_CASCADE_COUNT - 1;
    }

    for (uint i = 0; i < SHADOW_CASCADE_COUNT - 1; ++i)
    {
        float splitDepth = ShadowFiniteOr(shadowData.cascades[i].splitDepth, 0.0);
        if (splitDepth > 0.0 && viewDepth < splitDepth)
        {
            return i;
        }
    }
    return SHADOW_CASCADE_COUNT - 1;
}

// Sample the shadow factor for a single cascade (no blending).
// Returns 1.0 if fully lit, 0.0 if fully shadowed.
float SampleCascadeShadow(
    float3 samplePos,
    float3 normal,
    float3 lightDir,
    uint cascadeIndex,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    float3 shadowNDC = 0.0.xxx;
    if (!ProjectShadowCascade(samplePos, cascadeIndex, shadowData, shadowNDC))
    {
        return 1.0;
    }

    float2 shadowUV = ShadowNdcToUv(shadowNDC.xy);

    if (!all(isfinite(shadowUV)) ||
        any(shadowUV < 0.0) || any(shadowUV > 1.0) ||
        shadowNDC.z < 0.0 || shadowNDC.z > 1.0)
    {
        return 1.0;
    }

    float bias = ComputeSlopeScaledBias(normal, lightDir, shadowData) +
                 ComputeReceiverPlaneDepthBias(shadowNDC, shadowData);
    // Reverse-Z: push compare depth towards the light (larger = closer = near=1).
    float compareDepth = shadowNDC.z + bias;

    return SampleWeightedPcf(
        shadowUV, cascadeIndex, compareDepth, shadowData, shadowAtlas,
        shadowSampler);
}

// Compute the shadow factor for a world-space position.
// Returns 1.0 if fully lit, 0.0 if fully shadowed.
// Blends between adjacent cascades in the overlap zone for smooth transitions.
float ComputeShadowFactor(
    float3 worldPosition,
    float3 normal,
    float3 lightDir,
    float viewDepth,
    ShadowBuffer shadowData,
    Texture2DArray<float> shadowAtlas,
    SamplerComparisonState shadowSampler)
{
    if (!all(isfinite(worldPosition)) ||
        !all(isfinite(normal)) ||
        !all(isfinite(lightDir)) ||
        !isfinite(viewDepth))
    {
        return 1.0;
    }

    uint cascadeIndex = SelectCascade(viewDepth, shadowData);

    // Normal offset bias is scaled by each cascade's texel size.
    float3 samplePos = OffsetShadowSamplePosition(
        worldPosition, normal, lightDir, cascadeIndex, shadowData);
    float shadow = SampleCascadeShadow(
        samplePos, normal, lightDir, cascadeIndex,
        shadowData, shadowAtlas, shadowSampler);

    // Blend with the next cascade near the split boundary for smooth transitions.
    if (cascadeIndex < SHADOW_CASCADE_COUNT - 1)
    {
        float splitDepth =
            ShadowFiniteOr(shadowData.cascades[cascadeIndex].splitDepth, 0.0);
        float prevSplit = (cascadeIndex == 0)
            ? 0.0
            : ShadowFiniteOr(shadowData.cascades[cascadeIndex - 1].splitDepth,
                             0.0);
        float cascadeRange = splitDepth - prevSplit;
        float blendFraction =
            clamp(ShadowFiniteOr(shadowData.filterSettings.y, 0.0), 0.0, 0.45);
        float blendZone = cascadeRange * blendFraction;
        float blendStart = splitDepth - blendZone;

        if (isfinite(viewDepth) && cascadeRange > 1e-6 && viewDepth > blendStart)
        {
            float3 nextSamplePos = OffsetShadowSamplePosition(
                worldPosition, normal, lightDir, cascadeIndex + 1u,
                shadowData);
            float nextShadow = SampleCascadeShadow(
                nextSamplePos, normal, lightDir, cascadeIndex + 1,
                shadowData, shadowAtlas, shadowSampler);

            float blendFactor = (viewDepth - blendStart) / max(blendZone, 1e-6);
            blendFactor = saturate(blendFactor);
            shadow = lerp(shadow, nextShadow, blendFactor);
        }
    }

    return shadow;
}
