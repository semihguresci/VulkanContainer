struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

#include "object_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D materialTextures[];

#include "alpha_mask_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"

struct PushConstants
{
    uint objectIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] uint baseColorTextureIndex;
    [[vk::location(2)]] uint flags;
    [[vk::location(3)]] float alphaCutoff;
    [[vk::location(4)]] float baseColorAlpha;
    [[vk::location(5)]] uint opacityTextureIndex;
    [[vk::location(6)]] float opacityFactor;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID);
    ObjectBuffer obj = uObjects[objectIndex];
    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, input.inTexCoord,
        obj.heightTextureIndex, obj.heightScale, obj.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.texCoord = input.inTexCoord;
    output.baseColorTextureIndex = obj.baseColorTextureIndex;
    output.flags = obj.flags;
    output.alphaCutoff = obj.alphaCutoff;
    output.baseColorAlpha = obj.color.a;
    output.opacityTextureIndex = obj.opacityTextureIndex;
    output.opacityFactor = obj.opacityFactor;
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
    alpha = EvaluateOpacity(
        alpha, input.opacityTextureIndex, input.opacityFactor, input.texCoord);

    if ((input.flags & 0x1u) != 0u &&
        !AlphaMaskPass(alpha, input.alphaCutoff, input.pos))
    {
        // The depth prepass must apply the same alpha-test decision as shadow
        // and G-buffer passes, otherwise cutout materials can incorrectly hide
        // or reveal later lighting work.
        discard;
    }

    return 0.0.xxxx;
}
