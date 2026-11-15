#pragma once

#include <astra/rhi/types.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace astra::rhi {

struct MeshVertex final {
    Vec3 position;
    Vec3 normal;
    Color color;
    // Optional attributes; old vertex producers may leave these at zero.
    float texcoord_u{};
    float texcoord_v{};
    float texcoord1_u{};
    float texcoord1_v{};
    float tangent_x{};
    float tangent_y{};
    float tangent_z{};
    float tangent_w{1.0F};
    std::uint16_t joints[4]{};
    float weights[4]{};
};

struct MeshData final {
    // The renderer copies these spans during create_mesh; callers may release them afterward.
    std::span<const MeshVertex> vertices;
    std::span<const std::uint32_t> indices;
    struct MorphTarget final {
        // Per-vertex deltas. Empty attributes are treated as zero deltas.
        std::span<const Vec3> positions;
        std::span<const Vec3> normals;
        std::span<const Vec3> tangents;
    };
    std::span<const MorphTarget> morph_targets{};
};

[[nodiscard]] inline std::expected<void, std::string>
validate_morph_targets(std::size_t vertex_count,
                       std::span<const MeshData::MorphTarget> morph_targets) {
    if (morph_targets.size() > 256U)
        return std::unexpected("A mesh exceeds the 256 morph target limit.");
    if (!morph_targets.empty() && vertex_count == 0U)
        return std::unexpected("Morph targets require a non-empty base vertex array.");
    for (const auto& target : morph_targets) {
        const auto valid_attribute = [vertex_count](std::span<const Vec3> attribute) {
            return (attribute.empty() || attribute.size() == vertex_count) &&
                   std::ranges::all_of(attribute, [](const Vec3& value) {
                       return std::isfinite(value.x) && std::isfinite(value.y) &&
                              std::isfinite(value.z);
                   });
        };
        if (!valid_attribute(target.positions) || !valid_attribute(target.normals) ||
            !valid_attribute(target.tangents))
            return std::unexpected(
                "Morph target attributes must be finite and either empty or match the vertex count.");
        if (target.positions.empty() && target.normals.empty() && target.tangents.empty())
            return std::unexpected("A morph target must contain at least one vertex attribute.");
    }
    return {};
}

// Conservative displacement bound for morph-driven frustum culling. Each
// radius encloses one target's POSITION deltas; the triangle inequality keeps
// the result safe for negative and greater-than-one animation weights too.
[[nodiscard]] inline float morph_displacement_bound(std::span<const float> target_radii,
                                                      std::span<const float> weights) noexcept {
    if (weights.empty())
        return 0.0F;
    if (weights.size() != target_radii.size())
        return std::numeric_limits<float>::infinity();
    float result = 0.0F;
    for (std::size_t target = 0; target < weights.size(); ++target) {
        if (!std::isfinite(weights[target]) || !std::isfinite(target_radii[target]) ||
            target_radii[target] < 0.0F)
            return std::numeric_limits<float>::infinity();
        result += std::abs(weights[target]) * target_radii[target];
    }
    return result;
}

[[nodiscard]] inline bool morph_weights_are_finite(std::span<const float> weights) noexcept {
    return std::ranges::all_of(weights, [](float weight) { return std::isfinite(weight); });
}

template <typename T>
[[nodiscard]] inline std::span<const T> previous_frame_span(
    std::span<const T> current, std::span<const T> previous, bool has_previous_frame) noexcept {
    return has_previous_frame && !previous.empty() ? previous : current;
}

[[nodiscard]] inline std::expected<void, std::string> compact_instance_morph_weights(
    std::span<const float> current, std::span<const float> previous,
    std::span<const std::uint32_t> visible_instances, std::size_t target_count,
    std::vector<float>& compacted_current, std::vector<float>* compacted_previous = nullptr) {
    if (target_count == 0U || current.size() % target_count != 0U ||
        (!previous.empty() && previous.size() != current.size()) ||
        !morph_weights_are_finite(current) || !morph_weights_are_finite(previous) ||
        (compacted_previous != nullptr && compacted_previous == &compacted_current))
        return std::unexpected("Instance morph weights have an invalid target count or value.");
    if (visible_instances.size() > std::numeric_limits<std::size_t>::max() / target_count)
        return std::unexpected("Visible instance morph weights exceed the addressable size.");
    const auto instance_count = current.size() / target_count;
    compacted_current.clear();
    compacted_current.reserve(visible_instances.size() * target_count);
    if (compacted_previous != nullptr) {
        compacted_previous->clear();
        compacted_previous->reserve(visible_instances.size() * target_count);
    }
    for (const auto instance : visible_instances) {
        if (instance >= instance_count)
            return std::unexpected("Visible instance index exceeds the morph-weight array.");
        const auto offset = static_cast<std::size_t>(instance) * target_count;
        compacted_current.insert(compacted_current.end(), current.begin() + static_cast<std::ptrdiff_t>(offset),
                                 current.begin() + static_cast<std::ptrdiff_t>(offset + target_count));
        if (compacted_previous != nullptr) {
            const auto source = previous.empty() ? current : previous;
            compacted_previous->insert(compacted_previous->end(),
                source.begin() + static_cast<std::ptrdiff_t>(offset),
                source.begin() + static_cast<std::ptrdiff_t>(offset + target_count));
        }
    }
    return {};
}

// One renderable node in a virtual-geometry hierarchy. The sphere is in mesh
// local space. Indices address vertices relative to base_vertex. parent_index
// is UINT32_MAX for roots; nodes are stored in hierarchy order.
struct alignas(16) VirtualGeometryCluster final {
    Vec4 bounds_sphere{};
    float geometric_error{};
    std::uint32_t parent_index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::int32_t base_vertex{};
    std::uint32_t flags{}; // bit 0: leaf (must be selected when its parent is refined)
};
static_assert(sizeof(VirtualGeometryCluster) == 48);
static_assert(offsetof(VirtualGeometryCluster, geometric_error) == 16);
static_assert(offsetof(VirtualGeometryCluster, first_index) == 24);
static_assert(offsetof(VirtualGeometryCluster, flags) == 36);

struct VirtualGeometryData final {
    // These arrays are copied into persistent backend buffers during creation.
    std::span<const MeshVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::span<const VirtualGeometryCluster> clusters;
};

[[nodiscard]] inline std::expected<void, std::string>
validate_virtual_geometry(const VirtualGeometryData& data) {
    if (data.vertices.empty() || data.indices.empty() || data.clusters.empty())
        return std::unexpected("Virtual geometry requires vertices, indices, and clusters.");
    if (data.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
        data.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
        data.clusters.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected("Virtual geometry exceeds the indirect draw address limits.");
    for (std::size_t i = 0; i < data.clusters.size(); ++i) {
        const auto& cluster = data.clusters[i];
        const auto parent = cluster.parent_index;
        if (parent != std::numeric_limits<std::uint32_t>::max() &&
            (parent >= data.clusters.size() || parent == i))
            return std::unexpected("Virtual geometry contains an invalid parent index.");
        if (parent != std::numeric_limits<std::uint32_t>::max() &&
            (data.clusters[parent].flags & 1U) != 0U)
            return std::unexpected("Virtual geometry leaf cluster cannot have children.");
        if (!std::isfinite(cluster.bounds_sphere.x) || !std::isfinite(cluster.bounds_sphere.y) ||
            !std::isfinite(cluster.bounds_sphere.z) || !std::isfinite(cluster.bounds_sphere.w) ||
            cluster.bounds_sphere.w < 0.0F || !std::isfinite(cluster.geometric_error) ||
            cluster.geometric_error < 0.0F)
            return std::unexpected("Virtual geometry contains invalid cluster bounds or error.");
        if (cluster.index_count == 0 || cluster.index_count % 3U != 0U ||
            cluster.first_index > data.indices.size() ||
            cluster.index_count > data.indices.size() - cluster.first_index ||
            cluster.base_vertex < 0)
            return std::unexpected("Virtual geometry contains an invalid indexed draw range.");
        for (std::size_t index = cluster.first_index;
             index < static_cast<std::size_t>(cluster.first_index) + cluster.index_count; ++index) {
            const auto vertex =
                static_cast<std::uint64_t>(cluster.base_vertex) + data.indices[index];
            if (vertex >= data.vertices.size())
                return std::unexpected(
                    "Virtual geometry contains an out-of-range vertex reference.");
        }
    }

    // Parent links form a forest: every node must reach exactly one parentless
    // root. Since each node has at most one parent, checking every parent chain
    // for cycles also proves root reachability. Keep multiple roots valid; a
    // packed mesh may contain several independent hierarchy trees.
    constexpr auto no_parent = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint8_t> visit_state(data.clusters.size(), 0U);
    std::vector<std::size_t> path;
    path.reserve(data.clusters.size());
    std::size_t root_count = 0;
    for (const auto& cluster : data.clusters) {
        if (cluster.parent_index == no_parent)
            ++root_count;
    }
    if (root_count == 0)
        return std::unexpected("Virtual geometry hierarchy has no root clusters.");

    for (std::size_t start = 0; start < data.clusters.size(); ++start) {
        if (visit_state[start] == 2U)
            continue;

        path.clear();
        auto current = start;
        while (current != data.clusters.size() && visit_state[current] == 0U) {
            visit_state[current] = 1U;
            path.push_back(current);
            const auto parent = data.clusters[current].parent_index;
            current = parent == no_parent ? data.clusters.size() : parent;
        }
        if (current != data.clusters.size() && visit_state[current] == 1U)
            return std::unexpected("Virtual geometry hierarchy contains a parent cycle.");

        for (const auto node : path)
            visit_state[node] = 2U;
    }
    return {};
}

struct TextureData final {
    std::uint32_t width{};
    std::uint32_t height{};
    // Linear RGBA8 unless srgb is true. Rows are tightly packed, top-to-bottom.
    std::span<const std::byte> rgba8;
    bool srgb{};
};

struct EnvironmentData final {
    std::uint32_t width{};
    std::uint32_t height{};
    // Equirectangular, linear-light RGBA32F pixels (four floats per texel).
    std::span<const float> rgba32f;
    std::uint32_t environment_resolution{128};
    std::uint32_t irradiance_resolution{16};
    std::uint32_t prefilter_resolution{128};
    std::uint32_t prefilter_mip_count{5};
    std::uint32_t brdf_lut_resolution{128};
};

[[nodiscard]] inline std::expected<void, std::string>
validate_environment(const EnvironmentData& environment) {
    if (environment.width == 0 || environment.height == 0 || environment.width > 8192 ||
        environment.height > 4096 ||
        static_cast<std::uint64_t>(environment.width) * environment.height * 4U !=
            environment.rgba32f.size() ||
        environment.rgba32f.size_bytes() > 256U * 1024U * 1024U)
        return std::unexpected("Environment requires a valid RGBA32F equirectangular image.");
    const auto valid_resolution = [](std::uint32_t value, std::uint32_t maximum) {
        return value >= 4 && value <= maximum && (value & (value - 1U)) == 0U;
    };
    const auto maximum_mips = static_cast<std::uint32_t>(std::bit_width(environment.prefilter_resolution));
    if (!valid_resolution(environment.environment_resolution, 1024) ||
        !valid_resolution(environment.irradiance_resolution, 128) ||
        !valid_resolution(environment.prefilter_resolution, 1024) ||
        !valid_resolution(environment.brdf_lut_resolution, 512) ||
        environment.prefilter_mip_count == 0 || environment.prefilter_mip_count > 10 ||
        environment.prefilter_mip_count > maximum_mips)
        return std::unexpected("Environment convolution resolutions or mip count are invalid.");
    for (std::size_t i = 0; i < environment.rgba32f.size(); ++i) {
        if (!std::isfinite(environment.rgba32f[i]) ||
            (i % 4U != 3U && environment.rgba32f[i] < 0.0F))
            return std::unexpected("Environment pixels must contain finite, non-negative linear RGB.");
    }
    return {};
}

class GpuMesh {
  public:
    virtual ~GpuMesh() = default;
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

  protected:
    GpuMesh() = default;
};

class GpuVirtualGeometry {
  public:
    virtual ~GpuVirtualGeometry() = default;
    GpuVirtualGeometry(const GpuVirtualGeometry&) = delete;
    GpuVirtualGeometry& operator=(const GpuVirtualGeometry&) = delete;

  protected:
    GpuVirtualGeometry() = default;
};

struct VirtualGeometryView final {
    Matrix4 view_projection{};
    Vec3 camera_position{};
    float viewport_height_pixels{1080.0F};
    float vertical_fov_radians{1.04719755F};
    float maximum_error_pixels{1.0F};
};

[[nodiscard]] inline std::expected<void, std::string>
validate_virtual_geometry_view(const VirtualGeometryView& view) {
    if (!std::isfinite(view.camera_position.x) || !std::isfinite(view.camera_position.y) ||
        !std::isfinite(view.camera_position.z) || !std::isfinite(view.viewport_height_pixels) ||
        view.viewport_height_pixels <= 0.0F || !std::isfinite(view.vertical_fov_radians) ||
        view.vertical_fov_radians <= 0.0F || view.vertical_fov_radians >= 3.14159265F ||
        !std::isfinite(view.maximum_error_pixels) || view.maximum_error_pixels < 0.0F)
        return std::unexpected("Virtual geometry view has invalid camera or LOD parameters.");
    for (const auto value : view.view_projection.values) {
        if (!std::isfinite(value))
            return std::unexpected("Virtual geometry view-projection matrix is non-finite.");
    }
    return {};
}

class GpuTexture {
  public:
    virtual ~GpuTexture() = default;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

  protected:
    GpuTexture() = default;
};

class GpuEnvironment {
  public:
    virtual ~GpuEnvironment() = default;
    GpuEnvironment(const GpuEnvironment&) = delete;
    GpuEnvironment& operator=(const GpuEnvironment&) = delete;

  protected:
    GpuEnvironment() = default;
};

enum class MaterialFilter : std::uint8_t { nearest, linear };
enum class MaterialMipFilter : std::uint8_t { none, nearest, linear };
enum class MaterialAddressMode : std::uint8_t { repeat, mirrored_repeat, clamp_to_edge };

struct MaterialSamplerDesc final {
    MaterialFilter min_filter{MaterialFilter::linear};
    MaterialFilter mag_filter{MaterialFilter::linear};
    MaterialMipFilter mip_filter{MaterialMipFilter::linear};
    MaterialAddressMode address_u{MaterialAddressMode::repeat};
    MaterialAddressMode address_v{MaterialAddressMode::repeat};
};

struct MaterialUvTransform final {
    Vec2 offset{};
    Vec2 scale{1.0F, 1.0F};
    float rotation{};
};

struct MaterialData final {
    Color base_color{1.0F, 1.0F, 1.0F, 1.0F};
    Color emissive{};
    float metallic{};
    float roughness{0.72F};
    float normal_scale{1.0F};
    float occlusion_strength{1.0F};
    const GpuTexture* base_color_texture{};
    const GpuTexture* metallic_roughness_texture{};
    const GpuTexture* normal_texture{};
    const GpuTexture* occlusion_texture{};
    const GpuTexture* emissive_texture{};
    std::uint32_t base_color_texcoord{};
    std::uint32_t metallic_roughness_texcoord{};
    std::uint32_t normal_texcoord{};
    std::uint32_t occlusion_texcoord{};
    std::uint32_t emissive_texcoord{};
    MaterialUvTransform base_color_uv{};
    MaterialUvTransform metallic_roughness_uv{};
    MaterialUvTransform normal_uv{};
    MaterialUvTransform occlusion_uv{};
    MaterialUvTransform emissive_uv{};
    // glTF material alphaMode: opaque=0, mask=1, blend=2.
    std::uint32_t alpha_mode{};
    float alpha_cutoff{0.5F};
    bool double_sided{};
    MaterialSamplerDesc base_color_sampler{};
    MaterialSamplerDesc metallic_roughness_sampler{};
    MaterialSamplerDesc normal_sampler{};
    MaterialSamplerDesc occlusion_sampler{};
    MaterialSamplerDesc emissive_sampler{};
};

// Reusable world-space volumetric cloud controls. The Metal renderer evaluates
// a bounded 3D density field and feeds cloud motion/coverage into temporal AA.
struct VolumetricCloudSettings final {
    bool enabled{true};
    float base_altitude{1250.0F};
    float top_altitude{2450.0F};
    float density{1.0F};
    float coverage{0.55F};
    Vec3 wind_velocity{0.012F, 0.002F, -0.007F};
    std::uint32_t ray_steps{4};
};

[[nodiscard]] inline std::expected<void, std::string>
validate_volumetric_clouds(const VolumetricCloudSettings& clouds) {
    if (!std::isfinite(clouds.base_altitude) || !std::isfinite(clouds.top_altitude) ||
        clouds.base_altitude < 0.0F || clouds.top_altitude <= clouds.base_altitude ||
        !std::isfinite(clouds.density) || clouds.density < 0.0F || clouds.density > 8.0F ||
        !std::isfinite(clouds.coverage) || clouds.coverage < 0.0F || clouds.coverage > 1.0F ||
        !std::isfinite(clouds.wind_velocity.x) || !std::isfinite(clouds.wind_velocity.y) ||
        !std::isfinite(clouds.wind_velocity.z) || clouds.ray_steps == 0 || clouds.ray_steps > 12)
        return std::unexpected("Volumetric cloud settings contain invalid ranges or non-finite values.");
    return {};
}

struct DrawItem final {
    const GpuMesh* mesh{};
    Matrix4 model{};
    float metallic{};
    float roughness{0.72F};
    MaterialData material{};
    bool use_material{};
    std::span<const Matrix4> joint_matrices{};
    const GpuVirtualGeometry* virtual_geometry{};
    VirtualGeometryView virtual_geometry_view{};
    // Optional previous-frame transforms for temporal effects such as TAA.
    // When unavailable, backends should use the current transform as history.
    Matrix4 previous_model{};
    bool has_previous_transform{};
    std::span<const Matrix4> previous_joint_matrices{};
    // Current and previous morph weights are ordered exactly like MeshData::morph_targets.
    // Empty previous weights reuse current weights. Instance arrays are tightly packed as
    // [instance][target] and must match instance_transforms.size() * target_count.
    std::span<const float> morph_weights{};
    std::span<const float> previous_morph_weights{};
    std::span<const float> instance_morph_weights{};
    std::span<const float> previous_instance_morph_weights{};
    // Optional per-instance world transforms. The mesh/material are shared by all instances.
    std::span<const Matrix4> instance_transforms{};
    // Empty means reuse instance_transforms for static instances.
    std::span<const Matrix4> previous_instance_transforms{};
    // Optional indexed subrange within mesh; zero count means the full mesh.
    std::uint32_t first_index{};
    std::uint32_t index_count{};
};

// Isotropic local emitter. intensity is radiant intensity (W/sr when world units
// are meters); the shader evaluates inverse-square irradiance and a smooth cutoff.
struct PointLight final {
    Vec3 position{};
    float range{10.0F};
    Vec3 intensity{};
    float padding{};
};
static_assert(sizeof(PointLight) == 32);

inline constexpr std::size_t maximum_scene_point_lights = 32;

// Screen-space horizon AO settings. Radius is in world units; strength is a
// dimensionless visibility multiplier and bias suppresses self-occlusion.
struct GtaoSettings final {
    bool enabled{true};
    float radius{1.25F};
    float strength{1.15F};
    float bias{0.035F};
};

struct SceneSettings final {
    Vec3 camera_position{};
    Vec3 light_direction{-0.42F, -0.82F, -0.31F};
    Matrix4 light_view_projection{};
    Matrix4 inverse_view_projection{};
    float time_seconds{};
    float exposure{1.0F};
    Matrix4 previous_view_projection{};
    bool has_previous_view_projection{};
    // Previous camera origin lets temporal depth rejection compare the same
    // world point in the previous camera frame after camera translation.
    Vec3 previous_camera_position{};
    // Orthographic cameras have parallel rays and a per-pixel near-plane origin.
    bool orthographic_camera{};
    // Internal HDR/TAA resolution relative to the drawable. Tone mapping
    // upscales to the full presentation size; 1.0 preserves native resolution.
    float render_scale{1.0F};
    VolumetricCloudSettings clouds{};
    const GpuEnvironment* environment{};
    // Linear scene-referred radiance multiplier for the directional key light.
    // Kept separate from direction so day/night systems can change sun energy
    // and color without rebuilding scene geometry or editing shader constants.
    Vec3 directional_light_radiance{3.8F, 3.55F, 3.15F};
    // Bounded local emitters, expressed in world space and linear radiometric units.
    std::span<const PointLight> point_lights{};
    // Appended to preserve existing positional SceneSettings aggregate prefixes.
    GtaoSettings gtao{};
};

[[nodiscard]] inline std::expected<void, std::string>
validate_scene_lighting(const SceneSettings& settings) {
    const auto finite = [](Vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (!finite(settings.light_direction) ||
        settings.light_direction.x * settings.light_direction.x +
                settings.light_direction.y * settings.light_direction.y +
                settings.light_direction.z * settings.light_direction.z < 1.0e-8F)
        return std::unexpected("Directional light direction must be finite and non-zero.");
    if (!finite(settings.directional_light_radiance) ||
        settings.directional_light_radiance.x < 0.0F ||
        settings.directional_light_radiance.y < 0.0F ||
        settings.directional_light_radiance.z < 0.0F)
        return std::unexpected("Directional light radiance must be finite and non-negative.");
    if (settings.point_lights.size() > maximum_scene_point_lights)
        return std::unexpected("A scene may contain at most 32 point lights.");
    for (const auto& light : settings.point_lights) {
        if (!finite(light.position) || !finite(light.intensity) ||
            !std::isfinite(light.range) || light.range <= 0.0F ||
            light.intensity.x < 0.0F || light.intensity.y < 0.0F || light.intensity.z < 0.0F)
            return std::unexpected("Point lights require finite positions, positive ranges and non-negative intensity.");
    }
    if (!std::isfinite(settings.gtao.radius) || settings.gtao.radius < 0.0F ||
        settings.gtao.radius > 10.0F || !std::isfinite(settings.gtao.strength) ||
        settings.gtao.strength < 0.0F || settings.gtao.strength > 2.0F ||
        !std::isfinite(settings.gtao.bias) || settings.gtao.bias < 0.0F ||
        settings.gtao.bias > 0.5F)
        return std::unexpected("GTAO radius, strength or bias is outside its supported finite range.");
    return {};
}

enum class RenderErrorCode : unsigned char {
    unsupported_backend,
    device_unavailable,
    invalid_surface,
    shader_compilation_failed,
    pipeline_creation_failed,
    frame_acquisition_failed,
    command_submission_failed,
    invalid_mesh,
    resource_creation_failed,
    invalid_scene,
};

struct RenderError final {
    RenderErrorCode code{};
    std::string message;
};

class RenderDevice {
  public:
    virtual ~RenderDevice() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const std::string& device_name() const noexcept = 0;
    // Most recent completed GPU command-buffer duration, when the backend exposes it.
    [[nodiscard]] virtual double last_gpu_frame_time_ms() const noexcept { return 0.0; }

    [[nodiscard]] virtual std::expected<std::unique_ptr<GpuMesh>, RenderError>
    create_mesh(const MeshData& data) = 0;

    [[nodiscard]] virtual std::expected<std::unique_ptr<GpuVirtualGeometry>, RenderError>
    create_virtual_geometry(const VirtualGeometryData& data) {
        static_cast<void>(data);
        return std::unexpected(RenderError{
            .code = RenderErrorCode::unsupported_backend,
            .message = "This render backend does not support GPU virtual geometry.",
        });
    }

    [[nodiscard]] virtual std::expected<std::unique_ptr<GpuTexture>, RenderError>
    create_texture(const TextureData& data) = 0;

    [[nodiscard]] virtual std::expected<std::unique_ptr<GpuEnvironment>, RenderError>
    create_environment(const EnvironmentData& data) {
        static_cast<void>(data);
        return std::unexpected(RenderError{
            .code = RenderErrorCode::unsupported_backend,
            .message = "This render backend does not support image-based lighting.",
        });
    }

    // Submits a complete scene in one render pass and one presentation.
    [[nodiscard]] virtual std::expected<void, RenderError> render_frame(Color clear_color) = 0;
    [[nodiscard]] virtual std::expected<void, RenderError>
    render_scene(Color clear_color, std::span<const DrawItem> draw_items,
                 const Matrix4& view_projection, const SceneSettings& settings) = 0;
};

} // namespace astra::rhi
