// Stream compaction for GPU-culled indirect draw commands.
// Packs draw groups with instanceCount > 0 to the front of each bucket region,
// enabling drawIndexedIndirectCount to skip empty draws entirely.
//
// Dispatch: (ceil(total_groups / 256), 1, 1)

#include "ve_constants.slangh"

struct CompactPush {
	uint total_groups;
	uint bucket_offsets[GPU_CULL_BUCKET_COUNT];
	uint bucket_counts[GPU_CULL_BUCKET_COUNT];
};

[[vk::push_constant]] CompactPush push;

[[vk::binding(0, 0)]] RWByteAddressBuffer src_indirect;
[[vk::binding(1, 0)]] RWByteAddressBuffer dst_indirect;
[[vk::binding(2, 0)]] RWByteAddressBuffer compact_counts;

uint findBucket(uint gid) {
	for (uint b = 0; b < GPU_CULL_BUCKET_COUNT; b++)
		if (gid >= push.bucket_offsets[b] && gid < push.bucket_offsets[b] + push.bucket_counts[b])
			return b;
	return 0;
}

[numthreads(256, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {
	uint gid = tid.x;
	if (gid >= push.total_groups)
		return;

	uint src_offset = gid * 20;
	uint instance_count = src_indirect.Load(src_offset + 4);
	if (instance_count == 0)
		return;

	uint bucket = findBucket(gid);

	uint slot;
	compact_counts.InterlockedAdd(bucket * 4, 1, slot);

	uint dst_offset = (push.bucket_offsets[bucket] + slot) * 20;

	// Copy VkDrawIndexedIndirectCommand (5 x uint32 = 20 bytes)
	dst_indirect.Store(dst_offset + 0,  src_indirect.Load(src_offset + 0));  // indexCount
	dst_indirect.Store(dst_offset + 4,  instance_count);                     // instanceCount
	dst_indirect.Store(dst_offset + 8,  src_indirect.Load(src_offset + 8));  // firstIndex
	dst_indirect.Store(dst_offset + 12, src_indirect.Load(src_offset + 12)); // vertexOffset
	dst_indirect.Store(dst_offset + 16, src_indirect.Load(src_offset + 16)); // firstInstance
}
