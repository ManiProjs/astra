#include <astra/assets/animation.hpp>
#include <astra/assets/gltf.hpp>
#include <astra/assets/image.hpp>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <array>
#include <fstream>
#include <string_view>
#include <vector>

int main() {
    const auto loaded = astra::assets::load_gltf(std::filesystem::path{ASTRA_CESIUMMAN_PATH});
    assert(loaded);
    assert(loaded->meshes.size() == 1);
    assert(loaded->nodes.size() == 22);
    assert(loaded->skins.size() == 1);
    assert(loaded->animations.size() == 1);
    assert(loaded->images.size() == 1);
    assert(!loaded->skins.front().joints.empty());
    assert(loaded->skins.front().joints.size() ==
           loaded->skins.front().inverse_bind_matrices.size());
    assert(!loaded->animations.front().channels.empty());

    const auto pose = astra::assets::evaluate_animation(
        loaded->animations.front(), loaded->nodes, loaded->animations.front().duration * 0.5F);
    assert(pose && pose->size() == loaded->nodes.size());
    for (const auto& node : *pose) {
        assert(std::isfinite(node.translation.x));
        assert(std::isfinite(node.translation.y));
        assert(std::isfinite(node.translation.z));
        assert(std::isfinite(node.rotation.w));
        assert(std::isfinite(node.scale.x));
        assert(std::isfinite(node.scale.y));
        assert(std::isfinite(node.scale.z));
    }

    const auto player = astra::assets::load_gltf(std::filesystem::path{ASTRA_PLAYER_ASSET_PATH});
    assert(player);
    assert(player->meshes.size() == 1);
    assert(player->meshes.front().primitives.size() >= 8);
    assert(player->materials.size() >= 8);
    assert(player->images.size() >= 10);
    assert(player->skins.size() == 1);
    assert(player->skins.front().joints.size() == 69);
    assert(player->skins.front().joints.size() ==
           player->skins.front().inverse_bind_matrices.size());
    assert(player->animations.size() == 3);
    for (const auto& image : player->images) {
        const auto decoded = astra::assets::decode_image(image.bytes, image.mime_type);
        assert(decoded);
        assert(decoded->width <= 512);
        assert(decoded->height <= 512);
    }

    const auto human = astra::assets::load_gltf(std::filesystem::path{ASTRA_BOAR_HUMAN_ASSET_PATH});
    const auto boar = astra::assets::load_gltf(std::filesystem::path{ASTRA_BOAR_ASSET_PATH});
    assert(human);
    assert(boar);
    assert(human->meshes.size() == 1);
    assert(human->meshes.front().primitives.size() == 1);
    const auto& human_primitive = human->meshes.front().primitives.front();
    assert(human_primitive.vertices.size() > 140000);
    assert(human_primitive.indices.size() > 780000);
    assert(human->materials.size() == 1);
    assert(human->images.size() >= 3);
    for (const auto& image : human->images) {
        const auto decoded = astra::assets::decode_image(image.bytes, image.mime_type);
        assert(decoded);
        assert(decoded->width >= 1024);
        assert(decoded->height >= 1024);
    }
    assert(boar->meshes.size() == 3);
    assert(boar->materials.size() == 3);
    assert(boar->nodes.size() == 4);
    std::size_t boar_vertices = 0;
    std::size_t boar_indices = 0;
    for (const auto& mesh : boar->meshes) {
        assert(mesh.primitives.size() == 1);
        const auto& primitive = mesh.primitives.front();
        assert(primitive.mode == 4U);
        boar_vertices += primitive.vertices.size();
        boar_indices += primitive.indices.size();
        for (const auto& vertex : primitive.vertices) {
            assert(std::isfinite(vertex.position.x));
            assert(std::isfinite(vertex.position.y));
            assert(std::isfinite(vertex.position.z));
            assert(std::isfinite(vertex.normal.x));
            assert(std::isfinite(vertex.normal.y));
            assert(std::isfinite(vertex.normal.z));
        }
    }
    assert(boar_vertices > 4000);
    assert(boar_indices > 4000);
    for (const auto& animation : player->animations) {
        assert(!animation.channels.empty());
        const auto player_pose = astra::assets::evaluate_animation(
            animation, player->nodes, animation.duration * 0.5F);
        assert(player_pose && player_pose->size() == player->nodes.size());
        for (const auto& node : *player_pose) {
            assert(std::isfinite(node.translation.x));
            assert(std::isfinite(node.translation.y));
            assert(std::isfinite(node.translation.z));
            assert(std::isfinite(node.rotation.w));
        }
    }

    constexpr std::array<std::string_view, 6> nature_assets{
        "jacaranda_tree.glb", "fir_sapling.glb", "shrub_04.glb", "fern_02.glb",
        "rock_moss_set_01.glb", "island_tree_02.glb"};
    for (const auto relative_path : nature_assets) {
        const auto nature = astra::assets::load_gltf(
            std::filesystem::path{ASTRA_NATURE_ASSET_ROOT} / relative_path);
        assert(nature);
        assert(!nature->nodes.empty());
        assert(!nature->meshes.empty());
        assert(!nature->materials.empty());
        assert(!nature->images.empty());
        for (const auto& image : nature->images) {
            const auto decoded = astra::assets::decode_image(image.bytes, image.mime_type);
            assert(decoded);
            assert(decoded->width <= 256);
            assert(decoded->height <= 256);
        }
        for (const auto& mesh : nature->meshes) {
            assert(!mesh.primitives.empty());
            for (const auto& primitive : mesh.primitives) {
                assert(primitive.mode == 4U);
                assert(!primitive.vertices.empty());
                assert(!primitive.indices.empty());
                for (const auto& vertex : primitive.vertices) {
                    assert(std::isfinite(vertex.position.x));
                    assert(std::isfinite(vertex.position.y));
                    assert(std::isfinite(vertex.position.z));
                    assert(std::isfinite(vertex.normal.x));
                    assert(std::isfinite(vertex.normal.y));
                    assert(std::isfinite(vertex.normal.z));
                    assert(std::isfinite(vertex.texcoord0.x));
                    assert(std::isfinite(vertex.texcoord0.y));
                }
            }
        }
        for (const auto& material : nature->materials) {
            assert(std::isfinite(material.base_color_texture.uv_rotation));
            assert(std::isfinite(material.base_color_texture.uv_scale.x));
            assert(std::isfinite(material.base_color_texture.uv_scale.y));
        }
    }

    const auto verify_terrain_texture = [](std::string_view filename, std::string_view mime) {
        const auto path = std::filesystem::path{ASTRA_TERRAIN_ASSET_ROOT} / filename;
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        assert(stream);
        const auto end = stream.tellg();
        assert(end > 0);
        std::vector<std::byte> encoded(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(encoded.data()),
                    static_cast<std::streamsize>(encoded.size()));
        assert(stream);
        const auto decoded = astra::assets::decode_image(encoded, mime);
        assert(decoded);
        assert(decoded->width == 1024);
        assert(decoded->height == 1024);
    };
    verify_terrain_texture("forest_ground_04_diff_1k.jpg", "image/jpeg");
    verify_terrain_texture("forest_ground_04_nor_gl_1k.png", "image/png");
    verify_terrain_texture("forest_ground_04_arm_1k.png", "image/png");
    return 0;
}
