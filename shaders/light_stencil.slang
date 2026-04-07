struct CameraBuffer
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;

struct PushConstants
{
    float4 positionRadius;
    float4 colorIntensity;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

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
    float3 worldPosition =
        pc.positionRadius.xyz + kCubeVertices[vertexIndex] * pc.positionRadius.w;
    output.pos = mul(uCamera.viewProj, float4(worldPosition, 1.0));
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return 0.0.xxxx;
}
