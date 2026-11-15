#include <astra/vulkan/vulkan_device.hpp>

#include "platform_surface.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if !defined(ASTRA_VULKAN_USE_METAL_SURFACE)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

namespace astra::vulkan {
namespace {

rhi::RenderError error(rhi::RenderErrorCode code, std::string message) {
    return {.code = code, .message = std::move(message)};
}

std::string vk_message(const char* operation, VkResult result) {
    return std::string{operation} + " failed with VkResult " + std::to_string(result) + '.';
}

bool has_instance_extension(const char* name) {
    std::uint32_t count{};
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    return std::ranges::any_of(extensions, [name](const auto& extension) {
        return std::string{extension.extensionName} == name;
    });
}

bool has_device_extension(VkPhysicalDevice physical_device, const char* name) {
    std::uint32_t count{};
    if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count,
                                             extensions.data()) != VK_SUCCESS)
        return false;
    return std::ranges::any_of(extensions, [name](const auto& extension) {
        return std::string{extension.extensionName} == name;
    });
}

class VulkanRenderDevice final : public rhi::RenderDevice {
  public:
    ~VulkanRenderDevice() override { shutdown(); }

    [[nodiscard]] std::expected<void, rhi::RenderError> initialize(void* native_surface) {
        native_surface_ = native_surface;
        if (!native_surface_)
            return std::unexpected(error(rhi::RenderErrorCode::invalid_surface,
                                         "Vulkan requires a non-null native presentation surface."));

        std::vector<const char*> instance_extensions;
#if defined(ASTRA_VULKAN_USE_METAL_SURFACE)
        constexpr const char* metal_surface_extension = "VK_EXT_metal_surface";
        if (!has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME) ||
            !has_instance_extension(metal_surface_extension))
            return std::unexpected(error(
                rhi::RenderErrorCode::unsupported_backend,
                "MoltenVK must expose VK_KHR_surface and VK_EXT_metal_surface to present the AppKit CAMetalLayer."));
        instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, metal_surface_extension};
#else
        std::uint32_t required_count{};
        const auto* required = glfwGetRequiredInstanceExtensions(&required_count);
        if (!required || required_count == 0)
            return std::unexpected(error(rhi::RenderErrorCode::unsupported_backend,
                                         "GLFW reports no Vulkan instance surface extensions."));
        instance_extensions.assign(required, required + required_count);
#endif

        VkInstanceCreateFlags instance_flags{};
        if (has_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        if (has_instance_extension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Astra";
        application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.pEngineName = "Astra Engine";
        application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.flags = instance_flags;
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
        auto result = vkCreateInstance(&instance_info, nullptr, &instance_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateInstance", result)));

        std::string surface_error;
        if (!astra_vulkan_create_platform_surface(instance_, native_surface_, &surface_, surface_error))
            return std::unexpected(error(rhi::RenderErrorCode::invalid_surface,
                                         std::move(surface_error)));
        if (auto selected = select_physical_device(); !selected)
            return selected;
        if (auto created = create_logical_device(); !created)
            return created;
        if (auto created = create_swapchain_resources(); !created)
            return created;
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &image_available_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateSemaphore(image available)", result)));
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = vkCreateFence(device_, &fence_info, nullptr, &in_flight_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateFence", result)));
        return {};
    }

    [[nodiscard]] rhi::Backend backend() const noexcept override { return rhi::Backend::vulkan; }
    [[nodiscard]] const std::string& device_name() const noexcept override { return device_name_; }

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuMesh>, rhi::RenderError>
    create_mesh(const rhi::MeshData&) override {
        return std::unexpected(unsupported("GPU mesh creation"));
    }

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuVirtualGeometry>, rhi::RenderError>
    create_virtual_geometry(const rhi::VirtualGeometryData&) override {
        return std::unexpected(unsupported("GPU virtual-geometry creation"));
    }

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuTexture>, rhi::RenderError>
    create_texture(const rhi::TextureData&) override {
        return std::unexpected(unsupported("GPU texture creation"));
    }

    [[nodiscard]] std::expected<std::unique_ptr<rhi::GpuEnvironment>, rhi::RenderError>
    create_environment(const rhi::EnvironmentData&) override {
        return std::unexpected(unsupported("environment creation"));
    }

    [[nodiscard]] std::expected<void, rhi::RenderError> render_scene(
        rhi::Color, std::span<const rhi::DrawItem>, const rhi::Matrix4&,
        const rhi::SceneSettings&) override {
        return std::unexpected(unsupported("scene and mesh drawing"));
    }

    [[nodiscard]] std::expected<void, rhi::RenderError> render_frame(rhi::Color clear_color) override {
        if (!device_)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         "The Vulkan logical device is unavailable."));
        if (!swapchain_)
            return recreate_swapchain();
        auto result = vkWaitForFences(device_, 1, &in_flight_, VK_TRUE,
                                      std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkWaitForFences", result)));

        std::uint32_t image_index{};
        result = vkAcquireNextImageKHR(device_, swapchain_,
                                       std::numeric_limits<std::uint64_t>::max(),
                                       image_available_, VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return recreate_swapchain();
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return std::unexpected(error(rhi::RenderErrorCode::frame_acquisition_failed,
                                         vk_message("vkAcquireNextImageKHR", result)));
        const bool suboptimal = result == VK_SUBOPTIMAL_KHR;

        auto command_buffer = command_buffers_[image_index];
        result = vkResetCommandBuffer(command_buffer, 0);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkResetCommandBuffer", result)));
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkBeginCommandBuffer", result)));
        VkClearValue clear{};
        clear.color = {{clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha}};
        VkRenderPassBeginInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass_info.renderPass = render_pass_;
        pass_info.framebuffer = framebuffers_[image_index];
        pass_info.renderArea.extent = swapchain_extent_;
        pass_info.clearValueCount = 1;
        pass_info.pClearValues = &clear;
        vkCmdBeginRenderPass(command_buffer, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command_buffer);
        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkEndCommandBuffer", result)));

        result = vkResetFences(device_, 1, &in_flight_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkResetFences", result)));
        constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &present_ready_[image_index];
        result = vkQueueSubmit(queue_, 1, &submit, in_flight_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkQueueSubmit", result)));

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &present_ready_[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || suboptimal)
            return recreate_swapchain();
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::command_submission_failed,
                                         vk_message("vkQueuePresentKHR", result)));
        return {};
    }

  private:
    [[nodiscard]] rhi::RenderError unsupported(const char* operation) const {
        return error(rhi::RenderErrorCode::unsupported_backend,
                     std::string{"The Vulkan clear/present slice does not implement "} + operation + ".");
    }

    [[nodiscard]] std::expected<void, rhi::RenderError> select_physical_device() {
        std::uint32_t count{};
        auto result = vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         "No Vulkan physical devices were enumerated."));
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkEnumeratePhysicalDevices", result)));

        for (const auto candidate : devices) {
            if (!has_device_extension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;
            std::uint32_t family_count{};
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (std::uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
                    families[family].queueCount == 0)
                    continue;
                VkBool32 present_supported{};
                result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_,
                                                               &present_supported);
                if (result == VK_SUCCESS && present_supported) {
                    physical_device_ = candidate;
                    queue_family_ = family;
                    VkPhysicalDeviceProperties properties{};
                    vkGetPhysicalDeviceProperties(candidate, &properties);
                    device_name_ = properties.deviceName;
                    portability_subset_ = has_device_extension(
                        candidate, "VK_KHR_portability_subset");
                    return {};
                }
            }
        }
        return std::unexpected(error(
            rhi::RenderErrorCode::device_unavailable,
            "No Vulkan physical device has both a graphics queue and presentation support for this surface."));
    }

    [[nodiscard]] std::expected<void, rhi::RenderError> create_logical_device() {
        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (portability_subset_)
            extensions.push_back("VK_KHR_portability_subset");
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        device_info.ppEnabledExtensionNames = extensions.data();
        device_info.pEnabledFeatures = &features;
        const auto result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateDevice", result)));
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        const auto pool_result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
        if (pool_result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateCommandPool", pool_result)));
        return {};
    }

    [[nodiscard]] std::expected<void, rhi::RenderError> create_swapchain_resources() {
        VkSurfaceCapabilitiesKHR capabilities{};
        auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_,
                                                                 &capabilities);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result)));
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
            return std::unexpected(error(rhi::RenderErrorCode::unsupported_backend,
                                         "The Vulkan surface does not support color-attachment swapchain images."));
        std::uint32_t format_count{};
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
        if (result != VK_SUCCESS || format_count == 0)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         "The Vulkan surface exposes no presentation formats."));
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count,
                                                       formats.data());
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkGetPhysicalDeviceSurfaceFormatsKHR", result)));
        auto selected_format = formats.front();
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                selected_format = format;
                break;
            }
        }
        swapchain_format_ = selected_format.format;
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            swapchain_extent_ = capabilities.currentExtent;
        } else {
            std::uint32_t width{}, height{};
            astra_vulkan_get_framebuffer_size(native_surface_, width, height);
            if (width == 0 || height == 0)
                return std::unexpected(error(rhi::RenderErrorCode::frame_acquisition_failed,
                                             "The Vulkan presentation surface is currently minimized."));
            swapchain_extent_.width = std::clamp(width, capabilities.minImageExtent.width,
                                                  capabilities.maxImageExtent.width);
            swapchain_extent_.height = std::clamp(height, capabilities.minImageExtent.height,
                                                   capabilities.maxImageExtent.height);
        }
        if (swapchain_extent_.width == 0 || swapchain_extent_.height == 0)
            return std::unexpected(error(rhi::RenderErrorCode::frame_acquisition_failed,
                                         "The Vulkan presentation surface is currently minimized."));
        std::uint32_t image_count = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0)
            image_count = std::min(image_count, capabilities.maxImageCount);
        VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        constexpr std::array composite_options{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (const auto option : composite_options) {
            if ((capabilities.supportedCompositeAlpha & option) != 0) {
                composite_alpha = option;
                break;
            }
        }
        VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchain_info.surface = surface_;
        swapchain_info.minImageCount = image_count;
        swapchain_info.imageFormat = swapchain_format_;
        swapchain_info.imageColorSpace = selected_format.colorSpace;
        swapchain_info.imageExtent = swapchain_extent_;
        swapchain_info.imageArrayLayers = 1;
        swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.preTransform = capabilities.currentTransform;
        swapchain_info.compositeAlpha = composite_alpha;
        swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchain_info.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &swapchain_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateSwapchainKHR", result)));
        result = vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
        if (result != VK_SUCCESS || image_count == 0)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkGetSwapchainImagesKHR", result)));
        swapchain_images_.resize(image_count);
        result = vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkGetSwapchainImagesKHR", result)));

        present_ready_.resize(image_count);
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& semaphore : present_ready_) {
            result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore);
            if (result != VK_SUCCESS)
                return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                             vk_message("vkCreateSemaphore(present ready)", result)));
        }

        swapchain_views_.resize(image_count);
        for (std::size_t index = 0; index < swapchain_images_.size(); ++index) {
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = swapchain_images_[index];
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = swapchain_format_;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            result = vkCreateImageView(device_, &view_info, nullptr, &swapchain_views_[index]);
            if (result != VK_SUCCESS)
                return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                             vk_message("vkCreateImageView", result)));
        }

        VkAttachmentDescription attachment{};
        attachment.format = swapchain_format_;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass_info.attachmentCount = 1;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;
        pass_info.dependencyCount = 1;
        pass_info.pDependencies = &dependency;
        result = vkCreateRenderPass(device_, &pass_info, nullptr, &render_pass_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkCreateRenderPass", result)));

        framebuffers_.resize(image_count);
        for (std::size_t index = 0; index < swapchain_views_.size(); ++index) {
            VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer_info.renderPass = render_pass_;
            framebuffer_info.attachmentCount = 1;
            framebuffer_info.pAttachments = &swapchain_views_[index];
            framebuffer_info.width = swapchain_extent_.width;
            framebuffer_info.height = swapchain_extent_.height;
            framebuffer_info.layers = 1;
            result = vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &framebuffers_[index]);
            if (result != VK_SUCCESS)
                return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                             vk_message("vkCreateFramebuffer", result)));
        }
        command_buffers_.resize(image_count);
        VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate_info.commandPool = command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = image_count;
        result = vkAllocateCommandBuffers(device_, &allocate_info, command_buffers_.data());
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkAllocateCommandBuffers", result)));
        return {};
    }

    void destroy_swapchain_resources() noexcept {
        if (!device_)
            return;
        if (!command_buffers_.empty() && command_pool_)
            vkFreeCommandBuffers(device_, command_pool_, static_cast<std::uint32_t>(command_buffers_.size()),
                                 command_buffers_.data());
        command_buffers_.clear();
        for (const auto semaphore : present_ready_)
            if (semaphore) vkDestroySemaphore(device_, semaphore, nullptr);
        present_ready_.clear();
        for (const auto framebuffer : framebuffers_)
            if (framebuffer) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
        for (const auto view : swapchain_views_)
            if (view) vkDestroyImageView(device_, view, nullptr);
        swapchain_views_.clear();
        swapchain_images_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    [[nodiscard]] std::expected<void, rhi::RenderError> recreate_swapchain() {
        const auto result = vkDeviceWaitIdle(device_);
        if (result != VK_SUCCESS)
            return std::unexpected(error(rhi::RenderErrorCode::device_unavailable,
                                         vk_message("vkDeviceWaitIdle", result)));
        destroy_swapchain_resources();
        auto recreated = create_swapchain_resources();
        if (!recreated)
            destroy_swapchain_resources();
        return recreated;
    }

    void shutdown() noexcept {
        if (device_) {
            vkDeviceWaitIdle(device_);
            if (in_flight_) vkDestroyFence(device_, in_flight_, nullptr);
            if (image_available_) vkDestroySemaphore(device_, image_available_, nullptr);
            destroy_swapchain_resources();
            if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ && surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        in_flight_ = VK_NULL_HANDLE;
        image_available_ = VK_NULL_HANDLE;
        command_pool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
    }

    void* native_surface_{};
    VkInstance instance_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physical_device_{};
    VkDevice device_{};
    VkQueue queue_{};
    std::uint32_t queue_family_{};
    bool portability_subset_{};
    std::string device_name_;
    VkSwapchainKHR swapchain_{};
    VkFormat swapchain_format_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    VkRenderPass render_pass_{};
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool command_pool_{};
    std::vector<VkCommandBuffer> command_buffers_;
    VkSemaphore image_available_{};
    std::vector<VkSemaphore> present_ready_;
    VkFence in_flight_{};
};

} // namespace

std::expected<std::unique_ptr<rhi::RenderDevice>, rhi::RenderError>
create_vulkan_render_device(void* native_surface) {
    auto device = std::make_unique<VulkanRenderDevice>();
    if (auto initialized = device->initialize(native_surface); !initialized)
        return std::unexpected(initialized.error());
    return std::unique_ptr<rhi::RenderDevice>{std::move(device)};
}

} // namespace astra::vulkan
