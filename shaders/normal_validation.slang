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
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos;
};

struct GSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] nointerpolation float3 faceColor;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];
    float4 world = mul(obj.model, float4(input.inPosition, 1.0));
    output.worldPos = world.xyz;
    output.pos = mul(uCamera.viewProj, world);
    return output;
}

[shader("geometry")]
[maxvertexcount(3)]
void geomMain(triangle VSOutput input[3], inout TriangleStream<GSOutput> stream)
{
    const float3 p0 = input[0].worldPos;
    const float3 p1 = input[1].worldPos;
    const float3 p2 = input[2].worldPos;

    const float3 faceCross = cross(p1 - p0, p2 - p0);
    const float lenSq = dot(faceCross, faceCross);

    float3 baseColor = float3(1.0, 0.0, 0.0);
    if (lenSq >= 1e-10)
    {
        const float3 faceNormal = normalize(faceCross);
        const float3 centroid = (p0 + p1 + p2) / 3.0;
        const float3 viewDir = normalize(uCamera.cameraWorldPosition.xyz - centroid);
        const bool correct = dot(faceNormal, viewDir) > 0.0;
        baseColor = correct ? float3(0.0, 0.2, 1.0) : float3(1.0, 0.0, 0.0);
    }

    GSOutput out0;
    out0.pos = input[0].pos;
    out0.faceColor = baseColor;
    stream.Append(out0);

    GSOutput out1;
    out1.pos = input[1].pos;
    out1.faceColor = baseColor;
    stream.Append(out1);

    GSOutput out2;
    out2.pos = input[2].pos;
    out2.faceColor = baseColor;
    stream.Append(out2);

    stream.RestartStrip();
}

[shader("fragment")]
float4 fragMain(GSOutput input) : SV_Target0
{
    return float4(input.faceColor, 1.0);
}


