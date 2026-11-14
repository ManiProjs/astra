#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace astra::geometry {

struct Vec3 final {
    float x{};
    float y{};
    float z{};
};

struct Aabb final {
    Vec3 minimum{};
    Vec3 maximum{};
};

struct Bounds final {
    Aabb aabb{};
    Vec3 sphere_center{};
    float sphere_radius{};
};

struct BuildLimits final {
    std::uint32_t max_vertices{64};
    std::uint32_t max_triangles{124};
    std::uint32_t max_hierarchy_children{4};
    std::uint32_t max_coarse_vertices{64};
};

enum class BuildErrorCode : std::uint8_t {
    invalid_limits,
    index_count_not_triangular,
    index_out_of_range,
    non_finite_position,
};

struct BuildError final {
    BuildErrorCode code{};
    std::string message;
};

// Cluster indices address the source position array. Triangle indices address
// the cluster's compact vertex_indices array, three entries per triangle.
struct Meshlet final {
    std::vector<std::uint32_t> vertex_indices;
    std::vector<std::uint8_t> triangles;
    Bounds bounds{};
    float geometric_error{};
};

struct ClusterNode final {
    Bounds bounds{};
    // Conservative vertex-displacement estimate introduced by this node's
    // simplification plus the maximum inherited child error.
    float geometric_error{};
    std::vector<std::uint32_t> children;
    // Internal nodes own a renderable, simplified triangle mesh with local
    // uint32 indices. Its vertex count is bounded by BuildLimits::max_coarse_vertices.
    // Leaf nodes continue to reference their source meshlet through the hierarchy index.
    std::vector<Vec3> coarse_vertices;
    std::vector<std::uint32_t> coarse_indices;
    bool leaf{};
};

struct VirtualMesh final {
    std::vector<Meshlet> meshlets;
    // Leaves occupy [0, meshlets.size()); parent nodes follow in level order.
    // Child indices always refer to earlier nodes, so traversal is acyclic.
    std::vector<ClusterNode> hierarchy;
    std::vector<std::uint32_t> roots;
};

struct Camera final {
    Vec3 position{};
    // Vertical field of view in radians and viewport height in physical pixels.
    float vertical_fov_radians{1.04719755F};
    float viewport_height_pixels{1080.0F};
};

struct Selection final {
    // Each selected leaf resolves to its meshlet; each selected internal node
    // resolves to its own coarse_vertices/coarse_indices payload.
    std::vector<std::uint32_t> node_indices;
};

// CPU staging representation matching the RHI virtual-geometry cluster ABI.
// Keeping this independent of the RHI lets geometry builders and tests verify
// hierarchy flattening without requiring a graphics backend.
struct alignas(16) GpuClusterRecord final {
    std::array<float, 4> bounds_sphere{};
    float geometric_error{};
    std::uint32_t parent_index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::int32_t base_vertex{};
    std::uint32_t flags{}; // bit 0: leaf
};
static_assert(sizeof(GpuClusterRecord) == 48);
static_assert(offsetof(GpuClusterRecord, geometric_error) == 16);
static_assert(offsetof(GpuClusterRecord, first_index) == 24);
static_assert(offsetof(GpuClusterRecord, flags) == 36);

struct PackedVirtualMesh final {
    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
    // One record per hierarchy node, preserving node indices and parent links.
    std::vector<GpuClusterRecord> clusters;
};

enum class PackErrorCode : std::uint8_t {
    invalid_hierarchy,
    invalid_geometry,
    capacity_exceeded,
};

struct PackError final {
    PackErrorCode code{};
    std::string message;
};

namespace detail {

using Quadric = std::array<double, 10>;

inline void add_quadric(Quadric& target, const Quadric& source) {
    for (std::size_t i = 0; i < target.size(); ++i)
        target[i] += source[i];
}

inline Quadric plane_quadric(double x, double y, double z, double w) {
    return {x * x, x * y, x * z, x * w, y * y, y * z, y * w, z * z, z * w, w * w};
}

inline double evaluate_quadric(const Quadric& q, Vec3 p) {
    const double x = p.x;
    const double y = p.y;
    const double z = p.z;
    return q[0] * x * x + 2.0 * q[1] * x * y + 2.0 * q[2] * x * z +
           2.0 * q[3] * x + q[4] * y * y + 2.0 * q[5] * y * z +
           2.0 * q[6] * y + q[7] * z * z + 2.0 * q[8] * z + q[9];
}

inline bool solve_quadric_position(const Quadric& q, Vec3& position) {
    const double a00 = q[0], a01 = q[1], a02 = q[2];
    const double a11 = q[4], a12 = q[5], a22 = q[7];
    const double b0 = -q[3], b1 = -q[6], b2 = -q[8];
    const double determinant = a00 * (a11 * a22 - a12 * a12) -
                               a01 * (a01 * a22 - a02 * a12) +
                               a02 * (a01 * a12 - a02 * a11);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12)
        return false;
    const double inverse = 1.0 / determinant;
    const double x = (b0 * (a11 * a22 - a12 * a12) +
                      b1 * (a02 * a12 - a01 * a22) +
                      b2 * (a01 * a12 - a02 * a11)) * inverse;
    const double y = (b0 * (a02 * a12 - a01 * a22) +
                      b1 * (a00 * a22 - a02 * a02) +
                      b2 * (a01 * a02 - a00 * a12)) * inverse;
    const double z = (b0 * (a01 * a12 - a02 * a11) +
                      b1 * (a01 * a02 - a00 * a12) +
                      b2 * (a00 * a11 - a01 * a01)) * inverse;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    position = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    return true;
}

inline std::vector<Quadric> calculate_quadrics(std::span<const Vec3> vertices,
                                               std::span<const std::uint32_t> indices) {
    std::vector<Quadric> quadrics(vertices.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto a = vertices[indices[i]];
        const auto b = vertices[indices[i + 1]];
        const auto c = vertices[indices[i + 2]];
        const double ab_x = static_cast<double>(b.x) - a.x;
        const double ab_y = static_cast<double>(b.y) - a.y;
        const double ab_z = static_cast<double>(b.z) - a.z;
        const double ac_x = static_cast<double>(c.x) - a.x;
        const double ac_y = static_cast<double>(c.y) - a.y;
        const double ac_z = static_cast<double>(c.z) - a.z;
        double nx = ab_y * ac_z - ab_z * ac_y;
        double ny = ab_z * ac_x - ab_x * ac_z;
        double nz = ab_x * ac_y - ab_y * ac_x;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!std::isfinite(length) || length < 1.0e-12)
            continue;
        nx /= length;
        ny /= length;
        nz /= length;
        const double w = -(nx * a.x + ny * a.y + nz * a.z);
        const auto plane = plane_quadric(nx, ny, nz, w);
        add_quadric(quadrics[indices[i]], plane);
        add_quadric(quadrics[indices[i + 1]], plane);
        add_quadric(quadrics[indices[i + 2]], plane);
    }
    return quadrics;
}

inline float distance_point_triangle_squared(Vec3 point, Vec3 a, Vec3 b, Vec3 c) {
    const auto dot = [](Vec3 lhs, Vec3 rhs) {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    };
    const auto subtract = [](Vec3 lhs, Vec3 rhs) {
        return Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
    };
    const auto add_scaled = [](Vec3 origin, Vec3 direction, float scale) {
        return Vec3{origin.x + direction.x * scale, origin.y + direction.y * scale,
                    origin.z + direction.z * scale};
    };
    const auto distance_squared_local = [&dot, &subtract](Vec3 lhs, Vec3 rhs) {
        const auto delta = subtract(lhs, rhs);
        return dot(delta, delta);
    };
    const Vec3 ab = subtract(b, a);
    const Vec3 ac = subtract(c, a);
    const Vec3 ap = subtract(point, a);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F)
        return distance_squared_local(point, a);

    const Vec3 bp = subtract(point, b);
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3)
        return distance_squared_local(point, b);

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const float v = d1 / (d1 - d3);
        return distance_squared_local(point, add_scaled(a, ab, v));
    }

    const Vec3 cp = subtract(point, c);
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6)
        return distance_squared_local(point, c);

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const float w = d2 / (d2 - d6);
        return distance_squared_local(point, add_scaled(a, ac, w));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return distance_squared_local(point, add_scaled(b, subtract(c, b), w));
    }

    const float denominator = 1.0F / (va + vb + vc);
    const float v = vb * denominator;
    const float w = vc * denominator;
    return distance_squared_local(point, add_scaled(add_scaled(a, ab, v), ac, w));
}

// Deterministic edge-collapse quadric-error simplification. Each collapse
// preserves triangle connectivity, removes degenerate/duplicate faces, and
// places the surviving vertex at the minimum combined plane-error position
// (or the edge midpoint for singular quadrics). This operates on positions;
// callers that need normals/UVs must rebuild those attributes from the result.
[[nodiscard]] inline std::pair<std::vector<Vec3>, std::vector<std::uint32_t>>
simplify_quadric_error(std::span<const Vec3> source_vertices,
                       std::span<const std::uint32_t> source_indices,
                       std::uint32_t maximum_vertices) {
    if (source_indices.empty() || maximum_vertices < 3)
        return {};
    std::vector<Vec3> working_vertices(source_vertices.begin(), source_vertices.end());
    std::vector<std::uint32_t> working_indices(source_indices.begin(), source_indices.end());
    std::vector<bool> active(working_vertices.size(), false);
    std::size_t active_count = 0;
    for (const auto index : working_indices) {
        if (!active[index]) {
            active[index] = true;
            ++active_count;
        }
    }

    // Internal hierarchy nodes should actually represent a coarser cut even
    // when the configured hard cap is generous. Target roughly half the active
    // vertices, then honor the caller's stricter cap if one is supplied.
    const std::size_t target_vertices =
        std::min<std::size_t>(maximum_vertices, std::max<std::size_t>(3, active_count / 2U));
    const std::size_t target_triangles = std::max<std::size_t>(1, working_indices.size() / 6U);
    while (active_count > target_vertices) {
        if (active_count <= maximum_vertices && working_indices.size() / 3U <= target_triangles)
            break;
        const auto quadrics = calculate_quadrics(working_vertices, working_indices);
        std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
        for (std::size_t i = 0; i + 2 < working_indices.size(); i += 3) {
            const std::array<std::uint32_t, 3> triangle{
                working_indices[i], working_indices[i + 1], working_indices[i + 2]};
            for (std::size_t edge = 0; edge < 3; ++edge) {
                auto a = triangle[edge];
                auto b = triangle[(edge + 1) % 3];
                if (a > b)
                    std::swap(a, b);
                if (a != b)
                    edges.emplace(a, b);
            }
        }
        double best_cost = std::numeric_limits<double>::infinity();
        std::uint32_t best_keep = 0;
        std::uint32_t best_remove = 0;
        Vec3 best_position{};
        for (const auto [a, b] : edges) {
            Quadric combined = quadrics[a];
            add_quadric(combined, quadrics[b]);
            Vec3 candidate{};
            if (!solve_quadric_position(combined, candidate)) {
                candidate = {(working_vertices[a].x + working_vertices[b].x) * 0.5F,
                             (working_vertices[a].y + working_vertices[b].y) * 0.5F,
                             (working_vertices[a].z + working_vertices[b].z) * 0.5F};
            }
            const double cost = std::max(0.0, evaluate_quadric(combined, candidate));
            if (std::isfinite(cost) && cost < best_cost) {
                best_cost = cost;
                best_keep = a;
                best_remove = b;
                best_position = candidate;
            }
        }
        if (!std::isfinite(best_cost))
            break;

        working_vertices[best_keep] = best_position;
        active[best_remove] = false;
        --active_count;
        std::vector<std::uint32_t> collapsed;
        collapsed.reserve(working_indices.size());
        std::set<std::array<std::uint32_t, 3>> unique_triangles;
        for (std::size_t i = 0; i + 2 < working_indices.size(); i += 3) {
            auto a = working_indices[i] == best_remove ? best_keep : working_indices[i];
            auto b = working_indices[i + 1] == best_remove ? best_keep : working_indices[i + 1];
            auto c = working_indices[i + 2] == best_remove ? best_keep : working_indices[i + 2];
            if (a == b || b == c || a == c)
                continue;
            auto key = std::array<std::uint32_t, 3>{a, b, c};
            std::sort(key.begin(), key.end());
            if (!unique_triangles.insert(key).second)
                continue;
            collapsed.insert(collapsed.end(), {a, b, c});
        }
        working_indices = std::move(collapsed);
        if (working_indices.empty())
            break;
    }

    std::vector<std::uint32_t> remap(working_vertices.size(),
                                     std::numeric_limits<std::uint32_t>::max());
    std::vector<Vec3> simplified_vertices;
    simplified_vertices.reserve(active_count);
    for (std::size_t i = 0; i < active.size(); ++i) {
        if (!active[i])
            continue;
        remap[i] = static_cast<std::uint32_t>(simplified_vertices.size());
        simplified_vertices.push_back(working_vertices[i]);
    }
    std::vector<std::uint32_t> simplified_indices;
    simplified_indices.reserve(working_indices.size());
    for (const auto index : working_indices)
        simplified_indices.push_back(remap[index]);
    if (simplified_indices.empty()) {
        // Preserve one source face if a pathological disconnected/degenerate
        // input cannot retain a renderable result after edge collapse.
        const auto first = source_indices[0];
        const auto second = source_indices[1];
        const auto third = source_indices[2];
        simplified_vertices = {source_vertices[first], source_vertices[second],
                               source_vertices[third]};
        simplified_indices = {0U, 1U, 2U};
    }
    return {std::move(simplified_vertices), std::move(simplified_indices)};
}

} // namespace detail

[[nodiscard]] inline float distance_squared(Vec3 a, Vec3 b) noexcept {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

[[nodiscard]] inline Bounds calculate_bounds(std::span<const Vec3> positions,
                                             std::span<const std::uint32_t> indices) {
    Bounds result;
    const float infinity = std::numeric_limits<float>::infinity();
    result.aabb.minimum = {infinity, infinity, infinity};
    result.aabb.maximum = {-infinity, -infinity, -infinity};
    for (const auto index : indices) {
        const auto point = positions[index];
        result.aabb.minimum.x = std::min(result.aabb.minimum.x, point.x);
        result.aabb.minimum.y = std::min(result.aabb.minimum.y, point.y);
        result.aabb.minimum.z = std::min(result.aabb.minimum.z, point.z);
        result.aabb.maximum.x = std::max(result.aabb.maximum.x, point.x);
        result.aabb.maximum.y = std::max(result.aabb.maximum.y, point.y);
        result.aabb.maximum.z = std::max(result.aabb.maximum.z, point.z);
    }
    result.sphere_center = {
        (result.aabb.minimum.x + result.aabb.maximum.x) * 0.5F,
        (result.aabb.minimum.y + result.aabb.maximum.y) * 0.5F,
        (result.aabb.minimum.z + result.aabb.maximum.z) * 0.5F,
    };
    for (const auto index : indices)
        result.sphere_radius =
            std::max(result.sphere_radius,
                     std::sqrt(distance_squared(result.sphere_center, positions[index])));
    return result;
}

[[nodiscard]] inline std::expected<VirtualMesh, BuildError>
build_virtual_mesh(std::span<const Vec3> positions, std::span<const std::uint32_t> indices,
                   BuildLimits limits = {}) {
    if (limits.max_vertices < 3 || limits.max_vertices > 256 || limits.max_triangles == 0 ||
        limits.max_hierarchy_children < 2 || limits.max_coarse_vertices < 3)
        return std::unexpected(BuildError{
            BuildErrorCode::invalid_limits,
            "meshlet limits must be positive, max_vertices <= 256, and max_coarse_vertices >= 3"});
    if (indices.size() % 3 != 0)
        return std::unexpected(BuildError{BuildErrorCode::index_count_not_triangular,
                                          "index count must be divisible by three"});
    for (const auto point : positions) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            return std::unexpected(BuildError{BuildErrorCode::non_finite_position,
                                              "positions must contain finite coordinates"});
    }
    for (const auto index : indices) {
        if (index >= positions.size())
            return std::unexpected(BuildError{BuildErrorCode::index_out_of_range,
                                              "triangle index exceeds the position array"});
    }

    VirtualMesh mesh;
    Meshlet current;
    std::map<std::uint32_t, std::uint8_t> local_indices;
    const auto flush = [&] {
        if (current.triangles.empty())
            return;
        current.bounds = calculate_bounds(positions, current.vertex_indices);
        current.geometric_error = 0.0F;
        mesh.meshlets.push_back(std::move(current));
        current = Meshlet{};
        local_indices.clear();
    };

    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3) {
        std::array<std::uint32_t, 3> triangle_indices{indices[triangle], indices[triangle + 1],
                                                      indices[triangle + 2]};
        std::size_t new_vertices = 0;
        for (const auto index : triangle_indices)
            if (!local_indices.contains(index))
                ++new_vertices;
        // Count distinct new vertices within this triangle as well.
        for (std::size_t i = 0; i < triangle_indices.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (triangle_indices[i] == triangle_indices[j] &&
                    !local_indices.contains(triangle_indices[i]))
                    --new_vertices;
        if (!current.triangles.empty() &&
            (current.triangles.size() / 3 >= limits.max_triangles ||
             current.vertex_indices.size() + new_vertices > limits.max_vertices))
            flush();

        for (const auto index : triangle_indices) {
            auto found = local_indices.find(index);
            if (found == local_indices.end()) {
                const auto local = static_cast<std::uint8_t>(current.vertex_indices.size());
                current.vertex_indices.push_back(index);
                found = local_indices.emplace(index, local).first;
            }
            current.triangles.push_back(found->second);
        }
    }
    flush();

    mesh.hierarchy.reserve(mesh.meshlets.size());
    for (const auto& meshlet : mesh.meshlets)
        mesh.hierarchy.push_back({meshlet.bounds, meshlet.geometric_error, {}, {}, {}, true});

    std::vector<std::uint32_t> level;
    level.reserve(mesh.meshlets.size());
    for (std::uint32_t i = 0; i < mesh.meshlets.size(); ++i)
        level.push_back(i);
    while (level.size() > 1) {
        std::vector<std::uint32_t> parents;
        for (std::size_t first = 0; first < level.size(); first += limits.max_hierarchy_children) {
            const auto last = std::min(level.size(), first + limits.max_hierarchy_children);
            ClusterNode parent;
            parent.leaf = false;
            const float infinity = std::numeric_limits<float>::infinity();
            parent.bounds.aabb.minimum = {infinity, infinity, infinity};
            parent.bounds.aabb.maximum = {-infinity, -infinity, -infinity};
            for (std::size_t child = first; child < last; ++child) {
                parent.children.push_back(level[child]);
                const auto& bounds = mesh.hierarchy[level[child]].bounds;
                const auto include = [&](Vec3 point) {
                    parent.bounds.aabb.minimum.x = std::min(parent.bounds.aabb.minimum.x, point.x);
                    parent.bounds.aabb.minimum.y = std::min(parent.bounds.aabb.minimum.y, point.y);
                    parent.bounds.aabb.minimum.z = std::min(parent.bounds.aabb.minimum.z, point.z);
                    parent.bounds.aabb.maximum.x = std::max(parent.bounds.aabb.maximum.x, point.x);
                    parent.bounds.aabb.maximum.y = std::max(parent.bounds.aabb.maximum.y, point.y);
                    parent.bounds.aabb.maximum.z = std::max(parent.bounds.aabb.maximum.z, point.z);
                };
                include(bounds.aabb.minimum);
                include(bounds.aabb.maximum);
            }
            const auto& box = parent.bounds.aabb;
            parent.bounds.sphere_center = {
                (box.minimum.x + box.maximum.x) * 0.5F,
                (box.minimum.y + box.maximum.y) * 0.5F,
                (box.minimum.z + box.maximum.z) * 0.5F,
            };
            for (const auto child : parent.children) {
                const auto& node = mesh.hierarchy[child];
                parent.bounds.sphere_radius =
                    std::max(parent.bounds.sphere_radius,
                             std::sqrt(distance_squared(parent.bounds.sphere_center,
                                                        node.bounds.sphere_center)) +
                                 node.bounds.sphere_radius);
                parent.geometric_error = std::max(parent.geometric_error, node.geometric_error);
            }
            std::vector<Vec3> source_vertices;
            std::vector<std::uint32_t> source_indices;
            for (const auto child : parent.children) {
                const auto& node = mesh.hierarchy[child];
                if (node.leaf) {
                    const auto& meshlet = mesh.meshlets[child];
                    const auto base = static_cast<std::uint32_t>(source_vertices.size());
                    for (const auto source_index : meshlet.vertex_indices)
                        source_vertices.push_back(positions[source_index]);
                    for (const auto local_index : meshlet.triangles)
                        source_indices.push_back(base + local_index);
                } else {
                    const auto base = static_cast<std::uint32_t>(source_vertices.size());
                    source_vertices.insert(source_vertices.end(), node.coarse_vertices.begin(),
                                           node.coarse_vertices.end());
                    for (const auto local_index : node.coarse_indices)
                        source_indices.push_back(base + local_index);
                }
            }
            auto coarse = detail::simplify_quadric_error(source_vertices, source_indices,
                                                         limits.max_coarse_vertices);
            parent.coarse_vertices = std::move(coarse.first);
            parent.coarse_indices = std::move(coarse.second);
            // Sample the source vertices against the simplified triangle surface.
            // This is a useful one-sided geometric bound estimate, not a full
            // continuous Hausdorff-distance calculation.
            float simplification_error = 0.0F;
            for (const auto source : source_vertices) {
                float nearest_squared = std::numeric_limits<float>::infinity();
                for (std::size_t triangle = 0; triangle + 2 < parent.coarse_indices.size();
                     triangle += 3) {
                    nearest_squared = std::min(
                        nearest_squared,
                        detail::distance_point_triangle_squared(
                            source, parent.coarse_vertices[parent.coarse_indices[triangle]],
                            parent.coarse_vertices[parent.coarse_indices[triangle + 1]],
                            parent.coarse_vertices[parent.coarse_indices[triangle + 2]]));
                }
                simplification_error = std::max(simplification_error,
                                                std::sqrt(nearest_squared));
            }
            parent.geometric_error += simplification_error;
            const auto parent_index = static_cast<std::uint32_t>(mesh.hierarchy.size());
            mesh.hierarchy.push_back(std::move(parent));
            parents.push_back(parent_index);
        }
        level = std::move(parents);
    }
    mesh.roots = std::move(level);
    return mesh;
}

// Returns projected geometric error in pixels. Invalid camera parameters yield
// infinity, which conservatively requests refinement to leaf meshlets.
[[nodiscard]] inline float screen_space_error(const ClusterNode& node, const Camera& camera,
                                              float near_plane = 0.01F) noexcept {
    if (!std::isfinite(camera.vertical_fov_radians) || camera.vertical_fov_radians <= 0.0F ||
        camera.vertical_fov_radians >= 3.14159265F ||
        !std::isfinite(camera.viewport_height_pixels) || camera.viewport_height_pixels <= 0.0F ||
        !std::isfinite(near_plane) || near_plane <= 0.0F || !std::isfinite(camera.position.x) ||
        !std::isfinite(camera.position.y) || !std::isfinite(camera.position.z))
        return std::numeric_limits<float>::infinity();
    const float distance = std::sqrt(distance_squared(camera.position, node.bounds.sphere_center));
    const float depth = std::max(near_plane, distance - node.bounds.sphere_radius);
    const float projection =
        camera.viewport_height_pixels / (2.0F * std::tan(camera.vertical_fov_radians * 0.5F));
    return node.geometric_error * projection / depth;
}

// Traverses from roots. A node is emitted when its projected error is within
// the pixel budget; otherwise its children are visited. A leaf is always emitted.
[[nodiscard]] inline Selection select_clusters(const VirtualMesh& mesh, const Camera& camera,
                                               float maximum_error_pixels = 1.0F) {
    Selection result;
    if (!std::isfinite(maximum_error_pixels) || maximum_error_pixels < 0.0F)
        maximum_error_pixels = 0.0F;
    std::vector<std::uint32_t> pending(mesh.roots.rbegin(), mesh.roots.rend());
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index >= mesh.hierarchy.size())
            continue;
        const auto& node = mesh.hierarchy[index];
        if (node.leaf || screen_space_error(node, camera) <= maximum_error_pixels) {
            result.node_indices.push_back(index);
            continue;
        }
        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
            pending.push_back(*child);
    }
    return result;
}

// Flattens every hierarchy node into persistent GPU-ready vertex/index ranges.
// The compute shader evaluates projected error and parent error to choose a
// non-overlapping cut, then performs frustum culling and writes indirect args.
[[nodiscard]] inline std::expected<PackedVirtualMesh, PackError>
pack_virtual_mesh(const VirtualMesh& mesh, std::span<const Vec3> source_positions) {
    constexpr auto no_parent = std::numeric_limits<std::uint32_t>::max();
    if (mesh.hierarchy.size() > no_parent)
        return std::unexpected(PackError{PackErrorCode::capacity_exceeded,
                                         "Virtual geometry has too many hierarchy nodes."});

    PackedVirtualMesh packed;
    packed.clusters.resize(mesh.hierarchy.size());
    std::vector<std::uint32_t> parents(mesh.hierarchy.size(), no_parent);
    for (std::size_t parent = 0; parent < mesh.hierarchy.size(); ++parent) {
        for (const auto child : mesh.hierarchy[parent].children) {
            if (child >= mesh.hierarchy.size() || child == parent || parents[child] != no_parent)
                return std::unexpected(
                    PackError{PackErrorCode::invalid_hierarchy,
                              "Virtual geometry contains an invalid or multiply-owned child."});
            parents[child] = static_cast<std::uint32_t>(parent);
        }
    }
    std::vector<bool> root_seen(mesh.hierarchy.size(), false);
    for (const auto root : mesh.roots) {
        if (root >= mesh.hierarchy.size() || parents[root] != no_parent || root_seen[root])
            return std::unexpected(
                PackError{PackErrorCode::invalid_hierarchy,
                          "Virtual geometry roots are invalid or have parents."});
        root_seen[root] = true;
    }
    for (std::size_t node = 0; node < mesh.hierarchy.size(); ++node) {
        if ((parents[node] == no_parent) != root_seen[node])
            return std::unexpected(
                PackError{PackErrorCode::invalid_hierarchy,
                          "Virtual geometry roots do not cover every parentless node."});
    }

    for (std::size_t node_index = 0; node_index < mesh.hierarchy.size(); ++node_index) {
        const auto& node = mesh.hierarchy[node_index];
        auto& record = packed.clusters[node_index];
        const auto center = node.bounds.sphere_center;
        const auto radius = node.bounds.sphere_radius;
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
            !std::isfinite(radius) || radius < 0.0F || !std::isfinite(node.geometric_error) ||
            node.geometric_error < 0.0F)
            return std::unexpected(
                PackError{PackErrorCode::invalid_geometry,
                          "Virtual geometry contains invalid bounds or geometric error."});
        if (packed.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
            packed.indices.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(
                PackError{PackErrorCode::capacity_exceeded,
                          "Virtual geometry exceeds indirect draw address limits."});

        const auto base_vertex = static_cast<std::uint32_t>(packed.vertices.size());
        const auto first_index = static_cast<std::uint32_t>(packed.indices.size());
        if (node.leaf) {
            if (node_index >= mesh.meshlets.size())
                return std::unexpected(
                    PackError{PackErrorCode::invalid_hierarchy,
                              "A virtual geometry leaf has no matching meshlet."});
            const auto& meshlet = mesh.meshlets[node_index];
            if (meshlet.triangles.size() % 3U != 0U)
                return std::unexpected(
                    PackError{PackErrorCode::invalid_geometry,
                              "A virtual geometry meshlet has a non-triangular index count."});
            for (const auto source_index : meshlet.vertex_indices) {
                if (source_index >= source_positions.size())
                    return std::unexpected(PackError{
                        PackErrorCode::invalid_geometry,
                        "A virtual geometry meshlet references a missing source vertex."});
                const auto position = source_positions[source_index];
                if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                    !std::isfinite(position.z))
                    return std::unexpected(
                        PackError{PackErrorCode::invalid_geometry,
                                  "A virtual geometry source vertex is non-finite."});
                packed.vertices.push_back(position);
            }
            for (const auto local_index : meshlet.triangles) {
                if (local_index >= meshlet.vertex_indices.size())
                    return std::unexpected(
                        PackError{PackErrorCode::invalid_geometry,
                                  "A virtual geometry meshlet index is out of range."});
                packed.indices.push_back(local_index);
            }
        } else {
            if (node.coarse_indices.empty() || node.coarse_indices.size() % 3U != 0U)
                return std::unexpected(
                    PackError{PackErrorCode::invalid_geometry,
                              "An internal virtual geometry node has no valid coarse triangles."});
            for (const auto position : node.coarse_vertices) {
                if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                    !std::isfinite(position.z))
                    return std::unexpected(
                        PackError{PackErrorCode::invalid_geometry,
                                  "A virtual geometry coarse vertex is non-finite."});
                packed.vertices.push_back(position);
            }
            for (const auto local_index : node.coarse_indices) {
                if (local_index >= node.coarse_vertices.size())
                    return std::unexpected(
                        PackError{PackErrorCode::invalid_geometry,
                                  "A virtual geometry coarse index is out of range."});
                packed.indices.push_back(local_index);
            }
        }
        if (packed.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
            packed.indices.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(
                PackError{PackErrorCode::capacity_exceeded,
                          "Virtual geometry exceeds indirect draw address limits."});
        record.bounds_sphere = {center.x, center.y, center.z, radius};
        record.geometric_error = node.geometric_error;
        record.parent_index = parents[node_index];
        record.first_index = first_index;
        record.index_count = static_cast<std::uint32_t>(packed.indices.size()) - first_index;
        record.base_vertex = static_cast<std::int32_t>(base_vertex);
        record.flags = node.leaf ? 1U : 0U;
    }
    return packed;
}

} // namespace astra::geometry
