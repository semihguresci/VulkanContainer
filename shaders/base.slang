struct CameraBuffer
{
    float4x4 viewProj;
};

struct ObjectBuffer
{
    float4x4 model;
    float4 color;
    uint textureIndex;
    float3 padding;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D baseColorTextures[];

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
    [[vk::location(0)]] float3 baseColor;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] uint textureIndex;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.baseColor = obj.color.rgb;
    output.texCoord = input.inTexCoord;
    output.textureIndex = obj.textureIndex;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_Target0
{
    if (vertIn.textureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.textureIndex);
        float4 texColor = baseColorTextures[texIndex].Sample(baseSampler, vertIn.texCoord);
        return float4(texColor.rgb * vertIn.baseColor, texColor.a);
    }

    return float4(vertIn.baseColor, 1.0);
}
