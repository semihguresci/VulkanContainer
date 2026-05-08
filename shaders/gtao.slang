// gtao.slang — Ground-Truth Ambient Occlusion compute shader.
// Samples the depth buffer in a hemisphere pattern to estimate occlusion.
// Output: Half-resolution R8 AO texture.

#include "lighting_structs.slang"
#include "brdf_common.slang"
#include "push_constants_common.slang"

[[vk::push_constant]]
ConstantBuffer<GtaoPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] Texture2D<float>             gDepthTexture;
[[vk::binding(2, 0)]] SamplerState                 gDepthSampler;
[[vk::binding(3, 0)]] Texture2D<float4>            gNormalTexture;
[[vk::binding(4, 0)]] SamplerState                 gNormalSampler;
[[vk::binding(5, 0)]] RWTexture2D<float>           outAO;

static const uint GTAO_MAX_SAMPLE_COUNT = 64u;

// Simple hash for spatial noise.
float InterleavedGradientNoise(float2 pos)
{
    return frac(52.9829189 * frac(dot(pos, float2(0.06711056, 0.00583715))));
}

bool ReconstructViewPos(float2 uv, float depth, out float3 worldPosition)
{
    return TryReconstructWorldPosition(uv, depth, uCamera.inverseViewProj,
                                       worldPosition);
}

float2 PixelCenterUv(uint2 pixelCoord, uint2 dimensions)
{
    return (float2(pixelCoord) + 0.5.xx) /
           float2(max(dimensions.x, 1u), max(dimensions.y, 1u));
}

uint2 ScenePixelFromUv(float2 uv, uint2 dimensions)
{
    uint2 maxPixel = uint2(dimensions.x - 1u, dimensions.y - 1u);
    return min(uint2(saturate(uv) * float2(dimensions)), maxPixel);
}

bool LoadSceneDepth(uint2 pixelCoord, uint2 dimensions, out float depth)
{
    pixelCoord = min(pixelCoord, uint2(dimensions.x - 1u, dimensions.y - 1u));
    depth = gDepthTexture.Load(int3(pixelCoord, 0));
    return isfinite(depth) && depth > 0.0 && depth <= 1.0;
}

bool LoadSceneNormal(uint2 pixelCoord, uint2 dimensions, out float3 normal)
{
    pixelCoord = min(pixelCoord, uint2(dimensions.x - 1u, dimensions.y - 1u));
    float3 encoded = gNormalTexture.Load(int3(pixelCoord, 0)).xyz;
    float3 decoded = encoded * 2.0 - 1.0;
    float lenSq = dot(decoded, decoded);
    if (!all(isfinite(decoded)) || lenSq < 1e-8)
    {
        normal = float3(0.0, 1.0, 0.0);
        return false;
    }
    normal = decoded * rsqrt(lenSq);
    return true;
}

bool ProjectWorldToSceneUv(float3 worldPos, out float2 sceneUv)
{
    sceneUv = 0.0.xx;
    if (!all(isfinite(worldPos)))
    {
        return false;
    }

    float4 projected = mul(uCamera.viewProj, float4(worldPos, 1.0));
    if (!all(isfinite(projected)) || projected.w <= 1e-6)
    {
        return false;
    }

    float3 ndc = projected.xyz / projected.w;
    if (!all(isfinite(ndc)) || ndc.z <= 0.0 || ndc.z > 1.0)
    {
        return false;
    }

    sceneUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return all(isfinite(sceneUv)) && !any(sceneUv < 0.0) && !any(sceneUv > 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint aoWidth = 0u;
    uint aoHeight = 0u;
    outAO.GetDimensions(aoWidth, aoHeight);

    if (dtid.x >= aoWidth || dtid.y >= aoHeight) return;

    uint depthWidth = 0u;
    uint depthHeight = 0u;
    uint normalWidth = 0u;
    uint normalHeight = 0u;
    gDepthTexture.GetDimensions(depthWidth, depthHeight);
    gNormalTexture.GetDimensions(normalWidth, normalHeight);
    if (depthWidth == 0u || depthHeight == 0u ||
        normalWidth == 0u || normalHeight == 0u)
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    uint2 depthDimensions = uint2(depthWidth, depthHeight);
    uint2 normalDimensions = uint2(normalWidth, normalHeight);
    uint2 fullResPixel = min(dtid.xy * 2u + uint2(1u, 1u),
                             uint2(depthWidth - 1u, depthHeight - 1u));
    float2 uv = PixelCenterUv(fullResPixel, depthDimensions);

    float depth = 0.0;
    if (!LoadSceneDepth(fullResPixel, depthDimensions, depth))
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    float3 worldPos;
    if (!ReconstructViewPos(uv, depth, worldPos))
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    float3 normal = 0.0.xxx;
    uint2 normalPixel = min(fullResPixel,
                            uint2(normalWidth - 1u, normalHeight - 1u));
    if (!LoadSceneNormal(normalPixel, normalDimensions, normal))
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    float occlusion = 0.0;
    float noise = InterleavedGradientNoise(float2(dtid.xy));

    uint sampleCount = min(max(pc.sampleCount, 1u), GTAO_MAX_SAMPLE_COUNT);
    float radius = pc.aoRadius;
    if (!isfinite(radius) || radius <= 1e-5 || !isfinite(pc.aoIntensity))
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    for (uint i = 0u; i < sampleCount; ++i)
    {
        // Fibonacci spiral sample directions.
        float fi = float(i) + noise;
        float r = sqrt(fi / float(sampleCount));
        float angle = 2.4 * fi; // golden angle

        float3 sampleOffset;
        sampleOffset.x = r * cos(angle);
        sampleOffset.y = r * sin(angle);
        sampleOffset.z = sqrt(max(1.0 - r * r, 0.0));

        // Orient to normal hemisphere.
        float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
        float3 tangent = normalize(cross(up, normal));
        float3 bitangent = cross(normal, tangent);

        float3 sampleDir = tangent * sampleOffset.x +
                           bitangent * sampleOffset.y +
                           normal * sampleOffset.z;

        float3 samplePos = worldPos + sampleDir * radius;

        float2 sampleUV = 0.0.xx;
        if (!ProjectWorldToSceneUv(samplePos, sampleUV)) continue;

        uint2 sampleDepthPixel = ScenePixelFromUv(sampleUV, depthDimensions);
        float sampleDepth = 0.0;
        if (!LoadSceneDepth(sampleDepthPixel, depthDimensions, sampleDepth))
        {
            continue;
        }

        float3 sampleNormal = 0.0.xxx;
        uint2 sampleNormalPixel = ScenePixelFromUv(sampleUV, normalDimensions);
        if (!LoadSceneNormal(sampleNormalPixel, normalDimensions, sampleNormal) ||
            dot(normal, sampleNormal) <= 0.0)
        {
            continue;
        }

        float3 sampleWorldPos;
        if (!ReconstructViewPos(sampleUV, sampleDepth, sampleWorldPos))
        {
            continue;
        }

        float3 diff = sampleWorldPos - worldPos;
        float dist = length(diff);
        if (!isfinite(dist) || dist <= 1e-5)
        {
            continue;
        }
        float rangeFalloff = saturate(1.0 - dist / radius);

        float occlusionSample = saturate(dot(normal, diff / dist) - 0.01);
        occlusion += occlusionSample * rangeFalloff;
    }

    occlusion = 1.0 - saturate((occlusion / float(sampleCount)) * pc.aoIntensity);
    outAO[dtid.xy] = occlusion;
}
