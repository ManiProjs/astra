#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace astra::geometry {

using VirtualGeometryPageId = std::uint64_t;
inline constexpr VirtualGeometryPageId invalid_virtual_geometry_page =
    std::numeric_limits<VirtualGeometryPageId>::max();

struct VirtualGeometryPageDesc final {
    VirtualGeometryPageId id{invalid_virtual_geometry_page};
    VirtualGeometryPageId parent{invalid_virtual_geometry_page};
    std::size_t estimated_bytes{};
};

struct VirtualGeometryPageRequest final {
    VirtualGeometryPageId id{invalid_virtual_geometry_page};
    float priority{};
    std::uint64_t requested_frame{};
    std::uint32_t hierarchy_depth{};
};

struct VirtualGeometryPageCacheStats final {
    std::size_t budget_bytes{};
    std::size_t resident_bytes{};
    std::size_t registered_pages{};
    std::size_t resident_pages{};
    std::size_t pending_requests{};
};

// Thread-safe residency/request policy for a file or GPU-backed virtual-geometry
// streamer. Root pages are pinned; child pages can only become resident after
// their parent, and a parent cannot be evicted while a child remains resident.
// The application owns decompression, file I/O, upload and destruction; it calls
// mark_resident/mark_evicted after those operations complete.
class VirtualGeometryPageCache final {
public:
    explicit VirtualGeometryPageCache(std::size_t budget_bytes)
        : budget_bytes_(budget_bytes) {}

    [[nodiscard]] std::expected<void, std::string>
    register_page(VirtualGeometryPageDesc description) {
        if (description.id == invalid_virtual_geometry_page ||
            description.id == description.parent || description.estimated_bytes == 0)
            return std::unexpected("Virtual geometry page has an invalid id, parent, or byte estimate.");
        std::scoped_lock lock(mutex_);
        if (pages_.contains(description.id))
            return std::unexpected("Virtual geometry page id is already registered.");
        pages_.emplace(description.id, Entry{.description = description});
        return {};
    }

    [[nodiscard]] bool request_page(VirtualGeometryPageId id, std::uint64_t frame,
                                    float priority) {
        if (!std::isfinite(priority))
            return false;
        std::scoped_lock lock(mutex_);
        auto current = id;
        std::uint32_t depth = 0;
        std::set<VirtualGeometryPageId> visited;
        while (current != invalid_virtual_geometry_page) {
            const auto found = pages_.find(current);
            if (found == pages_.end() || !visited.insert(current).second)
                return false;
            auto& entry = found->second;
            if (entry.resident) {
                entry.last_used_frame = frame;
                return true;
            }
            entry.requested = true;
            entry.priority = std::max(entry.priority, priority);
            entry.requested_frame = frame;
            entry.request_depth = depth;
            current = entry.description.parent;
            ++depth;
        }
        return true;
    }

    [[nodiscard]] std::vector<VirtualGeometryPageRequest>
    take_requests(std::size_t maximum_count) {
        std::scoped_lock lock(mutex_);
        std::vector<VirtualGeometryPageRequest> requests;
        requests.reserve(std::min(maximum_count, pages_.size()));
        for (const auto& [id, entry] : pages_) {
            if (entry.requested && !entry.resident)
                requests.push_back({id, entry.priority, entry.requested_frame, entry.request_depth});
        }
        std::sort(requests.begin(), requests.end(), [](const auto& a, const auto& b) {
            if (a.hierarchy_depth != b.hierarchy_depth)
                return a.hierarchy_depth > b.hierarchy_depth;
            if (a.priority != b.priority)
                return a.priority > b.priority;
            return a.id < b.id;
        });
        if (requests.size() > maximum_count)
            requests.resize(maximum_count);
        for (const auto& request : requests)
            pages_.at(request.id).requested = false;
        return requests;
    }

    [[nodiscard]] std::expected<void, std::string>
    mark_resident(VirtualGeometryPageId id, std::uint64_t frame, std::size_t actual_bytes = 0) {
        std::scoped_lock lock(mutex_);
        const auto found = pages_.find(id);
        if (found == pages_.end())
            return std::unexpected("Cannot mark an unregistered virtual geometry page resident.");
        auto& entry = found->second;
        if (entry.resident)
            return std::unexpected("Virtual geometry page is already resident.");
        if (entry.description.parent != invalid_virtual_geometry_page) {
            const auto parent = pages_.find(entry.description.parent);
            if (parent == pages_.end() || !parent->second.resident)
                return std::unexpected("A virtual geometry child page requires its parent to be resident.");
        }
        const std::size_t bytes = actual_bytes == 0 ? entry.description.estimated_bytes : actual_bytes;
        if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - resident_bytes_)
            return std::unexpected("Virtual geometry page byte size overflows the residency budget.");
        entry.resident = true;
        entry.resident_bytes = bytes;
        entry.last_used_frame = frame;
        resident_bytes_ += bytes;
        return {};
    }

    [[nodiscard]] bool touch_page(VirtualGeometryPageId id, std::uint64_t frame) {
        std::scoped_lock lock(mutex_);
        const auto found = pages_.find(id);
        if (found == pages_.end() || !found->second.resident)
            return false;
        found->second.last_used_frame = frame;
        return true;
    }

    [[nodiscard]] std::vector<VirtualGeometryPageId> collect_evictions() const {
        std::scoped_lock lock(mutex_);
        std::vector<VirtualGeometryPageId> evictions;
        if (resident_bytes_ <= budget_bytes_)
            return evictions;
        std::set<VirtualGeometryPageId> planned;
        std::size_t planned_bytes = resident_bytes_;
        while (planned_bytes > budget_bytes_) {
            const Entry* candidate = nullptr;
            VirtualGeometryPageId candidate_id = invalid_virtual_geometry_page;
            for (const auto& [id, entry] : pages_) {
                if (!entry.resident || entry.requested ||
                    entry.description.parent == invalid_virtual_geometry_page || planned.contains(id))
                    continue;
                const bool has_live_child = std::ranges::any_of(pages_, [&](const auto& child) {
                    return child.second.resident && !planned.contains(child.first) &&
                           child.second.description.parent == id;
                });
                if (has_live_child)
                    continue;
                if (candidate == nullptr || entry.last_used_frame < candidate->last_used_frame ||
                    (entry.last_used_frame == candidate->last_used_frame && id < candidate_id)) {
                    candidate = &entry;
                    candidate_id = id;
                }
            }
            if (candidate == nullptr)
                break;
            planned.insert(candidate_id);
            planned_bytes -= candidate->resident_bytes;
            evictions.push_back(candidate_id);
        }
        return evictions;
    }

    [[nodiscard]] std::expected<void, std::string> mark_evicted(VirtualGeometryPageId id) {
        std::scoped_lock lock(mutex_);
        const auto found = pages_.find(id);
        if (found == pages_.end() || !found->second.resident)
            return std::unexpected("Cannot evict a page that is not resident.");
        auto& entry = found->second;
        if (entry.description.parent == invalid_virtual_geometry_page)
            return std::unexpected("Root virtual geometry pages are pinned and cannot be evicted.");
        const bool has_resident_child = std::ranges::any_of(pages_, [&](const auto& child) {
            return child.second.resident && child.second.description.parent == id;
        });
        if (has_resident_child)
            return std::unexpected("A virtual geometry page cannot be evicted while a child is resident.");
        resident_bytes_ -= entry.resident_bytes;
        entry.resident = false;
        entry.resident_bytes = 0;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> unregister_page(VirtualGeometryPageId id) {
        std::scoped_lock lock(mutex_);
        const auto found = pages_.find(id);
        if (found == pages_.end())
            return std::unexpected("Cannot unregister an unknown virtual geometry page.");
        if (found->second.resident ||
            found->second.description.parent == invalid_virtual_geometry_page)
            return std::unexpected("Resident and root virtual geometry pages cannot be unregistered.");
        const bool has_children = std::ranges::any_of(pages_, [&](const auto& child) {
            return child.second.description.parent == id;
        });
        if (has_children)
            return std::unexpected("A virtual geometry page cannot be unregistered while child pages exist.");
        pages_.erase(found);
        return {};
    }

    void set_budget(std::size_t bytes) {
        std::scoped_lock lock(mutex_);
        budget_bytes_ = bytes;
    }

    [[nodiscard]] bool is_resident(VirtualGeometryPageId id) const {
        std::scoped_lock lock(mutex_);
        const auto found = pages_.find(id);
        return found != pages_.end() && found->second.resident;
    }

    [[nodiscard]] VirtualGeometryPageCacheStats stats() const {
        std::scoped_lock lock(mutex_);
        VirtualGeometryPageCacheStats result{
            .budget_bytes = budget_bytes_,
            .resident_bytes = resident_bytes_,
            .registered_pages = pages_.size(),
        };
        for (const auto& [id, entry] : pages_) {
            (void)id;
            result.resident_pages += entry.resident ? 1U : 0U;
            result.pending_requests += entry.requested ? 1U : 0U;
        }
        return result;
    }

private:
    struct Entry final {
        VirtualGeometryPageDesc description{};
        bool requested{};
        bool resident{};
        float priority{};
        std::uint64_t requested_frame{};
        std::uint64_t last_used_frame{};
        std::uint32_t request_depth{};
        std::size_t resident_bytes{};
    };

    mutable std::mutex mutex_;
    std::map<VirtualGeometryPageId, Entry> pages_;
    std::size_t budget_bytes_{};
    std::size_t resident_bytes_{};
};

} // namespace astra::geometry
