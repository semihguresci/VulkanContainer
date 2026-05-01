#pragma once

struct BindlessPushConstants
{
    uint objectIndex;
};

struct LightPushConstants
{
    float4 positionRadius;
    float4 colorIntensity;
    float4 directionInnerCos;
    float4 coneOuterCosType;
};

struct ShadowPushConstants
{
    uint objectIndex;
    uint cascadeIndex;
};

struct PostProcessPushConstants
{
    uint outputMode;
    uint bloomEnabled;
    float bloomIntensity;
    float exposure;
    float cameraNear;
    float cameraFar;
    float cascadeSplits[4];
    uint tileCountX;
    uint totalLights;
    uint depthSliceCount;
    uint oitEnabled;
    uint exposureMode;
    float targetLuminance;
    float minExposure;
    float maxExposure;
    float adaptationRate;
};

struct TileCullPushConstants
{
    uint tileCountX;
    uint tileCountY;
    uint depthSliceCount;
    uint totalLights;
    float cameraNear;
    float cameraFar;
};

struct TiledLightingPushConstants
{
    uint tileCountX;
    uint tileCountY;
    uint depthSliceCount;
    float cameraNear;
    float cameraFar;
};

struct CullPushConstants
{
    uint objectCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct HiZPushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstMipLevel;
    uint pad0;
};

struct WireframePushConstants
{
    uint objectIndex;
    float padding0;
    float padding1;
    float padding2;
    float padding3;
    float padding4;
    float padding5;
    float padding6;
    float4 colorIntensity;
    float lineWidth;
    float padding7;
    float padding8;
    float padding9;
};

struct NormalValidationPushConstants
{
    uint objectIndex;
    uint showFaceFill;
    float faceAlpha;
    uint faceClassificationFlags;
};

struct SurfaceNormalPushConstants
{
    uint objectIndex;
    float lineLength;
    float lineOffset;
};

struct GBufferCompositePushConstants
{
    uint outputMode;
};

struct BloomDownsamplePushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float threshold;
    float knee;
    uint mipLevel;
    uint pad0;
};

struct BloomUpsamplePushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float filterRadius;
    float bloomIntensity;
    uint isFinalPass;
    uint pad0;
};

struct ExposureHistogramPushConstants
{
    uint width;
    uint height;
    uint binCount;
    float minLogLuminance;
    float maxLogLuminance;
    float pad0;
    float pad1;
    float pad2;
};

struct ExposureAdaptPushConstants
{
    uint binCount;
    uint exposureMode;
    float minLogLuminance;
    float maxLogLuminance;
    float targetLuminance;
    float minExposure;
    float maxExposure;
    float adaptationRate;
    float meteringLowPercentile;
    float meteringHighPercentile;
    float deltaSeconds;
    float manualExposure;
    float pad0;
    float pad1;
    float pad2;
    float pad3;
};

struct EquirectPushConstants
{
    uint faceIndex;
    uint faceSize;
};

struct IrradiancePushConstants
{
    uint faceIndex;
    uint faceSize;
};

struct PrefilterPushConstants
{
    uint faceIndex;
    uint faceSize;
    float roughness;
    uint sourceMipCount;
};

struct GtaoPushConstants
{
    float aoRadius;
    float aoIntensity;
    uint sampleCount;
    uint pad0;
    uint fullWidth;
    uint fullHeight;
    uint pad1;
    uint pad2;
};

struct BlurPushConstants
{
    uint width;
    uint height;
    float depthThreshold;
    float cameraNear;
    float cameraFar;
    float pad0;
    float pad1;
    float pad2;
};
