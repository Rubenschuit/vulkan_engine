// Cluster light assignment compute shader.
// Assigns point lights to 3D clusters for clustered forward rendering.
//
// Dispatch: (ceil(tiles_x * tiles_y / 256), 1, z_slices)
// Each thread handles one cluster in the XY plane for its Z-slice.

#include "ve_constants.slangh"
#include "ve_cluster.slangh"

// Set 0: Global UBO (reuses the same layout as graphics pipeline)
struct PointLightData {
	float4 position;  // xyz = world pos, w = range
	float4 color;     // xyz = color*intensity, w = intensity
};

struct UniformBuffer {
	float4x4 view;
	float4x4 proj;
	float4x4 projection_view;
	float4 camera_position;
	// We only need the view matrix, but must match the UBO layout up to the fields we access.
};

[[vk::binding(0, 0)]] ConstantBuffer<UniformBuffer> ubo;

// Set 1: Cluster data
[[vk::binding(0, 1)]] RWStructuredBuffer<PointLightData> light_data;
[[vk::binding(1, 1)]] RWStructuredBuffer<uint2>          cluster_counts;     // (offset, count) per cluster
[[vk::binding(2, 1)]] RWStructuredBuffer<uint>           light_index_list;   // packed light indices
[[vk::binding(3, 1)]] ConstantBuffer<ClusterParams>      cluster_params;
[[vk::binding(4, 1)]] RWStructuredBuffer<uint>           atomic_counter;     // global allocation counter

[numthreads(256, 1, 1)]
void compMain(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID) {
	uint flat_xy = dispatch_id.x;
	uint z_slice = group_id.z;

	uint tiles_x = cluster_params.grid_dims.x;
	uint tiles_y = cluster_params.grid_dims.y;
	uint clusters_per_slice = tiles_x * tiles_y;

	if (flat_xy >= clusters_per_slice) return;

	uint tile_x = flat_xy % tiles_x;
	uint tile_y = flat_xy / tiles_x;
	uint3 cluster_id = uint3(tile_x, tile_y, z_slice);

	uint cluster_index = flat_xy + z_slice * clusters_per_slice;

	// Compute view-space AABB for this cluster
	float3 aabb_min, aabb_max;
	getClusterAABB(cluster_id, cluster_params, aabb_min, aabb_max);

	// Test each light against the cluster AABB
	uint local_count = 0;
	uint local_indices[MAX_LIGHTS_PER_CLUSTER];

	uint num_lights = cluster_params.num_lights;
	for (uint i = 0; i < num_lights && local_count < MAX_LIGHTS_PER_CLUSTER; i++) {
		float4 world_pos = light_data[i].position;
		float range = world_pos.w;

		// Transform light to view space
		float4 view_pos = mul(cluster_params.view, float4(world_pos.xyz, 1.0));
		float3 light_view = view_pos.xyz;

		// Culling radius: CPU always provides effective range in w (derived from
		// intensity when the artist didn't set an explicit range). This matches the
		// windowed attenuation in the fragment shader so lights fade smoothly at
		// the cluster boundary instead of popping.
		if (range > 0.0 && sphereAABBIntersect(light_view, range, aabb_min, aabb_max)) {
			local_indices[local_count] = i;
			local_count++;
		}
	}

	// Atomically allocate space in the global light index list
	if (local_count > 0) {
		uint global_offset;
		InterlockedAdd(atomic_counter[0], local_count, global_offset);

		// Write light indices to the global list
		uint max_total = cluster_params.grid_dims.w * cluster_params.max_lights_per_cluster;
		uint write_count = min(local_count, max_total - min(global_offset, max_total));
		for (uint j = 0; j < write_count; j++) {
			light_index_list[global_offset + j] = local_indices[j];
		}

		// Write (offset, count) for this cluster
		cluster_counts[cluster_index] = uint2(global_offset, write_count);
	}
	// If local_count == 0, cluster_counts[cluster_index] remains (0, 0) from fillBuffer clear
}
