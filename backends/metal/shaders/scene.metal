#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float4 color [[attribute(2)]];
    float2 texcoord0 [[attribute(3)]];
    float4 tangent [[attribute(4)]];
    ushort4 joints [[attribute(5)]];
    float4 weights [[attribute(6)]];
    float2 texcoord1 [[attribute(7)]];
};

struct SceneUniforms {
    float4x4 view_projection;
    float4x4 model;
    float4x4 light_view_projection[3];
    float4 camera_position;
    float4 light_direction;
    float4 light_radiance;
    float4 material;
    float4 base_color;
    float4 emissive;
    float4 material_flags;
    float4 texture_flags2;
    float4 skin_flags;
    float4 scene_params;   // time, exposure, fog density, reserved
    float4 texcoord_sets0; // base color, metallic-roughness, normal, occlusion
    float4 texcoord_sets1; // emissive, reserved
    float4 alpha_flags;    // alpha mode, cutoff, double-sided, reserved
    float4 cascade_splits;
    float4 cascade_camera_forward;
    float4x4 previous_view_projection;
    float4x4 previous_model;
    float4 temporal_params;
    float4 uv_transform[5]; // offset.xy, scale.xy
    float4 uv_rotation[5];  // cos(rotation), sin(rotation), reserved, reserved
    float4 environment_flags; // enabled, maximum prefilter mip, reserved, reserved
    uint4 morph_flags; // target count, per-instance weights, vertex count, point-light count
};

struct PointLight {
    float4 position_range;
    float4 intensity;
};

struct MorphDelta {
    float4 position;
    float4 normal;
    float4 tangent;
};

float2 apply_uv_transform(float2 uv, float4 transform, float4 rotation) {
    const float2 scaled = uv * transform.zw;
    return transform.xy + float2(rotation.x * scaled.x - rotation.y * scaled.y,
                                 rotation.y * scaled.x + rotation.x * scaled.y);
}

struct VirtualGeometryCluster {
    float4 bounds_sphere;
    float geometric_error;
    uint parent_index;
    uint first_index;
    uint index_count;
    int base_vertex;
    uint flags;
    uint2 padding;
};

struct VirtualGeometryIndirectArguments {
    uint index_count;
    uint instance_count;
    uint index_start;
    int base_vertex;
    uint base_instance;
};

struct VirtualGeometryCullUniforms {
    float4x4 model;
    float4 camera_viewport; // camera xyz, viewport height in pixels
    float4 lod_parameters;  // tan(vertical FOV / 2), pixel threshold, near clamp, shadow
                            // world-units/texel
    float4 camera_forward;  // derived from the original camera VP, never the shadow VP
    float4 frustum_planes[6];
};

float3 virtual_geometry_world_center(float3 local_center, float4x4 model) {
    return (model * float4(local_center, 1.0)).xyz;
}

float virtual_geometry_operator_norm_bound(float4x4 model) {
    const float3 a0 = model[0].xyz;
    const float3 a1 = model[1].xyz;
    const float3 a2 = model[2].xyz;
    // sqrt(||A^T A||_infinity) bounds the spectral norm, including shear.
    const float g00 = dot(a0, a0);
    const float g01 = dot(a0, a1);
    const float g02 = dot(a0, a2);
    const float g11 = dot(a1, a1);
    const float g12 = dot(a1, a2);
    const float g22 = dot(a2, a2);
    const float row0 = abs(g00) + abs(g01) + abs(g02);
    const float row1 = abs(g01) + abs(g11) + abs(g12);
    const float row2 = abs(g02) + abs(g12) + abs(g22);
    return sqrt(max(row0, max(row1, row2)));
}

float virtual_geometry_world_radius(float local_radius, float4x4 model) {
    return local_radius * virtual_geometry_operator_norm_bound(model);
}

float virtual_geometry_projected_error(float geometric_error, float3 world_center,
                                       float world_radius,
                                       constant VirtualGeometryCullUniforms &uniforms) {
    if (geometric_error <= 0.0)
        return 0.0;
    const float camera_forward_depth =
        dot(world_center - uniforms.camera_viewport.xyz, uniforms.camera_forward.xyz);
    // Shadow passes measure error directly in cascade texels. Camera passes
    // retain perspective screen-space error based on viewport height and FOV.
    if (uniforms.lod_parameters.w > 0.0)
        return geometric_error / uniforms.lod_parameters.w;
    const float distance_to_camera =
        max(camera_forward_depth - world_radius, uniforms.lod_parameters.z);
    return geometric_error * uniforms.camera_viewport.w /
           max(2.0 * uniforms.lod_parameters.x * distance_to_camera, 0.000001);
}

kernel void astra_virtual_geometry_cull(constant VirtualGeometryCullUniforms &uniforms
                                        [[buffer(0)]],
                                        device const VirtualGeometryCluster *clusters [[buffer(1)]],
                                        device VirtualGeometryIndirectArguments *indirect_arguments
                                        [[buffer(2)]],
                                        uint cluster_index [[thread_position_in_grid]]) {
    const VirtualGeometryCluster cluster = clusters[cluster_index];
    const float3 world_center =
        virtual_geometry_world_center(cluster.bounds_sphere.xyz, uniforms.model);
    const float world_radius =
        virtual_geometry_world_radius(cluster.bounds_sphere.w, uniforms.model);
    bool outside_frustum = false;
    for (uint plane = 0; plane < 6; ++plane) {
        const float4 equation = uniforms.frustum_planes[plane];
        if (dot(equation.xyz, world_center) + equation.w < -world_radius) {
            outside_frustum = true;
            break;
        }
    }

    const float model_scale_bound = virtual_geometry_operator_norm_bound(uniforms.model);
    const float own_error =
        (cluster.flags & 1U) != 0U
            ? 0.0
            : virtual_geometry_projected_error(cluster.geometric_error * model_scale_bound,
                                               world_center, world_radius, uniforms);
    bool parent_refined = cluster.parent_index == 0xffffffffU;
    if (!parent_refined) {
        const VirtualGeometryCluster parent = clusters[cluster.parent_index];
        const float3 parent_center =
            virtual_geometry_world_center(parent.bounds_sphere.xyz, uniforms.model);
        const float parent_radius =
            virtual_geometry_world_radius(parent.bounds_sphere.w, uniforms.model);
        const float parent_error =
            (parent.flags & 1U) != 0U
                ? 0.0
                : virtual_geometry_projected_error(parent.geometric_error * model_scale_bound,
                                                   parent_center, parent_radius, uniforms);
        parent_refined = parent_error > uniforms.lod_parameters.y;
    }
    const bool selected =
        !outside_frustum && own_error <= uniforms.lod_parameters.y && parent_refined;
    VirtualGeometryIndirectArguments arguments;
    arguments.index_count = selected ? cluster.index_count : 0U;
    arguments.instance_count = selected ? 1U : 0U;
    arguments.index_start = cluster.first_index;
    arguments.base_vertex = cluster.base_vertex;
    arguments.base_instance = 0U;
    indirect_arguments[cluster_index] = arguments;
}

struct VertexOut {
    float4 position [[position]];
    float3 world_position;
    float3 normal;
    float4 color;
    float2 texcoord0;
    float2 texcoord1;
    float3 tangent;
    float tangent_sign;
    float4 current_clip;
    float4 previous_clip;
};

float4x4 skin_matrix_with_count(VertexIn input, uint joint_count,
                                constant float4x4 *joint_matrices) {
    if (joint_count == 0U)
        return float4x4(1.0);
    uint4 indices = min(uint4(input.joints), uint4(joint_count - 1));
    float4 weights = max(input.weights, 0.0);
    weights /= max(dot(weights, float4(1.0)), 0.000001);
    return joint_matrices[indices.x] * weights.x + joint_matrices[indices.y] * weights.y +
           joint_matrices[indices.z] * weights.z + joint_matrices[indices.w] * weights.w;
}

float4x4 skin_matrix(VertexIn input, constant SceneUniforms &scene,
                     constant float4x4 *joint_matrices) {
    return skin_matrix_with_count(input, uint(scene.skin_flags.x), joint_matrices);
}

float3x3 linear_part(float4x4 transform) {
    return float3x3(transform[0].xyz, transform[1].xyz, transform[2].xyz);
}

float3 transform_normal(float3x3 transform, float3 normal) {
    float3 c0 = cross(transform[1], transform[2]);
    float3 c1 = cross(transform[2], transform[0]);
    float3 c2 = cross(transform[0], transform[1]);
    float determinant = dot(transform[0], c0);
    if (abs(determinant) < 0.000001)
        return transform * normal;
    return (float3x3(c0, c1, c2) / determinant) * normal;
}

// All foliage passes use the same world-space deformation. Applying the offset
// after the model transform keeps wind direction stable for rotated/scaled
// instances and keeps camera, previous-frame and shadow silhouettes identical.
float3 foliage_wind_offset(float3 world_anchor, float bend, float time) {
    const float phase = time * 1.35 + world_anchor.x * 0.19 + world_anchor.z * 0.13;
    return float3((sin(phase) * 0.10 + sin(phase * 0.47 + 1.7) * 0.055) * bend,
                  0.0,
                  cos(phase * 0.73) * 0.045 * bend);
}

void apply_morph_data(VertexIn input, uint vertex_id, uint instance_id,
                      uint target_count, uint vertex_count, bool per_instance,
                      device const float *morph_weights,
                      device const MorphDelta *morph_deltas,
                      thread float3 &position, thread float3 &normal,
                      thread float3 &tangent) {
    const uint weight_base = per_instance ? instance_id * target_count : 0U;
    position = input.position;
    normal = input.normal;
    tangent = input.tangent.xyz;
    for (uint target = 0; target < target_count; ++target) {
        const float weight = morph_weights[weight_base + target];
        if (abs(weight) <= 1.0e-8)
            continue;
        const MorphDelta delta = morph_deltas[target * vertex_count + vertex_id];
        position += delta.position.xyz * weight;
        normal += delta.normal.xyz * weight;
        tangent += delta.tangent.xyz * weight;
    }
}

void apply_morph_targets(VertexIn input, uint vertex_id, uint instance_id,
                         constant SceneUniforms &scene,
                         device const float *morph_weights,
                         device const MorphDelta *morph_deltas,
                         thread float3 &position, thread float3 &normal,
                         thread float3 &tangent) {
    apply_morph_data(input, vertex_id, instance_id, scene.morph_flags.x,
                     scene.morph_flags.z, scene.morph_flags.y != 0U,
                     morph_weights, morph_deltas, position, normal, tangent);
}

struct MorphValidationUniforms {
    uint target_count;
    uint vertex_count;
    uint per_instance;
    uint reserved;
};

struct MorphValidationOutput {
    float4 current_position;
    float4 previous_position;
    float4 current_normal;
    float4 current_tangent;
};

// A small GPU regression kernel exercises the same morph and skinning helpers
// used by the production vertex entry points. It is built into the normal
// metallib and invoked only by the macOS GPU test target.
kernel void astra_morph_validation_kernel(
    device const float4 *positions [[buffer(0)]],
    device const float4 *normals [[buffer(1)]],
    device const float4 *tangents [[buffer(2)]],
    device const ushort4 *joints [[buffer(3)]],
    device const float4 *skin_weights [[buffer(4)]],
    device const MorphDelta *morph_deltas [[buffer(5)]],
    device const float *current_morph_weights [[buffer(6)]],
    device const float *previous_morph_weights [[buffer(7)]],
    constant float4x4 *current_joint_matrices [[buffer(8)]],
    constant float4x4 *previous_joint_matrices [[buffer(9)]],
    constant MorphValidationUniforms &uniforms [[buffer(10)]],
    device MorphValidationOutput *outputs [[buffer(11)]],
    uint2 thread_id [[thread_position_in_grid]]) {
    const uint vertex_id = thread_id.x;
    const uint instance_id = thread_id.y;
    VertexIn input;
    input.position = positions[vertex_id].xyz;
    input.normal = normals[vertex_id].xyz;
    input.tangent = tangents[vertex_id];
    input.joints = joints[vertex_id];
    input.weights = skin_weights[vertex_id];
    input.color = float4(1.0);
    input.texcoord0 = float2(0.0);
    input.texcoord1 = float2(0.0);

    float3 current_position;
    float3 current_normal;
    float3 current_tangent;
    apply_morph_data(input, vertex_id, instance_id, uniforms.target_count,
                     uniforms.vertex_count, uniforms.per_instance != 0U,
                     current_morph_weights, morph_deltas,
                     current_position, current_normal, current_tangent);
    VertexIn current_input = input;
    current_input.position = current_position;
    current_input.normal = current_normal;
    current_input.tangent.xyz = current_tangent;

    float3 previous_position;
    float3 previous_normal;
    float3 previous_tangent;
    apply_morph_data(input, vertex_id, instance_id, uniforms.target_count,
                     uniforms.vertex_count, uniforms.per_instance != 0U,
                     previous_morph_weights, morph_deltas,
                     previous_position, previous_normal, previous_tangent);
    VertexIn previous_input = input;
    previous_input.position = previous_position;
    previous_input.normal = previous_normal;
    previous_input.tangent.xyz = previous_tangent;

    const uint output_index = instance_id * uniforms.vertex_count + vertex_id;
    outputs[output_index].current_position =
        skin_matrix_with_count(current_input, 1U, current_joint_matrices) *
        float4(current_position, 1.0);
    outputs[output_index].previous_position =
        skin_matrix_with_count(previous_input, 1U, previous_joint_matrices) *
        float4(previous_position, 1.0);
    outputs[output_index].current_normal = float4(current_normal, 0.0);
    outputs[output_index].current_tangent = float4(current_tangent, input.tangent.w);
}

float4 skinned_position_at_time(VertexIn input, constant SceneUniforms &scene,
                                constant float4x4 *joint_matrices, uint joint_count, float time,
                                float4x4 model_transform) {
    float4 position =
        skin_matrix_with_count(input, joint_count, joint_matrices) * float4(input.position, 1.0);
    if (input.tangent.w == 0.0) {
        const float bend = clamp(input.texcoord1.y, 0.0, 1.0);
        const float3 world_anchor = (model_transform * float4(input.position, 1.0)).xyz;
        const float phase = time * 1.35 + world_anchor.x * 0.19 + world_anchor.z * 0.13;
        const float gust = sin(phase) * 0.10 + sin(phase * 0.47 + 1.7) * 0.055;
        position.x += gust * bend;
        position.z += cos(phase * 0.73) * 0.045 * bend;
    }
    return position;
}

float4 skinned_position(VertexIn input, constant SceneUniforms &scene,
                        constant float4x4 *joint_matrices, float4x4 model_transform) {
    return skinned_position_at_time(input, scene, joint_matrices, uint(scene.skin_flags.x),
                                    scene.scene_params.x, model_transform);
}

float3 skinned_normal(VertexIn input, constant SceneUniforms &scene,
                      constant float4x4 *joint_matrices) {
    float3x3 skin_linear = linear_part(skin_matrix(input, scene, joint_matrices));
    return transform_normal(skin_linear, input.normal);
}

vertex VertexOut astra_vertex(VertexIn input [[stage_in]],
                              constant SceneUniforms &scene [[buffer(1)]],
                              constant float4x4 *joint_matrices [[buffer(2)]],
                              constant float4x4 *previous_joint_matrices [[buffer(3)]],
                              device const float4x4 *instance_models [[buffer(4)]],
                              device const float4x4 *previous_instance_models [[buffer(5)]],
                              device const float *morph_weights [[buffer(6)]],
                              device const float *previous_morph_weights [[buffer(7)]],
                              device const MorphDelta *morph_deltas [[buffer(8)]],
                              uint vertex_id [[vertex_id]],
                              uint instance_id [[instance_id]]) {
    VertexOut out;
    float4x4 model_transform = scene.model;
    float4x4 previous_model_transform = scene.previous_model;
    if (scene.texture_flags2.w > 0.5) {
        model_transform = instance_models[instance_id] * scene.model;
        previous_model_transform = previous_instance_models[instance_id] * scene.previous_model;
    }
    // Evaluate the current skin matrix once. Position, normal and tangent all
    // use the same weighted joint transform; recomputing it three times made
    // the high-vertex-count player unnecessarily expensive on integrated GPUs.
    VertexIn current_input = input;
    float3 current_position;
    float3 current_normal;
    float3 current_tangent;
    apply_morph_targets(input, vertex_id, instance_id, scene, morph_weights, morph_deltas,
                        current_position, current_normal, current_tangent);
    current_input.position = current_position;
    current_input.normal = current_normal;
    current_input.tangent.xyz = current_tangent;
    const float4x4 current_skin = skin_matrix(current_input, scene, joint_matrices);
    float4 local_position = current_skin * float4(current_position, 1.0);
    float4 world = model_transform * local_position;
    if (input.tangent.w == 0.0) {
        const float bend = clamp(input.texcoord1.y, 0.0, 1.0);
        world.xyz += foliage_wind_offset(world.xyz, bend, scene.scene_params.x);
    }
    out.position = scene.view_projection * world;
    out.current_clip = out.position;
    out.previous_clip = out.position;
    if (scene.temporal_params.x > 0.5) {
        VertexIn previous_input = input;
        float3 previous_position;
        float3 previous_normal;
        float3 previous_tangent;
        apply_morph_targets(input, vertex_id, instance_id, scene, previous_morph_weights,
                            morph_deltas, previous_position, previous_normal, previous_tangent);
        previous_input.position = previous_position;
        previous_input.normal = previous_normal;
        previous_input.tangent.xyz = previous_tangent;
        float4 previous_local_position = skinned_position_at_time(
            previous_input, scene, previous_joint_matrices, uint(scene.skin_flags.y), scene.temporal_params.y,
            previous_model_transform);
        float4 previous_world = previous_model_transform * previous_local_position;
        if (input.tangent.w == 0.0) {
            const float bend = clamp(input.texcoord1.y, 0.0, 1.0);
            previous_world.xyz += foliage_wind_offset(previous_world.xyz, bend,
                                                       scene.temporal_params.y);
        }
        out.previous_clip = scene.previous_view_projection * previous_world;
    }
    out.world_position = world.xyz;
    float3x3 model_3x3 = float3x3(model_transform[0].xyz, model_transform[1].xyz,
                                  model_transform[2].xyz);
    const float3x3 skin_linear = linear_part(current_skin);
    float3 local_normal = transform_normal(skin_linear, current_normal);
    out.normal = normalize(transform_normal(model_3x3, local_normal));
    out.color = input.color;
    out.texcoord0 = input.texcoord0;
    out.texcoord1 = input.texcoord1;
    float3 transformed_tangent = model_3x3 * skin_linear * current_tangent;
    transformed_tangent -= out.normal * dot(out.normal, transformed_tangent);
    float tangent_length_squared = dot(transformed_tangent, transformed_tangent);
    if (tangent_length_squared > 0.000001) {
        out.tangent = transformed_tangent * rsqrt(tangent_length_squared);
    } else {
        // Degenerate transforms or tangent data: choose a stable perpendicular.
        float3 reference_axis =
            abs(out.normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
        out.tangent = normalize(cross(reference_axis, out.normal));
    }
    if (input.tangent.w == 0.0) {
        const float bend = clamp(input.texcoord1.y, 0.0, 1.0);
        const float phase = scene.scene_params.x * 1.35 + world.x * 0.19 + world.z * 0.13;
        const float sway = cos(phase * 0.73) * 0.12 * bend;
        out.normal = normalize(out.normal + float3(-sway, 0.0, sway * 0.45));
    }
    float model_determinant = dot(model_3x3[0], cross(model_3x3[1], model_3x3[2]));
    float skin_determinant = dot(skin_linear[0], cross(skin_linear[1], skin_linear[2]));
    float model_sign = abs(model_determinant) < 0.000001 ? 1.0 : sign(model_determinant);
    float skin_sign = abs(skin_determinant) < 0.000001 ? 1.0 : sign(skin_determinant);
    out.tangent_sign = (input.tangent.w == 0.0 ? 1.0 : input.tangent.w) * model_sign * skin_sign;
    return out;
}

fragment float4 astra_morph_validation_fragment(VertexOut input [[stage_in]]) {
    const float4 current = input.current_clip / max(input.current_clip.w, 1.0e-6);
    const float4 previous = input.previous_clip / max(input.previous_clip.w, 1.0e-6);
    return float4(current.xy, previous.xy);
}

struct ShadowVertexOut {
    float4 position [[position]];
    float2 texcoord0;
    float2 texcoord1;
    float4 color;
};

vertex ShadowVertexOut astra_shadow_vertex(VertexIn input [[stage_in]],
                                           constant SceneUniforms &scene [[buffer(1)]],
                                           constant float4x4 *joint_matrices [[buffer(2)]],
                                           device const float4x4 *instance_models [[buffer(4)]],
                                           device const float *morph_weights [[buffer(6)]],
                                           device const MorphDelta *morph_deltas [[buffer(8)]],
                                           uint vertex_id [[vertex_id]],
                                           uint instance_id [[instance_id]]) {
    float4x4 model_transform = scene.model;
    if (scene.texture_flags2.w > 0.5)
        model_transform = instance_models[instance_id] * scene.model;
    VertexIn morphed_input = input;
    float3 morphed_position;
    float3 morphed_normal;
    float3 morphed_tangent;
    apply_morph_targets(input, vertex_id, instance_id, scene, morph_weights, morph_deltas,
                        morphed_position, morphed_normal, morphed_tangent);
    morphed_input.position = morphed_position;
    morphed_input.normal = morphed_normal;
    morphed_input.tangent.xyz = morphed_tangent;
    float4 local_position = skinned_position(morphed_input, scene, joint_matrices, model_transform);
    float4 world_position = model_transform * local_position;
    if (input.tangent.w == 0.0) {
        const float bend = clamp(input.texcoord1.y, 0.0, 1.0);
        world_position.xyz += foliage_wind_offset(world_position.xyz, bend, scene.scene_params.x);
    }
    ShadowVertexOut out;
    out.position = scene.light_view_projection[uint(scene.scene_params.w)] * world_position;
    out.texcoord0 = input.texcoord0;
    out.texcoord1 = input.texcoord1;
    out.color = input.color;
    return out;
}

fragment float4 astra_morph_validation_shadow_fragment(ShadowVertexOut input [[stage_in]]) {
    return float4(input.position.z, input.texcoord0.x, input.color.r, 1.0);
}

fragment void astra_shadow_fragment(ShadowVertexOut input [[stage_in]],
                                    constant SceneUniforms &scene [[buffer(1)]],
                                    texture2d<float> base_color_map [[texture(0)]],
                                    sampler base_color_sampler [[sampler(0)]]) {
    if (scene.alpha_flags.x > 0.5 && scene.alpha_flags.x < 1.5) {
        const float2 source_uv =
            uint(scene.texcoord_sets0.x) == 0U ? input.texcoord0 : input.texcoord1;
        const float2 uv = apply_uv_transform(source_uv, scene.uv_transform[0],
                                             scene.uv_rotation[0]);
        const float texture_alpha =
            scene.material_flags.x > 0.5 ? base_color_map.sample(base_color_sampler, uv).a : 1.0;
        const float alpha = input.color.a * scene.base_color.a * texture_alpha;
        if (alpha < scene.alpha_flags.y)
            discard_fragment();
    }
}

struct FullscreenOut {
    float4 position [[position]];
    float2 uv;
};

vertex FullscreenOut astra_fullscreen_vertex(uint vertex_id [[vertex_id]]) {
    constexpr float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    FullscreenOut out;
    float2 p = positions[vertex_id];
    out.position = float4(p, 0.0, 1.0);
    out.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

struct SkyUniforms {
    float4x4 inverse_view_projection;
    float4x4 previous_view_projection;
    float4 camera_position;
    float4 previous_camera_position;
    float4 light_direction;
    float4 time_exposure;
    float4 jitter; // current NDC jitter xy, orthographic flag, reserved
    float4 clouds; // base altitude, top altitude, density, coverage
    float4 cloud_wind_steps; // wind xyz, ray step count
    float4 environment_flags; // enabled, reserved, reserved, reserved
};

struct TaaUniforms {
    float4x4 inverse_view_projection;
    float4 camera_position;
    float4 previous_camera_position;
    float4 jitter;
    float4 taa_params; // history weight, history valid, reserved, reserved
};

float hash21(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise2(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float cloud_field(float2 p) {
    float value = 0.0;
    float amplitude = 0.55;
    for (uint octave = 0; octave < 4; ++octave) {
        value += noise2(p) * amplitude;
        p = p * 2.03 + float2(17.1, 9.2);
        amplitude *= 0.5;
    }
    return value;
}

float hash31(float3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float noise3(float3 p) {
    float3 cell = floor(p);
    float3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(cell + float3(0.0, 0.0, 0.0));
    float n100 = hash31(cell + float3(1.0, 0.0, 0.0));
    float n010 = hash31(cell + float3(0.0, 1.0, 0.0));
    float n110 = hash31(cell + float3(1.0, 1.0, 0.0));
    float n001 = hash31(cell + float3(0.0, 0.0, 1.0));
    float n101 = hash31(cell + float3(1.0, 0.0, 1.0));
    float n011 = hash31(cell + float3(0.0, 1.0, 1.0));
    float n111 = hash31(cell + float3(1.0, 1.0, 1.0));
    float z0 = mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
                   mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
    return z0;
}

float cloud_field_direction(float3 direction, float time, float scale) {
    // Sampling a 3D field on the unit sphere avoids longitude seams and makes
    // cloud positions invariant under camera rotation.
    float3 p = direction * scale + float3(time * 0.012, 0.0, -time * 0.006);
    float value = 0.0;
    float amplitude = 0.55;
    // Environment reflections only need broad cloud structure; the primary sky
    // pass performs the detailed volumetric cloud march. Two octaves here avoid
    // repeating dozens of 3D hash operations for every lit geometry fragment.
    for (uint octave = 0; octave < 2; ++octave) {
        value += noise3(p) * amplitude;
        p = p * 2.03 + float3(17.1, 9.2, 5.7);
        amplitude *= 0.5;
    }
    return value;
}

float3 analytic_sky_radiance(float3 direction, float roughness, float3 sun_direction, float time) {
    float3 ray = normalize(direction);
    float blur = clamp(roughness * roughness, 0.0, 1.0);
    float3 broad_ray = normalize(mix(ray, float3(0.0, 0.28, 0.96), blur * 0.82));
    float horizon = smoothstep(-0.12, 0.72, broad_ray.y);
    float3 sky = mix(float3(0.48, 0.57, 0.66), float3(0.075, 0.19, 0.36), horizon);
    float sun_alignment = max(dot(ray, sun_direction), 0.0);
    sky +=
        float3(1.0, 0.72, 0.43) * pow(sun_alignment, mix(180.0, 5.0, blur)) * mix(0.35, 0.08, blur);
    // At full roughness the cloud contribution is exactly zero, so skip its
    // procedural field entirely. The detailed sky/cloud pass remains unchanged.
    if (blur < 0.98) {
        float cloud_a = cloud_field_direction(broad_ray, time, 5.2);
        float cloud_b = cloud_field_direction(broad_ray, time, 9.1);
        float clouds = smoothstep(0.58, 0.82, cloud_a * 0.68 + cloud_b * 0.48);
        sky = mix(sky, float3(0.72, 0.77, 0.81), clouds * (1.0 - blur) * 0.32);
    }
    return sky;
}

struct SceneFragmentOut {
    float4 color [[color(0)]];
    float2 motion [[color(1)]];
    float reactive [[color(2)]];
    float4 oit_accumulation [[color(3)]];
    float4 oit_revealage [[color(4)]];
    float4 indirect_diffuse [[color(5)]];
};

fragment SceneFragmentOut astra_sky_fragment(FullscreenOut input [[stage_in]],
                                             constant SkyUniforms &sky [[buffer(0)]],
                                             texturecube<float> environment_map [[texture(0)]],
                                             sampler environment_sampler [[sampler(0)]]) {
    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    ndc -= sky.jitter.xy;
    // Metal's normalized device depth is [0, 1]. Perspective rays originate at
    // the camera; orthographic rays are parallel and originate on the near plane.
    const float4 near_h = sky.inverse_view_projection * float4(ndc, 0.0, 1.0);
    const float4 far_h = sky.inverse_view_projection * float4(ndc, 1.0, 1.0);
    const float3 near_position = near_h.xyz / max(abs(near_h.w), 0.000001) * sign(near_h.w);
    const float3 far_position = far_h.xyz / max(abs(far_h.w), 0.000001) * sign(far_h.w);
    const bool orthographic = sky.jitter.z > 0.5;
    const float3 ray_origin = orthographic ? near_position : sky.camera_position.xyz;
    const float3 ray = normalize(far_position - near_position);
    float time = sky.time_exposure.x;
    float3 color;
    if (sky.environment_flags.x > 0.5) {
        color = environment_map.sample(environment_sampler, ray).rgb;
    } else {
        const float horizon = smoothstep(-0.08, 0.72, ray.y);
        const float3 zenith = float3(0.075, 0.19, 0.36);
        const float3 horizon_color = float3(0.52, 0.68, 0.82);
        color = mix(horizon_color, zenith, horizon);
        const float sun_alignment = max(dot(ray, normalize(-sky.light_direction.xyz)), 0.0);
        const float sun_glow = pow(sun_alignment, 18.0) * 0.22;
        color += float3(1.0, 0.72, 0.43) * sun_glow;
    }

    float cloud_coverage = 0.0;
    float3 opacity_weighted_position = float3(0.0);
    // A bounded ray march through a world-space cloud slab. Density is sampled
    // in 3D so cloud bodies have depth and parallax instead of being a sky decal.
    if (ray.y > 0.025 && sky.clouds.z > 0.0001) {
        const float cloud_base = sky.clouds.x;
        const float cloud_top = sky.clouds.y;
        const uint cloud_steps = clamp(uint(sky.cloud_wind_steps.w + 0.5), 1U, 12U);
        const float entry = max(0.0, (cloud_base - ray_origin.y) / ray.y);
        const float exit = min((cloud_top - ray_origin.y) / ray.y, 8500.0);
        if (exit > entry) {
            const float step_length = (exit - entry) / float(cloud_steps);
            const float3 wind = time * sky.cloud_wind_steps.xyz;
            const float sun_height =
                max(dot(normalize(-sky.light_direction.xyz), float3(0.0, 1.0, 0.0)), 0.0);
            const float3 lit_cloud =
                mix(float3(0.34, 0.42, 0.52), float3(1.0, 0.91, 0.78), 0.28 + sun_height * 0.72);
            float transmittance = 1.0;
            float3 in_scatter = float3(0.0);
            for (uint step = 0; step < cloud_steps; ++step) {
                const float distance_along_ray = entry + (float(step) + 0.5) * step_length;
                const float3 sample_position = ray_origin + ray * distance_along_ray;
                const float layer_height =
                    (sample_position.y - cloud_base) / (cloud_top - cloud_base);
                const float vertical_profile = smoothstep(0.0, 0.18, layer_height) *
                                               (1.0 - smoothstep(0.72, 1.0, layer_height));
                const float broad_density = noise3(sample_position * 0.00125 + wind);
                const float detail_density = noise2(sample_position.xz * 0.0031 + wind.xz * 1.7);
                const float shape = broad_density * 0.78 + detail_density * 0.22;
                const float coverage_low = 0.54 + (0.55 - sky.clouds.w) * 0.45;
                const float density = smoothstep(coverage_low, min(coverage_low + 0.19, 0.999), shape) *
                                      vertical_profile * sky.clouds.z;
                const float optical_depth = density * step_length * 0.00072;
                const float sample_transmittance = exp(-optical_depth);
                const float opacity = transmittance * (1.0 - sample_transmittance);
                in_scatter += lit_cloud * opacity;
                opacity_weighted_position += sample_position * opacity;
                transmittance *= sample_transmittance;
                if (transmittance < 0.025)
                    break;
            }
            color = color * transmittance + in_scatter;
            cloud_coverage = clamp(1.0 - transmittance, 0.0, 1.0);
        }
    }
    float2 motion = float2(0.0);
    if (sky.time_exposure.z > 0.5) {
        const float4 previous_sky_clip =
            orthographic ? sky.previous_view_projection *
                               float4(sky.previous_camera_position.xyz + ray * 10000.0, 1.0)
                         : sky.previous_view_projection * float4(ray, 0.0);
        if (previous_sky_clip.w > 0.00001) {
            const float2 previous_ndc = previous_sky_clip.xy / previous_sky_clip.w;
            const float2 previous_uv =
                float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
            const float2 sky_motion = input.uv - previous_uv;
            float2 cloud_motion = sky_motion;
            if (cloud_coverage > 0.0001) {
                const float3 representative_cloud_position =
                    opacity_weighted_position / cloud_coverage;
                // Finite cloud samples retain camera-translation parallax.
                const float4 previous_cloud_clip =
                    sky.previous_view_projection * float4(representative_cloud_position, 1.0);
                if (previous_cloud_clip.w > 0.00001) {
                    const float2 previous_cloud_ndc =
                        previous_cloud_clip.xy / previous_cloud_clip.w;
                    const float2 previous_cloud_uv =
                        float2(previous_cloud_ndc.x * 0.5 + 0.5, 0.5 - previous_cloud_ndc.y * 0.5);
                    cloud_motion = input.uv - previous_cloud_uv;
                }
            }
            motion = mix(sky_motion, cloud_motion, cloud_coverage);
        }
    }
    return SceneFragmentOut{float4(color, 1.0), motion, cloud_coverage, float4(0.0), float4(0.0),
                            float4(0.0)};
}

float distribution_ggx(float n_dot_h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denominator = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(M_PI_F * denominator * denominator, 0.0001);
}

float geometry_schlick(float n_dot_v, float roughness) {
    float k = (roughness + 1.0);
    k = (k * k) / 8.0;
    return n_dot_v / max(n_dot_v * (1.0 - k) + k, 0.0001);
}

float3 fresnel_schlick(float cosine, float3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

float shadow_pcf(float4 shadow_position, depth2d<float> shadow_map, sampler shadow_sampler,
                 float3 normal, float3 light_direction) {
    if (shadow_position.w <= 0.0)
        return 1.0;
    float3 projected = shadow_position.xyz / shadow_position.w;
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || projected.z < 0.0 || projected.z > 1.0)
        return 1.0;
    float bias = max(0.00045 * (1.0 - dot(normal, light_direction)), 0.00008);
    float2 texel = 1.0 / float2(shadow_map.get_width(), shadow_map.get_height());
    // Five bilinear comparison taps retain a broad, smooth PCF footprint while
    // avoiding the nine independent samples of a full 3x3 kernel. The center
    // receives slightly more weight to keep contact shadows anchored.
    constexpr float2 offsets[5] = {float2(0.0), float2(-1.0, 0.0), float2(1.0, 0.0),
                                   float2(0.0, -1.0), float2(0.0, 1.0)};
    constexpr float weights[5] = {0.25, 0.1875, 0.1875, 0.1875, 0.1875};
    float visibility = 0.0;
    for (uint tap = 0; tap < 5; ++tap) {
        const float2 tap_uv = uv + offsets[tap] * texel;
        // Outside the map is unoccluded; do not smear edge texels outward.
        const float sample_visibility =
            any(tap_uv < 0.0) || any(tap_uv > 1.0)
                ? 1.0
                : shadow_map.sample_compare(shadow_sampler, tap_uv, projected.z - bias);
        visibility += sample_visibility * weights[tap];
    }
    return visibility;
}

float cascade_shadow_visibility(float3 world_position, uint cascade, constant SceneUniforms &scene,
                                depth2d<float> shadow_map, sampler shadow_sampler, float3 normal,
                                float3 light_direction) {
    const float4 shadow_position =
        scene.light_view_projection[cascade] * float4(world_position, 1.0);
    return shadow_pcf(shadow_position, shadow_map, shadow_sampler, normal, light_direction);
}

struct SceneFragmentResources {
    texture2d<float> base_color_map [[id(0)]];
    texture2d<float> metallic_roughness_map [[id(1)]];
    texture2d<float> normal_map [[id(2)]];
    texture2d<float> occlusion_map [[id(3)]];
    texture2d<float> emissive_map [[id(4)]];
    depth2d<float> shadow_map0 [[id(5)]];
    depth2d<float> shadow_map1 [[id(6)]];
    depth2d<float> shadow_map2 [[id(7)]];
    texturecube<float> irradiance_map [[id(8)]];
    texturecube<float> prefiltered_environment [[id(9)]];
    texture2d<float> brdf_lut [[id(10)]];
    sampler base_color_sampler [[id(11)]];
    sampler metallic_roughness_sampler [[id(12)]];
    sampler normal_sampler [[id(13)]];
    sampler occlusion_sampler [[id(14)]];
    sampler emissive_sampler [[id(15)]];
    sampler shadow_sampler0 [[id(16)]];
    sampler shadow_sampler1 [[id(17)]];
    sampler shadow_sampler2 [[id(18)]];
    sampler environment_sampler [[id(19)]];
};

fragment SceneFragmentOut astra_fragment(
    VertexOut input [[stage_in]], constant SceneUniforms &scene [[buffer(1)]],
    constant SceneFragmentResources &resources [[buffer(2)]],
    constant PointLight *point_lights [[buffer(3)]],
    bool front_facing [[front_facing]]) {
    float2 base_color_uv = uint(scene.texcoord_sets0.x) == 0U ? input.texcoord0 : input.texcoord1;
    float2 metallic_roughness_uv =
        uint(scene.texcoord_sets0.y) == 0U ? input.texcoord0 : input.texcoord1;
    float2 normal_uv = uint(scene.texcoord_sets0.z) == 0U ? input.texcoord0 : input.texcoord1;
    float2 occlusion_uv = uint(scene.texcoord_sets0.w) == 0U ? input.texcoord0 : input.texcoord1;
    float2 emissive_uv = uint(scene.texcoord_sets1.x) == 0U ? input.texcoord0 : input.texcoord1;
    base_color_uv = apply_uv_transform(base_color_uv, scene.uv_transform[0], scene.uv_rotation[0]);
    metallic_roughness_uv =
        apply_uv_transform(metallic_roughness_uv, scene.uv_transform[1], scene.uv_rotation[1]);
    normal_uv = apply_uv_transform(normal_uv, scene.uv_transform[2], scene.uv_rotation[2]);
    occlusion_uv = apply_uv_transform(occlusion_uv, scene.uv_transform[3], scene.uv_rotation[3]);
    emissive_uv = apply_uv_transform(emissive_uv, scene.uv_transform[4], scene.uv_rotation[4]);
    float3 n = normalize(input.normal);
    if (scene.material_flags.z > 0.5) {
        float3 t = normalize(input.tangent - n * dot(n, input.tangent));
        float3 b = normalize(cross(n, t)) * input.tangent_sign;
        float3 mapped = resources.normal_map.sample(resources.normal_sampler, normal_uv).xyz * 2.0 - 1.0;
        mapped.xy *= scene.texture_flags2.y;
        n = normalize(t * mapped.x + b * mapped.y + n * mapped.z);
    }
    if (scene.alpha_flags.z > 0.5 && !front_facing)
        n = -n;
    float3 v = normalize(scene.camera_position.xyz - input.world_position);
    float3 l = normalize(-scene.light_direction.xyz);
    float3 h = normalize(v + l);
    float n_dot_l = max(dot(n, l), 0.0);
    float n_dot_v = max(dot(n, v), 0.001);
    float n_dot_h = max(dot(n, h), 0.0);
    float h_dot_v = max(dot(h, v), 0.0);

    float4 base_sample = scene.material_flags.x > 0.5
                             ? resources.base_color_map.sample(resources.base_color_sampler, base_color_uv)
                             : float4(1.0);
    float4 mr_sample =
        scene.material_flags.y > 0.5
            ? resources.metallic_roughness_map.sample(resources.metallic_roughness_sampler, metallic_roughness_uv)
            : float4(1.0);
    const float alpha = input.color.a * scene.base_color.a * base_sample.a;
    if (scene.alpha_flags.x > 0.5 && scene.alpha_flags.x < 1.5 && alpha < scene.alpha_flags.y)
        discard_fragment();
    float metallic = clamp(scene.material.x * mr_sample.b, 0.0, 1.0);
    float roughness = clamp(scene.material.y * mr_sample.g, 0.045, 1.0);
    float3 albedo = max(input.color.rgb * scene.base_color.rgb * base_sample.rgb, 0.0);
    float3 f0 = mix(float3(0.04), albedo, metallic);
    float3 fresnel = fresnel_schlick(h_dot_v, f0);
    float distribution = distribution_ggx(n_dot_h, roughness);
    float geometry = geometry_schlick(n_dot_v, roughness) * geometry_schlick(n_dot_l, roughness);
    float3 specular =
        distribution * geometry * fresnel / max(4.0 * n_dot_v * max(n_dot_l, 0.001), 0.001);
    float3 diffuse_weight = (1.0 - fresnel) * (1.0 - metallic);
    const float camera_distance =
        dot(input.world_position - scene.camera_position.xyz, scene.cascade_camera_forward.xyz);
    uint cascade = camera_distance <= scene.cascade_splits.x   ? 0U
                   : camera_distance <= scene.cascade_splits.y ? 1U
                                                               : 2U;
    float visibility = cascade == 0U ? cascade_shadow_visibility(input.world_position, 0U, scene,
                                                                 resources.shadow_map0, resources.shadow_sampler0, n, l)
                       : cascade == 1U
                           ? cascade_shadow_visibility(input.world_position, 1U, scene, resources.shadow_map1,
                                                       resources.shadow_sampler1, n, l)
                           : cascade_shadow_visibility(input.world_position, 2U, scene, resources.shadow_map2,
                                                       resources.shadow_sampler2, n, l);
    const float cascade_start = cascade == 0U ? 0.0F : scene.cascade_splits[cascade - 1U];
    const float cascade_end = scene.cascade_splits[cascade];
    const float blend_start = mix(cascade_start, cascade_end, 0.90);
    if (cascade < 2U && camera_distance > blend_start) {
        const float next_visibility =
            cascade == 0U ? cascade_shadow_visibility(input.world_position, 1U, scene, resources.shadow_map1,
                                                      resources.shadow_sampler1, n, l)
                          : cascade_shadow_visibility(input.world_position, 2U, scene, resources.shadow_map2,
                                                      resources.shadow_sampler2, n, l);
        const float blend = smoothstep(blend_start, cascade_end, camera_distance);
        visibility = mix(visibility, next_visibility, blend);
    }
    float3 direct = (diffuse_weight * albedo / M_PI_F + specular) *
                    (scene.light_radiance.xyz * n_dot_l * visibility);

    // Local emitters use radiometric intensity (W/sr for meter-sized world units), inverse-square
    // attenuation, and a smooth finite-range cutoff. They do not inherit the
    // directional sun's cascaded shadow visibility.
    const uint point_light_count = min(scene.morph_flags.w, 32U);
    for (uint light_index = 0U; light_index < point_light_count; ++light_index) {
        const PointLight light = point_lights[light_index];
        const float3 to_light = light.position_range.xyz - input.world_position;
        const float distance_squared = max(dot(to_light, to_light), 1.0e-4);
        const float distance_to_light = sqrt(distance_squared);
        const float range = light.position_range.w;
        if (distance_to_light >= range)
            continue;
        const float3 local_l = to_light / distance_to_light;
        const float3 local_h = normalize(v + local_l);
        const float local_n_dot_l = max(dot(n, local_l), 0.0);
        if (local_n_dot_l <= 0.0)
            continue;
        const float local_n_dot_h = max(dot(n, local_h), 0.0);
        const float local_h_dot_v = max(dot(local_h, v), 0.0);
        const float3 local_fresnel = fresnel_schlick(local_h_dot_v, f0);
        const float local_distribution = distribution_ggx(local_n_dot_h, roughness);
        const float local_geometry = geometry_schlick(n_dot_v, roughness) *
                                     geometry_schlick(local_n_dot_l, roughness);
        const float3 local_specular = local_distribution * local_geometry * local_fresnel /
            max(4.0 * n_dot_v * max(local_n_dot_l, 0.001), 0.001);
        const float range_fade = 1.0 - smoothstep(range * 0.8, range, distance_to_light);
        const float3 irradiance = light.intensity.xyz * (range_fade * range_fade / distance_squared);
        const float3 local_diffuse_weight = (1.0 - local_fresnel) * (1.0 - metallic);
        direct += (local_diffuse_weight * albedo / M_PI_F + local_specular) * irradiance * local_n_dot_l;
    }

    float3 sun_direction = normalize(-scene.light_direction.xyz);
    float3 reflected_direction = reflect(-v, n);
    float ao = scene.material_flags.w > 0.5
                   ? mix(1.0, resources.occlusion_map.sample(resources.occlusion_sampler, occlusion_uv).r,
                         clamp(scene.texture_flags2.z, 0.0, 1.0))
                   : 1.0;
    float3 ambient_diffuse;
    float3 ambient_specular;
    if (scene.environment_flags.x > 0.5) {
        // Diffuse irradiance is cosine-convolved during environment creation.
        const float3 diffuse_irradiance = resources.irradiance_map.sample(resources.environment_sampler, n).rgb;
        const float3 prefiltered = resources.prefiltered_environment.sample(
            resources.environment_sampler, reflected_direction,
            level(roughness * scene.environment_flags.y)).rgb;
        const float2 integrated_brdf =
            resources.brdf_lut.sample(resources.environment_sampler, float2(n_dot_v, roughness)).rg;
        ambient_diffuse = diffuse_irradiance * albedo * (1.0 - metallic) * ao;
        ambient_specular = prefiltered * (f0 * integrated_brdf.x + integrated_brdf.y) * ao;
    } else {
        // Analytic sky remains the compatibility fallback when no environment is assigned.
        const float3 diffuse_environment =
            analytic_sky_radiance(n, 1.0, sun_direction, scene.scene_params.x);
        const float3 specular_environment = analytic_sky_radiance(
            reflected_direction, roughness, sun_direction, scene.scene_params.x);
        const float3 environment_fresnel = fresnel_schlick(n_dot_v, f0);
        ambient_diffuse = diffuse_environment * albedo * (1.0 - metallic) * ao * 0.42;
        ambient_specular =
            specular_environment * environment_fresnel * (1.0 - 0.55 * roughness) * ao * 0.32;
    }
    float3 ambient = ambient_diffuse + ambient_specular;
    float3 emissive = scene.emissive.rgb;
    if (scene.texture_flags2.x > 0.5)
        emissive *= resources.emissive_map.sample(resources.emissive_sampler, emissive_uv).rgb;
    float3 color = direct + ambient + emissive;

    const float3 camera_to_surface = input.world_position - scene.camera_position.xyz;
    const float distance_to_camera = length(camera_to_surface);
    const float3 fog_ray = camera_to_surface / max(distance_to_camera, 0.0001);
    const float fog_density = max(scene.scene_params.z, 0.0001);
    const float fog_step_length = distance_to_camera / 3.0;
    const float3 fog_sun = normalize(-scene.light_direction.xyz);
    const float forward_scattering = pow(max(dot(fog_ray, fog_sun), 0.0), 8.0) * 0.16;
    float fog_transmittance = 1.0;
    float3 fog_scattering = float3(0.0);
    // Integrate height-varying participating media along the camera ray.
    // Three bounded samples keep the cost predictable on integrated GPUs.
    for (uint fog_step = 0; fog_step < 3; ++fog_step) {
        const float ray_distance = (float(fog_step) + 0.5) * fog_step_length;
        const float3 sample_position = scene.camera_position.xyz + fog_ray * ray_distance;
        const float height_delta = max(sample_position.y - scene.camera_position.y, 0.0);
        const float height_density = exp(-height_delta * 0.035);
        const float density_noise = mix(0.82, 1.18, noise2(sample_position.xz * 0.0031));
        const float local_density = fog_density * height_density * density_noise;
        const float optical_depth = local_density * fog_step_length;
        const float sample_transmittance = exp(-optical_depth);
        const float opacity = fog_transmittance * (1.0 - sample_transmittance);
        const float altitude = clamp(sample_position.y * 0.004 + 0.5, 0.0, 1.0);
        const float3 ambient_fog =
            mix(float3(0.43, 0.58, 0.70), float3(0.20, 0.34, 0.50), altitude);
        const float3 lit_fog = ambient_fog * (1.0 + forward_scattering);
        fog_scattering += lit_fog * opacity;
        fog_transmittance *= sample_transmittance;
    }
    fog_transmittance = max(fog_transmittance, 0.06);
    color = color * fog_transmittance + fog_scattering;
    float2 motion = float2(0.0);
    if (scene.temporal_params.x > 0.5 && input.current_clip.w > 0.00001 &&
        input.previous_clip.w > 0.00001) {
        const float2 current_ndc = input.current_clip.xy / input.current_clip.w;
        const float2 previous_ndc = input.previous_clip.xy / input.previous_clip.w;
        const float2 current_uv = float2(current_ndc.x * 0.5 + 0.5, 0.5 - current_ndc.y * 0.5);
        const float2 previous_uv = float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
        motion = current_uv - previous_uv;
    }
    const float reactive = scene.alpha_flags.x > 1.5 ? 1.0 : 0.0;
    // Weighted blended OIT: accumulation is additive and revealage is
    // multiplicative. The opaque pipeline masks these two outputs; the
    // transparent pipeline masks color/motion/reactive and enables OIT MRTs.
    const float oit_weight = clamp(
        pow(min(1.0, alpha * 10.0) + 0.01, 3.0) * 1.0e8 *
            pow(1.0 - input.position.z * 0.9, 3.0),
        1.0e-2, 3.0e3);
    const float4 oit_accumulation = float4(color * alpha, alpha) * oit_weight;
    const float4 oit_revealage = float4(alpha);
    return SceneFragmentOut{float4(color, alpha), motion, reactive,
                            oit_accumulation, oit_revealage,
                            float4(ambient_diffuse * fog_transmittance, 0.0)};
}

struct GtaoUniforms {
    float4x4 inverse_view_projection;
    float4 camera_position;
    float4 parameters; // radius in world units, strength, bias, reserved
    float4 jitter; // current NDC projection jitter xy
};

float3 gtao_reconstruct_world(depth2d<float> depth_texture, uint2 pixel,
                              uint2 dimensions, constant GtaoUniforms &uniforms) {
    const float depth = depth_texture.read(pixel);
    const float2 uv = (float2(pixel) + 0.5) / float2(dimensions);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - uniforms.jitter.xy;
    const float4 world_h = uniforms.inverse_view_projection * float4(ndc, depth, 1.0);
    return world_h.xyz / max(abs(world_h.w), 1.0e-6) * sign(world_h.w);
}

fragment float4 astra_gtao_composite_fragment(
    FullscreenOut input [[stage_in]], texture2d<float> hdr_color [[texture(0)]],
    texture2d<float> indirect_diffuse [[texture(1)]], depth2d<float> scene_depth [[texture(2)]],
    constant GtaoUniforms &uniforms [[buffer(0)]]) {
    const uint2 dimensions = uint2(scene_depth.get_width(), scene_depth.get_height());
    const uint2 pixel = min(uint2(input.position.xy), dimensions - 1U);
    const float3 hdr = hdr_color.read(pixel).rgb;
    const float3 indirect = indirect_diffuse.read(pixel).rgb;
    const float center_depth = scene_depth.read(pixel);
    if (center_depth >= 0.999999 || uniforms.parameters.x <= 0.0)
        return float4(hdr, 1.0);

    const float3 center = gtao_reconstruct_world(scene_depth, pixel, dimensions, uniforms);
    const uint2 x_before = uint2(pixel.x > 0U ? pixel.x - 1U : pixel.x, pixel.y);
    const uint2 x_after = uint2(min(pixel.x + 1U, dimensions.x - 1U), pixel.y);
    const uint2 y_before = uint2(pixel.x, pixel.y > 0U ? pixel.y - 1U : pixel.y);
    const uint2 y_after = uint2(pixel.x, min(pixel.y + 1U, dimensions.y - 1U));
    const float x_before_depth = scene_depth.read(x_before);
    const float x_after_depth = scene_depth.read(x_after);
    const float y_before_depth = scene_depth.read(y_before);
    const float y_after_depth = scene_depth.read(y_after);
    const uint2 x_sample = x_before_depth < 0.999999 &&
                                   (x_after_depth >= 0.999999 ||
                                    abs(x_before_depth - center_depth) < abs(x_after_depth - center_depth))
                               ? x_before : x_after;
    const uint2 y_sample = y_before_depth < 0.999999 &&
                                   (y_after_depth >= 0.999999 ||
                                    abs(y_before_depth - center_depth) < abs(y_after_depth - center_depth))
                               ? y_before : y_after;
    const float3 dx = gtao_reconstruct_world(scene_depth, x_sample, dimensions, uniforms) - center;
    const float3 dy = gtao_reconstruct_world(scene_depth, y_sample, dimensions, uniforms) - center;
    const float3 normal_cross = cross(dx, dy);
    float3 normal = dot(normal_cross, normal_cross) > 1.0e-8
                        ? normalize(normal_cross)
                        : normalize(uniforms.camera_position.xyz - center);
    if (dot(normal, uniforms.camera_position.xyz - center) < 0.0)
        normal = -normal;

    constexpr uint direction_count = 8U;
    constexpr uint steps_per_direction = 6U;
    constexpr float two_pi = 6.28318530718;
    float occlusion = 0.0;
    const float radius = uniforms.parameters.x;
    // Projection-independent 60-degree perspective approximation, capped to
    // keep the bounded screen-space search local at very close distances.
    const float pixel_radius = min(radius * 0.8660254 * float(dimensions.y) /
                                       max(length(center - uniforms.camera_position.xyz), 0.01),
                                   64.0);
    for (uint direction_index = 0U; direction_index < direction_count; ++direction_index) {
        const float angle = (float(direction_index) + 0.5) * (two_pi / float(direction_count));
        const float2 direction = float2(cos(angle), sin(angle));
        float horizon = -1.0;
        for (uint step = 1U; step <= steps_per_direction; ++step) {
            const float fraction = float(step) / float(steps_per_direction);
            const int2 offset = int2(round(direction * fraction * pixel_radius));
            const int2 sample_position = clamp(int2(pixel) + offset, int2(0), int2(dimensions) - 1);
            const uint2 sample_pixel = uint2(sample_position);
            if (scene_depth.read(sample_pixel) >= 0.999999)
                continue;
            const float3 delta = gtao_reconstruct_world(scene_depth, sample_pixel, dimensions, uniforms) - center;
            const float distance_to_sample = length(delta);
            if (distance_to_sample <= 1.0e-4 || distance_to_sample >= radius)
                continue;
            const float elevation = dot(normal, delta / distance_to_sample) - uniforms.parameters.z;
            const float range_weight = 1.0 - smoothstep(radius * 0.65, radius, distance_to_sample);
            horizon = max(horizon, elevation * range_weight);
        }
        occlusion += max(horizon, 0.0);
    }
    const float ao = clamp(1.0 - uniforms.parameters.y * occlusion / float(direction_count), 0.0, 1.0);
    const float3 indirect_after_ao = indirect * ao;
    return float4(hdr - indirect + indirect_after_ao, 1.0);
}

struct TaaFragmentOut {
    float4 color [[color(0)]];
    float depth [[color(1)]];
};

fragment TaaFragmentOut astra_taa_fragment(
    FullscreenOut input [[stage_in]], texture2d<float> current_color [[texture(0)]],
    texture2d<float> motion_vectors [[texture(1)]], depth2d<float> current_depth [[texture(2)]],
    texture2d<float> history_color [[texture(3)]], texture2d<float> history_depth [[texture(4)]],
    texture2d<float> reactive_mask [[texture(5)]],
    texture2d<float> oit_accumulation [[texture(6)]],
    texture2d<float> oit_revealage [[texture(7)]],
    sampler linear_clamp_sampler [[sampler(0)]], constant TaaUniforms &taa_uniforms [[buffer(0)]]) {
    const uint2 dimensions = uint2(current_color.get_width(), current_color.get_height());
    const uint2 pixel = min(uint2(input.position.xy), dimensions - 1U);
    const float2 uv = input.uv;
    float4 current = current_color.sample(linear_clamp_sampler, uv);
    const float4 accumulation = oit_accumulation.read(pixel);
    const float revealage = clamp(oit_revealage.read(pixel).r, 0.0, 1.0);
    const float transparent_coverage = 1.0 - revealage;
    if (transparent_coverage > 0.0001) {
        const float3 transparent_color = accumulation.rgb / max(accumulation.a, 0.00001);
        current.rgb = mix(current.rgb, transparent_color, transparent_coverage);
    }
    const float2 motion = motion_vectors.read(pixel).rg;
    const float2 history_uv = uv - motion;
    const float device_depth = current_depth.read(pixel);
    const float2 current_ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - taa_uniforms.jitter.xy;
    float current_distance = 1.0e20;
    float previous_expected_distance = 1.0e20;
    if (device_depth < 0.999999) {
        const float4 world_h =
            taa_uniforms.inverse_view_projection * float4(current_ndc, device_depth, 1.0);
        const float3 world_position = world_h.xyz / max(abs(world_h.w), 0.000001) * sign(world_h.w);
        current_distance = length(world_position - taa_uniforms.camera_position.xyz);
        // Compare against the expected distance from the previous camera to
        // this same world point, not the current camera's distance. Otherwise
        // ordinary camera translation rejects nearly all otherwise-valid history.
        previous_expected_distance =
            length(world_position - taa_uniforms.previous_camera_position.xyz);
    }
    bool accept_history = taa_uniforms.taa_params.y > 0.5 && all(history_uv >= float2(0.0)) &&
                          all(history_uv <= float2(1.0));
    if (accept_history) {
        // Depth must not interpolate across unrelated surfaces at silhouettes.
        const uint2 history_pixel =
            min(uint2(clamp(history_uv, 0.0, 1.0) * float2(dimensions)), dimensions - 1U);
        const float previous_distance = history_depth.read(history_pixel).r;
        const float depth_tolerance = max(0.025, previous_expected_distance * 0.001);
        accept_history = abs(previous_distance - previous_expected_distance) <= depth_tolerance;
    }

    float3 resolved = current.rgb;
    if (accept_history) {
        float3 neighborhood_min = float3(1.0e20);
        float3 neighborhood_max = float3(-1.0e20);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const int2 sample_position =
                    clamp(int2(pixel) + int2(x, y), int2(0), int2(dimensions) - 1);
                const uint2 neighbor_pixel = uint2(sample_position);
                float3 sample_color = current_color.read(neighbor_pixel).rgb;
                const float4 neighbor_accumulation = oit_accumulation.read(neighbor_pixel);
                const float neighbor_coverage =
                    1.0 - clamp(oit_revealage.read(neighbor_pixel).r, 0.0, 1.0);
                if (neighbor_coverage > 0.0001) {
                    const float3 neighbor_transparent =
                        neighbor_accumulation.rgb / max(neighbor_accumulation.a, 0.00001);
                    sample_color = mix(sample_color, neighbor_transparent, neighbor_coverage);
                }
                neighborhood_min = min(neighborhood_min, sample_color);
                neighborhood_max = max(neighborhood_max, sample_color);
            }
        }
        const float3 clamp_expansion = (neighborhood_max - neighborhood_min) * 0.08 + 0.0005;
        const float3 history =
            clamp(history_color.sample(linear_clamp_sampler, history_uv).rgb,
                  neighborhood_min - clamp_expansion, neighborhood_max + clamp_expansion);
        const float motion_pixels = length(motion * float2(dimensions));
        float temporal_weight = clamp(taa_uniforms.taa_params.x, 0.0, 0.97) *
                                (1.0 - smoothstep(0.5, 24.0, motion_pixels));
        // Transparent layers and moving cloud coverage are intentionally reactive.
        const float reactive = max(reactive_mask.read(pixel).r, transparent_coverage);
        temporal_weight *= 1.0 - clamp(reactive, 0.0, 1.0) * 0.97;
        resolved = mix(current.rgb, history, temporal_weight);
    }
    return TaaFragmentOut{float4(resolved, current.a), current_distance};
}

fragment float4 astra_tonemap_fragment(FullscreenOut input [[stage_in]],
                                       texture2d<float> hdr_color [[texture(0)]],
                                       sampler hdr_sampler [[sampler(0)]],
                                       constant float4 &post_params [[buffer(0)]]) {
    float3 hdr = hdr_color.sample(hdr_sampler, input.uv).rgb * max(post_params.x, 0.0);
    float3 mapped = (hdr * (2.51 * hdr + 0.03)) / (hdr * (2.43 * hdr + 0.59) + 0.14);
    return float4(clamp(mapped, 0.0, 1.0), 1.0);
}
