#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <astra/metal/metal_device.hpp>
#include <astra/renderer/temporal.hpp>
#include <astra/rhi/render_graph.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace astra::metal {
namespace {

constexpr std::size_t shadow_cascade_count = 3;
// Spend shadow texels where they are visible. The near cascade keeps full
// resolution; the wider mid/far cascades use progressively fewer texels.
constexpr std::array<NSUInteger, shadow_cascade_count> shadow_map_sizes{1024, 768, 384};
constexpr float default_fog_density = 0.008F;

using Vec3 = rhi::Vec3;
using Vec4 = rhi::Vec4;

struct VirtualGeometryIndirectArguments final {
  std::uint32_t index_count{};
  std::uint32_t instance_count{};
  std::uint32_t index_start{};
  std::int32_t base_vertex{};
  std::uint32_t base_instance{};
};

struct VirtualGeometryCullUniforms final {
  float model[16];
  float camera_viewport[4];
  float lod_parameters[4];
  float camera_forward[4];
  float frustum_planes[6][4];
};

// CPU-facing MeshVertex stays ergonomic and portable. Metal gets a compact
// 52-byte stream: positions remain full precision, while unit vectors, colors,
// UVs, tangents and normalized skin weights use GPU-native packed formats.
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

// Static geometry never consumes skin joints or weights. Omitting those 16
// bytes per vertex substantially reduces the scanned vegetation working set.
struct PackedStaticMeshVertex final {
  float position[3];
  std::int16_t normal[4];
  std::uint8_t color[4];
  std::uint16_t texcoord0[2];
  std::uint32_t tangent;
  std::uint16_t texcoord1[2];
};

struct PackedMorphDelta final {
  float position[4];
  float normal[4];
  float tangent[4];
};

struct PackedPointLight final {
  float position_range[4];
  float intensity[4];
};

static_assert(sizeof(rhi::MeshVertex) == 96);
static_assert(sizeof(PackedMeshVertex) == 52);
static_assert(offsetof(PackedMeshVertex, position) == 0);
static_assert(offsetof(PackedMeshVertex, normal) == 12);
static_assert(offsetof(PackedMeshVertex, color) == 20);
static_assert(offsetof(PackedMeshVertex, texcoord0) == 24);
static_assert(offsetof(PackedMeshVertex, tangent) == 28);
static_assert(offsetof(PackedMeshVertex, joints) == 32);
static_assert(offsetof(PackedMeshVertex, weights) == 40);
static_assert(offsetof(PackedMeshVertex, texcoord1) == 48);
static_assert(sizeof(PackedStaticMeshVertex) == 36);
static_assert(offsetof(PackedStaticMeshVertex, position) == 0);
static_assert(offsetof(PackedStaticMeshVertex, normal) == 12);
static_assert(offsetof(PackedStaticMeshVertex, color) == 20);
static_assert(offsetof(PackedStaticMeshVertex, texcoord0) == 24);
static_assert(offsetof(PackedStaticMeshVertex, tangent) == 28);
static_assert(offsetof(PackedStaticMeshVertex, texcoord1) == 32);
static_assert(sizeof(PackedMorphDelta) == 48);
static_assert(sizeof(PackedPointLight) == 32);
static_assert(offsetof(PackedPointLight, intensity) == 16);

std::int16_t pack_snorm16(float value) {
  return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

std::uint16_t pack_unorm16(float value) {
  return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F));
}

std::uint32_t pack_tangent(const rhi::MeshVertex& vertex) {
  const auto pack_component = [](float value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(
               std::lround(std::clamp(value, -1.0F, 1.0F) * 511.0F))) & 0x3ffU;
  };
  // Two W bits encode tangent handedness (-1/+1) or the vegetation-wind marker (0).
  const std::uint32_t handedness = vertex.tangent_w > 1.5F ? 0U
                                   : vertex.tangent_w < 0.0F ? 2U
                                                            : 1U;
  return pack_component(vertex.tangent_x) | (pack_component(vertex.tangent_y) << 10U) |
         (pack_component(vertex.tangent_z) << 20U) | (handedness << 30U);
}

std::uint8_t pack_unorm8(float value) {
  return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::uint16_t pack_half(float value) {
  const __fp16 half = static_cast<__fp16>(value);
  std::uint16_t bits{};
  std::memcpy(&bits, &half, sizeof(bits));
  return bits;
}

void pack_mesh_vertices(std::span<const rhi::MeshVertex> source, PackedMeshVertex* destination) {
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto& vertex = source[index];
    PackedMeshVertex result{};
    result.position[0] = vertex.position.x;
    result.position[1] = vertex.position.y;
    result.position[2] = vertex.position.z;
    result.normal[0] = pack_snorm16(vertex.normal.x);
    result.normal[1] = pack_snorm16(vertex.normal.y);
    result.normal[2] = pack_snorm16(vertex.normal.z);
    result.normal[3] = 0;
    result.color[0] = pack_unorm8(vertex.color.red);
    result.color[1] = pack_unorm8(vertex.color.green);
    result.color[2] = pack_unorm8(vertex.color.blue);
    result.color[3] = pack_unorm8(vertex.color.alpha);
    result.texcoord0[0] = pack_half(vertex.texcoord_u);
    result.texcoord0[1] = pack_half(vertex.texcoord_v);
    result.tangent = pack_tangent(vertex);
    std::copy(std::begin(vertex.joints), std::end(vertex.joints), std::begin(result.joints));
    for (std::size_t i = 0; i < 4; ++i)
      result.weights[i] = pack_unorm16(vertex.weights[i]);
    result.texcoord1[0] = pack_half(vertex.texcoord1_u);
    result.texcoord1[1] = pack_half(vertex.texcoord1_v);
    destination[index] = result;
  }
}

void pack_static_mesh_vertices(std::span<const rhi::MeshVertex> source,
                               PackedStaticMeshVertex* destination) {
  for (std::size_t index = 0; index < source.size(); ++index) {
    const auto& vertex = source[index];
    PackedStaticMeshVertex result{};
    result.position[0] = vertex.position.x;
    result.position[1] = vertex.position.y;
    result.position[2] = vertex.position.z;
    result.normal[0] = pack_snorm16(vertex.normal.x);
    result.normal[1] = pack_snorm16(vertex.normal.y);
    result.normal[2] = pack_snorm16(vertex.normal.z);
    result.color[0] = pack_unorm8(vertex.color.red);
    result.color[1] = pack_unorm8(vertex.color.green);
    result.color[2] = pack_unorm8(vertex.color.blue);
    result.color[3] = pack_unorm8(vertex.color.alpha);
    result.texcoord0[0] = pack_half(vertex.texcoord_u);
    result.texcoord0[1] = pack_half(vertex.texcoord_v);
    result.tangent = pack_tangent(vertex);
    result.texcoord1[0] = pack_half(vertex.texcoord1_u);
    result.texcoord1[1] = pack_half(vertex.texcoord1_v);
    destination[index] = result;
  }
}

static_assert(sizeof(rhi::VirtualGeometryCluster) == 48);
static_assert(sizeof(VirtualGeometryIndirectArguments) == 20);
static_assert(offsetof(VirtualGeometryCullUniforms, camera_viewport) == 64);
static_assert(offsetof(VirtualGeometryCullUniforms, lod_parameters) == 80);
static_assert(offsetof(VirtualGeometryCullUniforms, camera_forward) == 96);
static_assert(offsetof(VirtualGeometryCullUniforms, frustum_planes) == 112);
static_assert(sizeof(VirtualGeometryCullUniforms) == 208);

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 value, float amount) {
  return {value.x * amount, value.y * amount, value.z * amount};
}
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(Vec3 value) {
  const float magnitude = std::sqrt(dot(value, value));
  return magnitude > 0.000001F ? scale(value, 1.0F / magnitude) : Vec3{0.0F, -1.0F, 0.0F};
}

Vec4 transform(const rhi::Matrix4& matrix, Vec4 value) {
  const auto& m = matrix.values;
  return {m[0] * value.x + m[4] * value.y + m[8] * value.z + m[12] * value.w,
          m[1] * value.x + m[5] * value.y + m[9] * value.z + m[13] * value.w,
          m[2] * value.x + m[6] * value.y + m[10] * value.z + m[14] * value.w,
          m[3] * value.x + m[7] * value.y + m[11] * value.z + m[15] * value.w};
}

rhi::Matrix4 multiply(const rhi::Matrix4& a, const rhi::Matrix4& b) {
  rhi::Matrix4 result{};
  for (std::size_t column = 0; column < 4; ++column)
    for (std::size_t row = 0; row < 4; ++row)
      for (std::size_t k = 0; k < 4; ++k)
        result.values[column * 4 + row] += a.values[k * 4 + row] * b.values[column * 4 + k];
  return result;
}

float linear_operator_norm_bound(const rhi::Matrix4& matrix) {
  const Vec3 a0{matrix.values[0], matrix.values[1], matrix.values[2]};
  const Vec3 a1{matrix.values[4], matrix.values[5], matrix.values[6]};
  const Vec3 a2{matrix.values[8], matrix.values[9], matrix.values[10]};
  const float g00 = dot(a0, a0);
  const float g01 = dot(a0, a1);
  const float g02 = dot(a0, a2);
  const float g11 = dot(a1, a1);
  const float g12 = dot(a1, a2);
  const float g22 = dot(a2, a2);
  return std::sqrt(std::max({std::abs(g00) + std::abs(g01) + std::abs(g02),
                             std::abs(g01) + std::abs(g11) + std::abs(g12),
                             std::abs(g02) + std::abs(g12) + std::abs(g22)}));
}

struct BoundsSphere final {
  Vec3 center{};
  float radius{};
  float origin_radius{};
};

struct SkinBufferCacheEntry final {
  const rhi::Matrix4* data{};
  std::size_t count{};
  __strong id<MTLBuffer> buffer{};
};

struct MorphWeightBufferCacheEntry final {
  const float* data{};
  std::size_t count{};
  __strong id<MTLBuffer> buffer{};
};

bool sphere_intersects_frustum(const rhi::Matrix4& view_projection, Vec3 center, float radius) {
  const auto& m = view_projection.values;
  const std::array<std::array<float, 4>, 4> rows{{
      {{m[0], m[4], m[8], m[12]}},
      {{m[1], m[5], m[9], m[13]}},
      {{m[2], m[6], m[10], m[14]}},
      {{m[3], m[7], m[11], m[15]}},
  }};
  const std::array<std::array<float, 4>, 6> planes{{
      {{rows[3][0] + rows[0][0], rows[3][1] + rows[0][1], rows[3][2] + rows[0][2],
        rows[3][3] + rows[0][3]}},
      {{rows[3][0] - rows[0][0], rows[3][1] - rows[0][1], rows[3][2] - rows[0][2],
        rows[3][3] - rows[0][3]}},
      {{rows[3][0] + rows[1][0], rows[3][1] + rows[1][1], rows[3][2] + rows[1][2],
        rows[3][3] + rows[1][3]}},
      {{rows[3][0] - rows[1][0], rows[3][1] - rows[1][1], rows[3][2] - rows[1][2],
        rows[3][3] - rows[1][3]}},
      rows[2],
      {{rows[3][0] - rows[2][0], rows[3][1] - rows[2][1], rows[3][2] - rows[2][2],
        rows[3][3] - rows[2][3]}},
  }};
  for (const auto& plane : planes) {
    const float distance = plane[0] * center.x + plane[1] * center.y +
                           plane[2] * center.z + plane[3];
    const float normal_length =
        std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (distance < -radius * normal_length)
      return false;
  }
  return true;
}

BoundsSphere mesh_bounds(std::span<const rhi::MeshVertex> vertices) {
  Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
  Vec3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
               std::numeric_limits<float>::lowest()};
  float origin_radius = 0.0F;
  for (const auto& vertex : vertices) {
    const auto position = vertex.position;
    minimum = {std::min(minimum.x, position.x), std::min(minimum.y, position.y),
               std::min(minimum.z, position.z)};
    maximum = {std::max(maximum.x, position.x), std::max(maximum.y, position.y),
               std::max(maximum.z, position.z)};
    origin_radius = std::max(origin_radius, std::sqrt(dot(position, position)));
  }
  const Vec3 center = scale(add(minimum, maximum), 0.5F);
  float radius = 0.0F;
  for (const auto& vertex : vertices)
    radius = std::max(radius, std::sqrt(dot(subtract(vertex.position, center),
                                            subtract(vertex.position, center))));
  return {center, radius, origin_radius};
}

BoundsSphere virtual_geometry_bounds(std::span<const rhi::VirtualGeometryCluster> clusters) {
  Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
  Vec3 maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
               std::numeric_limits<float>::lowest()};
  for (const auto& cluster : clusters) {
    const auto center =
        Vec3{cluster.bounds_sphere.x, cluster.bounds_sphere.y, cluster.bounds_sphere.z};
    const float radius = cluster.bounds_sphere.w;
    minimum = {std::min(minimum.x, center.x - radius), std::min(minimum.y, center.y - radius),
               std::min(minimum.z, center.z - radius)};
    maximum = {std::max(maximum.x, center.x + radius), std::max(maximum.y, center.y + radius),
               std::max(maximum.z, center.z + radius)};
  }
  const Vec3 center = scale(add(minimum, maximum), 0.5F);
  float radius = 0.0F;
  float origin_radius = 0.0F;
  for (const auto& cluster : clusters) {
    const Vec3 cluster_center{cluster.bounds_sphere.x, cluster.bounds_sphere.y,
                              cluster.bounds_sphere.z};
    radius = std::max(
        radius, std::sqrt(dot(subtract(cluster_center, center), subtract(cluster_center, center))) +
                    cluster.bounds_sphere.w);
    origin_radius = std::max(origin_radius, std::sqrt(dot(cluster_center, cluster_center)) +
                                                cluster.bounds_sphere.w);
  }
  return {center, radius, origin_radius};
}

VirtualGeometryCullUniforms
make_virtual_geometry_cull_uniforms(const rhi::VirtualGeometryView& view, const rhi::Matrix4& model,
                                    const rhi::Matrix4& frustum_view_projection,
                                    float world_units_per_texel = 0.0F) {
  VirtualGeometryCullUniforms uniforms{};
  std::copy_n(model.values.data(), 16, uniforms.model);
  uniforms.camera_viewport[0] = view.camera_position.x;
  uniforms.camera_viewport[1] = view.camera_position.y;
  uniforms.camera_viewport[2] = view.camera_position.z;
  uniforms.camera_viewport[3] = view.viewport_height_pixels;
  uniforms.lod_parameters[0] = std::tan(view.vertical_fov_radians * 0.5F);
  uniforms.lod_parameters[1] = view.maximum_error_pixels;
  uniforms.lod_parameters[2] = 0.01F;
  // A nonzero value switches the shader to cascade-texel-space error.
  uniforms.lod_parameters[3] = world_units_per_texel;
  // Row 3 of a perspective VP carries clip-w, whose xyz points forward.
  const auto& camera_vp = view.view_projection.values;
  const Vec3 camera_forward = normalize({camera_vp[3], camera_vp[7], camera_vp[11]});
  uniforms.camera_forward[0] = camera_forward.x;
  uniforms.camera_forward[1] = camera_forward.y;
  uniforms.camera_forward[2] = camera_forward.z;
  const auto& m = frustum_view_projection.values;
  const std::array<std::array<float, 4>, 4> rows{{
      {{m[0], m[4], m[8], m[12]}},
      {{m[1], m[5], m[9], m[13]}},
      {{m[2], m[6], m[10], m[14]}},
      {{m[3], m[7], m[11], m[15]}},
  }};
  const std::array<std::array<float, 4>, 6> planes{{
      {{rows[3][0] + rows[0][0], rows[3][1] + rows[0][1], rows[3][2] + rows[0][2],
        rows[3][3] + rows[0][3]}},
      {{rows[3][0] - rows[0][0], rows[3][1] - rows[0][1], rows[3][2] - rows[0][2],
        rows[3][3] - rows[0][3]}},
      {{rows[3][0] + rows[1][0], rows[3][1] + rows[1][1], rows[3][2] + rows[1][2],
        rows[3][3] + rows[1][3]}},
      {{rows[3][0] - rows[1][0], rows[3][1] - rows[1][1], rows[3][2] - rows[1][2],
        rows[3][3] - rows[1][3]}},
      {{rows[2][0], rows[2][1], rows[2][2], rows[2][3]}},
      {{rows[3][0] - rows[2][0], rows[3][1] - rows[2][1], rows[3][2] - rows[2][2],
        rows[3][3] - rows[2][3]}},
  }};
  for (std::size_t plane = 0; plane < planes.size(); ++plane) {
    const auto& p = planes[plane];
    const float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    const float inverse_length = length > 0.000001F ? 1.0F / length : 0.0F;
    for (std::size_t component = 0; component < 4; ++component)
      uniforms.frustum_planes[plane][component] = p[component] * inverse_length;
  }
  return uniforms;
}

rhi::Matrix4 make_light_view(Vec3 center, Vec3 direction) {
  const Vec3 forward = normalize(direction);
  const Vec3 reference_up = std::abs(dot(forward, {0.0F, 1.0F, 0.0F})) > 0.95F
                                ? Vec3{0.0F, 0.0F, 1.0F}
                                : Vec3{0.0F, 1.0F, 0.0F};
  const Vec3 right = normalize(cross(forward, reference_up));
  const Vec3 up = cross(right, forward);
  rhi::Matrix4 view{};
  view.values[0] = right.x;
  view.values[4] = right.y;
  view.values[8] = right.z;
  view.values[1] = up.x;
  view.values[5] = up.y;
  view.values[9] = up.z;
  view.values[2] = -forward.x;
  view.values[6] = -forward.y;
  view.values[10] = -forward.z;
  view.values[12] = -dot(right, center);
  view.values[13] = -dot(up, center);
  view.values[14] = dot(forward, center);
  return view;
}

struct CascadedShadowData final {
  std::array<rhi::Matrix4, shadow_cascade_count> matrices{};
  std::array<float, shadow_cascade_count> splits{};
  std::array<float, shadow_cascade_count> world_units_per_texel{};
  Vec3 camera_forward{};
};

rhi::RenderError make_error(rhi::RenderErrorCode code, NSString* message) {
  const char* description = message.UTF8String;
  return {.code = code,
          .message = description != nullptr ? std::string{description}
                                            : std::string{"Unknown Metal error"}};
}

class MetalMesh final : public rhi::GpuMesh {
public:
  MetalMesh(id<MTLBuffer> vertices, id<MTLBuffer> indices, NSUInteger index_count,
            BoundsSphere bounds, bool skinned, id<MTLBuffer> morph_deltas,
            NSUInteger morph_target_count, NSUInteger vertex_count,
            std::vector<float> morph_delta_radii)
      : vertices_(vertices), indices_(indices), index_count_(index_count), bounds_(bounds),
        skinned_(skinned), morph_deltas_(morph_deltas),
        morph_target_count_(morph_target_count), vertex_count_(vertex_count),
        morph_delta_radii_(std::move(morph_delta_radii)) {}
  id<MTLBuffer> vertices() const noexcept { return vertices_; }
  id<MTLBuffer> indices() const noexcept { return indices_; }
  NSUInteger index_count() const noexcept { return index_count_; }
  const BoundsSphere& bounds() const noexcept { return bounds_; }
  BoundsSphere bounds(std::span<const float> weights) const noexcept {
    BoundsSphere result = bounds_;
    const float expansion = rhi::morph_displacement_bound(morph_delta_radii_, weights);
    result.radius += expansion;
    result.origin_radius += expansion;
    return result;
  }
  BoundsSphere bounds_for_instances(std::span<const float> weights) const noexcept {
    if (weights.empty() || morph_target_count_ == 0U)
      return bounds_;
    const auto target_count = static_cast<std::size_t>(morph_target_count_);
    if (weights.size() % target_count != 0U) {
      BoundsSphere result = bounds_;
      result.radius = result.origin_radius = std::numeric_limits<float>::infinity();
      return result;
    }
    float maximum_expansion = 0.0F;
    for (std::size_t offset = 0; offset < weights.size(); offset += target_count)
      maximum_expansion = std::max(maximum_expansion,
          rhi::morph_displacement_bound(morph_delta_radii_, weights.subspan(offset, target_count)));
    BoundsSphere result = bounds_;
    result.radius += maximum_expansion;
    result.origin_radius += maximum_expansion;
    return result;
  }
  bool skinned() const noexcept { return skinned_; }
  id<MTLBuffer> morph_deltas() const noexcept { return morph_deltas_; }
  NSUInteger morph_target_count() const noexcept { return morph_target_count_; }
  NSUInteger vertex_count() const noexcept { return vertex_count_; }

private:
  __strong id<MTLBuffer> vertices_;
  __strong id<MTLBuffer> indices_;
  NSUInteger index_count_{};
  BoundsSphere bounds_{};
  bool skinned_{};
  __strong id<MTLBuffer> morph_deltas_;
  NSUInteger morph_target_count_{};
  NSUInteger vertex_count_{};
  std::vector<float> morph_delta_radii_;
};

class MetalVirtualGeometry final : public rhi::GpuVirtualGeometry {
public:
  MetalVirtualGeometry(id<MTLBuffer> vertices, id<MTLBuffer> indices, id<MTLBuffer> clusters,
                       id<MTLBuffer> indirect_arguments, id<MTLIndirectCommandBuffer> icb,
                       id<MTLBuffer> icb_arguments, NSUInteger index_count,
                       NSUInteger cluster_count, BoundsSphere bounds)
      : vertices_(vertices), indices_(indices), clusters_(clusters),
        indirect_arguments_(indirect_arguments), icb_(icb), icb_arguments_(icb_arguments),
        index_count_(index_count),
        cluster_count_(cluster_count), bounds_(bounds) {}
  id<MTLBuffer> vertices() const noexcept { return vertices_; }
  id<MTLBuffer> indices() const noexcept { return indices_; }
  id<MTLBuffer> clusters() const noexcept { return clusters_; }
  id<MTLBuffer> indirect_arguments() const noexcept { return indirect_arguments_; }
  id<MTLIndirectCommandBuffer> icb() const noexcept { return icb_; }
  id<MTLBuffer> icb_arguments() const noexcept { return icb_arguments_; }
  NSUInteger index_count() const noexcept { return index_count_; }
  NSUInteger cluster_count() const noexcept { return cluster_count_; }
  const BoundsSphere& bounds() const noexcept { return bounds_; }

private:
  __strong id<MTLBuffer> vertices_;
  __strong id<MTLBuffer> indices_;
  __strong id<MTLBuffer> clusters_;
  __strong id<MTLBuffer> indirect_arguments_;
  __strong id<MTLIndirectCommandBuffer> icb_;
  __strong id<MTLBuffer> icb_arguments_;
  NSUInteger index_count_{};
  NSUInteger cluster_count_{};
  BoundsSphere bounds_{};
};

CascadedShadowData build_shadow_cascades(const rhi::SceneSettings& settings,
                                         std::span<const rhi::DrawItem> draw_items) {
  constexpr float split_lambda = 0.65F;
  CascadedShadowData result{};
  std::array<float, shadow_cascade_count + 1> distances{};
  std::array<std::array<Vec3, 4>, 2> frustum_planes{};
  for (std::size_t plane = 0; plane < 2; ++plane) {
    const float clip_z = plane == 0 ? 0.0F : 1.0F;
    std::size_t corner = 0;
    for (const float y : {-1.0F, 1.0F}) {
      for (const float x : {-1.0F, 1.0F}) {
        const Vec4 world = transform(settings.inverse_view_projection, {x, y, clip_z, 1.0F});
        const float inverse_w = std::abs(world.w) > 0.000001F ? 1.0F / world.w : 0.0F;
        frustum_planes[plane][corner++] = {world.x * inverse_w, world.y * inverse_w,
                                           world.z * inverse_w};
      }
    }
  }
  Vec3 near_center{};
  Vec3 far_center{};
  for (std::size_t corner = 0; corner < 4; ++corner) {
    near_center = add(near_center, frustum_planes[0][corner]);
    far_center = add(far_center, frustum_planes[1][corner]);
  }
  result.camera_forward = normalize(subtract(far_center, near_center));
  float near_distance = 0.0F;
  float far_distance = 0.0F;
  for (std::size_t corner = 0; corner < 4; ++corner) {
    near_distance +=
        dot(subtract(frustum_planes[0][corner], settings.camera_position), result.camera_forward) *
        0.25F;
    far_distance +=
        dot(subtract(frustum_planes[1][corner], settings.camera_position), result.camera_forward) *
        0.25F;
  }
  near_distance = std::max(near_distance, 0.01F);
  if (!std::isfinite(far_distance) || far_distance <= near_distance + 1.0F)
    far_distance = near_distance + 420.0F;
  distances[0] = near_distance;
  for (std::size_t cascade = 1; cascade <= shadow_cascade_count; ++cascade) {
    const float fraction = static_cast<float>(cascade) / static_cast<float>(shadow_cascade_count);
    const float logarithmic = near_distance * std::pow(far_distance / near_distance, fraction);
    const float uniform = near_distance + (far_distance - near_distance) * fraction;
    distances[cascade] = split_lambda * logarithmic + (1.0F - split_lambda) * uniform;
    result.splits[cascade - 1] = distances[cascade];
  }

  for (std::size_t cascade = 0; cascade < shadow_cascade_count; ++cascade) {
    std::array<Vec3, 8> corners{};
    const float start = (distances[cascade] - near_distance) / (far_distance - near_distance);
    const float end = (distances[cascade + 1] - near_distance) / (far_distance - near_distance);
    Vec3 center{};
    for (std::size_t corner = 0; corner < 4; ++corner) {
      const Vec3 ray = subtract(frustum_planes[1][corner], frustum_planes[0][corner]);
      corners[corner] = add(frustum_planes[0][corner], scale(ray, start));
      corners[corner + 4] = add(frustum_planes[0][corner], scale(ray, end));
      center = add(center, add(corners[corner], corners[corner + 4]));
    }
    center = scale(center, 1.0F / 8.0F);
    float corner_radius = 0.0F;
    for (const auto corner : corners)
      corner_radius = std::max(corner_radius,
                               std::sqrt(dot(subtract(corner, center), subtract(corner, center))));
    // A half-texel snap on both axes can move the center sqrt(2) half-texels
    // radially. Reserve that conservative margin before deriving texel size.
    const float snap_margin_fraction =
        1.41421356237F / static_cast<float>(shadow_map_sizes[cascade]);
    const float minimum_snapped_radius = corner_radius / (1.0F - snap_margin_fraction);
    float radius = std::ceil(minimum_snapped_radius * 16.0F) / 16.0F;
    radius = std::max(radius, 1.0F);

    const auto light_view = make_light_view(center, settings.light_direction);
    const Vec4 light_center = transform(light_view, {center.x, center.y, center.z, 1.0F});
    const float texel_world_size =
        (2.0F * radius) / static_cast<float>(shadow_map_sizes[cascade]);
    result.world_units_per_texel[cascade] = texel_world_size;
    const float snapped_x = std::floor(light_center.x / texel_world_size + 0.5F) * texel_world_size;
    const float snapped_y = std::floor(light_center.y / texel_world_size + 0.5F) * texel_world_size;
    const float offset_x = snapped_x - light_center.x;
    const float offset_y = snapped_y - light_center.y;
    auto stable_view = light_view;
    stable_view.values[12] += offset_x;
    stable_view.values[13] += offset_y;

    float minimum_depth = std::numeric_limits<float>::max();
    float maximum_depth = std::numeric_limits<float>::lowest();
    for (const auto corner : corners) {
      const Vec4 light = transform(stable_view, {corner.x, corner.y, corner.z, 1.0F});
      const float depth = -light.z;
      minimum_depth = std::min(minimum_depth, depth);
      maximum_depth = std::max(maximum_depth, depth);
    }
    // Receiver-slice corners alone do not bound shadow casters: a tree or
    // building well outside the camera slice can still project into it along
    // the directional light. Extend depth only for caster bounds overlapping
    // this cascade's light-space XY footprint, preserving receiver XY
    // stability.
    for (const auto& item : draw_items) {
      if (item.use_material && item.material.alpha_mode == 2U)
        continue;
      BoundsSphere local_bounds{};
      if (item.mesh != nullptr) {
        const auto* mesh = dynamic_cast<const MetalMesh*>(item.mesh);
        if (mesh == nullptr)
          continue;
        local_bounds = item.instance_morph_weights.empty()
            ? mesh->bounds(item.morph_weights)
            : mesh->bounds_for_instances(item.instance_morph_weights);
        if (!item.joint_matrices.empty()) {
          float skinned_radius = 0.0F;
          for (const auto& joint : item.joint_matrices) {
            const Vec3 translation{joint.values[12], joint.values[13], joint.values[14]};
            skinned_radius = std::max(skinned_radius, linear_operator_norm_bound(joint) *
                                                              local_bounds.origin_radius +
                                                          std::sqrt(dot(translation, translation)));
          }
          local_bounds.center = {};
          local_bounds.radius = skinned_radius;
        }
      } else if (item.virtual_geometry != nullptr) {
        const auto* geometry = dynamic_cast<const MetalVirtualGeometry*>(item.virtual_geometry);
        if (geometry == nullptr)
          continue;
        local_bounds = geometry->bounds();
      } else {
        continue;
      }
      const auto include_caster = [&](const rhi::Matrix4& instance_transform) {
        const rhi::Matrix4 model = item.instance_transforms.empty()
                                       ? item.model
                                       : multiply(instance_transform, item.model);
        const Vec4 world_center = transform(
            model, {local_bounds.center.x, local_bounds.center.y, local_bounds.center.z, 1.0F});
        const float world_radius = local_bounds.radius * linear_operator_norm_bound(model);
        const Vec4 light_center = transform(stable_view, world_center);
        if (std::abs(light_center.x) > radius + world_radius ||
            std::abs(light_center.y) > radius + world_radius)
          return;
        const float caster_depth = -light_center.z;
        minimum_depth = std::min(minimum_depth, caster_depth - world_radius);
        maximum_depth = std::max(maximum_depth, caster_depth + world_radius);
      };
      if (item.instance_transforms.empty()) {
        include_caster(item.model);
      } else {
        for (const auto& instance : item.instance_transforms)
          include_caster(instance);
      }
    }
    stable_view.values[14] += minimum_depth - 1.0F;
    maximum_depth = maximum_depth - minimum_depth + 1.0F;
    minimum_depth = 1.0F;
    const float depth_padding = std::max(50.0F, radius * 0.25F);
    const float near_plane = std::max(0.01F, minimum_depth - depth_padding);
    const float far_plane = std::max(near_plane + 1.0F, maximum_depth + depth_padding);
    rhi::Matrix4 projection{};
    projection.values[0] = 1.0F / radius;
    projection.values[5] = 1.0F / radius;
    projection.values[10] = 1.0F / (near_plane - far_plane);
    projection.values[14] = near_plane / (near_plane - far_plane);
    rhi::Matrix4 combined{};
    for (std::size_t column = 0; column < 4; ++column)
      for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t k = 0; k < 4; ++k)
          combined.values[column * 4 + row] +=
              projection.values[k * 4 + row] * stable_view.values[column * 4 + k];
    result.matrices[cascade] = combined;
  }
  return result;
}

class MetalTexture final : public rhi::GpuTexture {
public:
  explicit MetalTexture(id<MTLTexture> texture) : texture_(texture) {}
  id<MTLTexture> texture() const noexcept { return texture_; }

private:
  __strong id<MTLTexture> texture_;
};

class MetalEnvironment final : public rhi::GpuEnvironment {
public:
  MetalEnvironment(id<MTLTexture> environment, id<MTLTexture> irradiance,
                   id<MTLTexture> prefiltered,
                   id<MTLTexture> brdf_lut, std::uint32_t prefilter_mip_count)
      : environment_(environment), irradiance_(irradiance), prefiltered_(prefiltered), brdf_lut_(brdf_lut),
        prefilter_mip_count_(prefilter_mip_count) {}
  id<MTLTexture> environment() const noexcept { return environment_; }
  id<MTLTexture> irradiance() const noexcept { return irradiance_; }
  id<MTLTexture> prefiltered() const noexcept { return prefiltered_; }
  id<MTLTexture> brdf_lut() const noexcept { return brdf_lut_; }
  std::uint32_t prefilter_mip_count() const noexcept { return prefilter_mip_count_; }

private:
  __strong id<MTLTexture> environment_;
  __strong id<MTLTexture> irradiance_;
  __strong id<MTLTexture> prefiltered_;
  __strong id<MTLTexture> brdf_lut_;
  std::uint32_t prefilter_mip_count_{};
};

struct SceneUniforms final {
  float view_projection[16];
  float model[16];
  float light_view_projection[shadow_cascade_count][16];
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
  float temporal_params[4]; // previous transforms valid, previous scene time, reserved, reserved
  float uv_transform[5][4]; // offset.xy, scale.xy
  float uv_rotation[5][4];  // cos(rotation), sin(rotation), reserved, reserved
  float environment_flags[4]; // enabled, maximum prefilter mip, reserved, reserved
  std::uint32_t morph_flags[4]; // target count, per-instance weights, vertex count, point-light count
};

struct alignas(16) IblDispatchUniforms final {
  std::uint32_t dimensions[4]; // width, height, face, sample count
  float parameters[4];         // roughness, source width, source height, reserved
};

struct SkyUniforms final {
  float inverse_view_projection[16];
  float previous_view_projection[16];
  float camera_position[4];
  float previous_camera_position[4];
  float light_direction[4];
  float time_exposure[4]; // time, exposure, history valid, reserved
  float jitter[4];        // current NDC jitter xy, orthographic flag, reserved
  float clouds[4];        // base altitude, top altitude, density, coverage
  float cloud_wind_steps[4]; // wind xyz, ray step count
  float environment_flags[4]; // enabled, reserved, reserved, reserved
};

struct TaaUniforms final {
  float inverse_view_projection[16];
  float camera_position[4];
  float previous_camera_position[4];
  float jitter[4];
  float taa_params[4]{0.92F, 0.0F, 0.0F, 0.0F};
};

struct GtaoUniforms final {
  float inverse_view_projection[16];
  float camera_position[4];
  float parameters[4]; // radius in world units, strength, bias, reserved
  float jitter[4]; // current NDC projection jitter xy
};

static_assert(sizeof(rhi::MeshVertex) == 96);
static_assert(offsetof(rhi::MeshVertex, texcoord1_u) == 48);
static_assert(offsetof(rhi::MeshVertex, tangent_x) == 56);
static_assert(offsetof(rhi::MeshVertex, joints) == 72);
static_assert(offsetof(rhi::MeshVertex, weights) == 80);
static_assert(sizeof(SceneUniforms) == 896);
static_assert(offsetof(SceneUniforms, texcoord_sets0) == 480);
static_assert(offsetof(SceneUniforms, texcoord_sets1) == 496);
static_assert(offsetof(SceneUniforms, alpha_flags) == 512);
static_assert(offsetof(SceneUniforms, cascade_splits) == 528);
static_assert(offsetof(SceneUniforms, cascade_camera_forward) == 544);
static_assert(offsetof(SceneUniforms, previous_view_projection) == 560);
static_assert(offsetof(SceneUniforms, previous_model) == 624);
static_assert(offsetof(SceneUniforms, temporal_params) == 688);
static_assert(offsetof(SceneUniforms, uv_transform) == 704);
static_assert(offsetof(SceneUniforms, uv_rotation) == 784);
static_assert(offsetof(SceneUniforms, environment_flags) == 864);
static_assert(offsetof(SceneUniforms, morph_flags) == 880);
static_assert(sizeof(IblDispatchUniforms) == 32);
static_assert(sizeof(SkyUniforms) == 256);
static_assert(offsetof(TaaUniforms, camera_position) == 64);
static_assert(offsetof(TaaUniforms, previous_camera_position) == 80);
static_assert(offsetof(TaaUniforms, jitter) == 96);
static_assert(offsetof(TaaUniforms, taa_params) == 112);
static_assert(sizeof(TaaUniforms) == 128);
static_assert(sizeof(GtaoUniforms) == 112);

MTLVertexDescriptor* make_vertex_descriptor() {
  MTLVertexDescriptor* descriptor = [[MTLVertexDescriptor alloc] init];
  descriptor.attributes[0].format = MTLVertexFormatFloat3;
  descriptor.attributes[0].offset = offsetof(PackedMeshVertex, position);
  descriptor.attributes[0].bufferIndex = 0;
  descriptor.attributes[1].format = MTLVertexFormatShort4Normalized;
  descriptor.attributes[1].offset = offsetof(PackedMeshVertex, normal);
  descriptor.attributes[1].bufferIndex = 0;
  descriptor.attributes[2].format = MTLVertexFormatUChar4Normalized;
  descriptor.attributes[2].offset = offsetof(PackedMeshVertex, color);
  descriptor.attributes[2].bufferIndex = 0;
  descriptor.attributes[3].format = MTLVertexFormatHalf2;
  descriptor.attributes[3].offset = offsetof(PackedMeshVertex, texcoord0);
  descriptor.attributes[3].bufferIndex = 0;
  descriptor.attributes[4].format = MTLVertexFormatInt1010102Normalized;
  descriptor.attributes[4].offset = offsetof(PackedMeshVertex, tangent);
  descriptor.attributes[4].bufferIndex = 0;
  descriptor.attributes[5].format = MTLVertexFormatUShort4;
  descriptor.attributes[5].offset = offsetof(PackedMeshVertex, joints);
  descriptor.attributes[5].bufferIndex = 0;
  descriptor.attributes[6].format = MTLVertexFormatUShort4Normalized;
  descriptor.attributes[6].offset = offsetof(PackedMeshVertex, weights);
  descriptor.attributes[6].bufferIndex = 0;
  descriptor.attributes[7].format = MTLVertexFormatHalf2;
  descriptor.attributes[7].offset = offsetof(PackedMeshVertex, texcoord1);
  descriptor.attributes[7].bufferIndex = 0;
  descriptor.layouts[0].stride = sizeof(PackedMeshVertex);
  descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
  return descriptor;
}

MTLVertexDescriptor* make_static_vertex_descriptor() {
  MTLVertexDescriptor* descriptor = [[MTLVertexDescriptor alloc] init];
  descriptor.attributes[0].format = MTLVertexFormatFloat3;
  descriptor.attributes[0].offset = offsetof(PackedStaticMeshVertex, position);
  descriptor.attributes[0].bufferIndex = 0;
  descriptor.attributes[1].format = MTLVertexFormatShort4Normalized;
  descriptor.attributes[1].offset = offsetof(PackedStaticMeshVertex, normal);
  descriptor.attributes[1].bufferIndex = 0;
  descriptor.attributes[2].format = MTLVertexFormatUChar4Normalized;
  descriptor.attributes[2].offset = offsetof(PackedStaticMeshVertex, color);
  descriptor.attributes[2].bufferIndex = 0;
  descriptor.attributes[3].format = MTLVertexFormatHalf2;
  descriptor.attributes[3].offset = offsetof(PackedStaticMeshVertex, texcoord0);
  descriptor.attributes[3].bufferIndex = 0;
  descriptor.attributes[4].format = MTLVertexFormatInt1010102Normalized;
  descriptor.attributes[4].offset = offsetof(PackedStaticMeshVertex, tangent);
  descriptor.attributes[4].bufferIndex = 0;
  // The shared shader interface still declares skin attributes. Static draws
  // have a zero joint count, so skin_matrix_with_count returns identity before
  // reading these aliases of the already-resident position bytes.
  descriptor.attributes[5].format = MTLVertexFormatUShort4;
  descriptor.attributes[5].offset = 0;
  descriptor.attributes[5].bufferIndex = 0;
  descriptor.attributes[6].format = MTLVertexFormatUShort4Normalized;
  descriptor.attributes[6].offset = 0;
  descriptor.attributes[6].bufferIndex = 0;
  descriptor.attributes[7].format = MTLVertexFormatHalf2;
  descriptor.attributes[7].offset = offsetof(PackedStaticMeshVertex, texcoord1);
  descriptor.attributes[7].bufferIndex = 0;
  descriptor.layouts[0].stride = sizeof(PackedStaticMeshVertex);
  descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
  return descriptor;
}

id<MTLTexture> make_depth_texture(id<MTLDevice> device, NSUInteger width, NSUInteger height,
                                  MTLTextureUsage usage,
                                  MTLPixelFormat format = MTLPixelFormatDepth32Float) {
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = usage;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> make_color_texture(id<MTLDevice> device, MTLPixelFormat format, NSUInteger width,
                                  NSUInteger height, MTLTextureUsage usage) {
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = usage;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> make_cube_texture(id<MTLDevice> device, MTLPixelFormat format, NSUInteger size,
                                 NSUInteger mip_levels) {
  MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
  descriptor.textureType = MTLTextureTypeCube;
  descriptor.pixelFormat = format;
  descriptor.width = size;
  descriptor.height = size;
  descriptor.depth = 1;
  descriptor.mipmapLevelCount = mip_levels;
  descriptor.arrayLength = 1;
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> make_cube_array_view(id<MTLTexture> cube, NSUInteger mip_level) {
  if (cube == nil || mip_level >= cube.mipmapLevelCount)
    return nil;
  return [cube newTextureViewWithPixelFormat:cube.pixelFormat
                                 textureType:MTLTextureType2DArray
                                      levels:NSMakeRange(mip_level, 1)
                                      slices:NSMakeRange(0, 6)];
}

class MetalRenderDevice final : public rhi::RenderDevice {
public:
  static std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
  create(CAMetalLayer* layer) {
    if (layer == nil) {
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::invalid_surface,
                           .message = "The window did not provide a CAMetalLayer."});
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::device_unavailable,
                           .message = "Metal could not find a compatible GPU device."});
    }
    layer.device = device;
    // The postprocess shader outputs linear display-referred values; the
    // drawable's sRGB attachment performs the final transfer function.
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    layer.framebufferOnly = YES;
    // Two in-flight drawables are sufficient for a 60 Hz game loop and avoid
    // retaining a third full-size Retina IOSurface on unified-memory machines.
    layer.maximumDrawableCount = 2;

    NSError* error = nil;
    NSURL* library_url =
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:ASTRA_METALLIB_PATH]];
    id<MTLLibrary> library = [device newLibraryWithURL:library_url error:&error];
    if (library == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::shader_compilation_failed, error.localizedDescription));
    }
    id<MTLFunction> scene_vertex = [library newFunctionWithName:@"astra_vertex"];
    id<MTLFunction> scene_fragment = [library newFunctionWithName:@"astra_fragment"];
    id<MTLFunction> shadow_vertex = [library newFunctionWithName:@"astra_shadow_vertex"];
    id<MTLFunction> shadow_fragment = [library newFunctionWithName:@"astra_shadow_fragment"];
    id<MTLFunction> virtual_geometry_cull =
        [library newFunctionWithName:@"astra_virtual_geometry_cull"];
    id<MTLFunction> virtual_geometry_encode_icb =
        [library newFunctionWithName:@"astra_virtual_geometry_encode_icb"];
    id<MTLFunction> fullscreen_vertex = [library newFunctionWithName:@"astra_fullscreen_vertex"];
    id<MTLFunction> sky_fragment = [library newFunctionWithName:@"astra_sky_fragment"];
    id<MTLFunction> tonemap_fragment = [library newFunctionWithName:@"astra_tonemap_fragment"];
    id<MTLFunction> taa_fragment = [library newFunctionWithName:@"astra_taa_fragment"];
    id<MTLFunction> gtao_composite_fragment =
        [library newFunctionWithName:@"astra_gtao_composite_fragment"];
    id<MTLFunction> ibl_equirect = [library newFunctionWithName:@"astra_ibl_equirect_to_cube"];
    id<MTLFunction> ibl_irradiance = [library newFunctionWithName:@"astra_ibl_irradiance"];
    id<MTLFunction> ibl_prefilter = [library newFunctionWithName:@"astra_ibl_prefilter"];
    id<MTLFunction> ibl_brdf = [library newFunctionWithName:@"astra_ibl_brdf_lut"];
    if (scene_vertex == nil || scene_fragment == nil || shadow_vertex == nil ||
        shadow_fragment == nil || virtual_geometry_cull == nil || fullscreen_vertex == nil ||
        sky_fragment == nil || tonemap_fragment == nil || taa_fragment == nil ||
        gtao_composite_fragment == nil ||
        ibl_equirect == nil || ibl_irradiance == nil || ibl_prefilter == nil || ibl_brdf == nil) {
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::shader_compilation_failed,
                           .message = "The Metal library is missing a required scene, shadow, sky, "
                                      "or postprocess entry point."});
    }
    bool virtual_geometry_icb_supported = false;
    id<MTLArgumentEncoder> virtual_geometry_icb_argument_encoder = nil;
    id<MTLComputePipelineState> virtual_geometry_encode_icb_pipeline = nil;
    if (@available(macOS 10.15, *)) {
      if ([device supportsFamily:MTLGPUFamilyApple7] && virtual_geometry_encode_icb != nil) {
        virtual_geometry_icb_argument_encoder =
            [virtual_geometry_encode_icb newArgumentEncoderWithBufferIndex:3];
        virtual_geometry_encode_icb_pipeline =
            [device newComputePipelineStateWithFunction:virtual_geometry_encode_icb error:&error];
        virtual_geometry_icb_supported = virtual_geometry_icb_argument_encoder != nil &&
                                          virtual_geometry_icb_argument_encoder.encodedLength > 0 &&
                                          virtual_geometry_encode_icb_pipeline != nil;
      }
    }
    id<MTLArgumentEncoder> scene_fragment_argument_encoder =
        [scene_fragment newArgumentEncoderWithBufferIndex:2];
    if (scene_fragment_argument_encoder == nil || scene_fragment_argument_encoder.encodedLength == 0 ||
        scene_fragment_argument_encoder.alignment == 0) {
      return std::unexpected(make_error(
          rhi::RenderErrorCode::resource_creation_failed,
          @"Metal could not create the scene fragment resource argument encoder."));
    }

    MTLVertexDescriptor* vertex_descriptor = make_vertex_descriptor();
    MTLRenderPipelineDescriptor* scene_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    scene_descriptor.supportIndirectCommandBuffers = virtual_geometry_icb_supported ? YES : NO;
    scene_descriptor.vertexFunction = scene_vertex;
    scene_descriptor.fragmentFunction = scene_fragment;
    scene_descriptor.vertexDescriptor = vertex_descriptor;
    scene_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    scene_descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
    scene_descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatR8Unorm;
    scene_descriptor.colorAttachments[3].pixelFormat = MTLPixelFormatRGBA16Float;
    scene_descriptor.colorAttachments[4].pixelFormat = MTLPixelFormatR16Float;
    scene_descriptor.colorAttachments[5].pixelFormat = MTLPixelFormatRGBA16Float;
    scene_descriptor.colorAttachments[3].writeMask = MTLColorWriteMaskNone;
    scene_descriptor.colorAttachments[4].writeMask = MTLColorWriteMaskNone;
    scene_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> scene_pipeline =
        [device newRenderPipelineStateWithDescriptor:scene_descriptor error:&error];
    if (scene_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLRenderPipelineDescriptor* blend_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    blend_descriptor.supportIndirectCommandBuffers = virtual_geometry_icb_supported ? YES : NO;
    blend_descriptor.vertexFunction = scene_vertex;
    blend_descriptor.fragmentFunction = scene_fragment;
    blend_descriptor.vertexDescriptor = vertex_descriptor;
    blend_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    blend_descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
    blend_descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatR8Unorm;
    blend_descriptor.colorAttachments[3].pixelFormat = MTLPixelFormatRGBA16Float;
    blend_descriptor.colorAttachments[4].pixelFormat = MTLPixelFormatR16Float;
    blend_descriptor.colorAttachments[5].pixelFormat = MTLPixelFormatRGBA16Float;
    blend_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    // Transparent fragments only write the accumulation/revealage attachments.
    // Opaque color, motion and reactive outputs are preserved unchanged.
    for (NSUInteger attachment = 0; attachment < 3; ++attachment)
      blend_descriptor.colorAttachments[attachment].writeMask = MTLColorWriteMaskNone;
    blend_descriptor.colorAttachments[5].writeMask = MTLColorWriteMaskNone;
    MTLRenderPipelineColorAttachmentDescriptor* accumulation_attachment =
        blend_descriptor.colorAttachments[3];
    accumulation_attachment.blendingEnabled = YES;
    accumulation_attachment.rgbBlendOperation = MTLBlendOperationAdd;
    accumulation_attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
    accumulation_attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
    accumulation_attachment.alphaBlendOperation = MTLBlendOperationAdd;
    accumulation_attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    accumulation_attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
    MTLRenderPipelineColorAttachmentDescriptor* revealage_attachment =
        blend_descriptor.colorAttachments[4];
    revealage_attachment.blendingEnabled = YES;
    revealage_attachment.rgbBlendOperation = MTLBlendOperationAdd;
    revealage_attachment.sourceRGBBlendFactor = MTLBlendFactorZero;
    revealage_attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    revealage_attachment.alphaBlendOperation = MTLBlendOperationAdd;
    revealage_attachment.sourceAlphaBlendFactor = MTLBlendFactorZero;
    revealage_attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> blend_pipeline =
        [device newRenderPipelineStateWithDescriptor:blend_descriptor error:&error];
    if (blend_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLRenderPipelineDescriptor* shadow_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    shadow_descriptor.vertexFunction = shadow_vertex;
    shadow_descriptor.fragmentFunction = shadow_fragment;
    shadow_descriptor.vertexDescriptor = vertex_descriptor;
    shadow_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth16Unorm;
    id<MTLRenderPipelineState> shadow_pipeline =
        [device newRenderPipelineStateWithDescriptor:shadow_descriptor error:&error];
    if (shadow_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLVertexDescriptor* static_vertex_descriptor = make_static_vertex_descriptor();
    MTLRenderPipelineDescriptor* static_scene_descriptor = [scene_descriptor copy];
    static_scene_descriptor.vertexDescriptor = static_vertex_descriptor;
    id<MTLRenderPipelineState> static_scene_pipeline =
        [device newRenderPipelineStateWithDescriptor:static_scene_descriptor error:&error];
    if (static_scene_pipeline == nil)
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));

    MTLRenderPipelineDescriptor* static_blend_descriptor = [blend_descriptor copy];
    static_blend_descriptor.vertexDescriptor = static_vertex_descriptor;
    id<MTLRenderPipelineState> static_blend_pipeline =
        [device newRenderPipelineStateWithDescriptor:static_blend_descriptor error:&error];
    if (static_blend_pipeline == nil)
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));

    MTLRenderPipelineDescriptor* static_shadow_descriptor = [shadow_descriptor copy];
    static_shadow_descriptor.vertexDescriptor = static_vertex_descriptor;
    id<MTLRenderPipelineState> static_shadow_pipeline =
        [device newRenderPipelineStateWithDescriptor:static_shadow_descriptor error:&error];
    if (static_shadow_pipeline == nil)
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));

    id<MTLComputePipelineState> virtual_geometry_cull_pipeline =
        [device newComputePipelineStateWithFunction:virtual_geometry_cull error:&error];
    if (virtual_geometry_cull_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }
    id<MTLComputePipelineState> ibl_equirect_pipeline =
        [device newComputePipelineStateWithFunction:ibl_equirect error:&error];
    id<MTLComputePipelineState> ibl_irradiance_pipeline =
        [device newComputePipelineStateWithFunction:ibl_irradiance error:&error];
    id<MTLComputePipelineState> ibl_prefilter_pipeline =
        [device newComputePipelineStateWithFunction:ibl_prefilter error:&error];
    id<MTLComputePipelineState> ibl_brdf_pipeline =
        [device newComputePipelineStateWithFunction:ibl_brdf error:&error];
    if (ibl_equirect_pipeline == nil || ibl_irradiance_pipeline == nil ||
        ibl_prefilter_pipeline == nil || ibl_brdf_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLRenderPipelineDescriptor* sky_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    sky_descriptor.vertexFunction = fullscreen_vertex;
    sky_descriptor.fragmentFunction = sky_fragment;
    sky_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    sky_descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
    sky_descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatR8Unorm;
    sky_descriptor.colorAttachments[3].pixelFormat = MTLPixelFormatRGBA16Float;
    sky_descriptor.colorAttachments[4].pixelFormat = MTLPixelFormatR16Float;
    sky_descriptor.colorAttachments[3].writeMask = MTLColorWriteMaskNone;
    sky_descriptor.colorAttachments[4].writeMask = MTLColorWriteMaskNone;
    sky_descriptor.colorAttachments[5].pixelFormat = MTLPixelFormatRGBA16Float;
    sky_descriptor.colorAttachments[5].writeMask = MTLColorWriteMaskNone;
    sky_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> sky_pipeline =
        [device newRenderPipelineStateWithDescriptor:sky_descriptor error:&error];
    if (sky_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLRenderPipelineDescriptor* post_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    post_descriptor.vertexFunction = fullscreen_vertex;
    post_descriptor.fragmentFunction = tonemap_fragment;
    post_descriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
    id<MTLRenderPipelineState> post_pipeline =
        [device newRenderPipelineStateWithDescriptor:post_descriptor error:&error];
    if (post_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLRenderPipelineDescriptor* taa_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    taa_descriptor.vertexFunction = fullscreen_vertex;
    taa_descriptor.fragmentFunction = taa_fragment;
    taa_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    taa_descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatR16Float;
    id<MTLRenderPipelineState> taa_pipeline =
        [device newRenderPipelineStateWithDescriptor:taa_descriptor error:&error];
    if (taa_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLRenderPipelineDescriptor* gtao_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    gtao_descriptor.vertexFunction = fullscreen_vertex;
    gtao_descriptor.fragmentFunction = gtao_composite_fragment;
    gtao_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    id<MTLRenderPipelineState> gtao_pipeline =
        [device newRenderPipelineStateWithDescriptor:gtao_descriptor error:&error];
    if (gtao_pipeline == nil) {
      return std::unexpected(
          make_error(rhi::RenderErrorCode::pipeline_creation_failed, error.localizedDescription));
    }

    MTLDepthStencilDescriptor* depth_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    depth_descriptor.depthCompareFunction = MTLCompareFunctionLess;
    depth_descriptor.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_state =
        [device newDepthStencilStateWithDescriptor:depth_descriptor];
    MTLDepthStencilDescriptor* transparent_depth_descriptor =
        [[MTLDepthStencilDescriptor alloc] init];
    transparent_depth_descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
    transparent_depth_descriptor.depthWriteEnabled = NO;
    id<MTLDepthStencilState> transparent_depth_state =
        [device newDepthStencilStateWithDescriptor:transparent_depth_descriptor];
    MTLDepthStencilDescriptor* shadow_depth_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    shadow_depth_descriptor.depthCompareFunction = MTLCompareFunctionLess;
    shadow_depth_descriptor.depthWriteEnabled = YES;
    id<MTLDepthStencilState> shadow_depth_state =
        [device newDepthStencilStateWithDescriptor:shadow_depth_descriptor];

    MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.supportArgumentBuffers = YES;
    sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_descriptor.mipFilter = MTLSamplerMipFilterNearest;
    sampler_descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_descriptor.tAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];

    MTLSamplerDescriptor* taa_sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    taa_sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    taa_sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    taa_sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    taa_sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> taa_sampler = [device newSamplerStateWithDescriptor:taa_sampler_descriptor];

    MTLSamplerDescriptor* shadow_sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    shadow_sampler_descriptor.supportArgumentBuffers = YES;
    shadow_sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    shadow_sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    shadow_sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    shadow_sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    shadow_sampler_descriptor.compareFunction = MTLCompareFunctionLessEqual;
    id<MTLSamplerState> shadow_sampler =
        [device newSamplerStateWithDescriptor:shadow_sampler_descriptor];
    MTLSamplerDescriptor* ibl_sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    ibl_sampler_descriptor.supportArgumentBuffers = YES;
    ibl_sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    ibl_sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    ibl_sampler_descriptor.mipFilter = MTLSamplerMipFilterLinear;
    ibl_sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    ibl_sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> ibl_sampler = [device newSamplerStateWithDescriptor:ibl_sampler_descriptor];
    MTLSamplerDescriptor* ibl_equirect_sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    ibl_equirect_sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    ibl_equirect_sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    ibl_equirect_sampler_descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    ibl_equirect_sampler_descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    ibl_equirect_sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> ibl_equirect_sampler =
        [device newSamplerStateWithDescriptor:ibl_equirect_sampler_descriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    std::array<id<MTLTexture>, shadow_cascade_count> shadow_textures{};
    for (std::size_t cascade = 0; cascade < shadow_cascade_count; ++cascade)
      shadow_textures[cascade] = make_depth_texture(
          device, shadow_map_sizes[cascade], shadow_map_sizes[cascade],
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, MTLPixelFormatDepth16Unorm);
    if (queue == nil || depth_state == nil || transparent_depth_state == nil ||
        shadow_depth_state == nil || sampler == nil || taa_sampler == nil ||
        shadow_sampler == nil || ibl_sampler == nil || ibl_equirect_sampler == nil ||
        std::ranges::any_of(shadow_textures, [](id<MTLTexture> texture) {
          return texture == nil;
        })) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::resource_creation_failed,
          .message =
              "Metal could not create its queue, samplers, depth states, or reusable shadow map."});
    }

    auto result = std::unique_ptr<MetalRenderDevice>{new MetalRenderDevice{
        layer, device, queue, scene_pipeline, blend_pipeline, shadow_pipeline,
        static_scene_pipeline, static_blend_pipeline, static_shadow_pipeline, sky_pipeline,
        post_pipeline, taa_pipeline, gtao_pipeline, scene_fragment_argument_encoder, virtual_geometry_cull_pipeline,
        virtual_geometry_encode_icb_pipeline, virtual_geometry_icb_argument_encoder,
        virtual_geometry_icb_supported,
        ibl_equirect_pipeline, ibl_irradiance_pipeline, ibl_prefilter_pipeline, ibl_brdf_pipeline,
        depth_state, transparent_depth_state, shadow_depth_state, sampler, taa_sampler,
        shadow_sampler, ibl_sampler, ibl_equirect_sampler,
        shadow_textures, device.name}};
    return std::unique_ptr<rhi::RenderDevice>{std::move(result)};
  }

  rhi::Backend backend() const noexcept override { return rhi::Backend::metal; }
  const std::string& device_name() const noexcept override { return device_name_; }
  double last_gpu_frame_time_ms() const noexcept override {
    return static_cast<double>(gpu_time_nanoseconds_->load(std::memory_order_acquire)) * 1.0e-6;
  }

  std::expected<std::unique_ptr<rhi::GpuVirtualGeometry>, rhi::RenderError>
  create_virtual_geometry(const rhi::VirtualGeometryData& data) override {
    const auto valid = rhi::validate_virtual_geometry(data);
    if (!valid)
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::invalid_mesh, .message = valid.error()});
    const std::size_t vertex_bytes = data.vertices.size() * sizeof(PackedStaticMeshVertex);
    id<MTLBuffer> vertices = [device_ newBufferWithLength:vertex_bytes
                                                   options:MTLResourceStorageModeShared];
    if (vertices != nil)
      pack_static_mesh_vertices(data.vertices,
                                static_cast<PackedStaticMeshVertex*>(vertices.contents));
    id<MTLBuffer> indices = [device_ newBufferWithBytes:data.indices.data()
                                                 length:data.indices.size_bytes()
                                                options:MTLResourceStorageModeShared];
    id<MTLBuffer> clusters = [device_ newBufferWithBytes:data.clusters.data()
                                                  length:data.clusters.size_bytes()
                                                 options:MTLResourceStorageModeShared];
    const NSUInteger indirect_bytes =
        static_cast<NSUInteger>(data.clusters.size()) * sizeof(VirtualGeometryIndirectArguments);
    id<MTLBuffer> indirect_arguments = [device_ newBufferWithLength:indirect_bytes
                                                            options:MTLResourceStorageModeShared];
    if (vertices == nil || indices == nil || clusters == nil || indirect_arguments == nil) {
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::resource_creation_failed,
                           .message = "Metal could not allocate virtual geometry vertex, index, "
                                      "cluster, or indirect buffers."});
    }
    auto* arguments = static_cast<VirtualGeometryIndirectArguments*>(indirect_arguments.contents);
    for (std::size_t index = 0; index < data.clusters.size(); ++index) {
      const auto& cluster = data.clusters[index];
      arguments[index] = {cluster.index_count, 0U, cluster.first_index, cluster.base_vertex, 0U};
    }
    id<MTLIndirectCommandBuffer> icb = nil;
    id<MTLBuffer> icb_arguments = nil;
    if (virtual_geometry_icb_supported_) {
      MTLIndirectCommandBufferDescriptor* descriptor =
          [[MTLIndirectCommandBufferDescriptor alloc] init];
      descriptor.commandTypes = MTLIndirectCommandTypeDrawIndexed;
      descriptor.inheritPipelineState = YES;
      descriptor.inheritBuffers = YES;
      icb = [device_ newIndirectCommandBufferWithDescriptor:descriptor
                                             maxCommandCount:data.clusters.size()
                                                    options:MTLResourceStorageModePrivate];
      if (icb != nil) {
        icb_arguments = [device_ newBufferWithLength:virtual_geometry_icb_argument_encoder_.encodedLength
                                             options:MTLResourceStorageModeShared];
        if (icb_arguments != nil) {
          [virtual_geometry_icb_argument_encoder_ setArgumentBuffer:icb_arguments offset:0];
          [virtual_geometry_icb_argument_encoder_ setIndirectCommandBuffer:icb atIndex:0];
        }
      }
      if (icb == nil || icb_arguments == nil) {
        icb = nil;
        icb_arguments = nil;
      }
    }
    return std::unique_ptr<rhi::GpuVirtualGeometry>{new MetalVirtualGeometry{
        vertices, indices, clusters, indirect_arguments, icb, icb_arguments,
        static_cast<NSUInteger>(data.indices.size()), static_cast<NSUInteger>(data.clusters.size()),
        virtual_geometry_bounds(data.clusters)}};
  }

  std::expected<std::unique_ptr<rhi::GpuMesh>, rhi::RenderError>
  create_mesh(const rhi::MeshData& data) override {
    if (data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 != 0) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::invalid_mesh,
          .message = "A mesh needs vertices and a non-empty triangle index list."});
    }
    for (const auto index : data.indices) {
      if (index >= data.vertices.size()) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message = "A mesh index references a vertex outside the vertex array."});
      }
    }
    if (auto valid_morphs = rhi::validate_morph_targets(data.vertices.size(), data.morph_targets);
        !valid_morphs) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::invalid_mesh,
          .message = valid_morphs.error()});
    }
    if (!data.morph_targets.empty() &&
        data.vertices.size() > std::numeric_limits<std::size_t>::max() /
                                   data.morph_targets.size() / sizeof(PackedMorphDelta)) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::invalid_mesh,
          .message = "Morph target data exceeds the addressable GPU buffer size."});
    }
    const bool skinned = std::ranges::any_of(data.vertices, [](const rhi::MeshVertex& vertex) {
      return vertex.weights[0] > 0.0F || vertex.weights[1] > 0.0F ||
             vertex.weights[2] > 0.0F || vertex.weights[3] > 0.0F;
    });
    const std::size_t vertex_stride =
        skinned ? sizeof(PackedMeshVertex) : sizeof(PackedStaticMeshVertex);
    const std::size_t vertex_bytes = data.vertices.size() * vertex_stride;
    id<MTLBuffer> vertices = [device_ newBufferWithLength:vertex_bytes
                                                   options:MTLResourceStorageModeShared];
    if (vertices != nil) {
      if (skinned)
        pack_mesh_vertices(data.vertices, static_cast<PackedMeshVertex*>(vertices.contents));
      else
        pack_static_mesh_vertices(data.vertices,
                                  static_cast<PackedStaticMeshVertex*>(vertices.contents));
    }
    id<MTLBuffer> indices = [device_ newBufferWithBytes:data.indices.data()
                                                 length:data.indices.size_bytes()
                                                options:MTLResourceStorageModeShared];
    id<MTLBuffer> morph_deltas = nil;
    std::vector<float> morph_delta_radii;
    if (!data.morph_targets.empty()) {
      morph_delta_radii.reserve(data.morph_targets.size());
      const std::size_t delta_count = data.vertices.size() * data.morph_targets.size();
      morph_deltas = [device_ newBufferWithLength:delta_count * sizeof(PackedMorphDelta)
                                          options:MTLResourceStorageModeShared];
      if (morph_deltas != nil) {
        auto* packed = static_cast<PackedMorphDelta*>(morph_deltas.contents);
        for (std::size_t target_index = 0; target_index < data.morph_targets.size(); ++target_index) {
          const auto& target = data.morph_targets[target_index];
          float maximum_position_delta = 0.0F;
          for (const auto& delta : target.positions)
            maximum_position_delta = std::max(maximum_position_delta,
                std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
          morph_delta_radii.push_back(maximum_position_delta);
          for (std::size_t vertex_index = 0; vertex_index < data.vertices.size(); ++vertex_index) {
            auto& delta = packed[target_index * data.vertices.size() + vertex_index];
            delta = {};
            const auto write = [](float* output, std::span<const rhi::Vec3> source,
                                  std::size_t index) {
              if (!source.empty()) {
                output[0] = source[index].x;
                output[1] = source[index].y;
                output[2] = source[index].z;
              }
            };
            write(delta.position, target.positions, vertex_index);
            write(delta.normal, target.normals, vertex_index);
            write(delta.tangent, target.tangents, vertex_index);
          }
        }
      }
    }
    if (vertices == nil || indices == nil || (!data.morph_targets.empty() && morph_deltas == nil)) {
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::resource_creation_failed,
                           .message = "Metal could not allocate the mesh vertex, index, or morph buffer."});
    }
    return std::unique_ptr<rhi::GpuMesh>{new MetalMesh{vertices, indices,
                                                       static_cast<NSUInteger>(data.indices.size()),
                                                       mesh_bounds(data.vertices), skinned,
                                                       morph_deltas,
                                                       static_cast<NSUInteger>(data.morph_targets.size()),
                                                       static_cast<NSUInteger>(data.vertices.size()),
                                                       std::move(morph_delta_radii)}};
  }

  std::expected<std::unique_ptr<rhi::GpuTexture>, rhi::RenderError>
  create_texture(const rhi::TextureData& data) override {
    if (data.width == 0 || data.height == 0 ||
        data.width > std::numeric_limits<std::size_t>::max() / 4U / data.height ||
        data.rgba8.size() != static_cast<std::size_t>(data.width) * data.height * 4U) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::resource_creation_failed,
          .message = "Texture dimensions or tightly-packed RGBA8 payload are invalid."});
    }
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:(data.srgb ? MTLPixelFormatRGBA8Unorm_sRGB
                                                      : MTLPixelFormatRGBA8Unorm)
                                     width:data.width
                                    height:data.height
                                 mipmapped:YES];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (texture == nil) {
      return std::unexpected(
          rhi::RenderError{.code = rhi::RenderErrorCode::resource_creation_failed,
                           .message = "Metal could not allocate the texture."});
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, data.width, data.height)
               mipmapLevel:0
                 withBytes:data.rgba8.data()
               bytesPerRow:static_cast<NSUInteger>(data.width) * 4U];
    if (texture.mipmapLevelCount > 1) {
      id<MTLCommandBuffer> command = [queue_ commandBuffer];
      id<MTLBlitCommandEncoder> blit = command != nil ? [command blitCommandEncoder] : nil;
      if (command == nil || blit == nil) {
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                             .message = "Metal could not create a texture mipmap command."});
      }
      [blit generateMipmapsForTexture:texture];
      [blit endEncoding];
      [command commit];
    }
    return std::unique_ptr<rhi::GpuTexture>{new MetalTexture{texture}};
  }

  std::expected<std::unique_ptr<rhi::GpuEnvironment>, rhi::RenderError>
  create_environment(const rhi::EnvironmentData& data) override {
    if (const auto valid = rhi::validate_environment(data); !valid)
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::invalid_scene, .message = valid.error()});

    MTLTextureDescriptor* source_descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                     width:data.width
                                    height:data.height
                                 mipmapped:NO];
    source_descriptor.storageMode = MTLStorageModeShared;
    source_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> source = [device_ newTextureWithDescriptor:source_descriptor];
    const NSUInteger source_bytes_per_row = static_cast<NSUInteger>(data.width) * 4U * sizeof(float);
    if (source == nil)
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::resource_creation_failed,
          .message = "Metal could not allocate the HDR equirectangular source texture."});
    [source replaceRegion:MTLRegionMake2D(0, 0, data.width, data.height)
              mipmapLevel:0
                withBytes:data.rgba32f.data()
              bytesPerRow:source_bytes_per_row];

    const NSUInteger environment_mips = std::bit_width(data.environment_resolution);
    id<MTLTexture> environment_cube = make_cube_texture(
        device_, MTLPixelFormatRGBA16Float, data.environment_resolution, environment_mips);
    id<MTLTexture> irradiance_cube = make_cube_texture(
        device_, MTLPixelFormatRGBA16Float, data.irradiance_resolution, 1);
    id<MTLTexture> prefiltered_cube = make_cube_texture(
        device_, MTLPixelFormatRGBA16Float, data.prefilter_resolution, data.prefilter_mip_count);
    id<MTLTexture> brdf_lut = make_color_texture(
        device_, MTLPixelFormatRG16Float, data.brdf_lut_resolution, data.brdf_lut_resolution,
        MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    id<MTLTexture> environment_write_view = make_cube_array_view(environment_cube, 0);
    id<MTLTexture> irradiance_write_view = make_cube_array_view(irradiance_cube, 0);
    if (environment_cube == nil || irradiance_cube == nil || prefiltered_cube == nil ||
        brdf_lut == nil || environment_write_view == nil || irradiance_write_view == nil) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::resource_creation_failed,
          .message = "Metal could not allocate the IBL environment, irradiance, prefilter, or BRDF resources."});
    }

    id<MTLCommandBuffer> command = [queue_ commandBuffer];
    if (command == nil)
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::command_submission_failed,
          .message = "Metal could not allocate the IBL precomputation command buffer."});

    const auto dispatch_cube = [&](id<MTLComputePipelineState> pipeline,
                                   id<MTLTexture> input, id<MTLTexture> output,
                                   std::uint32_t width, std::uint32_t height,
                                   std::uint32_t samples, float roughness,
                                   float source_width, float source_height,
                                   id<MTLSamplerState> input_sampler) {
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (encoder == nil)
        return false;
      IblDispatchUniforms uniforms{};
      uniforms.dimensions[0] = width;
      uniforms.dimensions[1] = height;
      uniforms.dimensions[2] = 6;
      uniforms.dimensions[3] = samples;
      uniforms.parameters[0] = roughness;
      uniforms.parameters[1] = source_width;
      uniforms.parameters[2] = source_height;
      [encoder setComputePipelineState:pipeline];
      [encoder setBytes:&uniforms length:sizeof(uniforms) atIndex:0];
      [encoder setTexture:input atIndex:0];
      [encoder setTexture:output atIndex:1];
      [encoder setSamplerState:input_sampler atIndex:0];
      const NSUInteger thread_width = pipeline.threadExecutionWidth;
      const NSUInteger thread_height =
          std::max<NSUInteger>(1, pipeline.maxTotalThreadsPerThreadgroup / thread_width);
      [encoder dispatchThreads:MTLSizeMake(width, height, 6)
          threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
      [encoder endEncoding];
      return true;
    };

    if (!dispatch_cube(ibl_equirect_pipeline_, source, environment_write_view,
                       data.environment_resolution, data.environment_resolution, 1, 0.0F,
                       static_cast<float>(data.width), static_cast<float>(data.height),
                       ibl_equirect_sampler_)) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::command_submission_failed,
          .message = "Metal could not encode equirectangular-to-cubemap conversion."});
    }
    id<MTLBlitCommandEncoder> mip_blit = [command blitCommandEncoder];
    if (mip_blit == nil)
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::command_submission_failed,
          .message = "Metal could not encode environment cubemap mip generation."});
    [mip_blit generateMipmapsForTexture:environment_cube];
    [mip_blit endEncoding];

    if (!dispatch_cube(ibl_irradiance_pipeline_, environment_cube, irradiance_write_view,
                       data.irradiance_resolution, data.irradiance_resolution, 64, 0.0F,
                       static_cast<float>(data.environment_resolution),
                       static_cast<float>(data.environment_resolution), ibl_sampler_)) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::command_submission_failed,
          .message = "Metal could not encode diffuse irradiance convolution."});
    }

    for (std::uint32_t mip = 0; mip < data.prefilter_mip_count; ++mip) {
      const std::uint32_t mip_size = std::max(1U, data.prefilter_resolution >> mip);
      id<MTLTexture> mip_view = make_cube_array_view(prefiltered_cube, mip);
      if (mip_view == nil)
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::resource_creation_failed,
            .message = "Metal could not create a prefiltered environment mip view."});
      const float roughness = data.prefilter_mip_count > 1
                                  ? static_cast<float>(mip) /
                                        static_cast<float>(data.prefilter_mip_count - 1U)
                                  : 0.0F;
      if (!dispatch_cube(ibl_prefilter_pipeline_, environment_cube, mip_view, mip_size,
                         mip_size, 128, roughness,
                         static_cast<float>(data.environment_resolution),
                         static_cast<float>(data.environment_resolution), ibl_sampler_)) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::command_submission_failed,
            .message = "Metal could not encode GGX specular prefilter convolution."});
      }
    }

    id<MTLComputeCommandEncoder> brdf_encoder = [command computeCommandEncoder];
    if (brdf_encoder == nil)
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::command_submission_failed,
          .message = "Metal could not create the BRDF integration encoder."});
    IblDispatchUniforms brdf_uniforms{};
    brdf_uniforms.dimensions[0] = data.brdf_lut_resolution;
    brdf_uniforms.dimensions[1] = data.brdf_lut_resolution;
    brdf_uniforms.dimensions[2] = 1;
    brdf_uniforms.dimensions[3] = 128;
    [brdf_encoder setComputePipelineState:ibl_brdf_pipeline_];
    [brdf_encoder setBytes:&brdf_uniforms length:sizeof(brdf_uniforms) atIndex:0];
    [brdf_encoder setTexture:brdf_lut atIndex:0];
    const NSUInteger brdf_width = ibl_brdf_pipeline_.threadExecutionWidth;
    const NSUInteger brdf_height =
        std::max<NSUInteger>(1, ibl_brdf_pipeline_.maxTotalThreadsPerThreadgroup / brdf_width);
    [brdf_encoder dispatchThreads:MTLSizeMake(data.brdf_lut_resolution,
                                              data.brdf_lut_resolution, 1)
                threadsPerThreadgroup:MTLSizeMake(brdf_width, brdf_height, 1)];
    [brdf_encoder endEncoding];

    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::command_submission_failed,
          .message = command.error != nil && command.error.localizedDescription != nil
                         ? std::string{command.error.localizedDescription.UTF8String}
                         : std::string{"Metal IBL precomputation failed."}});
    }
    return std::unique_ptr<rhi::GpuEnvironment>{
        new MetalEnvironment{environment_cube, irradiance_cube, prefiltered_cube, brdf_lut,
                             data.prefilter_mip_count}};
  }

  std::expected<void, rhi::RenderError> render_frame(rhi::Color clear_color) override {
    const rhi::SceneSettings settings{};
    return encode_frame(clear_color, nullptr, settings, {});
  }

  std::expected<void, rhi::RenderError> render_scene(rhi::Color clear_color,
                                                     std::span<const rhi::DrawItem> draw_items,
                                                     const rhi::Matrix4& view_projection,
                                                     const rhi::SceneSettings& settings) override {
    for (const auto& item : draw_items) {
      if (item.mesh == nullptr && item.virtual_geometry == nullptr) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message = "A scene draw item has neither a mesh nor virtual geometry."});
      }
      if (item.mesh != nullptr && dynamic_cast<const MetalMesh*>(item.mesh) == nullptr) {
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::invalid_mesh,
                             .message = "A mesh was not created by a compatible Metal renderer."});
      }
      if (item.mesh != nullptr) {
        const auto* mesh = static_cast<const MetalMesh*>(item.mesh);
        const auto count = item.index_count == 0U ? mesh->index_count() : item.index_count;
        if (item.first_index > mesh->index_count() ||
            count > mesh->index_count() - item.first_index) {
          return std::unexpected(rhi::RenderError{
              .code = rhi::RenderErrorCode::invalid_mesh,
              .message = "A draw item index range exceeds its shared mesh index buffer."});
        }
      } else if (item.first_index != 0U || item.index_count != 0U) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message = "An index subrange requires a regular indexed mesh."});
      }
      if (item.virtual_geometry != nullptr &&
          dynamic_cast<const MetalVirtualGeometry*>(item.virtual_geometry) == nullptr) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message = "Virtual geometry was not created by a compatible Metal renderer."});
      }
      if (!item.instance_transforms.empty() &&
          (item.mesh == nullptr || item.virtual_geometry != nullptr ||
           !item.joint_matrices.empty() ||
           (!item.previous_instance_transforms.empty() &&
            item.previous_instance_transforms.size() != item.instance_transforms.size()))) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message = "Instancing requires a static mesh and matching previous transforms."});
      }
      if (item.virtual_geometry != nullptr) {
        const auto valid_view = rhi::validate_virtual_geometry_view(item.virtual_geometry_view);
        if (!valid_view)
          return std::unexpected(rhi::RenderError{.code = rhi::RenderErrorCode::invalid_mesh,
                                                  .message = valid_view.error()});
      }
      const auto is_metal_texture = [](const rhi::GpuTexture* texture) {
        return texture == nullptr || dynamic_cast<const MetalTexture*>(texture) != nullptr;
      };
      const auto& material = item.material;
      if (item.use_material && (!is_metal_texture(material.base_color_texture) ||
                                !is_metal_texture(material.metallic_roughness_texture) ||
                                !is_metal_texture(material.normal_texture) ||
                                !is_metal_texture(material.occlusion_texture) ||
                                !is_metal_texture(material.emissive_texture))) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message = "A material references a texture from an incompatible backend."});
      }
      if (item.joint_matrices.size() > 256U || item.previous_joint_matrices.size() > 256U) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message =
                "A current or previous skin palette exceeds the supported 256-joint limit."});
      }
      if (item.use_material && item.material.alpha_mode > 2U) {
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::invalid_mesh,
                             .message = "A material uses an unsupported alpha mode."});
      }
      if (item.use_material &&
          (item.material.base_color_texcoord > 1U ||
           item.material.metallic_roughness_texcoord > 1U || item.material.normal_texcoord > 1U ||
           item.material.occlusion_texcoord > 1U || item.material.emissive_texcoord > 1U)) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_mesh,
            .message =
                "A material texture selects an unsupported UV set; Metal supports UV0 and UV1."});
      }
    }
    if (!std::isfinite(settings.render_scale) || settings.render_scale < 0.5F ||
        settings.render_scale > 1.0F) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::resource_creation_failed,
          .message = "Scene render scale must be finite and between 0.5 and 1.0."});
    }
    if (const auto valid_clouds = rhi::validate_volumetric_clouds(settings.clouds);
        !valid_clouds) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::resource_creation_failed,
          .message = valid_clouds.error()});
    }
    if (settings.environment != nullptr &&
        dynamic_cast<const MetalEnvironment*>(settings.environment) == nullptr) {
      return std::unexpected(rhi::RenderError{
          .code = rhi::RenderErrorCode::invalid_scene,
          .message = "The environment was not created by this Metal renderer."});
    }
    return encode_frame(clear_color, &view_projection, settings, draw_items);
  }

private:
  MetalRenderDevice(
      CAMetalLayer* layer, id<MTLDevice> device, id<MTLCommandQueue> queue,
      id<MTLRenderPipelineState> scene_pipeline, id<MTLRenderPipelineState> blend_pipeline,
      id<MTLRenderPipelineState> shadow_pipeline,
      id<MTLRenderPipelineState> static_scene_pipeline,
      id<MTLRenderPipelineState> static_blend_pipeline,
      id<MTLRenderPipelineState> static_shadow_pipeline,
      id<MTLRenderPipelineState> sky_pipeline,
      id<MTLRenderPipelineState> post_pipeline, id<MTLRenderPipelineState> taa_pipeline,
      id<MTLRenderPipelineState> gtao_pipeline,
      id<MTLArgumentEncoder> scene_fragment_argument_encoder,
      id<MTLComputePipelineState> virtual_geometry_cull_pipeline,
      id<MTLComputePipelineState> virtual_geometry_encode_icb_pipeline,
      id<MTLArgumentEncoder> virtual_geometry_icb_argument_encoder,
      bool virtual_geometry_icb_supported,
      id<MTLComputePipelineState> ibl_equirect_pipeline,
      id<MTLComputePipelineState> ibl_irradiance_pipeline,
      id<MTLComputePipelineState> ibl_prefilter_pipeline,
      id<MTLComputePipelineState> ibl_brdf_pipeline,
      id<MTLDepthStencilState> depth_state, id<MTLDepthStencilState> transparent_depth_state,
      id<MTLDepthStencilState> shadow_depth_state, id<MTLSamplerState> sampler,
      id<MTLSamplerState> taa_sampler, id<MTLSamplerState> shadow_sampler,
      id<MTLSamplerState> ibl_sampler, id<MTLSamplerState> ibl_equirect_sampler,
      std::array<id<MTLTexture>, shadow_cascade_count> shadow_textures, NSString* device_name)
      : layer_(layer), device_(device), queue_(queue), scene_pipeline_(scene_pipeline),
        blend_pipeline_(blend_pipeline), shadow_pipeline_(shadow_pipeline),
        static_scene_pipeline_(static_scene_pipeline), static_blend_pipeline_(static_blend_pipeline),
        static_shadow_pipeline_(static_shadow_pipeline),
        sky_pipeline_(sky_pipeline), post_pipeline_(post_pipeline), taa_pipeline_(taa_pipeline),
        gtao_pipeline_(gtao_pipeline),
        scene_fragment_argument_encoder_(scene_fragment_argument_encoder),
        virtual_geometry_cull_pipeline_(virtual_geometry_cull_pipeline),
        virtual_geometry_encode_icb_pipeline_(virtual_geometry_encode_icb_pipeline),
        virtual_geometry_icb_argument_encoder_(virtual_geometry_icb_argument_encoder),
        virtual_geometry_icb_supported_(virtual_geometry_icb_supported),
        ibl_equirect_pipeline_(ibl_equirect_pipeline), ibl_irradiance_pipeline_(ibl_irradiance_pipeline),
        ibl_prefilter_pipeline_(ibl_prefilter_pipeline), ibl_brdf_pipeline_(ibl_brdf_pipeline),
        depth_state_(depth_state),
        transparent_depth_state_(transparent_depth_state), shadow_depth_state_(shadow_depth_state),
        sampler_(sampler), taa_sampler_(taa_sampler), shadow_sampler_(shadow_sampler),
        ibl_sampler_(ibl_sampler), ibl_equirect_sampler_(ibl_equirect_sampler),
        shadow_textures_(shadow_textures) {
    const char* name = device_name.UTF8String;
    device_name_ = name != nullptr ? name : "Apple GPU";
  }

  bool ensure_scene_targets(NSUInteger width, NSUInteger height) {
    if (hdr_texture_ != nil && indirect_diffuse_texture_ != nil && gtao_hdr_texture_ != nil &&
        depth_texture_ != nil && velocity_texture_ != nil &&
        reactive_texture_ != nil &&
        oit_accumulation_texture_ != nil && oit_revealage_texture_ != nil &&
        history_color_[0] != nil && history_color_[1] != nil && history_depth_[0] != nil &&
        history_depth_[1] != nil && hdr_texture_.width == width && hdr_texture_.height == height &&
        depth_texture_.width == width && depth_texture_.height == height &&
        indirect_diffuse_texture_.width == width && indirect_diffuse_texture_.height == height &&
        gtao_hdr_texture_.width == width && gtao_hdr_texture_.height == height &&
        velocity_texture_.width == width && velocity_texture_.height == height &&
        reactive_texture_.width == width && reactive_texture_.height == height &&
        oit_accumulation_texture_.width == width && oit_accumulation_texture_.height == height &&
        oit_revealage_texture_.width == width && oit_revealage_texture_.height == height)
      return true;
    constexpr MTLTextureUsage sampled_render_target =
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> new_hdr = make_color_texture(device_, MTLPixelFormatRGBA16Float, width, height,
                                                sampled_render_target);
    id<MTLTexture> new_indirect_diffuse = make_color_texture(
        device_, MTLPixelFormatRGBA16Float, width, height, sampled_render_target);
    id<MTLTexture> new_gtao_hdr = make_color_texture(
        device_, MTLPixelFormatRGBA16Float, width, height, sampled_render_target);
    id<MTLTexture> new_velocity = make_color_texture(device_, MTLPixelFormatRG16Float, width,
                                                     height, sampled_render_target);
    id<MTLTexture> new_reactive =
        make_color_texture(device_, MTLPixelFormatR8Unorm, width, height, sampled_render_target);
    id<MTLTexture> new_oit_accumulation = make_color_texture(
        device_, MTLPixelFormatRGBA16Float, width, height, sampled_render_target);
    id<MTLTexture> new_oit_revealage = make_color_texture(
        device_, MTLPixelFormatR16Float, width, height, sampled_render_target);
    id<MTLTexture> new_depth = make_depth_texture(
        device_, width, height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    std::array<id<MTLTexture>, 2> new_history_color{};
    std::array<id<MTLTexture>, 2> new_history_depth{};
    for (std::size_t index = 0; index < 2; ++index) {
      new_history_color[index] = make_color_texture(device_, MTLPixelFormatRGBA16Float, width,
                                                    height, sampled_render_target);
      new_history_depth[index] =
          make_color_texture(device_, MTLPixelFormatR16Float, width, height, sampled_render_target);
    }
    if (new_hdr == nil || new_indirect_diffuse == nil || new_gtao_hdr == nil ||
        new_velocity == nil || new_reactive == nil ||
        new_oit_accumulation == nil || new_oit_revealage == nil || new_depth == nil ||
        std::ranges::any_of(new_history_color,
                            [](id<MTLTexture> texture) { return texture == nil; }) ||
        std::ranges::any_of(new_history_depth,
                            [](id<MTLTexture> texture) { return texture == nil; }))
      return false;
    hdr_texture_ = new_hdr;
    indirect_diffuse_texture_ = new_indirect_diffuse;
    gtao_hdr_texture_ = new_gtao_hdr;
    velocity_texture_ = new_velocity;
    reactive_texture_ = new_reactive;
    oit_accumulation_texture_ = new_oit_accumulation;
    oit_revealage_texture_ = new_oit_revealage;
    depth_texture_ = new_depth;
    history_color_ = new_history_color;
    history_depth_ = new_history_depth;
    history_read_index_ = 0;
    history_valid_ = false;
    temporal_frame_index_ = 0;
    previous_jitter_ = {};
    return true;
  }

  id<MTLSamplerState> material_sampler(const rhi::MaterialSamplerDesc& sampling) {
    const std::array<std::uint8_t, 5> key{static_cast<std::uint8_t>(sampling.min_filter),
                                          static_cast<std::uint8_t>(sampling.mag_filter),
                                          static_cast<std::uint8_t>(sampling.mip_filter),
                                          static_cast<std::uint8_t>(sampling.address_u),
                                          static_cast<std::uint8_t>(sampling.address_v)};
    const auto cached =
        std::find_if(sampler_cache_.begin(), sampler_cache_.end(),
                     [&key](const SamplerCacheEntry& entry) { return entry.key == key; });
    if (cached != sampler_cache_.end())
      return cached->state;

    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.supportArgumentBuffers = YES;
    descriptor.minFilter = sampling.min_filter == rhi::MaterialFilter::nearest
                               ? MTLSamplerMinMagFilterNearest
                               : MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = sampling.mag_filter == rhi::MaterialFilter::nearest
                               ? MTLSamplerMinMagFilterNearest
                               : MTLSamplerMinMagFilterLinear;
    switch (sampling.mip_filter) {
    case rhi::MaterialMipFilter::none:
      descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
      break;
    case rhi::MaterialMipFilter::nearest:
      descriptor.mipFilter = MTLSamplerMipFilterNearest;
      break;
    case rhi::MaterialMipFilter::linear:
      descriptor.mipFilter = MTLSamplerMipFilterLinear;
      break;
    }
    const auto address_mode = [](rhi::MaterialAddressMode mode) {
      switch (mode) {
      case rhi::MaterialAddressMode::repeat:
        return MTLSamplerAddressModeRepeat;
      case rhi::MaterialAddressMode::mirrored_repeat:
        return MTLSamplerAddressModeMirrorRepeat;
      case rhi::MaterialAddressMode::clamp_to_edge:
        return MTLSamplerAddressModeClampToEdge;
      }
      return MTLSamplerAddressModeRepeat;
    };
    descriptor.sAddressMode = address_mode(sampling.address_u);
    descriptor.tAddressMode = address_mode(sampling.address_v);
    id<MTLSamplerState> state = [device_ newSamplerStateWithDescriptor:descriptor];
    if (state != nil)
      sampler_cache_.push_back(SamplerCacheEntry{key, state});
    return state != nil ? state : sampler_;
  }

  SceneUniforms make_scene_uniforms(const rhi::DrawItem& item, const rhi::Matrix4* view_projection,
                                    const rhi::SceneSettings& settings,
                                    const CascadedShadowData& shadows) const {
    SceneUniforms uniforms{};
    if (view_projection != nullptr)
      std::copy_n(view_projection->values.data(), 16, uniforms.view_projection);
    std::copy_n(item.model.values.data(), 16, uniforms.model);
    const auto& previous_model = item.has_previous_transform ? item.previous_model : item.model;
    std::copy_n(previous_model.values.data(), 16, uniforms.previous_model);
    const auto& previous_view_projection =
        settings.has_previous_view_projection
            ? settings.previous_view_projection
            : (view_projection != nullptr ? *view_projection : settings.previous_view_projection);
    std::copy_n(previous_view_projection.values.data(), 16, uniforms.previous_view_projection);
    for (std::size_t cascade = 0; cascade < shadow_cascade_count; ++cascade) {
      std::copy_n(shadows.matrices[cascade].values.data(), 16,
                  uniforms.light_view_projection[cascade]);
      uniforms.cascade_splits[cascade] = shadows.splits[cascade];
    }
    uniforms.cascade_camera_forward[0] = shadows.camera_forward.x;
    uniforms.cascade_camera_forward[1] = shadows.camera_forward.y;
    uniforms.cascade_camera_forward[2] = shadows.camera_forward.z;
    uniforms.camera_position[0] = settings.camera_position.x;
    uniforms.camera_position[1] = settings.camera_position.y;
    uniforms.camera_position[2] = settings.camera_position.z;
    uniforms.light_direction[0] = settings.light_direction.x;
    uniforms.light_direction[1] = settings.light_direction.y;
    uniforms.light_direction[2] = settings.light_direction.z;
    uniforms.light_radiance[0] = settings.directional_light_radiance.x;
    uniforms.light_radiance[1] = settings.directional_light_radiance.y;
    uniforms.light_radiance[2] = settings.directional_light_radiance.z;
    uniforms.material[0] = item.metallic;
    uniforms.material[1] = item.roughness;
    const auto set_color = [](float* target, rhi::Color color) {
      target[0] = color.red;
      target[1] = color.green;
      target[2] = color.blue;
      target[3] = color.alpha;
    };
    const auto& material = item.material;
    if (item.use_material) {
      uniforms.material[0] = material.metallic;
      uniforms.material[1] = material.roughness;
      set_color(uniforms.base_color, material.base_color);
      set_color(uniforms.emissive, material.emissive);
      uniforms.material_flags[0] = material.base_color_texture != nullptr;
      uniforms.material_flags[1] = material.metallic_roughness_texture != nullptr;
      uniforms.material_flags[2] = material.normal_texture != nullptr;
      uniforms.material_flags[3] = material.occlusion_texture != nullptr;
      uniforms.texture_flags2[0] = material.emissive_texture != nullptr;
      uniforms.texture_flags2[1] = material.normal_scale;
      uniforms.texture_flags2[2] = material.occlusion_strength;
      uniforms.texture_flags2[3] = item.instance_transforms.empty() ? 0.0F : 1.0F;
      uniforms.texcoord_sets0[0] = static_cast<float>(material.base_color_texcoord);
      uniforms.texcoord_sets0[1] = static_cast<float>(material.metallic_roughness_texcoord);
      uniforms.texcoord_sets0[2] = static_cast<float>(material.normal_texcoord);
      uniforms.texcoord_sets0[3] = static_cast<float>(material.occlusion_texcoord);
      uniforms.texcoord_sets1[0] = static_cast<float>(material.emissive_texcoord);
      uniforms.alpha_flags[0] = static_cast<float>(material.alpha_mode);
      uniforms.alpha_flags[1] = material.alpha_cutoff;
      uniforms.alpha_flags[2] = material.double_sided ? 1.0F : 0.0F;
      const std::array<rhi::MaterialUvTransform, 5> uv_transforms{
          material.base_color_uv, material.metallic_roughness_uv, material.normal_uv,
          material.occlusion_uv, material.emissive_uv};
      for (std::size_t index = 0; index < uv_transforms.size(); ++index) {
        const auto& transform = uv_transforms[index];
        uniforms.uv_transform[index][0] = transform.offset.x;
        uniforms.uv_transform[index][1] = transform.offset.y;
        uniforms.uv_transform[index][2] = transform.scale.x;
        uniforms.uv_transform[index][3] = transform.scale.y;
        uniforms.uv_rotation[index][0] = std::cos(transform.rotation);
        uniforms.uv_rotation[index][1] = std::sin(transform.rotation);
      }
    } else {
      set_color(uniforms.base_color, {1.0F, 1.0F, 1.0F, 1.0F});
    }
    uniforms.skin_flags[0] = static_cast<float>(item.joint_matrices.size());
    uniforms.morph_flags[3] = static_cast<std::uint32_t>(settings.point_lights.size());
    const bool has_previous_skin =
        item.has_previous_transform &&
        item.previous_joint_matrices.size() == item.joint_matrices.size();
    uniforms.skin_flags[1] = static_cast<float>(
        has_previous_skin ? item.previous_joint_matrices.size() : item.joint_matrices.size());
    uniforms.scene_params[0] = settings.time_seconds;
    uniforms.scene_params[1] = settings.exposure;
    uniforms.scene_params[2] = default_fog_density;
    uniforms.temporal_params[0] = settings.has_previous_view_projection ? 1.0F : 0.0F;
    uniforms.temporal_params[1] = previous_time_seconds_;
    if (const auto* environment =
            dynamic_cast<const MetalEnvironment*>(settings.environment)) {
      uniforms.environment_flags[0] = 1.0F;
      uniforms.environment_flags[1] =
          static_cast<float>(environment->prefilter_mip_count() - 1U);
    }
    return uniforms;
  }

  bool encode_virtual_geometry_culling(id<MTLCommandBuffer> command,
                                       std::span<const rhi::DrawItem> draw_items,
                                       const rhi::Matrix4* frustum_view_projection,
                                       float world_units_per_texel = 0.0F,
                                       bool encode_icb = false) {
    if (std::ranges::none_of(draw_items, [](const rhi::DrawItem& item) {
          return item.virtual_geometry != nullptr;
        }))
      return true;
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil)
      return false;
    [encoder setComputePipelineState:virtual_geometry_cull_pipeline_];
    const NSUInteger width = virtual_geometry_cull_pipeline_.threadExecutionWidth;
    bool dispatched = false;
    for (const auto& item : draw_items) {
      if (item.virtual_geometry == nullptr)
        continue;
      const auto* geometry = static_cast<const MetalVirtualGeometry*>(item.virtual_geometry);
      const auto& culling_matrix = frustum_view_projection != nullptr
                                       ? *frustum_view_projection
                                       : item.virtual_geometry_view.view_projection;
      const auto uniforms = make_virtual_geometry_cull_uniforms(
          item.virtual_geometry_view, item.model, culling_matrix, world_units_per_texel);
      [encoder setBytes:&uniforms length:sizeof(uniforms) atIndex:0];
      [encoder setBuffer:geometry->clusters() offset:0 atIndex:1];
      [encoder setBuffer:geometry->indirect_arguments() offset:0 atIndex:2];
      [encoder dispatchThreads:MTLSizeMake(geometry->cluster_count(), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
      dispatched = true;
    }
    if (dispatched && encode_icb && virtual_geometry_icb_supported_) {
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [encoder setComputePipelineState:virtual_geometry_encode_icb_pipeline_];
      const NSUInteger encode_width = virtual_geometry_encode_icb_pipeline_.threadExecutionWidth;
      for (const auto& item : draw_items) {
        if (item.virtual_geometry == nullptr)
          continue;
        const auto* geometry = static_cast<const MetalVirtualGeometry*>(item.virtual_geometry);
        if (geometry->icb() == nil || geometry->icb_arguments() == nil)
          continue;
        const std::uint32_t cluster_count =
            static_cast<std::uint32_t>(geometry->cluster_count());
        [encoder setBuffer:geometry->clusters() offset:0 atIndex:0];
        [encoder setBuffer:geometry->indirect_arguments() offset:0 atIndex:1];
        [encoder setBuffer:geometry->indices() offset:0 atIndex:2];
        [encoder setBuffer:geometry->icb_arguments() offset:0 atIndex:3];
        [encoder setBytes:&cluster_count length:sizeof(cluster_count) atIndex:4];
        [encoder useResource:geometry->clusters() usage:MTLResourceUsageRead];
        [encoder useResource:geometry->indirect_arguments() usage:MTLResourceUsageRead];
        [encoder useResource:geometry->indices() usage:MTLResourceUsageRead];
        [encoder useResource:geometry->icb() usage:MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(geometry->cluster_count(), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(encode_width, 1, 1)];
      }
    }
    if (dispatched)
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [encoder endEncoding];
    return true;
  }

  bool encode_geometry_pass(id<MTLCommandBuffer> command, const rhi::Matrix4* view_projection,
                            const rhi::SceneSettings& settings, const CascadedShadowData& shadows,
                            std::span<const rhi::DrawItem> draw_items, bool shadow_pass,
                            bool transparent_only = false,
                            std::size_t cascade_index = 0,
                            id<MTLBuffer> fragment_argument_buffer = nil,
                            NSUInteger fragment_argument_stride = 0,
                            NSUInteger* fragment_argument_cursor = nullptr,
                            id<MTLBuffer> point_light_buffer = nil) {
    const rhi::Matrix4* culling_view_projection =
        shadow_pass ? &shadows.matrices[cascade_index] : view_projection;
    const float world_units_per_texel =
        shadow_pass ? shadows.world_units_per_texel[cascade_index] : 0.0F;
    if (!encode_virtual_geometry_culling(command, draw_items, culling_view_projection,
                                         world_units_per_texel, !shadow_pass))
      return false;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (shadow_pass) {
      pass.depthAttachment.texture = shadow_textures_[cascade_index];
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 1.0;
    } else {
      pass.colorAttachments[0].texture = hdr_texture_;
      pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[1].texture = velocity_texture_;
      pass.colorAttachments[1].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[1].storeAction = MTLStoreActionStore;
      pass.colorAttachments[2].texture = reactive_texture_;
      pass.colorAttachments[2].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[2].storeAction = MTLStoreActionStore;
      pass.colorAttachments[3].texture = oit_accumulation_texture_;
      pass.colorAttachments[3].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[3].storeAction = MTLStoreActionStore;
      pass.colorAttachments[4].texture = oit_revealage_texture_;
      pass.colorAttachments[4].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[4].storeAction = MTLStoreActionStore;
      pass.colorAttachments[5].texture = indirect_diffuse_texture_;
      pass.colorAttachments[5].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[5].storeAction = MTLStoreActionStore;
      pass.depthAttachment.texture = depth_texture_;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 1.0;
    }
    std::vector<const rhi::DrawItem*> ordered;
    ordered.reserve(draw_items.size());
    const rhi::Matrix4& culling_matrix =
        shadow_pass ? shadows.matrices[cascade_index] : *view_projection;
    for (const auto& item : draw_items) {
      const bool is_transparent = item.use_material && item.material.alpha_mode == 2U;
      if (shadow_pass && is_transparent)
        continue;
      if (!shadow_pass && is_transparent != transparent_only)
        continue;
      // Static mesh and terrain bounds reject whole draw items before command encoding.
      // Skinned bounds are animated, so retain those until a dedicated animated bound exists.
      if (item.joint_matrices.empty() && item.instance_transforms.empty()) {
        BoundsSphere bounds{};
        if (item.mesh != nullptr) {
          const auto* mesh = static_cast<const MetalMesh*>(item.mesh);
          bounds = mesh->bounds(item.morph_weights);
        } else if (item.virtual_geometry != nullptr) {
          const auto* geometry = static_cast<const MetalVirtualGeometry*>(item.virtual_geometry);
          bounds = geometry->bounds();
        }
        const Vec4 world_center = transform(
            item.model, {bounds.center.x, bounds.center.y, bounds.center.z, 1.0F});
        const float world_radius = bounds.radius * linear_operator_norm_bound(item.model);
        if (!sphere_intersects_frustum(
                culling_matrix, {world_center.x, world_center.y, world_center.z}, world_radius))
          continue;
      }
      ordered.push_back(&item);
    }
    if (ordered.empty())
      return true;
    if (!shadow_pass && (fragment_argument_buffer == nil || fragment_argument_stride == 0 ||
                         fragment_argument_cursor == nullptr))
      return false;

    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil)
      return false;
    if (shadow_pass)
      [encoder setDepthBias:0.001F slopeScale:1.0F clamp:0.0F];
    else {
      const auto declare_texture = [encoder](id<MTLTexture> texture) {
        if (texture != nil)
          [encoder useResource:texture
                        usage:MTLResourceUsageRead
                       stages:MTLRenderStageFragment];
      };
      for (const auto* item : ordered) {
        if (!item->use_material)
          continue;
        const auto& material = item->material;
        const std::array<const rhi::GpuTexture*, 5> material_textures{
            material.base_color_texture, material.metallic_roughness_texture,
            material.normal_texture, material.occlusion_texture, material.emissive_texture};
        for (const auto* source : material_textures) {
          const auto* texture = dynamic_cast<const MetalTexture*>(source);
          declare_texture(texture != nullptr ? texture->texture() : nil);
        }
      }
      for (const auto& texture : shadow_textures_)
        declare_texture(texture);
      const auto* environment = dynamic_cast<const MetalEnvironment*>(settings.environment);
      if (environment != nullptr) {
        declare_texture(environment->irradiance());
        declare_texture(environment->prefiltered());
        declare_texture(environment->brdf_lut());
      }
    }

    for (const auto* item_pointer : ordered) {
      const auto& item = *item_pointer;
      const auto* mesh = item.mesh != nullptr ? static_cast<const MetalMesh*>(item.mesh) : nullptr;
      const auto* virtual_geometry =
          item.virtual_geometry != nullptr
              ? static_cast<const MetalVirtualGeometry*>(item.virtual_geometry)
              : nullptr;
      const std::size_t morph_target_count = mesh != nullptr ? mesh->morph_target_count() : 0U;
      if ((mesh == nullptr && (!item.morph_weights.empty() ||
                               !item.previous_morph_weights.empty() ||
                               !item.instance_morph_weights.empty() ||
                               !item.previous_instance_morph_weights.empty())) ||
          (mesh == nullptr && !item.instance_transforms.empty()) ||
          item.instance_transforms.size() > std::numeric_limits<std::uint32_t>::max() ||
          (!item.previous_instance_transforms.empty() &&
           item.previous_instance_transforms.size() != item.instance_transforms.size()) ||
          (!item.instance_morph_weights.empty() &&
           (!item.morph_weights.empty() || !item.previous_morph_weights.empty())) ||
          (!item.morph_weights.empty() && item.morph_weights.size() != morph_target_count) ||
          (!item.previous_morph_weights.empty() &&
           (item.morph_weights.empty() || item.previous_morph_weights.size() != morph_target_count)) ||
          (!item.instance_morph_weights.empty() &&
           (item.instance_transforms.empty() || morph_target_count == 0U ||
            item.instance_transforms.size() > std::numeric_limits<std::size_t>::max() /
                                                 morph_target_count ||
            item.instance_morph_weights.size() !=
                item.instance_transforms.size() * morph_target_count)) ||
          (!item.previous_instance_morph_weights.empty() &&
           (item.instance_morph_weights.empty() || item.instance_transforms.empty() ||
            morph_target_count == 0U ||
            item.instance_transforms.size() > std::numeric_limits<std::size_t>::max() /
                                                 morph_target_count ||
            item.previous_instance_morph_weights.size() !=
                item.instance_transforms.size() * morph_target_count)) ||
          !rhi::morph_weights_are_finite(item.morph_weights) ||
          !rhi::morph_weights_are_finite(item.previous_morph_weights) ||
          !rhi::morph_weights_are_finite(item.instance_morph_weights) ||
          !rhi::morph_weights_are_finite(item.previous_instance_morph_weights))
        return false;
      std::span<const rhi::Matrix4> instance_models = item.instance_transforms;
      std::span<const rhi::Matrix4> previous_instance_models = rhi::previous_frame_span(
          item.instance_transforms, item.previous_instance_transforms, item.has_previous_transform);
      std::span<const float> current_morph_weights = item.instance_morph_weights.empty()
          ? item.morph_weights : item.instance_morph_weights;
      const bool per_instance_morph_weights = !item.instance_morph_weights.empty();
      std::span<const float> previous_morph_weights = per_instance_morph_weights
          ? rhi::previous_frame_span(item.instance_morph_weights,
                                     item.previous_instance_morph_weights,
                                     item.has_previous_transform)
          : rhi::previous_frame_span(item.morph_weights, item.previous_morph_weights,
                                     item.has_previous_transform);
      if (!instance_models.empty()) {
        const auto shared_local_bounds = per_instance_morph_weights
            ? mesh->bounds()
            : mesh->bounds(item.morph_weights);
        auto& visible_instances = next_instance_cull_scratch();
        visible_instances.reserve(instance_models.size());
        const bool has_distinct_previous = !previous_instance_models.empty() &&
                                           previous_instance_models.data() != instance_models.data();
        std::vector<rhi::Matrix4>* visible_previous = nullptr;
        if (has_distinct_previous) {
          visible_previous = &next_instance_cull_scratch();
          visible_previous->reserve(previous_instance_models.size());
        }
        std::vector<float>* visible_morph_weights = nullptr;
        std::vector<float>* visible_previous_morph_weights = nullptr;
        std::vector<std::uint32_t> visible_instance_indices;
        if (per_instance_morph_weights) {
          visible_morph_weights = &next_instance_morph_scratch();
          visible_morph_weights->reserve(item.instance_morph_weights.size());
          visible_instance_indices.reserve(instance_models.size());
          if (item.has_previous_transform && !item.previous_instance_morph_weights.empty()) {
            visible_previous_morph_weights = &next_instance_morph_scratch();
            visible_previous_morph_weights->reserve(item.previous_instance_morph_weights.size());
          }
        }
        for (std::size_t instance_index = 0; instance_index < instance_models.size();
             ++instance_index) {
          const auto local_bounds = per_instance_morph_weights
              ? mesh->bounds(item.instance_morph_weights.subspan(
                    instance_index * morph_target_count, morph_target_count))
              : shared_local_bounds;
          const auto combined_model = multiply(instance_models[instance_index], item.model);
          const Vec4 world_center = transform(
              combined_model,
              {local_bounds.center.x, local_bounds.center.y, local_bounds.center.z, 1.0F});
          const float world_radius =
              local_bounds.radius * linear_operator_norm_bound(combined_model);
          if (!sphere_intersects_frustum(
                  culling_matrix, {world_center.x, world_center.y, world_center.z}, world_radius))
            continue;
          visible_instances.push_back(instance_models[instance_index]);
          if (per_instance_morph_weights)
            visible_instance_indices.push_back(static_cast<std::uint32_t>(instance_index));
          if (visible_previous != nullptr && instance_index < previous_instance_models.size())
            visible_previous->push_back(previous_instance_models[instance_index]);
        }
        if (visible_instances.empty())
          continue;
        instance_models = visible_instances;
        if (visible_previous != nullptr)
          previous_instance_models = *visible_previous;
        else
          previous_instance_models = instance_models;
        if (visible_morph_weights != nullptr) {
          const auto compacted = rhi::compact_instance_morph_weights(
              item.instance_morph_weights,
              visible_previous_morph_weights != nullptr
                  ? item.previous_instance_morph_weights : std::span<const float>{},
              visible_instance_indices, morph_target_count,
              *visible_morph_weights, visible_previous_morph_weights);
          if (!compacted)
            return false;
          current_morph_weights = *visible_morph_weights;
        }
        if (visible_previous_morph_weights != nullptr)
          previous_morph_weights = *visible_previous_morph_weights;
        else if (per_instance_morph_weights)
          previous_morph_weights = current_morph_weights;
      }
      const NSUInteger argument_offset =
          shadow_pass ? 0U : (*fragment_argument_cursor)++ * fragment_argument_stride;
      const bool blended = !shadow_pass && item.use_material && item.material.alpha_mode == 2U;
      const bool skinned = mesh != nullptr && mesh->skinned();
      id<MTLRenderPipelineState> pipeline =
          shadow_pass ? (skinned ? shadow_pipeline_ : static_shadow_pipeline_)
                      : blended ? (skinned ? blend_pipeline_ : static_blend_pipeline_)
                                : (skinned ? scene_pipeline_ : static_scene_pipeline_);
      [encoder setRenderPipelineState:pipeline];
      [encoder setDepthStencilState:shadow_pass
                                        ? shadow_depth_state_
                                        : (blended ? transparent_depth_state_ : depth_state_)];
      SceneUniforms uniforms = make_scene_uniforms(item, view_projection, settings, shadows);
      uniforms.scene_params[3] = static_cast<float>(cascade_index);
      const bool has_morph_weights = !current_morph_weights.empty();
      uniforms.morph_flags[0] = has_morph_weights ? static_cast<std::uint32_t>(morph_target_count) : 0U;
      uniforms.morph_flags[1] = per_instance_morph_weights ? 1U : 0U;
      uniforms.morph_flags[2] = mesh != nullptr ? static_cast<std::uint32_t>(mesh->vertex_count()) : 0U;
      [encoder setVertexBuffer:mesh != nullptr ? mesh->vertices() : virtual_geometry->vertices()
                        offset:0
                       atIndex:0];
      id<MTLBuffer> joint_buffer = nil;
      id<MTLBuffer> previous_joint_buffer = nil;
      if (!item.joint_matrices.empty()) {
        joint_buffer = skin_buffer(item.joint_matrices);
        if (joint_buffer == nil) {
          [encoder endEncoding];
          return false;
        }
        if (item.has_previous_transform &&
            item.previous_joint_matrices.size() == item.joint_matrices.size()) {
          previous_joint_buffer = skin_buffer(item.previous_joint_matrices);
          if (previous_joint_buffer == nil) {
            [encoder endEncoding];
            return false;
          }
        } else {
          previous_joint_buffer = joint_buffer;
        }
      }
      const rhi::Matrix4 identity_instance{};
      if (joint_buffer != nil)
        [encoder setVertexBuffer:joint_buffer offset:0 atIndex:2];
      else
        [encoder setVertexBytes:&identity_instance length:sizeof(identity_instance) atIndex:2];
      if (previous_joint_buffer != nil)
        [encoder setVertexBuffer:previous_joint_buffer offset:0 atIndex:3];
      else
        [encoder setVertexBytes:&identity_instance length:sizeof(identity_instance) atIndex:3];
      id<MTLBuffer> instance_buffer = nil;
      id<MTLBuffer> previous_instance_buffer = nil;
      if (!instance_models.empty()) {
        instance_buffer = skin_buffer(instance_models);
        previous_instance_buffer = previous_instance_models.empty()
                                      ? instance_buffer
                                      : skin_buffer(previous_instance_models);
        if (instance_buffer == nil || previous_instance_buffer == nil) {
          [encoder endEncoding];
          return false;
        }
      }
      if (instance_buffer != nil)
        [encoder setVertexBuffer:instance_buffer offset:0 atIndex:4];
      else
        [encoder setVertexBytes:&identity_instance length:sizeof(identity_instance) atIndex:4];
      if (previous_instance_buffer != nil)
        [encoder setVertexBuffer:previous_instance_buffer offset:0 atIndex:5];
      else
        [encoder setVertexBytes:&identity_instance length:sizeof(identity_instance) atIndex:5];
      id<MTLBuffer> current_morph_buffer = has_morph_weights
          ? morph_weight_buffer(current_morph_weights) : nil;
      id<MTLBuffer> previous_morph_buffer = has_morph_weights
          ? morph_weight_buffer(previous_morph_weights) : nil;
      if (has_morph_weights && (current_morph_buffer == nil || previous_morph_buffer == nil ||
                                mesh == nullptr || mesh->morph_deltas() == nil ||
                                current_morph_weights.size() !=
                                    (per_instance_morph_weights
                                         ? instance_models.size() * morph_target_count
                                         : morph_target_count) ||
                                previous_morph_weights.size() != current_morph_weights.size())) {
        [encoder endEncoding];
        return false;
      }
      const float zero_weight = 0.0F;
      const PackedMorphDelta zero_delta{};
      if (current_morph_buffer != nil)
        [encoder setVertexBuffer:current_morph_buffer offset:0 atIndex:6];
      else
        [encoder setVertexBytes:&zero_weight length:sizeof(zero_weight) atIndex:6];
      if (previous_morph_buffer != nil)
        [encoder setVertexBuffer:previous_morph_buffer offset:0 atIndex:7];
      else
        [encoder setVertexBytes:&zero_weight length:sizeof(zero_weight) atIndex:7];
      if (mesh != nullptr && mesh->morph_deltas() != nil)
        [encoder setVertexBuffer:mesh->morph_deltas() offset:0 atIndex:8];
      else
        [encoder setVertexBytes:&zero_delta length:sizeof(zero_delta) atIndex:8];
      [encoder setVertexBytes:&uniforms length:sizeof(SceneUniforms) atIndex:1];
      if (shadow_pass) {
        const auto* base_texture =
            item.use_material ? dynamic_cast<const MetalTexture*>(item.material.base_color_texture)
                              : nullptr;
        [encoder setFragmentTexture:base_texture != nullptr ? base_texture->texture() : nil
                            atIndex:0];
        [encoder setFragmentBytes:&uniforms length:sizeof(SceneUniforms) atIndex:1];
        [encoder setFragmentSamplerState:material_sampler(item.material.base_color_sampler)
                                 atIndex:0];
      } else {
        [scene_fragment_argument_encoder_ setArgumentBuffer:fragment_argument_buffer
                                                     offset:argument_offset];
        const auto bind_texture = [this](const rhi::GpuTexture* source, NSUInteger index) {
          const auto* texture = dynamic_cast<const MetalTexture*>(source);
          id<MTLTexture> resource = texture != nullptr ? texture->texture() : nil;
          [scene_fragment_argument_encoder_ setTexture:resource atIndex:index];
        };
        const auto& material = item.material;
        bind_texture(item.use_material ? material.base_color_texture : nullptr, 0);
        bind_texture(item.use_material ? material.metallic_roughness_texture : nullptr, 1);
        bind_texture(item.use_material ? material.normal_texture : nullptr, 2);
        bind_texture(item.use_material ? material.occlusion_texture : nullptr, 3);
        bind_texture(item.use_material ? material.emissive_texture : nullptr, 4);
        for (NSUInteger cascade = 0; cascade < shadow_cascade_count; ++cascade) {
          [scene_fragment_argument_encoder_ setTexture:shadow_textures_[cascade]
                                                atIndex:5 + cascade];
        }
        [encoder setFragmentBytes:&uniforms length:sizeof(SceneUniforms) atIndex:1];
        [scene_fragment_argument_encoder_ setSamplerState:material_sampler(material.base_color_sampler)
                                                  atIndex:11];
        [scene_fragment_argument_encoder_ setSamplerState:material_sampler(material.metallic_roughness_sampler)
                                                  atIndex:12];
        [scene_fragment_argument_encoder_ setSamplerState:material_sampler(material.normal_sampler)
                                                  atIndex:13];
        [scene_fragment_argument_encoder_ setSamplerState:material_sampler(material.occlusion_sampler)
                                                  atIndex:14];
        [scene_fragment_argument_encoder_ setSamplerState:material_sampler(material.emissive_sampler)
                                                  atIndex:15];
        [scene_fragment_argument_encoder_ setSamplerState:shadow_sampler_ atIndex:16];
        [scene_fragment_argument_encoder_ setSamplerState:shadow_sampler_ atIndex:17];
        [scene_fragment_argument_encoder_ setSamplerState:shadow_sampler_ atIndex:18];
        const auto* environment =
            dynamic_cast<const MetalEnvironment*>(settings.environment);
        const std::array<id<MTLTexture>, 3> environment_textures{
            environment != nullptr ? environment->irradiance() : nil,
            environment != nullptr ? environment->prefiltered() : nil,
            environment != nullptr ? environment->brdf_lut() : nil};
        for (NSUInteger texture_index = 0; texture_index < environment_textures.size();
             ++texture_index) {
          id<MTLTexture> texture = environment_textures[texture_index];
          [scene_fragment_argument_encoder_ setTexture:texture atIndex:8 + texture_index];
        }
        [scene_fragment_argument_encoder_ setSamplerState:ibl_sampler_ atIndex:19];
        [encoder setFragmentBuffer:fragment_argument_buffer offset:argument_offset atIndex:2];
        [encoder setFragmentBuffer:point_light_buffer offset:0 atIndex:3];
      }
      if (virtual_geometry != nullptr) {
        if (!shadow_pass && virtual_geometry->icb() != nil) {
          [encoder useResource:virtual_geometry->vertices()
                        usage:MTLResourceUsageRead
                       stages:MTLRenderStageVertex];
          [encoder useResource:virtual_geometry->indices()
                        usage:MTLResourceUsageRead
                       stages:MTLRenderStageVertex];
          if (joint_buffer != nil)
            [encoder useResource:joint_buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
          if (previous_joint_buffer != nil && previous_joint_buffer != joint_buffer)
            [encoder useResource:previous_joint_buffer
                          usage:MTLResourceUsageRead
                         stages:MTLRenderStageVertex];
          if (instance_buffer != nil)
            [encoder useResource:instance_buffer
                          usage:MTLResourceUsageRead
                         stages:MTLRenderStageVertex];
          if (previous_instance_buffer != nil && previous_instance_buffer != instance_buffer)
            [encoder useResource:previous_instance_buffer
                          usage:MTLResourceUsageRead
                         stages:MTLRenderStageVertex];
          [encoder useResource:fragment_argument_buffer
                        usage:MTLResourceUsageRead
                       stages:MTLRenderStageFragment];
          [encoder useResource:virtual_geometry->icb()
                        usage:MTLResourceUsageRead
                       stages:MTLRenderStageVertex | MTLRenderStageFragment];
          [encoder executeCommandsInBuffer:virtual_geometry->icb()
                                 withRange:NSMakeRange(0, virtual_geometry->cluster_count())];
        } else {
          for (NSUInteger cluster = 0; cluster < virtual_geometry->cluster_count(); ++cluster) {
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:virtual_geometry->indices()
                         indexBufferOffset:0
                            indirectBuffer:virtual_geometry->indirect_arguments()
                      indirectBufferOffset:cluster * sizeof(VirtualGeometryIndirectArguments)];
          }
        }
      } else {
        const NSUInteger instance_count = instance_models.empty()
                                              ? 1U
                                              : instance_models.size();
        const NSUInteger index_count = item.index_count == 0U ? mesh->index_count()
                                                               : item.index_count;
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:index_count
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:mesh->indices()
                     indexBufferOffset:static_cast<NSUInteger>(item.first_index) * sizeof(std::uint32_t)
                         instanceCount:instance_count];
      }
    }
    [encoder endEncoding];
    return true;
  }

  id<MTLBuffer> skin_buffer(std::span<const rhi::Matrix4> matrices) {
    const auto cached = std::find_if(
        skin_buffer_cache_.begin(), skin_buffer_cache_.end(), [&matrices](const auto& entry) {
          return entry.data == matrices.data() && entry.count == matrices.size();
        });
    if (cached != skin_buffer_cache_.end())
      return cached->buffer;
    id<MTLBuffer> buffer = [device_ newBufferWithBytes:matrices.data()
                                                length:matrices.size_bytes()
                                               options:MTLResourceStorageModeShared];
    if (buffer != nil)
      skin_buffer_cache_.push_back({matrices.data(), matrices.size(), buffer});
    return buffer;
  }

  id<MTLBuffer> morph_weight_buffer(std::span<const float> weights) {
    if (weights.empty())
      return nil;
    const auto cached = std::find_if(
        morph_weight_buffer_cache_.begin(), morph_weight_buffer_cache_.end(),
        [&weights](const auto& entry) {
          return entry.data == weights.data() && entry.count == weights.size();
        });
    if (cached != morph_weight_buffer_cache_.end())
      return cached->buffer;
    id<MTLBuffer> buffer = [device_ newBufferWithBytes:weights.data()
                                                length:weights.size_bytes()
                                               options:MTLResourceStorageModeShared];
    if (buffer != nil)
      morph_weight_buffer_cache_.push_back({weights.data(), weights.size(), buffer});
    return buffer;
  }

  std::vector<rhi::Matrix4>& next_instance_cull_scratch() {
    if (instance_cull_scratch_cursor_ == instance_cull_scratch_.size())
      instance_cull_scratch_.emplace_back();
    auto& result = instance_cull_scratch_[instance_cull_scratch_cursor_++];
    result.clear();
    return result;
  }

  std::vector<float>& next_instance_morph_scratch() {
    if (instance_morph_scratch_cursor_ == instance_morph_scratch_.size())
      instance_morph_scratch_.emplace_back();
    auto& result = instance_morph_scratch_[instance_morph_scratch_cursor_++];
    result.clear();
    return result;
  }

  std::expected<void, rhi::RenderError> encode_frame(rhi::Color clear_color,
                                                     const rhi::Matrix4* view_projection,
                                                     const rhi::SceneSettings& settings,
                                                     std::span<const rhi::DrawItem> draw_items) {
    @autoreleasepool {
      // One immutable skin-palette upload can serve every primitive and render pass this frame.
      skin_buffer_cache_.clear();
      morph_weight_buffer_cache_.clear();
      instance_cull_scratch_cursor_ = 0;
      instance_morph_scratch_cursor_ = 0;
      instance_cull_scratch_.reserve(draw_items.size() * (shadow_cascade_count + 2U) * 2U + 8U);
      instance_morph_scratch_.reserve(draw_items.size() * (shadow_cascade_count + 2U) * 2U + 8U);
      id<CAMetalDrawable> drawable = [layer_ nextDrawable];
      if (drawable == nil) {
        history_valid_ = false;
        return {};
      }
      const NSUInteger width = std::max<NSUInteger>(
          1U, static_cast<NSUInteger>(std::lround(drawable.texture.width * settings.render_scale)));
      const NSUInteger height = std::max<NSUInteger>(
          1U, static_cast<NSUInteger>(std::lround(drawable.texture.height * settings.render_scale)));
      if (width == 0 || height == 0 || !ensure_scene_targets(width, height)) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::resource_creation_failed,
            .message = "Metal could not allocate resize-aware HDR or depth targets."});
      }
      id<MTLCommandBuffer> command = [queue_ commandBuffer];
      if (command == nil) {
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                             .message = "Metal could not allocate a command buffer."});
      }
      const auto gpu_time_nanoseconds = gpu_time_nanoseconds_;
      [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status != MTLCommandBufferStatusCompleted)
          return;
        const double seconds = completed.GPUEndTime - completed.GPUStartTime;
        if (std::isfinite(seconds) && seconds > 0.0)
          gpu_time_nanoseconds->store(static_cast<std::uint64_t>(seconds * 1.0e9),
                                      std::memory_order_release);
      }];
      const renderer::TemporalJitter current_jitter =
          view_projection != nullptr
              ? renderer::temporal_jitter(
                    temporal_frame_index_,
                    {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)})
              : renderer::TemporalJitter{};
      const rhi::Matrix4 jittered_view_projection =
          view_projection != nullptr ? renderer::jitter_projection(*view_projection, current_jitter)
                                     : rhi::Matrix4{};
      rhi::SceneSettings frame_settings = settings;
      frame_settings.has_previous_view_projection =
          view_projection != nullptr && history_valid_ && settings.has_previous_view_projection;
      frame_settings.previous_view_projection =
          frame_settings.has_previous_view_projection
              ? renderer::jitter_projection(settings.previous_view_projection, previous_jitter_)
              : jittered_view_projection;
      const rhi::Matrix4* frame_view_projection =
          view_projection != nullptr ? &jittered_view_projection : nullptr;
      const CascadedShadowData shadows = build_shadow_cascades(settings, draw_items);
      const NSUInteger argument_alignment = scene_fragment_argument_encoder_.alignment;
      const NSUInteger argument_encoded_length = scene_fragment_argument_encoder_.encodedLength;
      if (argument_alignment == 0 || argument_encoded_length == 0 ||
          argument_encoded_length >
              std::numeric_limits<NSUInteger>::max() - (argument_alignment - 1U)) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::resource_creation_failed,
            .message = "The scene fragment argument-buffer layout is invalid."});
      }
      const NSUInteger fragment_argument_stride =
          ((argument_encoded_length + argument_alignment - 1U) / argument_alignment) *
          argument_alignment;
      if (draw_items.size() >
          std::numeric_limits<NSUInteger>::max() / fragment_argument_stride) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::resource_creation_failed,
            .message = "The scene fragment argument buffer exceeds Metal's addressable size."});
      }
      id<MTLBuffer> fragment_argument_buffer =
          draw_items.empty()
              ? nil
              : [device_ newBufferWithLength:fragment_argument_stride * draw_items.size()
                                     options:MTLResourceStorageModeShared];
      if (!draw_items.empty() && fragment_argument_buffer == nil) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::resource_creation_failed,
            .message = "Metal could not allocate the per-frame scene fragment argument buffer."});
      }
      NSUInteger fragment_argument_cursor = 0;

      std::vector<PackedPointLight> packed_point_lights;
      packed_point_lights.reserve(std::max<std::size_t>(1U, frame_settings.point_lights.size()));
      for (const auto& light : frame_settings.point_lights) {
        PackedPointLight packed{};
        packed.position_range[0] = light.position.x;
        packed.position_range[1] = light.position.y;
        packed.position_range[2] = light.position.z;
        packed.position_range[3] = light.range;
        packed.intensity[0] = light.intensity.x;
        packed.intensity[1] = light.intensity.y;
        packed.intensity[2] = light.intensity.z;
        packed_point_lights.push_back(packed);
      }
      if (packed_point_lights.empty())
        packed_point_lights.emplace_back();
      id<MTLBuffer> point_light_buffer = [device_
          newBufferWithBytes:packed_point_lights.data()
                      length:packed_point_lights.size() * sizeof(PackedPointLight)
                     options:MTLResourceStorageModeShared];
      if (point_light_buffer == nil) {
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::resource_creation_failed,
            .message = "Metal could not allocate the per-frame point-light buffer."});
      }

      rhi::RenderGraph graph;
      const auto texture_resource = [width, height](rhi::PixelFormat format,
                                                    rhi::TextureUsage usage) {
        rhi::TextureDesc desc{};
        desc.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        desc.format = format;
        desc.usage = usage;
        return desc;
      };
      const auto shadow_texture_resource = [](std::size_t cascade) {
        rhi::TextureDesc desc{};
        desc.extent = {static_cast<std::uint32_t>(shadow_map_sizes[cascade]),
                       static_cast<std::uint32_t>(shadow_map_sizes[cascade])};
        desc.format = rhi::PixelFormat::depth16_unorm;
        desc.usage = rhi::TextureUsage::depth_stencil_attachment | rhi::TextureUsage::sampled;
        return desc;
      };
      std::array<rhi::GraphResource, shadow_cascade_count> shadow_resources{};
      for (std::size_t cascade = 0; cascade < shadow_cascade_count; ++cascade) {
        shadow_resources[cascade] = graph.import_texture(
            "directional-shadow-cascade-" + std::to_string(cascade),
            shadow_texture_resource(cascade));
      }
      const auto hdr_resource =
          graph.import_texture("scene-hdr", texture_resource(rhi::PixelFormat::rgba16_float,
                                                             rhi::TextureUsage::color_attachment |
                                                                 rhi::TextureUsage::sampled));
      const auto indirect_diffuse_resource = graph.import_texture(
          "scene-indirect-diffuse",
          texture_resource(rhi::PixelFormat::rgba16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto gtao_hdr_resource = graph.import_texture(
          "gtao-composited-hdr",
          texture_resource(rhi::PixelFormat::rgba16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto velocity_resource = graph.import_texture(
          "scene-motion-vectors",
          texture_resource(rhi::PixelFormat::rg16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto reactive_resource = graph.import_texture(
          "scene-reactive-mask",
          texture_resource(rhi::PixelFormat::r8_unorm,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto oit_accumulation_resource = graph.import_texture(
          "oit-accumulation",
          texture_resource(rhi::PixelFormat::rgba16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto oit_revealage_resource = graph.import_texture(
          "oit-revealage",
          texture_resource(rhi::PixelFormat::r16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto depth_resource = graph.import_texture(
          "scene-depth", texture_resource(rhi::PixelFormat::depth32_float,
                                          rhi::TextureUsage::depth_stencil_attachment |
                                              rhi::TextureUsage::sampled));
      const std::size_t history_write_index = 1U - history_read_index_;
      const auto history_color_read_resource = graph.import_texture(
          "taa-history-color-read",
          texture_resource(rhi::PixelFormat::rgba16_float, rhi::TextureUsage::sampled));
      const auto history_depth_read_resource = graph.import_texture(
          "taa-history-depth-read",
          texture_resource(rhi::PixelFormat::r16_float, rhi::TextureUsage::sampled));
      const auto history_color_write_resource = graph.import_texture(
          "taa-history-color-write",
          texture_resource(rhi::PixelFormat::rgba16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto history_depth_write_resource = graph.import_texture(
          "taa-history-depth-write",
          texture_resource(rhi::PixelFormat::r16_float,
                           rhi::TextureUsage::color_attachment | rhi::TextureUsage::sampled));
      const auto drawable_resource =
          graph.import_texture("swapchain", texture_resource(rhi::PixelFormat::bgra8_srgb,
                                                             rhi::TextureUsage::color_attachment));

      if (view_projection != nullptr && !draw_items.empty()) {
        const auto shadow =
            graph.add_pass("directional-shadow", [this, command, frame_view_projection,
                                                  frame_settings, shadows, &draw_items]() {
              for (std::size_t cascade = 0; cascade < shadow_cascade_count; ++cascade) {
                if (!encode_geometry_pass(command, frame_view_projection, frame_settings, shadows,
                                          draw_items, true, false, cascade))
                  return std::expected<void, std::string>{std::unexpected(
                      "could not encode a cascaded shadow depth pass or allocate a skin palette")};
              }
              return std::expected<void, std::string>{};
            });
        for (const auto resource : shadow_resources) {
          if (!graph.use(shadow, resource, rhi::GraphAccess::write))
            return std::unexpected(
                rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                                 .message = "Invalid cascaded shadow pass resource declaration."});
        }
      }

      const auto sky = graph.add_pass("procedural-sky", [this, command, clear_color, frame_settings,
                                                         current_jitter]() {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = hdr_texture_;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(
            clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha);
        pass.colorAttachments[1].texture = velocity_texture_;
        pass.colorAttachments[1].loadAction = MTLLoadActionClear;
        pass.colorAttachments[1].storeAction = MTLStoreActionStore;
        pass.colorAttachments[1].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        pass.colorAttachments[2].texture = reactive_texture_;
        pass.colorAttachments[2].loadAction = MTLLoadActionClear;
        pass.colorAttachments[2].storeAction = MTLStoreActionStore;
        pass.colorAttachments[2].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        pass.colorAttachments[3].texture = oit_accumulation_texture_;
        pass.colorAttachments[3].loadAction = MTLLoadActionClear;
        pass.colorAttachments[3].storeAction = MTLStoreActionStore;
        pass.colorAttachments[3].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        pass.colorAttachments[4].texture = oit_revealage_texture_;
        pass.colorAttachments[4].loadAction = MTLLoadActionClear;
        pass.colorAttachments[4].storeAction = MTLStoreActionStore;
        pass.colorAttachments[4].clearColor = MTLClearColorMake(1.0, 1.0, 1.0, 1.0);
        pass.colorAttachments[5].texture = indirect_diffuse_texture_;
        pass.colorAttachments[5].loadAction = MTLLoadActionClear;
        pass.colorAttachments[5].storeAction = MTLStoreActionStore;
        pass.colorAttachments[5].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        pass.depthAttachment.texture = depth_texture_;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1.0;
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (encoder == nil)
          return std::expected<void, std::string>{std::unexpected("could not create sky encoder")};
        [encoder setRenderPipelineState:sky_pipeline_];
        SkyUniforms sky_uniforms{};
        std::copy_n(frame_settings.inverse_view_projection.values.data(), 16,
                    sky_uniforms.inverse_view_projection);
        std::copy_n(frame_settings.previous_view_projection.values.data(), 16,
                    sky_uniforms.previous_view_projection);
        sky_uniforms.camera_position[0] = frame_settings.camera_position.x;
        sky_uniforms.camera_position[1] = frame_settings.camera_position.y;
        sky_uniforms.camera_position[2] = frame_settings.camera_position.z;
        sky_uniforms.previous_camera_position[0] = frame_settings.has_previous_view_projection
                                                       ? frame_settings.previous_camera_position.x
                                                       : frame_settings.camera_position.x;
        sky_uniforms.previous_camera_position[1] = frame_settings.has_previous_view_projection
                                                       ? frame_settings.previous_camera_position.y
                                                       : frame_settings.camera_position.y;
        sky_uniforms.previous_camera_position[2] = frame_settings.has_previous_view_projection
                                                       ? frame_settings.previous_camera_position.z
                                                       : frame_settings.camera_position.z;
        sky_uniforms.light_direction[0] = frame_settings.light_direction.x;
        sky_uniforms.light_direction[1] = frame_settings.light_direction.y;
        sky_uniforms.light_direction[2] = frame_settings.light_direction.z;
        sky_uniforms.time_exposure[0] = frame_settings.time_seconds;
        sky_uniforms.time_exposure[1] = frame_settings.exposure;
        sky_uniforms.time_exposure[2] = frame_settings.has_previous_view_projection ? 1.0F : 0.0F;
        sky_uniforms.jitter[0] = current_jitter.x;
        sky_uniforms.jitter[1] = current_jitter.y;
        sky_uniforms.jitter[2] = frame_settings.orthographic_camera ? 1.0F : 0.0F;
        sky_uniforms.clouds[0] = frame_settings.clouds.base_altitude;
        sky_uniforms.clouds[1] = frame_settings.clouds.top_altitude;
        sky_uniforms.clouds[2] = frame_settings.clouds.density;
        sky_uniforms.clouds[3] = frame_settings.clouds.coverage;
        sky_uniforms.cloud_wind_steps[0] = frame_settings.clouds.wind_velocity.x;
        sky_uniforms.cloud_wind_steps[1] = frame_settings.clouds.wind_velocity.y;
        sky_uniforms.cloud_wind_steps[2] = frame_settings.clouds.wind_velocity.z;
        sky_uniforms.cloud_wind_steps[3] = static_cast<float>(frame_settings.clouds.ray_steps);
        sky_uniforms.clouds[2] *= frame_settings.clouds.enabled ? 1.0F : 0.0F;
        const auto* environment =
            dynamic_cast<const MetalEnvironment*>(frame_settings.environment);
        sky_uniforms.environment_flags[0] = environment != nullptr ? 1.0F : 0.0F;
        [encoder setFragmentBytes:&sky_uniforms length:sizeof(SkyUniforms) atIndex:0];
        [encoder setFragmentTexture:environment != nullptr ? environment->environment() : nil
                            atIndex:0];
        [encoder setFragmentSamplerState:ibl_sampler_ atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return std::expected<void, std::string>{};
      });
      if (!graph.use(sky, hdr_resource, rhi::GraphAccess::write) ||
          !graph.use(sky, indirect_diffuse_resource, rhi::GraphAccess::write) ||
          !graph.use(sky, depth_resource, rhi::GraphAccess::write) ||
          !graph.use(sky, velocity_resource, rhi::GraphAccess::write) ||
          !graph.use(sky, reactive_resource, rhi::GraphAccess::write) ||
          !graph.use(sky, oit_accumulation_resource, rhi::GraphAccess::write) ||
          !graph.use(sky, oit_revealage_resource, rhi::GraphAccess::write))
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                             .message = "Invalid sky pass resource declaration."});

      if (view_projection != nullptr && !draw_items.empty()) {
        const auto geometry =
            graph.add_pass("opaque-geometry", [this, command, frame_view_projection, frame_settings,
                                               shadows, &draw_items, fragment_argument_buffer,
                                               point_light_buffer,
                                               fragment_argument_stride,
                                               &fragment_argument_cursor]() {
              if (!encode_geometry_pass(command, frame_view_projection, frame_settings, shadows,
                                        draw_items, false, false, 0, fragment_argument_buffer,
                                        fragment_argument_stride, &fragment_argument_cursor,
                                        point_light_buffer))
                return std::expected<void, std::string>{
                    std::unexpected("could not encode HDR geometry or allocate a skin palette")};
              return std::expected<void, std::string>{};
            });
        bool shadow_reads_valid = true;
        for (const auto resource : shadow_resources)
          shadow_reads_valid =
              shadow_reads_valid && graph.use(geometry, resource, rhi::GraphAccess::read);
        if (!shadow_reads_valid || !graph.use(geometry, hdr_resource, rhi::GraphAccess::write) ||
            !graph.use(geometry, indirect_diffuse_resource, rhi::GraphAccess::write) ||
            !graph.use(geometry, velocity_resource, rhi::GraphAccess::write) ||
            !graph.use(geometry, reactive_resource, rhi::GraphAccess::write) ||
            !graph.use(geometry, depth_resource, rhi::GraphAccess::write))
          return std::unexpected(
              rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                               .message = "Invalid opaque geometry resource declaration."});

        const auto transparent =
            graph.add_pass("weighted-blended-oit", [this, command, frame_view_projection,
                                                     frame_settings, shadows, &draw_items,
                                                     fragment_argument_buffer, point_light_buffer,
                                                     fragment_argument_stride,
                                                     &fragment_argument_cursor]() {
              if (!encode_geometry_pass(command, frame_view_projection, frame_settings, shadows,
                                        draw_items, false, true, 0, fragment_argument_buffer,
                                        fragment_argument_stride, &fragment_argument_cursor,
                                        point_light_buffer))
                return std::expected<void, std::string>{
                    std::unexpected("could not encode weighted blended transparency")};
              return std::expected<void, std::string>{};
            });
        bool transparent_shadow_reads_valid = true;
        for (const auto resource : shadow_resources)
          transparent_shadow_reads_valid = transparent_shadow_reads_valid &&
                                           graph.use(transparent, resource, rhi::GraphAccess::read);
        if (!transparent_shadow_reads_valid ||
            !graph.use(transparent, depth_resource, rhi::GraphAccess::read) ||
            !graph.use(transparent, oit_accumulation_resource, rhi::GraphAccess::write) ||
            !graph.use(transparent, oit_revealage_resource, rhi::GraphAccess::write))
          return std::unexpected(
              rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                               .message = "Invalid weighted blended OIT pass resource declaration."});
      }

      const auto gtao = graph.add_pass(
          "ground-truth-ambient-occlusion",
          [this, command, frame_settings, current_jitter]() {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = gtao_hdr_texture_;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder =
                [command renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil)
              return std::expected<void, std::string>{
                  std::unexpected("could not create GTAO composite encoder")};
            [encoder setRenderPipelineState:gtao_pipeline_];
            [encoder setFragmentTexture:hdr_texture_ atIndex:0];
            [encoder setFragmentTexture:indirect_diffuse_texture_ atIndex:1];
            [encoder setFragmentTexture:depth_texture_ atIndex:2];
            GtaoUniforms uniforms{};
            std::copy_n(frame_settings.inverse_view_projection.values.data(), 16,
                        uniforms.inverse_view_projection);
            uniforms.camera_position[0] = frame_settings.camera_position.x;
            uniforms.camera_position[1] = frame_settings.camera_position.y;
            uniforms.camera_position[2] = frame_settings.camera_position.z;
            uniforms.parameters[0] = frame_settings.gtao.enabled ? frame_settings.gtao.radius : 0.0F;
            uniforms.parameters[1] = frame_settings.gtao.strength;
            uniforms.parameters[2] = frame_settings.gtao.bias;
            uniforms.jitter[0] = current_jitter.x;
            uniforms.jitter[1] = current_jitter.y;
            [encoder setFragmentBytes:&uniforms length:sizeof(GtaoUniforms) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            return std::expected<void, std::string>{};
          });
      if (!graph.use(gtao, hdr_resource, rhi::GraphAccess::read) ||
          !graph.use(gtao, indirect_diffuse_resource, rhi::GraphAccess::read) ||
          !graph.use(gtao, depth_resource, rhi::GraphAccess::read) ||
          !graph.use(gtao, gtao_hdr_resource, rhi::GraphAccess::write))
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                             .message = "Invalid GTAO resource declaration."});

      const auto taa = graph.add_pass(
          "temporal-aa", [this, command, frame_settings, current_jitter, history_write_index]() {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = history_color_[history_write_index];
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[1].texture = history_depth_[history_write_index];
            pass.colorAttachments[1].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[1].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil)
              return std::expected<void, std::string>{
                  std::unexpected("could not create TAA resolve encoder")};
            [encoder setRenderPipelineState:taa_pipeline_];
            [encoder setFragmentTexture:gtao_hdr_texture_ atIndex:0];
            [encoder setFragmentTexture:velocity_texture_ atIndex:1];
            [encoder setFragmentTexture:depth_texture_ atIndex:2];
            [encoder setFragmentTexture:history_color_[history_read_index_] atIndex:3];
            [encoder setFragmentTexture:history_depth_[history_read_index_] atIndex:4];
            [encoder setFragmentTexture:reactive_texture_ atIndex:5];
            [encoder setFragmentTexture:oit_accumulation_texture_ atIndex:6];
            [encoder setFragmentTexture:oit_revealage_texture_ atIndex:7];
            [encoder setFragmentSamplerState:taa_sampler_ atIndex:0];
            TaaUniforms uniforms{};
            std::copy_n(frame_settings.inverse_view_projection.values.data(), 16,
                        uniforms.inverse_view_projection);
            uniforms.camera_position[0] = frame_settings.camera_position.x;
            uniforms.camera_position[1] = frame_settings.camera_position.y;
            uniforms.camera_position[2] = frame_settings.camera_position.z;
            const auto previous_camera_position = frame_settings.has_previous_view_projection
                                                      ? frame_settings.previous_camera_position
                                                      : frame_settings.camera_position;
            uniforms.previous_camera_position[0] = previous_camera_position.x;
            uniforms.previous_camera_position[1] = previous_camera_position.y;
            uniforms.previous_camera_position[2] = previous_camera_position.z;
            uniforms.jitter[0] = current_jitter.x;
            uniforms.jitter[1] = current_jitter.y;
            uniforms.taa_params[1] = frame_settings.has_previous_view_projection ? 1.0F : 0.0F;
            [encoder setFragmentBytes:&uniforms length:sizeof(TaaUniforms) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            return std::expected<void, std::string>{};
          });
      if (!graph.use(taa, gtao_hdr_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, velocity_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, reactive_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, oit_accumulation_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, oit_revealage_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, depth_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, history_color_read_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, history_depth_read_resource, rhi::GraphAccess::read) ||
          !graph.use(taa, history_color_write_resource, rhi::GraphAccess::write) ||
          !graph.use(taa, history_depth_write_resource, rhi::GraphAccess::write))
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                             .message = "Invalid temporal AA resource declaration."});

      const auto post = graph.add_pass(
          "tone-map", [this, command, drawable, frame_settings, history_write_index]() {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil)
              return std::expected<void, std::string>{
                  std::unexpected("could not create tone-map encoder")};
            [encoder setRenderPipelineState:post_pipeline_];
            [encoder setFragmentTexture:history_color_[history_write_index] atIndex:0];
            [encoder setFragmentSamplerState:taa_sampler_ atIndex:0];
            const float exposure = frame_settings.exposure > 0.0F ? frame_settings.exposure : 1.0F;
            const float post_params[4] = {exposure, 0.0F, 0.0F, 0.0F};
            [encoder setFragmentBytes:post_params length:sizeof(post_params) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            return std::expected<void, std::string>{};
          });
      if (!graph.use(post, history_color_write_resource, rhi::GraphAccess::read) ||
          !graph.use(post, drawable_resource, rhi::GraphAccess::write))
        return std::unexpected(
            rhi::RenderError{.code = rhi::RenderErrorCode::command_submission_failed,
                             .message = "Invalid tone-map resource declaration."});

      auto compiled = graph.compile();
      if (!compiled)
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::command_submission_failed,
            .message = "Render graph compilation failed: " + compiled.error().message});
      auto executed = graph.execute(*compiled);
      if (!executed)
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::command_submission_failed,
            .message = "Render graph execution failed: " + executed.error().message});

      [command presentDrawable:drawable];
      [command commit];
      history_read_index_ = history_write_index;
      history_valid_ = view_projection != nullptr;
      if (view_projection != nullptr) {
        previous_time_seconds_ = settings.time_seconds;
        previous_jitter_ = current_jitter;
        ++temporal_frame_index_;
      } else {
        previous_jitter_ = {};
        temporal_frame_index_ = 0;
      }
      return {};
    }
  }

  __strong CAMetalLayer* layer_;
  __strong id<MTLDevice> device_;
  __strong id<MTLCommandQueue> queue_;
  __strong id<MTLRenderPipelineState> scene_pipeline_;
  __strong id<MTLRenderPipelineState> blend_pipeline_;
  __strong id<MTLRenderPipelineState> shadow_pipeline_;
  __strong id<MTLRenderPipelineState> static_scene_pipeline_;
  __strong id<MTLRenderPipelineState> static_blend_pipeline_;
  __strong id<MTLRenderPipelineState> static_shadow_pipeline_;
  __strong id<MTLRenderPipelineState> sky_pipeline_;
  __strong id<MTLRenderPipelineState> post_pipeline_;
  __strong id<MTLRenderPipelineState> taa_pipeline_;
  __strong id<MTLRenderPipelineState> gtao_pipeline_;
  __strong id<MTLArgumentEncoder> scene_fragment_argument_encoder_;
  __strong id<MTLComputePipelineState> virtual_geometry_cull_pipeline_;
  __strong id<MTLComputePipelineState> virtual_geometry_encode_icb_pipeline_;
  __strong id<MTLArgumentEncoder> virtual_geometry_icb_argument_encoder_;
  bool virtual_geometry_icb_supported_{};
  __strong id<MTLComputePipelineState> ibl_equirect_pipeline_;
  __strong id<MTLComputePipelineState> ibl_irradiance_pipeline_;
  __strong id<MTLComputePipelineState> ibl_prefilter_pipeline_;
  __strong id<MTLComputePipelineState> ibl_brdf_pipeline_;
  __strong id<MTLDepthStencilState> depth_state_;
  __strong id<MTLDepthStencilState> transparent_depth_state_;
  __strong id<MTLDepthStencilState> shadow_depth_state_;
  __strong id<MTLSamplerState> sampler_;
  __strong id<MTLSamplerState> taa_sampler_;
  __strong id<MTLSamplerState> shadow_sampler_;
  __strong id<MTLSamplerState> ibl_sampler_;
  __strong id<MTLSamplerState> ibl_equirect_sampler_;
  struct SamplerCacheEntry final {
    std::array<std::uint8_t, 5> key{};
    __strong id<MTLSamplerState> state;
  };
  std::vector<SamplerCacheEntry> sampler_cache_;
  std::vector<SkinBufferCacheEntry> skin_buffer_cache_;
  std::vector<MorphWeightBufferCacheEntry> morph_weight_buffer_cache_;
  std::vector<std::vector<rhi::Matrix4>> instance_cull_scratch_;
  std::size_t instance_cull_scratch_cursor_{};
  std::vector<std::vector<float>> instance_morph_scratch_;
  std::size_t instance_morph_scratch_cursor_{};
  std::array<__strong id<MTLTexture>, shadow_cascade_count> shadow_textures_;
  __strong id<MTLTexture> hdr_texture_;
  __strong id<MTLTexture> indirect_diffuse_texture_;
  __strong id<MTLTexture> gtao_hdr_texture_;
  __strong id<MTLTexture> velocity_texture_;
  __strong id<MTLTexture> reactive_texture_;
  __strong id<MTLTexture> oit_accumulation_texture_;
  __strong id<MTLTexture> oit_revealage_texture_;
  __strong id<MTLTexture> depth_texture_;
  std::array<__strong id<MTLTexture>, 2> history_color_{};
  std::array<__strong id<MTLTexture>, 2> history_depth_{};
  std::size_t history_read_index_{};
  bool history_valid_{};
  float previous_time_seconds_{};
  std::uint64_t temporal_frame_index_{};
  renderer::TemporalJitter previous_jitter_{};
  std::string device_name_;
  std::shared_ptr<std::atomic<std::uint64_t>> gpu_time_nanoseconds_{
      std::make_shared<std::atomic<std::uint64_t>>(0U)};
};

} // namespace

std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
create_metal_render_device(void* native_surface) {
  if (native_surface == nullptr) {
    return std::unexpected(
        rhi::RenderError{.code = rhi::RenderErrorCode::invalid_surface,
                         .message = "A valid native render surface is required."});
  }
  CAMetalLayer* layer = (__bridge CAMetalLayer*)native_surface;
  return MetalRenderDevice::create(layer);
}

} // namespace astra::metal
