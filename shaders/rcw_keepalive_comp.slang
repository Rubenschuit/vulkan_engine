// TTL keepalive over the prev map. Probes re-inserted this frame already
// carried their slot through the insert passes (which reset the slot age);
// probes missing from insertion age by one and are re-inserted with their old
// slot until the TTL expires or they leave the key window, then the slot
// returns to the pool.
//
// Indirect dispatch over the previous frame's probe count

#include "rc_world_common.slangh"

[shader("compute")]
[numthreads(RCW_LINEAR_WG, 1, 1)]
void compMain(uint3 thread_id: SV_DispatchThreadID) {
	uint parity = uint(push.anchor0_curr.w);
	uint prev_parity = 1u - parity;
	RcwCascadeDesc desc = cascade_descs[push.cascade_index];
	uint count = counters[rcwCounterIndex(push.cascade_index, prev_parity)].probes;
	if (thread_id.x >= count)
		return;

	uint2 probe_record = probes[desc.probe_base + prev_parity * push.probes_per_parity + thread_id.x];
	if (probe_record.y == RCW_INVALID)
		return;
	uint slot = probe_record.y & RCW_SLOT_MASK;
	if (slot >= desc.probe_cap)
		return;
	int3 offset;
	uint lod;
	rcwDecodeKey(probe_record.x, offset, lod);
	int scale = int(push.cascade_index + lod);
	int3 grid = (push.anchor0_prev.xyz >> scale) + offset;
	uint key_curr = rcwEncodeKey(grid, push.anchor0_curr.xyz >> scale, lod);
	if (key_curr == 0u) {
		rcwFreeSlot(desc, slot);
		return;
	}

	uint existing_probe_index;
	if (rcwHashLookup(desc, parity, key_curr, existing_probe_index))
		return; // Already inserted this frame

	uint age = slot_age[desc.slot_base + slot] + 1u;
	uint ttl = uint(max(push.anchor0_prev.w, 0));
	if (age > ttl) {
		rcwFreeSlot(desc, slot);
		return;
	}

	uint counter_index = rcwCounterIndex(push.cascade_index, parity);

	if (counters[counter_index].probes >= desc.probe_cap) {
		InterlockedAdd(counters[counter_index].overflow, 1u);
		rcwFreeSlot(desc, slot);
		return; // full pool
	}
	uint hash_index;
	uint status = rcwHashClaim(desc, parity, key_curr, hash_index);
	if (status != RCW_CLAIM_NEW) {
		if (status == RCW_CLAIM_FULL)
			InterlockedAdd(counters[counter_index].overflow, 1u);
		if (status != RCW_CLAIM_LIVE)
			rcwFreeSlot(desc, slot);
		return;
	}
	uint probe_index = rcwAllocEntry(desc, counter_index, rcwDirCount(push.cascade_index));
	if (probe_index == RCW_INVALID) {
		hash_vals[hash_index] = RCW_INVALID;
		rcwFreeSlot(desc, slot);
		return;
	}
	slot_age[desc.slot_base + slot] = age;
	probes[desc.probe_base + parity * push.probes_per_parity + probe_index] = uint2(key_curr, slot);
	hash_vals[hash_index] = probe_index;
}
