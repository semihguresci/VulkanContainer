#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(1, 0)]] RWStructuredBuffer<uint> luminanceHistogram;
[[vk::binding(2, 0)]] RWStructuredBuffer<ExposureStateData> exposureState;
[[vk::push_constant]] ConstantBuffer<ExposureAdaptPushConstants> pc;

static const uint EXPOSURE_MODE_MANUAL = 0u;

float FiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float ClampExposure(float exposure, float fallback)
{
    float minExposure = max(FiniteOr(pc.minExposure, 0.03125), 0.0);
    float maxExposure = max(FiniteOr(pc.maxExposure, 8.0), minExposure);
    return clamp(FiniteOr(exposure, fallback), minExposure, maxExposure);
}

float BinCenterLogLuminance(uint bin, uint binCount)
{
    float denominator = max(float(binCount - 1u), 1.0);
    float t = (float(bin) - 0.5) / denominator;
    return pc.minLogLuminance +
        t * max(pc.maxLogLuminance - pc.minLogLuminance, 1.0e-4);
}

[numthreads(1, 1, 1)]
[shader("compute")]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0u || dispatchThreadId.y != 0u ||
        dispatchThreadId.z != 0u)
    {
        return;
    }

    uint binCount = clamp(pc.binCount, 2u, 64u);
    float manualExposure = max(FiniteOr(pc.manualExposure, 0.25), 0.0);
    if (pc.exposureMode == EXPOSURE_MODE_MANUAL)
    {
        ExposureStateData manualState;
        manualState.exposure = ClampExposure(manualExposure, manualExposure);
        manualState.averageLuminance = max(FiniteOr(pc.targetLuminance, 0.18), 1.0e-6);
        manualState.targetExposure = manualState.exposure;
        manualState.initialized = 1.0;
        exposureState[0] = manualState;
        return;
    }

    uint nonBlackSampleCount = 0u;
    for (uint bin = 1u; bin < binCount; ++bin)
    {
        nonBlackSampleCount += luminanceHistogram[bin];
    }

    float averageLuminance = exp2(pc.minLogLuminance);
    if (nonBlackSampleCount > 0u)
    {
        float lowPercentile = clamp(FiniteOr(pc.meteringLowPercentile, 0.50), 0.0, 0.99);
        float highPercentile =
            clamp(FiniteOr(pc.meteringHighPercentile, 0.95),
                  lowPercentile + 0.01, 1.0);
        float lowerSample = float(nonBlackSampleCount) * lowPercentile;
        float upperSample = float(nonBlackSampleCount) * highPercentile;
        float cumulativeSamples = 0.0;
        float weightedLog2Luminance = 0.0;
        float accumulatedSamples = 0.0;

        for (uint bin = 1u; bin < binCount; ++bin)
        {
            uint count = luminanceHistogram[bin];
            if (count == 0u)
            {
                continue;
            }

            float binStart = cumulativeSamples;
            cumulativeSamples += float(count);
            float binEnd = cumulativeSamples;
            float includedSamples =
                max(0.0, min(binEnd, upperSample) - max(binStart, lowerSample));
            if (includedSamples <= 0.0)
            {
                continue;
            }

            weightedLog2Luminance +=
                BinCenterLogLuminance(bin, binCount) * includedSamples;
            accumulatedSamples += includedSamples;
        }

        if (accumulatedSamples <= 0.0)
        {
            for (uint bin = 1u; bin < binCount; ++bin)
            {
                uint count = luminanceHistogram[bin];
                weightedLog2Luminance +=
                    BinCenterLogLuminance(bin, binCount) * float(count);
            }
            accumulatedSamples = max(float(nonBlackSampleCount), 1.0);
        }

        averageLuminance = exp2(weightedLog2Luminance / accumulatedSamples);
    }

    float targetLuminance = max(FiniteOr(pc.targetLuminance, 0.18), 0.001);
    float targetExposure =
        ClampExposure(targetLuminance / max(averageLuminance, 1.0e-6),
                      manualExposure);

    ExposureStateData previous = exposureState[0];
    bool initialized =
        previous.initialized > 0.5 && isfinite(previous.exposure);
    float currentExposure = initialized
        ? ClampExposure(previous.exposure, manualExposure)
        : targetExposure;
    float adaptationAlpha =
        1.0 - exp(-max(FiniteOr(pc.adaptationRate, 1.5), 0.0) *
                  max(FiniteOr(pc.deltaSeconds, 1.0 / 60.0), 0.0));

    ExposureStateData next;
    next.exposure = ClampExposure(
        lerp(currentExposure, targetExposure, saturate(adaptationAlpha)),
        manualExposure);
    next.averageLuminance = averageLuminance;
    next.targetExposure = targetExposure;
    next.initialized = 1.0;
    exposureState[0] = next;
}
