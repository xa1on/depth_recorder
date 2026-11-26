#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <cstdint>
#include <vector>

static const uint32_t kVertSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000051,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000003b,0x0000003d,0x0000004c,
    0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,
    0x0000000c,0x00000070,0x00030005,0x00000017,0x00000063,0x00030005,0x00000021,0x0000007a,
    0x00030005,0x00000023,0x00004350,0x00040006,0x00000023,0x00000000,0x00000074,0x00030005,
    0x00000025,0x00006370,0x00060005,0x00000039,0x505f6c67,0x65567265,0x78657472,0x00000000,
    0x00060006,0x00000039,0x00000000,0x505f6c67,0x7469736f,0x006e6f69,0x00070006,0x00000039,
    0x00000001,0x505f6c67,0x746e696f,0x657a6953,0x00000000,0x00070006,0x00000039,0x00000002,
    0x435f6c67,0x4470696c,0x61747369,0x0065636e,0x00070006,0x00000039,0x00000003,0x435f6c67,
    0x446c6c75,0x61747369,0x0065636e,0x00030005,0x0000003b,0x00000000,0x00060005,0x0000003d,
    0x565f6c67,0x65747265,0x646e4978,0x00007865,0x00050005,0x0000004c,0x4374756f,0x726f6c6f,
    0x00000000,0x00030047,0x00000023,0x00000002,0x00050048,0x00000023,0x00000000,0x00000023,
    0x00000000,0x00030047,0x00000039,0x00000002,0x00050048,0x00000039,0x00000000,0x0000000b,
    0x00000000,0x00050048,0x00000039,0x00000001,0x0000000b,0x00000001,0x00050048,0x00000039,
    0x00000002,0x0000000b,0x00000003,0x00050048,0x00000039,0x00000003,0x0000000b,0x00000004,
    0x00040047,0x0000003d,0x0000000b,0x0000002a,0x00040047,0x0000004c,0x0000001e,0x00000000,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000002,0x00040015,0x00000008,0x00000020,0x00000000,
    0x0004002b,0x00000008,0x00000009,0x00000003,0x0004001c,0x0000000a,0x00000007,0x00000009,
    0x00040020,0x0000000b,0x00000007,0x0000000a,0x0004002b,0x00000006,0x0000000d,0x00000000,
    0x0004002b,0x00000006,0x0000000e,0xbf333333,0x0005002c,0x00000007,0x0000000f,0x0000000d,
    0x0000000e,0x0004002b,0x00000006,0x00000010,0x3f333333,0x0005002c,0x00000007,0x00000011,
    0x00000010,0x00000010,0x0005002c,0x00000007,0x00000012,0x0000000e,0x00000010,0x0006002c,
    0x0000000a,0x00000013,0x0000000f,0x00000011,0x00000012,0x00040017,0x00000014,0x00000006,
    0x00000003,0x0004001c,0x00000015,0x00000014,0x00000009,0x00040020,0x00000016,0x00000007,
    0x00000015,0x0004002b,0x00000006,0x00000018,0x3f800000,0x0004002b,0x00000006,0x00000019,
    0x3e4ccccd,0x0006002c,0x00000014,0x0000001a,0x00000018,0x00000019,0x00000019,0x0006002c,
    0x00000014,0x0000001b,0x00000019,0x00000018,0x00000019,0x0004002b,0x00000006,0x0000001c,
    0x3ecccccd,0x0006002c,0x00000014,0x0000001d,0x00000019,0x0000001c,0x00000018,0x0006002c,
    0x00000015,0x0000001e,0x0000001a,0x0000001b,0x0000001d,0x0004001c,0x0000001f,0x00000006,
    0x00000009,0x00040020,0x00000020,0x00000007,0x0000001f,0x0004002b,0x00000006,0x00000022,
    0x3e19999a,0x0003001e,0x00000023,0x00000006,0x00040020,0x00000024,0x00000009,0x00000023,
    0x0004003b,0x00000024,0x00000025,0x00000009,0x00040015,0x00000026,0x00000020,0x00000001,
    0x0004002b,0x00000026,0x00000027,0x00000000,0x00040020,0x00000028,0x00000009,0x00000006,
    0x0004002b,0x00000006,0x0000002e,0x3f000000,0x0004002b,0x00000006,0x0000002f,0x3f59999a,
    0x00040017,0x00000036,0x00000006,0x00000004,0x0004002b,0x00000008,0x00000037,0x00000001,
    0x0004001c,0x00000038,0x00000006,0x00000037,0x0006001e,0x00000039,0x00000036,0x00000006,
    0x00000038,0x00000038,0x00040020,0x0000003a,0x00000003,0x00000039,0x0004003b,0x0000003a,
    0x0000003b,0x00000003,0x00040020,0x0000003c,0x00000001,0x00000026,0x0004003b,0x0000003c,
    0x0000003d,0x00000001,0x00040020,0x0000003f,0x00000007,0x00000007,0x00040020,0x00000043,
    0x00000007,0x00000006,0x00040020,0x00000049,0x00000003,0x00000036,0x00040020,0x0000004b,
    0x00000003,0x00000014,0x0004003b,0x0000004b,0x0000004c,0x00000003,0x00040020,0x0000004e,
    0x00000007,0x00000014,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,
    0x00000005,0x0004003b,0x0000000b,0x0000000c,0x00000007,0x0004003b,0x00000016,0x00000017,
    0x00000007,0x0004003b,0x00000020,0x00000021,0x00000007,0x0003003e,0x0000000c,0x00000013,
    0x0003003e,0x00000017,0x0000001e,0x00050041,0x00000028,0x00000029,0x00000025,0x00000027,
    0x0004003d,0x00000006,0x0000002a,0x00000029,0x0006000c,0x00000006,0x0000002b,0x00000001,
    0x0000000d,0x0000002a,0x00050085,0x00000006,0x0000002c,0x00000022,0x0000002b,0x00050081,
    0x00000006,0x0000002d,0x00000019,0x0000002c,0x00050041,0x00000028,0x00000030,0x00000025,
    0x00000027,0x0004003d,0x00000006,0x00000031,0x00000030,0x0006000c,0x00000006,0x00000032,
    0x00000001,0x0000000e,0x00000031,0x00050085,0x00000006,0x00000033,0x00000022,0x00000032,
    0x00050083,0x00000006,0x00000034,0x0000002f,0x00000033,0x00060050,0x0000001f,0x00000035,
    0x0000002d,0x0000002e,0x00000034,0x0003003e,0x00000021,0x00000035,0x0004003d,0x00000026,
    0x0000003e,0x0000003d,0x00050041,0x0000003f,0x00000040,0x0000000c,0x0000003e,0x0004003d,
    0x00000007,0x00000041,0x00000040,0x0004003d,0x00000026,0x00000042,0x0000003d,0x00050041,
    0x00000043,0x00000044,0x00000021,0x00000042,0x0004003d,0x00000006,0x00000045,0x00000044,
    0x00050051,0x00000006,0x00000046,0x00000041,0x00000000,0x00050051,0x00000006,0x00000047,
    0x00000041,0x00000001,0x00070050,0x00000036,0x00000048,0x00000046,0x00000047,0x00000045,
    0x00000018,0x00050041,0x00000049,0x0000004a,0x0000003b,0x00000027,0x0003003e,0x0000004a,
    0x00000048,0x0004003d,0x00000026,0x0000004d,0x0000003d,0x00050041,0x0000004e,0x0000004f,
    0x00000017,0x0000004d,0x0004003d,0x00000014,0x00000050,0x0000004f,0x0003003e,0x0000004c,
    0x00000050,0x000100fd,0x00010038
};

static const uint32_t kFragSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000013,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000c,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00040005,0x0000000c,
    0x6f436e69,0x00726f6c,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000c,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,
    0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040017,0x0000000a,
    0x00000006,0x00000003,0x00040020,0x0000000b,0x00000001,0x0000000a,0x0004003b,0x0000000b,
    0x0000000c,0x00000001,0x0004002b,0x00000006,0x0000000e,0x3f800000,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x0000000a,0x0000000d,
    0x0000000c,0x00050051,0x00000006,0x0000000f,0x0000000d,0x00000000,0x00050051,0x00000006,
    0x00000010,0x0000000d,0x00000001,0x00050051,0x00000006,0x00000011,0x0000000d,0x00000002,
    0x00070050,0x00000007,0x00000012,0x0000000f,0x00000010,0x00000011,0x0000000e,0x0003003e,
    0x00000009,0x00000012,0x000100fd,0x00010038
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SampleVkDepthWnd";
    RegisterClassExW(&wc);

    RECT rc{0, 0, 640, 480};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sample Vulkan Depth", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return 1;

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = hInst;
    sci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface) != VK_SUCCESS) return 1;

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (gpu_count == 0) return 1;
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    VkPhysicalDevice gpu = gpus[0];

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, qfs.data());
    uint32_t gfx_qf = 0;
    for (uint32_t i = 0; i < qf_count; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { gfx_qf = i; break; }
    }

    float q_prio = 1.0f;
    VkDeviceQueueCreateInfo dqci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dqci.queueFamilyIndex = gfx_qf;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &q_prio;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(gpu, &dci, nullptr, &device) != VK_SUCCESS) return 1;

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, gfx_qf, 0, &queue);

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = 2;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = {640, 480};
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain) != VK_SUCCESS) return 1;

    uint32_t sc_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &sc_count, nullptr);
    std::vector<VkImage> sc_images(sc_count);
    vkGetSwapchainImagesKHR(device, swapchain, &sc_count, sc_images.data());

    std::vector<VkImageView> sc_views(sc_count);
    for (uint32_t i = 0; i < sc_count; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = sc_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &ivci, nullptr, &sc_views[i]);
    }

    // Depth image (D32_SFLOAT)
    VkImageCreateInfo dici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dici.imageType = VK_IMAGE_TYPE_2D;
    dici.format = VK_FORMAT_D32_SFLOAT;
    dici.extent = {640, 480, 1};
    dici.mipLevels = 1;
    dici.arrayLayers = 1;
    dici.samples = VK_SAMPLE_COUNT_1_BIT;
    dici.tiling = VK_IMAGE_TILING_OPTIMAL;
    dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage depth_img = VK_NULL_HANDLE;
    vkCreateImage(device, &dici, nullptr, &depth_img);

    VkMemoryRequirements dreq{};
    vkGetImageMemoryRequirements(device, depth_img, &dreq);
    VkPhysicalDeviceMemoryProperties mprops{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mprops);
    uint32_t dmem_idx = 0;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        if ((dreq.memoryTypeBits & (1u << i)) && (mprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            dmem_idx = i;
            break;
        }
    }
    VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    dmai.allocationSize = dreq.size;
    dmai.memoryTypeIndex = dmem_idx;
    VkDeviceMemory depth_mem = VK_NULL_HANDLE;
    vkAllocateMemory(device, &dmai, nullptr, &depth_mem);
    vkBindImageMemory(device, depth_img, depth_mem, 0);

    VkImageViewCreateInfo divci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    divci.image = depth_img;
    divci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    divci.format = VK_FORMAT_D32_SFLOAT;
    divci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depth_view = VK_NULL_HANDLE;
    vkCreateImageView(device, &divci, nullptr, &depth_view);

    // RenderPass
    VkAttachmentDescription atts[2]{};
    atts[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    atts[1].format = VK_FORMAT_D32_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference col_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dep_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &col_ref;
    sub.pDepthStencilAttachment = &dep_ref;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2;
    rpci.pAttachments = atts;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(device, &rpci, nullptr, &rp);

    std::vector<VkFramebuffer> fbs(sc_count);
    for (uint32_t i = 0; i < sc_count; ++i) {
        VkImageView views[] = {sc_views[i], depth_view};
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = rp;
        fbci.attachmentCount = 2;
        fbci.pAttachments = views;
        fbci.width = 640;
        fbci.height = 480;
        fbci.layers = 1;
        vkCreateFramebuffer(device, &fbci, nullptr, &fbs[i]);
    }

    // Pipeline
    VkShaderModuleCreateInfo vs_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kVertSpv), kVertSpv};
    VkShaderModuleCreateInfo fs_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kFragSpv), kFragSpv};
    VkShaderModule vs_mod = VK_NULL_HANDLE, fs_mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &vs_ci, nullptr, &vs_mod);
    vkCreateShaderModule(device, &fs_ci, nullptr, &fs_mod);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &plci, nullptr, &pipe_layout);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_mod, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs_mod, "main"};

    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, 640, 480, 0, 1};
    VkRect2D sciss{{0, 0}, {640, 480}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sciss};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba};

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cbs;
    gpci.layout = pipe_layout;
    gpci.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, gfx_qf};
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &cpci, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &cmd);

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore img_avail = VK_NULL_HANDLE, render_done = VK_NULL_HANDLE;
    vkCreateSemaphore(device, &semci, nullptr, &img_avail);
    vkCreateSemaphore(device, &semci, nullptr, &render_done);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fci, nullptr, &fence);

    MSG msg{};
    float t = 0.0f;
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        t += 0.05f;

        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);

        uint32_t idx = 0;
        if (vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, img_avail, VK_NULL_HANDLE, &idx) != VK_SUCCESS) continue;

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &cbbi);

        VkClearValue clears[2]{};
        clears[0].color = {{0.08f, 0.09f, 0.12f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = rp;
        rpbi.framebuffer = fbs[idx];
        rpbi.renderArea = {{0, 0}, {640, 480}};
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &t);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &img_avail;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &render_done;
        vkQueueSubmit(queue, 1, &si, fence);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &idx;
        vkQueuePresentKHR(queue, &pi);
    }

    vkDeviceWaitIdle(device);
    return 0;
}
