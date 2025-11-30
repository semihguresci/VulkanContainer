struct VSInput
{
    [[location(0)]] float2 position : POSITION0;
    [[location(1)]] float3 color : COLOR0;
};

struct VSOutput
{
    [[location(0)]] float3 color;
    float4 position : SV_Position;
};

struct MaterialConstants
{
    float4 baseColor;
};

[[vk::push_constant]]
MaterialConstants gMaterial;

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