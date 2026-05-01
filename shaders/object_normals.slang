#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

#include "object_index_common.slang"
[[vk::binding(2, 0)]] SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];

#include "alpha_mask_common.slang"
#include "pbr_material_common.slang"

[[vk::push_constant]]
ConstantBuffer<BindlessPushConstants> pc;

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
    [[vk::location(6)]] uint opacityTextureIndex;
    [[vk::location(7)]] float opacityFactor;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID,
                  uint startInstance : SV_StartInstanceLocation)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID, startInstance);
    ObjectBuffer obj = uObjects[objectIndex];
    GpuMaterial material = uMaterials[obj.objectInfo.x];

    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, input.inTexCoord,
        material.heightTextureIndex, material.heightScale, material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.objectNormal = normalize(input.inNormal);
    output.texCoord = input.inTexCoord;
    output.baseColorTextureIndex = material.baseColorTextureIndex;
    output.flags = material.flags | obj.objectInfo.y;
    output.alphaCutoff = material.alphaCutoff;
    output.baseColorAlpha = material.color.a;
    output.opacityTextureIndex = material.opacityTextureIndex;
    output.opacityFactor = material.opacityFactor;
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
        alpha *= SamplePbrTexture(
            input.baseColorTextureIndex, input.texCoord, 1.0.xxxx).a;
    }
    alpha = EvaluateOpacity(
        alpha, input.opacityTextureIndex, input.opacityFactor, input.texCoord);

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
