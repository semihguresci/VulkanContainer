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
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 baseColor;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    uint idx = NonUniformResourceIndex(pc.objectIndex);
    ObjectBuffer obj = uObjects[idx];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    // Prefer the material's base color; vertex color data is ignored for now so
    // glTF textures and factors can drive the appearance instead of per-vertex
    // attributes.
    output.baseColor = obj.color.rgb;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_Target0
{
    return float4(vertIn.baseColor, 1.0);
}
