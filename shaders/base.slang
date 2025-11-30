struct CameraBuffer
{
    float4x4 viewProj;
};

struct ObjectBuffer
{
    float4x4 model;
    float4 color;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] ConstantBuffer<ObjectBuffer> uObjects[];

[[vk::push_constant]]
struct PushConstants
{
    uint objectIndex;
};

PushConstants pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(1)]] float3 inColor;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 color;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    uint idx = NonUniformResourceIndex(pc.objectIndex);
    ObjectBuffer obj = uObjects[idx];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.color = input.inColor * obj.color.rgb;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_Target0
{
    return float4(vertIn.color, 1.0);
}
