#include <astra/assets/gltf.hpp>
#include <astra/assets/image.hpp>
#include <astra/core/log.hpp>
#include <astra/game/game.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef ASTRA_BOAR_ASSET_PATH
#define ASTRA_BOAR_ASSET_PATH "assets/characters/Boarbound.glb"
#endif
#ifndef ASTRA_BOAR_HUMAN_ASSET_PATH
#define ASTRA_BOAR_HUMAN_ASSET_PATH "assets/characters/Boarbound-Human.glb"
#endif

namespace {

using astra::assets::Asset;
using astra::assets::Node;
using astra::rhi::MaterialData;
using astra::rhi::Matrix4;
using astra::rhi::MeshVertex;
using astra::rhi::Vec3;
using astra::rhi::Vec4;

struct Part final {
    std::unique_ptr<astra::rhi::GpuMesh> mesh;
    MaterialData material;
};

struct Character final {
    std::vector<std::unique_ptr<astra::rhi::GpuTexture>> textures;
    std::vector<Part> parts;
};

struct Transform final {
    Vec3 translation{};
    Vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

Vec4 multiply(Vec4 a, Vec4 b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Vec3 rotate(Vec3 value, Vec4 q) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 cross1{u.y * value.z - u.z * value.y,
                      u.z * value.x - u.x * value.z,
                      u.x * value.y - u.y * value.x};
    const Vec3 cross2{u.y * cross1.z - u.z * cross1.y,
                      u.z * cross1.x - u.x * cross1.z,
                      u.x * cross1.y - u.y * cross1.x};
    return {value.x + 2.0F * (q.w * cross1.x + cross2.x),
            value.y + 2.0F * (q.w * cross1.y + cross2.y),
            value.z + 2.0F * (q.w * cross1.z + cross2.z)};
}

Transform world_transform(const Asset& asset, std::size_t index, std::vector<Transform>& cache,
                          std::vector<bool>& ready) {
    if (ready[index])
        return cache[index];
    const auto& node = asset.nodes[index];
    Transform local{{node.translation.x, node.translation.y, node.translation.z},
                    {node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w},
                    {node.scale.x, node.scale.y, node.scale.z}};
    if (node.parent >= 0) {
        const auto parent = world_transform(asset, static_cast<std::size_t>(node.parent), cache, ready);
        const auto translated = rotate({parent.scale.x * local.translation.x,
                                        parent.scale.y * local.translation.y,
                                        parent.scale.z * local.translation.z}, parent.rotation);
        local.translation = {parent.translation.x + translated.x,
                             parent.translation.y + translated.y,
                             parent.translation.z + translated.z};
        local.rotation = multiply(parent.rotation, local.rotation);
        local.scale = {parent.scale.x * local.scale.x, parent.scale.y * local.scale.y,
                       parent.scale.z * local.scale.z};
    }
    cache[index] = local;
    ready[index] = true;
    return local;
}

Vec3 transform_position(Vec3 position, const Transform& transform) {
    const auto rotated = rotate({position.x * transform.scale.x,
                                 position.y * transform.scale.y,
                                 position.z * transform.scale.z}, transform.rotation);
    return {transform.translation.x + rotated.x, transform.translation.y + rotated.y,
            transform.translation.z + rotated.z};
}

Vec3 transform_normal(Vec3 normal, const Transform& transform) {
    Vec3 result = rotate({normal.x / transform.scale.x, normal.y / transform.scale.y,
                          normal.z / transform.scale.z}, transform.rotation);
    const float length = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z);
    if (length > 1.0e-6F) {
        result.x /= length;
        result.y /= length;
        result.z /= length;
    }
    return result;
}

astra::rhi::MaterialSamplerDesc sampler(const astra::assets::TextureInfo& info,
                                        const Asset& asset) {
    astra::rhi::MaterialSamplerDesc result;
    if (info.texture >= asset.textures.size())
        return result;
    const auto& source = asset.textures[info.texture].sampling;
    result.min_filter = static_cast<astra::rhi::MaterialFilter>(source.min_filter);
    result.mag_filter = static_cast<astra::rhi::MaterialFilter>(source.mag_filter);
    result.mip_filter = static_cast<astra::rhi::MaterialMipFilter>(source.mip_filter);
    result.address_u = static_cast<astra::rhi::MaterialAddressMode>(source.wrap_s);
    result.address_v = static_cast<astra::rhi::MaterialAddressMode>(source.wrap_t);
    return result;
}

astra::rhi::MaterialUvTransform uv_transform(const astra::assets::TextureInfo& info) {
    return {.offset = {info.uv_offset.x, info.uv_offset.y},
            .scale = {info.uv_scale.x, info.uv_scale.y},
            .rotation = info.uv_rotation};
}

std::expected<Character, std::string> load_character(astra::renderer::Renderer& renderer,
                                                      const std::filesystem::path& path,
                                                      astra::core::Logger& logger) {
    auto loaded = astra::assets::load_gltf(path);
    if (!loaded)
        return std::unexpected("Could not load " + path.string() + ": " + loaded.error().message);
    Asset& asset = *loaded;
    Character character;
    std::map<std::pair<std::uint32_t, bool>, const astra::rhi::GpuTexture*> uploaded;
    const auto upload_texture = [&](std::uint32_t texture_index, bool srgb)
        -> const astra::rhi::GpuTexture* {
        if (texture_index >= asset.textures.size())
            return nullptr;
        const auto image_index = asset.textures[texture_index].image;
        if (image_index >= asset.images.size())
            return nullptr;
        const auto key = std::pair{image_index, srgb};
        if (const auto found = uploaded.find(key); found != uploaded.end())
            return found->second;
        const auto& image = asset.images[image_index];
        auto decoded = astra::assets::decode_image(image.bytes, image.mime_type);
        if (!decoded) {
            logger.log(astra::core::LogLevel::warning, "Assets",
                       "Texture decode failed for " + image.name + ": " + decoded.error().message);
            return nullptr;
        }
        auto texture = renderer.create_texture({decoded->width, decoded->height, decoded->rgba8, srgb});
        if (!texture) {
            logger.log(astra::core::LogLevel::warning, "Assets",
                       "Texture upload failed for " + image.name + ": " + texture.error().message);
            return nullptr;
        }
        const auto* reference = texture->get();
        character.textures.push_back(std::move(*texture));
        uploaded.emplace(key, reference);
        return reference;
    };

    std::vector<MaterialData> materials;
    materials.reserve(asset.materials.size());
    for (const auto& source : asset.materials) {
        MaterialData material;
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
        material.metallic_roughness_texture = upload_texture(source.metallic_roughness_texture.texture, false);
        material.normal_texture = upload_texture(source.normal_texture.texture, false);
        material.occlusion_texture = upload_texture(source.occlusion_texture.texture, false);
        material.emissive_texture = upload_texture(source.emissive_texture.texture, true);
        material.base_color_texcoord = source.base_color_texture.texcoord;
        material.metallic_roughness_texcoord = source.metallic_roughness_texture.texcoord;
        material.normal_texcoord = source.normal_texture.texcoord;
        material.occlusion_texcoord = source.occlusion_texture.texcoord;
        material.emissive_texcoord = source.emissive_texture.texcoord;
        material.base_color_uv = uv_transform(source.base_color_texture);
        material.metallic_roughness_uv = uv_transform(source.metallic_roughness_texture);
        material.normal_uv = uv_transform(source.normal_texture);
        material.occlusion_uv = uv_transform(source.occlusion_texture);
        material.emissive_uv = uv_transform(source.emissive_texture);
        material.base_color_sampler = sampler(source.base_color_texture, asset);
        material.metallic_roughness_sampler = sampler(source.metallic_roughness_texture, asset);
        material.normal_sampler = sampler(source.normal_texture, asset);
        material.occlusion_sampler = sampler(source.occlusion_texture, asset);
        material.emissive_sampler = sampler(source.emissive_texture, asset);
        materials.push_back(material);
    }

    std::vector<Transform> transforms(asset.nodes.size());
    std::vector<bool> transform_ready(asset.nodes.size(), false);
    for (std::size_t node_index = 0; node_index < asset.nodes.size(); ++node_index) {
        const auto& node = asset.nodes[node_index];
        if (node.mesh >= asset.meshes.size())
            continue;
        const auto node_transform = world_transform(asset, node_index, transforms, transform_ready);
        const auto& mesh = asset.meshes[node.mesh];
        for (const auto& primitive : mesh.primitives) {
            if (primitive.mode != 4U || primitive.vertices.empty() || primitive.indices.empty())
                continue;
            std::vector<MeshVertex> vertices;
            vertices.reserve(primitive.vertices.size());
            for (const auto& source : primitive.vertices) {
                MeshVertex vertex{};
                vertex.position = transform_position(
                    {source.position.x, source.position.y, source.position.z}, node_transform);
                vertex.normal = transform_normal(
                    {source.normal.x, source.normal.y, source.normal.z}, node_transform);
                vertex.color = {source.color.x, source.color.y, source.color.z, source.color.w};
                vertex.texcoord_u = source.texcoord0.x;
                vertex.texcoord_v = source.texcoord0.y;
                vertex.texcoord1_u = source.texcoord1.x;
                vertex.texcoord1_v = source.texcoord1.y;
                vertex.tangent_x = source.tangent.x;
                vertex.tangent_y = source.tangent.y;
                vertex.tangent_z = source.tangent.z;
                vertex.tangent_w = source.tangent.w;
                for (std::size_t i = 0; i < 4; ++i) {
                    vertex.joints[i] = source.joints[i];
                    const float weights[4]{source.weights.x, source.weights.y,
                                           source.weights.z, source.weights.w};
                    vertex.weights[i] = weights[i];
                }
                vertices.push_back(vertex);
            }
            auto gpu_mesh = renderer.create_mesh({vertices, primitive.indices});
            if (!gpu_mesh)
                return std::unexpected("Could not upload " + mesh.name + ": " + gpu_mesh.error().message);
            MaterialData material;
            if (primitive.material < materials.size())
                material = materials[primitive.material];
            character.parts.push_back({std::move(*gpu_mesh), material});
        }
    }
    if (character.parts.empty())
        return std::unexpected("The character asset contains no renderable triangle meshes: " + path.string());
    return character;
}

Matrix4 model_transform(float scale, float yaw, float bob) {
    Matrix4 result{};
    const float c = std::cos(yaw), s = std::sin(yaw);
    result.values[0] = c * scale;
    result.values[2] = -s * scale;
    result.values[5] = scale;
    result.values[8] = s * scale;
    result.values[10] = c * scale;
    result.values[12] = 0.0F;
    result.values[13] = bob;
    result.values[14] = 0.0F;
    result.values[15] = 1.0F;
    return result;
}

void draw_character(astra::game::Frame& frame, const Character& character, float opacity,
                    Matrix4 transform) {
    if (opacity <= 0.005F)
        return;
    for (const auto& part : character.parts) {
        auto material = part.material;
        material.base_color.alpha *= opacity;
        if (opacity < 0.999F)
            material.alpha_mode = 2U;
        frame.draw(*part.mesh, transform, material);
    }
}

} // namespace

int main() {
    astra::core::Logger logger{astra::core::LogLevel::info};
    auto created = astra::game::Game::create({
        .window = {.title = "Astra - The Boarbound", .width = 1280, .height = 800,
                   .resizable = true, .high_dpi = true},
    });
    if (!created) {
        logger.log(astra::core::LogLevel::critical, "Game", created.error().message);
        return 1;
    }
    auto game = std::move(*created);

    auto human = load_character(game.renderer(), ASTRA_BOAR_HUMAN_ASSET_PATH, logger);
    auto boar = load_character(game.renderer(), ASTRA_BOAR_ASSET_PATH, logger);
    if (!human || !boar) {
        logger.log(astra::core::LogLevel::critical, "Character",
                   !human ? human.error() : boar.error());
        return 1;
    }
    logger.log(astra::core::LogLevel::info, "Character",
               "Loaded an authored 262K-triangle human and a separate three-part boar model. Both use their imported glTF materials.");
    logger.log(astra::core::LogLevel::info, "Scene",
               "The scene begins as a human, transitions into the boar, holds the creature form, then reverses. Space pauses, R restarts, Escape exits.");

    bool paused = false;
    float elapsed = 0.0F;
    std::uint64_t frames = 0;
    game.run([&](astra::game::Frame& frame) {
        if (frame.input.pressed(astra::game::Key::escape)) {
            frame.window.request_close();
            return;
        }
        if (frame.input.pressed(astra::game::Key::space))
            paused = !paused;
        if (frame.input.pressed(astra::game::Key::r))
            elapsed = 0.0F;
        if (!paused)
            elapsed += frame.delta_time;

        frame.camera.position = {3.1F, 2.25F, 5.4F};
        frame.camera.target = {0.0F, 0.88F, 0.0F};
        constexpr float hold_seconds = 2.0F;
        constexpr float transition_seconds = 4.0F;
        constexpr float cycle_seconds = hold_seconds * 2.0F + transition_seconds * 2.0F;
        const float phase = std::fmod(elapsed, cycle_seconds);
        float boar_amount = 0.0F;
        if (phase >= hold_seconds && phase < hold_seconds + transition_seconds) {
            const float t = (phase - hold_seconds) / transition_seconds;
            boar_amount = t * t * (3.0F - 2.0F * t);
        } else if (phase >= hold_seconds + transition_seconds &&
                   phase < hold_seconds * 2.0F + transition_seconds) {
            boar_amount = 1.0F;
        } else if (phase >= hold_seconds * 2.0F + transition_seconds) {
            const float t = (phase - hold_seconds * 2.0F - transition_seconds) / transition_seconds;
            const float eased = t * t * (3.0F - 2.0F * t);
            boar_amount = 1.0F - eased;
        }

        const float breath = std::sin(elapsed * 2.1F) * 0.012F;
        const float human_opacity = 1.0F - boar_amount;
        const float boar_opacity = boar_amount;
        draw_character(frame, *human, human_opacity,
                       model_transform(1.0F - boar_amount * 0.12F,
                                       boar_amount * 0.12F, breath * (1.0F - boar_amount)));
        draw_character(frame, *boar, boar_opacity,
                       model_transform(0.82F + boar_amount * 0.18F,
                                       -0.12F * (1.0F - boar_amount), breath * boar_amount));

        if (auto result = frame.render({0.025F, 0.031F, 0.043F, 1.0F}); !result) {
            logger.log(astra::core::LogLevel::error, "Renderer", result.error().message);
            game.close();
            return;
        }
        ++frames;
    });
    logger.log(astra::core::LogLevel::info, "Renderer",
               "Presented " + std::to_string(frames) + " frames.");
    return 0;
}
