// bloom_upsample.slang — Bloom progressive upsample with 9-tap tent filter.
// Uses a 3x3 tent filter for smooth upsampling, additively blends with the next higher mip.

struct PushConstants
{
    uint2 srcResolution;
    uint2 dstResolution;
    float filterRadius;
    float bloomIntensity;  // Only used on final upsample (mip 0 target)
    uint  isFinalPass;     // 1 = final upsample that writes to bloom output
    uint  pad0;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float4> srcTexture;   // Lower-res mip being upsampled
[[vk::binding(1, 0)]] SamplerState linearSampler;
[[vk::binding(2, 0)]] Texture2D<float4> dstReadTexture; // Current mip to blend with
[[vk::binding(3, 0)]] RWTexture2D<float4> dstTexture;   // Output

// 9-tap tent filter for upsampling (3x3 bilinear taps).
float3 UpsampleTent9(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 texelSize, float radius)
{
    float2 d = texelSize * radius;

    float3 a = tex.SampleLevel(samp, uv + float2(-d.x, -d.y), 0.0).rgb;
    float3 b = tex.SampleLevel(samp, uv + float2( 0.0, -d.y), 0.0).rgb;
    float3 c = tex.SampleLevel(samp, uv + float2( d.x, -d.y), 0.0).rgb;

    float3 d0 = tex.SampleLevel(samp, uv + float2(-d.x, 0.0), 0.0).rgb;
    float3 e = tex.SampleLevel(samp, uv, 0.0).rgb;
    float3 f = tex.SampleLevel(samp, uv + float2( d.x, 0.0), 0.0).rgb;

    float3 g = tex.SampleLevel(samp, uv + float2(-d.x,  d.y), 0.0).rgb;
    float3 h = tex.SampleLevel(samp, uv + float2( 0.0,  d.y), 0.0).rgb;
    float3 i = tex.SampleLevel(samp, uv + float2( d.x,  d.y), 0.0).rgb;

    // Tent filter weights: 1 2 1 / 2 4 2 / 1 2 1 → sum = 16
    float3 result = e * 4.0;
    result += (b + d0 + f + h) * 2.0;
    result += (a + c + g + i);
    result *= (1.0 / 16.0);

    return result;
}

[numthreads(8, 8, 1)]
[shader("compute")]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.dstResolution.x || dtid.y >= pc.dstResolution.y)
        return;

    float2 texelSize = 1.0 / float2(pc.srcResolution);
    float2 uv = (float2(dtid.xy) + 0.5) / float2(pc.dstResolution);

    float3 upsampled = UpsampleTent9(srcTexture, linearSampler, uv, texelSize, pc.filterRadius);

    // Additive blend with the destination mip.
    float3 existing = dstReadTexture.SampleLevel(linearSampler, uv, 0.0).rgb;
    float3 result = existing + upsampled;

    dstTexture[dtid.xy] = float4(result, 1.0);
}
