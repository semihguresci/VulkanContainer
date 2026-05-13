// gtao_blur.slang — Bilateral blur for GTAO denoising.
// Depth-aware 5×5 blur to smooth AO while preserving edges.

#include "push_constants_common.slang"

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

bool ValidReverseZDepth(float depth)
{
    return isfinite(depth) && depth > 0.0 && depth <= 1.0;
}

float SanitizedNearPlane()
{
    return max(isfinite(pc.cameraNear) ? pc.cameraNear : 0.01, 1e-4);
}

float SanitizedFarPlane()
{
    float n = SanitizedNearPlane();
    return max(isfinite(pc.cameraFar) ? pc.cameraFar : n + 1e-3,
               n + 1e-3);
}

float LinearizeDepthForProjection(float depth)
{
    float n = SanitizedNearPlane();
    float f = SanitizedFarPlane();
    if (pc.orthographicDepth != 0u)
    {
        return lerp(f, n, saturate(depth));
    }
    return (n * f) / (saturate(depth) * (f - n) + n);
}

uint2 ClampPixel(uint2 pixel, uint2 dimensions)
{
    return min(pixel, dimensions - uint2(1u, 1u));
}

uint2 DepthPixelForAoPixel(uint2 aoPixel, uint2 aoDimensions,
                           uint2 depthDimensions)
{
    float2 depthCoord =
        (float2(aoPixel) + 0.5.xx) *
        (float2(depthDimensions) / float2(aoDimensions));
    return ClampPixel(uint2(depthCoord), depthDimensions);
}

float LoadAo(uint2 pixel)
{
    float ao = inAO.Load(int3(int(pixel.x), int(pixel.y), 0));
    return saturate(isfinite(ao) ? ao : 1.0);
}

float LoadDepthForAoPixel(uint2 aoPixel, uint2 aoDimensions,
                          uint2 depthDimensions)
{
    uint2 depthPixel =
        DepthPixelForAoPixel(aoPixel, aoDimensions, depthDimensions);
    return gDepthTexture.Load(int3(int(depthPixel.x), int(depthPixel.y), 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    if (pc.width == 0u || pc.height == 0u ||
        dtid.x >= pc.width || dtid.y >= pc.height)
    {
        return;
    }

    uint depthWidth;
    uint depthHeight;
    gDepthTexture.GetDimensions(depthWidth, depthHeight);
    uint2 aoDimensions = uint2(pc.width, pc.height);
    uint2 depthDimensions =
        uint2(max(depthWidth, 1u), max(depthHeight, 1u));

    uint2 centerPixel = dtid.xy;
    float centerDepth =
        LoadDepthForAoPixel(centerPixel, aoDimensions, depthDimensions);

    if (!ValidReverseZDepth(centerDepth))
    {
        outAO[dtid.xy] = 1.0;
        return;
    }

    float centerLinearDepth = LinearizeDepthForProjection(centerDepth);

    float totalWeight = 0.0;
    float totalAO = 0.0;

    for (int y = -KERNEL_RADIUS; y <= KERNEL_RADIUS; ++y)
    {
        for (int x = -KERNEL_RADIUS; x <= KERNEL_RADIUS; ++x)
        {
            int2 samplePixelSigned =
                clamp(int2(dtid.xy) + int2(x, y),
                      int2(0, 0),
                      int2(int(pc.width) - 1, int(pc.height) - 1));
            uint2 samplePixel = uint2(samplePixelSigned);

            float sampleDepth =
                LoadDepthForAoPixel(samplePixel, aoDimensions, depthDimensions);
            float sampleAO = LoadAo(samplePixel);

            float sampleLinearDepth =
                ValidReverseZDepth(sampleDepth) ?
                    LinearizeDepthForProjection(sampleDepth) :
                    SanitizedFarPlane();
            float depthDiff = abs(centerLinearDepth - sampleLinearDepth);
            float depthThreshold =
                max(isfinite(pc.depthThreshold) ? pc.depthThreshold : 0.0,
                    0.0);
            float depthLimit = max(depthThreshold, centerLinearDepth * 0.015);
            float depthWeight = depthDiff < depthLimit ? 1.0 : 0.0;

            float spatialWeight = GaussianWeight(x, y);
            float w = spatialWeight * depthWeight;

            totalAO += sampleAO * w;
            totalWeight += w;
        }
    }

    outAO[dtid.xy] = totalAO / max(totalWeight, 1e-4);
}
