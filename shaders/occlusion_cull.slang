// occlusion_cull.slang - Compute shader for Hi-Z occlusion culling.
// Tests each object's bounding sphere against the Hi-Z pyramid generated
// from the depth prepass. Objects that pass the occlusion test are written
// to the indirect draw buffer for the G-Buffer pass.

#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "draw_indirect_common.slang"

#include "object_data_common.slang"

[[vk::push_constant]]
ConstantBuffer<CullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer>                 uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer>              uObjects;
[[vk::binding(2, 0)]] StructuredBuffer<DrawIndexedIndirectCommand> uInputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>                     uDrawCount;
[[vk::binding(5, 0)]] Texture2D<float>                             gHiZPyramid;
[[vk::binding(6, 0)]] StructuredBuffer<uint>                       uInputDrawCount;
[[vk::binding(7, 0)]] SamplerState                                 gHiZSampler;

float ReverseZDepthFromWorldPoint(float4x4 viewProj, float3 worldPoint)
{
    float4 clip = mul(viewProj, float4(worldPoint, 1.0));
    if (clip.w <= 1e-6)
    {
        return 1.0;
    }
    return saturate(clip.z / clip.w);
}

// Project a world-space bounding sphere to a conservative screen-space AABB.
// The bounds intentionally over-estimate the footprint by using the nearest
// possible camera depth for the sphere. closestDepth is the reverse-Z depth of
// the sphere point nearest the camera (largest depth value on the sphere).
bool ProjectSphere(float4x4 viewProj, float3 cameraWorldPosition,
                   float3 center, float radius,
                   out float2 minUV, out float2 maxUV, out float closestDepth)
{
    float4 clipCenter = mul(viewProj, float4(center, 1.0));
    float3 cameraToCenter = center - cameraWorldPosition;
    float cameraDistance = length(cameraToCenter);

    // If the sphere contains the camera, straddles the near plane, or the
    // center is behind the eye, conservatively keep it visible.
    if (cameraDistance <= radius || clipCenter.w <= radius || clipCenter.w <= 0.0)
    {
        minUV = float2(0.0, 0.0);
        maxUV = float2(1.0, 1.0);
        closestDepth = 1.0;
        return true;
    }

    float3 ndc = clipCenter.xyz / clipCenter.w;

    // In Slang, matrix indexing is row-first. For a perspective view-projection
    // matrix, the XYZ lengths of rows 0/1 yield the horizontal/vertical
    // projection scale because the view rows are orthonormal.
    float projScaleX = length(float3(viewProj[0][0], viewProj[0][1], viewProj[0][2]));
    float projScaleY = length(float3(viewProj[1][0], viewProj[1][1], viewProj[1][2]));

    float nearestDepth = max(clipCenter.w - radius, 1e-4);
    float2 projRadius = float2(projScaleX, projScaleY) * (radius / nearestDepth);

    float2 ndcMin = max(ndc.xy - projRadius, float2(-1.0, -1.0));
    float2 ndcMax = min(ndc.xy + projRadius, float2( 1.0,  1.0));

    // Scene passes use a negative-height viewport, so framebuffer UV flips Y
    // relative to scene NDC.
    minUV = float2(ndcMin.x * 0.5 + 0.5, (-ndcMax.y) * 0.5 + 0.5);
    maxUV = float2(ndcMax.x * 0.5 + 0.5, (-ndcMin.y) * 0.5 + 0.5);

    float3 closestPoint =
        center - cameraToCenter * (radius / max(cameraDistance, 1e-6));
    closestDepth = ReverseZDepthFromWorldPoint(viewProj, closestPoint);

    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    uint frustumCulledCount = uInputDrawCount[0];
    if (idx >= frustumCulledCount) return;

    DrawIndexedIndirectCommand draw = uInputDraws[idx];
    uint objectIndex = draw.firstInstance;
    ObjectBuffer obj = uObjects[objectIndex];
    float4 bs = obj.boundingSphere;
    float3 center = bs.xyz;
    float radius = bs.w;

    bool visible = true;

    if (radius > 0.0)
    {
        float2 minUV, maxUV;
        float closestDepth;
        if (ProjectSphere(uCamera.viewProj, uCamera.cameraWorldPosition.xyz,
                          center, radius, minUV, maxUV, closestDepth))
        {
            uint hizW, hizH;
            gHiZPyramid.GetDimensions(hizW, hizH);

            float2 sizePixels = (maxUV - minUV) * float2(hizW, hizH);
            float maxExtent = max(sizePixels.x, sizePixels.y);
            float mipLevel = max(0.0, ceil(log2(maxExtent)) - 1.0);

            // Large projected bounds are poor occlusion-cull candidates with a
            // current-frame Hi-Z pyramid: wall reliefs and other attached
            // details can share screen space with the occluder that was just
            // written by the depth prepass. Keep those visible and let the
            // hardware depth test reject fragments instead.
            const float largeProjectedExtentPixels = 96.0;
            if (maxExtent <= largeProjectedExtentPixels)
            {
                float2 centerUV = (minUV + maxUV) * 0.5;
                float2 minMaxXUV = float2(maxUV.x, minUV.y);
                float2 maxMinXUV = float2(minUV.x, maxUV.y);
                float2 midTopUV = float2(centerUV.x, minUV.y);
                float2 midBottomUV = float2(centerUV.x, maxUV.y);
                float2 midLeftUV = float2(minUV.x, centerUV.y);
                float2 midRightUV = float2(maxUV.x, centerUV.y);

                // Reverse-Z Hi-Z stores the minimum depth in the sampled
                // region, i.e. the farthest visible surface. Use several
                // samples over the projected bounds so elongated or edge-aligned
                // meshes are not rejected by a single optimistic texel.
                float conservativeHiZ = gHiZPyramid.SampleLevel(
                    gHiZSampler, centerUV, mipLevel);
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, minUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, maxUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, minMaxXUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, maxMinXUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midTopUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midBottomUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midLeftUV, mipLevel));
                conservativeHiZ = min(conservativeHiZ,
                    gHiZPyramid.SampleLevel(gHiZSampler, midRightUV, mipLevel));

                // Keep the test biased toward visibility. A strict comparison
                // can falsely reject thin reliefs or objects embedded near a
                // wall after the depth prepass has populated Hi-Z with nearby
                // surfaces.
                const float visibilityBias = 0.005;
                if (closestDepth + visibilityBias < conservativeHiZ)
                {
                    visible = false;
                }
            }
        }
    }

    if (visible)
    {
        uint outIdx;
        InterlockedAdd(uDrawCount[0], 1, outIdx);
        uOutputDraws[outIdx] = draw;
    }
}
