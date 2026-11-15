#include <astra/assets/animation.hpp>
#include <astra/assets/gltf.hpp>
#include <astra/assets/image.hpp>
#include <astra/core/log.hpp>
#include <astra/game/game.hpp>
#include <astra/geometry/virtual_geometry.hpp>
#include <astra/geometry/virtual_geometry_streaming.hpp>
#if defined(ASTRA_HAS_PHYSICS)
#include <astra/physics/physics_world.hpp>
#endif
#include <astra/renderer/renderer.hpp>
#include <astra/world/character_controller.hpp>
#include <astra/world/vegetation_lod.hpp>
#include <astra/world/world_streamer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using astra::rhi::Color;
using astra::rhi::DrawItem;
using astra::rhi::Matrix4;
using astra::rhi::MeshVertex;
using astra::rhi::Vec3;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kRenderScale = 0.65F;

enum SettingsCommand : std::uint32_t {
    resolution_50 = 1,
    resolution_65,
    resolution_80,
    resolution_100,
    fps_30 = 10,
    fps_60,
    fps_90,
    fps_120,
    fps_144,
    fps_unlimited,
    toggle_adaptive_resolution = 20,
    toggle_performance_telemetry,
    toggle_benchmark_mode,
};

struct BenchmarkSummary final {
    double elapsed_seconds{};
    std::uint64_t frames{};
    double average_fps{};
    double average_cpu_ms{};
    double average_gpu_ms{};
    float render_scale{};
    std::size_t draw_count{};
    std::size_t resident_chunks{};
};

class BenchmarkCsvWriter final {
  public:
    BenchmarkCsvWriter() : worker_([this] { run(); }) {}
    BenchmarkCsvWriter(const BenchmarkCsvWriter&) = delete;
    BenchmarkCsvWriter& operator=(const BenchmarkCsvWriter&) = delete;

    ~BenchmarkCsvWriter() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }

    void enqueue(BenchmarkSummary summary) {
        {
            std::lock_guard lock{mutex_};
            pending_.push_back(summary);
        }
        ready_.notify_one();
    }

  private:
    void run() {
        for (;;) {
            BenchmarkSummary summary;
            {
                std::unique_lock lock{mutex_};
                ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) {
                    if (stopping_)
                        return;
                    continue;
                }
                summary = pending_.front();
                pending_.pop_front();
            }

            constexpr auto path = "astra_benchmark.csv";
            std::error_code error;
            const bool needs_header = !std::filesystem::exists(path, error) ||
                                      std::filesystem::file_size(path, error) == 0;
            std::ofstream output{path, std::ios::app};
            if (!output)
                continue;
            if (needs_header) {
                output << "elapsed_seconds,frames,average_fps,avg_cpu_ms,avg_gpu_ms,"
                          "render_scale,draw_count,resident_chunks\n";
            }
            output << std::fixed << std::setprecision(3) << summary.elapsed_seconds << ','
                   << summary.frames << ',' << summary.average_fps << ',' << summary.average_cpu_ms
                   << ',' << summary.average_gpu_ms << ',' << summary.render_scale << ','
                   << summary.draw_count << ',' << summary.resident_chunks << '\n';
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<BenchmarkSummary> pending_;
    bool stopping_{};
    std::thread worker_;
};

Vec3 add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 subtract(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 scale(Vec3 value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}
float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(Vec3 value) {
    const float length = std::sqrt(std::max(dot(value, value), 0.000001F));
    return scale(value, 1.0F / length);
}

float terrain_height(float x, float z) {
    const float broad = std::sin(x * 0.027F) * std::cos(z * 0.023F) * 3.2F;
    const float detail = std::sin((x + z) * 0.075F) * 0.7F + std::cos((x - z) * 0.061F) * 0.55F;
    const float hill =
        5.0F * std::exp(-((x - 43.0F) * (x - 43.0F) + (z + 48.0F) * (z + 48.0F)) / 1500.0F);
    return broad + detail + hill;
}

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result.values[column * 4 + row] = 0.0F;
            for (int k = 0; k < 4; ++k)
                result.values[column * 4 + row] += a.values[k * 4 + row] * b.values[column * 4 + k];
        }
    }
    return result;
}

Matrix4 perspective(float fov, float aspect, float near_plane, float far_plane) {
    Matrix4 result{};
    for (float& value : result.values)
        value = 0.0F;
    const float factor = 1.0F / std::tan(fov * 0.5F);
    result.values[0] = factor / aspect;
    result.values[5] = factor;
    result.values[10] = far_plane / (near_plane - far_plane);
    result.values[11] = -1.0F;
    result.values[14] = near_plane * far_plane / (near_plane - far_plane);
    return result;
}

Matrix4 orthographic(float half_width, float half_height, float near_plane, float far_plane) {
    Matrix4 result{};
    result.values[0] = 1.0F / half_width;
    result.values[5] = 1.0F / half_height;
    result.values[10] = 1.0F / (near_plane - far_plane);
    result.values[14] = near_plane / (near_plane - far_plane);
    return result;
}

Matrix4 look_at(Vec3 eye, Vec3 direction) {
    const Vec3 forward = normalize(direction);
    const Vec3 right = normalize(cross(forward, {0.0F, 1.0F, 0.0F}));
    const Vec3 up = cross(right, forward);
    Matrix4 result{};
    result.values[0] = right.x;
    result.values[4] = right.y;
    result.values[8] = right.z;
    result.values[1] = up.x;
    result.values[5] = up.y;
    result.values[9] = up.z;
    result.values[2] = -forward.x;
    result.values[6] = -forward.y;
    result.values[10] = -forward.z;
    result.values[12] = -dot(right, eye);
    result.values[13] = -dot(up, eye);
    result.values[14] = dot(forward, eye);
    return result;
}

Matrix4 translation(float x, float y, float z) {
    Matrix4 result{};
    result.values[12] = x;
    result.values[13] = y;
    result.values[14] = z;
    return result;
}

Matrix4 scaling(float x, float y, float z) {
    Matrix4 result{};
    result.values[0] = x;
    result.values[5] = y;
    result.values[10] = z;
    return result;
}

Matrix4 rotation_y(float angle) {
    Matrix4 result{};
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result.values[0] = c;
    result.values[2] = -s;
    result.values[8] = s;
    result.values[10] = c;
    return result;
}

std::uint32_t random_step(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}
float random_range(std::uint32_t& state, float minimum, float maximum) {
    const float unit = static_cast<float>(random_step(state) & 0x00FFFFFFU) / 16777215.0F;
    return minimum + (maximum - minimum) * unit;
}

std::vector<float> make_outdoor_environment(std::uint32_t width, std::uint32_t height,
                                            Vec3 sun_direction) {
    std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4U, 1.0F);
    sun_direction = normalize(sun_direction);
    const auto smooth = [](float low, float high, float value) {
        const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        const float latitude = (0.5F - (static_cast<float>(y) + 0.5F) /
                                             static_cast<float>(height)) * kPi;
        const float cos_latitude = std::cos(latitude);
        for (std::uint32_t x = 0; x < width; ++x) {
            const float longitude = ((static_cast<float>(x) + 0.5F) /
                                         static_cast<float>(width) - 0.5F) * (2.0F * kPi);
            const Vec3 direction{cos_latitude * std::cos(longitude), std::sin(latitude),
                                  cos_latitude * std::sin(longitude)};
            const float sky_elevation = smooth(-0.12F, 0.72F, direction.y);
            const float ground_weight = smooth(0.025F, 0.42F, -direction.y);
            const float sun_alignment = std::max(dot(direction, sun_direction), 0.0F);
            const float solar_disc = std::pow(sun_alignment, 900.0F) * 42.0F;
            const float solar_halo = std::pow(sun_alignment, 26.0F) * 0.42F;
            const std::array<float, 3> sky_horizon{0.48F, 0.57F, 0.66F};
            const std::array<float, 3> sky_zenith{0.075F, 0.19F, 0.36F};
            const std::array<float, 3> ground{0.075F, 0.105F, 0.052F};
            const std::array<float, 3> sun_tint{1.0F, 0.72F, 0.43F};
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float sky = std::lerp(sky_horizon[channel], sky_zenith[channel],
                                            sky_elevation);
                const float environment = std::lerp(sky, ground[channel], ground_weight);
                pixels[offset + channel] = environment +
                    sun_tint[channel] * (solar_disc + solar_halo);
            }
            pixels[offset + 3U] = 1.0F;
        }
    }
    return pixels;
}

struct RuntimePrimitive final {
    std::uint32_t node{};
    std::size_t mesh_group{};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    const astra::rhi::GpuMesh* mesh{};
    astra::rhi::MaterialData material;
};

struct RuntimeModel final {
    astra::assets::Asset source;
    std::vector<std::unique_ptr<astra::rhi::GpuTexture>> textures;
    std::vector<std::unique_ptr<astra::rhi::GpuMesh>> meshes;
    std::vector<RuntimePrimitive> primitives;
    std::vector<Matrix4> node_transforms;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    bool has_bounds{};
};

std::expected<std::vector<bool>, std::string>
selected_scene_nodes(const astra::assets::Asset& asset) {
    std::vector<bool> included(asset.nodes.size());
    std::vector<std::uint32_t> pending;
    if (asset.scenes.empty()) {
        for (std::uint32_t i = 0; i < asset.nodes.size(); ++i)
            if (asset.nodes[i].parent < 0)
                pending.push_back(i);
    } else {
        const auto scene_index =
            asset.default_scene < asset.scenes.size() ? asset.default_scene : 0U;
        pending = asset.scenes[scene_index].roots;
    }
    while (!pending.empty()) {
        const auto node_index = pending.back();
        pending.pop_back();
        if (node_index >= asset.nodes.size())
            return std::unexpected("The selected glTF scene references an invalid node.");
        if (included[node_index])
            continue;
        included[node_index] = true;
        for (const auto child : asset.nodes[node_index].children)
            pending.push_back(child);
    }
    return included;
}

std::expected<RuntimeModel, std::string> load_runtime_model(astra::renderer::Renderer& renderer,
                                                            const std::filesystem::path& path,
                                                            astra::core::Logger& logger) {
    auto loaded = astra::assets::load_gltf(path);
    if (!loaded)
        return std::unexpected("Could not load " + path.string() + ": " + loaded.error().message);

    RuntimeModel model;
    model.source = std::move(*loaded);
    auto scene_nodes = selected_scene_nodes(model.source);
    if (!scene_nodes)
        return std::unexpected(scene_nodes.error());
    std::map<std::pair<std::uint32_t, bool>, const astra::rhi::GpuTexture*> uploaded_textures;
    const auto upload_texture = [&](std::uint32_t texture_index,
                                    bool srgb) -> const astra::rhi::GpuTexture* {
        if (texture_index == std::numeric_limits<std::uint32_t>::max() ||
            texture_index >= model.source.textures.size())
            return nullptr;
        const auto image_index = model.source.textures[texture_index].image;
        if (image_index >= model.source.images.size())
            return nullptr;
        // glTF texture objects can share one image while specifying different samplers.
        // Samplers live on MaterialData, so upload each image/pixel-format only once.
        const auto key = std::pair{image_index, srgb};
        if (const auto found = uploaded_textures.find(key); found != uploaded_textures.end())
            return found->second;
        const auto& encoded = model.source.images[image_index];
        auto decoded = astra::assets::decode_image(encoded.bytes, encoded.mime_type);
        if (!decoded) {
            logger.log(astra::core::LogLevel::warning, "Assets",
                       "Could not decode texture " + encoded.name + ": " + decoded.error().message);
            return nullptr;
        }
        auto gpu = renderer.create_texture({decoded->width, decoded->height, decoded->rgba8, srgb});
        if (!gpu) {
            logger.log(astra::core::LogLevel::warning, "Assets",
                       "Could not upload texture " + encoded.name + ": " + gpu.error().message);
            return nullptr;
        }
        const auto* reference = gpu->get();
        model.textures.push_back(std::move(*gpu));
        uploaded_textures.emplace(key, reference);
        return reference;
    };

    std::vector<astra::rhi::MaterialData> materials;
    materials.reserve(model.source.materials.size());
    const auto material_sampler = [&](const astra::assets::TextureInfo& info) {
        astra::rhi::MaterialSamplerDesc result;
        if (info.texture >= model.source.textures.size())
            return result;
        const auto& source = model.source.textures[info.texture].sampling;
        result.min_filter = static_cast<astra::rhi::MaterialFilter>(source.min_filter);
        result.mag_filter = static_cast<astra::rhi::MaterialFilter>(source.mag_filter);
        result.mip_filter = static_cast<astra::rhi::MaterialMipFilter>(source.mip_filter);
        result.address_u = static_cast<astra::rhi::MaterialAddressMode>(source.wrap_s);
        result.address_v = static_cast<astra::rhi::MaterialAddressMode>(source.wrap_t);
        return result;
    };
    for (const auto& source : model.source.materials) {
        const std::array<std::uint32_t, 5> texcoord_sets{
            source.base_color_texture.texcoord, source.metallic_roughness_texture.texcoord,
            source.normal_texture.texcoord, source.occlusion_texture.texcoord,
            source.emissive_texture.texcoord};
        for (const auto set : texcoord_sets) {
            if (set > 1U)
                return std::unexpected("This renderer supports glTF TEXCOORD_0 and TEXCOORD_1; "
                                       "the asset requests a higher texture-coordinate set.");
        }
        astra::rhi::MaterialData material;
        material.base_color = {source.base_color_factor.x, source.base_color_factor.y,
                               source.base_color_factor.z, source.base_color_factor.w};
        material.emissive = {source.emissive_factor.x, source.emissive_factor.y,
                             source.emissive_factor.z, 1.0F};
        material.metallic = source.metallic_factor;
        material.roughness = source.roughness_factor;
        material.normal_scale = source.normal_texture.scale;
        material.occlusion_strength = source.occlusion_texture.scale;
        material.alpha_mode = source.alpha_mode;
        material.alpha_cutoff = source.alpha_cutoff;
        material.double_sided = source.double_sided;
        material.base_color_texture = upload_texture(source.base_color_texture.texture, true);
        material.metallic_roughness_texture =
            upload_texture(source.metallic_roughness_texture.texture, false);
        material.normal_texture = upload_texture(source.normal_texture.texture, false);
        material.occlusion_texture = upload_texture(source.occlusion_texture.texture, false);
        material.emissive_texture = upload_texture(source.emissive_texture.texture, true);
        material.base_color_texcoord = source.base_color_texture.texcoord;
        material.metallic_roughness_texcoord = source.metallic_roughness_texture.texcoord;
        material.normal_texcoord = source.normal_texture.texcoord;
        material.occlusion_texcoord = source.occlusion_texture.texcoord;
        material.emissive_texcoord = source.emissive_texture.texcoord;
        const auto uv_transform = [](const astra::assets::TextureInfo& info) {
            return astra::rhi::MaterialUvTransform{.offset = {info.uv_offset.x, info.uv_offset.y},
                                                   .scale = {info.uv_scale.x, info.uv_scale.y},
                                                   .rotation = info.uv_rotation};
        };
        material.base_color_uv = uv_transform(source.base_color_texture);
        material.metallic_roughness_uv = uv_transform(source.metallic_roughness_texture);
        material.normal_uv = uv_transform(source.normal_texture);
        material.occlusion_uv = uv_transform(source.occlusion_texture);
        material.emissive_uv = uv_transform(source.emissive_texture);
        material.base_color_sampler = material_sampler(source.base_color_texture);
        material.metallic_roughness_sampler = material_sampler(source.metallic_roughness_texture);
        material.normal_sampler = material_sampler(source.normal_texture);
        material.occlusion_sampler = material_sampler(source.occlusion_texture);
        material.emissive_sampler = material_sampler(source.emissive_texture);
        materials.push_back(material);
    }

    struct MeshBuildGroup final {
        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<std::vector<Vec3>> morph_positions;
        std::vector<std::vector<Vec3>> morph_normals;
        std::vector<std::vector<Vec3>> morph_tangents;
        std::vector<astra::rhi::MeshData::MorphTarget> morph_targets;
    };
    std::vector<MeshBuildGroup> mesh_groups;
    const auto same_vertex = [](const MeshVertex& a, const MeshVertex& b) {
        return a.position.x == b.position.x && a.position.y == b.position.y &&
               a.position.z == b.position.z && a.normal.x == b.normal.x &&
               a.normal.y == b.normal.y && a.normal.z == b.normal.z &&
               a.color.red == b.color.red && a.color.green == b.color.green &&
               a.color.blue == b.color.blue && a.color.alpha == b.color.alpha &&
               a.texcoord_u == b.texcoord_u &&
               a.texcoord_v == b.texcoord_v && a.texcoord1_u == b.texcoord1_u &&
               a.texcoord1_v == b.texcoord1_v && a.tangent_x == b.tangent_x &&
               a.tangent_y == b.tangent_y && a.tangent_z == b.tangent_z &&
               a.tangent_w == b.tangent_w && std::equal(std::begin(a.joints), std::end(a.joints),
                                                        std::begin(b.joints)) &&
               std::equal(std::begin(a.weights), std::end(a.weights), std::begin(b.weights));
    };

    for (std::uint32_t node_index = 0; node_index < model.source.nodes.size(); ++node_index) {
        if (!(*scene_nodes)[node_index])
            continue;
        const auto& node = model.source.nodes[node_index];
        if (node.mesh == std::numeric_limits<std::uint32_t>::max())
            continue;
        if (node.mesh >= model.source.meshes.size())
            return std::unexpected("A glTF node references an invalid mesh.");
        const auto& mesh = model.source.meshes[node.mesh];
        for (const auto& primitive : mesh.primitives) {
            if (primitive.mode != 4) {
                logger.log(astra::core::LogLevel::warning, "Assets",
                           "Skipping a non-triangle primitive in " + mesh.name);
                continue;
            }
            std::vector<MeshVertex> vertices;
            vertices.reserve(primitive.vertices.size());
            float primitive_min_y = std::numeric_limits<float>::infinity();
            float primitive_max_y = -std::numeric_limits<float>::infinity();
            for (const auto& source : primitive.vertices) {
                primitive_min_y = std::min(primitive_min_y, source.position.y);
                primitive_max_y = std::max(primitive_max_y, source.position.y);
            }
            const std::string material_name = primitive.material < model.source.materials.size()
                                                  ? model.source.materials[primitive.material].name
                                                  : std::string{};
            const bool foliage_material = material_name.find("leaf") != std::string::npos ||
                                          material_name.find("leaves") != std::string::npos ||
                                          material_name.find("fern") != std::string::npos ||
                                          material_name.find("shrub") != std::string::npos ||
                                          material_name.find("twigs") != std::string::npos;
            const float primitive_height = std::max(primitive_max_y - primitive_min_y, 1.0e-4F);
            for (const auto& source : primitive.vertices) {
                MeshVertex vertex{};
                vertex.position = {source.position.x, source.position.y, source.position.z};
                vertex.normal = {source.normal.x, source.normal.y, source.normal.z};
                vertex.color = {source.color.x, source.color.y, source.color.z, source.color.w};
                vertex.texcoord_u = source.texcoord0.x;
                vertex.texcoord_v = source.texcoord0.y;
                vertex.texcoord1_u = source.texcoord1.x;
                vertex.texcoord1_v = source.texcoord1.y;
                vertex.tangent_x = source.tangent.x;
                vertex.tangent_y = source.tangent.y;
                vertex.tangent_z = source.tangent.z;
                vertex.tangent_w = source.tangent.w;
                if (foliage_material) {
                    // The imported vegetation has authored UV0 but no UV1. Reserve UV1.y
                    // as a normalized vertical bend weight for the shared wind path.
                    vertex.texcoord1_v = std::clamp(
                        (source.position.y - primitive_min_y) / primitive_height, 0.0F, 1.0F);
                    // Zero is the renderer's reserved vegetation-wind marker.
                    // Keep the authored tangent xyz; the shader restores a stable
                    // positive tangent handedness for marked foliage.
                    vertex.tangent_w = 0.0F;
                }
                for (std::size_t joint = 0; joint < 4; ++joint) {
                    vertex.joints[joint] = source.joints[joint];
                    const float weights[4]{source.weights.x, source.weights.y, source.weights.z,
                                           source.weights.w};
                    vertex.weights[joint] = weights[joint];
                }
                vertices.push_back(vertex);
            }
            const bool has_authored_tangents =
                std::any_of(vertices.begin(), vertices.end(), [](const MeshVertex& vertex) {
                    return vertex.tangent_x * vertex.tangent_x +
                               vertex.tangent_y * vertex.tangent_y +
                               vertex.tangent_z * vertex.tangent_z >
                           0.5F;
                });
            const std::uint32_t normal_uv_set = primitive.material < materials.size()
                                                    ? materials[primitive.material].normal_texcoord
                                                    : 0U;
            if (!has_authored_tangents) {
                std::vector<Vec3> tangent_sums(vertices.size());
                std::vector<Vec3> bitangent_sums(vertices.size());
                for (std::size_t triangle = 0; triangle + 2U < primitive.indices.size();
                     triangle += 3U) {
                    const auto i0 = primitive.indices[triangle];
                    const auto i1 = primitive.indices[triangle + 1U];
                    const auto i2 = primitive.indices[triangle + 2U];
                    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                        continue;
                    const auto& a = vertices[i0];
                    const auto& b = vertices[i1];
                    const auto& c = vertices[i2];
                    const Vec3 edge1 = subtract(b.position, a.position);
                    const Vec3 edge2 = subtract(c.position, a.position);
                    const float au = normal_uv_set == 0 ? a.texcoord_u : a.texcoord1_u;
                    const float av = normal_uv_set == 0 ? a.texcoord_v : a.texcoord1_v;
                    const float bu = normal_uv_set == 0 ? b.texcoord_u : b.texcoord1_u;
                    const float bv = normal_uv_set == 0 ? b.texcoord_v : b.texcoord1_v;
                    const float cu = normal_uv_set == 0 ? c.texcoord_u : c.texcoord1_u;
                    const float cv = normal_uv_set == 0 ? c.texcoord_v : c.texcoord1_v;
                    const float du1 = bu - au;
                    const float dv1 = bv - av;
                    const float du2 = cu - au;
                    const float dv2 = cv - av;
                    const float determinant = du1 * dv2 - dv1 * du2;
                    if (std::abs(determinant) < 1.0e-8F)
                        continue;
                    const float inverse = 1.0F / determinant;
                    const Vec3 tangent =
                        scale(subtract(scale(edge1, dv2), scale(edge2, dv1)), inverse);
                    const Vec3 bitangent =
                        scale(subtract(scale(edge2, du1), scale(edge1, du2)), inverse);
                    for (const auto index : {i0, i1, i2}) {
                        tangent_sums[index] = add(tangent_sums[index], tangent);
                        bitangent_sums[index] = add(bitangent_sums[index], bitangent);
                    }
                }
                for (std::size_t i = 0; i < vertices.size(); ++i) {
                    const Vec3 n = normalize(vertices[i].normal);
                    Vec3 tangent = subtract(tangent_sums[i], scale(n, dot(n, tangent_sums[i])));
                    if (dot(tangent, tangent) < 1.0e-8F) {
                        const Vec3 axis =
                            std::abs(n.y) < 0.9F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
                        tangent = cross(axis, n);
                    }
                    tangent = normalize(tangent);
                    vertices[i].tangent_x = tangent.x;
                    vertices[i].tangent_y = tangent.y;
                    vertices[i].tangent_z = tangent.z;
                    vertices[i].tangent_w =
                        dot(cross(n, tangent), bitangent_sums[i]) < 0.0F ? -1.0F : 1.0F;
                }
            }
            if (node.skin != std::numeric_limits<std::uint32_t>::max()) {
                for (auto& vertex : vertices) {
                    const float total = vertex.weights[0] + vertex.weights[1] + vertex.weights[2] +
                                        vertex.weights[3];
                    if (!(total > 1.0e-6F)) {
                        vertex.joints[0] = 0;
                        vertex.weights[0] = 1.0F;
                    }
                }
            }
            std::vector<std::vector<Vec3>> morph_positions;
            std::vector<std::vector<Vec3>> morph_normals;
            std::vector<std::vector<Vec3>> morph_tangents;
            morph_positions.reserve(primitive.morph_targets.size());
            morph_normals.reserve(primitive.morph_targets.size());
            morph_tangents.reserve(primitive.morph_targets.size());
            for (const auto& source_target : primitive.morph_targets) {
                morph_positions.emplace_back();
                morph_normals.emplace_back();
                morph_tangents.emplace_back();
                const auto convert = [](const std::vector<astra::assets::Vec3>& source,
                                        std::vector<Vec3>& destination) {
                    destination.reserve(source.size());
                    for (const auto& value : source)
                        destination.push_back({value.x, value.y, value.z});
                };
                convert(source_target.positions, morph_positions.back());
                convert(source_target.normals, morph_normals.back());
                convert(source_target.tangents, morph_tangents.back());
            }
            const auto group = std::find_if(mesh_groups.begin(), mesh_groups.end(),
                                            [&vertices, &same_vertex, &morph_positions](const auto& candidate) {
                return morph_positions.empty() && candidate.morph_targets.empty() &&
                       candidate.vertices.size() == vertices.size() &&
                       std::equal(candidate.vertices.begin(), candidate.vertices.end(),
                                  vertices.begin(), same_vertex);
            });
            std::size_t group_index = 0;
            if (group == mesh_groups.end()) {
                group_index = mesh_groups.size();
                mesh_groups.push_back({std::move(vertices), {}, std::move(morph_positions),
                                       std::move(morph_normals), std::move(morph_tangents), {}});
                auto& created_group = mesh_groups.back();
                for (std::size_t target = 0; target < created_group.morph_positions.size(); ++target)
                    created_group.morph_targets.push_back({created_group.morph_positions[target],
                                                           created_group.morph_normals[target],
                                                           created_group.morph_tangents[target]});
            } else {
                group_index = static_cast<std::size_t>(group - mesh_groups.begin());
            }
            auto& mesh_group = mesh_groups[group_index];
            if (mesh_group.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
                primitive.indices.size() >
                    std::numeric_limits<std::uint32_t>::max() - mesh_group.indices.size())
                return std::unexpected("A glTF shared mesh index buffer exceeds 32-bit limits.");
            const auto first_index = static_cast<std::uint32_t>(mesh_group.indices.size());
            const auto index_count = static_cast<std::uint32_t>(primitive.indices.size());
            mesh_group.indices.insert(mesh_group.indices.end(), primitive.indices.begin(),
                                      primitive.indices.end());
            astra::rhi::MaterialData material;
            if (primitive.material < materials.size())
                material = materials[primitive.material];
            model.primitives.push_back(
                {node_index, group_index, first_index, index_count, nullptr, material});
        }
    }
    model.meshes.reserve(mesh_groups.size());
    for (std::size_t group_index = 0; group_index < mesh_groups.size(); ++group_index) {
        const auto& group = mesh_groups[group_index];
        auto gpu_mesh = renderer.create_mesh({group.vertices, group.indices, group.morph_targets});
        if (!gpu_mesh)
            return std::unexpected("Could not upload a shared glTF mesh: " +
                                   gpu_mesh.error().message);
        model.meshes.push_back(std::move(*gpu_mesh));
    }
    for (auto& primitive : model.primitives)
        primitive.mesh = model.meshes[primitive.mesh_group].get();
    if (model.primitives.empty())
        return std::unexpected("The glTF asset contains no renderable triangle primitives.");
    logger.log(astra::core::LogLevel::info, "Assets",
               "Imported " + path.filename().string() + " with " +
                   std::to_string(model.primitives.size()) + " mesh primitives, " +
                   std::to_string(model.meshes.size()) + " shared vertex buffers, " +
                   std::to_string(model.source.materials.size()) + " PBR materials, and " +
                   std::to_string(model.source.animations.size()) + " animation clips.");
    return model;
}

Matrix4 quaternion_matrix(astra::assets::Vec4 q) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 0.000001F) {
        q.x /= length;
        q.y /= length;
        q.z /= length;
        q.w /= length;
    } else {
        q = {0.0F, 0.0F, 0.0F, 1.0F};
    }
    Matrix4 result{};
    result.values[0] = 1.0F - 2.0F * (q.y * q.y + q.z * q.z);
    result.values[1] = 2.0F * (q.x * q.y + q.z * q.w);
    result.values[2] = 2.0F * (q.x * q.z - q.y * q.w);
    result.values[4] = 2.0F * (q.x * q.y - q.z * q.w);
    result.values[5] = 1.0F - 2.0F * (q.x * q.x + q.z * q.z);
    result.values[6] = 2.0F * (q.y * q.z + q.x * q.w);
    result.values[8] = 2.0F * (q.x * q.z + q.y * q.w);
    result.values[9] = 2.0F * (q.y * q.z - q.x * q.w);
    result.values[10] = 1.0F - 2.0F * (q.x * q.x + q.y * q.y);
    return result;
}

astra::assets::Vec4 slerp_quaternion(astra::assets::Vec4 a, astra::assets::Vec4 b, float t) {
    const auto normalize_quaternion = [](astra::assets::Vec4 q) {
        const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (length > 1.0e-6F)
            return astra::assets::Vec4{q.x / length, q.y / length, q.z / length, q.w / length};
        return astra::assets::Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    };
    a = normalize_quaternion(a);
    b = normalize_quaternion(b);
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosine < 0.0F) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    if (cosine > 0.9995F) {
        return normalize_quaternion({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                     a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
    const float sine = std::sin(angle);
    const float from_weight = std::sin((1.0F - t) * angle) / sine;
    const float to_weight = std::sin(t * angle) / sine;
    return {a.x * from_weight + b.x * to_weight, a.y * from_weight + b.y * to_weight,
            a.z * from_weight + b.z * to_weight, a.w * from_weight + b.w * to_weight};
}

std::vector<astra::assets::NodePose> blend_node_poses(std::span<const astra::assets::NodePose> from,
                                                      std::span<const astra::assets::NodePose> to,
                                                      float amount) {
    if (from.size() != to.size())
        return {to.begin(), to.end()};
    amount = std::clamp(amount, 0.0F, 1.0F);
    std::vector<astra::assets::NodePose> result(from.size());
    for (std::size_t i = 0; i < from.size(); ++i) {
        const auto& a = from[i];
        const auto& b = to[i];
        result[i].translation = {a.translation.x + (b.translation.x - a.translation.x) * amount,
                                 a.translation.y + (b.translation.y - a.translation.y) * amount,
                                 a.translation.z + (b.translation.z - a.translation.z) * amount};
        result[i].rotation = slerp_quaternion(a.rotation, b.rotation, amount);
        result[i].scale = {a.scale.x + (b.scale.x - a.scale.x) * amount,
                           a.scale.y + (b.scale.y - a.scale.y) * amount,
                           a.scale.z + (b.scale.z - a.scale.z) * amount};
        if (a.morph_weights.size() == b.morph_weights.size()) {
            result[i].morph_weights.resize(a.morph_weights.size());
            for (std::size_t weight = 0; weight < a.morph_weights.size(); ++weight)
                result[i].morph_weights[weight] =
                    a.morph_weights[weight] + (b.morph_weights[weight] - a.morph_weights[weight]) * amount;
        } else {
            result[i].morph_weights = b.morph_weights;
        }
    }
    return result;
}

Matrix4 node_local_matrix(const astra::assets::Node& node, const astra::assets::NodePose& pose) {
    if (node.has_matrix) {
        Matrix4 result{};
        std::copy(node.matrix.begin(), node.matrix.end(), result.values.data());
        return result;
    }
    const auto rotation = quaternion_matrix(pose.rotation);
    return multiply(
        multiply(translation(pose.translation.x, pose.translation.y, pose.translation.z), rotation),
        scaling(pose.scale.x, pose.scale.y, pose.scale.z));
}

std::vector<Matrix4> build_global_node_matrices(const astra::assets::Asset& asset,
                                                std::span<const astra::assets::NodePose> poses) {
    if (poses.size() != asset.nodes.size())
        throw std::invalid_argument("node pose count does not match the glTF node count");
    std::vector<Matrix4> global(asset.nodes.size());
    std::vector<unsigned char> state(asset.nodes.size());
    const auto build = [&](const auto& self, std::size_t index) -> void {
        if (state[index] == 2)
            return;
        if (state[index] == 1)
            throw std::runtime_error("glTF node hierarchy contains a cycle");
        state[index] = 1;
        const auto& node = asset.nodes[index];
        const auto local = node_local_matrix(node, poses[index]);
        if (node.parent >= 0) {
            const auto parent = static_cast<std::size_t>(node.parent);
            if (parent >= asset.nodes.size())
                throw std::runtime_error("glTF node parent index is invalid");
            self(self, parent);
            global[index] = multiply(global[parent], local);
        } else {
            global[index] = local;
        }
        state[index] = 2;
    };
    for (std::size_t i = 0; i < asset.nodes.size(); ++i)
        build(build, i);
    return global;
}

Vec3 transform_point(const Matrix4& matrix, Vec3 point) {
    return {matrix.values[0] * point.x + matrix.values[4] * point.y + matrix.values[8] * point.z +
                matrix.values[12],
            matrix.values[1] * point.x + matrix.values[5] * point.y + matrix.values[9] * point.z +
                matrix.values[13],
            matrix.values[2] * point.x + matrix.values[6] * point.y + matrix.values[10] * point.z +
                matrix.values[14]};
}

void prepare_static_runtime_model(RuntimeModel& model) {
    std::vector<astra::assets::NodePose> poses(model.source.nodes.size());
    for (std::size_t i = 0; i < model.source.nodes.size(); ++i) {
        const auto& node = model.source.nodes[i];
        poses[i] = {node.translation, node.rotation, node.scale, node.weights};
    }
    model.node_transforms = build_global_node_matrices(model.source, poses);
    const float infinity = std::numeric_limits<float>::infinity();
    model.bounds_min = {infinity, infinity, infinity};
    model.bounds_max = {-infinity, -infinity, -infinity};
    for (std::size_t node_index = 0; node_index < model.source.nodes.size(); ++node_index) {
        const auto mesh_index = model.source.nodes[node_index].mesh;
        if (mesh_index >= model.source.meshes.size())
            continue;
        for (const auto& primitive : model.source.meshes[mesh_index].primitives) {
            for (const auto& vertex : primitive.vertices) {
                const Vec3 point =
                    transform_point(model.node_transforms[node_index],
                                    {vertex.position.x, vertex.position.y, vertex.position.z});
                model.bounds_min.x = std::min(model.bounds_min.x, point.x);
                model.bounds_min.y = std::min(model.bounds_min.y, point.y);
                model.bounds_min.z = std::min(model.bounds_min.z, point.z);
                model.bounds_max.x = std::max(model.bounds_max.x, point.x);
                model.bounds_max.y = std::max(model.bounds_max.y, point.y);
                model.bounds_max.z = std::max(model.bounds_max.z, point.z);
                model.has_bounds = true;
            }
        }
    }
    if (!model.has_bounds || !std::isfinite(model.bounds_min.y) ||
        !std::isfinite(model.bounds_max.y) || model.bounds_max.y <= model.bounds_min.y)
        throw std::runtime_error("Static glTF model has invalid or empty bounds.");
}

void release_imported_geometry_and_images(astra::assets::Asset& asset) {
    asset.images.clear();
    asset.images.shrink_to_fit();
    asset.textures.clear();
    asset.textures.shrink_to_fit();
    asset.samplers.clear();
    asset.samplers.shrink_to_fit();
    asset.materials.clear();
    asset.materials.shrink_to_fit();
    asset.meshes.clear();
    asset.meshes.shrink_to_fit();
    asset.scenes.clear();
    asset.scenes.shrink_to_fit();
}

std::expected<Matrix4, std::string> inverse_matrix(const Matrix4& matrix) {
    double augmented[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column)
            augmented[row][column] = matrix.values[column * 4 + row];
        augmented[row][row + 4] = 1.0;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
                pivot = row;
        if (std::abs(augmented[pivot][column]) < 1.0e-10)
            return std::unexpected("glTF mesh node transform is singular");
        if (pivot != column)
            for (std::size_t k = 0; k < 8; ++k)
                std::swap(augmented[pivot][k], augmented[column][k]);
        const double divisor = augmented[column][column];
        for (double& value : augmented[column])
            value /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column)
                continue;
            const double factor = augmented[row][column];
            for (std::size_t k = 0; k < 8; ++k)
                augmented[row][k] -= factor * augmented[column][k];
        }
    }
    Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result.values[column * 4 + row] = static_cast<float>(augmented[row][column + 4]);
    return result;
}

void build_skin_palette(const astra::assets::Asset& asset, std::uint32_t mesh_node,
                        std::span<const Matrix4> global_nodes, std::vector<Matrix4>& palette) {
    palette.clear();
    if (mesh_node >= asset.nodes.size() || global_nodes.size() != asset.nodes.size())
        throw std::invalid_argument("invalid glTF mesh node or global transform array");
    const auto skin_index = asset.nodes[mesh_node].skin;
    if (skin_index == std::numeric_limits<std::uint32_t>::max())
        return;
    if (skin_index >= asset.skins.size())
        throw std::runtime_error("glTF node references an invalid skin");
    const auto& skin = asset.skins[skin_index];
    auto inverse_mesh = inverse_matrix(global_nodes[mesh_node]);
    if (!inverse_mesh)
        throw std::runtime_error(inverse_mesh.error());
    palette.reserve(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const auto joint = skin.joints[i];
        if (joint >= global_nodes.size())
            throw std::runtime_error("glTF skin joint index is invalid");
        Matrix4 inverse_bind{};
        std::copy(skin.inverse_bind_matrices[i].begin(), skin.inverse_bind_matrices[i].end(),
                  inverse_bind.values.data());
        palette.push_back(multiply(multiply(*inverse_mesh, global_nodes[joint]), inverse_bind));
    }
}

struct MeshBuilder {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;

    void triangle(Vec3 a, Vec3 b, Vec3 c, Color color) {
        const Vec3 normal = normalize(cross(subtract(b, a), subtract(c, a)));
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({a, normal, color});
        vertices.push_back({b, normal, color});
        vertices.push_back({c, normal, color});
        indices.insert(indices.end(), {base, base + 1U, base + 2U});
    }

    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Color color) {
        triangle(a, b, c, color);
        triangle(a, c, d, color);
    }

    void cone(float x, float z, float base_y, float radius, float height, Color color,
              unsigned segments = 9) {
        const Vec3 tip{x, base_y + height, z};
        for (unsigned i = 0; i < segments; ++i) {
            const float a0 = 2.0F * kPi * static_cast<float>(i) / static_cast<float>(segments);
            const float a1 = 2.0F * kPi * static_cast<float>(i + 1U) / static_cast<float>(segments);
            const Vec3 p0{x + std::cos(a0) * radius, base_y, z + std::sin(a0) * radius};
            const Vec3 p1{x + std::cos(a1) * radius, base_y, z + std::sin(a1) * radius};
            triangle(p0, p1, tip, color);
        }
    }

    void tapered_branch(Vec3 start, Vec3 end, float radius_start, float radius_end, Color color,
                        unsigned segments = 6) {
        const Vec3 axis = normalize(subtract(end, start));
        const Vec3 reference =
            std::abs(axis.y) < 0.92F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
        const Vec3 tangent = normalize(cross(axis, reference));
        const Vec3 bitangent = normalize(cross(axis, tangent));
        for (unsigned i = 0; i < segments; ++i) {
            const float a0 = 2.0F * kPi * static_cast<float>(i) / static_cast<float>(segments);
            const float a1 = 2.0F * kPi * static_cast<float>(i + 1U) / static_cast<float>(segments);
            const Vec3 radial0 = add(scale(tangent, std::cos(a0)), scale(bitangent, std::sin(a0)));
            const Vec3 radial1 = add(scale(tangent, std::cos(a1)), scale(bitangent, std::sin(a1)));
            const Vec3 a = add(start, scale(radial0, radius_start));
            const Vec3 b = add(start, scale(radial1, radius_start));
            const Vec3 c = add(end, scale(radial1, radius_end));
            const Vec3 d = add(end, scale(radial0, radius_end));
            quad(a, b, c, d, color);
        }
    }

    void leaf_cluster(Vec3 center, Vec3 radii, Color color, unsigned longitude = 9,
                      unsigned latitude = 5) {
        const auto point = [&](unsigned ring, unsigned segment) {
            const float v = static_cast<float>(ring) / static_cast<float>(latitude);
            const float phi = (v - 0.5F) * kPi;
            const float theta =
                2.0F * kPi * static_cast<float>(segment) / static_cast<float>(longitude);
            const float ring_radius = std::cos(phi);
            return Vec3{center.x + radii.x * ring_radius * std::cos(theta),
                        center.y + radii.y * std::sin(phi),
                        center.z + radii.z * ring_radius * std::sin(theta)};
        };
        for (unsigned ring = 0; ring < latitude; ++ring) {
            for (unsigned segment = 0; segment < longitude; ++segment) {
                const unsigned next = (segment + 1U) % longitude;
                const Vec3 a = point(ring, segment);
                const Vec3 b = point(ring, next);
                const Vec3 c = point(ring + 1U, next);
                const Vec3 d = point(ring + 1U, segment);
                const float variation =
                    0.91F + 0.09F * std::sin(static_cast<float>(segment * 7U + ring * 11U));
                const Color shade{color.red * variation, color.green * variation,
                                  color.blue * variation, 1.0F};
                // Reverse the latitude/longitude winding so the ellipsoid normals
                // face outward under the renderer's back-face culling convention.
                quad(a, d, c, b, shade);
            }
        }
    }

    void grass_blade(Vec3 base, float height, float width, float angle, float lean,
                     Color base_color, Color tip_color) {
        // A narrow, curved ribbon reads as a blade from oblique views; three
        // segments avoid the flat triangular silhouettes of the old tuft mesh.
        constexpr std::array<float, 4> height_fractions{0.0F, 0.34F, 0.70F, 1.0F};
        constexpr std::array<float, 4> width_fractions{1.0F, 0.76F, 0.39F, 0.035F};
        const Vec3 side_axis{std::cos(angle), 0.0F, std::sin(angle)};
        const Vec3 bend_axis{std::cos(angle + 1.1F), 0.0F, std::sin(angle + 1.1F)};
        const auto center_at = [&](float t) {
            return add(add(base, {0.0F, height * t, 0.0F}),
                       scale(bend_axis, lean * t * t));
        };
        const auto color_at = [&](float t) {
            return Color{base_color.red + (tip_color.red - base_color.red) * t,
                         base_color.green + (tip_color.green - base_color.green) * t,
                         base_color.blue + (tip_color.blue - base_color.blue) * t, 1.0F};
        };
        const auto append_vertex = [&](Vec3 position, Vec3 normal, Color color, float bend) {
            MeshVertex vertex{position, normal, color};
            // The Metal scene, previous-frame and shadow paths share this marker.
            // UV1.y is the normalized distance from the rooted, stationary base.
            vertex.texcoord1_v = bend;
            vertex.tangent_x = 1.0F;
            vertex.tangent_w = 0.0F;
            vertices.push_back(vertex);
        };
        for (std::size_t segment = 0; segment + 1U < height_fractions.size(); ++segment) {
            const float t0 = height_fractions[segment];
            const float t1 = height_fractions[segment + 1U];
            const Vec3 center0 = center_at(t0);
            const Vec3 center1 = center_at(t1);
            const Vec3 half_width0 = scale(side_axis, width * width_fractions[segment] * 0.5F);
            const Vec3 half_width1 = scale(side_axis, width * width_fractions[segment + 1U] * 0.5F);
            const Vec3 left0 = subtract(center0, half_width0);
            const Vec3 right0 = add(center0, half_width0);
            const Vec3 left1 = subtract(center1, half_width1);
            const Vec3 right1 = add(center1, half_width1);
            const Vec3 normal = normalize(cross(subtract(right0, left0),
                                                subtract(center1, center0)));
            const auto first = static_cast<std::uint32_t>(vertices.size());
            append_vertex(left0, normal, color_at(t0), t0);
            append_vertex(right0, normal, color_at(t0), t0);
            append_vertex(right1, normal, color_at(t1), t1);
            append_vertex(left1, normal, color_at(t1), t1);
            indices.insert(indices.end(), {first, first + 1U, first + 2U,
                                           first, first + 2U, first + 3U});

            const auto back = static_cast<std::uint32_t>(vertices.size());
            const Vec3 back_normal = scale(normal, -1.0F);
            append_vertex(left0, back_normal, color_at(t0), t0);
            append_vertex(left1, back_normal, color_at(t1), t1);
            append_vertex(right1, back_normal, color_at(t1), t1);
            append_vertex(right0, back_normal, color_at(t0), t0);
            indices.insert(indices.end(), {back, back + 1U, back + 2U,
                                           back, back + 2U, back + 3U});
        }
    }

    void grass_tuft(float x, float z, float ground, std::uint32_t& seed) {
        const float scale_value = random_range(seed, 0.72F, 1.34F);
        const float hue = random_range(seed, 0.0F, 1.0F);
        const Color root{0.055F + hue * 0.035F, 0.12F + hue * 0.075F, 0.025F + hue * 0.025F, 1.0F};
        const Color tip{0.18F + hue * 0.12F, 0.30F + hue * 0.17F, 0.045F + hue * 0.055F, 1.0F};
        const unsigned blades = 8U + (random_step(seed) % 4U);
        for (unsigned blade = 0; blade < blades; ++blade) {
            const float angle = random_range(seed, 0.0F, 2.0F * kPi);
            const float height = random_range(seed, 0.32F, 0.72F) * scale_value;
            const float width = random_range(seed, 0.045F, 0.095F) * scale_value;
            const float lean = random_range(seed, 0.07F, 0.22F) * scale_value;
            const float offset_angle = random_range(seed, 0.0F, 2.0F * kPi);
            const float offset = random_range(seed, 0.0F, 0.13F);
            const Vec3 base{x + std::cos(offset_angle) * offset, ground,
                            z + std::sin(offset_angle) * offset};
            grass_blade(base, height, width, angle, lean, root, tip);
        }
    }

    void tree(float x, float z, float ground, float size) {
        const float trunk_height = size * 1.12F;
        const float trunk_radius = size * 0.075F;
        const Color bark{0.20F, 0.105F, 0.055F, 1.0F};
        tapered_branch({x, ground, z}, {x + size * 0.025F, ground + trunk_height, z}, trunk_radius,
                       trunk_radius * 0.42F, bark, 9);
        for (unsigned branch = 0; branch < 5; ++branch) {
            const float angle = 2.0F * kPi * static_cast<float>(branch) / 5.0F + 0.31F;
            const float y = ground + trunk_height * (0.42F + 0.095F * static_cast<float>(branch));
            const Vec3 start{x, y, z};
            const Vec3 end{x + std::cos(angle) * size * 0.48F, y + size * 0.22F,
                           z + std::sin(angle) * size * 0.48F};
            tapered_branch(start, end, trunk_radius * 0.48F, trunk_radius * 0.12F, bark, 6);
        }
        const Color canopy{0.075F, 0.235F, 0.095F, 1.0F};
        leaf_cluster({x, ground + trunk_height + size * 0.04F, z},
                     {size * 0.48F, size * 0.43F, size * 0.46F}, canopy);
        leaf_cluster({x - size * 0.27F, ground + trunk_height * 0.82F, z + size * 0.08F},
                     {size * 0.34F, size * 0.31F, size * 0.33F}, {0.09F, 0.27F, 0.105F, 1.0F});
        leaf_cluster({x + size * 0.25F, ground + trunk_height * 0.91F, z - size * 0.12F},
                     {size * 0.36F, size * 0.32F, size * 0.35F}, {0.105F, 0.30F, 0.11F, 1.0F});
    }

    void shrub(float x, float z, float ground, float size, std::uint32_t& seed) {
        const float green = random_range(seed, 0.19F, 0.30F);
        const Color foliage{0.055F, green, 0.065F, 1.0F};
        const unsigned lobes = 4U + random_step(seed) % 3U;
        for (unsigned i = 0; i < lobes; ++i) {
            const float angle = 2.0F * kPi * static_cast<float>(i) / static_cast<float>(lobes);
            const float radius = size * random_range(seed, 0.18F, 0.38F);
            const Vec3 center{x + std::cos(angle) * radius,
                              ground + size * random_range(seed, 0.22F, 0.43F),
                              z + std::sin(angle) * radius};
            leaf_cluster(center, {size * 0.34F, size * 0.30F, size * 0.34F}, foliage, 7, 4);
        }
    }

    void rock(float x, float z, float ground, float size, Color color) {
        const Vec3 top{x, ground + size, z};
        const Vec3 bottom{x, ground - size * 0.12F, z};
        const std::array<Vec3, 4> ring{{
            {x + size, ground, z},
            {x, ground + size * 0.08F, z + size * 0.82F},
            {x - size, ground - size * 0.04F, z},
            {x, ground + size * 0.04F, z - size * 0.82F},
        }};
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const auto next = (i + 1U) % ring.size();
            triangle(top, ring[i], ring[next], color);
            triangle(bottom, ring[next], ring[i], color);
        }
    }
};

MeshBuilder build_chunk_terrain(const astra::world::TerrainChunk& chunk, unsigned step) {
    MeshBuilder mesh;
    step = std::max(step, 1U);
    std::vector<std::uint32_t> coordinates;
    for (std::uint32_t value = 0; value <= chunk.resolution; value += step)
        coordinates.push_back(value);
    if (coordinates.empty() || coordinates.back() != chunk.resolution)
        coordinates.push_back(chunk.resolution);
    const auto side = static_cast<std::uint32_t>(coordinates.size());
    const float spacing = chunk.size / static_cast<float>(chunk.resolution);
    mesh.vertices.reserve(static_cast<std::size_t>(side) * side + side * 8U);
    mesh.indices.reserve(static_cast<std::size_t>(side - 1U) * (side - 1U) * 6U + side * 24U);

    const auto vertex_index = [side](std::uint32_t x, std::uint32_t z) { return z * side + x; };
    for (const auto z : coordinates) {
        for (const auto x : coordinates) {
            const auto left = x == 0 ? x : x - 1U;
            const auto right = std::min(x + 1U, chunk.resolution);
            const auto down = z == 0 ? z : z - 1U;
            const auto up = std::min(z + 1U, chunk.resolution);
            const float dx = (chunk.height_at(right, z) - chunk.height_at(left, z)) /
                             (std::max(static_cast<float>(right - left), 1.0F) * spacing);
            const float dz = (chunk.height_at(x, up) - chunk.height_at(x, down)) /
                             (std::max(static_cast<float>(up - down), 1.0F) * spacing);
            const float world_x = chunk.world_origin_x + static_cast<float>(x) * spacing;
            const float world_z = chunk.world_origin_z + static_cast<float>(z) * spacing;
            MeshVertex vertex{{world_x, chunk.height_at(x, z), world_z},
                              normalize({-dx, 1.0F, -dz}),
                              {1.0F, 1.0F, 1.0F, 1.0F}};
            vertex.texcoord_u = world_x * 0.055F;
            vertex.texcoord_v = world_z * 0.055F;
            vertex.tangent_x = 1.0F;
            vertex.tangent_w = -1.0F;
            mesh.vertices.push_back(vertex);
        }
    }
    for (std::uint32_t z = 0; z + 1U < side; ++z) {
        for (std::uint32_t x = 0; x + 1U < side; ++x) {
            const auto a = vertex_index(x, z);
            const auto b = vertex_index(x + 1U, z);
            const auto c = vertex_index(x, z + 1U);
            const auto d = vertex_index(x + 1U, z + 1U);
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }

    // Downward skirts conceal small cracks where adjacent chunks use different LODs.
    constexpr float skirt_depth = 1.8F;
    const auto add_skirt = [&](const std::vector<std::uint32_t>& edge) {
        for (std::size_t i = 0; i + 1U < edge.size(); ++i) {
            const auto a = edge[i];
            const auto b = edge[i + 1U];
            auto lower_a = mesh.vertices[a];
            auto lower_b = mesh.vertices[b];
            lower_a.position.y -= skirt_depth;
            lower_b.position.y -= skirt_depth;
            const auto lower_a_index = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(lower_a);
            const auto lower_b_index = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(lower_b);
            mesh.indices.insert(mesh.indices.end(),
                                {a, lower_a_index, b, b, lower_a_index, lower_b_index});
        }
    };
    std::vector<std::uint32_t> edge;
    edge.reserve(side);
    for (std::uint32_t x = 0; x < side; ++x)
        edge.push_back(vertex_index(x, 0));
    add_skirt(edge);
    edge.clear();
    for (std::uint32_t z = 0; z < side; ++z)
        edge.push_back(vertex_index(side - 1U, z));
    add_skirt(edge);
    edge.clear();
    for (std::uint32_t x = side; x-- > 0;)
        edge.push_back(vertex_index(x, side - 1U));
    add_skirt(edge);
    edge.clear();
    for (std::uint32_t z = side; z-- > 0;)
        edge.push_back(vertex_index(0, z));
    add_skirt(edge);
    return mesh;
}

enum class VegetationAssetKind : std::uint8_t {
    jacaranda,
    island_tree,
    fir_sapling,
    shrub,
    fern,
    rock
};

struct VegetationPlacement final {
    VegetationAssetKind kind{};
    Vec3 position{};
    float yaw{};
    float scale{1.0F};
    astra::world::TerrainLod visible_through{astra::world::TerrainLod::near};
};

std::vector<VegetationPlacement> build_chunk_placements(const astra::world::TerrainChunk& chunk) {
    std::vector<VegetationPlacement> placements;
    placements.reserve(12);
    std::uint32_t seed = 0xA57A1234U ^ (static_cast<std::uint32_t>(chunk.coord.x) * 0x9E3779B9U) ^
                         (static_cast<std::uint32_t>(chunk.coord.z) * 0x85EBCA6BU);
    const auto place = [&](VegetationAssetKind kind, astra::world::TerrainLod visible_through,
                           float minimum_scale, float maximum_scale, float margin,
                           float minimum_spacing) {
        for (unsigned attempt = 0; attempt < 24; ++attempt) {
            const float x = chunk.world_origin_x + random_range(seed, margin, chunk.size - margin);
            const float z = chunk.world_origin_z + random_range(seed, margin, chunk.size - margin);
            const float dx = (terrain_height(x + 0.6F, z) - terrain_height(x - 0.6F, z)) / 1.2F;
            const float dz = (terrain_height(x, z + 0.6F) - terrain_height(x, z - 0.6F)) / 1.2F;
            if (dx * dx + dz * dz > 0.72F)
                continue;
            const float spacing_squared = minimum_spacing * minimum_spacing;
            const bool overlaps =
                std::any_of(placements.begin(), placements.end(),
                            [x, z, spacing_squared](const auto& existing) {
                                const float offset_x = x - existing.position.x;
                                const float offset_z = z - existing.position.z;
                                return offset_x * offset_x + offset_z * offset_z < spacing_squared;
                            });
            if (overlaps)
                continue;
            placements.push_back({kind,
                                  {x, terrain_height(x, z), z},
                                  random_range(seed, 0.0F, 2.0F * kPi),
                                  random_range(seed, minimum_scale, maximum_scale),
                                  visible_through});
            return;
        }
    };

    // A stable species mix: mature broadleaf trees, a smaller coastal species and
    // a three-sapling cluster. The first mature tree survives in the far terrain tier.
    place(VegetationAssetKind::jacaranda, astra::world::TerrainLod::far, 0.44F, 0.53F, 8.0F, 10.0F);
    place(VegetationAssetKind::island_tree, astra::world::TerrainLod::medium, 1.00F, 1.20F, 7.0F,
          8.0F);
    place(VegetationAssetKind::jacaranda, astra::world::TerrainLod::medium, 0.40F, 0.49F, 8.0F,
          10.0F);
    place(VegetationAssetKind::fir_sapling, astra::world::TerrainLod::medium, 1.45F, 1.85F, 5.0F,
          6.0F);
    place(VegetationAssetKind::island_tree, astra::world::TerrainLod::near, 0.92F, 1.12F, 7.0F,
          8.0F);
    place(VegetationAssetKind::island_tree, astra::world::TerrainLod::near, 0.82F, 1.00F, 7.0F,
          7.0F);

    // Understory is deliberately near-only; each fern/rock asset is a small natural cluster.
    place(VegetationAssetKind::shrub, astra::world::TerrainLod::near, 2.5F, 3.4F, 3.0F, 4.0F);
    place(VegetationAssetKind::fern, astra::world::TerrainLod::near, 0.62F, 0.82F, 3.0F, 2.5F);
    place(VegetationAssetKind::fern, astra::world::TerrainLod::near, 0.55F, 0.75F, 3.0F, 2.5F);
    place(VegetationAssetKind::rock, astra::world::TerrainLod::near, 0.20F, 0.27F, 3.0F, 3.0F);
    return placements;
}

MeshBuilder build_chunk_vegetation(const astra::world::TerrainChunk& chunk,
                                   astra::world::TerrainLod lod,
                                   std::span<const VegetationPlacement> placements) {
    MeshBuilder mesh;
    const auto density = astra::world::vegetation_lod_density(lod);
    std::uint32_t seed = 0xA57A1234U ^ (static_cast<std::uint32_t>(chunk.coord.x) * 0x9E3779B9U) ^
                         (static_cast<std::uint32_t>(chunk.coord.z) * 0x85EBCA6BU);
    // Deterministic curved grass ribbons complement the scanned tree and understory
    // assets. Their root-to-tip bend weights feed the same GPU wind path as foliage.
    for (unsigned i = 0; i < density.grass_clumps; ++i) {
        if (random_range(seed, 0.0F, 1.0F) < 0.055F)
            continue;
        const float x = chunk.world_origin_x + random_range(seed, 1.0F, chunk.size - 1.0F);
        const float z = chunk.world_origin_z + random_range(seed, 1.0F, chunk.size - 1.0F);
        const float ground = terrain_height(x, z);
        const float dx = (terrain_height(x + 0.45F, z) - terrain_height(x - 0.45F, z)) / 0.9F;
        const float dz = (terrain_height(x, z + 0.45F) - terrain_height(x, z - 0.45F)) / 0.9F;
        if (dx * dx + dz * dz > 0.72F)
            continue;
        const bool under_canopy = std::any_of(
            placements.begin(), placements.end(), [x, z](const VegetationPlacement& placement) {
                if (placement.kind != VegetationAssetKind::jacaranda &&
                    placement.kind != VegetationAssetKind::island_tree &&
                    placement.kind != VegetationAssetKind::fir_sapling)
                    return false;
                const float dx_tree = x - placement.position.x;
                const float dz_tree = z - placement.position.z;
                const float radius =
                    placement.kind == VegetationAssetKind::jacaranda     ? 11.0F * placement.scale
                    : placement.kind == VegetationAssetKind::island_tree ? 4.4F * placement.scale
                                                                         : 1.4F * placement.scale;
                return dx_tree * dx_tree + dz_tree * dz_tree < radius * radius;
            });
        if (!under_canopy)
            mesh.grass_tuft(x, z, ground, seed);
    }
    return mesh;
}

struct TerrainMaterialResources final {
    std::vector<std::unique_ptr<astra::rhi::GpuTexture>> textures;
    astra::rhi::MaterialData material;
};

struct GpuChunk final {
    astra::geometry::VirtualMesh virtual_geometry;
    std::vector<astra::geometry::Vec3> source_positions;
    std::optional<astra::geometry::PackedVirtualMesh> packed_virtual_geometry;
    std::unique_ptr<astra::rhi::GpuVirtualGeometry> gpu_virtual_geometry;
    astra::geometry::VirtualGeometryPageId page_id{
        astra::geometry::invalid_virtual_geometry_page};
    std::size_t page_bytes{};
    bool page_upload_ready{};
    // Keep only the active grass LOD; retaining all visited tiers multiplies chunk memory.
    std::unique_ptr<astra::rhi::GpuMesh> vegetation_mesh;
    astra::world::TerrainLod vegetation_mesh_lod{astra::world::TerrainLod::near};
    bool vegetation_mesh_built{};
    std::vector<VegetationPlacement> vegetation_instances;
    astra::world::TerrainLod lod{astra::world::TerrainLod::near};
#if defined(ASTRA_HAS_PHYSICS)
    std::optional<astra::physics::BodyHandle> terrain_collider;
#endif
};

struct NatureAssets final {
    RuntimeModel jacaranda;
    RuntimeModel island_tree;
    RuntimeModel fir_sapling;
    RuntimeModel shrub;
    RuntimeModel fern;
    RuntimeModel rock;

    [[nodiscard]] const RuntimeModel& get(VegetationAssetKind kind) const {
        switch (kind) {
        case VegetationAssetKind::jacaranda:
            return jacaranda;
        case VegetationAssetKind::island_tree:
            return island_tree;
        case VegetationAssetKind::fir_sapling:
            return fir_sapling;
        case VegetationAssetKind::shrub:
            return shrub;
        case VegetationAssetKind::fern:
            return fern;
        case VegetationAssetKind::rock:
            return rock;
        }
        throw std::logic_error("Unknown vegetation asset kind.");
    }
};

Matrix4 make_vegetation_instance_transform(const RuntimeModel& model,
                                           const VegetationPlacement& placement) {
    const Vec3 center{(model.bounds_min.x + model.bounds_max.x) * 0.5F, model.bounds_min.y,
                      (model.bounds_min.z + model.bounds_max.z) * 0.5F};
    return multiply(multiply(multiply(translation(placement.position.x, placement.position.y,
                                                  placement.position.z),
                                      rotation_y(placement.yaw)),
                             scaling(placement.scale, placement.scale, placement.scale)),
                    translation(-center.x, -center.y, -center.z));
}

std::expected<TerrainMaterialResources, std::string>
create_terrain_material(astra::renderer::Renderer& renderer) {
    TerrainMaterialResources result;
    const auto upload_file =
        [&](std::string_view filename,
            bool srgb) -> std::expected<const astra::rhi::GpuTexture*, std::string> {
        const auto path =
            std::filesystem::path{ASTRA_ASSET_ROOT} / "terrain" / "forest_ground_04" / filename;
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return std::unexpected("Could not open terrain texture: " + path.string());
        const auto end = stream.tellg();
        if (end <= 0)
            return std::unexpected("Terrain texture is empty: " + path.string());
        std::vector<std::byte> encoded(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(encoded.data()),
                    static_cast<std::streamsize>(encoded.size()));
        if (!stream)
            return std::unexpected("Could not read terrain texture: " + path.string());
        const std::string mime = path.extension() == ".jpg" ? "image/jpeg" : "image/png";
        auto decoded = astra::assets::decode_image(encoded, mime);
        if (!decoded)
            return std::unexpected("Could not decode terrain texture " + path.string() + ": " +
                                   decoded.error().message);
        auto texture =
            renderer.create_texture({decoded->width, decoded->height, decoded->rgba8, srgb});
        if (!texture)
            return std::unexpected(texture.error().message);
        const auto* pointer = texture->get();
        result.textures.push_back(std::move(*texture));
        return pointer;
    };

    auto albedo_texture = upload_file("forest_ground_04_diff_1k.jpg", true);
    auto normal_texture = upload_file("forest_ground_04_nor_gl_1k.png", false);
    auto arm_texture = upload_file("forest_ground_04_arm_1k.png", false);
    if (!albedo_texture || !normal_texture || !arm_texture)
        return std::unexpected(!albedo_texture   ? albedo_texture.error()
                               : !normal_texture ? normal_texture.error()
                                                 : arm_texture.error());

    result.material.base_color_texture = *albedo_texture;
    result.material.normal_texture = *normal_texture;
    // Poly Haven's ARM map packs AO in R, roughness in G and metallic in B.
    result.material.metallic_roughness_texture = *arm_texture;
    result.material.occlusion_texture = *arm_texture;
    result.material.base_color = {1.0F, 1.0F, 1.0F, 1.0F};
    result.material.metallic = 0.0F;
    result.material.roughness = 0.88F;
    result.material.normal_scale = 1.0F;
    result.material.occlusion_strength = 0.85F;
    return result;
}

} // namespace

int main() {
    astra::core::Logger logger{astra::core::LogLevel::info};
    auto game_result = astra::game::Game::create({
        .window =
            {
                .title = "Astra - Open World Walkthrough",
                .width = 1024,
                .height = 640,
                .resizable = true,
                // Keep the Retina drawable; the renderer uses TAA-assisted 0.75x internal resolution.
                .high_dpi = true,
            },
    });
    if (!game_result) {
        logger.log(astra::core::LogLevel::critical, "Game", game_result.error().message);
        return 1;
    }
    auto game = std::move(*game_result);
    auto& window = game.window();
    auto& renderer = game.renderer();
    // Metal initializes the layer with display sync enabled. The example owns
    // pacing so menu-selected caps above the display refresh can take effect.
    window.set_display_sync(false);
    const std::array settings_menu{
        astra::platform::WindowMenuItem{"Resolution: 50%", resolution_50},
        astra::platform::WindowMenuItem{"Resolution: 65%", resolution_65},
        astra::platform::WindowMenuItem{"Resolution: 80%", resolution_80},
        astra::platform::WindowMenuItem{"Resolution: 100%", resolution_100},
        astra::platform::WindowMenuItem{"", 0, true},
        astra::platform::WindowMenuItem{"Max FPS: 30", fps_30},
        astra::platform::WindowMenuItem{"Max FPS: 60", fps_60},
        astra::platform::WindowMenuItem{"Max FPS: 90", fps_90},
        astra::platform::WindowMenuItem{"Max FPS: 120", fps_120},
        astra::platform::WindowMenuItem{"Max FPS: 144", fps_144},
        astra::platform::WindowMenuItem{"Max FPS: Unlimited", fps_unlimited},
        astra::platform::WindowMenuItem{"", 0, true},
        astra::platform::WindowMenuItem{"Toggle Adaptive Resolution", toggle_adaptive_resolution},
        astra::platform::WindowMenuItem{"Toggle Performance Telemetry", toggle_performance_telemetry},
        astra::platform::WindowMenuItem{"Toggle Benchmark Mode", toggle_benchmark_mode},
    };
    window.set_menu_items(settings_menu);

    auto character_result = load_runtime_model(
        renderer,
        std::filesystem::path{ASTRA_ASSET_ROOT} / "characters" / "AstraPlayer-runtime.glb", logger);
    if (!character_result) {
        logger.log(astra::core::LogLevel::critical, "Character", character_result.error());
        return 1;
    }
    auto character = std::move(*character_result);
    // Keep the character's node hierarchy, skin data and animation clips only.
    // Imported CPU vertex/index arrays and encoded images duplicate uploaded GPU assets.
    release_imported_geometry_and_images(character.source);
    const auto find_animation = [&](std::string_view name) {
        for (std::size_t i = 0; i < character.source.animations.size(); ++i)
            if (character.source.animations[i].name == name)
                return i;
        return std::size_t{0};
    };
    const std::size_t idle_animation = find_animation("Idle");
    const std::size_t walk_animation = find_animation("Walk");
    const std::size_t run_animation = find_animation("Run");

    NatureAssets nature;
    const auto load_nature_asset = [&](std::string_view relative_path, RuntimeModel& destination) {
        auto loaded = load_runtime_model(
            renderer,
            std::filesystem::path{ASTRA_ASSET_ROOT} / "nature" / "runtime" / relative_path, logger);
        if (!loaded) {
            logger.log(astra::core::LogLevel::critical, "Vegetation", loaded.error());
            return false;
        }
        destination = std::move(*loaded);
        try {
            prepare_static_runtime_model(destination);
            // GPU meshes/textures and precomputed transforms now own everything the
            // vegetation renderer needs. Release the decoded GLB CPU payloads.
            destination.source = {};
        } catch (const std::exception& error) {
            logger.log(astra::core::LogLevel::critical, "Vegetation", error.what());
            return false;
        }
        return true;
    };
    if (!load_nature_asset("jacaranda_tree.glb", nature.jacaranda) ||
        !load_nature_asset("island_tree_02.glb", nature.island_tree) ||
        !load_nature_asset("fir_sapling.glb", nature.fir_sapling) ||
        !load_nature_asset("shrub_04.glb", nature.shrub) ||
        !load_nature_asset("fern_02.glb", nature.fern) ||
        !load_nature_asset("rock_moss_set_01.glb", nature.rock))
        return 1;

    auto terrain_material_result = create_terrain_material(renderer);
    if (!terrain_material_result) {
        logger.log(astra::core::LogLevel::critical, "Terrain", terrain_material_result.error());
        return 1;
    }
    auto terrain_material = std::move(*terrain_material_result);

    // Feed the same outdoor sky/sun used by the scene into the renderer's full
    // diffuse-irradiance + GGX-prefiltered specular IBL path. This replaces the
    // much cheaper analytic ambient approximation for the nature scene.
    const Vec3 light_direction{-0.42F, -0.82F, -0.31F};
    constexpr std::uint32_t environment_width = 512;
    constexpr std::uint32_t environment_height = 256;
    auto environment_pixels =
        make_outdoor_environment(environment_width, environment_height,
                                 scale(light_direction, -1.0F));
    const astra::rhi::EnvironmentData environment_data{
        .width = environment_width,
        .height = environment_height,
        .rgba32f = environment_pixels,
        .environment_resolution = 128,
        .irradiance_resolution = 16,
        .prefilter_resolution = 128,
        .prefilter_mip_count = 5,
        .brdf_lut_resolution = 128,
    };
    auto environment_result = renderer.create_environment(environment_data);
    std::unique_ptr<astra::rhi::GpuEnvironment> outdoor_environment;
    if (environment_result) {
        outdoor_environment = std::move(*environment_result);
        logger.log(astra::core::LogLevel::info, "Lighting",
                   "Outdoor HDR environment, diffuse irradiance and GGX specular prefilter are ready.");
    } else {
        logger.log(astra::core::LogLevel::warning, "Lighting",
                   "Could not create outdoor image-based lighting; using the analytic sky fallback: " +
                       environment_result.error().message);
    }
    environment_pixels.clear();
    environment_pixels.shrink_to_fit();

    astra::core::ThreadPool streaming_workers{2};
    astra::world::WorldStreamerConfig streaming_config;
    streaming_config.chunk_size = 64.0F;
    streaming_config.resolution = 32;
    streaming_config.load_radius = 1;
    streaming_config.unload_radius = 1;
    streaming_config.max_in_flight = 2;
    streaming_config.max_resident_chunks = 9;
    streaming_config.medium_lod_distance = 48.0F;
    streaming_config.far_lod_distance = 96.0F;
    astra::world::WorldStreamer streamer{streaming_workers, streaming_config, terrain_height};
    // Reserve the highest valid id for the pinned world root. Chunk ids are a
    // bijective coordinate encoding, so explicitly reject this reserved value.
    constexpr astra::geometry::VirtualGeometryPageId world_geometry_root_page =
        astra::geometry::invalid_virtual_geometry_page - 1U;
    astra::geometry::VirtualGeometryPageCache geometry_pages{256U * 1024U * 1024U};
    const auto chunk_geometry_page_id = [](astra::world::ChunkCoord coord) {
        const auto x = static_cast<std::uint32_t>(coord.x) ^ 0x80000000U;
        const auto z = static_cast<std::uint32_t>(coord.z) ^ 0x80000000U;
        return (static_cast<std::uint64_t>(x) << 32U) | z;
    };
    if (auto registered = geometry_pages.register_page(
            {world_geometry_root_page, astra::geometry::invalid_virtual_geometry_page, 1});
        !registered || !geometry_pages.mark_resident(world_geometry_root_page, 0)) {
        logger.log(astra::core::LogLevel::critical, "GeometryStreaming",
                   registered ? "Could not make the world root geometry page resident."
                               : registered.error());
        return 1;
    }
#if defined(ASTRA_HAS_PHYSICS)
    auto physics_result = astra::physics::PhysicsWorld::create({
        .gravity = {0.0F, -24.0F, 0.0F},
        .fixed_step_seconds = 1.0F / 60.0F,
        .max_substeps = 4,
        .max_bodies = 256,
        .worker_threads = 2,
    });
    if (!physics_result) {
        logger.log(astra::core::LogLevel::critical, "Physics", physics_result.error().message);
        return 1;
    }
    auto physics_world = std::move(*physics_result);
    auto player_result = physics_world.create_character({
        .capsule_half_height = 0.52F,
        .capsule_radius = 0.38F,
        .position = {0.0F, terrain_height(0.0F, 12.0F), 12.0F},
        .mass = 70.0F,
        .max_strength = 100.0F,
        .max_slope_angle_radians = 50.0F * kPi / 180.0F,
    });
    if (!player_result) {
        logger.log(astra::core::LogLevel::critical, "Physics", player_result.error().message);
        return 1;
    }
    const auto player = *player_result;
#else
    astra::world::CharacterController player;
    player.set_position({0.0F, terrain_height(0.0F, 12.0F), 12.0F});
#endif

    std::map<astra::world::ChunkCoord, GpuChunk> gpu_chunks;

    logger.log(astra::core::LogLevel::info, "Renderer",
               "Initialized GPU: " + renderer.device_name());
    logger.log(
        astra::core::LogLevel::info, "World",
        "Started asynchronous chunk streaming with three terrain LOD tiers and bounded memory.");
    logger.log(astra::core::LogLevel::info, "Character",
               "Loaded the high-detail Astra player with a 69-joint rig, PBR textures, and "
               "idle/walk/run animation.");
#if defined(ASTRA_HAS_PHYSICS)
    logger.log(astra::core::LogLevel::info, "Physics",
               "Player movement uses Jolt CharacterVirtual; streamed terrain chunks register static triangle-mesh colliders.");
#endif
    logger.log(astra::core::LogLevel::info, "Vegetation",
               "Loaded CC0 scanned tree, sapling, shrub, fern, and mossy-rock assets with shared "
               "wind deformation.");
    logger.log(
        astra::core::LogLevel::info, "Controls",
        "WASD: move | Shift: sprint | Space: jump | Mouse drag: orbit | Scroll: zoom | "
        "Settings menu: resolution, FPS cap, adaptive resolution, telemetry, benchmark mode");

    float yaw = -kPi * 0.5F;
    float character_yaw = kPi;
    float pitch = 0.12F;
    float camera_distance = 6.2F;
    float world_time_seconds = 0.0F;
    // Keep the presentation at native drawable resolution while adapting the
    // HDR/TAA workload to the GPU's measured frame time. Changes are deliberately
    // coarse and infrequent: reallocating temporal targets every few frames would
    // cost more memory bandwidth than it saves.
    float render_scale = kRenderScale;
    bool adaptive_resolution = true;
    bool performance_telemetry = true;
    bool benchmark_mode = false;
    bool display_sync_enabled = false;
    int maximum_fps = 60;
    float saved_render_scale = render_scale;
    int saved_maximum_fps = maximum_fps;
    bool saved_adaptive_resolution = adaptive_resolution;
    bool saved_performance_telemetry = performance_telemetry;
    bool saved_display_sync_enabled = display_sync_enabled;
    BenchmarkCsvWriter benchmark_csv;
    auto benchmark_started = std::chrono::steady_clock::now();
    std::uint64_t benchmark_frames = 0;
    double benchmark_cpu_milliseconds = 0.0;
    double benchmark_gpu_milliseconds = 0.0;
    float active_clip_time = 0.0F;
    float previous_clip_time = 0.0F;
    float animation_blend_elapsed = 0.18F;
    std::size_t active_clip_index = idle_animation;
    std::size_t previous_clip_index = idle_animation;
    constexpr float animation_blend_duration = 0.18F;
    Matrix4 previous_view_projection{};
    bool has_previous_view_projection = false;
    Vec3 previous_camera_position{};
    Matrix4 previous_character_transform{};
    std::vector<Matrix4> previous_global_nodes;
    std::vector<std::vector<Matrix4>> previous_skin_palettes(character.source.nodes.size());
    std::vector<std::vector<Matrix4>> skin_palettes(character.source.nodes.size());
    std::vector<std::vector<float>> previous_node_morph_weights(character.source.nodes.size());
    std::vector<bool> skin_palette_built(character.source.nodes.size());
    std::vector<DrawItem> draw_items;
    draw_items.reserve(128);
    std::array<std::vector<Matrix4>, 6> vegetation_instance_transforms;
    for (auto& instances : vegetation_instance_transforms)
        instances.reserve(192);
    float performance_elapsed = 0.0F;
    double performance_cpu_milliseconds = 0.0;
    double performance_gpu_milliseconds = 0.0;
    std::uint32_t performance_frames = 0;
    double displayed_fps = 0.0;
    double displayed_cpu_ms = 0.0;
    double displayed_gpu_ms = 0.0;
    float telemetry_title_elapsed = 0.0F;
    bool has_previous_character_transform = false;

    std::uint64_t frame_number = 0;
    game.run([&](astra::game::Frame& frame) {
        ++frame_number;
        const auto frame_cpu_start = std::chrono::steady_clock::now();
        const auto& input = frame.input;
        if (input.pressed(astra::game::Key::escape)) {
            window.request_close();
            return;
        }
        if (!window.is_open())
            return;

        if (!benchmark_mode) {
            if (input.menu_command_pressed(resolution_50)) {
                render_scale = 0.50F;
                adaptive_resolution = false;
            } else if (input.menu_command_pressed(resolution_65)) {
                render_scale = 0.65F;
                adaptive_resolution = false;
            } else if (input.menu_command_pressed(resolution_80)) {
                render_scale = 0.80F;
                adaptive_resolution = false;
            } else if (input.menu_command_pressed(resolution_100)) {
                render_scale = 1.0F;
                adaptive_resolution = false;
            }
            if (input.menu_command_pressed(fps_30))
                maximum_fps = 30;
            if (input.menu_command_pressed(fps_60))
                maximum_fps = 60;
            if (input.menu_command_pressed(fps_90))
                maximum_fps = 90;
            if (input.menu_command_pressed(fps_120))
                maximum_fps = 120;
            if (input.menu_command_pressed(fps_144))
                maximum_fps = 144;
            if (input.menu_command_pressed(fps_unlimited))
                maximum_fps = 0;
            if (input.menu_command_pressed(toggle_adaptive_resolution))
                adaptive_resolution = !adaptive_resolution;
            if (input.menu_command_pressed(toggle_performance_telemetry))
                performance_telemetry = !performance_telemetry;
        }
        if (input.menu_command_pressed(toggle_benchmark_mode)) {
            if (!benchmark_mode) {
                saved_render_scale = render_scale;
                saved_maximum_fps = maximum_fps;
                saved_adaptive_resolution = adaptive_resolution;
                saved_performance_telemetry = performance_telemetry;
                saved_display_sync_enabled = display_sync_enabled;
                benchmark_mode = true;
                benchmark_started = std::chrono::steady_clock::now();
                benchmark_frames = 0;
                benchmark_cpu_milliseconds = 0.0;
                benchmark_gpu_milliseconds = 0.0;
                render_scale = 1.0F;
                maximum_fps = 0;
                adaptive_resolution = false;
                performance_telemetry = false;
                display_sync_enabled = false;
                window.set_display_sync(display_sync_enabled);
            } else {
                const auto ended = std::chrono::steady_clock::now();
                const double elapsed_seconds =
                    std::chrono::duration<double>(ended - benchmark_started).count();
                if (benchmark_frames != 0U && elapsed_seconds > 0.0) {
                    benchmark_csv.enqueue({
                        .elapsed_seconds = elapsed_seconds,
                        .frames = benchmark_frames,
                        .average_fps = static_cast<double>(benchmark_frames) / elapsed_seconds,
                        .average_cpu_ms =
                            benchmark_cpu_milliseconds / static_cast<double>(benchmark_frames),
                        .average_gpu_ms =
                            benchmark_gpu_milliseconds / static_cast<double>(benchmark_frames),
                        .render_scale = render_scale,
                        .draw_count = draw_items.size(),
                        .resident_chunks = gpu_chunks.size(),
                    });
                }
                benchmark_mode = false;
                render_scale = saved_render_scale;
                maximum_fps = saved_maximum_fps;
                adaptive_resolution = saved_adaptive_resolution;
                performance_telemetry = saved_performance_telemetry;
                display_sync_enabled = saved_display_sync_enabled;
                window.set_display_sync(display_sync_enabled);
            }
        }

        const float delta_time = std::clamp(frame.delta_time, 0.0F, 0.05F);
        camera_distance = std::clamp(camera_distance - input.scroll_y() * 0.65F, 2.4F, 15.0F);
        if (input.down(astra::game::MouseButton::left)) {
            yaw += input.mouse_delta_x() * 0.004F;
            pitch = std::clamp(pitch - input.mouse_delta_y() * 0.004F, -1.45F, 1.45F);
        }
        world_time_seconds += delta_time;

        const Vec3 forward{std::cos(yaw), 0.0F, std::sin(yaw)};
        const Vec3 right{-std::sin(yaw), 0.0F, std::cos(yaw)};
        Vec3 movement{};
        if (input.down(astra::game::Key::w) || input.down(astra::game::Key::up))
            movement = add(movement, forward);
        if (input.down(astra::game::Key::s) || input.down(astra::game::Key::down))
            movement = subtract(movement, forward);
        if (input.down(astra::game::Key::a) || input.down(astra::game::Key::left))
            movement = subtract(movement, right);
        if (input.down(astra::game::Key::d) || input.down(astra::game::Key::right))
            movement = add(movement, right);
        const float movement_length = std::sqrt(dot(movement, movement));
        const bool is_moving = movement_length > 0.001F;
        const bool sprinting =
            input.down(astra::game::Key::left_shift) || input.down(astra::game::Key::right_shift);
        if (is_moving)
            character_yaw = std::atan2(movement.x, movement.z);
#if defined(ASTRA_HAS_PHYSICS)
        const float inverse_move_length = movement_length > 1.0F ? 1.0F / movement_length : 1.0F;
        const float movement_speed = 7.0F * (sprinting ? 1.9F : 1.0F);
        if (!physics_world.set_horizontal_velocity(
                player, {movement.x * inverse_move_length * movement_speed, 0.0F,
                         movement.z * inverse_move_length * movement_speed})) {
            logger.log(astra::core::LogLevel::error, "Physics", "Could not update player velocity.");
            window.request_close();
            return;
        }
        if (input.pressed(astra::game::Key::space) && !physics_world.jump(player, 8.0F)) {
            // A jump request can be rejected only for an invalid character or speed;
            // airborne jump buffering is retained by the physics world.
            logger.log(astra::core::LogLevel::warning, "Physics", "Player jump request was rejected.");
        }
        if (auto stepped = physics_world.step(delta_time); !stepped) {
            logger.log(astra::core::LogLevel::error, "Physics", stepped.error().message);
            window.request_close();
            return;
        }
        const auto player_state = physics_world.character_state(player);
        if (!player_state) {
            logger.log(astra::core::LogLevel::error, "Physics", player_state.error().message);
            window.request_close();
            return;
        }
        const auto position = Vec3{player_state->position.x, player_state->position.y,
                                   player_state->position.z};
#else
        player.step(delta_time,
                    {movement.x, movement.z, sprinting ? 1.9F : 1.0F,
                     input.pressed(astra::game::Key::space)},
                    terrain_height);
        const auto position =
            Vec3{player.state().position.x, player.state().position.y, player.state().position.z};
#endif
        if (!character.source.animations.empty()) {
            const std::size_t requested_clip = !is_moving  ? idle_animation
                                               : sprinting ? run_animation
                                                           : walk_animation;
            if (requested_clip != active_clip_index) {
                previous_clip_index = active_clip_index;
                previous_clip_time = active_clip_time;
                active_clip_index = requested_clip;
                active_clip_time = 0.0F;
                animation_blend_elapsed = 0.0F;
            }
            active_clip_time += delta_time;
            if (animation_blend_elapsed < animation_blend_duration) {
                previous_clip_time += delta_time;
                animation_blend_elapsed =
                    std::min(animation_blend_duration, animation_blend_elapsed + delta_time);
            }
        }

        streamer.update(position.x, position.z);
        for (const auto& [coord, chunk] : streamer.chunks()) {
            const auto info = streamer.metadata().find(coord);
            const auto lod = info == streamer.metadata().end() ? astra::world::TerrainLod::near
                                                               : info->second.lod;
            auto found = gpu_chunks.find(coord);
            if (found != gpu_chunks.end()) {
                found->second.lod = lod;
                continue;
            }
            auto terrain_data = build_chunk_terrain(chunk, 1U);
            GpuChunk gpu_chunk;
            gpu_chunk.lod = lod;
            gpu_chunk.vegetation_instances = build_chunk_placements(chunk);
            gpu_chunk.source_positions.reserve(terrain_data.vertices.size());
            for (const auto& vertex : terrain_data.vertices)
                gpu_chunk.source_positions.push_back(
                    {vertex.position.x, vertex.position.y, vertex.position.z});
            auto virtual_geometry = astra::geometry::build_virtual_mesh(
                gpu_chunk.source_positions, terrain_data.indices,
                astra::geometry::BuildLimits{64, 124, 4, 256});
            if (!virtual_geometry) {
                logger.log(astra::core::LogLevel::error, "Geometry",
                           "Could not build terrain meshlets: " + virtual_geometry.error().message);
                continue;
            }
            gpu_chunk.virtual_geometry = std::move(*virtual_geometry);
            auto packed_virtual_geometry = astra::geometry::pack_virtual_mesh(
                gpu_chunk.virtual_geometry, gpu_chunk.source_positions);
            if (!packed_virtual_geometry) {
                logger.log(astra::core::LogLevel::error, "GeometryStreaming",
                           "Could not pack terrain geometry page: " +
                               packed_virtual_geometry.error().message);
                continue;
            }
            gpu_chunk.page_id = chunk_geometry_page_id(coord);
            if (gpu_chunk.page_id == world_geometry_root_page ||
                gpu_chunk.page_id == astra::geometry::invalid_virtual_geometry_page) {
                logger.log(astra::core::LogLevel::error, "GeometryStreaming",
                           "Terrain chunk coordinate is reserved for virtual geometry page metadata.");
#if defined(ASTRA_HAS_PHYSICS)
                if (gpu_chunk.terrain_collider)
                    (void)physics_world.destroy_body(*gpu_chunk.terrain_collider);
#endif
                continue;
            }
            gpu_chunk.page_bytes =
                packed_virtual_geometry->vertices.size() * sizeof(MeshVertex) +
                packed_virtual_geometry->indices.size() * sizeof(std::uint32_t) +
                packed_virtual_geometry->clusters.size() *
                    (sizeof(astra::rhi::VirtualGeometryCluster) +
                     5U * sizeof(std::uint32_t) + 64U); // cull args + conservative ICB command allowance
            gpu_chunk.packed_virtual_geometry = std::move(*packed_virtual_geometry);
            if (auto registered = geometry_pages.register_page(
                    {gpu_chunk.page_id, world_geometry_root_page,
                     std::max<std::size_t>(gpu_chunk.page_bytes, 1)});
                !registered) {
                logger.log(astra::core::LogLevel::error, "GeometryStreaming",
                           "Could not register terrain geometry page: " + registered.error());
#if defined(ASTRA_HAS_PHYSICS)
                if (gpu_chunk.terrain_collider)
                    (void)physics_world.destroy_body(*gpu_chunk.terrain_collider);
#endif
                continue;
            }
#if defined(ASTRA_HAS_PHYSICS)
            // Create collision only after the renderable page is valid and registered,
            // so failed geometry construction cannot leak a physics body.
            const std::uint32_t terrain_side = chunk.resolution + 1U;
            const std::size_t collision_vertex_count =
                static_cast<std::size_t>(terrain_side) * terrain_side;
            const std::size_t collision_index_count =
                static_cast<std::size_t>(chunk.resolution) * chunk.resolution * 6U;
            std::vector<astra::rhi::Vec3> collision_vertices;
            collision_vertices.reserve(collision_vertex_count);
            for (std::size_t index = 0; index < collision_vertex_count; ++index) {
                const auto& vertex = terrain_data.vertices[index];
                collision_vertices.push_back(vertex.position);
            }
            const std::span<const std::uint32_t> collision_indices{
                terrain_data.indices.data(), collision_index_count};
            auto terrain_collider =
                physics_world.create_static_mesh(collision_vertices, collision_indices);
            if (terrain_collider) {
                gpu_chunk.terrain_collider = *terrain_collider;
            } else {
                logger.log(astra::core::LogLevel::warning, "Physics",
                           "Could not create terrain collider: " + terrain_collider.error().message);
            }
#endif
            gpu_chunks.emplace(coord, std::move(gpu_chunk));
        }
        for (auto it = gpu_chunks.begin(); it != gpu_chunks.end();) {
            if (!streamer.chunks().contains(it->first)) {
#if defined(ASTRA_HAS_PHYSICS)
                if (it->second.terrain_collider)
                    (void)physics_world.destroy_body(*it->second.terrain_collider);
#endif
                if (geometry_pages.is_resident(it->second.page_id))
                    (void)geometry_pages.mark_evicted(it->second.page_id);
                if (it->second.page_id != astra::geometry::invalid_virtual_geometry_page)
                    (void)geometry_pages.unregister_page(it->second.page_id);
                it = gpu_chunks.erase(it);
            } else {
                ++it;
            }
        }

        // Keep GPU page uploads bounded and prioritize chunks nearest the player.
        for (auto& [coord, chunk] : gpu_chunks) {
            const float center_x = (static_cast<float>(coord.x) + 0.5F) *
                                   streaming_config.chunk_size;
            const float center_z = (static_cast<float>(coord.z) + 0.5F) *
                                   streaming_config.chunk_size;
            const float dx = center_x - position.x;
            const float dz = center_z - position.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (chunk.gpu_virtual_geometry) {
                if (distance <= streaming_config.chunk_size * 1.25F)
                    (void)geometry_pages.touch_page(chunk.page_id, frame_number);
                continue;
            }
            if (chunk.page_upload_ready || !chunk.packed_virtual_geometry)
                continue;
            if (!geometry_pages.request_page(chunk.page_id, frame_number,
                                             1000.0F / (1.0F + distance))) {
                logger.log(astra::core::LogLevel::warning, "GeometryStreaming",
                           "Could not queue a terrain geometry page request.");
            }
        }
        for (const auto& request : geometry_pages.take_requests(2)) {
            for (auto& [coord, chunk] : gpu_chunks) {
                (void)coord;
                if (chunk.page_id == request.id) {
                    chunk.page_upload_ready = true;
                    break;
                }
            }
        }

        // The cache's byte budget covers live GPU pages. Destroy leaf pages
        // before their parents (the cache returns a dependency-safe order),
        // while retaining packed CPU staging so a visible page can be uploaded
        // again if it is requested after eviction.
        for (const auto page_id : geometry_pages.collect_evictions()) {
            const auto chunk = std::ranges::find_if(gpu_chunks, [page_id](const auto& entry) {
                return entry.second.page_id == page_id;
            });
            if (chunk == gpu_chunks.end())
                continue;
            if (auto evicted = geometry_pages.mark_evicted(page_id); evicted) {
                chunk->second.gpu_virtual_geometry.reset();
                chunk->second.page_upload_ready = false;
            } else {
                logger.log(astra::core::LogLevel::warning, "GeometryStreaming", evicted.error());
            }
        }

        const Vec3 camera_forward{std::cos(yaw), 0.0F, std::sin(yaw)};
        const Vec3 camera_position =
            add(add(position, scale(camera_forward, -camera_distance * std::cos(pitch))),
                {0.0F, camera_distance * 0.47F + camera_distance * std::sin(pitch), 0.0F});
        const Vec3 camera_target = add(position, {0.0F, 1.05F, 0.0F});
        const Matrix4 view_projection =
            multiply(perspective(70.0F * kPi / 180.0F,
                                 static_cast<float>(window.width()) /
                                     static_cast<float>(std::max(window.height(), 1U)),
                                 0.08F, 420.0F),
                     look_at(camera_position, subtract(camera_target, camera_position)));
        const auto inverse_view_projection = inverse_matrix(view_projection);
        if (!inverse_view_projection) {
            logger.log(astra::core::LogLevel::error, "Camera", inverse_view_projection.error());
            window.request_close();
            return;
        }
        const Vec3 light_position = subtract(position, scale(light_direction, 220.0F));
        const Matrix4 light_view_projection = multiply(orthographic(160.0F, 160.0F, 1.0F, 450.0F),
                                                       look_at(light_position, light_direction));

        draw_items.clear();
        for (auto& instances : vegetation_instance_transforms)
            instances.clear();
        draw_items.reserve(
            std::max<std::size_t>(128, gpu_chunks.size() * 2U + character.primitives.size() + 32U));
        for (auto& [coord, chunk] : gpu_chunks) {
            (void)coord;
            if (!chunk.gpu_virtual_geometry && chunk.page_upload_ready &&
                chunk.packed_virtual_geometry) {
                const auto& packed = *chunk.packed_virtual_geometry;
                    std::vector<MeshVertex> vertices;
                    vertices.reserve(packed.vertices.size());
                    for (const auto& source : packed.vertices) {
                        const float dx = (terrain_height(source.x + 0.25F, source.z) -
                                          terrain_height(source.x - 0.25F, source.z)) /
                                         0.5F;
                        const float dz = (terrain_height(source.x, source.z + 0.25F) -
                                          terrain_height(source.x, source.z - 0.25F)) /
                                         0.5F;
                        MeshVertex vertex{{source.x, source.y, source.z},
                                          normalize({-dx, 1.0F, -dz}),
                                          {1.0F, 1.0F, 1.0F, 1.0F}};
                        vertex.texcoord_u = source.x * 0.085F;
                        vertex.texcoord_v = source.z * 0.085F;
                        vertex.tangent_x = 1.0F;
                        vertex.tangent_w = -1.0F;
                        vertices.push_back(vertex);
                    }
                    std::vector<astra::rhi::VirtualGeometryCluster> clusters;
                    clusters.reserve(packed.clusters.size());
                    for (const auto& source : packed.clusters) {
                        clusters.push_back({
                            .bounds_sphere = {source.bounds_sphere[0], source.bounds_sphere[1],
                                              source.bounds_sphere[2], source.bounds_sphere[3]},
                            .geometric_error = source.geometric_error,
                            .parent_index = source.parent_index,
                            .first_index = source.first_index,
                            .index_count = source.index_count,
                            .base_vertex = source.base_vertex,
                            .flags = source.flags,
                        });
                    }
                    const astra::rhi::VirtualGeometryData upload{
                        .vertices = vertices,
                        .indices = packed.indices,
                        .clusters = clusters,
                    };
                    if (auto valid = astra::rhi::validate_virtual_geometry(upload); !valid) {
                        logger.log(astra::core::LogLevel::warning, "Geometry", valid.error());
                        chunk.page_upload_ready = false;
                    } else if (auto uploaded = renderer.create_virtual_geometry(upload); uploaded) {
                        chunk.gpu_virtual_geometry = std::move(*uploaded);
                        if (auto resident = geometry_pages.mark_resident(
                                chunk.page_id, frame_number, chunk.page_bytes); !resident) {
                            logger.log(astra::core::LogLevel::warning, "GeometryStreaming",
                                       resident.error());
                            chunk.gpu_virtual_geometry.reset();
                            chunk.page_upload_ready = false;
                            continue;
                        }
                        chunk.page_upload_ready = false;
                        // The Metal buffers now own the packed meshlet data. Keeping both
                        // the full hierarchy and source positions wastes CPU RAM per chunk.
                        chunk.virtual_geometry = {};
                        chunk.source_positions.clear();
                        chunk.source_positions.shrink_to_fit();
                    } else {
                        logger.log(astra::core::LogLevel::warning, "Geometry",
                                   uploaded.error().message);
                        chunk.page_upload_ready = false;
                    }
            }
            if (chunk.gpu_virtual_geometry) {
                const float pixel_error = chunk.lod == astra::world::TerrainLod::near     ? 0.8F
                                          : chunk.lod == astra::world::TerrainLod::medium ? 2.5F
                                                                                          : 16.0F;
                const astra::rhi::VirtualGeometryView virtual_view{
                    .view_projection = view_projection,
                    .camera_position = {camera_position.x, camera_position.y, camera_position.z},
                    .viewport_height_pixels =
                        static_cast<float>(std::max(window.height(), 1U)) * render_scale,
                    .vertical_fov_radians = 70.0F * kPi / 180.0F,
                    .maximum_error_pixels = pixel_error,
                };
                if (auto valid = astra::rhi::validate_virtual_geometry_view(virtual_view); !valid) {
                    logger.log(astra::core::LogLevel::warning, "Geometry", valid.error());
                    continue;
                }
                DrawItem terrain_item{};
                terrain_item.material = terrain_material.material;
                terrain_item.use_material = true;
                terrain_item.virtual_geometry = chunk.gpu_virtual_geometry.get();
                terrain_item.virtual_geometry_view = virtual_view;
                draw_items.push_back(terrain_item);
            }
            if (!chunk.vegetation_mesh_built || chunk.vegetation_mesh_lod != chunk.lod) {
                const auto& source_chunk = streamer.chunks().at(coord);
                auto vegetation_data =
                    build_chunk_vegetation(source_chunk, chunk.lod, chunk.vegetation_instances);
                if (!vegetation_data.indices.empty()) {
                    auto uploaded =
                        renderer.create_mesh({vegetation_data.vertices, vegetation_data.indices});
                    if (uploaded) {
                        chunk.vegetation_mesh = std::move(*uploaded);
                    } else {
                        logger.log(astra::core::LogLevel::warning, "Vegetation",
                                   uploaded.error().message);
                    }
                } else {
                    chunk.vegetation_mesh.reset();
                }
                chunk.vegetation_mesh_lod = chunk.lod;
                chunk.vegetation_mesh_built = true;
            }
            if (chunk.vegetation_mesh)
                draw_items.push_back({chunk.vegetation_mesh.get(), Matrix4{}, 0.0F, 0.84F});

            for (const auto& placement : chunk.vegetation_instances) {
                if (static_cast<unsigned>(chunk.lod) >
                    static_cast<unsigned>(placement.visible_through))
                    continue;
                const auto& model = nature.get(placement.kind);
                vegetation_instance_transforms[static_cast<std::size_t>(placement.kind)].push_back(
                    make_vegetation_instance_transform(model, placement));
            }
        }

        // One indexed draw per asset primitive, regardless of how many copies are in the
        // streamed world. Static transforms are shared by the main and shadow passes.
        for (std::size_t kind = 0; kind < vegetation_instance_transforms.size(); ++kind) {
            const auto& instances = vegetation_instance_transforms[kind];
            if (instances.empty())
                continue;
            const auto& model = nature.get(static_cast<VegetationAssetKind>(kind));
            for (const auto& primitive : model.primitives) {
                if (!primitive.mesh || primitive.node >= model.node_transforms.size())
                    continue;
                DrawItem item{};
                item.mesh = primitive.mesh;
                item.first_index = primitive.first_index;
                item.index_count = primitive.index_count;
                item.model = model.node_transforms[primitive.node];
                item.previous_model = item.model;
                item.has_previous_transform = true;
                item.material = primitive.material;
                item.use_material = true;
                item.instance_transforms = instances;
                item.previous_instance_transforms = instances;
                draw_items.push_back(item);
            }
        }

        std::vector<astra::assets::NodePose> poses(character.source.nodes.size());
        for (std::size_t i = 0; i < character.source.nodes.size(); ++i) {
            const auto& node = character.source.nodes[i];
            poses[i] = {node.translation, node.rotation, node.scale, node.weights};
        }
        if (!character.source.animations.empty()) {
            const auto evaluate_clip = [&](std::size_t index, float time) {
                const auto& clip =
                    character.source
                        .animations[std::min(index, character.source.animations.size() - 1U)];
                return astra::assets::evaluate_animation(
                    clip, character.source.nodes, std::fmod(time, std::max(clip.duration, 0.001F)));
            };
            auto current_pose = evaluate_clip(active_clip_index, active_clip_time);
            if (current_pose) {
                if (animation_blend_elapsed < animation_blend_duration) {
                    auto previous_pose = evaluate_clip(previous_clip_index, previous_clip_time);
                    if (previous_pose) {
                        poses =
                            blend_node_poses(*previous_pose, *current_pose,
                                             animation_blend_elapsed / animation_blend_duration);
                    } else {
                        poses = std::move(*current_pose);
                    }
                } else {
                    poses = std::move(*current_pose);
                }
            } else {
                logger.log(astra::core::LogLevel::warning, "Animation",
                           current_pose.error().message);
            }
        }
        const auto global_nodes = build_global_node_matrices(character.source, poses);
        std::fill(skin_palette_built.begin(), skin_palette_built.end(), false);
        for (const auto& primitive : character.primitives) {
            if (primitive.node >= skin_palettes.size() || skin_palette_built[primitive.node])
                continue;
            build_skin_palette(character.source, primitive.node, global_nodes,
                               skin_palettes[primitive.node]);
            skin_palette_built[primitive.node] = true;
        }
        const Matrix4 character_transform = multiply(
            multiply(translation(position.x, position.y, position.z), rotation_y(character_yaw)),
            scaling(1.0F, 1.0F, 1.0F));
        for (std::size_t i = 0; i < character.primitives.size(); ++i) {
            const auto& primitive = character.primitives[i];
            DrawItem item{};
            item.mesh = primitive.mesh;
            item.first_index = primitive.first_index;
            item.index_count = primitive.index_count;
            item.model = multiply(character_transform, global_nodes[primitive.node]);
            item.material = primitive.material;
            item.use_material = true;
            item.joint_matrices = skin_palettes[primitive.node];
            item.morph_weights = poses[primitive.node].morph_weights;
            if (has_previous_character_transform &&
                previous_global_nodes.size() == character.source.nodes.size() &&
                previous_skin_palettes.size() == character.source.nodes.size() &&
                previous_skin_palettes[primitive.node].size() ==
                    skin_palettes[primitive.node].size()) {
                item.previous_model =
                    multiply(previous_character_transform, previous_global_nodes[primitive.node]);
                item.has_previous_transform = true;
                item.previous_joint_matrices = previous_skin_palettes[primitive.node];
                if (previous_node_morph_weights[primitive.node].size() ==
                    poses[primitive.node].morph_weights.size())
                    item.previous_morph_weights = previous_node_morph_weights[primitive.node];
            }
            draw_items.push_back(item);
        }
        const astra::rhi::SceneSettings scene_settings{
            .camera_position = camera_position,
            .light_direction = light_direction,
            .light_view_projection = light_view_projection,
            .inverse_view_projection = *inverse_view_projection,
            .time_seconds = world_time_seconds,
            .exposure = 1.0F,
            .previous_view_projection = previous_view_projection,
            .has_previous_view_projection = has_previous_view_projection,
            .previous_camera_position = previous_camera_position,
            .render_scale = render_scale,
            .clouds = {.enabled = true,
                       .base_altitude = 1250.0F,
                       .top_altitude = 2450.0F,
                       .density = 1.0F,
                       .coverage = 0.58F,
                       .wind_velocity = {0.012F, 0.002F, -0.007F},
                       .ray_steps = 6},
            .environment = outdoor_environment.get(),
        };
        if (auto result = renderer.render_scene(draw_items, view_projection,
                                                {0.48F, 0.66F, 0.82F, 1.0F}, scene_settings);
            !result) {
            logger.log(astra::core::LogLevel::error, "Renderer", result.error().message);
            window.request_close();
            return;
        }
        // Advance temporal history only after the backend accepted and presented this frame.
        previous_view_projection = view_projection;
        previous_camera_position = camera_position;
        has_previous_view_projection = true;
        previous_character_transform = character_transform;
        previous_global_nodes = global_nodes;
        previous_skin_palettes = skin_palettes;
        for (std::size_t node = 0; node < poses.size(); ++node)
            previous_node_morph_weights[node] = poses[node].morph_weights;
        has_previous_character_transform = true;
        if (benchmark_mode) {
            ++benchmark_frames;
            benchmark_cpu_milliseconds += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - frame_cpu_start)
                                              .count();
            benchmark_gpu_milliseconds += renderer.last_gpu_frame_time_ms();
        }
        performance_elapsed += delta_time;
        performance_cpu_milliseconds += std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - frame_cpu_start)
                                            .count();
        performance_gpu_milliseconds += renderer.last_gpu_frame_time_ms();
        ++performance_frames;
        telemetry_title_elapsed += delta_time;
        if (telemetry_title_elapsed >= 1.0F) {
            telemetry_title_elapsed = 0.0F;
            if (performance_telemetry) {
                const auto page_stats = geometry_pages.stats();
                const auto title = "Astra Open World | " +
                    (displayed_fps > 0.0 ? std::to_string(static_cast<int>(displayed_fps)) + " FPS"
                                         : std::string{"measuring FPS"}) +
                    " | CPU " + std::to_string(displayed_cpu_ms) + " ms | GPU " +
                    std::to_string(displayed_gpu_ms) + " ms | " +
                    std::to_string(static_cast<int>(render_scale * 100.0F)) + "% | " +
                    std::to_string(draw_items.size()) + " draws | " +
                    std::to_string(gpu_chunks.size()) + " chunks | " +
                    std::to_string(page_stats.resident_bytes / (1024U * 1024U)) + " MiB geometry";
                window.set_title(title);
            } else {
                window.set_title("Astra - Open World Walkthrough");
            }
        }
        if (performance_elapsed >= 3.0F && performance_frames != 0U) {
            const double average_cpu_ms =
                performance_cpu_milliseconds / static_cast<double>(performance_frames);
            const double average_fps =
                static_cast<double>(performance_frames) / static_cast<double>(performance_elapsed);
            const double average_gpu_ms =
                performance_gpu_milliseconds / static_cast<double>(performance_frames);
            const auto page_stats = geometry_pages.stats();
            displayed_fps = average_fps;
            displayed_cpu_ms = average_cpu_ms;
            displayed_gpu_ms = average_gpu_ms;
            const float previous_render_scale = render_scale;
            // Target a little below the 16.67 ms v-sync budget so the renderer
            // retains headroom for streaming and OS compositing at larger sizes.
            // The dead band prevents oscillation around the target.
            if (adaptive_resolution && average_gpu_ms > 12.0 && render_scale > 0.50F)
                render_scale = std::max(0.50F, render_scale - 0.05F);
            else if (adaptive_resolution && average_gpu_ms > 0.0 && average_gpu_ms < 9.5 &&
                     render_scale < 0.85F)
                render_scale = std::min(0.85F, render_scale + 0.05F);
            if (performance_telemetry) logger.log(astra::core::LogLevel::info, "Performance",
                       "Frame cadence " + std::to_string(average_fps) + " FPS; frame callback " +
                           std::to_string(average_cpu_ms) + " ms; GPU " +
                           std::to_string(average_gpu_ms) + " ms; " +
                           "internal scale " + std::to_string(render_scale) +
                           (render_scale != previous_render_scale ? " (adjusted); " : "; ") +
                           std::to_string(draw_items.size()) + " draw items across " +
                           std::to_string(gpu_chunks.size()) + " resident chunks; " +
                           std::to_string(page_stats.resident_bytes / (1024U * 1024U)) +
                           " MiB virtual geometry across " +
                           std::to_string(page_stats.resident_pages) + " pages (" +
                           std::to_string(page_stats.pending_requests) + " pending).");
            performance_elapsed = 0.0F;
            performance_cpu_milliseconds = 0.0;
            performance_gpu_milliseconds = 0.0;
            performance_frames = 0;
        }
        if (maximum_fps > 0) {
            const auto frame_budget = std::chrono::duration<double>{1.0 / maximum_fps};
            std::this_thread::sleep_until(frame_cpu_start +
                                          std::chrono::duration_cast<
                                              std::chrono::steady_clock::duration>(frame_budget));
        }
    });
    if (benchmark_mode) {
        const double elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - benchmark_started)
                .count();
        if (benchmark_frames != 0U && elapsed_seconds > 0.0) {
            benchmark_csv.enqueue({
                .elapsed_seconds = elapsed_seconds,
                .frames = benchmark_frames,
                .average_fps = static_cast<double>(benchmark_frames) / elapsed_seconds,
                .average_cpu_ms =
                    benchmark_cpu_milliseconds / static_cast<double>(benchmark_frames),
                .average_gpu_ms =
                    benchmark_gpu_milliseconds / static_cast<double>(benchmark_frames),
                .render_scale = render_scale,
                .draw_count = draw_items.size(),
                .resident_chunks = gpu_chunks.size(),
            });
        }
    }
    logger.log(astra::core::LogLevel::info, "World", "Leaving the open world.");
    return 0;
}
