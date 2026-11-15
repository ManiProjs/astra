#include <astra/opengl/opengl_device.hpp>

int main() {
    const auto result = astra::opengl::create_opengl_render_device(nullptr);
    return !result && result.error().code == astra::rhi::RenderErrorCode::invalid_surface ? 0 : 1;
}
