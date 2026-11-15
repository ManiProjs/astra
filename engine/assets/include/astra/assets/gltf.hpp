#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace astra::assets {

struct Error final {
    std::filesystem::path file;
    std::string message;
};

struct Vec2 final {
    float x{}, y{};
};
struct Vec3 final {
    float x{}, y{}, z{};
};
struct Vec4 final {
    float x{}, y{}, z{}, w{};
};
using Mat4 = std::array<float, 16>; // glTF column-major

struct Vertex final {
    Vec3 position{};
    Vec3 normal{};
    Vec4 tangent{};
    Vec2 texcoord0{};
    Vec2 texcoord1{};
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<std::uint16_t, 4> joints{};
    Vec4 weights{};
};

struct Primitive final {
    struct MorphTarget final {
        // glTF morph attributes are vertex deltas relative to the base primitive.
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec3> tangents;
    };
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MorphTarget> morph_targets;
    std::uint32_t material{~std::uint32_t{0}};
    bool has_joint_attributes{};
    bool has_weight_attributes{};
    // glTF primitive modes: points=0, lines=1, line_loop=2, line_strip=3,
    // triangles=4, triangle_strip=5, triangle_fan=6.
    std::uint32_t mode{4};
};

struct Mesh final {
    std::string name;
    std::vector<Primitive> primitives;
    std::vector<float> weights;
};

struct TextureImage final {
    std::string name;
    std::string mime_type;
    std::filesystem::path source_path;
    std::vector<std::byte> bytes;
};

enum class TextureFilter : std::uint8_t { nearest, linear };
enum class TextureMipFilter : std::uint8_t { none, nearest, linear };
enum class TextureWrap : std::uint8_t { repeat, mirrored_repeat, clamp_to_edge };

struct Sampler final {
    TextureFilter mag_filter{TextureFilter::linear};
    TextureFilter min_filter{TextureFilter::linear};
    TextureMipFilter mip_filter{TextureMipFilter::linear};
    TextureWrap wrap_s{TextureWrap::repeat};
    TextureWrap wrap_t{TextureWrap::repeat};
};

struct Texture final {
    std::string name;
    std::uint32_t image{~std::uint32_t{0}};
    std::uint32_t sampler{~std::uint32_t{0}};
    Sampler sampling{};
};

struct TextureInfo final {
    std::uint32_t texture{~std::uint32_t{0}};
    std::uint32_t texcoord{};
    float scale{1.0F}; // normal scale / occlusion strength
    // KHR_texture_transform: offset + rotation * (scale * UV).
    Vec2 uv_offset{};
    Vec2 uv_scale{1.0F, 1.0F};
    float uv_rotation{};
};

struct Material final {
    std::string name;
    Vec4 base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    float metallic_factor{1.0F};
    float roughness_factor{1.0F};
    Vec3 emissive_factor{};
    TextureInfo base_color_texture{};
    TextureInfo metallic_roughness_texture{};
    TextureInfo normal_texture{};
    TextureInfo occlusion_texture{};
    TextureInfo emissive_texture{};
    std::uint32_t alpha_mode{}; // opaque=0, mask=1, blend=2
    float alpha_cutoff{0.5F};
    bool double_sided{};
};

struct Node final {
    std::string name;
    std::int32_t parent{-1};
    std::vector<std::uint32_t> children;
    std::uint32_t mesh{~std::uint32_t{0}};
    std::uint32_t skin{~std::uint32_t{0}};
    // Optional per-node override of the mesh's default morph weights.
    std::vector<float> weights;
    Vec3 translation{};
    Vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3 scale{1.0F, 1.0F, 1.0F};
    Mat4 matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool has_matrix{};
};

struct Skin final {
    std::string name;
    std::int32_t skeleton{-1};
    std::vector<std::uint32_t> joints;
    std::vector<Mat4> inverse_bind_matrices;
};

enum class AnimationPath : std::uint8_t { translation, rotation, scale, weights };
enum class Interpolation : std::uint8_t { linear, step, cubic_spline };

struct AnimationSampler final {
    Interpolation interpolation{Interpolation::linear};
    std::vector<float> times;
    // Values are tightly packed vectors of 1, 3, or 4 components. For CUBICSPLINE,
    // each key contributes in-tangent, value, out-tangent in that order.
    std::uint32_t components{};
    std::vector<float> values;
};

struct AnimationChannel final {
    std::uint32_t sampler{};
    std::uint32_t node{};
    AnimationPath path{AnimationPath::translation};
    // Number of scalar morph weights for a weights channel; zero for TRS channels.
    std::uint32_t weight_count{};
};

struct Animation final {
    std::string name;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
    float duration{};
};

struct Scene final {
    std::string name;
    std::vector<std::uint32_t> roots;
};

struct Asset final {
    std::filesystem::path source;
    std::vector<TextureImage> images;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<Skin> skins;
    std::vector<Animation> animations;
    std::vector<Scene> scenes;
    std::uint32_t default_scene{~std::uint32_t{0}};
};

struct LoadOptions final {
    std::size_t maximum_file_bytes{512U * 1024U * 1024U};
    std::size_t maximum_json_depth{128};
};

// Loads .gltf JSON with external/data-URI resources, or binary .glb (2.0).
// Accessor data is expanded into CPU vertex/index arrays; image payloads remain encoded.
[[nodiscard]] std::expected<Asset, Error> load_gltf(const std::filesystem::path& path,
                                                    LoadOptions options = {});

} // namespace astra::assets
