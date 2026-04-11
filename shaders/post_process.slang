static const uint INVALID_NODE_INDEX = 0xffffffffu;
static const uint MAX_OIT_NODES_PER_PIXEL = 64u;

struct PushConstants
{
    uint outputMode;
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

[[vk::binding(0, 0)]] SamplerState gBufferSampler;
[[vk::binding(1, 0)]] Texture2D<float4> sceneColorTexture;
[[vk::binding(2, 0)]] Texture2D<float4> gAlbedoTexture;
[[vk::binding(3, 0)]] Texture2D<float4> gNormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> gMaterialTexture;
[[vk::binding(5, 0)]] Texture2D<float4> gEmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float4> gPositionTexture;
[[vk::binding(0, 1)]] Texture2D<uint> headPointerImage;
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

float3 PositionVisual(float4 position)
{
    if (position.w <= 0.0)
    {
        return 0.0.xxx;
    }
    float3 absPos = abs(position.xyz);
    return absPos / (1.0 + absPos);
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

    float4 sceneColor = sceneColorTexture.SampleLevel(gBufferSampler, screenUV, 0.0);
    float4 albedo = gAlbedoTexture.SampleLevel(gBufferSampler, screenUV, 0.0);
    float4 normal = gNormalTexture.SampleLevel(gBufferSampler, screenUV, 0.0);
    float4 material = gMaterialTexture.SampleLevel(gBufferSampler, screenUV, 0.0);
    float4 emissive = gEmissiveTexture.SampleLevel(gBufferSampler, screenUV, 0.0);
    float4 position = gPositionTexture.SampleLevel(gBufferSampler, screenUV, 0.0);

    float3 transparentColor = 0.0.xxx;
    float3 compositedColor = sceneColor.rgb;
    float revealage = 1.0;

    if (pixelCoord.x < uOit.viewportWidth && pixelCoord.y < uOit.viewportHeight)
    {
        uint sortedIndices[MAX_OIT_NODES_PER_PIXEL];
        uint nodeCount = 0u;

        uint current = headPointerImage.Load(int3(pixelCoord, 0));
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
        float depthVisual = 1.0 - exp(-position.w * 0.02);
        return float4(depthVisual.xxx, 1.0);
    }

    if (pc.outputMode == 5)
    {
        float3 mappedEmissive = ToneMapACES(emissive.rgb * 2.0);
        return float4(pow(mappedEmissive, 1.0 / 2.2), 1.0);
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
        Rect positionRect = { float2(0.82, 0.36), float2(0.98, 0.64) };

        const float panelPadding = 0.006;
        const float3 panelBorderColor = float3(0.60, 0.62, 0.66);
        const float3 panelTileBackgroundColor = float3(0.12, 0.12, 0.13);
        const float3 screenBackgroundColor = float3(0.02, 0.02, 0.025);

        if (IsPanelBorder(screenUV, centerRect, panelPadding) ||
            IsPanelBorder(screenUV, albedoRect, panelPadding) ||
            IsPanelBorder(screenUV, normalRect, panelPadding) ||
            IsPanelBorder(screenUV, materialRect, panelPadding) ||
            IsPanelBorder(screenUV, emissiveRect, panelPadding) ||
            IsPanelBorder(screenUV, positionRect, panelPadding))
        {
            return float4(panelBorderColor, 1.0);
        }

        Rect centerInner = InsetRect(centerRect, panelPadding);
        Rect albedoInner = InsetRect(albedoRect, panelPadding);
        Rect normalInner = InsetRect(normalRect, panelPadding);
        Rect materialInner = InsetRect(materialRect, panelPadding);
        Rect emissiveInner = InsetRect(emissiveRect, panelPadding);
        Rect positionInner = InsetRect(positionRect, panelPadding);

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

                uint current = headPointerImage.Load(int3(panelPixel, 0));
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
            mappedCenter = pow(mappedCenter, 1.0 / 2.2);
            return float4(mappedCenter, 1.0);
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
            return float4(pow(mappedEmissive, 1.0 / 2.2), 1.0);
        }

        if (InRect(screenUV, positionRect))
        {
            if (!InRect(screenUV, positionInner))
            {
                return float4(panelTileBackgroundColor, 1.0);
            }
            float2 panelUV = RectUV(screenUV, positionInner);
            float4 panelPosition = gPositionTexture.SampleLevel(gBufferSampler, panelUV, 0.0);
            return float4(PositionVisual(panelPosition), 1.0);
        }

        return float4(screenBackgroundColor, 1.0);
    }

    if (pc.outputMode == 10)
    {
        return float4(sceneColor.rgb, 1.0);
    }

    const float exposure = 0.25;
    float3 mapped = ToneMapACES(compositedColor * exposure);
    mapped = pow(mapped, 1.0 / 2.2);
    return float4(mapped, 1.0);
}
