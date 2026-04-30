// shadow_cull.slang - Compute shader for per-cascade light-space shadow caster culling.

#include "lighting_structs.slang"

struct ObjectBuffer
{
	column_major float4x4 model;
	float4 normalMatrix0;
	float4 normalMatrix1;
	float4 normalMatrix2;
	float4 color;
	float3 emissiveColor;
	float emissiveStrength;
	float2 metallicRoughness;
	float alphaCutoff;
	float normalTextureScale;
	float occlusionStrength;
	uint baseColorTextureIndex;
	uint normalTextureIndex;
	uint occlusionTextureIndex;
	uint emissiveTextureIndex;
	uint metallicRoughnessTextureIndex;
	uint flags;
	float4 boundingSphere;
};

struct DrawIndexedIndirectCommand
{
	uint indexCount;
	uint instanceCount;
	uint firstIndex;
	int  vertexOffset;
	uint firstInstance;
};

[[vk::push_constant]]
ConstantBuffer<ShadowCullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<ShadowCullBuffer> uShadowCull;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] StructuredBuffer<DrawIndexedIndirectCommand> uInputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> uDrawCount;

bool SphereIntersectsCascade(float4 sphere, ShadowCascadeCullData cascade)
{
	float3 center = mul(cascade.lightView, float4(sphere.xyz, 1.0)).xyz;
	float radius = sphere.w;
	float3 sphereMin = center - radius.xxx;
	float3 sphereMax = center + radius.xxx;

	return sphereMax.x >= cascade.casterMinBounds.x &&
		   sphereMin.x <= cascade.casterMaxBounds.x &&
		   sphereMax.y >= cascade.casterMinBounds.y &&
		   sphereMin.y <= cascade.casterMaxBounds.y &&
		   sphereMax.z >= cascade.casterMinBounds.z &&
		   sphereMin.z <= cascade.casterMaxBounds.z;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
	uint idx = dtid.x;
	if (idx >= pc.drawCount || pc.cascadeIndex >= SHADOW_CASCADE_COUNT)
	{
		return;
	}

	DrawIndexedIndirectCommand draw = uInputDraws[idx];
	uint objectIndex = draw.firstInstance;
	if (objectIndex >= pc.objectCount)
	{
		return;
	}
	ObjectBuffer objectData = uObjects[objectIndex];
	float4 boundingSphere = objectData.boundingSphere;

	bool visible = boundingSphere.w <= 0.0 ||
				   SphereIntersectsCascade(
					   boundingSphere,
					   uShadowCull.cascades[pc.cascadeIndex]);

	if (visible)
	{
		uint outIdx;
		InterlockedAdd(uDrawCount[0], 1, outIdx);
		uOutputDraws[pc.outputOffset + outIdx] = draw;
	}
}
