struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

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

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

#include "object_index_common.slang"
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D materialTextures[];

#include "alpha_mask_common.slang"

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
    [[vk::location(0)]] float3 objectNormal;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] uint baseColorTextureIndex;
    [[vk::location(3)]] uint flags;
    [[vk::location(4)]] float alphaCutoff;
    [[vk::location(5)]] float baseColorAlpha;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID);
    ObjectBuffer obj = uObjects[objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.objectNormal = normalize(input.inNormal);
    output.texCoord = input.inTexCoord;
    output.baseColorTextureIndex = obj.baseColorTextureIndex;
    output.flags = obj.flags;
    output.alphaCutoff = obj.alphaCutoff;
    output.baseColorAlpha = obj.color.a;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    if ((input.flags & 0x2u) != 0u)
    {
        discard;
    }

    float alpha = input.baseColorAlpha;
    if (input.baseColorTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(input.baseColorTextureIndex);
        alpha *= materialTextures[texIndex].Sample(baseSampler, input.texCoord).a;
    }

    if ((input.flags & 0x1u) != 0u &&
        !AlphaMaskPass(alpha, input.alphaCutoff, input.pos))
    {
        discard;
    }

    float3 objectNormal = normalize(input.objectNormal);
    if (!all(isfinite(objectNormal)))
    {
        objectNormal = float3(0.0, 1.0, 0.0);
    }

    return float4(saturate(objectNormal * 0.5 + 0.5), 1.0);
}
