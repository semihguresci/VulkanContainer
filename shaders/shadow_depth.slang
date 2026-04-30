// shadow_depth.slang - Depth-only shader for shadow map rendering.
// Transforms geometry by cascade viewProj * object model matrix and mirrors
// the depth prepass alpha-mask discard path so cutout materials cast cutout
// shadows instead of solid silhouettes.

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

#include "alpha_mask_common.slang"
#include "object_index_common.slang"

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

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID);
    ObjectBuffer obj = uObjects[objectIndex];
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
