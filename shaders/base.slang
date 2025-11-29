struct VSInput
{
    [[vk::location(0)]] float2 position;
    [[vk::location(1)]] float3 color;
};

struct VSOutput
{
    [[vk::builtin("Position")]] float4 position;
    [[vk::location(0)]] float3 color;
};

struct MaterialConstants
{
    float4 baseColor;
};

[[vk::push_constant]] ConstantBuffer<MaterialConstants> gMaterial;

VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.color = input.color;
    return output;
}

float4 psMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0f) * gMaterial.baseColor;
}
