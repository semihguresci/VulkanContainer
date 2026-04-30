struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    column_major float4x4 model;
    float4 normalMatrix0;
    float4 normalMatrix1;
    float4 normalMatrix2;
    float4 color;
    float3 emissiveColor;
    float emissiveStrength;
    float2 metallicRoughness;
    float alphaCutoff;
    float normalTextureScale;
    float occlusionStrength;
    uint baseColorTextureIndex;
    uint normalTextureIndex;
    uint occlusionTextureIndex;
    uint emissiveTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint flags;
    float4 boundingSphere;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

#include "object_index_common.slang"

struct PushConstants
{
    uint objectIndex;
    float lineLength;
    float lineOffset;
};
[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(1)]] float3 inColor;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(4)]] float4 inTangent;
};

struct VSOutput
{
    float3 worldPosition : TEXCOORD0;
};

struct GSOutput
{
    float4 pos   : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID);
    ObjectBuffer obj = uObjects[objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.worldPosition = worldPos.xyz;
    return output;
}

void appendLine(inout LineStream<GSOutput> stream,
                float3 startWorldPosition,
                float3 endWorldPosition,
                float4 startColor,
                float4 endColor)
{
    GSOutput startVertex;
    startVertex.pos = mul(uCamera.viewProj, float4(startWorldPosition, 1.0));
    startVertex.color = startColor;
    stream.Append(startVertex);

    GSOutput endVertex;
    endVertex.pos = mul(uCamera.viewProj, float4(endWorldPosition, 1.0));
    endVertex.color = endColor;
    stream.Append(endVertex);
    stream.RestartStrip();
}

// Visualize the geometric face normal. This is the authoritative orientation
// signal for diagnosing winding/front-face issues.
[shader("geometry")]
[maxvertexcount(2)]
void geomMain(triangle VSOutput input[3], inout LineStream<GSOutput> stream)
{
    const float faceNormalLength = max(pc.lineLength, 0.001);
    const float normalOffset = max(pc.lineOffset, 0.0);

    static const float4 kFaceBaseColor = float4(0.15, 0.85, 1.0, 1.0);
    static const float4 kFaceTipColor  = float4(1.0, 0.92, 0.10, 1.0);

    const float3 p0 = input[0].worldPosition;
    const float3 p1 = input[1].worldPosition;
    const float3 p2 = input[2].worldPosition;

    const float3 edge0 = p1 - p0;
    const float3 edge1 = p2 - p0;
    const float3 faceNormalRaw = cross(edge0, edge1);
    if (!all(isfinite(faceNormalRaw)) || length(faceNormalRaw) < 1e-7)
    {
        return;
    }

    const float3 faceNormal = normalize(faceNormalRaw);

    const float3 centroid = (p0 + p1 + p2) / 3.0;
    float3 cameraOffsetDirection = uCamera.cameraWorldPosition.xyz - centroid;
    if (!all(isfinite(cameraOffsetDirection)) || length(cameraOffsetDirection) < 1e-6)
    {
        cameraOffsetDirection = faceNormal;
    }
    else
    {
        cameraOffsetDirection = normalize(cameraOffsetDirection);
    }
    const float3 lineStart = centroid + cameraOffsetDirection * normalOffset;

    appendLine(stream,
               lineStart,
               lineStart + faceNormal * faceNormalLength,
               kFaceBaseColor,
               kFaceTipColor);
}

[shader("fragment")]
float4 fragMain(GSOutput input) : SV_Target0
{
    return input.color;
}

