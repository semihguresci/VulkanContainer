#pragma once

#include "push_constants_common.slang"

// Matches container::gpu::SceneClipState.
struct SceneClipState
{
    uint boxClipEnabled;
    uint boxClipInvert;
    uint boxClipPlaneCount;
    uint padding0;
    float4 boxClipPlanes[6];
};

[[vk::binding(6, 0)]]
ConstantBuffer<SceneClipState> uSceneClip;

BoxClipState MakeSceneBoxClipState(SceneClipState sceneClip)
{
    BoxClipState boxClip;
    boxClip.enabled = sceneClip.boxClipEnabled;
    boxClip.invert = sceneClip.boxClipInvert;
    boxClip.planeCount = sceneClip.boxClipPlaneCount;
    boxClip.padding0 = 0u;
    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        boxClip.planes[i] = sceneClip.boxClipPlanes[i];
    }
    return boxClip;
}

bool SceneClipPlanesClip(float3 worldPos, uint sectionPlaneEnabled,
                         float4 sectionPlane)
{
    if (SectionPlaneClips(worldPos, sectionPlaneEnabled, sectionPlane))
    {
        return true;
    }
    return BoxClipPlanesClip(worldPos, MakeSceneBoxClipState(uSceneClip));
}
