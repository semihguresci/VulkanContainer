// irradiance_convolution.slang — Compute shader for diffuse irradiance cubemap.
// Convolves the environment cubemap over a hemisphere per texel.
// Output: 32×32 per face irradiance cubemap.

static const float PI = 3.14159265359;
static const uint SAMPLE_COUNT = 1024u;

#include "push_constants_common.slang"

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

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
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

    // Cosine-weighted hemisphere integration estimates:
    // integral(Li * NdotL dw) = PI * average(Li), because pdf = NdotL / PI.
    // This preserves irradiance energy while avoiding the old dense angular
    // grid's tens of thousands of samples per cubemap texel.
    float3 irradiance = 0.0.xxx;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float phi = 2.0 * PI * Xi.x;
        float cosTheta = sqrt(1.0 - Xi.y);
        float sinTheta = sqrt(Xi.y);
        float3 tangentSample = float3(cos(phi) * sinTheta,
                                      sin(phi) * sinTheta,
                                      cosTheta);
        float3 sampleDir = tangentSample.x * right +
                           tangentSample.y * up +
                           tangentSample.z * N;
        irradiance += envCubemap.SampleLevel(envSampler, sampleDir, 0.0).rgb;
    }

    irradiance = PI * irradiance / float(SAMPLE_COUNT);
    outIrradiance[uint3(dtid.xy, pc.faceIndex)] = float4(irradiance, 1.0);
}
