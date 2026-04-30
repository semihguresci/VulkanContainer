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
    [[vk::location(17)]] uint roughnessTextureIndex;
    [[vk::location(18)]] uint metalnessTextureIndex;
    [[vk::location(19)]] uint specularTextureIndex;
    [[vk::location(20)]] uint opacityTextureIndex;
    [[vk::location(21)]] float opacityFactor;
    [[vk::location(22)]] float specularFactor;
    [[vk::location(23)]] uint objectIndex;
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

    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, input.inTexCoord,
        obj.heightTextureIndex, obj.heightScale, obj.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
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
    output.roughnessTextureIndex = obj.roughnessTextureIndex;
    output.metalnessTextureIndex = obj.metalnessTextureIndex;
    output.specularTextureIndex = obj.specularTextureIndex;
    output.opacityTextureIndex = obj.opacityTextureIndex;
    output.opacityFactor = obj.opacityFactor;
    output.specularFactor = obj.specularFactor;
    output.objectIndex = objectIndex;
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
    baseAlpha = EvaluateOpacity(
        baseAlpha, vertIn.opacityTextureIndex, vertIn.opacityFactor,
        vertIn.texCoord);

    if ((vertIn.flags & kObjectFlagAlphaMask) != 0u &&
        !AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, vertIn.pos))
    {
        discard;
    }

    float sampledMetallic = EvaluateMetalness(
        vertIn.metallicRoughness.x, vertIn.metallicRoughnessTextureIndex,
        vertIn.metalnessTextureIndex, vertIn.texCoord);
    float sampledRoughness = EvaluateRoughness(
        vertIn.metallicRoughness.y, vertIn.metallicRoughnessTextureIndex,
        vertIn.roughnessTextureIndex, vertIn.texCoord, vertIn.flags);
    float specularFactor = EvaluateSpecularFactor(
        vertIn.specularFactor, vertIn.specularTextureIndex, vertIn.texCoord);
    ObjectBuffer obj = uObjects[vertIn.objectIndex];
    float3 specularColor = EvaluateSpecularColor(
        obj.specularColorFactor.rgb, obj.specularColorTextureIndex,
        vertIn.specularTextureIndex, vertIn.texCoord);
    float dielectricF0 = EvaluateDielectricF0(
        specularFactor, specularColor, obj.ior);

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

    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - vertIn.worldPos,
                             float3(0.0, 0.0, 1.0));
    float NdotV = saturate(dot(surfaceNormal.shadingNormal, V));

    float clearcoat = EvaluateClearcoatFactor(
        obj.clearcoatFactor, obj.clearcoatTextureIndex, vertIn.texCoord);
    if (clearcoat > 0.0)
    {
        float clearcoatRoughness = EvaluateClearcoatRoughness(
            obj.clearcoatRoughnessFactor, obj.clearcoatRoughnessTextureIndex,
            vertIn.texCoord);
        sampledRoughness =
            lerp(sampledRoughness, min(sampledRoughness, clearcoatRoughness),
                 clearcoat * 0.35);
        dielectricF0 = saturate(dielectricF0 + clearcoat * 0.01);
    }

    float3 sheenColor = EvaluateSheenColor(
        obj.sheenColorFactor.rgb, obj.sheenColorTextureIndex, vertIn.texCoord);
    float sheenWeight = MaxComponent(sheenColor);
    if (sheenWeight > 0.0)
    {
        float sheenRoughness = EvaluateSheenRoughness(
            obj.sheenRoughnessFactor, obj.sheenRoughnessTextureIndex,
            vertIn.texCoord);
        float grazing = pow(1.0 - NdotV, 2.0) * (1.0 - 0.35 * sheenRoughness);
        emissive += sheenColor * grazing * 0.08;
        dielectricF0 = saturate(dielectricF0 + sheenWeight * grazing * 0.004);
    }

    float iridescence = EvaluateIridescenceFactor(
        obj.iridescenceFactor, obj.iridescenceTextureIndex, vertIn.texCoord);
    if (iridescence > 0.0)
    {
        float thickness = EvaluateIridescenceThickness(
            obj.iridescenceThicknessMinimum, obj.iridescenceThicknessMaximum,
            obj.iridescenceThicknessTextureIndex, vertIn.texCoord);
        baseColor *= ApproximateIridescenceTint(
            iridescence * 0.25, obj.iridescenceIor, thickness, NdotV);
        dielectricF0 = saturate(dielectricF0 + iridescence * 0.006);
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) != 0u)
    {
        PSOutput unlitOutput;
        unlitOutput.albedo = float4(0.0, 0.0, 0.0, baseAlpha);
        unlitOutput.normal =
            float4(surfaceNormal.shadingNormal * 0.5 + 0.5, 1.0);
        unlitOutput.material = float4(0.0, 1.0, 1.0, thinSurface ? 1.0 : 0.0);
        unlitOutput.emissive = float4(baseColor + emissive, 0.0);
        return unlitOutput;
    }

    PSOutput output;
    output.albedo = float4(baseColor, baseAlpha);
    output.normal = float4(surfaceNormal.shadingNormal * 0.5 + 0.5, 1.0);
    output.material = float4(sampledMetallic, sampledRoughness, occlusion,
                             thinSurface ? 1.0 : 0.0);
    output.emissive = float4(emissive, dielectricF0);
    return output;
}
