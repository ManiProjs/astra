#pragma once

#include <astra/rhi/render_device.hpp>

#include <expected>
#include <memory>

namespace astra::opengl {

[[nodiscard]] std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
create_opengl_render_device(void* native_window);

} // namespace astra::opengl
