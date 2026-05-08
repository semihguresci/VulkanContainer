// local_shadow_common.slang - Shared light-space local shadow-map sampling.
// Point/spot/area lighting should use this for real local-light occlusion.
// Deferred opaque point/spot passes may add screen-space contact visibility as
// an explicit detail term; area and transparent paths stay atlas-only here.
#pragma once

#include "lighting_structs.slang"
#include "brdf_common.slang"

float LocalShadowFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

bool LocalShadowMapsEnabled(uint localShadowEnabled,
                            LocalShadowBuffer localShadow)
{
    return localShadowEnabled != 0u &&
           localShadow.counts.x != 0u &&
           localShadow.counts.z != 0u &&
           localShadow.counts.w != 0u;
}

bool LocalShadowMapsEnabled(LightingBuffer lighting,
                            LocalShadowBuffer localShadow)
{
    return lighting.shadowEnabled != 0u &&
           LocalShadowMapsEnabled(lighting.localShadowEnabled, localShadow);
}

uint LocalShadowBaseLayerFromPointLight(PointLightData light)
{
    float encoded = light.coneOuterCosType.z;
    if (!isfinite(encoded) || encoded < 1.0)
    {
        return MAX_SHADOWED_LOCAL_LIGHT_LAYERS;
    }
    return uint(encoded + 0.5) - 1u;
}

uint LocalShadowLayerCountFromPointLight(PointLightData light)
{
    float encoded = light.coneOuterCosType.w;
    if (!isfinite(encoded) || encoded < 1.0)
    {
        return 0u;
    }
    return uint(encoded + 0.5);
}

bool PointLightHasLocalShadowLayers(PointLightData light,
                                    LocalShadowBuffer localShadow)
{
    uint baseLayer = LocalShadowBaseLayerFromPointLight(light);
    uint layerCount = LocalShadowLayerCountFromPointLight(light);
    if (baseLayer >= MAX_SHADOWED_LOCAL_LIGHT_LAYERS ||
        layerCount == 0u ||
        baseLayer >= localShadow.counts.x)
    {
        return false;
    }

    if (layerCount > MAX_SHADOWED_LOCAL_LIGHT_LAYERS - baseLayer)
    {
        return false;
    }

    return baseLayer + layerCount <= localShadow.counts.x;
}

uint SelectPointLocalShadowFace(float3 lightToReceiver)
{
    float3 a = abs(lightToReceiver);
    if (a.x >= a.y && a.x >= a.z)
    {
        return lightToReceiver.x >= 0.0 ? 0u : 1u;
    }
    if (a.y >= a.x && a.y >= a.z)
    {
        return lightToReceiver.y >= 0.0 ? 2u : 3u;
    }
    return lightToReceiver.z >= 0.0 ? 4u : 5u;
}

uint PackedLocalShadowAreaRef(LocalShadowBuffer localShadow, uint areaIndex)
{
    if (areaIndex >= MAX_AREA_LIGHTS)
    {
        return 0u;
    }
    uint4 packed = localShadow.areaLightRefs[areaIndex / 4u];
    return packed[areaIndex & 3u];
}

float LocalShadowSlopeBias(float3 normal, float3 surfaceToLight,
                           LocalShadowBuffer localShadow)
{
    float NdotL =
        saturate(LocalShadowFiniteOr(dot(normal, surfaceToLight), 0.0));
    float slope = sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.12);
    float constantBias =
        max(LocalShadowFiniteOr(localShadow.filterSettings.z, 0.0), 0.0);
    float slopeScale =
        max(LocalShadowFiniteOr(localShadow.biasSettings.z, 0.0), 0.0);
    float maxBias =
        max(LocalShadowFiniteOr(localShadow.filterSettings.w, constantBias),
            0.0);
    return min(constantBias + slope * slopeScale, maxBias);
}

float LocalShadowReceiverPlaneBias(float3 shadowNdc,
                                   LocalShadowBuffer localShadow)
{
    // Local shadows are sampled from per-light loops in tiled and transparent
    // paths. Screen-space derivatives inside those non-uniform branches can
    // overestimate bias and create view-dependent light leaks.
    return 0.0;
}

float LocalShadowSoftFilterRadiusTexels(
    LocalShadowLayerData layer,
    float distanceToLight,
    LocalShadowBuffer localShadow)
{
    float baseRadiusTexels =
        max(LocalShadowFiniteOr(localShadow.filterSettings.x, 0.0), 0.0);
    float maxSoftRadiusTexels =
        max(LocalShadowFiniteOr(localShadow.filterSettings.y,
                                baseRadiusTexels),
            baseRadiusTexels);
    float texelSizeWorld =
        max(LocalShadowFiniteOr(layer.params.x, 0.0), 1.0e-5);
    float sourceRadiusWorld =
        max(LocalShadowFiniteOr(layer.params.w, 0.0), 0.0);
    float lightRange =
        max(LocalShadowFiniteOr(layer.positionRange.w, 0.0), 1.0e-4);
    float receiverT = saturate(distanceToLight / lightRange);

    // Area and finite-radius local lights need the PCF footprint to grow in
    // light texel space as the receiver moves away from the source. This is a
    // bounded penumbra approximation, not a screen-space contact term.
    float projectedSourceRadiusTexels =
        (sourceRadiusWorld / texelSizeWorld) * lerp(0.20, 0.85, receiverT);
    return clamp(max(baseRadiusTexels, projectedSourceRadiusTexels),
                 max(baseRadiusTexels, 0.25),
                 maxSoftRadiusTexels);
}

float3 OffsetLocalShadowSamplePosition(
    float3 worldPosition,
    float3 normal,
    float3 surfaceToLight,
    LocalShadowLayerData layer,
    LocalShadowBuffer localShadow)
{
    float minNormalBiasTexels =
        max(LocalShadowFiniteOr(localShadow.biasSettings.x, 0.0), 0.0);
    float maxNormalBiasTexels =
        max(LocalShadowFiniteOr(localShadow.biasSettings.y,
                                minNormalBiasTexels),
            minNormalBiasTexels);
    float normalBiasTexels = lerp(
        minNormalBiasTexels,
        maxNormalBiasTexels,
        saturate(1.0 - LocalShadowFiniteOr(dot(normal, surfaceToLight), 0.0)));
    return worldPosition +
        normal * max(LocalShadowFiniteOr(layer.params.x, 0.0), 0.0) *
        normalBiasTexels;
}

float SampleLocalShadowLayerPcf(
    uint layerIndex,
    float2 shadowUv,
    float compareDepth,
    float filterRadiusTexels,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    if (!isfinite(compareDepth))
    {
        return 1.0;
    }

    compareDepth = saturate(compareDepth);
    float invResolution =
        1.0 / max(LocalShadowFiniteOr(float(localShadow.counts.z),
                                      float(LOCAL_SHADOW_MAP_RESOLUTION)),
                  float(LOCAL_SHADOW_MAP_RESOLUTION));
    float2 texelSize = (filterRadiusTexels * invResolution).xx;
    float shadow = 0.0;
    float totalWeight = 0.0;

    [unroll]
    for (int y = -3; y <= 3; ++y)
    {
        [unroll]
        for (int x = -3; x <= 3; ++x)
        {
            float2 o = float2(float(x), float(y));
            float weight = exp2(-dot(o, o) * 0.28);
            float2 tapUv = shadowUv + o * texelSize;
            if (all(isfinite(tapUv)) &&
                all(tapUv >= 0.0.xx) && all(tapUv <= 1.0.xx))
            {
                shadow += localShadowAtlas.SampleCmpLevelZero(
                    localShadowSampler,
                    float3(tapUv, float(layerIndex)),
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

float SampleLocalShadowLayer(
    uint layerIndex,
    float3 worldPosition,
    float3 receiverNormal,
    float3 surfaceToLight,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    if (localShadow.counts.w == 0u ||
        layerIndex >= localShadow.counts.x ||
        layerIndex >= MAX_SHADOWED_LOCAL_LIGHT_LAYERS)
    {
        return 1.0;
    }

    LocalShadowLayerData layer = localShadow.layers[layerIndex];
    if (layer.meta.w == 0u)
    {
        return 1.0;
    }

    float3 lightToReceiver = worldPosition - layer.positionRange.xyz;
    float distanceToLight = length(lightToReceiver);
    float rawLightRange = layer.positionRange.w;
    if (!isfinite(rawLightRange) || rawLightRange <= 1e-4)
    {
        return 1.0;
    }
    float lightRange = max(rawLightRange, 1e-4);
    if (!isfinite(distanceToLight) ||
        distanceToLight <= 1e-4 ||
        distanceToLight >= lightRange)
    {
        return 1.0;
    }
    float rangeFadeStart = lightRange * 0.95;
    float rangeVisibility =
        saturate((lightRange - distanceToLight) /
                 max(lightRange - rangeFadeStart, 1e-4));

    float3 normal = SafeNormalize(receiverNormal, -surfaceToLight);
    float3 samplePosition = OffsetLocalShadowSamplePosition(
        worldPosition, normal, surfaceToLight, layer, localShadow);
    float4 shadowClip = mul(layer.viewProj, float4(samplePosition, 1.0));
    if (!all(isfinite(shadowClip)) || shadowClip.w <= 1e-6)
    {
        return 1.0;
    }

    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    if (!all(isfinite(shadowNdc)))
    {
        return 1.0;
    }

    float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    if (!all(isfinite(shadowUv)) ||
        any(shadowUv < 0.0.xx) || any(shadowUv > 1.0.xx) ||
        shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    float bias = LocalShadowSlopeBias(normal, surfaceToLight, localShadow) +
        LocalShadowReceiverPlaneBias(shadowNdc, localShadow);
    float filterRadiusTexels = LocalShadowSoftFilterRadiusTexels(
        layer, distanceToLight, localShadow);
    float shadow = SampleLocalShadowLayerPcf(
        layerIndex, shadowUv, shadowNdc.z + bias, filterRadiusTexels,
        localShadow, localShadowAtlas, localShadowSampler);
    return lerp(1.0, shadow, rangeVisibility);
}

float LocalPointLightShadowVisibility(
    PointLightData light,
    float3 worldPosition,
    float3 receiverNormal,
    float3 surfaceToLight,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    uint baseLayer = LocalShadowBaseLayerFromPointLight(light);
    uint layerCount = LocalShadowLayerCountFromPointLight(light);
    if (!PointLightHasLocalShadowLayers(light, localShadow))
    {
        return 1.0;
    }

    bool isSpot = light.coneOuterCosType.y >= 0.5;
    uint layerIndex = baseLayer;
    if (!isSpot && layerCount >= LOCAL_SHADOW_POINT_FACE_COUNT)
    {
        layerIndex += SelectPointLocalShadowFace(
            worldPosition - light.positionRadius.xyz);
    }
    if (layerIndex >= baseLayer + layerCount)
    {
        return 1.0;
    }

    return SampleLocalShadowLayer(
        layerIndex, worldPosition, receiverNormal, surfaceToLight,
        localShadow, localShadowAtlas, localShadowSampler);
}

float LocalAreaLightShadowVisibility(
    uint areaIndex,
    float3 worldPosition,
    float3 receiverNormal,
    float3 samplePosition,
    LocalShadowBuffer localShadow,
    Texture2DArray<float> localShadowAtlas,
    SamplerComparisonState localShadowSampler)
{
    uint encodedLayer = PackedLocalShadowAreaRef(localShadow, areaIndex);
    if (encodedLayer == 0u)
    {
        return 1.0;
    }

    float3 surfaceToLight =
        SafeNormalize(samplePosition - worldPosition, float3(0.0, 1.0, 0.0));
    return SampleLocalShadowLayer(
        encodedLayer - 1u, worldPosition, receiverNormal, surfaceToLight,
        localShadow, localShadowAtlas, localShadowSampler);
}
