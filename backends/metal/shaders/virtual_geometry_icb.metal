#include <metal_stdlib>
#include <metal_command_buffer>

using namespace metal;

// Keep this layout in sync with the 48-byte RHI VirtualGeometryCluster and
// scene.metal's astra_virtual_geometry_cull output.
struct AstraVirtualGeometryCluster {
    float4 bounds_sphere;
    float geometric_error;
    uint parent_index;
    uint first_index;
    uint index_count;
    int base_vertex;
    uint flags;
    uint2 padding;
};
static_assert(sizeof(AstraVirtualGeometryCluster) == 48,
              "Astra virtual geometry cluster ABI must remain 48 bytes");

// Matches VirtualGeometryIndirectArguments in scene.metal and the Metal host.
struct AstraVirtualGeometryCullResult {
    uint index_count;
    uint instance_count;
    uint index_start;
    int base_vertex;
    uint base_instance;
};
static_assert(sizeof(AstraVirtualGeometryCullResult) == 20,
              "Astra virtual geometry cull result ABI must remain 20 bytes");

// Metal exposes an ICB to a kernel through an argument buffer. The host must
// encode the MTLIndirectCommandBuffer at argument-buffer id 0.
struct AstraVirtualGeometryICBContainer {
    command_buffer commands [[id(0)]];
};

// One thread owns one ICB slot, so no atomics or cross-thread synchronization
// are needed. The culling kernel remains responsible for the existing
// frustum + hierarchical projected-error selection policy; this kernel only
// translates those results into executable indexed draw commands.
kernel void astra_virtual_geometry_encode_icb(
    device const AstraVirtualGeometryCluster *clusters [[buffer(0)]],
    device const AstraVirtualGeometryCullResult *culling_results [[buffer(1)]],
    device uint *index_buffer [[buffer(2)]],
    device AstraVirtualGeometryICBContainer *icb_container [[buffer(3)]],
    constant uint &cluster_count [[buffer(4)]],
    uint cluster_index [[thread_position_in_grid]]) {
    if (cluster_index >= cluster_count)
        return;

    render_command command(icb_container->commands, cluster_index);

    // Reset first on every frame. A reset command executes as a no-op, which
    // guarantees that an invisible cluster cannot retain a previous frame's
    // draw. The host may also reset the ICB with a blit encoder before dispatch.
    command.reset();

    const AstraVirtualGeometryCluster cluster = clusters[cluster_index];
    const AstraVirtualGeometryCullResult visible = culling_results[cluster_index];
    if (visible.index_count == 0U || visible.instance_count == 0U ||
        cluster.index_count == 0U || visible.index_count != cluster.index_count ||
        visible.index_start != cluster.first_index ||
        visible.base_vertex != cluster.base_vertex || visible.base_vertex < 0)
        return;

    // The ICB descriptor inherits stage buffers and pipeline state from the
    // parent render encoder. This preserves scene uniforms, material resources,
    // and the chunk's packed static vertex buffer at vertex slot 0. The index
    // buffer is embedded in the indexed draw command itself.
    // The culling result is the authoritative visibility decision; validate
    // its range against the persistent cluster record before encoding it.
    command.draw_indexed_primitives(
        primitive_type::triangle,
        visible.index_count,
        index_buffer + visible.index_start,
        visible.instance_count,
        uint(visible.base_vertex),
        visible.base_instance);
}
