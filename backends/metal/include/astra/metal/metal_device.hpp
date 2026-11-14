#pragma once

#include <astra/rhi/render_device.hpp>

#include <expected>
#include <memory>

namespace astra::metal {

[[nodiscard]] std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
create_metal_render_device(void* native_surface);

} // namespace astra::metal
