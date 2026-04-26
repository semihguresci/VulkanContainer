// gtao_blur.slang — Bilateral blur for GTAO denoising.
// Depth-aware 5×5 blur to smooth AO while preserving edges.

struct BlurPushConstants
{
    uint  width;
    uint  height;
    float depthThreshold;
    uint  pad0;
};

[[vk::push_constant]]
ConstantBuffer<BlurPushConstants> pc;

[[vk::binding(0, 0)]] Texture2D<float>   inAO;
[[vk::binding(1, 0)]] SamplerState       aoSampler;
[[vk::binding(2, 0)]] Texture2D<float>   gDepthTexture;
[[vk::binding(3, 0)]] SamplerState       depthSampler;
[[vk::binding(4, 0)]] RWTexture2D<float> outAO;

static const int KERNEL_RADIUS = 2;

// Gaussian weights for a 5×5 kernel (sigma ≈ 1.0).
float GaussianWeight(int x, int y)
{
    static const float sigma = 1.0;
    float d2 = float(x * x + y * y);
    return exp(-d2 / (2.0 * sigma * sigma));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.width || dtid.y >= pc.height) return;

    float2 texelSize = 1.0 / float2(pc.width, pc.height);
    float2 centerUV = (float2(dtid.xy) + 0.5) * texelSize;

    float centerDepth = gDepthTexture.SampleLevel(depthSampler, centerUV, 0.0);
    float centerAO = inAO.SampleLevel(aoSampler, centerUV, 0.0);

    if (centerDepth == 0.0)
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    float totalWeight = 0.0;
    float totalAO = 0.0;

    for (int y = -KERNEL_RADIUS; y <= KERNEL_RADIUS; ++y)
    {
        for (int x = -KERNEL_RADIUS; x <= KERNEL_RADIUS; ++x)
        {
            float2 sampleUV = centerUV + float2(x, y) * texelSize;
            sampleUV = clamp(sampleUV, 0.0, 1.0);

            float sampleDepth = gDepthTexture.SampleLevel(depthSampler, sampleUV, 0.0);
            float sampleAO = inAO.SampleLevel(aoSampler, sampleUV, 0.0);

            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = depthDiff < pc.depthThreshold ? 1.0 : 0.0;

            float spatialWeight = GaussianWeight(x, y);
            float w = spatialWeight * depthWeight;

            totalAO += sampleAO * w;
            totalWeight += w;
        }
    }

    outAO[dtid.xy] = totalAO / max(totalWeight, 1e-4);
}
