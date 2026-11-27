#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "hook_common.h"
#include "fps_overlay_font.h"
#include "depth_convert_spv.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

dr::SharedConfigView g_config_view;
dr::IpcRing g_ipc_ring;
std::recursive_mutex g_mutex;

constexpr uint32_t kStagingSlots = 3;

struct DepthConvertPC {
    uint32_t width;
    uint32_t height;
    uint32_t mode;
    uint32_t linearize;
    uint32_t reversed_z;
    float z_near;
    float z_far;
    float pad;
};

inline void* dispatch_key(const void* handle) {
    return *reinterpret_cast<void* const*>(handle);
}

bool is_depth_format(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

bool is_rgba_swapchain_format(VkFormat fmt) {
    return fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB;
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
};

struct ImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool is_depth = false;
};

struct ImageViewInfo {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct RenderPassInfo {
    int32_t depth_attachment_index = -1;
};

struct FramebufferInfo {
    std::vector<VkImageView> attachments;
};

struct SwapchainInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;

    // Next-layer dispatch pointers
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;
    PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdBeginRenderingKHR CmdBeginRenderingKHR = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
    PFN_vkCreateRenderPass2KHR CreateRenderPass2KHR = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;

    // Tracked app objects
    std::unordered_map<VkImage, ImageInfo> images;
    std::unordered_map<VkImageView, ImageViewInfo> image_views;
    std::unordered_map<VkRenderPass, RenderPassInfo> render_passes;
    std::unordered_map<VkFramebuffer, FramebufferInfo> framebuffers;
    std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchains;
    std::unordered_map<VkCommandBuffer, VkImage> cmd_active_depth;
    std::unordered_map<VkQueue, uint32_t> queue_families;
    uint32_t default_queue_family = 0;

    // Heuristic & frame state
    dr::DepthHeuristic heuristic;
    uint64_t frame_idx = 0;
    int64_t qpc_freq = 0;
    int64_t last_qpc = 0;
    LONG last_cycle_seq = 0;
    uint32_t selected_depth_idx = 0;
    uint32_t swap_w = 0;
    uint32_t swap_h = 0;

    // Internal capture & overlay resources
    bool res_initialized = false;
    uint32_t res_w = 0;
    uint32_t res_h = 0;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd_bufs[kStagingSlots]{};
    VkFence fences[kStagingSlots]{};
    VkSemaphore overlay_sem = VK_NULL_HANDLE;

    VkSampler depth_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkShaderModule comp_shader = VK_NULL_HANDLE;
    VkPipeline comp_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;

    VkImage out_gray_img = VK_NULL_HANDLE;
    VkDeviceMemory out_gray_mem = VK_NULL_HANDLE;
    VkImageView out_gray_view = VK_NULL_HANDLE;

    VkImage out_rgba_img = VK_NULL_HANDLE;
    VkDeviceMemory out_rgba_mem = VK_NULL_HANDLE;
    VkImageView out_rgba_view = VK_NULL_HANDLE;

    std::unordered_map<VkImage, VkImageView> depth_sample_views;

    VkBuffer color_buf[kStagingSlots]{};
    VkDeviceMemory color_mem[kStagingSlots]{};
    void* color_mapped[kStagingSlots]{};

    VkBuffer depth_buf[kStagingSlots]{};
    VkDeviceMemory depth_mem[kStagingSlots]{};
    void* depth_mapped[kStagingSlots]{};

    bool slot_pending[kStagingSlots]{};
    bool slot_has_color[kStagingSlots]{};
    int64_t slot_qpc[kStagingSlots]{};
    uint32_t slot_mode[kStagingSlots]{};
    bool slot_has_depth[kStagingSlots]{};
    uint32_t slot_depth_bpp[kStagingSlots]{};
    bool slot_is_rgba[kStagingSlots]{};
    int64_t last_preview_qpc = 0;

    // FPS overlay resources
    VkImage overlay_img = VK_NULL_HANDLE;
    VkDeviceMemory overlay_mem = VK_NULL_HANDLE;
    VkBuffer overlay_buf = VK_NULL_HANDLE;
    VkDeviceMemory overlay_buf_mem = VK_NULL_HANDLE;
    void* overlay_buf_mapped = nullptr;
    uint32_t overlay_w = 0;
    uint32_t overlay_h = 0;

    std::vector<uint8_t> color_pack_buf;
    std::vector<uint8_t> depth_pack_buf;
};

std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;

uint32_t find_memory_type(DeviceData* dev, uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    dev->inst->GetPhysicalDeviceMemoryProperties(dev->physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

bool create_buffer_and_mem(DeviceData* dev, VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags props, VkBuffer& out_buf, VkDeviceMemory& out_mem, void** out_mapped) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dev->CreateBuffer(dev->device, &bci, nullptr, &out_buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    dev->GetBufferMemoryRequirements(dev->device, out_buf, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(dev, req.memoryTypeBits, props);
    if (dev->AllocateMemory(dev->device, &mai, nullptr, &out_mem) != VK_SUCCESS) return false;
    if (dev->BindBufferMemory(dev->device, out_buf, out_mem, 0) != VK_SUCCESS) return false;
    if (out_mapped) {
        if (dev->MapMemory(dev->device, out_mem, 0, size, 0, out_mapped) != VK_SUCCESS) return false;
    }
    return true;
}

bool create_image_and_mem(DeviceData* dev, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                          VkImage& out_img, VkDeviceMemory& out_mem) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dev->CreateImage(dev->device, &ici, nullptr, &out_img) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    dev->GetImageMemoryRequirements(dev->device, out_img, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dev->AllocateMemory(dev->device, &mai, nullptr, &out_mem) != VK_SUCCESS) return false;
    return dev->BindImageMemory(dev->device, out_img, out_mem, 0) == VK_SUCCESS;
}

void cmd_barrier(DeviceData* dev, VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                 VkImageLayout old_layout, VkImageLayout new_layout,
                 VkAccessFlags src_access, VkAccessFlags dst_access,
                 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    dev->CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void release_size_dependent(DeviceData* dev) {
    if (dev->DeviceWaitIdle) dev->DeviceWaitIdle(dev->device);

    for (uint32_t i = 0; i < kStagingSlots; ++i) {
        if (dev->color_mapped[i]) { dev->UnmapMemory(dev->device, dev->color_mem[i]); dev->color_mapped[i] = nullptr; }
        if (dev->color_buf[i]) { dev->DestroyBuffer(dev->device, dev->color_buf[i], nullptr); dev->color_buf[i] = VK_NULL_HANDLE; }
        if (dev->color_mem[i]) { dev->FreeMemory(dev->device, dev->color_mem[i], nullptr); dev->color_mem[i] = VK_NULL_HANDLE; }

        if (dev->depth_mapped[i]) { dev->UnmapMemory(dev->device, dev->depth_mem[i]); dev->depth_mapped[i] = nullptr; }
        if (dev->depth_buf[i]) { dev->DestroyBuffer(dev->device, dev->depth_buf[i], nullptr); dev->depth_buf[i] = VK_NULL_HANDLE; }
        if (dev->depth_mem[i]) { dev->FreeMemory(dev->device, dev->depth_mem[i], nullptr); dev->depth_mem[i] = VK_NULL_HANDLE; }
        dev->slot_pending[i] = false;
    }

    if (dev->out_gray_view) { dev->DestroyImageView(dev->device, dev->out_gray_view, nullptr); dev->out_gray_view = VK_NULL_HANDLE; }
    if (dev->out_gray_img) { dev->DestroyImage(dev->device, dev->out_gray_img, nullptr); dev->out_gray_img = VK_NULL_HANDLE; }
    if (dev->out_gray_mem) { dev->FreeMemory(dev->device, dev->out_gray_mem, nullptr); dev->out_gray_mem = VK_NULL_HANDLE; }

    if (dev->out_rgba_view) { dev->DestroyImageView(dev->device, dev->out_rgba_view, nullptr); dev->out_rgba_view = VK_NULL_HANDLE; }
    if (dev->out_rgba_img) { dev->DestroyImage(dev->device, dev->out_rgba_img, nullptr); dev->out_rgba_img = VK_NULL_HANDLE; }
    if (dev->out_rgba_mem) { dev->FreeMemory(dev->device, dev->out_rgba_mem, nullptr); dev->out_rgba_mem = VK_NULL_HANDLE; }

    dev->res_w = 0;
    dev->res_h = 0;
}

void destroy_all_internal_resources(DeviceData* dev) {
    release_size_dependent(dev);

    for (auto& [img, view] : dev->depth_sample_views) {
        if (view) dev->DestroyImageView(dev->device, view, nullptr);
    }
    dev->depth_sample_views.clear();

    if (dev->overlay_buf_mapped) { dev->UnmapMemory(dev->device, dev->overlay_buf_mem); dev->overlay_buf_mapped = nullptr; }
    if (dev->overlay_buf) { dev->DestroyBuffer(dev->device, dev->overlay_buf, nullptr); dev->overlay_buf = VK_NULL_HANDLE; }
    if (dev->overlay_buf_mem) { dev->FreeMemory(dev->device, dev->overlay_buf_mem, nullptr); dev->overlay_buf_mem = VK_NULL_HANDLE; }
    if (dev->overlay_img) { dev->DestroyImage(dev->device, dev->overlay_img, nullptr); dev->overlay_img = VK_NULL_HANDLE; }
    if (dev->overlay_mem) { dev->FreeMemory(dev->device, dev->overlay_mem, nullptr); dev->overlay_mem = VK_NULL_HANDLE; }

    if (dev->desc_pool) { dev->DestroyDescriptorPool(dev->device, dev->desc_pool, nullptr); dev->desc_pool = VK_NULL_HANDLE; }
    if (dev->comp_pipeline) { dev->DestroyPipeline(dev->device, dev->comp_pipeline, nullptr); dev->comp_pipeline = VK_NULL_HANDLE; }
    if (dev->comp_shader) { dev->DestroyShaderModule(dev->device, dev->comp_shader, nullptr); dev->comp_shader = VK_NULL_HANDLE; }
    if (dev->pipe_layout) { dev->DestroyPipelineLayout(dev->device, dev->pipe_layout, nullptr); dev->pipe_layout = VK_NULL_HANDLE; }
    if (dev->dsl) { dev->DestroyDescriptorSetLayout(dev->device, dev->dsl, nullptr); dev->dsl = VK_NULL_HANDLE; }
    if (dev->depth_sampler) { dev->DestroySampler(dev->device, dev->depth_sampler, nullptr); dev->depth_sampler = VK_NULL_HANDLE; }

    if (dev->overlay_sem) { dev->DestroySemaphore(dev->device, dev->overlay_sem, nullptr); dev->overlay_sem = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < kStagingSlots; ++i) {
        if (dev->fences[i]) { dev->DestroyFence(dev->device, dev->fences[i], nullptr); dev->fences[i] = VK_NULL_HANDLE; }
    }
    if (dev->cmd_pool) { dev->DestroyCommandPool(dev->device, dev->cmd_pool, nullptr); dev->cmd_pool = VK_NULL_HANDLE; }
    dev->res_initialized = false;
}

bool ensure_resources(DeviceData* dev, uint32_t w, uint32_t h, uint32_t queue_family) {
    if (w == 0 || h == 0) return false;

    if (!dev->res_initialized) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        dev->qpc_freq = freq.QuadPart;

        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = queue_family;
        if (dev->CreateCommandPool(dev->device, &cpci, nullptr, &dev->cmd_pool) != VK_SUCCESS) return false;

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = dev->cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = kStagingSlots;
        if (dev->AllocateCommandBuffers(dev->device, &cbai, dev->cmd_bufs) != VK_SUCCESS) return false;

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (uint32_t i = 0; i < kStagingSlots; ++i) {
            if (dev->CreateFence(dev->device, &fci, nullptr, &dev->fences[i]) != VK_SUCCESS) return false;
        }

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (dev->CreateSemaphore(dev->device, &sci, nullptr, &dev->overlay_sem) != VK_SUCCESS) return false;

        VkSamplerCreateInfo smci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        smci.magFilter = VK_FILTER_NEAREST;
        smci.minFilter = VK_FILTER_NEAREST;
        smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (dev->CreateSampler(dev->device, &smci, nullptr, &dev->depth_sampler) != VK_SUCCESS) return false;

        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 3;
        dslci.pBindings = bindings;
        if (dev->CreateDescriptorSetLayout(dev->device, &dslci, nullptr, &dev->dsl) != VK_SUCCESS) return false;

        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DepthConvertPC)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dev->dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (dev->CreatePipelineLayout(dev->device, &plci, nullptr, &dev->pipe_layout) != VK_SUCCESS) return false;

        VkShaderModuleCreateInfo shci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shci.codeSize = sizeof(g_depth_convert_spv);
        shci.pCode = g_depth_convert_spv;
        if (dev->CreateShaderModule(dev->device, &shci, nullptr, &dev->comp_shader) != VK_SUCCESS) return false;

        VkComputePipelineCreateInfo cpci_pipe{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci_pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci_pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci_pipe.stage.module = dev->comp_shader;
        cpci_pipe.stage.pName = "main";
        cpci_pipe.layout = dev->pipe_layout;
        if (dev->CreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &cpci_pipe, nullptr, &dev->comp_pipeline) != VK_SUCCESS) return false;

        VkDescriptorPoolSize pool_sizes[2] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        };
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = pool_sizes;
        if (dev->CreateDescriptorPool(dev->device, &dpci, nullptr, &dev->desc_pool) != VK_SUCCESS) return false;

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = dev->desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dev->dsl;
        if (dev->AllocateDescriptorSets(dev->device, &dsai, &dev->desc_set) != VK_SUCCESS) return false;

        dev->res_initialized = true;
    }

    if (dev->res_w != w || dev->res_h != h) {
        release_size_dependent(dev);
        dev->res_w = w;
        dev->res_h = h;

        if (!create_image_and_mem(dev, w, h, VK_FORMAT_R8_UNORM,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  dev->out_gray_img, dev->out_gray_mem)) return false;
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = dev->out_gray_img;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dev->CreateImageView(dev->device, &ivci, nullptr, &dev->out_gray_view);

        if (!create_image_and_mem(dev, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  dev->out_rgba_img, dev->out_rgba_mem)) return false;
        ivci.image = dev->out_rgba_img;
        ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
        dev->CreateImageView(dev->device, &ivci, nullptr, &dev->out_rgba_view);

        const VkDeviceSize frame_bytes = static_cast<VkDeviceSize>(w) * h * 4;
        const VkMemoryPropertyFlags host_props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < kStagingSlots; ++i) {
            if (!create_buffer_and_mem(dev, frame_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_props,
                                       dev->color_buf[i], dev->color_mem[i], &dev->color_mapped[i])) return false;
            if (!create_buffer_and_mem(dev, frame_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_props,
                                       dev->depth_buf[i], dev->depth_mem[i], &dev->depth_mapped[i])) return false;
        }
    }
    return true;
}

bool ensure_overlay_image(DeviceData* dev, uint32_t bw, uint32_t bh) {
    if (dev->overlay_img && dev->overlay_w == bw && dev->overlay_h == bh) return true;
    if (dev->overlay_buf_mapped) { dev->UnmapMemory(dev->device, dev->overlay_buf_mem); dev->overlay_buf_mapped = nullptr; }
    if (dev->overlay_buf) { dev->DestroyBuffer(dev->device, dev->overlay_buf, nullptr); dev->overlay_buf = VK_NULL_HANDLE; }
    if (dev->overlay_buf_mem) { dev->FreeMemory(dev->device, dev->overlay_buf_mem, nullptr); dev->overlay_buf_mem = VK_NULL_HANDLE; }
    if (dev->overlay_img) { dev->DestroyImage(dev->device, dev->overlay_img, nullptr); dev->overlay_img = VK_NULL_HANDLE; }
    if (dev->overlay_mem) { dev->FreeMemory(dev->device, dev->overlay_mem, nullptr); dev->overlay_mem = VK_NULL_HANDLE; }

    if (!create_image_and_mem(dev, bw, bh, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              dev->overlay_img, dev->overlay_mem)) return false;

    const VkDeviceSize sz = static_cast<VkDeviceSize>(bw) * bh * 4;
    if (!create_buffer_and_mem(dev, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               dev->overlay_buf, dev->overlay_buf_mem, &dev->overlay_buf_mapped)) return false;

    dev->overlay_w = bw;
    dev->overlay_h = bh;
    return true;
}

VkImageView get_depth_sample_view(DeviceData* dev, VkImage depth_img, VkFormat fmt) {
    auto it = dev->depth_sample_views.find(depth_img);
    if (it != dev->depth_sample_views.end()) return it->second;

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = depth_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = fmt;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (dev->CreateImageView(dev->device, &ivci, nullptr, &view) == VK_SUCCESS) {
        dev->depth_sample_views[depth_img] = view;
    }
    return view;
}

// --- Intercepted Vulkan Functions ---

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateImage(
    VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateImage) return VK_ERROR_INITIALIZATION_FAILED;

    VkImageCreateInfo mod{};
    bool is_depth = false;
    if (pCreateInfo && is_depth_format(pCreateInfo->format) && pCreateInfo->samples == VK_SAMPLE_COUNT_1_BIT) {
        mod = *pCreateInfo;
        mod.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        pCreateInfo = &mod;
        is_depth = true;
    }

    const VkResult res = dev->CreateImage(device, pCreateInfo, pAllocator, pImage);
    if (res == VK_SUCCESS && pImage && *pImage && pCreateInfo) {
        std::lock_guard lock(g_mutex);
        dev->images[*pImage] = {pCreateInfo->extent.width, pCreateInfo->extent.height, pCreateInfo->format, is_depth};
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            dev = it->second.get();
            dev->images.erase(image);
            auto vit = dev->depth_sample_views.find(image);
            if (vit != dev->depth_sample_views.end()) {
                if (vit->second) dev->DestroyImageView(device, vit->second, nullptr);
                dev->depth_sample_views.erase(vit);
            }
        }
    }
    if (dev && dev->DestroyImage) dev->DestroyImage(device, image, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateImageView) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = dev->CreateImageView(device, pCreateInfo, pAllocator, pView);
    if (res == VK_SUCCESS && pView && *pView && pCreateInfo) {
        std::lock_guard lock(g_mutex);
        dev->image_views[*pView] = {pCreateInfo->image, pCreateInfo->format};
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroyImageView(
    VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            dev = it->second.get();
            dev->image_views.erase(imageView);
        }
    }
    if (dev && dev->DestroyImageView) dev->DestroyImageView(device, imageView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateRenderPass) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = dev->CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    if (res == VK_SUCCESS && pRenderPass && *pRenderPass && pCreateInfo) {
        int32_t d_idx = -1;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
            if (is_depth_format(pCreateInfo->pAttachments[i].format)) {
                d_idx = static_cast<int32_t>(i);
                break;
            }
        }
        std::lock_guard lock(g_mutex);
        dev->render_passes[*pRenderPass] = {d_idx};
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateRenderPass2(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateRenderPass2) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = dev->CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    if (res == VK_SUCCESS && pRenderPass && *pRenderPass && pCreateInfo) {
        int32_t d_idx = -1;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
            if (is_depth_format(pCreateInfo->pAttachments[i].format)) {
                d_idx = static_cast<int32_t>(i);
                break;
            }
        }
        std::lock_guard lock(g_mutex);
        dev->render_passes[*pRenderPass] = {d_idx};
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateRenderPass2KHR(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateRenderPass2KHR) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = dev->CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
    if (res == VK_SUCCESS && pRenderPass && *pRenderPass && pCreateInfo) {
        int32_t d_idx = -1;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
            if (is_depth_format(pCreateInfo->pAttachments[i].format)) {
                d_idx = static_cast<int32_t>(i);
                break;
            }
        }
        std::lock_guard lock(g_mutex);
        dev->render_passes[*pRenderPass] = {d_idx};
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroyRenderPass(
    VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            dev = it->second.get();
            dev->render_passes.erase(renderPass);
        }
    }
    if (dev && dev->DestroyRenderPass) dev->DestroyRenderPass(device, renderPass, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = dev->CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
    if (res == VK_SUCCESS && pFramebuffer && *pFramebuffer && pCreateInfo && pCreateInfo->pAttachments) {
        FramebufferInfo info;
        info.attachments.assign(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
        std::lock_guard lock(g_mutex);
        dev->framebuffers[*pFramebuffer] = std::move(info);
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroyFramebuffer(
    VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            dev = it->second.get();
            dev->framebuffers.erase(framebuffer);
        }
    }
    if (dev && dev->DestroyFramebuffer) dev->DestroyFramebuffer(device, framebuffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

    VkSwapchainCreateInfoKHR mod{};
    if (pCreateInfo) {
        mod = *pCreateInfo;
        mod.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        pCreateInfo = &mod;
    }

    const VkResult res = dev->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS && pSwapchain && *pSwapchain && pCreateInfo) {
        uint32_t count = 0;
        dev->GetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        std::vector<VkImage> imgs(count);
        if (count > 0) dev->GetSwapchainImagesKHR(device, *pSwapchain, &count, imgs.data());

        std::lock_guard lock(g_mutex);
        dev->swap_w = pCreateInfo->imageExtent.width;
        dev->swap_h = pCreateInfo->imageExtent.height;
        dev->swapchains[*pSwapchain] = {pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, pCreateInfo->imageFormat, std::move(imgs)};
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            dev = it->second.get();
            dev->swapchains.erase(swapchain);
        }
    }
    if (dev && dev->DestroySwapchainKHR) dev->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL hook_vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (dev && dev->GetDeviceQueue) {
        dev->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue) {
            std::lock_guard lock(g_mutex);
            dev->queue_families[*pQueue] = queueFamilyIndex;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL hook_vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin, VkSubpassContents contents) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(commandBuffer));
        if (it != g_devices.end()) {
            dev = it->second.get();
            dev->cmd_active_depth.erase(commandBuffer);
            if (pRenderPassBegin && dev->swap_w > 0) {
                auto rpit = dev->render_passes.find(pRenderPassBegin->renderPass);
                auto fbit = dev->framebuffers.find(pRenderPassBegin->framebuffer);
                if (rpit != dev->render_passes.end() && fbit != dev->framebuffers.end()) {
                    const int32_t d_idx = rpit->second.depth_attachment_index;
                    if (d_idx >= 0 && static_cast<size_t>(d_idx) < fbit->second.attachments.size()) {
                        VkImageView iv = fbit->second.attachments[d_idx];
                        auto ivit = dev->image_views.find(iv);
                        if (ivit != dev->image_views.end()) {
                            VkImage depth_img = ivit->second.image;
                            auto iit = dev->images.find(depth_img);
                            if (iit != dev->images.end() && iit->second.is_depth) {
                                dev->heuristic.record_draw(reinterpret_cast<uintptr_t>(depth_img),
                                                           iit->second.width, iit->second.height,
                                                           dev->swap_w, dev->swap_h);
                                dev->cmd_active_depth[commandBuffer] = depth_img;
                            }
                        }
                    }
                }
            }
        }
    }
    if (dev && dev->CmdBeginRenderPass) dev->CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

static void track_dynamic_rendering_depth(DeviceData* dev, VkCommandBuffer cmd, const VkRenderingInfo* info) {
    dev->cmd_active_depth.erase(cmd);
    if (!info || !info->pDepthAttachment || info->pDepthAttachment->imageView == VK_NULL_HANDLE || dev->swap_w == 0) return;
    auto ivit = dev->image_views.find(info->pDepthAttachment->imageView);
    if (ivit == dev->image_views.end()) return;
    VkImage depth_img = ivit->second.image;
    auto iit = dev->images.find(depth_img);
    if (iit != dev->images.end() && iit->second.is_depth) {
        dev->heuristic.record_draw(reinterpret_cast<uintptr_t>(depth_img),
                                   iit->second.width, iit->second.height,
                                   dev->swap_w, dev->swap_h);
        dev->cmd_active_depth[cmd] = depth_img;
    }
}

VKAPI_ATTR void VKAPI_CALL hook_vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(commandBuffer));
        if (it != g_devices.end()) {
            dev = it->second.get();
            track_dynamic_rendering_depth(dev, commandBuffer, pRenderingInfo);
        }
    }
    if (dev && dev->CmdBeginRendering) dev->CmdBeginRendering(commandBuffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL hook_vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(commandBuffer));
        if (it != g_devices.end()) {
            dev = it->second.get();
            track_dynamic_rendering_depth(dev, commandBuffer, pRenderingInfo);
        }
    }
    if (dev && dev->CmdBeginRenderingKHR) dev->CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL hook_vkCmdClearAttachments(
    VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment* pAttachments,
    uint32_t rectCount, const VkClearRect* pRects) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(commandBuffer));
        if (it != g_devices.end()) {
            dev = it->second.get();
            bool clears_depth = false;
            for (uint32_t i = 0; i < attachmentCount; ++i) {
                if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
                    clears_depth = true;
                    break;
                }
            }
            if (clears_depth && dev->swap_w > 0) {
                auto dit = dev->cmd_active_depth.find(commandBuffer);
                if (dit != dev->cmd_active_depth.end()) {
                    auto iit = dev->images.find(dit->second);
                    if (iit != dev->images.end()) {
                        dev->heuristic.record_clear(reinterpret_cast<uintptr_t>(dit->second),
                                                    iit->second.width, iit->second.height,
                                                    dev->swap_w, dev->swap_h);
                    }
                }
            }
        }
    }
    if (dev && dev->CmdClearAttachments) {
        dev->CmdClearAttachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(queue));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (!dev || !dev->QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return dev->QueuePresentKHR(queue, pPresentInfo);
    }

    VkPresentInfoKHR modified_present = *pPresentInfo;
    bool submitted_internal = false;

    {
        std::lock_guard lock(g_mutex);
        const VkSwapchainKHR sc = pPresentInfo->pSwapchains[0];
        const uint32_t img_idx = pPresentInfo->pImageIndices[0];
        auto scit = dev->swapchains.find(sc);
        if (scit != dev->swapchains.end() && img_idx < scit->second.images.size()) {
            const SwapchainInfo& sci = scit->second;
            const uint32_t w = sci.width;
            const uint32_t h = sci.height;
            const VkImage swap_img = sci.images[img_idx];
            uint32_t q_fam = dev->default_queue_family;
            if (auto qfit = dev->queue_families.find(queue); qfit != dev->queue_families.end()) {
                q_fam = qfit->second;
            }

            if (ensure_resources(dev, w, h, q_fam)) {
                dr::ensure_ipc_connected(g_config_view, g_ipc_ring);
                dr::SharedConfig* cfg = g_config_view.get();
                int64_t now_qpc = 0;
                const double fps = dr::tick_fps_and_telemetry(cfg, 3, dev->last_qpc, dev->qpc_freq,
                                                              dev->last_cycle_seq, dev->selected_depth_idx, now_qpc);

                const bool is_recording = (cfg && cfg->recording != 0 && g_ipc_ring.event_handle() != nullptr);
                const bool is_preview = (!is_recording && cfg && cfg->preview_enabled != 0 && g_ipc_ring.event_handle() != nullptr &&
                                         dr::should_capture_preview_now(now_qpc, dev->qpc_freq, dev->last_preview_qpc));
                const bool capture_now = is_recording || is_preview;
                const bool overlay = (cfg && cfg->overlay_enabled != 0);

                // 1. Drain completed readback slot(s)
                const uint32_t write_slot = static_cast<uint32_t>(dev->frame_idx % kStagingSlots);
                if (is_recording || (cfg && cfg->preview_enabled != 0 && g_ipc_ring.event_handle())) {
                    for (uint32_t off = 1; off <= kStagingSlots; ++off) {
                        const uint32_t read_slot = (write_slot + off) % kStagingSlots;
                        if (!dev->slot_pending[read_slot]) continue;
                        dev->WaitForFences(dev->device, 1, &dev->fences[read_slot], VK_TRUE, UINT64_MAX);

                        uint32_t color_bytes = 0;
                        bool is_rgba = false;
                        if (dev->slot_has_color[read_slot]) {
                            color_bytes = w * h * 4;
                            dev->color_pack_buf.resize(color_bytes);
                            std::memcpy(dev->color_pack_buf.data(), dev->color_mapped[read_slot], color_bytes);
                            is_rgba = dev->slot_is_rgba[read_slot];
                        }

                        uint32_t depth_bytes = 0;
                        if (dev->slot_has_depth[read_slot]) {
                            depth_bytes = w * h * dev->slot_depth_bpp[read_slot];
                            dev->depth_pack_buf.resize(depth_bytes);
                            std::memcpy(dev->depth_pack_buf.data(), dev->depth_mapped[read_slot], depth_bytes);
                        }

                        dr::push_frame_to_ring(g_ipc_ring, cfg, dev->frame_idx, dev->slot_qpc[read_slot], dev->qpc_freq,
                                               w, h, dev->slot_mode[read_slot], dev->slot_has_depth[read_slot],
                                               color_bytes ? dev->color_pack_buf.data() : nullptr, color_bytes,
                                               depth_bytes ? dev->depth_pack_buf.data() : nullptr, depth_bytes,
                                               is_rgba, !dev->slot_has_color[read_slot]);
                        dev->slot_pending[read_slot] = false;
                    }
                } else {
                    for (bool& p : dev->slot_pending) p = false;
                }

                // 2. Record current frame capture + overlay command buffer
                if (capture_now || overlay) {
                    if (dev->slot_pending[write_slot]) {
                        dev->WaitForFences(dev->device, 1, &dev->fences[write_slot], VK_TRUE, UINT64_MAX);
                        dev->slot_pending[write_slot] = false;
                    }
                    dev->ResetFences(dev->device, 1, &dev->fences[write_slot]);

                    VkCommandBuffer cmd = dev->cmd_bufs[write_slot];
                    dev->ResetCommandBuffer(cmd, 0);
                    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    dev->BeginCommandBuffer(cmd, &cbbi);

                    VkImageLayout swap_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                    if (capture_now) {
                        if (is_recording) {
                            cmd_barrier(dev, cmd, swap_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        swap_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                            swap_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

                            VkBufferImageCopy bic{};
                            bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            bic.imageExtent = {w, h, 1};
                            dev->CmdCopyImageToBuffer(cmd, swap_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dev->color_buf[write_slot], 1, &bic);
                        }

                        const bool want_depth = (cfg->depth_enabled != 0);
                        const uint32_t mode = static_cast<uint32_t>(cfg->depth_mode);
                        bool captured_depth = false;

                        dr::DepthCandidate cand{};
                        const bool has_cand = dr::select_depth_candidate(dev->heuristic.ranked_candidates(), cfg, dev->selected_depth_idx, cand);

                        if (want_depth && has_cand) {
                            VkImage depth_img = reinterpret_cast<VkImage>(cand.key);
                            auto iit = dev->images.find(depth_img);
                            if (iit != dev->images.end()) {
                                VkImageView d_view = get_depth_sample_view(dev, depth_img, iit->second.format);
                                if (d_view) {
                                    VkDescriptorImageInfo d_info{dev->depth_sampler, d_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                                    VkDescriptorImageInfo g_info{VK_NULL_HANDLE, dev->out_gray_view, VK_IMAGE_LAYOUT_GENERAL};
                                    VkDescriptorImageInfo r_info{VK_NULL_HANDLE, dev->out_rgba_view, VK_IMAGE_LAYOUT_GENERAL};
                                    VkWriteDescriptorSet writes[3]{};
                                    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dev->desc_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &d_info, nullptr, nullptr};
                                    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dev->desc_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &g_info, nullptr, nullptr};
                                    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dev->desc_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &r_info, nullptr, nullptr};
                                    dev->UpdateDescriptorSets(dev->device, 3, writes, 0, nullptr);

                                    const bool is_gray = (mode == static_cast<uint32_t>(dr::DepthMode::Gray8));
                                    VkImage out_img = is_gray ? dev->out_gray_img : dev->out_rgba_img;

                                    cmd_barrier(dev, cmd, depth_img, VK_IMAGE_ASPECT_DEPTH_BIT,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                                    cmd_barrier(dev, cmd, out_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                                0, VK_ACCESS_SHADER_WRITE_BIT,
                                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

                                    DepthConvertPC pc{};
                                    pc.width = w;
                                    pc.height = h;
                                    pc.mode = mode;
                                    pc.linearize = static_cast<uint32_t>(cfg->linearize);
                                    pc.reversed_z = static_cast<uint32_t>(cfg->reversed_z);
                                    pc.z_near = cfg->z_near;
                                    pc.z_far = cfg->z_far;

                                    dev->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->comp_pipeline);
                                    dev->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev->pipe_layout, 0, 1, &dev->desc_set, 0, nullptr);
                                    dev->CmdPushConstants(cmd, dev->pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                                    dev->CmdDispatch(cmd, (w + 15) / 16, (h + 15) / 16, 1);

                                    cmd_barrier(dev, cmd, out_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                                    cmd_barrier(dev, cmd, depth_img, VK_IMAGE_ASPECT_DEPTH_BIT,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

                                    VkBufferImageCopy dbic{};
                                    dbic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                    dbic.imageExtent = {w, h, 1};
                                    dev->CmdCopyImageToBuffer(cmd, out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dev->depth_buf[write_slot], 1, &dbic);
                                    captured_depth = true;
                                }
                            }
                        }

                        dev->slot_qpc[write_slot] = now_qpc;
                        dev->slot_mode[write_slot] = mode;
                        dev->slot_has_depth[write_slot] = captured_depth;
                        dev->slot_depth_bpp[write_slot] = (mode == static_cast<uint32_t>(dr::DepthMode::Gray8)) ? 1u : 4u;
                        dev->slot_is_rgba[write_slot] = is_rgba_swapchain_format(sci.format);
                        dev->slot_has_color[write_slot] = is_recording;
                        dev->slot_pending[write_slot] = true;
                    }

                    if (overlay) {
                        std::vector<uint8_t> rgba;
                        uint32_t bw = 0, bh = 0;
                        const double rec_fps = cfg ? (static_cast<double>(cfg->actual_rec_fps_x10) / 10.0) : 0.0;
                        dr::render_fps_overlay_rgba(fps, rec_fps, cfg->recording != 0, rgba, bw, bh);

                        if (bw <= w && bh <= h && ensure_overlay_image(dev, bw, bh)) {
                            std::memcpy(dev->overlay_buf_mapped, rgba.data(), rgba.size());

                            cmd_barrier(dev, cmd, dev->overlay_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                            VkBufferImageCopy obic{};
                            obic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            obic.imageExtent = {bw, bh, 1};
                            dev->CmdCopyBufferToImage(cmd, dev->overlay_buf, dev->overlay_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &obic);

                            cmd_barrier(dev, cmd, dev->overlay_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                            cmd_barrier(dev, cmd, swap_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                        swap_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                            swap_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                            int32_t dx = 0, dy = 0;
                            dr::compute_overlay_pixel_rect(bw, bh, w, h, static_cast<dr::OverlayCorner>(cfg->overlay_corner), dx, dy);
                            dx = std::max<int32_t>(0, dx);
                            dy = std::max<int32_t>(0, dy);

                            VkImageBlit blit{};
                            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            blit.srcOffsets[0] = {0, 0, 0};
                            blit.srcOffsets[1] = {static_cast<int32_t>(bw), static_cast<int32_t>(bh), 1};
                            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            blit.dstOffsets[0] = {dx, dy, 0};
                            blit.dstOffsets[1] = {dx + static_cast<int32_t>(bw), dy + static_cast<int32_t>(bh), 1};
                            dev->CmdBlitImage(cmd, dev->overlay_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
                        }
                    }

                    if (swap_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                        cmd_barrier(dev, cmd, swap_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                    swap_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                    }

                    dev->EndCommandBuffer(cmd);

                    std::vector<VkPipelineStageFlags> wait_stages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
                    si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                    si.pWaitDstStageMask = wait_stages.empty() ? nullptr : wait_stages.data();
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &cmd;
                    si.signalSemaphoreCount = 1;
                    si.pSignalSemaphores = &dev->overlay_sem;

                    if (dev->QueueSubmit(queue, 1, &si, dev->fences[write_slot]) == VK_SUCCESS) {
                        if (!capture_now) {
                            // If not capturing, wait on overlay fence next time this slot is used
                            dev->WaitForFences(dev->device, 1, &dev->fences[write_slot], VK_TRUE, UINT64_MAX);
                        }
                        modified_present.waitSemaphoreCount = 1;
                        modified_present.pWaitSemaphores = &dev->overlay_sem;
                        submitted_internal = true;
                    }
                }

                dev->frame_idx++;
                dev->heuristic.end_frame_prune();
                dev->heuristic.begin_frame(dev->frame_idx);
            }
        }
    }

    return dev->QueuePresentKHR(queue, submitted_internal ? &modified_present : pPresentInfo);
}

// --- Instance & Device Lifecycle ---

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    auto* layer_info = const_cast<VkLayerInstanceCreateInfo*>(
        reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (layer_info && !(layer_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                           layer_info->function == VK_LAYER_LINK_INFO)) {
        layer_info = const_cast<VkLayerInstanceCreateInfo*>(
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(layer_info->pNext));
    }
    if (!layer_info || !layer_info->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = layer_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layer_info->u.pLayerInfo = layer_info->u.pLayerInfo->pNext;

    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    auto inst = std::make_unique<InstanceData>();
    inst->instance = *pInstance;
    inst->next_gipa = next_gipa;
    inst->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*pInstance, "vkDestroyInstance"));
    inst->GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(next_gipa(*pInstance, "vkGetPhysicalDeviceMemoryProperties"));
    inst->GetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(next_gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    std::lock_guard lock(g_mutex);
    g_instances[dispatch_key(*pInstance)] = std::move(inst);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    std::unique_ptr<InstanceData> inst;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) {
            inst = std::move(it->second);
            g_instances.erase(it);
        }
    }
    if (inst && inst->DestroyInstance) inst->DestroyInstance(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto* layer_info = const_cast<VkLayerDeviceCreateInfo*>(
        reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (layer_info && !(layer_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                           layer_info->function == VK_LAYER_LINK_INFO)) {
        layer_info = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(layer_info->pNext));
    }
    if (!layer_info || !layer_info->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = layer_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = layer_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    layer_info->u.pLayerInfo = layer_info->u.pLayerInfo->pNext;

    InstanceData* inst_ptr = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_instances.find(dispatch_key(physicalDevice));
        if (it != g_instances.end()) inst_ptr = it->second.get();
    }
    if (!inst_ptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto next_create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(inst_ptr->instance, "vkCreateDevice"));
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    auto dev = std::make_unique<DeviceData>();
    dev->device = *pDevice;
    dev->physical_device = physicalDevice;
    dev->inst = inst_ptr;
    dev->next_gdpa = next_gdpa;
    if (pCreateInfo->queueCreateInfoCount > 0 && pCreateInfo->pQueueCreateInfos) {
        dev->default_queue_family = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    }

#define LOAD_DEV(name) dev->name = reinterpret_cast<PFN_vk##name>(next_gdpa(*pDevice, "vk" #name))
    LOAD_DEV(DestroyDevice);
    LOAD_DEV(GetDeviceQueue);
    LOAD_DEV(CreateImage);
    LOAD_DEV(DestroyImage);
    LOAD_DEV(CreateImageView);
    LOAD_DEV(DestroyImageView);
    LOAD_DEV(CreateBuffer);
    LOAD_DEV(DestroyBuffer);
    LOAD_DEV(AllocateMemory);
    LOAD_DEV(FreeMemory);
    LOAD_DEV(BindBufferMemory);
    LOAD_DEV(BindImageMemory);
    LOAD_DEV(MapMemory);
    LOAD_DEV(UnmapMemory);
    LOAD_DEV(GetBufferMemoryRequirements);
    LOAD_DEV(GetImageMemoryRequirements);
    LOAD_DEV(CreateCommandPool);
    LOAD_DEV(DestroyCommandPool);
    LOAD_DEV(AllocateCommandBuffers);
    LOAD_DEV(FreeCommandBuffers);
    LOAD_DEV(BeginCommandBuffer);
    LOAD_DEV(EndCommandBuffer);
    LOAD_DEV(ResetCommandBuffer);
    LOAD_DEV(QueueSubmit);
    LOAD_DEV(QueueWaitIdle);
    LOAD_DEV(DeviceWaitIdle);
    LOAD_DEV(CreateFence);
    LOAD_DEV(DestroyFence);
    LOAD_DEV(WaitForFences);
    LOAD_DEV(ResetFences);
    LOAD_DEV(CreateSemaphore);
    LOAD_DEV(DestroySemaphore);
    LOAD_DEV(CreateSampler);
    LOAD_DEV(DestroySampler);
    LOAD_DEV(CreateShaderModule);
    LOAD_DEV(DestroyShaderModule);
    LOAD_DEV(CreateDescriptorSetLayout);
    LOAD_DEV(DestroyDescriptorSetLayout);
    LOAD_DEV(CreatePipelineLayout);
    LOAD_DEV(DestroyPipelineLayout);
    LOAD_DEV(CreateComputePipelines);
    LOAD_DEV(DestroyPipeline);
    LOAD_DEV(CreateDescriptorPool);
    LOAD_DEV(DestroyDescriptorPool);
    LOAD_DEV(AllocateDescriptorSets);
    LOAD_DEV(UpdateDescriptorSets);
    LOAD_DEV(CmdBindPipeline);
    LOAD_DEV(CmdBindDescriptorSets);
    LOAD_DEV(CmdPushConstants);
    LOAD_DEV(CmdDispatch);
    LOAD_DEV(CmdPipelineBarrier);
    LOAD_DEV(CmdCopyImage);
    LOAD_DEV(CmdCopyImageToBuffer);
    LOAD_DEV(CmdCopyBufferToImage);
    LOAD_DEV(CmdBlitImage);
    LOAD_DEV(CmdClearAttachments);
    LOAD_DEV(CmdClearDepthStencilImage);
    LOAD_DEV(CmdBeginRenderPass);
    LOAD_DEV(CmdBeginRendering);
    LOAD_DEV(CmdBeginRenderingKHR);
    LOAD_DEV(CreateRenderPass);
    LOAD_DEV(CreateRenderPass2);
    LOAD_DEV(CreateRenderPass2KHR);
    LOAD_DEV(DestroyRenderPass);
    LOAD_DEV(CreateFramebuffer);
    LOAD_DEV(DestroyFramebuffer);
    LOAD_DEV(CreateSwapchainKHR);
    LOAD_DEV(DestroySwapchainKHR);
    LOAD_DEV(GetSwapchainImagesKHR);
    LOAD_DEV(QueuePresentKHR);
#undef LOAD_DEV

    std::lock_guard lock(g_mutex);
    g_devices[dispatch_key(*pDevice)] = std::move(dev);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    std::unique_ptr<DeviceData> dev;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            dev = std::move(it->second);
            g_devices.erase(it);
        }
    }
    if (dev) {
        destroy_all_internal_resources(dev.get());
        if (dev->DestroyDevice) dev->DestroyDevice(device, pAllocator);
    }
}

PFN_vkVoidFunction lookup_device_proc(const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkGetDeviceQueue);
    if (std::strcmp(pName, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateImage);
    if (std::strcmp(pName, "vkDestroyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroyImage);
    if (std::strcmp(pName, "vkCreateImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateImageView);
    if (std::strcmp(pName, "vkDestroyImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroyImageView);
    if (std::strcmp(pName, "vkCreateRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateRenderPass);
    if (std::strcmp(pName, "vkCreateRenderPass2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateRenderPass2);
    if (std::strcmp(pName, "vkCreateRenderPass2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateRenderPass2KHR);
    if (std::strcmp(pName, "vkDestroyRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroyRenderPass);
    if (std::strcmp(pName, "vkCreateFramebuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateFramebuffer);
    if (std::strcmp(pName, "vkDestroyFramebuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroyFramebuffer);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroySwapchainKHR);
    if (std::strcmp(pName, "vkCmdBeginRenderPass") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCmdBeginRenderPass);
    if (std::strcmp(pName, "vkCmdBeginRendering") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCmdBeginRendering);
    if (std::strcmp(pName, "vkCmdBeginRenderingKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCmdBeginRenderingKHR);
    if (std::strcmp(pName, "vkCmdClearAttachments") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCmdClearAttachments);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkQueuePresentKHR);
    return nullptr;
}

} // namespace

#pragma comment(linker, "/EXPORT:vkGetDeviceProcAddr")
#pragma comment(linker, "/EXPORT:vkGetInstanceProcAddr")
#pragma comment(linker, "/EXPORT:vkNegotiateLoaderLayerInterfaceVersion")

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (pName && std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    }
    if (auto fn = lookup_device_proc(pName)) return fn;

    DeviceData* dev = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) dev = it->second.get();
    }
    if (dev && dev->next_gdpa) return dev->next_gdpa(device, pName);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkDestroyInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(hook_vkCreateDevice);
    if (auto fn = lookup_device_proc(pName)) return fn;

    InstanceData* inst = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) inst = it->second.get();
    }
    if (inst && inst->next_gipa) return inst->next_gipa(instance, pName);
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_ipc_ring.close();
        g_config_view.close();
    }
    return TRUE;
}
