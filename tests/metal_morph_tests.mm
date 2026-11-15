#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <astra/metal/metal_device.hpp>
#include <astra/rhi/render_device.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#ifndef ASTRA_METALLIB_PATH
#error ASTRA_METALLIB_PATH must point to the compiled Astra Metal library
#endif

namespace {

struct alignas(16) PackedMorphDelta final {
    float position[4];
    float normal[4];
    float tangent[4];
};
static_assert(sizeof(PackedMorphDelta) == 48);

struct MorphValidationUniforms final {
    std::uint32_t target_count{};
    std::uint32_t vertex_count{};
    std::uint32_t per_instance{};
    std::uint32_t reserved{};
};

struct MorphValidationOutput final {
    float current_position[4];
    float previous_position[4];
    float current_normal[4];
    float current_tangent[4];
};

struct PackedMeshVertex final {
    float position[3];
    std::int16_t normal[4];
    std::uint8_t color[4];
    std::uint16_t texcoord0[2];
    std::uint32_t tangent;
    std::uint16_t joints[4];
    std::uint16_t weights[4];
    std::uint16_t texcoord1[2];
};
static_assert(sizeof(PackedMeshVertex) == 52);

struct SceneUniforms final {
    float view_projection[16];
    float model[16];
    float light_view_projection[3][16];
    float camera_position[4];
    float light_direction[4];
    float light_radiance[4];
    float material[4];
    float base_color[4];
    float emissive[4];
    float material_flags[4];
    float texture_flags2[4];
    float skin_flags[4];
    float scene_params[4];
    float texcoord_sets0[4];
    float texcoord_sets1[4];
    float alpha_flags[4];
    float cascade_splits[4];
    float cascade_camera_forward[4];
    float previous_view_projection[16];
    float previous_model[16];
    float temporal_params[4];
    float uv_transform[5][4];
    float uv_rotation[5][4];
    float environment_flags[4];
    std::uint32_t morph_flags[4];
};
static_assert(sizeof(SceneUniforms) == 896);
static_assert(offsetof(SceneUniforms, morph_flags) == 880);

id<MTLBuffer> make_buffer(id<MTLDevice> device, const void* data, std::size_t size) {
    return [device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
}

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 1.0e-4F;
}

const char* error_message(NSError* error, const char* fallback) {
    const char* message = error.localizedDescription.UTF8String;
    return message != nullptr ? message : fallback;
}

void expect_vec3(const float* actual, float x, float y, float z) {
    assert(near(actual[0], x));
    assert(near(actual[1], y));
    assert(near(actual[2], z));
}

void set_scaled_identity(float* matrix, float scale) {
    matrix[0] = scale;
    matrix[5] = scale;
    matrix[10] = scale;
    matrix[15] = 1.0F;
}

MTLVertexDescriptor* make_test_vertex_descriptor() {
    MTLVertexDescriptor* descriptor = [[MTLVertexDescriptor alloc] init];
    const std::array<MTLVertexFormat, 8> formats{
        MTLVertexFormatFloat3, MTLVertexFormatShort4Normalized,
        MTLVertexFormatUChar4Normalized, MTLVertexFormatHalf2,
        MTLVertexFormatInt1010102Normalized, MTLVertexFormatUShort4,
        MTLVertexFormatUShort4Normalized, MTLVertexFormatHalf2};
    const std::array<NSUInteger, 8> offsets{
        offsetof(PackedMeshVertex, position), offsetof(PackedMeshVertex, normal),
        offsetof(PackedMeshVertex, color), offsetof(PackedMeshVertex, texcoord0),
        offsetof(PackedMeshVertex, tangent), offsetof(PackedMeshVertex, joints),
        offsetof(PackedMeshVertex, weights), offsetof(PackedMeshVertex, texcoord1)};
    for (NSUInteger attribute = 0; attribute < formats.size(); ++attribute) {
        descriptor.attributes[attribute].format = formats[attribute];
        descriptor.attributes[attribute].offset = offsets[attribute];
        descriptor.attributes[attribute].bufferIndex = 0;
    }
    descriptor.layouts[0].stride = sizeof(PackedMeshVertex);
    descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    return descriptor;
}

std::array<float, 4> find_rendered_pixel(id<MTLTexture> texture) {
    constexpr NSUInteger width = 64;
    constexpr NSUInteger height = 64;
    std::vector<float> pixels(width * height * 4U);
    [texture getBytes:pixels.data()
          bytesPerRow:width * 4U * sizeof(float)
           fromRegion:MTLRegionMake2D(0, 0, width, height)
          mipmapLevel:0];
    for (std::size_t pixel = 0; pixel < width * height; ++pixel) {
        const auto* value = pixels.data() + pixel * 4U;
        if (value[3] > 0.01F)
            return {value[0], value[1], value[2], value[3]};
    }
    return {};
}

std::array<float, 4> render_production_vertex_entry(id<MTLDevice> device,
                                                   id<MTLLibrary> library,
                                                   NSString* vertex_name,
                                                   NSString* fragment_name,
                                                   SceneUniforms uniforms) {
    id<MTLFunction> vertex_function = [library newFunctionWithName:vertex_name];
    id<MTLFunction> fragment_function = [library newFunctionWithName:fragment_name];
    assert(vertex_function != nil && fragment_function != nil);
    MTLRenderPipelineDescriptor* pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.vertexFunction = vertex_function;
    pipeline_descriptor.fragmentFunction = fragment_function;
    pipeline_descriptor.vertexDescriptor = make_test_vertex_descriptor();
    pipeline_descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
    pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
    if (pipeline == nil) {
        std::cerr << "Could not create production morph vertex pipeline: "
                  << error_message(error, "unknown pipeline error") << '\n';
        std::abort();
    }

    PackedMeshVertex vertex{};
    vertex.position[0] = 0.0F;
    vertex.position[1] = 0.0F;
    vertex.position[2] = 0.0F;
    vertex.normal[2] = 32767;
    for (auto& channel : vertex.color)
        channel = 255;
    vertex.tangent = 0x400001FFU;
    vertex.weights[0] = 65535U;
    const std::array<PackedMorphDelta, 2> morph_deltas{{
        {{0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.1F, 0.0F, 0.0F},
         {0.0F, 1.0F, 0.0F, 0.0F}},
        {{0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.1F, 0.0F},
         {0.0F, 0.0F, 1.0F, 0.0F}},
    }};
    const std::array<float, 2> current_weights{0.5F, 0.25F};
    const std::array<float, 2> previous_weights{0.25F, 0.0F};
    std::array<float, 16> current_joint{};
    std::array<float, 16> previous_joint{};
    std::array<float, 16> identity{};
    set_scaled_identity(current_joint.data(), 1.0F);
    set_scaled_identity(previous_joint.data(), 1.0F);
    set_scaled_identity(identity.data(), 1.0F);
    current_joint[12] = 0.2F;
    previous_joint[12] = 0.1F;

    id<MTLBuffer> vertex_buffer = make_buffer(device, &vertex, sizeof(vertex));
    id<MTLBuffer> current_joint_buffer = make_buffer(device, current_joint.data(), sizeof(current_joint));
    id<MTLBuffer> previous_joint_buffer = make_buffer(device, previous_joint.data(), sizeof(previous_joint));
    id<MTLBuffer> current_instance_buffer = make_buffer(device, identity.data(), sizeof(identity));
    id<MTLBuffer> previous_instance_buffer = make_buffer(device, identity.data(), sizeof(identity));
    id<MTLBuffer> current_weight_buffer = make_buffer(device, current_weights.data(), sizeof(current_weights));
    id<MTLBuffer> previous_weight_buffer = make_buffer(device, previous_weights.data(), sizeof(previous_weights));
    id<MTLBuffer> morph_delta_buffer = make_buffer(device, morph_deltas.data(), sizeof(morph_deltas));

    MTLTextureDescriptor* texture_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:64 height:64 mipmapped:NO];
    texture_descriptor.storageMode = MTLStorageModeShared;
    texture_descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:texture_descriptor];
    assert(texture != nil);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    assert(encoder != nil);
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
    [encoder setVertexBuffer:current_joint_buffer offset:0 atIndex:2];
    [encoder setVertexBuffer:previous_joint_buffer offset:0 atIndex:3];
    [encoder setVertexBuffer:current_instance_buffer offset:0 atIndex:4];
    [encoder setVertexBuffer:previous_instance_buffer offset:0 atIndex:5];
    [encoder setVertexBuffer:current_weight_buffer offset:0 atIndex:6];
    [encoder setVertexBuffer:previous_weight_buffer offset:0 atIndex:7];
    [encoder setVertexBuffer:morph_delta_buffer offset:0 atIndex:8];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        std::cerr << "Production morph vertex pass failed: "
                  << error_message(command.error, "unknown GPU error") << '\n';
        std::abort();
    }
    return find_rendered_pixel(texture);
}

} // namespace

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::cerr << "No Metal device is available; skipping GPU morph validation.\n";
            return 77;
        }

        NSError* error = nil;
        NSString* library_path = [NSString stringWithUTF8String:ASTRA_METALLIB_PATH];
        NSURL* library_url = [NSURL fileURLWithPath:library_path];
        id<MTLLibrary> library = [device newLibraryWithURL:library_url error:&error];
        if (library == nil) {
            std::cerr << "Could not load Astra metallib: "
                      << error_message(error, "unknown error") << '\n';
            return 1;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"astra_morph_validation_kernel"];
        if (function == nil) {
            std::cerr << "The Astra morph validation kernel is missing from the metallib.\n";
            return 1;
        }
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (pipeline == nil || queue == nil) {
            std::cerr << "Could not create the Metal morph validation pipeline: "
                      << error_message(error, "unknown error") << '\n';
            return 1;
        }

        CAMetalLayer* layer = [CAMetalLayer layer];
        auto render_device = astra::metal::create_metal_render_device((__bridge void*)layer);
        if (!render_device) {
            std::cerr << "Could not create the Metal RHI for mesh upload validation: "
                      << render_device.error().message << '\n';
            return 1;
        }
        std::array<astra::rhi::MeshVertex, 3> mesh_vertices{};
        mesh_vertices[0].position = {0.0F, 0.0F, 0.0F};
        mesh_vertices[1].position = {1.0F, 0.0F, 0.0F};
        mesh_vertices[2].position = {0.0F, 1.0F, 0.0F};
        for (auto& vertex : mesh_vertices) {
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.color = {1.0F, 1.0F, 1.0F, 1.0F};
            vertex.tangent_x = 1.0F;
            vertex.tangent_w = 1.0F;
        }
        const std::array<std::uint32_t, 3> mesh_indices{0, 1, 2};
        const std::array<astra::rhi::Vec3, 3> mesh_position_deltas{{
            {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 0.1F}}};
        const std::array<astra::rhi::Vec3, 3> mesh_normal_deltas{{
            {0.0F, 0.1F, 0.0F}, {0.0F, 0.1F, 0.0F}, {0.0F, 0.1F, 0.0F}}};
        const std::array<astra::rhi::Vec3, 3> mesh_tangent_deltas{{
            {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 0.1F}}};
        const std::array<astra::rhi::MeshData::MorphTarget, 2> mesh_morph_targets{{
            {.positions = mesh_position_deltas, .normals = mesh_normal_deltas,
             .tangents = mesh_tangent_deltas},
            {.positions = mesh_position_deltas},
        }};
        auto uploaded_mesh = (*render_device)->create_mesh(
            {mesh_vertices, mesh_indices, mesh_morph_targets});
        if (!uploaded_mesh) {
            std::cerr << "Metal RHI morph mesh upload failed: " << uploaded_mesh.error().message << '\n';
            return 1;
        }
        uploaded_mesh->reset();
        const std::array<astra::rhi::Vec3, 2> invalid_delta_count{};
        const std::array<astra::rhi::MeshData::MorphTarget, 1> invalid_morph_targets{{
            {.positions = invalid_delta_count},
        }};
        auto invalid_mesh = (*render_device)->create_mesh(
            {mesh_vertices, mesh_indices, invalid_morph_targets});
        assert(!invalid_mesh);
        render_device->reset();

        constexpr std::uint32_t vertex_count = 143446;
        const PackedMorphDelta first_target{
            {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.1F, 0.0F, 0.0F},
            {0.0F, 1.0F, 0.0F, 0.0F}};
        const PackedMorphDelta second_target{
            {0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.1F, 0.0F},
            {0.0F, 0.0F, 1.0F, 0.0F}};
        std::vector<std::array<float, 4>> positions(vertex_count, {1.0F, 0.0F, 0.0F, 0.0F});
        std::vector<std::array<float, 4>> normals(vertex_count, {0.0F, 1.0F, 0.0F, 0.0F});
        std::vector<std::array<float, 4>> tangents(vertex_count, {1.0F, 0.0F, 0.0F, 1.0F});
        std::vector<std::array<std::uint16_t, 4>> joints(vertex_count, {0, 0, 0, 0});
        std::vector<std::array<float, 4>> skin_weights(vertex_count, {1.0F, 0.0F, 0.0F, 0.0F});
        std::vector<PackedMorphDelta> morph_deltas(static_cast<std::size_t>(vertex_count) * 2U);
        for (std::uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
            morph_deltas[vertex] = first_target;
            morph_deltas[vertex_count + vertex] = second_target;
        }
        const std::array<float, 4> current_weights{0.5F, 0.25F, 0.0F, 1.0F};
        const std::array<float, 4> previous_weights{0.25F, 0.0F, 0.5F, 0.5F};
        std::array<float, 16> current_joint{};
        std::array<float, 16> previous_joint{};
        current_joint[0] = current_joint[5] = current_joint[10] = current_joint[15] = 1.0F;
        previous_joint[0] = previous_joint[5] = previous_joint[10] = previous_joint[15] = 1.0F;
        current_joint[12] = 2.0F;
        previous_joint[12] = 1.0F;
        const MorphValidationUniforms uniforms{.target_count = 2, .vertex_count = vertex_count,
                                                .per_instance = 1};

        id<MTLBuffer> position_buffer = make_buffer(device, positions.data(), positions.size() * sizeof(positions[0]));
        id<MTLBuffer> normal_buffer = make_buffer(device, normals.data(), normals.size() * sizeof(normals[0]));
        id<MTLBuffer> tangent_buffer = make_buffer(device, tangents.data(), tangents.size() * sizeof(tangents[0]));
        id<MTLBuffer> joint_index_buffer = make_buffer(device, joints.data(), joints.size() * sizeof(joints[0]));
        id<MTLBuffer> skin_weight_buffer = make_buffer(device, skin_weights.data(), skin_weights.size() * sizeof(skin_weights[0]));
        id<MTLBuffer> morph_delta_buffer = make_buffer(device, morph_deltas.data(), morph_deltas.size() * sizeof(morph_deltas[0]));
        id<MTLBuffer> current_weight_buffer = make_buffer(device, current_weights.data(), sizeof(current_weights));
        id<MTLBuffer> previous_weight_buffer = make_buffer(device, previous_weights.data(), sizeof(previous_weights));
        id<MTLBuffer> current_joint_buffer = make_buffer(device, current_joint.data(), sizeof(current_joint));
        id<MTLBuffer> previous_joint_buffer = make_buffer(device, previous_joint.data(), sizeof(previous_joint));
        id<MTLBuffer> output_buffer = [device newBufferWithLength:
                                                         static_cast<std::size_t>(vertex_count) * 2U * sizeof(MorphValidationOutput)
                                                         options:MTLResourceStorageModeShared];
        if (position_buffer == nil || normal_buffer == nil || tangent_buffer == nil ||
            joint_index_buffer == nil || skin_weight_buffer == nil || morph_delta_buffer == nil ||
            current_weight_buffer == nil || previous_weight_buffer == nil ||
            current_joint_buffer == nil || previous_joint_buffer == nil || output_buffer == nil) {
            std::cerr << "Could not allocate Metal morph validation buffers.\n";
            return 1;
        }

        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil) {
            std::cerr << "Could not create the Metal morph validation command encoder.\n";
            return 1;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:position_buffer offset:0 atIndex:0];
        [encoder setBuffer:normal_buffer offset:0 atIndex:1];
        [encoder setBuffer:tangent_buffer offset:0 atIndex:2];
        [encoder setBuffer:joint_index_buffer offset:0 atIndex:3];
        [encoder setBuffer:skin_weight_buffer offset:0 atIndex:4];
        [encoder setBuffer:morph_delta_buffer offset:0 atIndex:5];
        [encoder setBuffer:current_weight_buffer offset:0 atIndex:6];
        [encoder setBuffer:previous_weight_buffer offset:0 atIndex:7];
        [encoder setBuffer:current_joint_buffer offset:0 atIndex:8];
        [encoder setBuffer:previous_joint_buffer offset:0 atIndex:9];
        [encoder setBytes:&uniforms length:sizeof(uniforms) atIndex:10];
        [encoder setBuffer:output_buffer offset:0 atIndex:11];
        [encoder dispatchThreads:MTLSizeMake(vertex_count, 2, 1)
            threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
        [encoder endEncoding];

        // The command buffer must retain every resource referenced by the encoded dispatch.
        position_buffer = nil;
        normal_buffer = nil;
        tangent_buffer = nil;
        joint_index_buffer = nil;
        skin_weight_buffer = nil;
        morph_delta_buffer = nil;
        current_weight_buffer = nil;
        previous_weight_buffer = nil;
        current_joint_buffer = nil;
        previous_joint_buffer = nil;
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            std::cerr << "Metal morph validation failed: "
                      << error_message(command.error, "unknown GPU error") << '\n';
            return 1;
        }

        const auto* output = static_cast<const MorphValidationOutput*>(output_buffer.contents);
        expect_vec3(output[0].current_position, 3.0F, 0.5F, 0.25F);
        expect_vec3(output[0].previous_position, 2.0F, 0.25F, 0.0F);
        expect_vec3(output[0].current_normal, 0.0F, 1.05F, 0.025F);
        expect_vec3(output[0].current_tangent, 1.0F, 0.5F, 0.25F);
        const auto second_instance = static_cast<std::size_t>(vertex_count);
        expect_vec3(output[second_instance].current_position, 3.0F, 0.0F, 1.0F);
        expect_vec3(output[second_instance].previous_position, 2.0F, 0.5F, 0.5F);
        expect_vec3(output[second_instance + vertex_count - 1U].current_position,
                    3.0F, 0.0F, 1.0F);

        SceneUniforms scene{};
        set_scaled_identity(scene.view_projection, 0.5F);
        set_scaled_identity(scene.model, 1.0F);
        for (auto& light_view_projection : scene.light_view_projection)
            set_scaled_identity(light_view_projection, 0.5F);
        set_scaled_identity(scene.previous_view_projection, 0.5F);
        set_scaled_identity(scene.previous_model, 1.0F);
        scene.camera_position[3] = 1.0F;
        scene.skin_flags[0] = 1.0F;
        scene.skin_flags[1] = 1.0F;
        scene.temporal_params[0] = 1.0F;
        scene.morph_flags[0] = 2U;
        scene.morph_flags[1] = 0U;
        scene.morph_flags[2] = 1U;
        const auto vertex_pixel = render_production_vertex_entry(
            device, library, @"astra_vertex", @"astra_morph_validation_fragment", scene);
        expect_vec3(vertex_pixel.data(), 0.1F, 0.25F, 0.05F);
        assert(near(vertex_pixel[3], 0.125F));

        scene.scene_params[3] = 0.0F;
        const auto shadow_pixel = render_production_vertex_entry(
            device, library, @"astra_shadow_vertex", @"astra_morph_validation_shadow_fragment", scene);
        assert(near(shadow_pixel[0], 0.125F));

        if (command.GPUEndTime > command.GPUStartTime) {
            const double gpu_ms = (command.GPUEndTime - command.GPUStartTime) * 1000.0;
            std::cout << "GPU morph + skinning validation dispatch: " << gpu_ms << " ms\n";
        }
        std::cout << "Metal morph tests passed: RHI morph mesh upload/validation, multiple targets, "
                     "per-instance weights, morph-before-skin order, previous-frame positions, "
                     "and command-buffer resource lifetime.\n";
        return 0;
    }
}
