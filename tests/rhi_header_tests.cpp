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
    assert(validate_volumetric_clouds(VolumetricCloudSettings{}));
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
