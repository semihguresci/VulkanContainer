struct SurfaceNormalState
{
    float3 geometricNormal;
    float3 shadingNormal;
    float3 tangent;
    float3 bitangent;
};

float3 SafeNormalize(float3 v, float3 fallback)
{
    float lenSq = dot(v, v);
    if (!all(isfinite(v)) || lenSq < 1e-8)
    {
        return fallback;
    }
    return v * rsqrt(lenSq);
}

float3 FallbackTangent(float3 normal)
{
    float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                        : float3(1.0, 0.0, 0.0);
    float3 tangent = cross(axis, normal);
    if (dot(tangent, tangent) < 1e-8)
    {
        tangent = cross(float3(0.0, 0.0, 1.0), normal);
    }
    return SafeNormalize(tangent, float3(1.0, 0.0, 0.0));
}

float3 BuildWorldNormal(float3x3 normalMatrix, float3 objectNormal)
{
    return SafeNormalize(mul(normalMatrix, objectNormal), float3(0.0, 1.0, 0.0));
}

float4 BuildWorldTangent(float3x3 modelMatrix, float3 worldNormal,
                         float4 objectTangent)
{
    float3 tangent = mul(modelMatrix, objectTangent.xyz);
    tangent -= worldNormal * dot(worldNormal, tangent);
    tangent = SafeNormalize(tangent, FallbackTangent(worldNormal));

    float tangentHandedness = abs(objectTangent.w) > 0.5 ? objectTangent.w : 1.0;
    float modelHandedness = determinant(modelMatrix) < 0.0 ? -1.0 : 1.0;
    tangentHandedness = tangentHandedness * modelHandedness;
    tangentHandedness = tangentHandedness < 0.0 ? -1.0 : 1.0;
    return float4(tangent, tangentHandedness);
}

SurfaceNormalState EvaluateSurfaceNormal(float3 worldNormal, float4 worldTangent,
                                         float2 texCoord, uint normalTextureIndex)
{
    SurfaceNormalState state;
    state.geometricNormal = SafeNormalize(worldNormal, float3(0.0, 1.0, 0.0));

    float3 tangent = worldTangent.xyz;
    tangent -= state.geometricNormal * dot(state.geometricNormal, tangent);
    tangent = SafeNormalize(tangent, FallbackTangent(state.geometricNormal));

    float tangentSign = abs(worldTangent.w) > 0.5 ? worldTangent.w : 1.0;
    float3 bitangent = cross(state.geometricNormal, tangent) * tangentSign;
    bitangent = SafeNormalize(
        bitangent,
        cross(state.geometricNormal, FallbackTangent(state.geometricNormal)) *
            tangentSign);

    state.tangent = tangent;
    state.bitangent = bitangent;
    state.shadingNormal = state.geometricNormal;

    if (normalTextureIndex != 0xffffffff)
    {
        uint texIndex = NonUniformResourceIndex(normalTextureIndex);
        float3 normalSample =
            materialTextures[texIndex].Sample(baseSampler, texCoord).xyz * 2.0 - 1.0;
        state.shadingNormal = SafeNormalize(
            tangent * normalSample.x + bitangent * normalSample.y +
                state.geometricNormal * normalSample.z,
            state.geometricNormal);
        if (dot(state.shadingNormal, state.geometricNormal) < 0.0)
        {
            state.shadingNormal *= -1.0;
        }
    }

    return state;
}
