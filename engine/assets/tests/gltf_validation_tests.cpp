#include <astra/assets/gltf.hpp>
#include <astra/assets/animation.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace astra::assets;

namespace {

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void write_animation_fixture(const std::filesystem::path& json_path,
                             const std::filesystem::path& bin_path, bool duplicate_times,
                             bool weights) {
    std::vector<std::byte> bytes(480);
    const std::array<float, 8> values{
        0.0F, duplicate_times ? 0.0F : 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 2.0F, 3.0F};
    std::memcpy(bytes.data(), values.data(), sizeof(values));
    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + bin_path.filename().string() +
        "\",\"byteLength\":480}],\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":8},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}],"
        "\"nodes\":[{}],\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"" +
        (weights ? "weights" : "translation") + "\"}}]}]}";
    write_file(json_path, json);
}

void write_color_fixture(const std::filesystem::path& json_path,
                         const std::filesystem::path& bin_path, bool integer_uv) {
    std::vector<std::byte> bytes(78U);
    const std::array<float, 9> positions{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    const std::array<float, 9> colors{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const std::array<std::uint8_t, 6> uv{0, 0, 255, 0, 0, 255};
    std::memcpy(bytes.data(), positions.data(), sizeof(positions));
    std::memcpy(bytes.data() + 36, colors.data(), sizeof(colors));
    if (integer_uv)
        std::memcpy(bytes.data() + 72, uv.data(), sizeof(uv));
    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" +
                       bin_path.filename().string() +
                       "\",\"byteLength\":" + std::to_string(bytes.size()) +
                       "}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":6}],\"accessors\":["
                       "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                       "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}";
    if (integer_uv)
        json += ",{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC2\"}";
    json += "],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"COLOR_0\":1";
    if (integer_uv)
        json += ",\"TEXCOORD_0\":2";
    json += "}}]}]}";
    write_file(json_path, json);
}

void write_occlusion_fixture(const std::filesystem::path& json_path) {
    write_file(json_path,
               "{\"asset\":{\"version\":\"2.0\"},"
               "\"images\":[{\"uri\":\"data:application/octet-stream;base64,AA==\"}],"
               "\"textures\":[{\"source\":0}],"
               "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":0.25}}]}");
}

void write_morph_fixture(const std::filesystem::path& json_path,
                         const std::filesystem::path& bin_path,
                         const std::string& mesh_weights, const std::string& node_weights) {
    const std::array<float, 45> values{
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.2F, 0.0F, 0.0F, 0.2F, 0.0F, 0.0F, 0.2F,
        0.0F, 0.1F, 0.0F, 0.0F, 0.1F, 0.0F, 0.0F, 0.1F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F};
    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(values.data()), sizeof(values));
    }
    write_file(json_path,
        "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" +
        bin_path.filename().string() + "\",\"byteLength\":180}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":108,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":116,\"byteLength\":16},"
        "{\"buffer\":0,\"byteOffset\":132,\"byteLength\":48}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":4,\"type\":\"SCALAR\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":12,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"weights\":" + mesh_weights +
        ",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"targets\":[{\"POSITION\":1},{\"NORMAL\":2}]}]}],"
        "\"nodes\":[{\"mesh\":0,\"weights\":" + node_weights + "}],"
        "\"animations\":["
        "{\"samplers\":[{\"input\":3,\"output\":4}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]},"
        "{\"samplers\":[{\"input\":3,\"output\":5,\"interpolation\":\"CUBICSPLINE\"}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]}");
}

void write_sampler_material_fixture(const std::filesystem::path& json_path) {
    write_file(
        json_path,
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"images\":[{\"uri\":\"data:application/octet-stream;base64,AA==\"}],"
        "\"samplers\":["
        "{\"magFilter\":9728,\"minFilter\":9984,\"wrapS\":33648,\"wrapT\":33071},"
        "{\"magFilter\":9729,\"minFilter\":9729}],"
        "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":0,\"sampler\":1},{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.25,0.5],\"scale\":[2.0,3.0],\"rotation\":1.57079632679,\"texCoord\":1}}},"
        "\"metallicRoughnessTexture\":{\"index\":1}},\"normalTexture\":{\"index\":2},"
        "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.37,\"doubleSided\":true},"
        "{\"alphaMode\":\"BLEND\"},{\"alphaMode\":\"OPAQUE\"}]}");
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path();
    const auto json_path = directory / "astra_gltf_validation_test.gltf";
    const auto bin_path = directory / "astra_gltf_validation_test.bin";
    write_animation_fixture(json_path, bin_path, false, false);
    assert(load_gltf(json_path));

    write_animation_fixture(json_path, bin_path, true, false);
    assert(!load_gltf(json_path)); // Animation key times must be strictly increasing.
    write_animation_fixture(json_path, bin_path, false, true);
    assert(!load_gltf(json_path)); // Morph-weight animation is not implemented.

    write_animation_fixture(json_path, bin_path, false, false);
    LoadOptions limited;
    limited.maximum_file_bytes = 500;
    assert(!load_gltf(json_path, limited)); // Aggregate decoded resource budget is enforced.

    const auto color_path = directory / "astra_gltf_color_test.gltf";
    const auto color_bin_path = directory / "astra_gltf_color_test.bin";
    write_color_fixture(color_path, color_bin_path, false);
    const auto color_asset = load_gltf(color_path);
    assert(color_asset && color_asset->meshes.size() == 1);
    const auto& color = color_asset->meshes.front().primitives.front().vertices.front().color;
    assert(color.x == 1.0F && color.y == 0.0F && color.z == 0.0F && color.w == 1.0F);
    write_color_fixture(color_path, color_bin_path, true);
    assert(!load_gltf(color_path)); // Integer texture coordinates must be normalized.

    const auto occlusion_path = directory / "astra_gltf_occlusion_test.gltf";
    write_occlusion_fixture(occlusion_path);
    const auto occlusion_asset = load_gltf(occlusion_path);
    assert(occlusion_asset && occlusion_asset->materials.size() == 1);
    assert(occlusion_asset->materials.front().occlusion_texture.scale == 0.25F);

    const auto sampler_path = directory / "astra_gltf_sampler_test.gltf";
    write_sampler_material_fixture(sampler_path);
    const auto sampler_asset = load_gltf(sampler_path);
    assert(sampler_asset && sampler_asset->samplers.size() == 2);
    assert(sampler_asset->textures.size() == 3 && sampler_asset->materials.size() == 3);
    const auto& nearest = sampler_asset->textures[0].sampling;
    assert(nearest.mag_filter == TextureFilter::nearest);
    assert(nearest.min_filter == TextureFilter::nearest);
    assert(nearest.mip_filter == TextureMipFilter::nearest);
    assert(nearest.wrap_s == TextureWrap::mirrored_repeat);
    assert(nearest.wrap_t == TextureWrap::clamp_to_edge);
    const auto& no_mips = sampler_asset->textures[1].sampling;
    assert(no_mips.min_filter == TextureFilter::linear);
    assert(no_mips.mip_filter == TextureMipFilter::none);
    const auto& defaults = sampler_asset->textures[2].sampling;
    assert(defaults.mag_filter == TextureFilter::linear);
    assert(defaults.min_filter == TextureFilter::linear);
    assert(defaults.mip_filter == TextureMipFilter::linear);
    assert(defaults.wrap_s == TextureWrap::repeat && defaults.wrap_t == TextureWrap::repeat);
    const auto& material = sampler_asset->materials.front();
    assert(material.alpha_mode == 1 && material.alpha_cutoff == 0.37F && material.double_sided);
    assert(material.base_color_texture.texture == 0);
    assert(material.base_color_texture.texcoord == 1);
    assert(material.base_color_texture.uv_offset.x == 0.25F);
    assert(material.base_color_texture.uv_offset.y == 0.5F);
    assert(material.base_color_texture.uv_scale.x == 2.0F);
    assert(material.base_color_texture.uv_scale.y == 3.0F);
    assert(std::abs(material.base_color_texture.uv_rotation - 1.57079632679F) < 1.0e-6F);
    assert(material.metallic_roughness_texture.texture == 1);
    assert(material.metallic_roughness_texture.uv_scale.x == 1.0F);
    assert(material.metallic_roughness_texture.uv_scale.y == 1.0F);
    assert(material.normal_texture.texture == 2);
    assert(sampler_asset->materials[1].alpha_mode == 2);
    assert(sampler_asset->materials[1].alpha_cutoff == 0.5F);
    assert(!sampler_asset->materials[1].double_sided);
    assert(sampler_asset->materials[2].alpha_mode == 0);

    write_file(sampler_path,
               "{\"asset\":{\"version\":\"2.0\"},\"samplers\":[{\"minFilter\":1234}]}");
    assert(!load_gltf(sampler_path)); // Unknown sampler enumerants are rejected.

    const auto morph_path = directory / "astra_gltf_morph_test.gltf";
    const auto morph_bin_path = directory / "astra_gltf_morph_test.bin";
    write_morph_fixture(morph_path, morph_bin_path, "[0.25, 0.5]", "[0.75, 0.2]");
    const auto morph_asset = load_gltf(morph_path);
    assert(morph_asset && morph_asset->meshes.size() == 1 && morph_asset->nodes.size() == 1);
    const auto& morph_mesh = morph_asset->meshes.front();
    assert(morph_mesh.weights.size() == 2 && morph_mesh.weights[0] == 0.25F &&
           morph_mesh.weights[1] == 0.5F);
    assert(morph_asset->nodes.front().weights.size() == 2 &&
           morph_asset->nodes.front().weights[0] == 0.75F &&
           morph_asset->nodes.front().weights[1] == 0.2F);
    const auto& morph = morph_mesh.primitives.front().morph_targets.front();
    assert(morph.positions.size() == 3 && morph.normals.empty() && morph.tangents.empty());
    assert(morph.positions[0].z == 0.2F && morph.positions[2].z == 0.2F);
    const auto& second_morph = morph_mesh.primitives.front().morph_targets[1];
    assert(second_morph.positions.empty() && second_morph.normals.size() == 3 &&
           second_morph.tangents.empty());
    assert(morph_asset->animations.size() == 2);
    auto morph_pose = evaluate_animation(morph_asset->animations.front(),
                                         morph_asset->nodes, 0.5F);
    assert(morph_pose && morph_pose->front().morph_weights.size() == 2);
    assert(std::abs(morph_pose->front().morph_weights[0] - 0.5F) < 1.0e-5F);
    assert(std::abs(morph_pose->front().morph_weights[1] - 0.5F) < 1.0e-5F);
    auto cubic_morph_pose = evaluate_animation(morph_asset->animations[1],
                                              morph_asset->nodes, 0.5F);
    assert(cubic_morph_pose && cubic_morph_pose->front().morph_weights.size() == 2);
    assert(std::abs(cubic_morph_pose->front().morph_weights[0] - 0.5F) < 1.0e-5F);
    assert(std::abs(cubic_morph_pose->front().morph_weights[1] - 0.5F) < 1.0e-5F);
    write_morph_fixture(morph_path, morph_bin_path, "[0.25]", "[0.75, 0.2]");
    assert(!load_gltf(morph_path)); // Default weight count must match the target count.

    std::filesystem::remove(json_path);
    std::filesystem::remove(bin_path);
    std::filesystem::remove(color_path);
    std::filesystem::remove(color_bin_path);
    std::filesystem::remove(occlusion_path);
    std::filesystem::remove(sampler_path);
    std::filesystem::remove(morph_path);
    std::filesystem::remove(morph_bin_path);
}
