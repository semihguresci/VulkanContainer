#pragma once

struct ObjectBuffer
{
    // Must match container::gpu::ObjectData in include/Container/utility/SceneData.h.
    column_major float4x4 model;
    float4 normalMatrix0;
    float4 normalMatrix1;
    float4 normalMatrix2;
    // x = material index; yzw are reserved for future per-object draw metadata.
    uint4 objectInfo;
    float4 boundingSphere;
};
