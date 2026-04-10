struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    column_major float4x4 model;
    column_major float4x4 normalMatrix;
    float4 color;
    float3 emissiveColor;
    float emissiveStrength;
    float2 metallicRoughness;
    float alphaCutoff;
    uint baseColorTextureIndex;
    uint normalTextureIndex;
    uint occlusionTextureIndex;
    uint emissiveTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint flags;
    float2 padding;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D materialTextures[];

#include "surface_normal_common.slang"

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
    [[vk::location(0)]] float4 baseColor;
    [[vk::location(1)]] float2 texCoord;
    [[vk::location(2)]] uint baseColorTextureIndex;
    [[vk::location(3)]] uint normalTextureIndex;
    [[vk::location(4)]] uint occlusionTextureIndex;
    [[vk::location(5)]] uint emissiveTextureIndex;
    [[vk::location(6)]] uint metallicRoughnessTextureIndex;
    [[vk::location(7)]] float3 emissiveColor;
    [[vk::location(8)]] float emissiveStrength;
    [[vk::location(9)]] float2 metallicRoughness;
    [[vk::location(10)]] float3 worldPos;
    [[vk::location(11)]] float alphaCutoff;
    [[vk::location(12)]] uint flags;
    [[vk::location(13)]] float3 worldNormal;
    [[vk::location(14)]] float4 worldTangent;
};

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 material : SV_Target2;
    float4 emissive : SV_Target3;
    float4 position : SV_Target4;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    float3x3 modelMatrix = (float3x3)obj.model;
    float3 worldNormal = BuildWorldNormal((float3x3)obj.normalMatrix, input.inNormal);
    float4 worldTangent = BuildWorldTangent(modelMatrix, worldNormal, input.inTangent);

    output.pos = mul(uCamera.viewProj, worldPos);
    output.baseColor = obj.color;
    output.texCoord = input.inTexCoord;
    output.baseColorTextureIndex = obj.baseColorTextureIndex;
    output.normalTextureIndex = obj.normalTextureIndex;
    output.occlusionTextureIndex = obj.occlusionTextureIndex;
    output.emissiveTextureIndex = obj.emissiveTextureIndex;
    output.metallicRoughnessTextureIndex = obj.metallicRoughnessTextureIndex;
    output.emissiveColor = obj.emissiveColor;
    output.emissiveStrength = obj.emissiveStrength;
    output.metallicRoughness = obj.metallicRoughness;
    output.worldPos = worldPos.xyz;
    output.alphaCutoff = obj.alphaCutoff;
    output.flags = obj.flags;
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    return output;
}

[shader("fragment")]
PSOutput fragMain(VSOutput vertIn)
{
    if ((vertIn.flags & 0x2u) != 0u)
    {
        discard;
    }

    float3 baseColor = vertIn.baseColor.rgb;
    float baseAlpha = vertIn.baseColor.a;

    if (vertIn.baseColorTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.baseColorTextureIndex);
        float4 texColor = materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord);
        baseColor *= texColor.rgb;
        baseAlpha *= texColor.a;
    }

    if ((vertIn.flags & 0x1u) != 0u && baseAlpha < vertIn.alphaCutoff)
    {
        discard;
    }

    float sampledMetallic = vertIn.metallicRoughness.x;
    float sampledRoughness = vertIn.metallicRoughness.y;
    if (vertIn.metallicRoughnessTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.metallicRoughnessTextureIndex);
        float4 mrSample = materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord);
        sampledRoughness *= mrSample.g;
        sampledMetallic *= mrSample.b;
    }

    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, vertIn.texCoord,
        vertIn.normalTextureIndex);

    float occlusion = 1.0;
    if (vertIn.occlusionTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.occlusionTextureIndex);
        occlusion = materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord).r;
    }

    float3 emissive = vertIn.emissiveColor * vertIn.emissiveStrength;
    if (vertIn.emissiveTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.emissiveTextureIndex);
        emissive += materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord).rgb
                    * vertIn.emissiveStrength;
    }

    float linearDepth = length(vertIn.worldPos - uCamera.cameraWorldPosition.xyz);

    PSOutput output;
    output.albedo = float4(baseColor, baseAlpha);
    output.normal = float4(surfaceNormal.shadingNormal * 0.5 + 0.5, 1.0);
    output.material = float4(sampledMetallic, sampledRoughness, occlusion, baseAlpha);
    output.emissive = float4(emissive, 1.0);
    output.position = float4(vertIn.worldPos, linearDepth);
    return output;
}
