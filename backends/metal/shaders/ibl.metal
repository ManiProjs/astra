#include <metal_stdlib>
using namespace metal;

// CPU layout: alignas(16), exactly 32 bytes. dimensions = width, height,
// face_count (six for cube dispatches), sample_count. parameters = roughness,
// source_width, source_height, reserved.
struct AstraIblUniforms {
    uint4 dimensions;
    float4 parameters;
};

constant uint ASTRA_IBL_MAX_IRRADIANCE_SAMPLES = 64U;
constant uint ASTRA_IBL_MAX_GGX_SAMPLES = 128U;
constant float ASTRA_IBL_PI = 3.14159265358979323846F;

// Cube convention, matching the six 2D-array slices in +X, -X, +Y, -Y, +Z,
// -Z order. UV origin is the upper-left; u and v span [-1,+1]. Keep this
// mapping identical when uploading/rendering any other cube-map source.
float3 astra_ibl_cube_direction(uint face, float2 uv) {
    const float2 p = uv * 2.0F - 1.0F;
    switch (face) {
    case 0U: return normalize(float3(1.0F, -p.y, -p.x));
    case 1U: return normalize(float3(-1.0F, -p.y, p.x));
    case 2U: return normalize(float3(p.x, 1.0F, p.y));
    case 3U: return normalize(float3(p.x, -1.0F, -p.y));
    case 4U: return normalize(float3(p.x, -p.y, 1.0F));
    default: return normalize(float3(-p.x, -p.y, -1.0F));
    }
}

float2 astra_ibl_equirect_uv(float3 direction) {
    const float longitude = atan2(direction.z, direction.x);
    const float latitude = asin(clamp(direction.y, -1.0F, 1.0F));
    return float2(longitude / (2.0F * ASTRA_IBL_PI) + 0.5F,
                  0.5F - latitude / ASTRA_IBL_PI);
}

float astra_ibl_radical_inverse(uint bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return float(bits) * 2.3283064365386963e-10F;
}

float2 astra_ibl_hammersley(uint index, uint count) {
    return float2(float(index) / float(count), astra_ibl_radical_inverse(index));
}

void astra_ibl_basis(float3 normal, thread float3 &tangent, thread float3 &bitangent) {
    const float3 helper = abs(normal.z) < 0.999F ? float3(0.0F, 0.0F, 1.0F)
                                                : float3(0.0F, 1.0F, 0.0F);
    tangent = normalize(cross(helper, normal));
    bitangent = cross(normal, tangent);
}

float3 astra_ibl_cosine_sample(float2 xi, float3 normal) {
    const float phi = 2.0F * ASTRA_IBL_PI * xi.x;
    const float radius = sqrt(xi.y);
    const float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(1.0F - xi.y));
    float3 tangent;
    float3 bitangent;
    astra_ibl_basis(normal, tangent, bitangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

float3 astra_ibl_ggx_sample(float2 xi, float roughness, float3 normal) {
    const float alpha = max(roughness * roughness, 0.001F);
    const float alpha_squared = alpha * alpha;
    const float phi = 2.0F * ASTRA_IBL_PI * xi.x;
    const float cos_theta = sqrt((1.0F - xi.y) /
                                 max(1.0F + (alpha_squared - 1.0F) * xi.y, 0.000001F));
    const float sin_theta = sqrt(max(1.0F - cos_theta * cos_theta, 0.0F));
    const float3 local_half = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    float3 tangent;
    float3 bitangent;
    astra_ibl_basis(normal, tangent, bitangent);
    return normalize(tangent * local_half.x + bitangent * local_half.y + normal * local_half.z);
}

float astra_ibl_ggx_distribution(float n_dot_h, float roughness) {
    const float alpha = max(roughness * roughness, 0.001F);
    const float alpha_squared = alpha * alpha;
    const float denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0F) + 1.0F;
    return alpha_squared / max(ASTRA_IBL_PI * denominator * denominator, 1.0e-7F);
}

// Equirectangular RGBA32F -> six RGBA16F cube-array slices.
kernel void astra_ibl_equirect_to_cube(
    constant AstraIblUniforms &uniforms [[buffer(0)]],
    texture2d<float, access::sample> equirectangular [[texture(0)]],
    texture2d_array<half, access::write> environment_cube [[texture(1)]],
    sampler environment_sampler [[sampler(0)]],
    uint3 gid [[thread_position_in_grid]]) {
    if (gid.x >= uniforms.dimensions.x || gid.y >= uniforms.dimensions.y ||
        gid.z >= min(uniforms.dimensions.z, 6U) || gid.z >= environment_cube.get_array_size())
        return;
    const float2 uv = (float2(gid.xy) + 0.5F) / float2(uniforms.dimensions.xy);
    const float3 direction = astra_ibl_cube_direction(gid.z, uv);
    const float2 source_uv = astra_ibl_equirect_uv(direction);
    const float4 radiance = equirectangular.sample(environment_sampler, source_uv);
    environment_cube.write(half4(radiance), gid.xy, gid.z);
}

// Diffuse irradiance is the cosine-weighted hemisphere average (the usual
// irradiance integral divided by PI), stored as six RGBA16F array slices.
kernel void astra_ibl_irradiance(
    constant AstraIblUniforms &uniforms [[buffer(0)]],
    texturecube<float, access::sample> environment_cube [[texture(0)]],
    texture2d_array<half, access::write> irradiance_cube [[texture(1)]],
    sampler environment_sampler [[sampler(0)]],
    uint3 gid [[thread_position_in_grid]]) {
    if (gid.x >= uniforms.dimensions.x || gid.y >= uniforms.dimensions.y ||
        gid.z >= min(uniforms.dimensions.z, 6U) || gid.z >= irradiance_cube.get_array_size())
        return;
    const float2 uv = (float2(gid.xy) + 0.5F) / float2(uniforms.dimensions.xy);
    const float3 normal = astra_ibl_cube_direction(gid.z, uv);
    const uint sample_count = clamp(uniforms.dimensions.w, 1U, ASTRA_IBL_MAX_IRRADIANCE_SAMPLES);
    float3 irradiance = float3(0.0F);
    for (uint i = 0U; i < sample_count; ++i) {
        const float3 direction = astra_ibl_cosine_sample(astra_ibl_hammersley(i, sample_count), normal);
        irradiance += environment_cube.sample(environment_sampler, direction).rgb;
    }
    irradiance /= float(sample_count);
    irradiance_cube.write(half4(half3(irradiance), 1.0H), gid.xy, gid.z);
}

// GGX importance-sampled specular prefilter. Each dispatch writes one mip/view
// at parameters.x roughness; dimensions.z selects the six destination faces.
kernel void astra_ibl_prefilter(
    constant AstraIblUniforms &uniforms [[buffer(0)]],
    texturecube<float, access::sample> environment_cube [[texture(0)]],
    texture2d_array<half, access::write> prefiltered_cube [[texture(1)]],
    sampler environment_sampler [[sampler(0)]],
    uint3 gid [[thread_position_in_grid]]) {
    if (gid.x >= uniforms.dimensions.x || gid.y >= uniforms.dimensions.y ||
        gid.z >= min(uniforms.dimensions.z, 6U) || gid.z >= prefiltered_cube.get_array_size())
        return;
    const float2 uv = (float2(gid.xy) + 0.5F) / float2(uniforms.dimensions.xy);
    const float3 normal = astra_ibl_cube_direction(gid.z, uv);
    const float roughness = clamp(uniforms.parameters.x, 0.0F, 1.0F);
    if (roughness <= 0.001F) {
        prefiltered_cube.write(half4(half3(environment_cube.sample(environment_sampler, normal).rgb),
                                    1.0H), gid.xy, gid.z);
        return;
    }
    const uint sample_count = clamp(uniforms.dimensions.w, 1U, ASTRA_IBL_MAX_GGX_SAMPLES);
    const float source_width = max(uniforms.parameters.y, 1.0F);
    const float source_height = max(uniforms.parameters.z, 1.0F);
    const float texel_solid_angle = 4.0F * ASTRA_IBL_PI / (6.0F * source_width * source_height);
    float3 color = float3(0.0F);
    float total_weight = 0.0F;
    for (uint i = 0U; i < sample_count; ++i) {
        const float3 half_vector = astra_ibl_ggx_sample(astra_ibl_hammersley(i, sample_count),
                                                       roughness, normal);
        const float3 light = normalize(2.0F * dot(normal, half_vector) * half_vector - normal);
        const float n_dot_l = max(dot(normal, light), 0.0F);
        if (n_dot_l > 0.0F) {
            const float n_dot_h = max(dot(normal, half_vector), 0.0F);
            const float h_dot_v = max(dot(half_vector, normal), 0.0001F);
            const float pdf = max(astra_ibl_ggx_distribution(n_dot_h, roughness) * n_dot_h /
                                      (4.0F * h_dot_v),
                                  0.0001F);
            const float sample_solid_angle = 1.0F / (float(sample_count) * pdf);
            const float mip_level = max(0.0F, 0.5F * log2(sample_solid_angle / texel_solid_angle));
            color += environment_cube.sample(environment_sampler, light, level(mip_level)).rgb * n_dot_l;
            total_weight += n_dot_l;
        }
    }
    color /= max(total_weight, 0.0001F);
    prefiltered_cube.write(half4(half3(color), 1.0H), gid.xy, gid.z);
}

float2 astra_ibl_integrate_brdf(float n_dot_v, float roughness, uint sample_count) {
    const float3 view = float3(sqrt(max(1.0F - n_dot_v * n_dot_v, 0.0F)), 0.0F, n_dot_v);
    float2 result = float2(0.0F);
    for (uint i = 0U; i < sample_count; ++i) {
        const float3 half_vector = astra_ibl_ggx_sample(astra_ibl_hammersley(i, sample_count),
                                                       roughness, float3(0.0F, 0.0F, 1.0F));
        const float3 light = normalize(2.0F * dot(view, half_vector) * half_vector - view);
        const float n_dot_l = max(light.z, 0.0F);
        const float n_dot_h = max(half_vector.z, 0.0F);
        const float v_dot_h = max(dot(view, half_vector), 0.0F);
        if (n_dot_l > 0.0F && n_dot_h > 0.0F) {
            const float k = (roughness * roughness) * 0.5F;
            const float visibility_v = n_dot_v / max(n_dot_v * (1.0F - k) + k, 0.0001F);
            const float visibility_l = n_dot_l / max(n_dot_l * (1.0F - k) + k, 0.0001F);
            const float visibility = visibility_v * visibility_l * v_dot_h /
                                     max(n_dot_h * n_dot_v, 0.0001F);
            const float fresnel = pow(1.0F - v_dot_h, 5.0F);
            result += float2((1.0F - fresnel) * visibility, fresnel * visibility);
        }
    }
    return result / float(sample_count);
}

// Split-sum BRDF integration LUT: x = N dot V, y = perceptual roughness.
kernel void astra_ibl_brdf_lut(
    constant AstraIblUniforms &uniforms [[buffer(0)]],
    texture2d<half, access::write> brdf_lut [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= uniforms.dimensions.x || gid.y >= uniforms.dimensions.y)
        return;
    const float2 uv = (float2(gid) + 0.5F) / float2(uniforms.dimensions.xy);
    const float n_dot_v = max(uv.x, 0.001F);
    const float roughness = clamp(uv.y, 0.001F, 1.0F);
    const uint sample_count = clamp(uniforms.dimensions.w, 1U, ASTRA_IBL_MAX_GGX_SAMPLES);
    const float2 integrated = astra_ibl_integrate_brdf(n_dot_v, roughness, sample_count);
    brdf_lut.write(half4(half2(integrated), 0.0H, 1.0H), gid);
}
