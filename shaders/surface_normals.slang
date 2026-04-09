struct CameraBuffer
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
    float4 cameraWorldPosition;
};

struct ObjectBuffer
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 color;
    float3 emissiveColor;
    float emissiveStrength;
    float2 metallicRoughness;
    float alphaCutoff;
    uint baseColorTextureIndex;
    uint normalTextureIndex;
    uint occlusionTextureIndex;
    uint emissiveTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint flags;
    float2 padding;
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;

struct PushConstants
{
    uint objectIndex;
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

// Vertex shader passes world-space position and the interpolated vertex normal.
// The geometry shader will also compute a geometric face normal for comparison/fallback.
struct VSOutput
{
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal   : TEXCOORD1;
};

struct GSOutput
{
    float4 pos   : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output;
    ObjectBuffer obj = uObjects[pc.objectIndex];

    float4 worldPos4 = mul(obj.model, float4(input.inPosition, 1.0));

    // Transform normal by the normal matrix (inverse-transpose of model).
    // We keep the raw transformed normal without normalization so the geometry
    // shader can use the un-normalized length as a sanity metric.
    float3 worldNormal = mul((float3x3)obj.normalMatrix, input.inNormal);

    output.worldPosition = worldPos4.xyz;
    output.worldNormal   = worldNormal;
    return output;
}

// Each triangle produces one normal line at its centroid.
// The line direction is the geometric face normal (cross product of edges),
// which is the ground truth for surface orientation and independent of the
// vertex normal attribute convention.
[shader("geometry")]
[maxvertexcount(6)]
void geomMain(triangle VSOutput input[3], inout LineStream<GSOutput> stream)
{
    static const float kNormalLength = 0.12;
    // Color at the base of the line (cyan) blending to tip (yellow).
    static const float4 kBaseColor = float4(0.15, 0.85, 1.0, 1.0);
    static const float4 kTipColor  = float4(1.0,  0.9,  0.1, 1.0);

    float3 p0 = input[0].worldPosition;
    float3 p1 = input[1].worldPosition;
    float3 p2 = input[2].worldPosition;

    // Geometric face normal from edge cross product.
    float3 edge0 = p1 - p0;
    float3 edge1 = p2 - p0;
    float3 faceNormal = cross(edge0, edge1);
    float faceNormalLen = length(faceNormal);
    if (faceNormalLen < 1e-7)
        return; // Degenerate triangle - skip.
    faceNormal /= faceNormalLen;

    // Use averaged vertex normal from the normal attribute. If it is
    // degenerate, fall back to the geometric face normal.
    float3 avgVertexNormal = input[0].worldNormal
                           + input[1].worldNormal
                           + input[2].worldNormal;
    float avgLen = length(avgVertexNormal);
    float3 shadingNormal = (avgLen > 1e-5) ? (avgVertexNormal / avgLen) : faceNormal;

    // Ensure the shading normal faces the same hemisphere as the face normal
    // (handles back-face / winding conventions transparently).
    if (dot(shadingNormal, faceNormal) < 0.0)
        shadingNormal = -shadingNormal;

    float3 centroid = (p0 + p1 + p2) / 3.0;
    float3 tipPos   = centroid + shadingNormal * kNormalLength;

    // Emit vertex normal line (centroid to tip)
    GSOutput base;
    base.pos   = mul(uCamera.viewProj, float4(centroid, 1.0));
    base.color = kBaseColor;
    stream.Append(base);

    GSOutput tip;
    tip.pos   = mul(uCamera.viewProj, float4(tipPos, 1.0));
    tip.color = kTipColor;
    stream.Append(tip);
    stream.RestartStrip();

    // Emit a second, shorter line showing the geometric face normal for comparison.
    static const float kFaceNormalLength = kNormalLength * 0.45;
    static const float4 kFaceBaseColor = float4(1.0, 0.3, 0.3, 0.7);
    static const float4 kFaceTipColor  = float4(1.0, 0.6, 0.1, 0.7);

    float3 faceTipPos = centroid + faceNormal * kFaceNormalLength;

    GSOutput faceBase;
    faceBase.pos   = mul(uCamera.viewProj, float4(centroid, 1.0));
    faceBase.color = kFaceBaseColor;
    stream.Append(faceBase);

    GSOutput faceTip;
    faceTip.pos   = mul(uCamera.viewProj, float4(faceTipPos, 1.0));
    faceTip.color = kFaceTipColor;
    stream.Append(faceTip);
    stream.RestartStrip();
}

[shader("fragment")]
float4 fragMain(GSOutput input) : SV_Target0
{
    return input.color;
}
