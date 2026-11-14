#include <astra/geometry/virtual_geometry.hpp>
#include <astra/geometry/virtual_geometry_streaming.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

int main() {
    using namespace astra::geometry;

    {
        const std::vector<Vec3> positions{
            {10.0F, 0.0F, 0.0F}, {11.0F, 0.0F, 0.0F}, {10.0F, 1.0F, 0.0F},
            {11.0F, 1.0F, 0.0F}, {12.0F, 0.0F, 0.0F}, {12.0F, 1.0F, 0.0F},
        };
        const std::vector<std::uint32_t> indices{0, 1, 2, 1, 3, 2, 1, 4, 3, 4, 5, 3};
        const auto built = build_virtual_mesh(positions, indices, {3, 1, 2, 3});
        assert(built);
        const auto& mesh = *built;
        assert(mesh.meshlets.size() == 4);
        assert(mesh.hierarchy.size() > mesh.meshlets.size());
        assert(mesh.roots.size() == 1);
        const auto packed = pack_virtual_mesh(mesh, positions);
        assert(packed);
        assert(packed->clusters.size() == mesh.hierarchy.size());
        for (std::size_t i = 0; i < packed->clusters.size(); ++i) {
            const auto& cluster = packed->clusters[i];
            const auto& node = mesh.hierarchy[i];
            assert(cluster.index_count > 0 && cluster.index_count % 3U == 0U);
            assert(cluster.first_index + cluster.index_count <= packed->indices.size());
            assert(cluster.base_vertex >= 0);
            assert((cluster.flags & 1U) == (node.leaf ? 1U : 0U));
            for (std::size_t index = cluster.first_index;
                 index < static_cast<std::size_t>(cluster.first_index) + cluster.index_count;
                 ++index)
                assert(static_cast<std::size_t>(cluster.base_vertex) + packed->indices[index] <
                       packed->vertices.size());
        }
        for (const auto root : mesh.roots)
            assert(packed->clusters[root].parent_index ==
                   std::numeric_limits<std::uint32_t>::max());
        for (std::size_t parent = 0; parent < mesh.hierarchy.size(); ++parent) {
            for (const auto child : mesh.hierarchy[parent].children)
                assert(packed->clusters[child].parent_index == parent);
            if (mesh.hierarchy[parent].leaf)
                assert((packed->clusters[parent].flags & 1U) != 0U);
            else
                assert((packed->clusters[parent].flags & 1U) == 0U);
        }
        auto invalid_parent_mesh = mesh;
        const auto owned_child = mesh.hierarchy[mesh.roots.front()].children.front();
        invalid_parent_mesh.hierarchy[0].children.push_back(owned_child);
        const auto invalid_parent_pack = pack_virtual_mesh(invalid_parent_mesh, positions);
        assert(!invalid_parent_pack);
        assert(invalid_parent_pack.error().code == PackErrorCode::invalid_hierarchy);
        auto missing_root_mesh = mesh;
        missing_root_mesh.roots.clear();
        assert(!pack_virtual_mesh(missing_root_mesh, positions));
        VirtualMesh cyclic_mesh;
        cyclic_mesh.hierarchy.resize(2);
        cyclic_mesh.hierarchy[0].children.push_back(1);
        cyclic_mesh.hierarchy[1].children.push_back(0);
        assert(!pack_virtual_mesh(cyclic_mesh, positions));
        for (const auto& meshlet : mesh.meshlets) {
            assert(meshlet.vertex_indices.size() <= 3);
            assert(meshlet.triangles.size() / 3 <= 1);
            assert(meshlet.triangles.size() % 3 == 0);
            assert(meshlet.geometric_error >= 0.0F);
            assert(meshlet.bounds.aabb.minimum.x >= 10.0F);
            for (const auto local : meshlet.triangles)
                assert(local < meshlet.vertex_indices.size());
        }
        const auto again = build_virtual_mesh(positions, indices, {3, 1, 2, 3});
        assert(again);
        assert(again->meshlets.size() == mesh.meshlets.size());
        for (std::size_t i = 0; i < mesh.meshlets.size(); ++i) {
            assert(again->meshlets[i].vertex_indices == mesh.meshlets[i].vertex_indices);
            assert(again->meshlets[i].triangles == mesh.meshlets[i].triangles);
        }

        const Camera near_camera{{10.5F, 0.5F, 2.0F}, 1.04719755F, 1080.0F};
        const Camera far_camera{{10.5F, 0.5F, 10000.0F}, 1.04719755F, 1080.0F};
        const auto detailed = select_clusters(mesh, near_camera, 1.0F);
        const auto coarse = select_clusters(mesh, far_camera, 1.0F);
        assert(detailed.node_indices.size() == mesh.meshlets.size());
        assert(coarse.node_indices.size() <= detailed.node_indices.size());
        assert(coarse.node_indices.size() == 1);
        const auto& far_node = mesh.hierarchy[coarse.node_indices.front()];
        assert(!far_node.leaf);
        assert(!far_node.coarse_vertices.empty());
        assert(!far_node.coarse_indices.empty());
        assert(far_node.coarse_indices.size() % 3 == 0);
        for (const auto index : far_node.coarse_indices)
            assert(index < far_node.coarse_vertices.size());
        for (const auto selected : detailed.node_indices) {
            const auto& leaf = mesh.hierarchy[selected];
            assert(leaf.leaf);
            assert(leaf.coarse_vertices.empty());
            assert(leaf.coarse_indices.empty());
        }

        const auto bounded = build_virtual_mesh(positions, indices, {3, 1, 2, 3});
        assert(bounded);
        for (std::size_t i = bounded->meshlets.size(); i < bounded->hierarchy.size(); ++i) {
            const auto& parent = bounded->hierarchy[i];
            assert(parent.coarse_vertices.size() <= 3);
            assert(!parent.coarse_vertices.empty());
            assert(!parent.coarse_indices.empty());
            assert(parent.coarse_indices.size() % 3 == 0);
            for (const auto index : parent.coarse_indices)
                assert(index < parent.coarse_vertices.size());
        }
        const auto bounded_again = build_virtual_mesh(positions, indices, {3, 1, 2, 3});
        assert(bounded_again);
        assert(bounded_again->hierarchy.size() == bounded->hierarchy.size());
        for (std::size_t i = bounded->meshlets.size(); i < bounded->hierarchy.size(); ++i) {
            assert(bounded_again->hierarchy[i].coarse_indices ==
                   bounded->hierarchy[i].coarse_indices);
            assert(bounded_again->hierarchy[i].coarse_vertices.size() ==
                   bounded->hierarchy[i].coarse_vertices.size());
            for (std::size_t vertex = 0; vertex < bounded->hierarchy[i].coarse_vertices.size();
                 ++vertex) {
                const auto a = bounded_again->hierarchy[i].coarse_vertices[vertex];
                const auto b = bounded->hierarchy[i].coarse_vertices[vertex];
                assert(a.x == b.x && a.y == b.y && a.z == b.z);
            }
        }
    }

    {
        const std::vector<Vec3> positions{
            {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
        const std::vector<std::uint32_t> triangle{0, 1, 2};
        assert(!build_virtual_mesh(positions, triangle, {2, 1, 2}));
        assert(!build_virtual_mesh(positions, triangle, {3, 1, 2, 2}));
        assert(!build_virtual_mesh(positions, std::vector<std::uint32_t>{0, 1}));
        assert(!build_virtual_mesh(positions, std::vector<std::uint32_t>{0, 1, 3}));
        const std::vector<Vec3> invalid{{0.0F, 0.0F, 0.0F},
                                        {std::numeric_limits<float>::infinity(), 0.0F, 0.0F},
                                        {0.0F, 1.0F, 0.0F}};
        assert(!build_virtual_mesh(invalid, triangle));
        const auto empty = build_virtual_mesh(positions, std::span<const std::uint32_t>{});
        assert(empty && empty->meshlets.empty() && empty->roots.empty());
    }

    {
        const std::vector<Vec3> positions{
            {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
        const std::vector<std::uint32_t> triangle{0, 1, 2};
        const auto mesh = build_virtual_mesh(positions, triangle);
        assert(mesh && mesh->hierarchy.size() == 1);
        const Camera invalid_camera{{}, 0.0F, 1080.0F};
        assert(std::isinf(screen_space_error(mesh->hierarchy[0], invalid_camera)));
        assert(select_clusters(*mesh, invalid_camera).node_indices.size() == 1);
    }

    {
        std::vector<Vec3> positions;
        std::vector<std::uint32_t> indices;
        for (std::uint32_t z = 0; z <= 32; ++z) {
            for (std::uint32_t x = 0; x <= 32; ++x) {
                positions.push_back({static_cast<float>(x) * 2.0F,
                                     std::sin(static_cast<float>(x) * 0.12F) * 1.4F +
                                         std::cos(static_cast<float>(z) * 0.1F),
                                     static_cast<float>(z) * 2.0F});
            }
        }
        for (std::uint32_t z = 0; z < 32; ++z) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const auto a = z * 33U + x;
                const auto b = a + 1U;
                const auto c = a + 33U;
                const auto d = c + 1U;
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }
        const auto terrain = build_virtual_mesh(positions, indices, {64, 124, 4, 256});
        assert(terrain && terrain->meshlets.size() > 10);
        const Camera near_camera{{32.0F, 3.0F, -12.0F}, 1.22173F, 900.0F};
        const Camera far_camera{{32.0F, 3.0F, 220.0F}, 1.22173F, 900.0F};
        const auto detailed = select_clusters(*terrain, near_camera, 0.8F);
        const auto distant = select_clusters(*terrain, far_camera, 16.0F);
        assert(detailed.node_indices.size() == terrain->meshlets.size());
        assert(distant.node_indices.size() < detailed.node_indices.size());
        for (const auto selected : distant.node_indices) {
            const auto& node = terrain->hierarchy[selected];
            if (!node.leaf) {
                assert(!node.coarse_vertices.empty());
                assert(!node.coarse_indices.empty());
            }
        }
    }

    {
        VirtualGeometryPageCache cache{200};
        constexpr auto root = VirtualGeometryPageId{100};
        constexpr auto child = VirtualGeometryPageId{101};
        constexpr auto grandchild = VirtualGeometryPageId{102};
        constexpr auto sibling = VirtualGeometryPageId{103};
        assert(cache.register_page({root, invalid_virtual_geometry_page, 100}));
        assert(cache.register_page({child, root, 80}));
        assert(cache.register_page({grandchild, child, 50}));
        assert(cache.register_page({sibling, root, 60}));
        assert(!cache.register_page({child, root, 80}));

        assert(cache.request_page(grandchild, 1, 10.0F));
        const auto root_request = cache.take_requests(1);
        assert(root_request.size() == 1 && root_request.front().id == root);
        assert(cache.mark_resident(root, 1));
        const auto child_requests = cache.take_requests(8);
        assert(child_requests.size() == 2);
        assert(child_requests[0].id == child && child_requests[1].id == grandchild);
        assert(!cache.mark_resident(grandchild, 2));
        assert(cache.mark_resident(child, 2));
        assert(cache.mark_resident(grandchild, 3));

        assert(cache.request_page(sibling, 4, 3.0F));
        const auto sibling_request = cache.take_requests(1);
        assert(sibling_request.size() == 1 && sibling_request.front().id == sibling);
        assert(cache.mark_resident(sibling, 4));
        assert(cache.stats().resident_bytes == 290);
        assert(!cache.mark_evicted(child)); // parent cannot leave while its child is resident
        const auto evictions = cache.collect_evictions();
        assert(evictions.size() == 2);
        assert(cache.mark_evicted(grandchild));
        assert(cache.mark_evicted(sibling));
        assert(cache.mark_evicted(child));
        assert(!cache.mark_evicted(root)); // coarse roots remain pinned
        assert(cache.stats().resident_bytes == 100);
        assert(cache.is_resident(root) && !cache.is_resident(child));
        assert(!cache.unregister_page(root)); // roots remain registered
        assert(!cache.unregister_page(child)); // a parent with children cannot be removed
        assert(cache.unregister_page(grandchild));
        assert(cache.unregister_page(sibling));
        assert(cache.unregister_page(child));
        assert(!cache.unregister_page(child)); // stale/duplicate unregister is rejected
        assert(cache.stats().registered_pages == 1);

        VirtualGeometryPageCache reserved_ids{1024};
        assert(!reserved_ids.register_page(
            {invalid_virtual_geometry_page, invalid_virtual_geometry_page, 1}));
        assert(!reserved_ids.register_page({1, 1, 1}));
        constexpr auto reserved_root = invalid_virtual_geometry_page - 1U;
        assert(reserved_ids.register_page(
            {reserved_root, invalid_virtual_geometry_page, 1}));
        assert(reserved_ids.mark_resident(reserved_root, 0));
        assert(!reserved_ids.mark_evicted(reserved_root));
        assert(!reserved_ids.unregister_page(reserved_root));

        VirtualGeometryPageCache cyclic{1024};
        assert(cyclic.register_page({1, 2, 16}));
        assert(cyclic.register_page({2, 1, 16}));
        assert(!cyclic.request_page(1, 0, 1.0F));
    }
    return 0;
}
