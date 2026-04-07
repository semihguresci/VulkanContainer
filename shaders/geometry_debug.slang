struct CameraBuffer
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    float4x4 model;
    float4x4 normalMatrix;
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
};

struct VSOutput
{
    float4 pos : SV_Position;
    float pointSize : PSIZE;
    float3 color : COLOR0;
};

float3 hashColor(uint index)
{
    float scalar = float(index + 1u);
    float3 seed = float3(
        frac(sin(scalar * 12.9898) * 43758.5453),
        frac(sin(scalar * 78.2330) * 24634.6345),
        frac(sin(scalar * 39.4250) * 56445.2344));
    return 0.25.xxx + 0.75.xxx * seed;
}

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.pointSize = 4.0;
    output.color = hashColor(pc.objectIndex);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
