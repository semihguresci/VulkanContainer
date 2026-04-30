struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    // Must match container::gpu::ObjectData. Texture index 0xffffffff means
    // "not bound" so materials can share this struct without extra booleans.
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
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D materialTextures[];

#include "alpha_mask_common.slang"
#include "object_index_common.slang"
#include "surface_normal_common.slang"

struct PushConstants
{
    uint objectIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

static const uint kObjectFlagAlphaMask = 0x1u;
static const uint kObjectFlagTransparentOnly = 0x2u;
static const uint kObjectFlagDoubleSided = 0x4u;

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
    [[vk::location(12)]] float normalTextureScale;
    [[vk::location(13)]] float occlusionStrength;
    [[vk::location(14)]] uint flags;
    [[vk::location(15)]] float3 worldNormal;
    [[vk::location(16)]] float4 worldTangent;
};

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 material : SV_Target2;
    float4 emissive : SV_Target3;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID);
    ObjectBuffer obj = uObjects[objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    float3x3 modelMatrix = (float3x3)obj.model;
    float3 normalVector =
        obj.normalMatrix0.xyz * input.inNormal.x +
        obj.normalMatrix1.xyz * input.inNormal.y +
        obj.normalMatrix2.xyz * input.inNormal.z;
    float3 worldNormal = SafeNormalize(normalVector, float3(0.0, 1.0, 0.0));
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
    output.normalTextureScale = obj.normalTextureScale;
    output.occlusionStrength = obj.occlusionStrength;
    output.flags = obj.flags;
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    return output;
}

[shader("fragment")]
PSOutput fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace)
{
    if ((vertIn.flags & kObjectFlagTransparentOnly) != 0u)
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

    if ((vertIn.flags & kObjectFlagAlphaMask) != 0u &&
        !AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, vertIn.pos))
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

    float faceSign =
        ((vertIn.flags & kObjectFlagDoubleSided) != 0u && !isFrontFace)
            ? -1.0 : 1.0;
    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, vertIn.texCoord,
        vertIn.normalTextureIndex, vertIn.normalTextureScale, faceSign);
    const bool thinSurface =
        (vertIn.flags & (kObjectFlagAlphaMask | kObjectFlagDoubleSided)) ==
        (kObjectFlagAlphaMask | kObjectFlagDoubleSided);

    float occlusion = 1.0;
    if (vertIn.occlusionTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.occlusionTextureIndex);
        float sampledOcclusion =
            materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord).r;
        occlusion = lerp(1.0, sampledOcclusion, saturate(vertIn.occlusionStrength));
    }

    float3 emissive = vertIn.emissiveColor;
    if (vertIn.emissiveTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.emissiveTextureIndex);
        emissive *= materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord).rgb;
    }
    emissive *= vertIn.emissiveStrength;

    PSOutput output;
    output.albedo = float4(baseColor, 1.0);
    output.normal = float4(surfaceNormal.shadingNormal * 0.5 + 0.5, 1.0);
    output.material = float4(sampledMetallic, sampledRoughness, occlusion,
                             thinSurface ? 1.0 : 0.0);
    output.emissive = float4(emissive, 1.0);
    return output;
}
