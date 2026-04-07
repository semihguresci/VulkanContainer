static const float PI = 3.14159265359;
static const uint MAX_POINT_LIGHTS = 4u;
static const uint INVALID_NODE_INDEX = 0xffffffffu;

struct CameraBuffer
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    float4x4 model;
    float4x4 normalMatrix;
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

struct PointLightData
{
    float4 positionRadius;
    float4 colorIntensity;
};

struct LightingBuffer
{
    float4 directionalDirection;
    float4 directionalColorIntensity;
    uint pointLightCount;
    float3 padding;
    PointLightData pointLights[MAX_POINT_LIGHTS];
};

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
[[vk::binding(0, 2)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 2)]] RWStructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 2)]] RWStructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 2)]] ConstantBuffer<OitMetadataBuffer> uOit;

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

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-4);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(N, V)), roughness) *
           GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float3 EvaluateLight(float3 albedo, float3 normal, float3 V, float3 L,
                     float3 radiance, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float3 F0 = lerp(0.04.xxx, albedo, metallic);
    float NDF = DistributionGGX(normal, H, roughness);
    float G = GeometrySmith(normal, V, L, roughness);
    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float3 numerator = NDF * G * F;
    float denominator =
        max(4.0 * saturate(dot(normal, V)) * saturate(dot(normal, L)), 1e-4);
    float3 specular = numerator / denominator;
    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float NdotL = saturate(dot(normal, L));
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

float3 fallbackTangent(float3 normal)
{
    float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                        : float3(1.0, 0.0, 0.0);
    float3 tangent = cross(axis, normal);
    if (length(tangent) < 1e-4)
    {
        tangent = cross(float3(0.0, 0.0, 1.0), normal);
    }
    if (length(tangent) < 1e-4)
    {
        tangent = float3(1.0, 0.0, 0.0);
    }
    return normalize(tangent);
}

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    float3x3 modelMatrix = (float3x3)obj.model;
    float3 worldNormal = mul((float3x3)obj.normalMatrix, input.inNormal);
    if (!all(isfinite(worldNormal)) || length(worldNormal) < 1e-4)
    {
        worldNormal = float3(0.0, 1.0, 0.0);
    }
    else
    {
        worldNormal = normalize(worldNormal);
    }

    float3 worldTangent = mul(modelMatrix, input.inTangent.xyz);
    worldTangent = worldTangent - worldNormal * dot(worldNormal, worldTangent);
    if (!all(isfinite(worldTangent)) || length(worldTangent) < 1e-4)
    {
        worldTangent = fallbackTangent(worldNormal);
    }
    else
    {
        worldTangent = normalize(worldTangent);
    }

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
    output.worldTangent = float4(worldTangent, input.inTangent.w);
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

    if ((vertIn.flags & 0x1u) != 0u && baseAlpha < vertIn.alphaCutoff)
    {
        discard;
    }

    if (baseAlpha <= 0.01)
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

    float3 dpdx = ddx(vertIn.worldPos);
    float3 dpdy = ddy(vertIn.worldPos);
    float3 faceNormal = normalize(cross(dpdx, dpdy));
    if (!all(isfinite(faceNormal)) || length(faceNormal) < 1e-4)
    {
        faceNormal = float3(0.0, 1.0, 0.0);
    }

    float3 geomNormal = vertIn.worldNormal;
    if (!all(isfinite(geomNormal)) || length(geomNormal) < 1e-4)
    {
        geomNormal = faceNormal;
    }
    else
    {
        geomNormal = normalize(geomNormal);
    }

    float3 tangent = vertIn.worldTangent.xyz;
    tangent = tangent - geomNormal * dot(geomNormal, tangent);
    if (!all(isfinite(tangent)) || length(tangent) < 1e-4)
    {
        tangent = fallbackTangent(geomNormal);
    }
    else
    {
        tangent = normalize(tangent);
    }

    float tangentSign = abs(vertIn.worldTangent.w) > 0.5 ? vertIn.worldTangent.w : 1.0;
    float3 bitangent = normalize(cross(geomNormal, tangent)) * tangentSign;
    if (!all(isfinite(bitangent)) || length(bitangent) < 1e-4)
    {
        bitangent = normalize(cross(geomNormal, fallbackTangent(geomNormal)));
    }

    float3 shadingNormal = geomNormal;
    if (vertIn.normalTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.normalTextureIndex);
        float3 normalSample = materialTextures[texIndex]
                                  .Sample(baseSampler, vertIn.texCoord)
                                  .xyz * 2.0 - 1.0;
        shadingNormal = normalize(
            tangent * normalSample.x +
            bitangent * normalSample.y +
            geomNormal * normalSample.z);
    }

    if (!isFrontFace && (vertIn.flags & 0x4u) != 0u)
    {
        shadingNormal = -shadingNormal;
    }

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

    float metallic = saturate(sampledMetallic);
    float roughness = clamp(sampledRoughness, 0.045, 1.0);
    float3 V = normalize(uCamera.cameraWorldPosition.xyz - vertIn.worldPos);

    float3 lighting = baseColor * 0.03 * occlusion;
    float3 directionalL = normalize(-uLighting.directionalDirection.xyz);
    float3 directionalRadiance =
        uLighting.directionalColorIntensity.rgb * uLighting.directionalColorIntensity.a;
    lighting += EvaluateLight(baseColor, shadingNormal, V, directionalL,
                              directionalRadiance, metallic, roughness);

    for (uint i = 0; i < uLighting.pointLightCount && i < MAX_POINT_LIGHTS; ++i)
    {
        float3 lightVector = uLighting.pointLights[i].positionRadius.xyz - vertIn.worldPos;
        float distanceToLight = length(lightVector);
        float lightRadius = max(uLighting.pointLights[i].positionRadius.w, 1e-3);
        if (distanceToLight >= lightRadius)
        {
            continue;
        }

        float attenuation = pow(saturate(1.0 - distanceToLight / lightRadius), 2.0);
        float3 radiance =
            uLighting.pointLights[i].colorIntensity.rgb *
            uLighting.pointLights[i].colorIntensity.a * attenuation;
        lighting += EvaluateLight(baseColor, shadingNormal, V,
                                  lightVector / max(distanceToLight, 1e-4),
                                  radiance, metallic, roughness);
    }

    lighting += emissive;

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
