// hiz_generate.slang — Compute shader for hierarchical depth buffer generation.
// Generates a min-reduction mip chain from the depth prepass output.
// For reverse-Z (near=1, far=0), we take the minimum (conservative far plane).

struct HiZPushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstMipLevel;
    uint pad0;
};

[[vk::push_constant]]
ConstantBuffer<HiZPushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float>    gSrcDepth;
[[vk::binding(1, 0)]] SamplerState        gSrcSampler;
[[vk::binding(2, 0)]] RWTexture2D<float>  gDstMip;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2 dstCoord = dtid.xy;

    // Mip 0 is a full-resolution copy of the depth buffer. Higher mips are
    // min-reduced by 2x from the previous Hi-Z mip.
    uint dstWidth = pc.dstMipLevel == 0
        ? pc.srcWidth
        : max(pc.srcWidth >> 1, 1u);
    uint dstHeight = pc.dstMipLevel == 0
        ? pc.srcHeight
        : max(pc.srcHeight >> 1, 1u);

    if (dstCoord.x >= dstWidth || dstCoord.y >= dstHeight)
        return;

    float2 srcTexelSize = float2(1.0 / float(pc.srcWidth), 1.0 / float(pc.srcHeight));
    float2 uv = pc.dstMipLevel == 0
        ? (float2(dstCoord) + 0.5) * srcTexelSize
        : (float2(dstCoord) * 2.0 + 1.0) * srcTexelSize;

    if (pc.dstMipLevel == 0)
    {
        gDstMip[dstCoord] = gSrcDepth.SampleLevel(gSrcSampler, uv, 0);
        return;
    }

    // Reverse-Z: min = closest to far plane = most conservative for occlusion.
    float d00 = gSrcDepth.SampleLevel(gSrcSampler, uv + float2(-0.25, -0.25) * srcTexelSize, 0);
    float d10 = gSrcDepth.SampleLevel(gSrcSampler, uv + float2( 0.25, -0.25) * srcTexelSize, 0);
    float d01 = gSrcDepth.SampleLevel(gSrcSampler, uv + float2(-0.25,  0.25) * srcTexelSize, 0);
    float d11 = gSrcDepth.SampleLevel(gSrcSampler, uv + float2( 0.25,  0.25) * srcTexelSize, 0);
    float minDepth = min(min(d00, d10), min(d01, d11));

    gDstMip[dstCoord] = minDepth;
}
