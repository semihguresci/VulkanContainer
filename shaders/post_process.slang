#include "lighting_structs.slang"
#include "oit_common.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] SamplerState gBufferSampler;
[[vk::binding(1, 0)]] Texture2D<float4> sceneColorTexture;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float> gDepthTexture;
[[vk::binding(7, 0)]] Texture2D<float4> gBloomTexture;
[[vk::binding(8, 0)]] SamplerState bloomSampler;
[[vk::binding(10, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(11, 0)]] ConstantBuffer<ShadowBuffer> uShadow;
[[vk::binding(12, 0)]] Texture2DArray<float> gShadowAtlas;
[[vk::binding(13, 0)]] StructuredBuffer<ExposureStateData> uExposureState;

[[vk::binding(9, 0)]] StructuredBuffer<TileLightGrid> uTileGrid;
[[vk::binding(0, 1)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 1)]] StructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 1)]] StructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 1)]] ConstantBuffer<OitMetadataBuffer> uOit;

[[vk::push_constant]]
ConstantBuffer<PostProcessPushConstants> pc;

static const uint EXPOSURE_MODE_MANUAL = 0u;

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
};

struct Rect
{
    float2 min;
    float2 max;
};

float3 ToneMapACES(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 ToSwapchainLinear(float3 color)
{
    // The swapchain is created with an sRGB format, so Vulkan performs the
    // final linear-to-sRGB conversion when this fragment output is stored.
    return saturate(color);
}

float ResolveExposureForToneMapping()
{
    if (pc.exposureMode == EXPOSURE_MODE_MANUAL)
    {
        return max(pc.exposure, 0.0);
    }

    ExposureStateData exposureState = uExposureState[0];
    float exposure =
        exposureState.initialized > 0.5 && isfinite(exposureState.exposure)
            ? exposureState.exposure
            : pc.exposure;
    if (!isfinite(exposure))
    {
        exposure = pc.exposure;
    }

    float minExposure = max(pc.minExposure, 0.0);
    float maxExposure = max(pc.maxExposure, minExposure);
    return clamp(max(exposure, 0.0), minExposure, maxExposure);
}

float4 CompositeFrontToBack(float3 sceneColor, uint sortedIndices[MAX_OIT_NODES_PER_PIXEL], uint nodeCount,
                            out float3 transparentColor, out float revealage)
{
    // Transparent nodes are already sorted nearest-to-farthest by the caller.
    // Front-to-back compositing lets revealage track how much opaque scene
    // color remains visible through the accumulated alpha.
    float3 accumColor = 0.0.xxx;
    float accumAlpha = 0.0;

    for (uint i = 0u; i < nodeCount; ++i)
    {
        TransparentNode node = transparentNodes[sortedIndices[i]];
        float alpha = saturate(node.color.a);
        float weight = (1.0 - accumAlpha) * alpha;
        accumColor += node.color.rgb * weight;
        accumAlpha += weight;
    }

    transparentColor = accumColor;
    revealage = 1.0 - accumAlpha;
    return float4(accumColor + sceneColor * revealage, 1.0);
}

struct OitResolveResult
{
    float3 compositedColor;
    float3 transparentColor;
    float revealage;
};

OitResolveResult ResolveOitPixel(uint2 pixelCoord, float3 sceneColor)
{
    OitResolveResult result;
    result.compositedColor = sceneColor;
    result.transparentColor = 0.0.xxx;
    result.revealage = 1.0;

    if (pc.oitEnabled == 0u ||
        pixelCoord.x >= uOit.viewportWidth ||
        pixelCoord.y >= uOit.viewportHeight)
    {
        return result;
    }

    uint current = headPointerImage.Load(int2(pixelCoord));
    if (current == INVALID_NODE_INDEX)
    {
        return result;
    }

    uint sortedIndices[MAX_OIT_NODES_PER_PIXEL];
    uint nodeCount = 0u;
    while (current != INVALID_NODE_INDEX && nodeCount < MAX_OIT_NODES_PER_PIXEL)
    {
        if (current >= uOit.nodeCapacity)
        {
            break;
        }

        TransparentNode node = transparentNodes[current];
        uint insertAt = nodeCount;
        while (insertAt > 0u)
        {
            TransparentNode prevNode = transparentNodes[sortedIndices[insertAt - 1u]];
            if (prevNode.depth >= node.depth)
            {
                break;
            }
            sortedIndices[insertAt] = sortedIndices[insertAt - 1u];
            --insertAt;
        }
        sortedIndices[insertAt] = current;
        ++nodeCount;
        current = node.next;
    }

    if (nodeCount > 0u)
    {
        float4 composited = CompositeFrontToBack(
            sceneColor, sortedIndices, nodeCount,
            result.transparentColor, result.revealage);
        result.compositedColor = composited.rgb;
    }
    return result;
}

bool InRect(float2 uv, Rect rect)
{
    return all(uv >= rect.min) && all(uv <= rect.max);
}

Rect InsetRect(Rect rect, float padding)
{
    Rect inner;
    inner.min = rect.min + padding.xx;
    inner.max = rect.max - padding.xx;
    if (inner.min.x > inner.max.x)
    {
        float midX = (rect.min.x + rect.max.x) * 0.5;
        inner.min.x = midX;
        inner.max.x = midX;
    }
    if (inner.min.y > inner.max.y)
    {
        float midY = (rect.min.y + rect.max.y) * 0.5;
        inner.min.y = midY;
        inner.max.y = midY;
    }
    return inner;
}

float2 RectUV(float2 uv, Rect rect)
{
    float2 rectSize = max(rect.max - rect.min, 1e-6.xx);
    return saturate((uv - rect.min) / rectSize);
}

bool IsPanelBorder(float2 uv, Rect rect, float borderSize)
{
    if (!InRect(uv, rect))
    {
        return false;
    }

    Rect innerRect = InsetRect(rect, borderSize);
    if (innerRect.min.x >= innerRect.max.x || innerRect.min.y >= innerRect.max.y)
    {
        return true;
    }

    return !InRect(uv, innerRect);
}

float3 MaterialVisual(float4 material)
{
    return float3(saturate(material.x), saturate(material.y), 0.0);
}

float3 NormalVisual(float4 encodedNormal)
{
    float3 normal = normalize(encodedNormal.xyz * 2.0 - 1.0);
    if (!all(isfinite(normal)))
    {
        normal = float3(0.0, 1.0, 0.0);
    }
    return normal * 0.5 + 0.5;
}

float3 DepthVisual(float depth)
{
    if (depth == 0.0)
    {
        return 0.0.xxx;
    }
    float n = pc.cameraNear;
    float f = pc.cameraFar;
    float linearDepth = (n * f) / (depth * (f - n) + n);
    float normalizedDepth = saturate((linearDepth - n) / max(f - n, 1e-6));
    return normalizedDepth.xxx;
}

float3 ReconstructWorldPosition(float2 texCoord, float depth,
                                column_major float4x4 inverseViewProj)
{
    float2 ndcXY = texCoord * 2.0 - 1.0;
    ndcXY.y = -ndcXY.y;
    float4 clipPosition = float4(ndcXY, depth, 1.0);
    float4 worldPosition = mul(inverseViewProj, clipPosition);
    return worldPosition.xyz / max(worldPosition.w, 1e-6);
}

float EdgeDistance01(float2 uv)
{
    return min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
}

uint2 TexturePixelFromUV(float2 uv, uint width, uint height)
{
    uint2 maxPixel = uint2(max(width, 1u) - 1u, max(height, 1u) - 1u);
    return min(uint2(saturate(uv) * float2(max(width, 1u), max(height, 1u))), maxPixel);
}

uint2 OitPixelFromUV(float2 uv)
{
    uint2 maxPixel = uint2(max(uOit.viewportWidth, 1u) - 1u,
                           max(uOit.viewportHeight, 1u) - 1u);
    return min(uint2(saturate(uv) * float2(max(uOit.viewportWidth, 1u),
                                           max(uOit.viewportHeight, 1u))),
               maxPixel);
}

float ViewDepthFromDeviceDepth(float depthSample)
{
    float n = pc.cameraNear;
    float f = pc.cameraFar;
    return (n * f) / (depthSample * (f - n) + n);
}

uint CascadeIndexForViewDepth(float viewZ)
{
    uint cascadeIndex = 3u;
    for (uint i = 0u; i < 3u; ++i)
    {
        if (viewZ < pc.cascadeSplits[i])
        {
            cascadeIndex = i;
            break;
        }
    }
    return cascadeIndex;
}

float3 LitViewColor(float2 uv, uint2 pixelCoord, float resolvedExposure,
                    bool oitAvailable)
{
    float3 litColor = sceneColorTexture.SampleLevel(gBufferSampler, uv, 0.0).rgb;
    float3 compositedColor = litColor;
    if (oitAvailable)
    {
        OitResolveResult oit = ResolveOitPixel(pixelCoord, litColor);
        compositedColor = oit.compositedColor;
    }

    if (pc.bloomEnabled != 0u)
    {
        float3 bloom = gBloomTexture.SampleLevel(bloomSampler, uv, 0.0).rgb;
        compositedColor += bloom * pc.bloomIntensity;
    }

    return ToSwapchainLinear(ToneMapACES(compositedColor * resolvedExposure));
}

float3 ShadowCascadeViewColor(uint2 pixelCoord, float3 sceneRgb)
{
    float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));
    if (depthSample == 0.0)
    {
        return 0.0.xxx;
    }

    float viewZ = ViewDepthFromDeviceDepth(depthSample);

    static const float3 cascadeColors[4] = {
        float3(0.2, 0.8, 0.2),
        float3(0.2, 0.5, 0.9),
        float3(0.9, 0.7, 0.1),
        float3(0.9, 0.2, 0.2),
    };

    uint cascadeIndex = CascadeIndexForViewDepth(viewZ);

    float blendIndicator = 0.0;
    if (cascadeIndex < 3u)
    {
        float splitDepth = pc.cascadeSplits[cascadeIndex];
        float prevSplit = (cascadeIndex == 0u) ? 0.0 : pc.cascadeSplits[cascadeIndex - 1u];
        float cascadeRange = splitDepth - prevSplit;
        float blendFraction = clamp(uShadow.filterSettings.y, 0.0, 0.45);
        float blendZone = cascadeRange * blendFraction;
        float blendStart = splitDepth - blendZone;
        if (viewZ > blendStart)
        {
            blendIndicator = saturate((viewZ - blendStart) / max(blendZone, 1e-6));
        }
    }

    float3 color = cascadeColors[cascadeIndex];
    if (blendIndicator > 0.0 && cascadeIndex < 3u)
    {
        color = lerp(color, cascadeColors[cascadeIndex + 1u], blendIndicator);
    }

    float3 sceneGray = dot(sceneRgb, float3(0.299, 0.587, 0.114)).xxx;
    return sceneGray * 0.15 + color * 0.5;
}

float3 ShadowTexelDensityViewColor(uint2 pixelCoord)
{
    float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));
    if (depthSample == 0.0)
    {
        return 0.0.xxx;
    }

    uint cascadeIndex = CascadeIndexForViewDepth(ViewDepthFromDeviceDepth(depthSample));
    float texelSize = max(uShadow.cascades[cascadeIndex].texelSize, 1e-6);
    float texelsPerMeter = 1.0 / texelSize;
    float density = saturate(log2(max(texelsPerMeter, 1e-4)) / 8.0);
    return lerp(float3(0.05, 0.08, 0.14), float3(0.15, 0.8, 1.0), density);
}

float3 TileLightHeatViewColor(uint2 pixelCoord, uint width, uint height,
                              float3 sceneRgb)
{
    if (pc.totalLights == 0u)
    {
        return 0.0.xxx;
    }

    static const uint TILE_SIZE = 16u;
    float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));

    uint2 tileCoord = pixelCoord / TILE_SIZE;
    uint safeDepthSliceCount = max(pc.depthSliceCount, 1u);
    uint tileCountY = (height + TILE_SIZE - 1u) / TILE_SIZE;
    uint sliceZ = DepthToClusterSlice(depthSample, pc.cameraNear, pc.cameraFar,
                                      safeDepthSliceCount);
    uint clusterIndex =
        (sliceZ * tileCountY + tileCoord.y) * pc.tileCountX + tileCoord.x;
    uint lightCount = uTileGrid[clusterIndex].count;

    float maxLights = max(float(pc.totalLights), 1.0);
    float t = saturate(float(lightCount) / maxLights);

    float3 heatColor;
    if (t < 0.2)
    {
        heatColor = lerp(float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), t / 0.2);
    }
    else if (t < 0.4)
    {
        heatColor = lerp(float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 1.0), (t - 0.2) / 0.2);
    }
    else if (t < 0.6)
    {
        heatColor = lerp(float3(0.0, 1.0, 1.0), float3(0.0, 1.0, 0.0), (t - 0.4) / 0.2);
    }
    else if (t < 0.8)
    {
        heatColor = lerp(float3(0.0, 1.0, 0.0), float3(1.0, 1.0, 0.0), (t - 0.6) / 0.2);
    }
    else
    {
        heatColor = lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), (t - 0.8) / 0.2);
    }

    float3 sceneGray = dot(sceneRgb, float3(0.299, 0.587, 0.114)).xxx;
    float3 result = sceneGray * 0.15 + heatColor * 0.65;
    bool onTileBorder = (pixelCoord.x % TILE_SIZE == 0) || (pixelCoord.y % TILE_SIZE == 0);
    return onTileBorder ? lerp(result, float3(0.3, 0.3, 0.3), 0.5) : result;
}

float3 DebugTextureViewColor(uint mode, float2 uv, uint2 pixelCoord,
                             uint width, uint height, float resolvedExposure,
                             bool oitAvailable)
{
    if (mode == 0u)
    {
        return LitViewColor(uv, pixelCoord, resolvedExposure, oitAvailable);
    }
    if (mode == 1u)
    {
        return gAlbedoTexture.SampleLevel(gBufferSampler, uv, 0.0).rgb;
    }
    if (mode == 2u)
    {
        return NormalVisual(gNormalTexture.SampleLevel(gBufferSampler, uv, 0.0));
    }
    if (mode == 3u)
    {
        return MaterialVisual(gMaterialTexture.Load(int3(pixelCoord, 0)));
    }
    if (mode == 4u)
    {
        return DepthVisual(gDepthTexture.SampleLevel(gBufferSampler, uv, 0.0));
    }
    if (mode == 5u)
    {
        float3 emissive = gEmissiveTexture.SampleLevel(gBufferSampler, uv, 0.0).rgb;
        return ToSwapchainLinear(ToneMapACES(emissive * 2.0));
    }
    if (mode == 6u || mode == 7u)
    {
        if (!oitAvailable)
        {
            return 0.0.xxx;
        }
        OitResolveResult oit = ResolveOitPixel(pixelCoord, 0.0.xxx);
        return mode == 6u ? oit.transparentColor : oit.revealage.xxx;
    }
    if (mode == 11u)
    {
        if (pc.cascadeSplits[0] <= 0.0)
        {
            return 0.0.xxx;
        }
        return ShadowCascadeViewColor(pixelCoord,
                                      sceneColorTexture.SampleLevel(gBufferSampler, uv, 0.0).rgb);
    }
    if (mode == 12u)
    {
        return TileLightHeatViewColor(pixelCoord, width, height,
                                      sceneColorTexture.SampleLevel(gBufferSampler, uv, 0.0).rgb);
    }
    if (mode == 13u)
    {
        if (pc.cascadeSplits[0] <= 0.0)
        {
            return 0.0.xxx;
        }
        return ShadowTexelDensityViewColor(pixelCoord);
    }
    if (mode == 14u)
    {
        if (pc.bloomEnabled == 0u)
        {
            return 0.0.xxx;
        }
        return ToSwapchainLinear(ToneMapACES(
            gBloomTexture.SampleLevel(bloomSampler, uv, 0.0).rgb * max(pc.bloomIntensity, 1.0)));
    }
    return 0.0.xxx;
}

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 kPositions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0),
    };

    static const float2 kTexCoords[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };

    VSOutput output;
    output.pos = float4(kPositions[vertexIndex], 0.0, 1.0);
    output.texCoord = kTexCoords[vertexIndex];
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    uint width = 0u;
    uint height = 0u;
    sceneColorTexture.GetDimensions(width, height);
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));
    float2 screenUV = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));

    uint outputMode = pc.outputMode;
    if (outputMode == 1u)
    {
        return float4(gAlbedoTexture.Load(int3(pixelCoord, 0)).rgb, 1.0);
    }

    if (outputMode == 2u)
    {
        return float4(NormalVisual(gNormalTexture.Load(int3(pixelCoord, 0))), 1.0);
    }

    if (outputMode == 3u)
    {
        return float4(MaterialVisual(gMaterialTexture.Load(int3(pixelCoord, 0))), 1.0);
    }

    if (outputMode == 4u)
    {
        float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));
        return float4(DepthVisual(depthSample), 1.0);
    }

    if (outputMode == 5u)
    {
        float3 emissive = gEmissiveTexture.Load(int3(pixelCoord, 0)).rgb;
        return float4(ToSwapchainLinear(ToneMapACES(emissive * 2.0)), 1.0);
    }

    if (outputMode == 6u || outputMode == 7u)
    {
        if (pc.oitEnabled == 0u)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        OitResolveResult oit = ResolveOitPixel(pixelCoord, 0.0.xxx);
        if (outputMode == 6u)
        {
            return float4(oit.transparentColor, 1.0);
        }
        return float4(oit.revealage.xxx, 1.0);
    }

    float4 sceneColor = sceneColorTexture.Load(int3(pixelCoord, 0));

    float3 compositedColor = sceneColor.rgb;
    float resolvedExposure = ResolveExposureForToneMapping();

    bool oitAvailable = pc.oitEnabled != 0u;
    bool resolveCurrentPixelOit =
        oitAvailable && outputMode == 0u;

    if (resolveCurrentPixelOit)
    {
        OitResolveResult oit = ResolveOitPixel(pixelCoord, sceneColor.rgb);
        compositedColor = oit.compositedColor;
    }

    if (outputMode == 8u)
    {
        Rect overviewRect = { float2(0.02, 0.04), float2(0.98, 0.96) };
        const float panelPadding = 0.006;
        const float3 panelBorderColor = float3(0.60, 0.62, 0.66);
        const float3 panelTileBackgroundColor = float3(0.12, 0.12, 0.13);

        if (InRect(screenUV, overviewRect))
        {
            static const uint OVERVIEW_COLUMNS = 3u;
            static const uint OVERVIEW_ROWS = 4u;
            float2 gridUV = RectUV(screenUV, overviewRect);
            uint column = min(uint(gridUV.x * float(OVERVIEW_COLUMNS)), OVERVIEW_COLUMNS - 1u);
            uint row = min(uint(gridUV.y * float(OVERVIEW_ROWS)), OVERVIEW_ROWS - 1u);
            uint slot = row * OVERVIEW_COLUMNS + column;

            uint panelMode = 0u;
            if (slot == 0u) panelMode = 0u;       // Lit
            else if (slot == 1u) panelMode = 1u;  // Albedo
            else if (slot == 2u) panelMode = 2u;  // Normals
            else if (slot == 3u) panelMode = 3u;  // Material
            else if (slot == 4u) panelMode = 4u;  // Depth
            else if (slot == 5u) panelMode = 5u;  // Emissive
            else if (slot == 6u) panelMode = 6u;  // Transparency
            else if (slot == 7u) panelMode = 7u;  // Revealage
            else if (slot == 8u) panelMode = 11u; // Shadow cascades
            else if (slot == 9u) panelMode = 12u; // Tile light heat map
            else if (slot == 10u) panelMode = 13u; // Shadow texel density
            else if (slot == 11u) panelMode = 14u; // Bloom texture

            float2 cellSize =
                (overviewRect.max - overviewRect.min) /
                float2(float(OVERVIEW_COLUMNS), float(OVERVIEW_ROWS));
            Rect panelRect;
            panelRect.min = overviewRect.min + cellSize * float2(float(column), float(row));
            panelRect.max = panelRect.min + cellSize;

            if (IsPanelBorder(screenUV, panelRect, panelPadding))
            {
                return float4(panelBorderColor, 1.0);
            }

            Rect panelInner = InsetRect(panelRect, panelPadding);
            if (!InRect(screenUV, panelInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }

            float2 panelUV = RectUV(screenUV, panelInner);
            uint2 panelPixel = TexturePixelFromUV(panelUV, width, height);
            if (panelMode == 6u || panelMode == 7u)
            {
                panelPixel = OitPixelFromUV(panelUV);
            }
            return float4(DebugTextureViewColor(panelMode, panelUV, panelPixel,
                                                width, height, resolvedExposure,
                                                oitAvailable), 1.0);
        }

        float3 overviewCompositedColor = sceneColor.rgb;
        if (oitAvailable)
        {
            OitResolveResult overviewOit =
                ResolveOitPixel(pixelCoord, sceneColor.rgb);
            overviewCompositedColor = overviewOit.compositedColor;
        }
        float3 overviewBackground = ToneMapACES(overviewCompositedColor * resolvedExposure);
        overviewBackground = ToSwapchainLinear(overviewBackground);
        overviewBackground = lerp(overviewBackground, panelTileBackgroundColor, 0.35);
        return float4(overviewBackground, 1.0);
    }

    if (outputMode == 10u)
    {
        return float4(sceneColor.rgb, 1.0);
    }

    // Shadow cascade debug view: color-code pixels by cascade index.
    if (outputMode == 11u)
    {
        float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));
        if (depthSample == 0.0)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }

        // Linearize reverse-Z depth to positive view-space distance.
        // With perspectiveRH_ReverseZ: near maps to depth=1, far maps to depth=0.
        //   viewZ = (near * far) / (depth * (far - near) + near)
        float n = pc.cameraNear;
        float f = pc.cameraFar;
        float viewZ = (n * f) / (depthSample * (f - n) + n);

        static const float3 cascadeColors[4] = {
            float3(0.2, 0.8, 0.2),   // green  — cascade 0 (nearest)
            float3(0.2, 0.5, 0.9),   // blue   — cascade 1
            float3(0.9, 0.7, 0.1),   // yellow — cascade 2
            float3(0.9, 0.2, 0.2),   // red    — cascade 3 (farthest)
        };

        uint cascadeIndex = 3;
        for (uint i = 0; i < 3; ++i)
        {
            if (viewZ < pc.cascadeSplits[i])
            {
                cascadeIndex = i;
                break;
            }
        }

        // Blend zone visualization: brighten pixels in the transition overlap.
        float blendIndicator = 0.0;
        if (cascadeIndex < 3)
        {
            float splitDepth = pc.cascadeSplits[cascadeIndex];
            float prevSplit = (cascadeIndex == 0) ? 0.0 : pc.cascadeSplits[cascadeIndex - 1];
            float cascadeRange = splitDepth - prevSplit;
            float blendFraction = clamp(uShadow.filterSettings.y, 0.0, 0.45);
            float blendZone = cascadeRange * blendFraction;
            float blendStart = splitDepth - blendZone;
            if (viewZ > blendStart)
            {
                blendIndicator = saturate((viewZ - blendStart) / max(blendZone, 1e-6));
            }
        }

        float3 color = cascadeColors[cascadeIndex];
        // Mix with next cascade color in blend zone for visual verification.
        if (blendIndicator > 0.0 && cascadeIndex < 3)
        {
            color = lerp(color, cascadeColors[cascadeIndex + 1], blendIndicator);
        }

        // Overlay on dimmed scene for context.
        float3 sceneGray = dot(sceneColor.rgb, float3(0.299, 0.587, 0.114)).xxx;
        float3 dimScene = sceneGray * 0.15;
        float3 result = dimScene + color * 0.5;
        return float4(result, 1.0);
    }

    // Shadow texel density debug view: brighter means finer world-space shadow
    // resolution in the selected cascade.
    if (outputMode == 13u)
    {
        float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));
        if (depthSample == 0.0)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }

        float n = pc.cameraNear;
        float f = pc.cameraFar;
        float viewZ = (n * f) / (depthSample * (f - n) + n);

        uint cascadeIndex = 3;
        for (uint i = 0; i < 3; ++i)
        {
            if (viewZ < pc.cascadeSplits[i])
            {
                cascadeIndex = i;
                break;
            }
        }

        float texelSize = max(uShadow.cascades[cascadeIndex].texelSize, 1e-6);
        float texelsPerMeter = 1.0 / texelSize;
        float density = saturate(log2(max(texelsPerMeter, 1e-4)) / 8.0);
        float3 color = lerp(float3(0.05, 0.08, 0.14),
                            float3(0.15, 0.8, 1.0), density);
        return float4(color, 1.0);
    }

    // Tile light heat map: color-code pixels by per-tile light count.
    if (outputMode == 12u)
    {
        if (pc.totalLights == 0u)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }

        static const uint TILE_SIZE = 16u;
        float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));

        uint2 tileCoord = pixelCoord / TILE_SIZE;
        uint safeDepthSliceCount = max(pc.depthSliceCount, 1u);
        uint tileCountY = (height + TILE_SIZE - 1u) / TILE_SIZE;
        uint sliceZ = DepthToClusterSlice(depthSample, pc.cameraNear, pc.cameraFar,
                                          safeDepthSliceCount);
        uint clusterIndex =
            (sliceZ * tileCountY + tileCoord.y) * pc.tileCountX + tileCoord.x;
        uint lightCount = uTileGrid[clusterIndex].count;

        // Heat map: black (0) → blue → cyan → green → yellow → red → white.
        float maxLights = max(float(pc.totalLights), 1.0);
        float t = saturate(float(lightCount) / maxLights);

        float3 heatColor;
        if (t < 0.2)
        {
            float s = t / 0.2;
            heatColor = lerp(float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), s);
        }
        else if (t < 0.4)
        {
            float s = (t - 0.2) / 0.2;
            heatColor = lerp(float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 1.0), s);
        }
        else if (t < 0.6)
        {
            float s = (t - 0.4) / 0.2;
            heatColor = lerp(float3(0.0, 1.0, 1.0), float3(0.0, 1.0, 0.0), s);
        }
        else if (t < 0.8)
        {
            float s = (t - 0.6) / 0.2;
            heatColor = lerp(float3(0.0, 1.0, 0.0), float3(1.0, 1.0, 0.0), s);
        }
        else
        {
            float s = (t - 0.8) / 0.2;
            heatColor = lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), s);
        }

        // Show tile grid lines for visual clarity.
        bool onTileBorder = (pixelCoord.x % TILE_SIZE == 0) || (pixelCoord.y % TILE_SIZE == 0);

        // Overlay on dimmed scene.
        float3 sceneGray = dot(sceneColor.rgb, float3(0.299, 0.587, 0.114)).xxx;
        float3 dimScene = sceneGray * 0.15;
        float3 result = dimScene + heatColor * 0.65;

        if (onTileBorder)
        {
            result = lerp(result, float3(0.3, 0.3, 0.3), 0.5);
        }

        return float4(result, 1.0);
    }

    // Add bloom contribution before tone mapping.
    float3 finalHdr = compositedColor;
    if (pc.bloomEnabled != 0)
    {
        float3 bloom = gBloomTexture.Sample(bloomSampler, screenUV).rgb;
        finalHdr += bloom * pc.bloomIntensity;
    }

    float3 mapped = ToneMapACES(finalHdr * resolvedExposure);
    return float4(ToSwapchainLinear(mapped), 1.0);
}
