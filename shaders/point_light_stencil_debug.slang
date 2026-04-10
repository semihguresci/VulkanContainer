struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] SamplerState gBufferSampler;
[[vk::binding(6, 0)]] Texture2D<float4> gPositionTexture;

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
    [[vk::location(0)]] float2 texCoord;
};

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 kPositions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0),
    };

    static const float2 kTexCoords[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };

    VSOutput output;
    output.pos = float4(kPositions[vertexIndex], 0.0, 1.0);
    output.texCoord = kTexCoords[vertexIndex];
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    uint width = 0u;
    uint height = 0u;
    gPositionTexture.GetDimensions(width, height);
    uint2 pixelCoord = uint2(input.pos.xy);
    pixelCoord = min(pixelCoord, uint2(max(width, 1u) - 1u, max(height, 1u) - 1u));
    float2 uv = (float2(pixelCoord) + 0.5.xx) / float2(max(width, 1u), max(height, 1u));

    float4 positionSample = gPositionTexture.SampleLevel(gBufferSampler, uv, 0.0);
    if (positionSample.w <= 0.0)
    {
        return 0.0.xxxx;
    }

    float3 worldPosition = positionSample.xyz;
    float3 toLight = pc.positionRadius.xyz - worldPosition;
    float lightRadius = max(pc.positionRadius.w, 1e-3);
    float distSq = dot(toLight, toLight);
    if (distSq >= lightRadius * lightRadius)
    {
        return 0.0.xxxx;
    }

    float3 debugColor = pc.colorIntensity.rgb;
    float maxChannel = max(debugColor.r, max(debugColor.g, debugColor.b));
    if (maxChannel > 1e-4)
    {
        debugColor /= maxChannel;
    }
    else
    {
        debugColor = float3(1.0, 0.5, 0.0);
    }

    return float4(debugColor, 1.0);
}
