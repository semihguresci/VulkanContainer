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
    [[vk::location(11)]] float3 worldNormal;
};

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

    float3 worldNormal = mul((float3x3)obj.normalMatrix, input.inNormal);
    if (!all(isfinite(worldNormal)) || length(worldNormal) < 1e-4)
    {
        worldNormal = float3(0.0, 1.0, 0.0);
    }
    else
    {
        worldNormal = normalize(worldNormal);
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
    output.worldNormal = worldNormal;

    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_Target0
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

    if (vertIn.occlusionTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.occlusionTextureIndex);
        float occlusion = materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord).r;
        baseColor *= occlusion;
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

    float3 geomNormal = vertIn.worldNormal;
    if (!all(isfinite(geomNormal)) || length(geomNormal) < 1e-4)
    {
        geomNormal = float3(0.0, 1.0, 0.0);
    }
    else
    {
        geomNormal = normalize(geomNormal);
    }

    float3 shadingNormal = geomNormal;

    float3 lightDir = normalize(float3(0.5, 0.75, -1.0));
    float lighting = saturate(dot(shadingNormal, lightDir));
    baseColor *= 0.2 + 0.8 * lighting;

    if (vertIn.emissiveTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.emissiveTextureIndex);
        float3 emissive = materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord).rgb;
        baseColor += emissive * vertIn.emissiveStrength;
    }

    baseColor = lerp(baseColor, baseColor * (1.0 - sampledRoughness * 0.5), 0.5);
    baseColor = lerp(baseColor, baseColor * (1.0 - sampledMetallic * 0.35), 0.35);
    baseColor += vertIn.emissiveColor * vertIn.emissiveStrength;

    return float4(baseColor, baseAlpha);
}
