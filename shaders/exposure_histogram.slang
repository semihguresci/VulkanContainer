#include "push_constants_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> sceneColorTexture;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> luminanceHistogram;
[[vk::push_constant]] ConstantBuffer<ExposureHistogramPushConstants> pc;

float SceneLuminance(float3 color)
{
    return dot(max(color, 0.0.xxx), float3(0.2126, 0.7152, 0.0722));
}

[numthreads(16, 16, 1)]
[shader("compute")]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= pc.width || dispatchThreadId.y >= pc.height)
    {
        return;
    }

    float3 color = sceneColorTexture.Load(int3(dispatchThreadId.xy, 0)).rgb;
    float luminance = SceneLuminance(color);

    uint bin = 0u;
    if (luminance > 1.0e-5)
    {
        float logLuminance = log2(luminance);
        float normalized = saturate(
            (logLuminance - pc.minLogLuminance) /
            max(pc.maxLogLuminance - pc.minLogLuminance, 1.0e-4));
        uint nonBlackBinCount = max(pc.binCount - 1u, 1u);
        bin = 1u + min((uint)(normalized * (float)(nonBlackBinCount - 1u)),
                       nonBlackBinCount - 1u);
    }

    uint ignored = 0u;
    InterlockedAdd(luminanceHistogram[bin], 1u, ignored);
    InterlockedAdd(luminanceHistogram[pc.binCount], 1u, ignored);
}
