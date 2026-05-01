#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;

[[vk::push_constant]]
ConstantBuffer<LightPushConstants> pc;

struct VSOutput
{
    float4 pos : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float3 kAxisVertices[6] = {
        float3(-1.0,  0.0,  0.0),
        float3( 1.0,  0.0,  0.0),
        float3( 0.0, -1.0,  0.0),
        float3( 0.0,  1.0,  0.0),
        float3( 0.0,  0.0, -1.0),
        float3( 0.0,  0.0,  1.0),
    };

    VSOutput output;
    float gizmoRadius =
        PointLightHasFiniteRange(pc.positionRadius.w) ? pc.positionRadius.w : 1.0;
    float3 worldPosition =
        pc.positionRadius.xyz + kAxisVertices[vertexIndex] * gizmoRadius;
    output.pos = mul(uCamera.viewProj, float4(worldPosition, 1.0));
    output.color = pc.colorIntensity.rgb;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
