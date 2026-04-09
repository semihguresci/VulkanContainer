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
    // Gram-Schmidt orthogonalize against the world normal once, here in the vertex shader.
    float3 worldTangentOrtho = worldTangent - worldNormal * dot(worldNormal, worldTangent);
    if (!all(isfinite(worldTangentOrtho)) || length(worldTangentOrtho) < 1e-4)
    {
        worldTangentOrtho = fallbackTangent(worldNormal);
    }
    else
    {
        worldTangentOrtho = normalize(worldTangentOrtho);
    }

    // Tangent handedness: glTF stores it in inTangent.w (+1 or -1).
    // Flip when the model matrix has a negative determinant (mirror transform).
    float modelHandednessSign = determinant(modelMatrix) < 0.0 ? -1.0 : 1.0;
    float tangentHandedness = input.inTangent.w * modelHandednessSign;
    tangentHandedness = tangentHandedness < 0.0 ? -1.0 : 1.0;

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
    output.worldTangent = float4(worldTangentOrtho, tangentHandedness);
    return output;
}

[shader("fragment")]
PSOutput fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace)
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

    float3 dpdx = ddx(vertIn.worldPos);
    float3 dpdy = ddy(vertIn.worldPos);
    float3 faceNormal = normalize(cross(dpdx, dpdy));
    if (!all(isfinite(faceNormal)) || length(faceNormal) < 1e-4)
    {
        faceNormal = float3(0.0, 1.0, 0.0);
    }
    if (!isFrontFace)
    {
        faceNormal = -faceNormal;
    }

    float3 geomNormal = vertIn.worldNormal;
    bool hasGeomNormal = all(isfinite(geomNormal)) && length(geomNormal) >= 1e-4;
    if (!hasGeomNormal)
    {
        geomNormal = faceNormal;
    }
    else
    {
        geomNormal = normalize(geomNormal);
        // Trust rasterized face orientation: force interpolated normal to same hemisphere.
        if (dot(geomNormal, faceNormal) < 0.0)
        {
            geomNormal = -geomNormal;
        }
    }

    float3 tangent = vertIn.worldTangent.xyz;
    if (!all(isfinite(tangent)) || length(tangent) < 1e-4)
    {
        tangent = fallbackTangent(geomNormal);
    }
    else
    {
        tangent = normalize(tangent);
    }

    float tangentSign = abs(vertIn.worldTangent.w) > 0.5 ? vertIn.worldTangent.w : 1.0;
    // RH world space: cross(geomNormal, tangent) gives the standard bitangent.
    float3 bitangent = normalize(cross(geomNormal, tangent)) * tangentSign;
    if (!all(isfinite(bitangent)) || length(bitangent) < 1e-4)
    {
        bitangent = normalize(cross(geomNormal, fallbackTangent(geomNormal))) * tangentSign;
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

    // Keep shading normal on the same hemisphere as the rasterized face.
    if (dot(shadingNormal, faceNormal) < 0.0)
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

    float linearDepth = length(vertIn.worldPos - uCamera.cameraWorldPosition.xyz);

    PSOutput output;
    output.albedo = float4(baseColor, baseAlpha);
    output.normal = float4(normalize(shadingNormal) * 0.5 + 0.5, 1.0);
    output.material = float4(sampledMetallic, sampledRoughness, occlusion, baseAlpha);
    output.emissive = float4(emissive, 1.0);
    output.position = float4(vertIn.worldPos, linearDepth);
    return output;
}
