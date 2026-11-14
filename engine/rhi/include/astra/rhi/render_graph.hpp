#pragma once

#include <astra/rhi/descriptors.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace astra::rhi {

struct GraphResource final {
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
    friend constexpr bool operator==(GraphResource, GraphResource) = default;
};

struct GraphPass final {
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
    friend constexpr bool operator==(GraphPass, GraphPass) = default;
};

enum class GraphAccess : std::uint8_t { read, write, read_write };
enum class GraphResourceKind : std::uint8_t { texture, buffer };

struct GraphResourceDesc final {
    GraphResourceKind kind{GraphResourceKind::texture};
    TextureDesc texture{};
    BufferDesc buffer{};
    bool imported{};
};

struct GraphResourceUse final {
    GraphResource resource{};
    GraphAccess access{GraphAccess::read};
};

struct CompiledGraph final {
    struct ResourceLifetime final {
        GraphResource resource{};
        std::uint32_t first_pass{};
        std::uint32_t last_pass{};
    };
    std::vector<GraphPass> execution_order;
    std::vector<ResourceLifetime> lifetimes;
};

enum class GraphErrorCode : std::uint8_t {
    invalid_handle,
    duplicate_resource_use,
    read_before_write,
    dependency_cycle,
    pass_execution_failed,
};

struct GraphError final {
    GraphErrorCode code{};
    std::string message;
};

// A backend-independent pass graph. Backends provide pass callbacks which encode
// their native commands; compilation orders callbacks and derives resource lifetimes.
class RenderGraph final {
  public:
    [[nodiscard]] GraphResource create_texture(std::string name, TextureDesc description) {
        return add_resource(std::move(name), GraphResourceDesc{.kind = GraphResourceKind::texture,
                                                               .texture = description});
    }

    [[nodiscard]] GraphResource create_buffer(std::string name, BufferDesc description) {
        return add_resource(std::move(name), GraphResourceDesc{.kind = GraphResourceKind::buffer,
                                                              .buffer = description});
    }

    [[nodiscard]] GraphResource import_texture(std::string name, TextureDesc description) {
        return add_resource(std::move(name), GraphResourceDesc{.kind = GraphResourceKind::texture,
                                                               .texture = description,
                                                               .imported = true});
    }

    [[nodiscard]] GraphResource import_buffer(std::string name, BufferDesc description) {
        return add_resource(std::move(name), GraphResourceDesc{.kind = GraphResourceKind::buffer,
                                                              .buffer = description,
                                                              .imported = true});
    }

    [[nodiscard]] GraphPass add_pass(std::string name) {
        passes_.push_back(Pass{.name = std::move(name)});
        return GraphPass{static_cast<std::uint32_t>(passes_.size() - 1)};
    }

    using PassCallback = std::function<std::expected<void, std::string>()>;

    [[nodiscard]] GraphPass add_pass(std::string name, PassCallback callback) {
        passes_.push_back(Pass{.name = std::move(name), .callback = std::move(callback)});
        return GraphPass{static_cast<std::uint32_t>(passes_.size() - 1)};
    }

    [[nodiscard]] bool use(GraphPass pass, GraphResource resource, GraphAccess access) {
        if (!valid(pass) || !valid(resource)) return false;
        auto& uses = passes_[pass.index].uses;
        if (std::ranges::any_of(uses, [resource](const GraphResourceUse& use) {
                return use.resource == resource;
            })) return false;
        uses.push_back({resource, access});
        return true;
    }

    // Declare that predecessor must execute before pass. Resource hazards add
    // their own edges automatically; explicit edges are useful for side effects.
    [[nodiscard]] bool depends_on(GraphPass pass, GraphPass predecessor) {
        if (!valid(pass) || !valid(predecessor) || pass == predecessor) return false;
        auto& dependencies = passes_[pass.index].dependencies;
        if (std::ranges::find(dependencies, predecessor) != dependencies.end()) return false;
        dependencies.push_back(predecessor);
        return true;
    }

    [[nodiscard]] std::expected<CompiledGraph, GraphError> compile() const {
        const auto pass_count = passes_.size();
        std::vector<std::vector<std::uint32_t>> edges(pass_count);
        std::vector<std::uint32_t> indegree(pass_count);
        std::vector<std::int64_t> last_writer(resources_.size(), -1);
        std::vector<std::vector<std::uint32_t>> readers(resources_.size());
        std::vector<CompiledGraph::ResourceLifetime> lifetimes(resources_.size());
        std::vector<bool> used(resources_.size());
        std::vector<std::uint32_t> execution_rank(pass_count);
        for (auto& lifetime : lifetimes)
            lifetime.first_pass = std::numeric_limits<std::uint32_t>::max();

        for (std::uint32_t pass_index = 0; pass_index < pass_count; ++pass_index) {
            for (const auto predecessor : passes_[pass_index].dependencies)
                add_edge(edges, indegree, predecessor.index, pass_index);
            for (const auto& use : passes_[pass_index].uses) {
                if (!valid(use.resource))
                    return std::unexpected(GraphError{GraphErrorCode::invalid_handle,
                                                      "pass references an invalid resource"});
                const auto resource_index = use.resource.index;
                used[resource_index] = true;
                auto& lifetime = lifetimes[resource_index];
                lifetime.resource = use.resource;
                lifetime.first_pass = std::min(lifetime.first_pass, pass_index);
                lifetime.last_pass = pass_index;

                const bool reads = use.access != GraphAccess::write;
                const bool writes = use.access != GraphAccess::read;
                if (reads && last_writer[resource_index] < 0 && !resources_[resource_index].desc.imported)
                    return std::unexpected(GraphError{GraphErrorCode::read_before_write,
                        "transient graph resource is read before its first write: " + resources_[resource_index].name});
                if (reads && last_writer[resource_index] >= 0)
                    add_edge(edges, indegree, static_cast<std::uint32_t>(last_writer[resource_index]), pass_index);
                if (writes) {
                    if (last_writer[resource_index] >= 0)
                        add_edge(edges, indegree, static_cast<std::uint32_t>(last_writer[resource_index]), pass_index);
                    for (const auto reader : readers[resource_index])
                        add_edge(edges, indegree, reader, pass_index);
                    readers[resource_index].clear();
                    last_writer[resource_index] = pass_index;
                } else {
                    readers[resource_index].push_back(pass_index);
                }
            }
        }

        std::vector<std::uint32_t> ready;
        for (std::uint32_t i = 0; i < pass_count; ++i)
            if (indegree[i] == 0) ready.push_back(i);
        CompiledGraph result;
        result.execution_order.reserve(pass_count);
        for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
            const auto current = ready[cursor];
            execution_rank[current] = static_cast<std::uint32_t>(result.execution_order.size());
            result.execution_order.push_back(GraphPass{current});
            for (const auto next : edges[current])
                if (--indegree[next] == 0) ready.push_back(next);
        }
        if (result.execution_order.size() != pass_count)
            return std::unexpected(GraphError{GraphErrorCode::dependency_cycle,
                                              "render graph contains a dependency cycle"});
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (!used[i]) continue;
            auto lifetime = lifetimes[i];
            lifetime.first_pass = std::numeric_limits<std::uint32_t>::max();
            lifetime.last_pass = 0;
            for (std::size_t pass_index = 0; pass_index < passes_.size(); ++pass_index) {
                const auto& pass = passes_[pass_index];
                if (std::ranges::any_of(pass.uses, [i](const GraphResourceUse& use) {
                        return use.resource.index == i;
                    })) {
                    const auto rank = execution_rank[static_cast<std::uint32_t>(pass_index)];
                    lifetime.first_pass = std::min(lifetime.first_pass, rank);
                    lifetime.last_pass = std::max(lifetime.last_pass, rank);
                }
            }
            result.lifetimes.push_back(lifetime);
        }
        return result;
    }

    // Execute callbacks in the compiled dependency order. Native resource
    // allocation remains backend-owned; imported resources refer to resources
    // already created by that backend.
    [[nodiscard]] std::expected<void, GraphError> execute(const CompiledGraph& graph) const {
        if (graph.execution_order.size() != passes_.size())
            return std::unexpected(GraphError{GraphErrorCode::invalid_handle,
                                              "compiled graph does not match this graph"});
        for (const auto pass : graph.execution_order) {
            if (!valid(pass))
                return std::unexpected(GraphError{GraphErrorCode::invalid_handle,
                                                  "compiled graph contains an invalid pass"});
            const auto& callback = passes_[pass.index].callback;
            if (!callback)
                continue;
            auto result = callback();
            if (!result)
                return std::unexpected(
                    GraphError{GraphErrorCode::pass_execution_failed,
                               passes_[pass.index].name + ": " + result.error()});
        }
        return {};
    }

    [[nodiscard]] std::size_t resource_count() const noexcept { return resources_.size(); }
    [[nodiscard]] std::size_t pass_count() const noexcept { return passes_.size(); }

  private:
    struct Resource final { std::string name; GraphResourceDesc desc; };
    struct Pass final {
        std::string name;
        std::vector<GraphResourceUse> uses;
        std::vector<GraphPass> dependencies;
        PassCallback callback;
    };

    [[nodiscard]] GraphResource add_resource(std::string name, GraphResourceDesc desc) {
        resources_.push_back(Resource{.name = std::move(name), .desc = desc});
        return GraphResource{static_cast<std::uint32_t>(resources_.size() - 1)};
    }
    [[nodiscard]] bool valid(GraphResource resource) const noexcept {
        return resource.valid() && resource.index < resources_.size();
    }
    [[nodiscard]] bool valid(GraphPass pass) const noexcept {
        return pass.valid() && pass.index < passes_.size();
    }
    static void add_edge(std::vector<std::vector<std::uint32_t>>& edges,
                         std::vector<std::uint32_t>& indegree,
                         std::uint32_t from, std::uint32_t to) {
        if (from == to || std::ranges::find(edges[from], to) != edges[from].end()) return;
        edges[from].push_back(to);
        ++indegree[to];
    }

    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
};

} // namespace astra::rhi
