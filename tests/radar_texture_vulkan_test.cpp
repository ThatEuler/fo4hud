// Optional GPU smoke test: exercise the layer's actual texture loading,
// ImGui registration, map geometry, upload, and shutdown without a game window.
#include "../layer.cpp"
#include <cassert>
#include <dlfcn.h>

int main() {
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(loader);
#define LOAD(name) auto next_##name = reinterpret_cast<PFN_##name>(dlsym(loader, #name)); assert(next_##name)
    LOAD(vkCreateInstance); LOAD(vkDestroyInstance); LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties); LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice); LOAD(vkGetDeviceQueue); LOAD(vkDeviceWaitIdle);
    LOAD(vkCreateRenderPass); LOAD(vkDestroyRenderPass); LOAD(vkGetInstanceProcAddr);
    LOAD(vkGetDeviceProcAddr);
    VkInstance instance_handle = VK_NULL_HANDLE;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME};
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    assert(next_vkCreateInstance(&instance_info, nullptr, &instance_handle) == VK_SUCCESS);
    uint32_t count = 0;
    assert(next_vkEnumeratePhysicalDevices(instance_handle, &count, nullptr) == VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> physical_devices(count);
    assert(next_vkEnumeratePhysicalDevices(instance_handle, &count, physical_devices.data()) == VK_SUCCESS);
    const VkPhysicalDevice physical = physical_devices[0];
    next_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    next_vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    assert(family < count);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device_handle = VK_NULL_HANDLE;
    assert(next_vkCreateDevice(physical, &device_info, nullptr, &device_handle) == VK_SUCCESS);
    VkQueue queue = VK_NULL_HANDLE;
    next_vkGetDeviceQueue(device_handle, family, 0, &queue);
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    assert(next_vkCreateRenderPass(device_handle, &pass_info, nullptr, &pass) == VK_SUCCESS);

    struct LoaderContext {
        VkInstance instance;
        VkDevice device;
        PFN_vkGetInstanceProcAddr instance_proc;
        PFN_vkGetDeviceProcAddr device_proc;
    } context{instance_handle, device_handle, next_vkGetInstanceProcAddr, next_vkGetDeviceProcAddr};
    device.handle = device_handle;
    device.wait_idle = next_vkDeviceWaitIdle;
    for (int cycle = 0; cycle < 2; ++cycle) {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().DisplaySize = ImVec2(600, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        assert(ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, [](const char* name, void* data) {
            auto& c = *static_cast<LoaderContext*>(data);
            auto function = c.device_proc(c.device, name);
            if (!function) function = c.instance_proc(c.instance, name);
            if (!function) std::fprintf(stderr, "Missing Vulkan function: %s\n", name);
            return function;
        }, &context));
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = instance_handle;
        init.PhysicalDevice = physical;
        init.Device = device_handle;
        init.QueueFamily = family;
        init.Queue = queue;
        init.DescriptorPoolSize = 32;
        init.MinImageCount = init.ImageCount = 2;
        init.PipelineInfoMain.RenderPass = pass;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        assert(ImGui_ImplVulkan_Init(&init));
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        Swapchain sc;
        Fo4MapInfo map{-135168, 102400, 114688, -147456, "textures/interface/pip-boy/worldmap_d.dds",
                       55.0f / 2048, 55.0f / 2048, 1990.0f / 2048, 1990.0f / 2048};
        ImTextureData* texture = radar_map_texture(sc, map.texture);
        assert(texture && texture->Width == 2048 && texture->Height == 2048);
        assert(radar_map_texture(sc, map.texture) == texture);
        auto* draw = ImGui::GetBackgroundDrawList();
        draw_radar_map(draw, texture, map, {-80000, 90000, 0.7f, 20000, 250}, {300, 300}, 0.85f);
        const auto expected_polygon = fo4hud_radar_map_polygon(map, {-80000, 90000, 0.7f, 20000, 250});
        assert(expected_polygon.size() >= 3);
        assert(draw->VtxBuffer.Size == int(expected_polygon.size()));
        ImGui::Render();
        for (ImTextureData* pending : ImGui::GetPlatformIO().Textures)
            ImGui_ImplVulkan_UpdateTexture(pending);
        assert(texture->Status == ImTextureStatus_OK && texture->TexID != ImTextureID_Invalid);
        ImGui::NewFrame();
        // Interior captures replace their pixels without allocating another
        // descriptor each refresh, and retire a resized texture after GPU idle.
        Fo4LocalMapFrame local;
        local.width = 3; local.height = 2; local.sequence = 1;
        local.pixels.assign(3 * 2 * 4, 255);
        auto* local_texture = radar_local_map_texture(sc, local);
        assert(local_texture && local_texture->Status == ImTextureStatus_WantCreate);
        ImGui_ImplVulkan_UpdateTexture(local_texture);
        const ImTextureID descriptor = local_texture->TexID;
        local.sequence = 2; local.pixels[0] = 17;
        assert(radar_local_map_texture(sc, local) == local_texture);
        assert(local_texture->Status == ImTextureStatus_WantUpdates);
        ImGui_ImplVulkan_UpdateTexture(local_texture);
        assert(local_texture->Status == ImTextureStatus_OK && local_texture->TexID == descriptor);
        local.width = 4; local.height = 3; local.sequence = 3;
        local.pixels.assign(4 * 3 * 4, 100);
        local_texture = radar_local_map_texture(sc, local);
        assert(local_texture->Status == ImTextureStatus_WantCreate);
        ImGui_ImplVulkan_UpdateTexture(local_texture);
        assert(local_texture->Width == 4 && local_texture->Height == 3 && local_texture->Status == ImTextureStatus_OK);
        local.pixels.pop_back();
        assert(!radar_local_map_texture(sc, local));
        ImGui::GetBackgroundDrawList()->AddImage(local_texture->GetTexRef(), {0, 0}, {40, 30});
        ImGui::Render();
        assert(next_vkDeviceWaitIdle(device_handle) == VK_SUCCESS);
        ImGui_ImplVulkan_Shutdown();
        assert(texture->BackendUserData == nullptr);
        assert(local_texture->BackendUserData == nullptr);
        ImGui::UnregisterUserTexture(texture);
        ImGui::UnregisterUserTexture(local_texture);
        sc.map_textures.clear();
        sc.local_map_texture.reset();
        ImGui::DestroyContext();
    }
    next_vkDestroyRenderPass(device_handle, pass, nullptr);
    next_vkDestroyDevice(device_handle, nullptr);
    device = {};
    next_vkDestroyInstance(instance_handle, nullptr);
    dlclose(loader);
}
