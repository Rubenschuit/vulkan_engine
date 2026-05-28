// Cluster light assignment compute shader.
// Assigns point lights to 3D clusters for clustered forward rendering.
//
// Dispatch: (ceil(tiles_x * tiles_y / CLUSTER_ASSIGN_WORKGROUP_SIZE), 1, z_slices)
// Each thread handles one cluster in the XY plane for its Z-slice.
//
// Each cluster owns a fixed slice [cluster_index * MAX_LIGHTS_PER_CLUSTER, +cap)
// in light_index_list.

#include "ve_constants.slangh"
#include "ve_cluster.slangh"

struct PointLightData {
	float4 position;  // xyz = world pos, w = range
	float4 color;     // unused in compute
};

struct UniformBuffer {
	float4x4 view;
	float4x4 proj;
	float4x4 projection_view;
	float4 camera_position;
};

[[vk::binding(0, 0)]] ConstantBuffer<UniformBuffer> ubo;

[[vk::binding(0, 1)]] RWStructuredBuffer<PointLightData> light_data;
[[vk::binding(1, 1)]] RWStructuredBuffer<uint2>          cluster_counts;     // (offset, count) per cluster
[[vk::binding(2, 1)]] RWStructuredBuffer<uint>           light_index_list;
[[vk::binding(3, 1)]] ConstantBuffer<ClusterParams>      cluster_params;

groupshared float4 s_view_pos_range[CLUSTER_ASSIGN_WORKGROUP_SIZE];

[numthreads(CLUSTER_ASSIGN_WORKGROUP_SIZE, 1, 1)]
void compMain(uint3 dispatch_id : SV_DispatchThreadID,
              uint3 group_id    : SV_GroupID,
              uint  local_id    : SV_GroupThreadID) {
	uint flat_xy = dispatch_id.x;
	uint z_slice = group_id.z;

	uint tiles_x = cluster_params.grid_dims.x;
	uint tiles_y = cluster_params.grid_dims.y;
	uint clusters_per_slice = tiles_x * tiles_y;

	bool active = flat_xy < clusters_per_slice;

	float3 aabb_min = float3(0.0);
	float3 aabb_max = float3(0.0);
	uint cluster_index = 0;
	if (active) {
		uint tile_x = flat_xy % tiles_x;
		uint tile_y = flat_xy / tiles_x;
		uint3 cluster_id = uint3(tile_x, tile_y, z_slice);
		cluster_index = flat_xy + z_slice * clusters_per_slice;
		getClusterAABB(cluster_id, cluster_params, aabb_min, aabb_max);
	}

	uint cluster_base = cluster_index * MAX_LIGHTS_PER_CLUSTER;
	uint local_count = 0;

	uint num_lights = cluster_params.num_lights;
	uint num_batches = (num_lights + CLUSTER_ASSIGN_WORKGROUP_SIZE - 1) / CLUSTER_ASSIGN_WORKGROUP_SIZE;

	for (uint batch = 0; batch < num_batches; batch++) {
		uint base = batch * CLUSTER_ASSIGN_WORKGROUP_SIZE;
		uint load_idx = base + local_id;
		if (load_idx < num_lights) {
			float4 wp = light_data[load_idx].position;
			float3 vp = mul(cluster_params.view, float4(wp.xyz, 1.0)).xyz;
			s_view_pos_range[local_id] = float4(vp, wp.w);
		}
		GroupMemoryBarrierWithGroupSync();

		if (active && local_count < MAX_LIGHTS_PER_CLUSTER) {
			uint batch_size = min(num_lights - base, CLUSTER_ASSIGN_WORKGROUP_SIZE);
			for (uint i = 0; i < batch_size; i++) {
				if (local_count >= MAX_LIGHTS_PER_CLUSTER)
					break;

				float4 vpr = s_view_pos_range[i];
				float range = vpr.w;
				if (range <= 0.0)
					continue;

				if (sphereAABBIntersect(vpr.xyz, range, aabb_min, aabb_max)) {
					light_index_list[cluster_base + local_count] = base + i;
					local_count++;
				}
			}
		}

		GroupMemoryBarrierWithGroupSync();
	}

	if (active)
		cluster_counts[cluster_index] = uint2(cluster_base, local_count);
}