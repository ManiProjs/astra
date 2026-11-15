#include <astra/assets/animation.hpp>
#include <astra/assets/gltf.hpp>
#include <astra/assets/image.hpp>
#include <astra/core/log.hpp>
#include <astra/game/game.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef ASTRA_BOAR_ASSET_PATH
#define ASTRA_BOAR_ASSET_PATH "assets/characters/AstraBoar.glb"
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
    std::vector<float> morph_weights;
    std::uint32_t node{};
    std::uint32_t skin{~std::uint32_t{0}};
};

struct Character final {
    std::vector<std::unique_ptr<astra::rhi::GpuTexture>> textures;
    std::vector<Part> parts;
    std::unique_ptr<Asset> source;
};

struct Transform final {
    Vec3 translation{};
    Vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

Matrix4 transform_matrix(const Transform& transform) {
    const auto q = transform.rotation;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Matrix4 result{};
    result.values[0] = (1.0F - 2.0F * (yy + zz)) * transform.scale.x;
    result.values[1] = (2.0F * (xy + wz)) * transform.scale.x;
    result.values[2] = (2.0F * (xz - wy)) * transform.scale.x;
    result.values[4] = (2.0F * (xy - wz)) * transform.scale.y;
    result.values[5] = (1.0F - 2.0F * (xx + zz)) * transform.scale.y;
    result.values[6] = (2.0F * (yz + wx)) * transform.scale.y;
    result.values[8] = (2.0F * (xz + wy)) * transform.scale.z;
    result.values[9] = (2.0F * (yz - wx)) * transform.scale.z;
    result.values[10] = (1.0F - 2.0F * (xx + yy)) * transform.scale.z;
    result.values[12] = transform.translation.x;
    result.values[13] = transform.translation.y;
    result.values[14] = transform.translation.z;
    result.values[15] = 1.0F;
    return result;
}

Matrix4 multiply_matrix(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                result.values[column * 4 + row] +=
                    a.values[k * 4 + row] * b.values[column * 4 + k];
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

    for (std::size_t node_index = 0; node_index < asset.nodes.size(); ++node_index) {
        const auto& node = asset.nodes[node_index];
        if (node.mesh >= asset.meshes.size())
            continue;
        const auto& mesh = asset.meshes[node.mesh];
        for (const auto& primitive : mesh.primitives) {
            if (primitive.mode != 4U || primitive.vertices.empty() || primitive.indices.empty())
                continue;
            std::vector<MeshVertex> vertices;
            vertices.reserve(primitive.vertices.size());
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
                for (std::size_t i = 0; i < 4; ++i) {
                    vertex.joints[i] = source.joints[i];
                    const float weights[4]{source.weights.x, source.weights.y,
                                           source.weights.z, source.weights.w};
                    vertex.weights[i] = weights[i];
                }
                vertices.push_back(vertex);
            }
            std::vector<std::vector<Vec3>> morph_positions;
            std::vector<std::vector<Vec3>> morph_normals;
            std::vector<std::vector<Vec3>> morph_tangents;
            std::vector<astra::rhi::MeshData::MorphTarget> morph_targets;
            morph_positions.reserve(primitive.morph_targets.size());
            morph_normals.reserve(primitive.morph_targets.size());
            morph_tangents.reserve(primitive.morph_targets.size());
            morph_targets.reserve(primitive.morph_targets.size());
            const auto convert_deltas = [](const std::vector<astra::assets::Vec3>& source,
                                           std::vector<Vec3>& destination) {
                destination.reserve(source.size());
                for (const auto& value : source)
                    destination.push_back({value.x, value.y, value.z});
            };
            for (const auto& source_target : primitive.morph_targets) {
                morph_positions.emplace_back();
                morph_normals.emplace_back();
                morph_tangents.emplace_back();
                convert_deltas(source_target.positions, morph_positions.back());
                convert_deltas(source_target.normals, morph_normals.back());
                convert_deltas(source_target.tangents, morph_tangents.back());
                morph_targets.push_back({morph_positions.back(), morph_normals.back(),
                                         morph_tangents.back()});
            }
            auto gpu_mesh = renderer.create_mesh({vertices, primitive.indices, morph_targets});
            if (!gpu_mesh)
                return std::unexpected("Could not upload " + mesh.name + ": " + gpu_mesh.error().message);
            MaterialData material;
            if (primitive.material < materials.size())
                material = materials[primitive.material];
            character.parts.push_back({std::move(*gpu_mesh), material, node.weights,
                                       static_cast<std::uint32_t>(node_index), node.skin});
        }
    }
    if (character.parts.empty())
        return std::unexpected("The character asset contains no renderable triangle meshes: " + path.string());
    character.source = std::make_unique<Asset>(std::move(asset));
    return character;
}

Matrix4 model_transform(float scale, float yaw, float offset_x) {
    Matrix4 result{};
    const float c = std::cos(yaw), s = std::sin(yaw);
    result.values[0] = c * scale;
    result.values[2] = -s * scale;
    result.values[5] = scale;
    result.values[8] = s * scale;
    result.values[10] = c * scale;
    result.values[12] = offset_x;
    result.values[13] = 0.0F;
    result.values[14] = 0.0F;
    result.values[15] = 1.0F;
    return result;
}

std::vector<astra::assets::NodePose> default_pose(const Asset& asset) {
    std::vector<astra::assets::NodePose> result;
    result.reserve(asset.nodes.size());
    for (const auto& node : asset.nodes)
        result.push_back({node.translation, node.rotation, node.scale, node.weights});
    return result;
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
            return std::unexpected("Character node transform is singular");
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

std::vector<Matrix4> global_matrices(const Asset& asset,
                                     std::span<const astra::assets::NodePose> poses) {
    if (poses.size() != asset.nodes.size())
        throw std::runtime_error("Node pose count does not match the glTF node count");
    std::vector<Matrix4> result(asset.nodes.size());
    std::vector<unsigned char> state(asset.nodes.size());
    const auto build = [&](const auto& self, std::size_t index) -> void {
        if (state[index] == 2)
            return;
        if (state[index] == 1)
            throw std::runtime_error("glTF node hierarchy contains a cycle");
        state[index] = 1;
        const auto& node = asset.nodes[index];
        Matrix4 local{};
        if (node.has_matrix) {
            std::copy(node.matrix.begin(), node.matrix.end(), local.values.data());
        } else {
            const auto& pose = poses[index];
            local = transform_matrix({{pose.translation.x, pose.translation.y, pose.translation.z},
                                      {pose.rotation.x, pose.rotation.y, pose.rotation.z,
                                       pose.rotation.w},
                                      {pose.scale.x, pose.scale.y, pose.scale.z}});
        }
        if (node.parent >= 0) {
            const auto parent = static_cast<std::size_t>(node.parent);
            if (parent >= asset.nodes.size())
                throw std::runtime_error("glTF node parent index is invalid");
            self(self, parent);
            result[index] = multiply_matrix(result[parent], local);
        } else {
            result[index] = local;
        }
        state[index] = 2;
    };
    for (std::size_t i = 0; i < asset.nodes.size(); ++i)
        build(build, i);
    return result;
}

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(Vec3 value) {
    const float length = std::sqrt(dot(value, value));
    return length > 1.0e-6F ? Vec3{value.x / length, value.y / length, value.z / length}
                            : Vec3{0.0F, 0.0F, -1.0F};
}

Matrix4 camera_view_projection(const astra::game::Camera& camera, float aspect) {
    const auto forward = normalize(subtract(camera.target, camera.position));
    auto right = normalize(cross(forward, camera.up));
    if (dot(right, right) < 0.5F)
        right = normalize(cross(forward, {0.0F, 0.0F, 1.0F}));
    const auto up = cross(right, forward);
    Matrix4 view{};
    view.values[0] = right.x; view.values[4] = right.y; view.values[8] = right.z;
    view.values[1] = up.x; view.values[5] = up.y; view.values[9] = up.z;
    view.values[2] = -forward.x; view.values[6] = -forward.y; view.values[10] = -forward.z;
    view.values[12] = -dot(right, camera.position);
    view.values[13] = -dot(up, camera.position);
    view.values[14] = dot(forward, camera.position);

    Matrix4 projection{};
    projection.values.fill(0.0F);
    const float fov = camera.vertical_fov_degrees * std::numbers::pi_v<float> / 180.0F;
    const float factor = 1.0F / std::tan(fov * 0.5F);
    projection.values[0] = factor / aspect;
    projection.values[5] = factor;
    projection.values[10] = camera.far_clip / (camera.near_clip - camera.far_clip);
    projection.values[11] = -1.0F;
    projection.values[14] = camera.near_clip * camera.far_clip /
                            (camera.near_clip - camera.far_clip);
    Matrix4 result{};
    result.values.fill(0.0F);
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                result.values[column * 4 + row] +=
                    projection.values[k * 4 + row] * view.values[column * 4 + k];
    return result;
}

std::expected<std::vector<Matrix4>, std::string>
skin_palette(const Asset& asset, const Part& part, std::span<const Matrix4> globals) {
    std::vector<Matrix4> palette;
    if (part.skin == ~std::uint32_t{0})
        return palette;
    if (part.skin >= asset.skins.size() || part.node >= globals.size())
        return std::unexpected("Character primitive references an invalid skin or node");
    const auto& skin = asset.skins[part.skin];
    if (skin.joints.size() != skin.inverse_bind_matrices.size())
        return std::unexpected("Character skin joint and inverse-bind counts differ");
    auto inverse_mesh = inverse_matrix(globals[part.node]);
    if (!inverse_mesh)
        return std::unexpected(inverse_mesh.error());
    palette.reserve(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const auto joint = skin.joints[i];
        if (joint >= globals.size())
            return std::unexpected("Character skin joint index is out of range");
        Matrix4 inverse_bind{};
        std::copy(skin.inverse_bind_matrices[i].begin(), skin.inverse_bind_matrices[i].end(),
                  inverse_bind.values.data());
        palette.push_back(multiply_matrix(multiply_matrix(*inverse_mesh, globals[joint]),
                                          inverse_bind));
    }
    return palette;
}

std::expected<void, std::string>
draw_character(astra::game::Frame& frame, const Character& character, Matrix4 transform,
               Matrix4 previous_transform,
               std::span<const astra::assets::NodePose> poses,
               std::span<const astra::assets::NodePose> previous_poses) {
    const auto globals = global_matrices(*character.source, poses);
    const auto previous_globals = global_matrices(*character.source, previous_poses);
    for (const auto& part : character.parts) {
        auto palette = skin_palette(*character.source, part, globals);
        auto previous_palette = skin_palette(*character.source, part, previous_globals);
        if (!palette || !previous_palette)
            return std::unexpected(!palette ? palette.error() : previous_palette.error());
        astra::rhi::DrawItem item{};
        item.mesh = part.mesh.get();
        item.model = multiply_matrix(transform, globals[part.node]);
        item.previous_model = multiply_matrix(previous_transform, previous_globals[part.node]);
        item.has_previous_transform = true;
        item.material = part.material;
        item.use_material = true;
        // Morph weights belong to the evaluated node pose, not the mesh's
        // import-time defaults. Keep the previous pose separate so temporal
        // velocity uses the exact deformation submitted on the prior frame.
        if (part.node < poses.size())
            item.morph_weights = poses[part.node].morph_weights;
        if (part.node < previous_poses.size())
            item.previous_morph_weights = previous_poses[part.node].morph_weights;
        item.joint_matrices = *palette;
        item.previous_joint_matrices = *previous_palette;
        frame.draw(std::move(item));
    }
    return {};
}

} // namespace

int main() {
    astra::core::Logger logger{astra::core::LogLevel::info};
    auto created = astra::game::Game::create({
        .window = {.title = "Astra - Character Asset Inspection", .width = 1280, .height = 800,
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
               "Loaded the human source and a 966K-triangle boar with a 13-joint skin and four animation clips.");
    logger.log(astra::core::LogLevel::warning, "Deformation",
               "The endpoints still have no validated shared topology or authored correspondence. This scene previews the assets and plays the boar's skeletal animation; it does not perform the human-to-boar transformation.");

    auto human_pose = default_pose(*human->source);
    auto boar_pose = default_pose(*boar->source);
    auto previous_human_pose = human_pose;
    auto previous_boar_pose = boar_pose;
    const auto idle = std::find_if(boar->source->animations.begin(), boar->source->animations.end(),
                                   [](const auto& animation) { return animation.name == "idle"; });
    const auto human_model = model_transform(0.9F, 0.0F, -1.25F);
    const auto boar_model = model_transform(0.9F, 0.0F, 1.25F);

    std::uint64_t frames = 0;
    Matrix4 previous_view_projection{};
    Vec3 previous_camera_position{};
    bool has_previous_view_projection = false;
    game.run([&](astra::game::Frame& frame) {
        if (frame.input.pressed(astra::game::Key::escape)) {
            frame.window.request_close();
            return;
        }
        frame.camera.position = {4.4F, 2.55F, 7.0F};
        frame.camera.target = {0.0F, 0.88F, 0.0F};
        if (idle != boar->source->animations.end()) {
            const float duration = std::max(idle->duration, 0.001F);
            auto evaluated = astra::assets::evaluate_animation(
                *idle, boar->source->nodes, std::fmod(frame.elapsed_time, duration));
            if (evaluated)
                boar_pose = std::move(*evaluated);
        }
        if (auto result = draw_character(frame, *human, human_model, human_model,
                                         human_pose, previous_human_pose); !result) {
            logger.log(astra::core::LogLevel::error, "Character", result.error());
            game.close();
            return;
        }
        if (auto result = draw_character(frame, *boar, boar_model, boar_model,
                                         boar_pose, previous_boar_pose); !result) {
            logger.log(astra::core::LogLevel::error, "Character", result.error());
            game.close();
            return;
        }

        const float aspect = static_cast<float>(std::max(frame.window.width(), 1U)) /
                             static_cast<float>(std::max(frame.window.height(), 1U));
        const auto current_view_projection = camera_view_projection(frame.camera, aspect);
        astra::rhi::SceneSettings scene_settings{};
        scene_settings.previous_view_projection = previous_view_projection;
        scene_settings.has_previous_view_projection = has_previous_view_projection;
        scene_settings.previous_camera_position = previous_camera_position;
        if (auto result = frame.render({0.025F, 0.031F, 0.043F, 1.0F}, scene_settings); !result) {
            logger.log(astra::core::LogLevel::error, "Renderer", result.error().message);
            game.close();
            return;
        }
        // Advance camera and deformation history only after a successful render.
        previous_view_projection = current_view_projection;
        previous_camera_position = frame.camera.position;
        has_previous_view_projection = true;
        previous_human_pose = human_pose;
        previous_boar_pose = boar_pose;
        ++frames;
    });
    logger.log(astra::core::LogLevel::info, "Renderer",
               "Presented " + std::to_string(frames) + " frames.");
    return 0;
}
