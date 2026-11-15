#include <astra/rhi/rhi.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

int main() {
    using namespace astra::rhi;

    static_assert(std::is_aggregate_v<DrawItem>);
    static_assert(std::is_aggregate_v<SceneSettings>);
    // Preserve the pre-temporal positional aggregate prefixes.
    const DrawItem legacy_draw{nullptr, {}, 0.0F, 0.72F, {}, false, {}, nullptr, {}};
    assert(!legacy_draw.has_previous_transform);
    assert(legacy_draw.previous_joint_matrices.empty());
    const SceneSettings legacy_settings{{}, {}, {}, {}, 0.0F, 1.0F};
    assert(!legacy_settings.has_previous_view_projection);
    assert(legacy_settings.previous_view_projection.values[0] == 1.0F);
    assert(legacy_settings.previous_camera_position.x == 0.0F);
    assert(legacy_settings.previous_camera_position.y == 0.0F);
    assert(legacy_settings.previous_camera_position.z == 0.0F);
    assert(validate_scene_lighting(SceneSettings{}));
    auto gtao_settings = SceneSettings{};
    assert(gtao_settings.gtao.enabled);
    assert(validate_scene_lighting(gtao_settings));
    gtao_settings.gtao.radius = -0.1F;
    assert(!validate_scene_lighting(gtao_settings));
    gtao_settings = SceneSettings{};
    gtao_settings.gtao.strength = std::numeric_limits<float>::infinity();
    assert(!validate_scene_lighting(gtao_settings));
    gtao_settings = SceneSettings{};
    gtao_settings.gtao.bias = 0.6F;
    assert(!validate_scene_lighting(gtao_settings));
    const std::array<PointLight, 1> valid_point_lights{{
        {.position = {1.0F, 2.0F, 3.0F}, .range = 12.0F, .intensity = {18.0F, 14.0F, 10.0F}}}};
    auto local_lighting = SceneSettings{};
    local_lighting.point_lights = valid_point_lights;
    assert(validate_scene_lighting(local_lighting));
    auto invalid_point_light = valid_point_lights;
    invalid_point_light[0].range = 0.0F;
    local_lighting.point_lights = invalid_point_light;
    assert(!validate_scene_lighting(local_lighting));
    invalid_point_light = valid_point_lights;
    invalid_point_light[0].intensity.y = -1.0F;
    local_lighting.point_lights = invalid_point_light;
    assert(!validate_scene_lighting(local_lighting));
    const std::array<PointLight, maximum_scene_point_lights + 1> too_many_point_lights{};
    local_lighting.point_lights = too_many_point_lights;
    assert(!validate_scene_lighting(local_lighting));
    auto invalid_lighting = SceneSettings{};
    invalid_lighting.directional_light_radiance = {1.0F, -0.1F, 1.0F};
    assert(!validate_scene_lighting(invalid_lighting));
    invalid_lighting = SceneSettings{};
    invalid_lighting.light_direction = {};
    assert(!validate_scene_lighting(invalid_lighting));
    assert(validate_volumetric_clouds(VolumetricCloudSettings{}));
    const std::array<Vec3, 3> morph_positions{{{0.0F, 0.0F, 0.1F},
                                               {0.0F, 0.0F, 0.1F},
                                               {0.0F, 0.0F, 0.1F}}};
    const std::array<Vec3, 3> morph_normals{{{0.0F, 0.1F, 0.0F},
                                             {0.0F, 0.1F, 0.0F},
                                             {0.0F, 0.1F, 0.0F}}};
    const std::array<MeshData::MorphTarget, 2> morph_targets{{
        {.positions = morph_positions},
        {.normals = morph_normals},
    }};
    assert(validate_morph_targets(3, morph_targets));
    const std::array<MeshData::MorphTarget, 1> missing_attributes{{{}}};
    assert(!validate_morph_targets(3, missing_attributes));
    const std::array<Vec3, 2> wrong_morph_count{};
    const std::array<MeshData::MorphTarget, 1> wrong_morph{{{.positions = wrong_morph_count}}};
    assert(!validate_morph_targets(3, wrong_morph));
    const std::array<Vec3, 1> non_finite_morph{{{NAN, 0.0F, 0.0F}}};
    const std::array<MeshData::MorphTarget, 1> invalid_morph{{{.positions = non_finite_morph}}};
    assert(!validate_morph_targets(1, invalid_morph));
    const std::vector<MeshData::MorphTarget> too_many_morphs(257);
    assert(!validate_morph_targets(3, too_many_morphs));
    assert(!validate_morph_targets(0, morph_targets));
    const std::array<float, 2> morph_target_radii{1.0F, 3.0F};
    const std::array<float, 2> morph_weights{0.5F, -2.0F};
    assert(std::abs(morph_displacement_bound(morph_target_radii, morph_weights) - 6.5F) < 1.0e-5F);
    assert(std::isinf(morph_displacement_bound(morph_target_radii,
                                                std::span<const float>{morph_weights.data(), 1})));
    const std::array<float, 2> non_finite_weights{0.5F, NAN};
    assert(!morph_weights_are_finite(non_finite_weights));
    assert(std::isinf(morph_displacement_bound(morph_target_radii, non_finite_weights)));
    const std::array<float, 2> invalid_target_radii{
        1.0F, std::numeric_limits<float>::infinity()};
    const std::array<float, 2> zero_morph_weights{0.0F, 0.0F};
    assert(std::isinf(morph_displacement_bound(invalid_target_radii, zero_morph_weights)));
    const std::array<float, 6> instance_current{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F};
    const std::array<float, 6> instance_previous{0.0F, 0.1F, 0.2F, 0.3F, 0.4F, 0.5F};
    const std::array<std::uint32_t, 2> visible_instances{2U, 0U};
    std::vector<float> compacted_current;
    std::vector<float> compacted_previous;
    assert(compact_instance_morph_weights(instance_current, instance_previous,
                                          visible_instances, 2, compacted_current,
                                          &compacted_previous));
    assert((compacted_current == std::vector<float>{0.5F, 0.6F, 0.1F, 0.2F}));
    assert((compacted_previous == std::vector<float>{0.4F, 0.5F, 0.0F, 0.1F}));
    compacted_current.clear();
    assert(compact_instance_morph_weights(instance_current, {}, visible_instances, 2,
                                          compacted_current));
    assert((compacted_current == std::vector<float>{0.5F, 0.6F, 0.1F, 0.2F}));
    const std::array<std::uint32_t, 1> invalid_visible_instance{3U};
    assert(!compact_instance_morph_weights(instance_current, {}, invalid_visible_instance,
                                           2, compacted_current));
    const auto previous_if_valid = previous_frame_span(
        std::span<const float>{instance_current.data(), 2},
        std::span<const float>{instance_previous.data(), 2}, true);
    const auto current_if_invalid = previous_frame_span(
        std::span<const float>{instance_current.data(), 2},
        std::span<const float>{instance_previous.data(), 2}, false);
    assert(previous_if_valid.data() == instance_previous.data());
    assert(current_if_invalid.data() == instance_current.data());
    const auto current_if_missing = previous_frame_span(
        std::span<const float>{instance_current.data(), 2}, {}, true);
    assert(current_if_missing.data() == instance_current.data());
    auto invalid_clouds = VolumetricCloudSettings{};
    invalid_clouds.top_altitude = invalid_clouds.base_altitude;
    assert(!validate_volumetric_clouds(invalid_clouds));
    const std::array<float, 16> environment_pixels{
        1.0F, 0.5F, 0.25F, 1.0F, 0.0F, 0.1F, 0.2F, 1.0F,
        0.2F, 0.3F, 0.4F, 1.0F, 0.7F, 0.8F, 0.9F, 1.0F};
    EnvironmentData environment{.width = 2, .height = 2, .rgba32f = environment_pixels};
    assert(validate_environment(environment));
    environment.prefilter_mip_count = 9;
    assert(!validate_environment(environment));
    environment.prefilter_mip_count = 5;
    environment.rgba32f = std::span<const float>{environment_pixels.data(), 15};
    assert(!validate_environment(environment));
    invalid_clouds = VolumetricCloudSettings{};
    invalid_clouds.ray_steps = 13;
    assert(!validate_volumetric_clouds(invalid_clouds));

    const std::vector<MeshVertex> virtual_vertices(3);
    const std::vector<std::uint32_t> virtual_indices{0, 1, 2};
    const std::vector<VirtualGeometryCluster> virtual_clusters{
        {.bounds_sphere = {0.0F, 0.0F, 0.0F, 1.0F},
         .geometric_error = 0.0F,
         .first_index = 0,
         .index_count = 3,
         .base_vertex = 0,
         .flags = 1U},
    };
    const VirtualGeometryData virtual_data{virtual_vertices, virtual_indices, virtual_clusters};
    assert(validate_virtual_geometry(virtual_data));

    // A valid forest may contain several independent roots, each with its own
    // descendants. Every cluster must still resolve to exactly one root.
    auto forest_clusters = virtual_clusters;
    forest_clusters.push_back(virtual_clusters[0]);
    forest_clusters.push_back(virtual_clusters[0]);
    forest_clusters[1].parent_index = 2U;
    forest_clusters[2].parent_index = std::numeric_limits<std::uint32_t>::max();
    forest_clusters[2].flags = 0U;
    const VirtualGeometryData forest_data{virtual_vertices, virtual_indices, forest_clusters};
    assert(validate_virtual_geometry(forest_data));

    // A leaf cluster cannot be used as a parent, even in an otherwise valid tree.
    auto leaf_parent_clusters = forest_clusters;
    leaf_parent_clusters[2].flags = 1U;
    const VirtualGeometryData leaf_parent_data{virtual_vertices, virtual_indices,
                                               leaf_parent_clusters};
    assert(!validate_virtual_geometry(leaf_parent_data));

    // Reject a cycle even when a separate, otherwise-valid root tree exists.
    auto cyclic_clusters = forest_clusters;
    cyclic_clusters[1].parent_index = 2U;
    cyclic_clusters[2].parent_index = 1U;
    const VirtualGeometryData cyclic_data{virtual_vertices, virtual_indices, cyclic_clusters};
    assert(!validate_virtual_geometry(cyclic_data));

    // Reject a parent cycle when it is the entire hierarchy (no roots).
    auto rootless_cycle = virtual_clusters;
    rootless_cycle.push_back(virtual_clusters[0]);
    rootless_cycle[0].parent_index = 1U;
    rootless_cycle[1].parent_index = 0U;
    const VirtualGeometryData rootless_cycle_data{virtual_vertices, virtual_indices,
                                                  rootless_cycle};
    assert(!validate_virtual_geometry(rootless_cycle_data));

    // Reject disconnected cyclic components rather than accepting the valid
    // root tree and silently leaving the remaining clusters unreachable.
    auto disconnected_cycle = forest_clusters;
    disconnected_cycle[1].parent_index = 2U;
    disconnected_cycle[2].parent_index = 1U;
    disconnected_cycle.push_back(virtual_clusters[0]);
    const VirtualGeometryData disconnected_cycle_data{virtual_vertices, virtual_indices,
                                                      disconnected_cycle};
    assert(!validate_virtual_geometry(disconnected_cycle_data));

    auto invalid_virtual_clusters = virtual_clusters;
    invalid_virtual_clusters[0].index_count = 4;
    const VirtualGeometryData invalid_virtual_data{virtual_vertices, virtual_indices,
                                                   invalid_virtual_clusters};
    assert(!validate_virtual_geometry(invalid_virtual_data));
    assert(validate_virtual_geometry_view(VirtualGeometryView{}));
    auto invalid_view = VirtualGeometryView{};
    invalid_view.maximum_error_pixels = -1.0F;
    assert(!validate_virtual_geometry_view(invalid_view));

    const auto usage = TextureUsage::sampled | TextureUsage::color_attachment;
    assert(contains(usage, TextureUsage::sampled));
    assert(contains(usage, TextureUsage::color_attachment));
    assert(!contains(usage, TextureUsage::storage));

    RenderGraph graph;
    const auto color =
        graph.create_texture("scene-color", TextureDesc{.extent = {1280, 720},
                                                        .format = PixelFormat::rgba16_float,
                                                        .usage = TextureUsage::color_attachment |
                                                                 TextureUsage::sampled});
    const auto output =
        graph.import_texture("swapchain", TextureDesc{.extent = {1280, 720},
                                                      .format = PixelFormat::bgra8_srgb,
                                                      .usage = TextureUsage::color_attachment});

    const auto scene = graph.add_pass("scene");
    const auto tonemap = graph.add_pass("tonemap");
    assert(graph.use(scene, color, GraphAccess::write));
    assert(graph.use(tonemap, color, GraphAccess::read));
    assert(graph.use(tonemap, output, GraphAccess::write));
    assert(!graph.use(tonemap, output, GraphAccess::read));

    const auto compiled = graph.compile();
    assert(compiled);
    assert(compiled->execution_order.size() == 2);
    assert(compiled->execution_order[0] == scene);
    assert(compiled->execution_order[1] == tonemap);
    assert(compiled->lifetimes.size() == 2);
    assert(compiled->lifetimes[0].resource == color);
    assert(compiled->lifetimes[0].first_pass == 0);
    assert(compiled->lifetimes[0].last_pass == 1);

    RenderGraph invalid_graph;
    const auto transient = invalid_graph.create_texture("uninitialized", TextureDesc{});
    const auto invalid_pass = invalid_graph.add_pass("read-before-write");
    assert(invalid_graph.use(invalid_pass, transient, GraphAccess::read));
    const auto invalid = invalid_graph.compile();
    assert(!invalid);
    assert(invalid.error().code == GraphErrorCode::read_before_write);

    RenderGraph ordered_graph;
    const auto early = ordered_graph.add_pass("declared-first");
    const auto prerequisite = ordered_graph.add_pass("must-run-first");
    assert(ordered_graph.depends_on(early, prerequisite));
    const auto ordered = ordered_graph.compile();
    assert(ordered);
    assert(ordered->execution_order[0] == prerequisite);
    assert(ordered->execution_order[1] == early);

    RenderGraph cyclic_graph;
    const auto pass_a = cyclic_graph.add_pass("a");
    const auto pass_b = cyclic_graph.add_pass("b");
    assert(cyclic_graph.depends_on(pass_a, pass_b));
    assert(cyclic_graph.depends_on(pass_b, pass_a));
    const auto cyclic = cyclic_graph.compile();
    assert(!cyclic);
    assert(cyclic.error().code == GraphErrorCode::dependency_cycle);

    RenderGraph executable_graph;
    const auto execution_color = executable_graph.import_texture("color", TextureDesc{});
    std::vector<int> execution_trace;
    const auto execution_scene = executable_graph.add_pass("scene", [&execution_trace] {
        execution_trace.push_back(1);
        return std::expected<void, std::string>{};
    });
    const auto execution_post = executable_graph.add_pass("post", [&execution_trace] {
        execution_trace.push_back(2);
        return std::expected<void, std::string>{};
    });
    assert(executable_graph.use(execution_scene, execution_color, GraphAccess::write));
    assert(executable_graph.use(execution_post, execution_color, GraphAccess::read));
    const auto executable = executable_graph.compile();
    assert(executable);
    assert(executable_graph.execute(*executable));
    assert((execution_trace == std::vector<int>{1, 2}));

    RenderGraph failing_graph;
    const auto failed_pass = failing_graph.add_pass("intentional-failure", [] {
        return std::expected<void, std::string>{std::unexpected("test failure")};
    });
    static_cast<void>(failed_pass);
    const auto failing_compiled = failing_graph.compile();
    assert(failing_compiled);
    const auto failed_execution = failing_graph.execute(*failing_compiled);
    assert(!failed_execution);
    assert(failed_execution.error().code == GraphErrorCode::pass_execution_failed);
    assert(failed_execution.error().message == "intentional-failure: test failure");

    return 0;
}
