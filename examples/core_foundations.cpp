#include <astra/core/allocator.hpp>
#include <astra/core/config.hpp>
#include <astra/core/log.hpp>
#include <astra/core/thread_pool.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

int main() {
    using namespace astra::core;

    Logger logger{LogLevel::debug};
    logger.log(LogLevel::info, "Example", "Core foundations demo starting");

    ConfigStore settings;
    (void)settings.set_default("renderer.vsync", true);
    settings.set("renderer.width", std::int64_t{1920});
    settings.set("renderer.name", std::string{"Astra sample renderer"});

    logger.log(LogLevel::info, "Config",
               settings.get_or<bool>("renderer.vsync", false) ? "VSync enabled" : "VSync disabled");
    logger.log(LogLevel::info, "Config",
               "Width: " + std::to_string(settings.get_or<std::int64_t>("renderer.width", 1280)));

    DefaultAllocator system_allocator;
    TrackingAllocator tracked_allocator{system_allocator};
    {
        UniqueAllocation buffer{tracked_allocator, 4096, 64};
        std::memset(buffer.get(), 0, buffer.size());
        logger.log(LogLevel::info, "Memory",
                   "Allocated and cleared " + std::to_string(buffer.size()) + " bytes");
    }
    const auto memory = tracked_allocator.stats();
    logger.log(LogLevel::info, "Memory",
               "Live allocations after scope: " + std::to_string(memory.live_allocations));

    ThreadPool jobs{2};
    auto sum = jobs.submit([](int left, int right) { return left + right; }, 20, 22);
    auto greeting = jobs.submit([] { return std::string{"Hello from an Astra worker"}; });
    logger.log(LogLevel::info, "Jobs", greeting.get());
    logger.log(LogLevel::info, "Jobs", "20 + 22 = " + std::to_string(sum.get()));
    jobs.shutdown(ShutdownMode::drain);

    logger.log(LogLevel::info, "Example", "Core foundations demo complete");
    return memory.bytes_in_use == 0 && memory.live_allocations == 0 ? 0 : 1;
}
