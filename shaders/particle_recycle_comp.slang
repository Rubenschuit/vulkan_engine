// Recycle freed slots back into the free list.
//
// Runs after the main update kernel. For each entry in recycle_queue (indices
// [0..slot_pool_counters[RECYCLE_QUEUE_COUNT_INDEX])), pushes the slot onto
// the free list, then zeroes the recycle-queue count for the next frame.
//
// Dispatched as a single workgroup (1,1,1) so that the same workgroup can
// safely clear the recycle-queue count after the loop with a workgroup barrier

#include "particle_common.slangh"

[shader("compute")]
[numthreads(256, 1, 1)]
void compMain(uint3 group_thread_id : SV_GroupThreadID) {
	uint tid = group_thread_id.x;
	uint count = slot_pool_counters[RECYCLE_QUEUE_COUNT_INDEX];

	for (uint k = tid; k < count; k += 256u)
		pushFreeIndex(recycle_queue[k]);

	GroupMemoryBarrierWithGroupSync();

	if (tid == 0u)
		slot_pool_counters[RECYCLE_QUEUE_COUNT_INDEX] = 0u;
}
