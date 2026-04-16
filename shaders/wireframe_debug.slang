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

struct PushConstants
{
    uint objectIndex;
    float3 _pad0;
    float4 colorIntensity;
    float lineWidth;
    float3 _pad1;
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
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    uint objectIndex = instanceID;
    ObjectBuffer obj = uObjects[objectIndex];
    float4 worldPos = mul(obj.model, float4(input.inPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return float4(pc.colorIntensity.rgb, saturate(pc.colorIntensity.a));
}



