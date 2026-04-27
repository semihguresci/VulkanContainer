// shadow_depth.slang - Depth-only shader for shadow map rendering.
// Transforms geometry by cascade viewProj * object model matrix and mirrors
// the depth prepass alpha-mask discard path so cutout materials cast cutout
// shadows instead of solid silhouettes.

struct ObjectBuffer
{
    column_major float4x4 model;
    column_major float4x4 normalMatrix;
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

struct ShadowCascadeData
{
    column_major float4x4 viewProj;
    float splitDepth;
    float3 padding;
};

static const uint SHADOW_CASCADE_COUNT = 4u;

struct ShadowBuffer
{
    ShadowCascadeData cascades[SHADOW_CASCADE_COUNT];
};

[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D materialTextures[];
[[vk::binding(0, 1)]] ConstantBuffer<ShadowBuffer> uShadow;

struct PushConstants
{
    uint objectIndex;
    uint cascadeIndex;
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
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] uint baseColorTextureIndex;
    [[vk::location(2)]] uint flags;
    [[vk::location(3)]] float alphaCutoff;
    [[vk::location(4)]] float baseColorAlpha;
};

static const uint kObjectFlagAlphaMask = 0x1u;
static const uint kObjectFlagTransparentOnly = 0x2u;

float Bayer4x4Threshold(uint2 pixelCoord)
{
    uint index = (pixelCoord.x & 3u) | ((pixelCoord.y & 3u) << 2u);
    uint bayer = 0u;
    switch (index)
    {
    case 0u:  bayer = 0u;  break;
    case 1u:  bayer = 8u;  break;
    case 2u:  bayer = 2u;  break;
    case 3u:  bayer = 10u; break;
    case 4u:  bayer = 12u; break;
    case 5u:  bayer = 4u;  break;
    case 6u:  bayer = 14u; break;
    case 7u:  bayer = 6u;  break;
    case 8u:  bayer = 3u;  break;
    case 9u:  bayer = 11u; break;
    case 10u: bayer = 1u;  break;
    case 11u: bayer = 9u;  break;
    case 12u: bayer = 15u; break;
    case 13u: bayer = 7u;  break;
    case 14u: bayer = 13u; break;
    default:  bayer = 5u;  break;
    }
    return (float(bayer) + 0.5) / 16.0;
}

bool AlphaMaskPass(float alpha, float cutoff, float4 pixelPosition)
{
    float alphaWidth = max(abs(ddx(alpha)) + abs(ddy(alpha)), 1e-4);
    float coverage = saturate((alpha - cutoff) / alphaWidth + 0.5);
    if (coverage <= 0.0) return false;
    if (coverage >= 1.0) return true;
    return coverage >= Bayer4x4Threshold(uint2(pixelPosition.xy));
}

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[instanceID];
    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uShadow.cascades[pc.cascadeIndex].viewProj, worldPos);
    output.texCoord = input.inTexCoord;
    output.baseColorTextureIndex = obj.baseColorTextureIndex;
    output.flags = obj.flags;
    output.alphaCutoff = obj.alphaCutoff;
    output.baseColorAlpha = obj.color.a;
    return output;
}

[shader("fragment")]
void fragMain(VSOutput input)
{
    if ((input.flags & kObjectFlagTransparentOnly) != 0u)
    {
        discard;
    }

    float alpha = input.baseColorAlpha;
    if (input.baseColorTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(input.baseColorTextureIndex);
        alpha *= materialTextures[texIndex].Sample(baseSampler, input.texCoord).a;
    }

    if ((input.flags & kObjectFlagAlphaMask) != 0u &&
        !AlphaMaskPass(alpha, input.alphaCutoff, input.pos))
    {
        discard;
    }
}
