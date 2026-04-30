#include "brdf_common.slang"
#include "lighting_structs.slang"

static const uint INVALID_NODE_INDEX = 0xffffffffu;

#include "object_data_common.slang"

struct TransparentNode
{
    float4 color;
    float depth;
    uint next;
    float2 padding;
};

struct OitMetadataBuffer
{
    uint nodeCapacity;
    uint viewportWidth;
    uint viewportHeight;
    uint reserved;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState baseSampler;
[[vk::binding(3, 0)]] Texture2D materialTextures[];
[[vk::binding(0, 1)]] ConstantBuffer<LightingBuffer> uLighting;
[[vk::binding(1, 1)]] StructuredBuffer<PointLightData> uPointLights;
[[vk::binding(0, 2)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 2)]] RWStructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 2)]] RWStructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 2)]] ConstantBuffer<OitMetadataBuffer> uOit;
[[vk::binding(10, 3)]] TextureCube<float4> gIrradianceCubemap;
[[vk::binding(11, 3)]] TextureCube<float4> gPrefilteredCubemap;
[[vk::binding(12, 3)]] Texture2D<float2> gBrdfLUT;
[[vk::binding(13, 3)]] SamplerState envSampler;
[[vk::binding(14, 3)]] SamplerState brdfSampler;

#include "object_index_common.slang"
#include "pbr_material_common.slang"
#include "surface_normal_common.slang"

struct PushConstants
{
    uint objectIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

static const uint kObjectFlagAlphaCutoff = 0x1u;
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
    [[vk::location(21)]] uint transmissionTextureIndex;
    [[vk::location(22)]] float opacityFactor;
    [[vk::location(23)]] float specularFactor;
    [[vk::location(24)]] float transmissionFactor;
    [[vk::location(25)]] uint objectIndex;
};

float3 EvaluateThinSurfaceTransmission(float3 albedo, float3 normal, float3 V,
                                       float3 L, float3 radiance,
                                       float alpha, bool isBackFace)
{
    float wrappedDiffuse = saturate((dot(normal, L) + 0.45) / 1.45);
    float backLight = saturate(dot(-normal, L));
    float viewThrough = pow(saturate(dot(-L, V)), 2.0);
    float faceWeight = isBackFace ? 1.0 : 0.45;
    float opacityWeight = saturate(1.0 - alpha * 0.5);
    float transmission = max(wrappedDiffuse * 0.35, backLight * viewThrough) *
                         faceWeight * opacityWeight;
    return albedo * radiance * transmission;
}

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
    output.transmissionTextureIndex = obj.transmissionTextureIndex;
    output.opacityFactor = obj.opacityFactor;
    output.specularFactor = obj.specularFactor;
    output.transmissionFactor = obj.transmissionFactor;
    output.objectIndex = objectIndex;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
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

    if ((vertIn.flags & kObjectFlagAlphaCutoff) != 0u && baseAlpha < vertIn.alphaCutoff)
    {
        discard;
    }

    if (baseAlpha <= 0.01)
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
    float transmissionFactor = EvaluateTransmissionFactor(
        vertIn.transmissionFactor, vertIn.transmissionTextureIndex, vertIn.texCoord);
    ObjectBuffer obj = uObjects[vertIn.objectIndex];
    float3 specularColor = EvaluateSpecularColor(
        obj.specularColorFactor.rgb, obj.specularColorTextureIndex,
        vertIn.specularTextureIndex, vertIn.texCoord);
    float clearcoat = EvaluateClearcoatFactor(
        obj.clearcoatFactor, obj.clearcoatTextureIndex, vertIn.texCoord);
    float clearcoatRoughness = EvaluateClearcoatRoughness(
        obj.clearcoatRoughnessFactor, obj.clearcoatRoughnessTextureIndex,
        vertIn.texCoord);
    float3 sheenColor = EvaluateSheenColor(
        obj.sheenColorFactor.rgb, obj.sheenColorTextureIndex, vertIn.texCoord);
    float sheenRoughness = EvaluateSheenRoughness(
        obj.sheenRoughnessFactor, obj.sheenRoughnessTextureIndex,
        vertIn.texCoord);
    float iridescence = EvaluateIridescenceFactor(
        obj.iridescenceFactor, obj.iridescenceTextureIndex, vertIn.texCoord);
    float iridescenceThickness = EvaluateIridescenceThickness(
        obj.iridescenceThicknessMinimum, obj.iridescenceThicknessMaximum,
        obj.iridescenceThicknessTextureIndex, vertIn.texCoord);
    float volumeThickness = EvaluateThickness(
        obj.thicknessFactor, obj.thicknessTextureIndex, vertIn.texCoord);

    float faceSign =
        ((vertIn.flags & kObjectFlagDoubleSided) != 0u && !isFrontFace)
            ? -1.0 : 1.0;
    SurfaceNormalState surfaceNormal = EvaluateSurfaceNormal(
        vertIn.worldNormal, vertIn.worldTangent, vertIn.texCoord,
        vertIn.normalTextureIndex, vertIn.normalTextureScale, faceSign);

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

    float metallic = sampledMetallic;
    float roughness = sampledRoughness;
    float3 V = SafeNormalize(uCamera.cameraWorldPosition.xyz - vertIn.worldPos,
                             float3(0.0, 0.0, 1.0));
    float NdotV = max(dot(surfaceNormal.shadingNormal, V), 0.0);
    float3 iridescenceTint = ApproximateIridescenceTint(
        iridescence, obj.iridescenceIor, iridescenceThickness, NdotV);
    float3 F0 = EvaluatePbrF0(
        baseColor, metallic, specularFactor, specularColor * iridescenceTint,
        obj.ior);

    float3 F_ibl = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
    float3 irradiance = gIrradianceCubemap.Sample(envSampler, surfaceNormal.shadingNormal).rgb;
    float3 diffuseIBL = irradiance * baseColor;
    float3 R = reflect(-V, surfaceNormal.shadingNormal);
    float maxMipLevel = max(float(uLighting.prefilteredMipCount) - 1.0, 0.0);
    float3 prefilteredColor = gPrefilteredCubemap.SampleLevel(envSampler, R, roughness * maxMipLevel).rgb;
    float2 brdf = gBrdfLUT.Sample(brdfSampler, float2(NdotV, roughness));
    float3 specularIBL =
        EvaluateSpecularIblMultipleScattering(prefilteredColor, F0, brdf);

    float diffuseAO = occlusion;
    float specularOcclusion = saturate(pow(NdotV + diffuseAO, exp2(-16.0 * roughness - 1.0)) - 1.0 + diffuseAO);
    specularOcclusion = lerp(1.0, specularOcclusion, saturate(1.0 - roughness));
    float3 lighting = kD_ibl * diffuseIBL * diffuseAO + specularIBL * specularOcclusion;

    if ((vertIn.flags & kPbrObjectFlagUnlit) != 0u)
    {
        lighting = baseColor;
        baseAlpha *= lerp(1.0, 0.35, transmissionFactor);
    }

    float3 directionalL = SafeNormalize(-uLighting.directionalDirection.xyz,
                                        float3(0.0, 1.0, 0.0));
    float3 directionalRadiance =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;
    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        lighting += EvaluatePbrDirectLight(
            baseColor, F0, metallic, roughness, surfaceNormal.shadingNormal, V,
            directionalL, directionalRadiance);
    }
    const bool thinSurface =
        (vertIn.flags & (kObjectFlagAlphaCutoff | kObjectFlagDoubleSided)) ==
        (kObjectFlagAlphaCutoff | kObjectFlagDoubleSided);
    if (thinSurface)
    {
        if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
        {
            lighting += EvaluateThinSurfaceTransmission(
                baseColor, surfaceNormal.shadingNormal, V, directionalL,
                directionalRadiance, saturate(baseAlpha), !isFrontFace);
        }
    }

    for (uint i = 0; i < uLighting.pointLightCount && i < MAX_POINT_LIGHTS; ++i)
    {
        PointLightData light = uPointLights[i];
        float3 lightVector = light.positionRadius.xyz - vertIn.worldPos;
        float lightRadius = max(light.positionRadius.w, 1e-3);
        float distanceSq = dot(lightVector, lightVector);
        if (distanceSq >= lightRadius * lightRadius)
        {
            continue;
        }

        float invDistance = rsqrt(max(distanceSq, 1e-8));
        float3 L = lightVector * invDistance;
        float attenuation = SmoothRangeAttenuation(distanceSq, lightRadius);
        float3 radiance =
            light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
        {
            lighting += EvaluatePbrDirectLight(
                baseColor, F0, metallic, roughness, surfaceNormal.shadingNormal,
                V, L, radiance);
            if (thinSurface)
            {
                lighting += EvaluateThinSurfaceTransmission(
                    baseColor, surfaceNormal.shadingNormal, V, L, radiance,
                    saturate(baseAlpha), !isFrontFace);
            }
        }
    }

    if ((vertIn.flags & kPbrObjectFlagUnlit) == 0u)
    {
        if (clearcoat > 0.0)
        {
            float3 clearcoatNormal = surfaceNormal.shadingNormal;
            if (obj.clearcoatNormalTextureIndex != kInvalidMaterialTexture)
            {
                clearcoatNormal = EvaluateSurfaceNormal(
                    vertIn.worldNormal, vertIn.worldTangent, vertIn.texCoord,
                    obj.clearcoatNormalTextureIndex,
                    obj.clearcoatNormalTextureScale, faceSign).shadingNormal;
            }
            float clearcoatNdotV = max(dot(clearcoatNormal, V), 0.0);
            float3 clearcoatF0 = IorToF0(1.5).xxx;
            float3 clearcoatR = reflect(-V, clearcoatNormal);
            float3 clearcoatPrefiltered = gPrefilteredCubemap.SampleLevel(
                envSampler, clearcoatR, clearcoatRoughness * maxMipLevel).rgb;
            float2 clearcoatBrdf =
                gBrdfLUT.Sample(
                    brdfSampler, float2(clearcoatNdotV, clearcoatRoughness));
            float3 clearcoatIbl =
                EvaluateSpecularIblMultipleScattering(
                    clearcoatPrefiltered, clearcoatF0, clearcoatBrdf);
            lighting = lighting * (1.0 - clearcoat * 0.04) +
                       clearcoatIbl * clearcoat;
            lighting += EvaluatePbrDirectLight(
                            0.0.xxx, clearcoatF0, 1.0, clearcoatRoughness,
                            clearcoatNormal, V, directionalL,
                            directionalRadiance) *
                        clearcoat;
        }

        float sheenWeight = MaxComponent(sheenColor);
        if (sheenWeight > 0.0)
        {
            float grazing = pow(1.0 - NdotV, 2.0);
            float sheenScale = grazing * (1.0 - 0.35 * sheenRoughness);
            lighting = lighting * (1.0 - saturate(sheenWeight * 0.25)) +
                       sheenColor * (irradiance + directionalRadiance) *
                           sheenScale * 0.35;
        }
    }

    lighting += emissive;

    float transmissionWeight = saturate(transmissionFactor * (1.0 - metallic));
    if (transmissionWeight > 0.0)
    {
        float3 transmittedColor = 0.0.xxx;
        if (volumeThickness > 0.0)
        {
            float safeIor = max(obj.ior, 1.0);
            float halfSpread = (safeIor - 1.0) * 0.025 * max(obj.dispersion, 0.0);
            float3 iors = max(float3(safeIor - halfSpread, safeIor,
                                     safeIor + halfSpread),
                              1.0.xxx);
            float etaR = isFrontFace ? (1.0 / iors.r) : iors.r;
            float etaG = isFrontFace ? (1.0 / iors.g) : iors.g;
            float etaB = isFrontFace ? (1.0 / iors.b) : iors.b;
            float3 refrR = refract(-V, surfaceNormal.shadingNormal, etaR);
            float3 refrG = refract(-V, surfaceNormal.shadingNormal, etaG);
            float3 refrB = refract(-V, surfaceNormal.shadingNormal, etaB);
            if (dot(refrG, refrG) > 1e-6 && all(isfinite(refrG)))
            {
                transmittedColor.r = gPrefilteredCubemap.SampleLevel(
                    envSampler, refrR, roughness * maxMipLevel).r;
                transmittedColor.g = gPrefilteredCubemap.SampleLevel(
                    envSampler, refrG, roughness * maxMipLevel).g;
                transmittedColor.b = gPrefilteredCubemap.SampleLevel(
                    envSampler, refrB, roughness * maxMipLevel).b;
            }
        }
        else
        {
            transmittedColor = gPrefilteredCubemap.SampleLevel(
                envSampler, -V, roughness * maxMipLevel).rgb;
        }

        if (dot(transmittedColor, transmittedColor) > 0.0)
        {
            transmittedColor = ApplyBeerLambert(
                transmittedColor * baseColor, obj.attenuationColor.rgb,
                obj.attenuationDistance, volumeThickness);
            lighting = lerp(lighting, transmittedColor + emissive,
                            transmissionWeight * 0.75);
        }
        baseAlpha *= lerp(1.0, 0.35, transmissionWeight);
    }

    uint2 pixelCoord = uint2(vertIn.pos.xy);
    if (pixelCoord.x >= uOit.viewportWidth || pixelCoord.y >= uOit.viewportHeight)
    {
        return 0.0.xxxx;
    }

    uint nodeIndex = 0u;
    InterlockedAdd(nodeCounter[0], 1u, nodeIndex);
    if (nodeIndex >= uOit.nodeCapacity)
    {
        return 0.0.xxxx;
    }

    TransparentNode node;
    node.color = float4(lighting, saturate(baseAlpha));
    node.depth = saturate(vertIn.pos.z);
    node.next = INVALID_NODE_INDEX;
    node.padding = 0.0.xx;

    uint previousHead = INVALID_NODE_INDEX;
    InterlockedExchange(headPointerImage[pixelCoord], nodeIndex, previousHead);
    node.next = previousHead;
    transparentNodes[nodeIndex] = node;

    return 0.0.xxxx;
}
