#include "push_constants_common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> sceneColorTexture;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> luminanceHistogram;
[[vk::push_constant]] ConstantBuffer<ExposureHistogramPushConstants> pc;

static const uint EXPOSURE_HISTOGRAM_MAX_BIN_COUNT = 64u;

float SceneLuminance(float3 color)
{
    if (!all(isfinite(color)))
    {
        return 0.0;
    }

    float luminance =
        dot(max(color, 0.0.xxx), float3(0.2126, 0.7152, 0.0722));
    return isfinite(luminance) ? max(luminance, 0.0) : 0.0;
}

[numthreads(16, 16, 1)]
[shader("compute")]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= pc.width || dispatchThreadId.y >= pc.height)
    {
        return;
    }

    if (pc.binCount == 0u)
    {
        return;
    }
    uint binCount = clamp(pc.binCount, 2u, EXPOSURE_HISTOGRAM_MAX_BIN_COUNT);

    float3 color = sceneColorTexture.Load(int3(dispatchThreadId.xy, 0)).rgb;
    float luminance = SceneLuminance(color);
    if (!isfinite(luminance))
    {
        luminance = 0.0;
    }

    uint bin = 0u;
    if (luminance > 1.0e-5 && binCount > 1u)
    {
        float minLogLuminance = isfinite(pc.minLogLuminance)
            ? pc.minLogLuminance
            : -12.0;
        float maxLogLuminance =
            (isfinite(pc.maxLogLuminance) && pc.maxLogLuminance > minLogLuminance)
                ? pc.maxLogLuminance
                : minLogLuminance + 1.0;
        float logLuminance = log2(luminance);
        float normalized = saturate(
            (logLuminance - minLogLuminance) /
            max(maxLogLuminance - minLogLuminance, 1.0e-4));
        uint nonBlackBinCount = binCount - 1u;
        bin = 1u + min((uint)(normalized * (float)(nonBlackBinCount - 1u)),
                       nonBlackBinCount - 1u);
    }

    uint ignored = 0u;
    InterlockedAdd(luminanceHistogram[bin], 1u, ignored);
    InterlockedAdd(luminanceHistogram[binCount], 1u, ignored);
}
