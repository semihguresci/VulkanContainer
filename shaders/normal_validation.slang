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
    uint showFaceFill;
    float faceAlpha;
    float _pad0;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos;
};

struct GSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] nointerpolation float3 faceColor;
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
    float4 world = mul(obj.model, float4(objectPosition, 1.0));
    output.worldPos = world.xyz;
    output.pos = mul(uCamera.viewProj, world);
    return output;
}

[shader("geometry")]
[maxvertexcount(3)]
void geomMain(triangle VSOutput input[3], inout TriangleStream<GSOutput> stream)
{
    const float3 p0 = input[0].worldPos;
    const float3 p1 = input[1].worldPos;
    const float3 p2 = input[2].worldPos;

    const float3 faceCross = cross(p1 - p0, p2 - p0);
    const float lenSq = dot(faceCross, faceCross);

    float3 baseColor = float3(1.0, 0.0, 0.0);
    const float faceAlpha = saturate(pc.faceAlpha);
    if (lenSq >= 1e-10)
    {
        const float3 faceNormal = normalize(faceCross);
        const float3 centroid = (p0 + p1 + p2) / 3.0;
        float3 viewDir = uCamera.cameraWorldPosition.xyz - centroid;
        if (!all(isfinite(viewDir)) || length(viewDir) < 1e-6)
        {
            viewDir = faceNormal;
        }
        else
        {
            viewDir = normalize(viewDir);
        }
        const bool correct = dot(faceNormal, viewDir) > 0.0;
        baseColor = correct ? float3(0.0, 0.2, 1.0) : float3(1.0, 0.0, 0.0);
    }

    if (pc.showFaceFill != 0u && faceAlpha > 0.001)
    {
        GSOutput out0;
        out0.pos = input[0].pos;
        out0.faceColor = baseColor;
        stream.Append(out0);

        GSOutput out1;
        out1.pos = input[1].pos;
        out1.faceColor = baseColor;
        stream.Append(out1);

        GSOutput out2;
        out2.pos = input[2].pos;
        out2.faceColor = baseColor;
        stream.Append(out2);

        stream.RestartStrip();
    }
}

[shader("fragment")]
float4 fragMain(GSOutput input) : SV_Target0
{
    return float4(input.faceColor, saturate(pc.faceAlpha));
}


