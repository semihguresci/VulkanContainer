#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;

[[vk::push_constant]]
ConstantBuffer<LightPushConstants> pc;

struct VSOutput
{
    float4 pos : SV_Position;
};

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float3 kCubeVertices[36] = {
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0),
        float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0), float3(-1.0, -1.0, -1.0),
        float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0), float3(-1.0, -1.0,  1.0),
        float3(-1.0,  1.0,  1.0), float3(-1.0,  1.0, -1.0), float3(-1.0, -1.0, -1.0),
        float3(-1.0, -1.0, -1.0), float3(-1.0, -1.0,  1.0), float3(-1.0,  1.0,  1.0),
        float3( 1.0,  1.0,  1.0), float3( 1.0,  1.0, -1.0), float3( 1.0, -1.0, -1.0),
        float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0),
        float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0, -1.0,  1.0),
        float3( 1.0, -1.0,  1.0), float3(-1.0, -1.0,  1.0), float3(-1.0, -1.0, -1.0),
        float3(-1.0,  1.0, -1.0), float3( 1.0,  1.0, -1.0), float3( 1.0,  1.0,  1.0),
        float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0), float3(-1.0,  1.0, -1.0)
    };

    VSOutput output;
    float finiteVolumeRadius =
        PointLightHasFiniteRange(pc.positionRadius.w) ? pc.positionRadius.w : 1000000.0;
    float3 worldPosition =
        pc.positionRadius.xyz + kCubeVertices[vertexIndex] * finiteVolumeRadius;
    output.pos = mul(uCamera.viewProj, float4(worldPosition, 1.0));
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return 0.0.xxxx;
}
