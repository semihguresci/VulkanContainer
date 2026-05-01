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

// Simple hash for spatial noise.
float InterleavedGradientNoise(float2 pos)
{
    return frac(52.9829189 * frac(dot(pos, float2(0.06711056, 0.00583715))));
}

float3 ReconstructViewPos(float2 uv, float depth)
{
    return ReconstructWorldPosition(uv, depth, uCamera.inverseViewProj);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint aoWidth = 0u;
    uint aoHeight = 0u;
    outAO.GetDimensions(aoWidth, aoHeight);

    if (dtid.x >= aoWidth || dtid.y >= aoHeight) return;

    // Map half-res texel to full-res UV.
    float2 uv = (float2(dtid.xy) * 2.0 + 1.0) / float2(pc.fullWidth, pc.fullHeight);

    float depth = gDepthTexture.SampleLevel(gDepthSampler, uv, 0.0);
    if (depth == 0.0)
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    float3 worldPos = ReconstructViewPos(uv, depth);
    float3 normal = normalize(gNormalTexture.SampleLevel(gNormalSampler, uv, 0.0).xyz * 2.0 - 1.0);

    float occlusion = 0.0;
    float noise = InterleavedGradientNoise(float2(dtid.xy));

    uint sampleCount = max(pc.sampleCount, 1u);
    float radius = pc.aoRadius;

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

        // Project sample to screen.
        float4 projected = mul(uCamera.viewProj, float4(samplePos, 1.0));
        projected.xyz /= max(projected.w, 1e-6);
        float2 sampleUV = float2(projected.x * 0.5 + 0.5, -projected.y * 0.5 + 0.5);

        if (any(sampleUV < 0.0) || any(sampleUV > 1.0)) continue;

        float sampleDepth = gDepthTexture.SampleLevel(gDepthSampler, sampleUV, 0.0);
        float3 sampleWorldPos = ReconstructViewPos(sampleUV, sampleDepth);

        float3 diff = sampleWorldPos - worldPos;
        float dist = length(diff);
        float rangeFalloff = saturate(1.0 - dist / radius);

        float occlusionSample = saturate(dot(normal, diff / max(dist, 1e-6)) - 0.01);
        occlusion += occlusionSample * rangeFalloff;
    }

    occlusion = 1.0 - saturate((occlusion / float(sampleCount)) * pc.aoIntensity);
    outAO[dtid.xy] = occlusion;
}
