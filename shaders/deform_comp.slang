// Per-vertex deformation (morph targets + linear blend skinning) for all deformed
// entities in a single dispatch.
//
// One workgroup processes up to SKINNING_WORKGROUP_SIZE vertices of one deform slot.
// The CPU builds a table `wg_info[]` with one entry per workgroup. Each entry carries
// per-entity flags selecting morph and/or skin at runtime (one uber-pipeline).

#include "ve_constants.slangh"

// Inputs live in the static regions of the mega buffers:
//   mega_vbo[input_vertex_offset + v]              - bind-pose VeMesh::Vertex (64 B)
//   mega_skin[input_skin_offset + v]               - SkinVertex (16 B)
//   palette[palette_offset + joint]                - joint matrices (mesh-entity-local)
//   morph_pos[morph_base + t*vertex_count + v]     - POSITION delta (vec4, .xyz)
//   morph_nrm[morph_base + t*vertex_count + v]     - NORMAL delta (vec4, .xyz)
//   morph_weights[weight_offset + t]               - per-target weight
//
// Outputs are written into the dynamic region of the mega VBO + shadow VBO at
// output_vertex_offset. Slots are disjoint by construction. Morph is applied first
// (mesh-local), then skinning, matching glTF semantics.

static const uint DEFORM_HAS_SKIN  = 0x1u;
static const uint DEFORM_HAS_MORPH = 0x2u;

struct DeformWorkgroupInfo {
	uint vertex_count;          // total vertices in this slot
	uint palette_offset;        // in matrices
	uint input_vertex_offset;   // in vertices, into the mega VBO
	uint input_skin_offset;     // in skin vertices, into the mega skin VBO
	uint output_vertex_offset;  // in vertices, into the mega VBO dynamic region
	uint local_vertex_base;     // first vertex index this workgroup handles
	uint deform_flags;          // DEFORM_HAS_SKIN | DEFORM_HAS_MORPH
	uint target_count;
	uint weight_offset;         // index into morph_weights for this entity
	uint morph_base;            // base index (vec4 units) into morph_pos/morph_nrm
	uint _pad0;
	uint _pad1;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> mega_vbo;          // 64B vertex stride: read bind-pose, write deformed output
[[vk::binding(1, 0)]] StructuredBuffer<uint4> mega_skin;            // 16B SkinVertex: 4 uint16 joints + 4 uint16 weights packed into uint4
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> palette;
[[vk::binding(3, 0)]] RWByteAddressBuffer mega_shadow;              // 12B vec3 stride: write position-only output
[[vk::binding(4, 0)]] StructuredBuffer<DeformWorkgroupInfo> wg_info;
[[vk::binding(5, 0)]] StructuredBuffer<float4> morph_pos;           // POSITION deltas (.xyz)
[[vk::binding(6, 0)]] StructuredBuffer<float4> morph_nrm;           // NORMAL deltas (.xyz)
[[vk::binding(7, 0)]] StructuredBuffer<float> morph_weights;

[numthreads(SKINNING_WORKGROUP_SIZE, 1, 1)]
void compMain(uint3 group_id : SV_GroupID, uint3 group_thread_id : SV_GroupThreadID) {
	DeformWorkgroupInfo info = wg_info[group_id.x];
	uint v = info.local_vertex_base + group_thread_id.x;
	if (v >= info.vertex_count)
		return;

	uint in_v = info.input_vertex_offset + v;
	uint out_v = info.output_vertex_offset + v;

	// Read 64B vertex as four float4 (chunk3 = COLOR_0, not deformed):
	float4 chunk0 = mega_vbo[in_v * 4 + 0];
	float4 chunk1 = mega_vbo[in_v * 4 + 1];
	float4 chunk2 = mega_vbo[in_v * 4 + 2];
	float4 color  = mega_vbo[in_v * 4 + 3];

	float3 pos     = chunk0.xyz;
	float3 normal  = float3(chunk0.w, chunk1.x, chunk1.y);
	float2 uv      = float2(chunk1.z, chunk1.w);
	float4 tangent = chunk2;

	// Morph (mesh-local): accumulate weighted position + normal deltas.
	if ((info.deform_flags & DEFORM_HAS_MORPH) != 0u) {
		for (uint t = 0; t < info.target_count; ++t) {
			float w = morph_weights[info.weight_offset + t];
			uint idx = info.morph_base + t * info.vertex_count + v;
			pos    += w * morph_pos[idx].xyz;
			normal += w * morph_nrm[idx].xyz;
		}
		float nl = length(normal);
		if (nl > 1e-6)
			normal = normal / nl; // morph deltas can cancel the base normal
	}

	float3 out_pos    = pos;
	float3 out_normal = normal;
	float3 out_tan    = tangent.xyz;

	// Skinning (LBS) applied to the morphed vertex.
	if ((info.deform_flags & DEFORM_HAS_SKIN) != 0u) {
		uint in_s = info.input_skin_offset + v;
		uint4 packed = mega_skin[in_s];
		uint joints[4] = {
			packed.x & 0xFFFFu, (packed.x >> 16) & 0xFFFFu,
			packed.y & 0xFFFFu, (packed.y >> 16) & 0xFFFFu,
		};
		float weights[4] = {
			float(packed.z & 0xFFFFu) / 65535.0f, float((packed.z >> 16) & 0xFFFFu) / 65535.0f,
			float(packed.w & 0xFFFFu) / 65535.0f, float((packed.w >> 16) & 0xFFFFu) / 65535.0f,
		};

		float4x4 skin = weights[0] * palette[info.palette_offset + joints[0]];
		skin += weights[1] * palette[info.palette_offset + joints[1]];
		skin += weights[2] * palette[info.palette_offset + joints[2]];
		skin += weights[3] * palette[info.palette_offset + joints[3]];

		float3x3 skin3 = (float3x3)skin;
		float3x3 cofactor;
		cofactor[0] = cross(skin3[1], skin3[2]);
		cofactor[1] = cross(skin3[2], skin3[0]);
		cofactor[2] = cross(skin3[0], skin3[1]);

		out_pos    = mul(skin, float4(pos, 1.0)).xyz;
		out_normal = mul(cofactor, normal);
		out_tan    = mul(skin3, tangent.xyz);
	}

	mega_vbo[out_v * 4 + 0] = float4(out_pos, out_normal.x);
	mega_vbo[out_v * 4 + 1] = float4(out_normal.y, out_normal.z, uv.x, uv.y);
	mega_vbo[out_v * 4 + 2] = float4(out_tan, tangent.w);
	mega_vbo[out_v * 4 + 3] = color;

	mega_shadow.Store3(out_v * 12, asuint(out_pos));
}
