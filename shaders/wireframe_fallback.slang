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

#include "object_index_common.slang"
#include "pbr_material_common.slang"

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

struct GSOutput
{
    float4 pos : SV_Position;
    noperspective [[vk::location(0)]] float3 bary;
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
    output.pos = mul(uCamera.viewProj, worldPos);
    return output;
}

[shader("geometry")]
[maxvertexcount(3)]
void geomMain(triangle VSOutput input[3], inout TriangleStream<GSOutput> stream)
{
    GSOutput out0;
    out0.pos = input[0].pos;
    out0.bary = float3(1.0, 0.0, 0.0);
    stream.Append(out0);

    GSOutput out1;
    out1.pos = input[1].pos;
    out1.bary = float3(0.0, 1.0, 0.0);
    stream.Append(out1);

    GSOutput out2;
    out2.pos = input[2].pos;
    out2.bary = float3(0.0, 0.0, 1.0);
    stream.Append(out2);

    stream.RestartStrip();
}

[shader("fragment")]
float4 fragMain(GSOutput input) : SV_Target0
{
    float3 deriv = fwidth(input.bary);
    deriv = max(deriv, float3(1e-6, 1e-6, 1e-6));

    float widthPx = max(pc.lineWidth, 1.0);
    float3 edgeDistance = input.bary / deriv;
    float edgeMetric = min(min(edgeDistance.x, edgeDistance.y), edgeDistance.z);
    float edgeAlpha = 1.0 - smoothstep(0.0, widthPx, edgeMetric);
    float alpha = saturate(pc.colorIntensity.a) * saturate(edgeAlpha);
    if (alpha <= 0.001)
    {
        discard;
    }
    return float4(pc.colorIntensity.rgb, alpha);
}
