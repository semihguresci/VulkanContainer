// shadow_common.slang — Shared shadow sampling functions.
// Included by deferred_directional.slang for CSM shadow evaluation.
#pragma once

#include "lighting_structs.slang"

float2 ShadowNdcToUv(float2 shadowNdcXY)
{
    // Shadow cascades render with a positive-height viewport, so the atlas
    // keeps Vulkan's default framebuffer orientation and does not need the
    // scene pass Y inversion used for negative-height viewports.
    return shadowNdcXY * 0.5 + 0.5;
}

// Select the cascade index based on the fragment's view-space depth.
// splitDepths are in linear view-space depth, compared with reverse-Z convention
// where near=1 and far=0 in NDC but splitDepths use positive linear distances.
uint SelectCascade(float viewDepth, ShadowBuffer shadowData)
{
    for (uint i = 0; i < SHADOW_CASCADE_COUNT - 1; ++i)
    {
        if (viewDepth < shadowData.cascades[i].splitDepth)
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
    float4 shadowClip = mul(shadowData.cascades[cascadeIndex].viewProj, float4(samplePos, 1.0));
    float3 shadowNDC = shadowClip.xyz / shadowClip.w;

    float2 shadowUV = ShadowNdcToUv(shadowNDC.xy);

    if (any(shadowUV < 0.0) || any(shadowUV > 1.0))
    {
        return 1.0;
    }

    float NdotL = saturate(dot(normal, -lightDir));
    float bias = max(0.005 * (1.0 - NdotL), 0.001);
    // Reverse-Z: push compare depth towards the light (larger = closer = near=1).
    float compareDepth = shadowNDC.z + bias;

    float texelSize = 1.0 / 2048.0;
    float shadow = 0.0;
    float2 texelSizeVec = float2(texelSize, texelSize);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(float(x), float(y)) * texelSizeVec;
            shadow += shadowAtlas.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowUV + offset, float(cascadeIndex)),
                compareDepth);
        }
    }
    shadow /= 9.0;

    return shadow;
}

// Fraction of the cascade range used as the blend overlap zone on each side.
static const float CASCADE_BLEND_FRACTION = 0.1;

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
    uint cascadeIndex = SelectCascade(viewDepth, shadowData);

    // Normal offset bias: push sample position along surface normal to reduce acne.
    float texelSize = 1.0 / 2048.0;
    float normalBias = texelSize * 2.0;
    float3 samplePos = worldPosition + normal * normalBias;

    float shadow = SampleCascadeShadow(
        samplePos, normal, lightDir, cascadeIndex,
        shadowData, shadowAtlas, shadowSampler);

    // Blend with the next cascade near the split boundary for smooth transitions.
    if (cascadeIndex < SHADOW_CASCADE_COUNT - 1)
    {
        float splitDepth = shadowData.cascades[cascadeIndex].splitDepth;
        float prevSplit = (cascadeIndex == 0)
            ? 0.0
            : shadowData.cascades[cascadeIndex - 1].splitDepth;
        float cascadeRange = splitDepth - prevSplit;
        float blendZone = cascadeRange * CASCADE_BLEND_FRACTION;
        float blendStart = splitDepth - blendZone;

        if (viewDepth > blendStart)
        {
            float nextShadow = SampleCascadeShadow(
                samplePos, normal, lightDir, cascadeIndex + 1,
                shadowData, shadowAtlas, shadowSampler);

            float blendFactor = (viewDepth - blendStart) / max(blendZone, 1e-6);
            blendFactor = saturate(blendFactor);
            shadow = lerp(shadow, nextShadow, blendFactor);
        }
    }

    return shadow;
}
