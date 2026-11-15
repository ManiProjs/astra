#include <astra/opengl/opengl_device.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <string>
#include <utility>

namespace astra::opengl {
namespace {

rhi::RenderError unsupported(const char* operation) {
    return {.code = rhi::RenderErrorCode::unsupported_backend,
            .message = std::string{"The initial OpenGL backend does not implement "} + operation + "."};
}

class OpenGLRenderDevice final : public rhi::RenderDevice {
  public:
    OpenGLRenderDevice(GLFWwindow* window, std::string name)
        : window_(window), name_(std::move(name)) {}

    rhi::Backend backend() const noexcept override { return rhi::Backend::opengl; }
    const std::string& device_name() const noexcept override { return name_; }

    std::expected<std::unique_ptr<rhi::GpuMesh>, rhi::RenderError>
    create_mesh(const rhi::MeshData&) override {
        return std::unexpected(unsupported("GPU mesh creation"));
    }

    std::expected<std::unique_ptr<rhi::GpuTexture>, rhi::RenderError>
    create_texture(const rhi::TextureData&) override {
        return std::unexpected(unsupported("GPU texture creation"));
    }

    std::expected<void, rhi::RenderError> render_frame(rhi::Color clear_color) override {
        if (!window_ || glfwWindowShouldClose(window_))
            return std::unexpected(rhi::RenderError{
                .code = rhi::RenderErrorCode::device_unavailable,
                .message = "The OpenGL presentation window is no longer available."});
        glfwMakeContextCurrent(window_);
        int framebuffer_width{}, framebuffer_height{};
        glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);
        glViewport(0, 0, framebuffer_width, framebuffer_height);
        glClearColor(clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glfwSwapBuffers(window_);
        return {};
    }

    std::expected<void, rhi::RenderError>
    render_scene(rhi::Color clear_color, std::span<const rhi::DrawItem> draw_items,
                 const rhi::Matrix4&, const rhi::SceneSettings&) override {
        if (!draw_items.empty())
            return std::unexpected(unsupported("scene and mesh drawing"));
        return render_frame(clear_color);
    }

  private:
    GLFWwindow* window_{};
    std::string name_;
};

} // namespace

std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
create_opengl_render_device(void* native_window) {
    auto* window = static_cast<GLFWwindow*>(native_window);
    if (!window)
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::invalid_surface,
            .message = "OpenGL requires a GLFW window created by the GLFW platform adapter."});
    glfwMakeContextCurrent(window);
    const auto* renderer = glGetString(GL_RENDERER);
    const auto* version = glGetString(GL_VERSION);
    if (!renderer || !version)
        return std::unexpected(rhi::RenderError{
            .code = rhi::RenderErrorCode::device_unavailable,
            .message = "GLFW did not provide a current OpenGL context."});
    std::string name{reinterpret_cast<const char*>(renderer)};
    name += " (OpenGL ";
    name += reinterpret_cast<const char*>(version);
    name += ')';
    return std::unique_ptr<rhi::RenderDevice>{
        std::make_unique<OpenGLRenderDevice>(window, std::move(name))};
}

} // namespace astra::opengl
