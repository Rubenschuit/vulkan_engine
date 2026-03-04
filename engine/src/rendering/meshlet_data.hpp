#pragma once
#include "ve_config.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace ve {

// GPU-side per-meshlet metadata
struct MeshletGPU {
    glm::vec4 bounding_sphere;    // xyz = local-space center, w = radius
    glm::vec4 cone_apex;          // xyz = local-space cone apex, w = unused
    glm::vec4 cone_axis_cutoff;   // xyz = cone axis, w = cutoff (>= 1.0 = degenerate, skip test)
    uint32_t  index_offset;       // first index in global meshlet IBO
    uint32_t  index_count;        // triangle_count * 3
    uint32_t  vertex_offset;      // start vertex in mega VBO (same as owning object's vertex_offset)
    uint32_t  _pad = 0;
};
static_assert(sizeof(MeshletGPU) == 64, "MeshletGPU must be 64 bytes");

// GPU-side per-object meshlet table
struct MeshletObjectInfo {
    uint32_t meshlet_offset[MAX_LOD_LEVELS];  // start index into MeshletGPU[] per LOD
    uint32_t meshlet_count[MAX_LOD_LEVELS];   // number of meshlets per LOD (0 if LOD unused)
};
static_assert(sizeof(MeshletObjectInfo) == 32, "MeshletObjectInfo must be 32 bytes");

// Pass-1 output: one entry per object that survived frustum+HiZ cull.
struct VisibleObjectEntry {
    uint32_t gpu_id;
    uint32_t lod_level;
    uint32_t meshlet_offset;  // cached from MeshletObjectInfo[gpu_id].meshlet_offset[lod]
    uint32_t meshlet_count;   // cached from MeshletObjectInfo[gpu_id].meshlet_count[lod]
};
static_assert(sizeof(VisibleObjectEntry) == 16, "VisibleObjectEntry must be 16 bytes");

// CPU-side per-LOD meshlet data. Built at mesh load time, consumed by PbrMegaBuffer::build().
struct CpuMeshletLod {
    std::vector<MeshletGPU> meshlets;
    std::vector<uint32_t>   indices;
};

// CPU-side per-mesh meshlet data across all LODs.
struct CpuMeshletData {
    std::vector<CpuMeshletLod> lods;
};

} // namespace ve