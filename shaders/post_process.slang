static const uint INVALID_NODE_INDEX = 0xffffffffu;

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
[[vk::binding(0, 1)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 1)]] RWStructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 1)]] RWStructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 1)]] ConstantBuffer<OitMetadataBuffer> uOit;

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
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

void InsertNodeSorted(uint nodeIndex, inout uint sortedHead)
{
    TransparentNode node = transparentNodes[nodeIndex];
    if (sortedHead == INVALID_NODE_INDEX)
    {
        node.next = INVALID_NODE_INDEX;
        transparentNodes[nodeIndex] = node;
        sortedHead = nodeIndex;
        return;
    }

    TransparentNode sortedHeadNode = transparentNodes[sortedHead];
    if (node.depth > sortedHeadNode.depth)
    {
        node.next = sortedHead;
        transparentNodes[nodeIndex] = node;
        sortedHead = nodeIndex;
        return;
    }

    uint currentIndex = sortedHead;
    while (currentIndex != INVALID_NODE_INDEX)
    {
        TransparentNode currentNode = transparentNodes[currentIndex];
        uint nextIndex = currentNode.next;
        if (nextIndex == INVALID_NODE_INDEX)
        {
            node.next = INVALID_NODE_INDEX;
            transparentNodes[nodeIndex] = node;
            currentNode.next = nodeIndex;
            transparentNodes[currentIndex] = currentNode;
            return;
        }

        TransparentNode nextNode = transparentNodes[nextIndex];
        if (node.depth > nextNode.depth)
        {
            node.next = nextIndex;
            transparentNodes[nodeIndex] = node;
            currentNode.next = nodeIndex;
            transparentNodes[currentIndex] = currentNode;
            return;
        }

        currentIndex = nextIndex;
    }
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
    float4 sceneColor = sceneColorTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float4 albedo = gAlbedoTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float4 normal = gNormalTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float4 material = gMaterialTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float4 emissive = gEmissiveTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);
    float4 position = gPositionTexture.SampleLevel(gBufferSampler, input.texCoord, 0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    float3 transparentColor = 0.0.xxx;
    float3 compositedColor = sceneColor.rgb;
    float revealage = 1.0;

    if (pixelCoord.x < uOit.viewportWidth && pixelCoord.y < uOit.viewportHeight)
    {
        uint sortedHead = INVALID_NODE_INDEX;
        uint current = headPointerImage[pixelCoord];
        while (current != INVALID_NODE_INDEX)
        {
            TransparentNode node = transparentNodes[current];
            uint next = node.next;
            node.next = INVALID_NODE_INDEX;
            transparentNodes[current] = node;
            InsertNodeSorted(current, sortedHead);
            current = next;
        }

        current = sortedHead;
        while (current != INVALID_NODE_INDEX)
        {
            TransparentNode node = transparentNodes[current];
            compositedColor = lerp(compositedColor, node.color.rgb, node.color.a);
            transparentColor = lerp(transparentColor, node.color.rgb, node.color.a);
            revealage *= (1.0 - node.color.a);
            current = node.next;
        }
    }

    if (pc.outputMode == 1)
    {
        return float4(albedo.rgb, 1.0);
    }

    if (pc.outputMode == 2)
    {
        return float4(normal.xyz, 1.0);
    }

    if (pc.outputMode == 3)
    {
        return float4(material.xyz, 1.0);
    }

    if (pc.outputMode == 4)
    {
        float depthVisual = 1.0 - exp(-position.w * 0.02);
        return float4(depthVisual.xxx, 1.0);
    }

    if (pc.outputMode == 5)
    {
        return float4(emissive.rgb, 1.0);
    }

    if (pc.outputMode == 6)
    {
        return float4(transparentColor, 1.0);
    }

    if (pc.outputMode == 7)
    {
        return float4(revealage.xxx, 1.0);
    }

    float3 mapped = ToneMapACES(compositedColor);
    mapped = pow(mapped, 1.0 / 2.2);
    return float4(mapped, 1.0);
}
