struct CameraBuffer
{
    float4x4 viewProj;
};

struct ObjectBuffer
{
    float4x4 model;
    float4 color;
    float3 emissiveColor;
    float emissiveStrength;
    float2 metallicRoughness;
    uint baseColorTextureIndex;
    uint normalTextureIndex;
    uint occlusionTextureIndex;
    uint emissiveTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint padding;
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
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 baseColor;
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
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.baseColor = obj.color.rgb;
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
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_Target0
{
    float3 baseColor = vertIn.baseColor;
    float baseAlpha = 1.0;

    if (vertIn.baseColorTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.baseColorTextureIndex);
        float4 texColor = materialTextures[texIndex].Sample(baseSampler, vertIn.texCoord);
        baseColor *= texColor.rgb;
        baseAlpha = texColor.a;
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

    float3 dpdx = ddx(vertIn.worldPos);
    float3 dpdy = ddy(vertIn.worldPos);
    float2 duvdx = ddx(vertIn.texCoord);
    float2 duvdy = ddy(vertIn.texCoord);

    float3 geomNormal = normalize(cross(dpdx, dpdy));
    if (!all(isfinite(geomNormal)) || length(geomNormal) < 1e-4)
    {
        geomNormal = float3(0.0, 0.0, 1.0);
    }

    float3 tangent = dpdx * duvdy.y - dpdy * duvdx.y;
    float3 bitangent = -dpdx * duvdy.x + dpdy * duvdx.x;
    if (length(tangent) < 1e-4 || length(bitangent) < 1e-4)
    {
        tangent = float3(1.0, 0.0, 0.0);
        bitangent = float3(0.0, 1.0, 0.0);
    }
    tangent = normalize(tangent);
    bitangent = normalize(bitangent);

    float3 shadingNormal = geomNormal;
    if (vertIn.normalTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(vertIn.normalTextureIndex);
        float3 normalSample = materialTextures[texIndex]
                                  .Sample(baseSampler, vertIn.texCoord)
                                  .xyz * 2.0 - 1.0;
        float3x3 tbn = float3x3(tangent, bitangent, geomNormal);
        shadingNormal = normalize(mul(normalSample, tbn));
    }

    float3 lightDir = normalize(float3(0.5, 0.75, 1.0));
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
