#include <astra/geometry/virtual_geometry_streaming.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {
    using namespace astra::geometry;
    constexpr VirtualGeometryPageId world_root = 1;
    constexpr std::array<VirtualGeometryPageDesc, 9> pages{{
        {world_root, invalid_virtual_geometry_page, 4U * 1024U * 1024U},
        {10, world_root, 12U * 1024U * 1024U},
        {11, world_root, 12U * 1024U * 1024U},
        {12, world_root, 12U * 1024U * 1024U},
        {20, 10, 4U * 1024U * 1024U},
        {21, 10, 4U * 1024U * 1024U},
        {22, 11, 4U * 1024U * 1024U},
        {23, 11, 4U * 1024U * 1024U},
        {24, 12, 4U * 1024U * 1024U},
    }};

    VirtualGeometryPageCache cache{48U * 1024U * 1024U};
    for (const auto& page : pages) {
        if (auto result = cache.register_page(page); !result) {
            std::cerr << result.error() << '\n';
            return 1;
        }
    }
    if (auto result = cache.mark_resident(world_root, 0); !result) {
        std::cerr << result.error() << '\n';
        return 1;
    }

    // A moving view requests coarse pages first; fine child pages are queued
    // only after their parent has become resident. Upload/decompression is
    // represented by mark_resident(), as a real asset streamer would call it.
    constexpr std::array<std::array<VirtualGeometryPageId, 3>, 3> camera_regions{{
        {{20, 21, 22}}, {{22, 23, 24}}, {{20, 23, 24}},
    }};
    std::uint64_t frame = 1;
    for (const auto& region : camera_regions) {
        for (std::size_t index = 0; index < region.size(); ++index)
            if (!cache.request_page(region[index], frame,
                                    10.0F - static_cast<float>(index)))
                return 1;

        // A bounded upload queue avoids a burst of geometry allocations.
        for (const auto& request : cache.take_requests(4)) {
            if (!cache.is_resident(request.id) &&
                !cache.mark_resident(request.id, frame)) {
                // A child whose parent is not ready stays requested on the next frame.
                if (request.hierarchy_depth > 0 &&
                    !cache.request_page(request.id, frame + 1U, request.priority))
                    return 1;
            }
        }
        for (const auto page : cache.collect_evictions())
            (void)cache.mark_evicted(page);
        const auto stats = cache.stats();
        std::cout << "frame=" << frame << " resident_pages=" << stats.resident_pages
                  << " resident_mib=" << stats.resident_bytes / (1024U * 1024U)
                  << " pending=" << stats.pending_requests << '\n';
        ++frame;
    }
    return 0;
}
