// hiz_generate.slang — Compute shader for hierarchical depth buffer generation.
// Generates a min-reduction mip chain from the depth prepass output.
// For reverse-Z (near=1, far=0), we take the minimum (conservative far plane).

#include "push_constants_common.slang"

[[vk::push_constant]]
ConstantBuffer<HiZPushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float>    gSrcDepth;
[[vk::binding(1, 0)]] SamplerState        gSrcSampler;
[[vk::binding(2, 0)]] RWTexture2D<float>  gDstMip;

float LoadSourceDepthClamped(uint2 coord, uint2 srcExtent)
{
    uint2 clampedCoord = min(coord, srcExtent - 1u);
    float depth = gSrcDepth.Load(int3(clampedCoord, 0));
    return (isfinite(depth) && depth >= 0.0 && depth <= 1.0) ? depth : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2 dstCoord = dtid.xy;

    if (pc.srcWidth == 0u || pc.srcHeight == 0u)
    {
        return;
    }

    // Mip 0 is a full-resolution copy of the depth buffer. Higher mips are
    // min-reduced by 2x from the previous Hi-Z mip.
    uint dstWidth = pc.dstMipLevel == 0
        ? pc.srcWidth
        : max((pc.srcWidth + 1u) >> 1, 1u);
    uint dstHeight = pc.dstMipLevel == 0
        ? pc.srcHeight
        : max((pc.srcHeight + 1u) >> 1, 1u);

    if (dstCoord.x >= dstWidth || dstCoord.y >= dstHeight)
        return;

    uint2 srcExtent = uint2(pc.srcWidth, pc.srcHeight);

    if (pc.dstMipLevel == 0)
    {
        gDstMip[dstCoord] = LoadSourceDepthClamped(dstCoord, srcExtent);
        return;
    }

    // Reverse-Z: min = closest to far plane = most conservative for occlusion.
    uint2 srcBase = dstCoord * 2u;
    float d00 = LoadSourceDepthClamped(srcBase + uint2(0u, 0u), srcExtent);
    float d10 = LoadSourceDepthClamped(srcBase + uint2(1u, 0u), srcExtent);
    float d01 = LoadSourceDepthClamped(srcBase + uint2(0u, 1u), srcExtent);
    float d11 = LoadSourceDepthClamped(srcBase + uint2(1u, 1u), srcExtent);
    float minDepth = min(min(d00, d10), min(d01, d11));

    gDstMip[dstCoord] = minDepth;
}
