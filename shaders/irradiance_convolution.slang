// irradiance_convolution.slang — Compute shader for diffuse irradiance cubemap.
// Convolves the environment cubemap over a hemisphere per texel.
// Output: 32×32 per face irradiance cubemap.

static const float PI = 3.14159265359;

struct IrradiancePushConstants
{
    uint faceIndex;
    uint faceSize;
};

[[vk::push_constant]]
ConstantBuffer<IrradiancePushConstants> pc;

[[vk::binding(0, 0)]] TextureCube<float4>      envCubemap;
[[vk::binding(1, 0)]] SamplerState             envSampler;
[[vk::binding(2, 0)]] RWTexture2DArray<float4>  outIrradiance;

float3 CubeTexelToDir(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    float3 dir;
    switch (face)
    {
    case 0: dir = float3( 1.0, -st.y, -st.x); break;
    case 1: dir = float3(-1.0, -st.y,  st.x); break;
    case 2: dir = float3( st.x,  1.0,  st.y); break;
    case 3: dir = float3( st.x, -1.0, -st.y); break;
    case 4: dir = float3( st.x, -st.y,  1.0); break;
    case 5: dir = float3(-st.x, -st.y, -1.0); break;
    default: dir = float3(0.0, 0.0, 1.0); break;
    }
    return normalize(dir);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= pc.faceSize || dtid.y >= pc.faceSize) return;

    float2 uv = (float2(dtid.xy) + 0.5) / float(pc.faceSize);
    float3 N = CubeTexelToDir(pc.faceIndex, uv);

    // Build tangent frame.
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    float sampleCount = 0.0;

    static const float sampleDelta = 0.025;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            float sinTheta = sin(theta);
            float cosTheta = cos(theta);
            float3 tangentSample = float3(sinTheta * cos(phi),
                                          sinTheta * sin(phi),
                                          cosTheta);
            float3 sampleDir = tangentSample.x * right +
                               tangentSample.y * up +
                               tangentSample.z * N;

            irradiance += envCubemap.SampleLevel(envSampler, sampleDir, 0.0).rgb *
                          cosTheta * sinTheta;
            sampleCount += 1.0;
        }
    }

    irradiance = PI * irradiance / max(sampleCount, 1.0);
    outIrradiance[uint3(dtid.xy, pc.faceIndex)] = float4(irradiance, 1.0);
}
