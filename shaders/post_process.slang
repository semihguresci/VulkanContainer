static const uint INVALID_NODE_INDEX = 0xffffffffu;
static const uint MAX_OIT_NODES_PER_PIXEL = 64u;

struct PushConstants
{
    uint outputMode;
    uint bloomEnabled;
    float bloomIntensity;
    float cameraNear;
    float cameraFar;
    float cascadeSplits[4];
    uint tileCountX;
    uint totalLights;
    uint depthSliceCount;
};

struct TransparentNode
{
    float4 color;
    float depth;
    uint next;
    float2 padding;
};

struct OitMetadataBuffer
{
    uint nodeCapacity;
    uint viewportWidth;
    uint viewportHeight;
    uint reserved;
};

struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ShadowCascadeData
{
    column_major float4x4 viewProj;
    float splitDepth;
    float3 padding;
};

struct ShadowBuffer
{
    ShadowCascadeData cascades[4];
};

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

struct TileLightGrid
{
    uint offset;
    uint count;
};

float LinearizeReverseZDepth(float depth, float nearPlane, float farPlane)
{
    float n = max(nearPlane, 1e-4);
    float f = max(farPlane, n + 1e-3);
    return (n * f) / (saturate(depth) * (f - n) + n);
}

uint DepthToClusterSlice(float depth, float nearPlane, float farPlane, uint depthSliceCount)
{
    uint safeSliceCount = max(depthSliceCount, 1u);
    float n = max(nearPlane, 1e-4);
    float f = max(farPlane, n + 1e-3);
    float viewDepth = clamp(LinearizeReverseZDepth(depth, n, f), n, f);
    float normalized = log2(viewDepth / n) / max(log2(f / n), 1e-6);
    uint slice = uint(saturate(normalized) * float(safeSliceCount));
    return min(slice, safeSliceCount - 1u);
}

[[vk::binding(9, 0)]] StructuredBuffer<TileLightGrid> uTileGrid;
[[vk::binding(0, 1)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 1)]] StructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 1)]] StructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 1)]] ConstantBuffer<OitMetadataBuffer> uOit;

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

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

float4 CompositeFrontToBack(float3 sceneColor, uint sortedIndices[MAX_OIT_NODES_PER_PIXEL], uint nodeCount,
                            out float3 transparentColor, out float revealage)
{
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

    float4 sceneColor = sceneColorTexture.Load(int3(pixelCoord, 0));
    float4 albedo = gAlbedoTexture.Load(int3(pixelCoord, 0));
    float4 normal = gNormalTexture.Load(int3(pixelCoord, 0));
    float4 material = gMaterialTexture.Load(int3(pixelCoord, 0));
    float4 emissive = gEmissiveTexture.Load(int3(pixelCoord, 0));
    float depthSample = gDepthTexture.Load(int3(pixelCoord, 0));

    float3 transparentColor = 0.0.xxx;
    float3 compositedColor = sceneColor.rgb;
    float revealage = 1.0;

    if (pixelCoord.x < uOit.viewportWidth && pixelCoord.y < uOit.viewportHeight)
    {
        uint sortedIndices[MAX_OIT_NODES_PER_PIXEL];
        uint nodeCount = 0u;

        uint current = headPointerImage.Load(int2(pixelCoord));
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

        float4 composited = CompositeFrontToBack(sceneColor.rgb, sortedIndices, nodeCount,
                                                 transparentColor, revealage);
        compositedColor = composited.rgb;
    }

    if (pc.outputMode == 1)
    {
        return float4(albedo.rgb, 1.0);
    }

    if (pc.outputMode == 2)
    {
        return float4(NormalVisual(normal), 1.0);
    }

    if (pc.outputMode == 3)
    {
        return float4(MaterialVisual(material), 1.0);
    }

    if (pc.outputMode == 4)
    {
        return float4(DepthVisual(depthSample), 1.0);
    }

    if (pc.outputMode == 5)
    {
        float3 mappedEmissive = ToneMapACES(emissive.rgb * 2.0);
        return float4(ToSwapchainLinear(mappedEmissive), 1.0);
    }

    if (pc.outputMode == 6)
    {
        return float4(transparentColor, 1.0);
    }

    if (pc.outputMode == 7)
    {
        return float4(revealage.xxx, 1.0);
    }

    if (pc.outputMode == 8)
    {
        Rect centerRect = { float2(0.20, 0.18), float2(0.80, 0.82) };
        Rect albedoRect = { float2(0.02, 0.68), float2(0.18, 0.92) };
        Rect normalRect = { float2(0.82, 0.68), float2(0.98, 0.92) };
        Rect materialRect = { float2(0.02, 0.08), float2(0.18, 0.32) };
        Rect emissiveRect = { float2(0.82, 0.08), float2(0.98, 0.32) };
        Rect depthRect = { float2(0.82, 0.36), float2(0.98, 0.64) };

        const float panelPadding = 0.006;
        const float3 panelBorderColor = float3(0.60, 0.62, 0.66);
        const float3 panelTileBackgroundColor = float3(0.12, 0.12, 0.13);
        float3 overviewBackground = ToneMapACES(compositedColor * 0.25);
        overviewBackground = ToSwapchainLinear(overviewBackground);
        overviewBackground = lerp(overviewBackground, panelTileBackgroundColor, 0.35);

        if (IsPanelBorder(screenUV, centerRect, panelPadding) ||
            IsPanelBorder(screenUV, albedoRect, panelPadding) ||
            IsPanelBorder(screenUV, normalRect, panelPadding) ||
            IsPanelBorder(screenUV, materialRect, panelPadding) ||
            IsPanelBorder(screenUV, emissiveRect, panelPadding) ||
            IsPanelBorder(screenUV, depthRect, panelPadding))
        {
            return float4(panelBorderColor, 1.0);
        }

        Rect centerInner = InsetRect(centerRect, panelPadding);
        Rect albedoInner = InsetRect(albedoRect, panelPadding);
        Rect normalInner = InsetRect(normalRect, panelPadding);
        Rect materialInner = InsetRect(materialRect, panelPadding);
        Rect emissiveInner = InsetRect(emissiveRect, panelPadding);
        Rect depthInner = InsetRect(depthRect, panelPadding);

        if (InRect(screenUV, centerRect))
        {
            if (!InRect(screenUV, centerInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }

            float2 panelUV = RectUV(screenUV, centerInner);
            float3 litColor = sceneColorTexture.SampleLevel(gBufferSampler, panelUV, 0.0).rgb;
            const float exposure = 0.25;
            litColor *= exposure;

            uint2 panelPixel = uint2(panelUV * float2(uOit.viewportWidth, uOit.viewportHeight));
            float3 panelTransparentColor = 0.0.xxx;
            float panelRevealage = 1.0;
            float3 panelCompositedColor = litColor;

            if (panelPixel.x < uOit.viewportWidth && panelPixel.y < uOit.viewportHeight)
            {
                uint sortedIndices[MAX_OIT_NODES_PER_PIXEL];
                uint nodeCount = 0u;

                uint current = headPointerImage.Load(int2(panelPixel));
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

                float4 panelComposited = CompositeFrontToBack(litColor, sortedIndices, nodeCount,
                                                              panelTransparentColor, panelRevealage);
                panelCompositedColor = panelComposited.rgb;
            }

            float3 mappedCenter = ToneMapACES(panelCompositedColor);
            return float4(ToSwapchainLinear(mappedCenter), 1.0);
        }

        if (InRect(screenUV, albedoRect))
        {
            if (!InRect(screenUV, albedoInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }
            float2 panelUV = RectUV(screenUV, albedoInner);
            return float4(gAlbedoTexture.SampleLevel(gBufferSampler, panelUV, 0.0).rgb, 1.0);
        }

        if (InRect(screenUV, normalRect))
        {
            if (!InRect(screenUV, normalInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }
            float2 panelUV = RectUV(screenUV, normalInner);
            float4 panelNormal = gNormalTexture.SampleLevel(gBufferSampler, panelUV, 0.0);
            return float4(NormalVisual(panelNormal), 1.0);
        }

        if (InRect(screenUV, materialRect))
        {
            if (!InRect(screenUV, materialInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }
            float2 panelUV = RectUV(screenUV, materialInner);
            float4 panelMaterial = gMaterialTexture.SampleLevel(gBufferSampler, panelUV, 0.0);
            return float4(MaterialVisual(panelMaterial), 1.0);
        }

        if (InRect(screenUV, emissiveRect))
        {
            if (!InRect(screenUV, emissiveInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }
            float2 panelUV = RectUV(screenUV, emissiveInner);
            float3 panelEmissive = gEmissiveTexture.SampleLevel(gBufferSampler, panelUV, 0.0).rgb;
            float3 mappedEmissive = ToneMapACES(panelEmissive * 2.0);
            return float4(ToSwapchainLinear(mappedEmissive), 1.0);
        }

        if (InRect(screenUV, depthRect))
        {
            if (!InRect(screenUV, depthInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }
            float2 panelUV = RectUV(screenUV, depthInner);
            float panelDepth = gDepthTexture.SampleLevel(gBufferSampler, panelUV, 0.0);
            return float4(DepthVisual(panelDepth), 1.0);
        }

        return float4(overviewBackground, 1.0);
    }

    if (pc.outputMode == 10)
    {
        return float4(sceneColor.rgb, 1.0);
    }

    // Shadow cascade debug view: color-code pixels by cascade index.
    if (pc.outputMode == 11)
    {
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
            float blendZone = cascadeRange * 0.1;
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

    // Tile light heat map: color-code pixels by per-tile light count.
    if (pc.outputMode == 12)
    {
        static const uint TILE_SIZE = 16u;

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

    const float exposure = 0.25;
    float3 mapped = ToneMapACES(finalHdr * exposure);
    return float4(ToSwapchainLinear(mapped), 1.0);
}
