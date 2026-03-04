// Meshlet culling setup kernel (LEGACY - no longer dispatched).
// The prefix scan and dispatch indirect computation have been moved into
// pass 1 (gpu_cull_meshlet_comp.slang) using atomics.
// This file is kept so the shader auto-discovery build step does not break.

[numthreads(1, 1, 1)]
void compMain(uint3 tid : SV_DispatchThreadID) {}
