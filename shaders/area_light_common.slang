// area_light_common.slang - Shared sampled area-light helpers.
#pragma once

#include "lighting_structs.slang"
#include "brdf_common.slang"

static const uint AREA_LIGHT_INTEGRATION_SAMPLE_COUNT = 4u;
static const float AREA_LIGHT_MIN_DISTANCE_SQ = 0.01;

struct AreaLightFrame
{
    float3 position;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float2 halfSize;
    float area;
    float type;
};

float3 AreaLightFallbackTangent(float3 normal)
{
    float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                      : float3(1.0, 0.0, 0.0);
    return SafeNormalize(cross(up, normal), float3(1.0, 0.0, 0.0));
}

AreaLightFrame BuildAreaLightFrame(AreaLightData light)
{
    AreaLightFrame frame;
    frame.position = light.positionRange.xyz;
    frame.normal =
        SafeNormalize(light.directionType.xyz, float3(0.0, 0.0, -1.0));

    float3 tangentHint = light.tangentHalfSize.xyz -
        frame.normal * dot(light.tangentHalfSize.xyz, frame.normal);
    frame.tangent =
        SafeNormalize(tangentHint, AreaLightFallbackTangent(frame.normal));

    float3 bitangentHint = light.bitangentHalfSize.xyz -
        frame.normal * dot(light.bitangentHalfSize.xyz, frame.normal) -
        frame.tangent * dot(light.bitangentHalfSize.xyz, frame.tangent);
    frame.bitangent =
        SafeNormalize(bitangentHint,
                      SafeNormalize(cross(frame.normal, frame.tangent),
                                    float3(0.0, 1.0, 0.0)));

    frame.type = light.directionType.w;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        float radius = max(light.tangentHalfSize.w, 1.0e-3);
        frame.halfSize = radius.xx;
        frame.area = PI * radius * radius;
    }
    else
    {
        frame.halfSize = float2(max(light.tangentHalfSize.w, 1.0e-3),
                                max(light.bitangentHalfSize.w, 1.0e-3));
        frame.area = 4.0 * frame.halfSize.x * frame.halfSize.y;
    }

    return frame;
}

float2 AreaLightRectSampleOffset(uint sampleIndex)
{
    static const float kGauss = 0.57735026919;
    static const float2 offsets[4] = {
        float2(-kGauss, -kGauss),
        float2( kGauss, -kGauss),
        float2(-kGauss,  kGauss),
        float2( kGauss,  kGauss),
    };
    return offsets[sampleIndex & 3u];
}

float2 AreaLightDiskSampleOffset(uint sampleIndex)
{
    static const float2 offsets[4] = {
        float2(-0.5, -0.5),
        float2( 0.5, -0.5),
        float2(-0.5,  0.5),
        float2( 0.5,  0.5),
    };
    return offsets[sampleIndex & 3u];
}

float3 AreaLightSamplePosition(AreaLightFrame frame, uint sampleIndex)
{
    float2 offset =
        (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
            ? AreaLightDiskSampleOffset(sampleIndex) * frame.halfSize.x
            : AreaLightRectSampleOffset(sampleIndex) * frame.halfSize;
    return frame.position + frame.tangent * offset.x +
        frame.bitangent * offset.y;
}

float3 EvaluateAreaLightSampleRadiance(AreaLightData light,
                                       AreaLightFrame frame,
                                       float3 worldPosition,
                                       uint sampleIndex,
                                       out float3 surfaceToLight)
{
    float3 samplePosition = AreaLightSamplePosition(frame, sampleIndex);
    float3 lightVector = samplePosition - worldPosition;
    float distanceSq = dot(lightVector, lightVector);
    float range = light.positionRange.w;
    surfaceToLight = frame.normal;

    if (PointLightHasFiniteRange(range) && distanceSq >= range * range)
    {
        return 0.0.xxx;
    }

    surfaceToLight = lightVector * rsqrt(max(distanceSq, 1.0e-8));
    float sourceFacing = saturate(dot(frame.normal, -surfaceToLight));
    if (sourceFacing <= 0.0)
    {
        return 0.0.xxx;
    }

    float attenuation =
        SmoothRangeAttenuation(distanceSq, range) * sourceFacing *
        frame.area / (float(AREA_LIGHT_INTEGRATION_SAMPLE_COUNT) *
                      max(distanceSq, AREA_LIGHT_MIN_DISTANCE_SQ));
    return light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
}
