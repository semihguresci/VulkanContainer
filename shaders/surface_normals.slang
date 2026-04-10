struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    column_major float4x4 model;
    column_major float4x4 normalMatrix;
    float4 color;
    float3 emissiveColor;
    float emissiveStrength;
    float2 metallicRoughness;
    float alphaCutoff;
    uint baseColorTextureIndex;
    uint normalTextureIndex;
    uint occlusionTextureIndex;
    uint emissiveTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint flags;
    float2 padding;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

struct PushConstants
{
    uint objectIndex;
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
    float3 worldNormal   : TEXCOORD1;
};

struct GSOutput
{
    float4 pos   : SV_Position;
    float4 color : COLOR0;
};

float3 safeNormalize(float3 value, float3 fallbackValue)
{
    if (!all(isfinite(value)) || length(value) < 1e-6)
    {
        return normalize(fallbackValue);
    }
    return normalize(value);
}

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    float3 worldNormal = mul((float3x3)obj.normalMatrix, input.inNormal);

    output.worldPosition = worldPos.xyz;
    output.worldNormal = safeNormalize(worldNormal, float3(0.0, 1.0, 0.0));
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

// Visualize actual triangle winding first, then compare the loaded/interpolated
// normal against it. The face normal is the authoritative orientation signal.
[shader("geometry")]
[maxvertexcount(4)]
void geomMain(triangle VSOutput input[3], inout LineStream<GSOutput> stream)
{
    static const float kFaceNormalLength = 0.16;
    static const float kVertexNormalLength = 0.10;
    static const float kNormalOffset = 0.002;

    static const float4 kFaceBaseColor = float4(0.15, 0.85, 1.0, 1.0);
    static const float4 kFaceTipColor  = float4(1.0, 0.92, 0.10, 1.0);
    static const float4 kVertexBaseColor = float4(1.0, 0.25, 0.75, 0.9);
    static const float4 kVertexTipColor  = float4(0.95, 0.95, 0.95, 0.9);

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
    const float3 averagedVertexNormal = safeNormalize(
        input[0].worldNormal + input[1].worldNormal + input[2].worldNormal,
        faceNormal);

    const float3 centroid = (p0 + p1 + p2) / 3.0;
    const float3 cameraOffsetDirection = safeNormalize(
        uCamera.cameraWorldPosition.xyz - centroid, faceNormal);
    const float3 lineStart = centroid + cameraOffsetDirection * kNormalOffset;

    appendLine(stream,
               lineStart,
               lineStart + faceNormal * kFaceNormalLength,
               kFaceBaseColor,
               kFaceTipColor);

    appendLine(stream,
               lineStart,
               lineStart + averagedVertexNormal * kVertexNormalLength,
               kVertexBaseColor,
               kVertexTipColor);
}

[shader("fragment")]
float4 fragMain(GSOutput input) : SV_Target0
{
    return input.color;
}
