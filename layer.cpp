#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include "wayland_input.h"
#include "wine_input_hooks.h"
#include "game_entities.h"
#include "local_map.h"
#include "lockpick_overlay.h"
#include "hacking_overlay.h"
#include "companion_overlay.h"
#include "dialogue_affinity_overlay.h"
#include "trap_overlay.h"
#include "game_combat.h"
#include "game_control.h"
#include "game_workshop.h"
#include "workshop_selection.h"
#include "settings.h"
#include "logging.h"
#include <vulkan/vk_layer.h>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {
struct Instance {
    VkInstance handle = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next = nullptr;
};
struct Device {
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr next = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR get_images = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkDeviceWaitIdle wait_idle = nullptr;
    PFN_vkGetDeviceQueue get_queue = nullptr;
    PFN_vkCreateCommandPool create_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_commands = nullptr;
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    PFN_vkResetCommandBuffer reset_command = nullptr;
    PFN_vkBeginCommandBuffer begin_command = nullptr;
    PFN_vkEndCommandBuffer end_command = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_pass = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_pass = nullptr;
    PFN_vkCreateSemaphore create_semaphore = nullptr;
    PFN_vkDestroySemaphore destroy_semaphore = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkWaitForFences wait_fences = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkCreateImageView create_view = nullptr;
    PFN_vkDestroyImageView destroy_view = nullptr;
    PFN_vkCreateRenderPass create_pass = nullptr;
    PFN_vkDestroyRenderPass destroy_pass = nullptr;
    PFN_vkCreateFramebuffer create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_capabilities = nullptr;
    std::unordered_map<VkQueue, uint32_t> queues;
};
struct Frame {
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
};
struct RadarWindowFade {
    float opacity = 1.0f;
    std::vector<std::pair<int, int>> opaque_ranges;

    void keep(ImDrawList* draw, int first_vertex) {
        opaque_ranges.emplace_back(first_vertex, draw->VtxBuffer.Size);
    }
    void apply(ImDrawList* draw, bool radar_selected, bool hovered, float delta_time) {
        if (!radar_selected) {
            opacity = 1.0f;
            return;
        }
        const float target = hovered ? 1.0f : 0.0f;
        const float duration = hovered ? 0.15f : 0.25f;
        const float step = std::isfinite(delta_time) ? std::max(0.0f, delta_time) / duration : 0.0f;
        opacity += std::clamp(target - opacity, -step, step);
        // Fade the finished window geometry instead of Style.Alpha: ImGui
        // skips windows at zero style alpha, including our persistent radar.
        // Raw draw-list colors (compass and counters) must fade as well.
        size_t range = 0;
        for (int i = 0; i < draw->VtxBuffer.Size; ++i) {
            while (range < opaque_ranges.size() && i >= opaque_ranges[range].second) ++range;
            if (range < opaque_ranges.size() && i >= opaque_ranges[range].first) continue;
            ImU32& color = draw->VtxBuffer[i].col;
            const unsigned alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
            const unsigned faded = static_cast<unsigned>(alpha * opacity + 0.5f);
            color = (color & ~IM_COL32_A_MASK) | (faded << IM_COL32_A_SHIFT);
        }
    }
};

struct Swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<Frame> frames;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    ImGuiContext* context = nullptr;
    ImFont* label_font = nullptr;
    RadarWindowFade radar_fade;
    std::unordered_map<std::string, std::unique_ptr<ImTextureData>> map_textures;
    std::unique_ptr<ImTextureData> local_map_texture;
    uint64_t local_map_sequence = 0;
    bool backend_initialized = false;
    bool renderable = false;
    bool capturable = false;
};
std::mutex mutex;
Instance instance;
Device device;
VkResult allocate_overlay_commands(VkDevice handle, const VkCommandBufferAllocateInfo* info,
                                  VkCommandBuffer* commands) {
    const VkResult result = device.allocate_commands(handle, info, commands);
    if (result != VK_SUCCESS) return result;
    // Calls made down the layer chain bypass the loader's allocation wrapper.
    // Initialize dispatch data on our extra buffers before another layer uses them.
    if (device.set_loader_data)
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            const VkResult initialized = device.set_loader_data(handle, commands[i]);
            if (initialized != VK_SUCCESS) return initialized;
        }
    return VK_SUCCESS;
}
std::unordered_map<VkSwapchainKHR, Swapchain> swapchains;
VkSwapchainKHR active_swapchain = VK_NULL_HANDLE;
VkSurfaceKHR wayland_surface_handle = VK_NULL_HANDLE;
OverlaySettings overlay_settings;
bool settings_loaded = false;
bool settings_dirty = false;
std::string settings_error;
std::string imgui_ini_path;
std::chrono::steady_clock::time_point settings_changed_at{};

void flush_settings() {
    // Check the hotkey state even at shutdown, so a final hide/show without
    // another presented frame is saved too.
    if (settings_loaded && overlay_settings.overlay_visible != fo4hud_overlay_visible()) {
        overlay_settings.overlay_visible = fo4hud_overlay_visible();
        settings_dirty = true;
    }
    if (!settings_dirty) return;
    if (fo4hud_save_settings(overlay_settings, settings_error))
        settings_dirty = false;
    else
        settings_changed_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}
void error(const char* message, VkResult result = VK_SUCCESS) {
    fo4hud_log("layer", "fo4hud: %s (%d)", message, int(result));
}
void trace(const char* name, VkResult result = VK_SUCCESS) {
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("layer", "fo4hud [%ld]: %s (%d)", long(getpid()), name, int(result));
}
template <typename T> T load_device(const char* name) {
    return reinterpret_cast<T>(device.next(device.handle, name));
}

template <typename T> T load_instance(const char* name) {
    return reinterpret_cast<T>(instance.next(instance.handle, name));
}

VkLayerInstanceCreateInfo* instance_link(const VkInstanceCreateInfo* info) {
    for (auto* p = reinterpret_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if (link->function == VK_LAYER_LINK_INFO) return link;
        }
    return nullptr;
}
VkLayerDeviceCreateInfo* device_link(const VkDeviceCreateInfo* info) {
    for (auto* p = reinterpret_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if (link->function == VK_LAYER_LINK_INFO) return link;
        }
    return nullptr;
}

void destroy_overlay(Swapchain& sc) {
    flush_settings();
    if (sc.pool || sc.pass || sc.context) device.wait_idle(device.handle);
    if (sc.context) {
        ImGui::SetCurrentContext(sc.context);
        if (sc.backend_initialized) ImGui_ImplVulkan_Shutdown();
        for (auto& entry : sc.map_textures)
            if (entry.second) ImGui::UnregisterUserTexture(entry.second.get());
        sc.map_textures.clear();
        if (sc.local_map_texture) ImGui::UnregisterUserTexture(sc.local_map_texture.get());
        sc.local_map_texture.reset();
        sc.local_map_sequence = 0;
        ImGui::DestroyContext(sc.context);
        sc.context = nullptr;
        sc.label_font = nullptr;
        sc.backend_initialized = false;
    }
    for (auto& frame : sc.frames) {
        if (frame.fence) device.destroy_fence(device.handle, frame.fence, nullptr);
        if (frame.done) device.destroy_semaphore(device.handle, frame.done, nullptr);
        if (frame.framebuffer) device.destroy_framebuffer(device.handle, frame.framebuffer, nullptr);
        if (frame.view) device.destroy_view(device.handle, frame.view, nullptr);
    }
    if (sc.pool) device.destroy_pool(device.handle, sc.pool, nullptr);
    if (sc.pass) device.destroy_pass(device.handle, sc.pass, nullptr);
    sc.frames.clear();
    sc.pool = VK_NULL_HANDLE;
    sc.pass = VK_NULL_HANDLE;
    if (active_swapchain == sc.handle) active_swapchain = VK_NULL_HANDLE;
}

bool init_overlay(Swapchain& sc, VkQueue queue, uint32_t family) {
    if (active_swapchain != VK_NULL_HANDLE) return false;
    uint32_t count = 0;
    if (device.get_images(device.handle, sc.handle, &count, nullptr) != VK_SUCCESS || count < 2) return false;
    sc.images.resize(count);
    if (device.get_images(device.handle, sc.handle, &count, sc.images.data()) != VK_SUCCESS) return false;
    sc.images.resize(count);
    sc.frames.resize(count);

    VkAttachmentDescription attachment{};
    attachment.format = sc.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    if (device.create_pass(device.handle, &pass_info, nullptr, &sc.pass) != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    if (device.create_pool(device.handle, &pool_info, nullptr, &sc.pool) != VK_SUCCESS) return false;
    std::vector<VkCommandBuffer> commands(count);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = sc.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = count;
    if (allocate_overlay_commands(device.handle, &alloc, commands.data()) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < count; ++i) {
        auto& frame = sc.frames[i];
        frame.command = commands[i];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = sc.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = sc.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (device.create_view(device.handle, &view, nullptr, &frame.view) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = sc.pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &frame.view;
        fb.width = sc.extent.width;
        fb.height = sc.extent.height;
        fb.layers = 1;
        if (device.create_framebuffer(device.handle, &fb, nullptr, &frame.framebuffer) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (device.create_semaphore(device.handle, &sem, nullptr, &frame.done) != VK_SUCCESS) return false;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (device.create_fence(device.handle, &fence, nullptr, &frame.fence) != VK_SUCCESS) return false;
    }
    sc.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(sc.context);
    imgui_ini_path = fo4hud_imgui_ini_path();
    std::error_code settings_directory_error;
    std::filesystem::create_directories(std::filesystem::path(imgui_ini_path).parent_path(),
                                        settings_directory_error);
    ImGui::GetIO().IniFilename = settings_directory_error ? nullptr : imgui_ini_path.c_str();
    if (settings_directory_error)
        settings_error = "Cannot create settings directory: " + settings_directory_error.message();
    // Keep the UI's bitmap font, and use a scalable font for ESP labels.
    ImGui::GetIO().Fonts->AddFontDefaultBitmap();
    sc.label_font = ImGui::GetIO().Fonts->AddFontDefaultVector();
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, [](const char* name, void*) {
        if (std::strcmp(name, "vkAllocateCommandBuffers") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(allocate_overlay_commands);
        auto function = device.next(device.handle, name);
        return function ? function : instance.next(instance.handle, name);
    })) return false;
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_0;
    init.Instance = instance.handle;
    init.PhysicalDevice = device.physical;
    init.Device = device.handle;
    init.QueueFamily = family;
    init.Queue = queue;
    init.DescriptorPoolSize = 32;
    init.MinImageCount = 2;
    init.ImageCount = count;
    init.PipelineInfoMain.RenderPass = sc.pass;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&init)) return false;
    sc.backend_initialized = true;
    active_swapchain = sc.handle;
    fo4hud_log("layer", "fo4hud: overlay initialized (%ux%u)", sc.extent.width, sc.extent.height);
    return true;
}

ImTextureData* radar_map_texture(Swapchain& sc, const std::string& path) {
    auto found = sc.map_textures.find(path);
    if (found != sc.map_textures.end()) return found->second.get();
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    std::string message;
    if (!fo4hud_load_map_pixels(path, width, height, pixels, message)) {
        fo4hud_log("radar", "map %s unavailable: %s", path.c_str(), message.c_str());
        sc.map_textures.emplace(path, nullptr);
        return nullptr;
    }
    auto texture = std::make_unique<ImTextureData>();
    texture->Create(ImTextureFormat_RGBA32, width, height);
    std::memcpy(texture->GetPixels(), pixels.data(), pixels.size());
    texture->UseColors = true;
    ImGui::RegisterUserTexture(texture.get());
    ImTextureData* result = texture.get();
    sc.map_textures.emplace(path, std::move(texture));
    fo4hud_log("radar", "map %s loaded (%dx%d)", path.c_str(), width, height);
    return result;
}

ImTextureData* radar_local_map_texture(Swapchain& sc, const Fo4LocalMapFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 4096 || frame.height > 4096 ||
        frame.pixels.size() != size_t(frame.width) * frame.height * 4) return nullptr;
    if (sc.local_map_texture &&
        (sc.local_map_texture->Width != frame.width || sc.local_map_texture->Height != frame.height)) {
        // Old swapchain frames may still reference the descriptor being retired.
        device.wait_idle(device.handle);
        auto* old = sc.local_map_texture.get();
        old->WantDestroyNextFrame = true;
        old->UnusedFrames = INT_MAX;
        old->SetStatus(ImTextureStatus_WantDestroy);
        ImGui_ImplVulkan_UpdateTexture(old);
        ImGui::UnregisterUserTexture(old);
        sc.local_map_texture.reset();
        sc.local_map_sequence = 0;
    }
    if (!sc.local_map_texture) {
        sc.local_map_texture = std::make_unique<ImTextureData>();
        sc.local_map_texture->Create(ImTextureFormat_RGBA32, frame.width, frame.height);
        sc.local_map_texture->UseColors = true;
        ImGui::RegisterUserTexture(sc.local_map_texture.get());
    }
    auto* texture = sc.local_map_texture.get();
    if (sc.local_map_sequence != frame.sequence) {
        std::memcpy(texture->GetPixels(), frame.pixels.data(), frame.pixels.size());
        if (texture->Status == ImTextureStatus_OK) {
            texture->UpdateRect = {0, 0, (unsigned short)frame.width, (unsigned short)frame.height};
            texture->Updates.resize(0);
            texture->Updates.push_back(texture->UpdateRect);
            texture->SetStatus(ImTextureStatus_WantUpdates);
        }
        sc.local_map_sequence = frame.sequence;
    }
    return texture;
}

void draw_radar_map(ImDrawList* draw, ImTextureData* texture, const Fo4MapInfo& map,
                    const Fo4RadarTransform& transform, ImVec2 center, float opacity) {
    const auto polygon = fo4hud_radar_map_polygon(map, transform);
    if (polygon.size() < 3) return;
    draw->PushTexture(texture->GetTexRef());
    draw->PrimReserve(int(polygon.size() - 2) * 3, int(polygon.size()));
    const auto base = draw->_VtxCurrentIdx;
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        draw->PrimWriteIdx(ImDrawIdx(base));
        draw->PrimWriteIdx(ImDrawIdx(base + i));
        draw->PrimWriteIdx(ImDrawIdx(base + i + 1));
    }
    const ImU32 color = IM_COL32(255, 255, 255, int(std::clamp(opacity, 0.0f, 1.0f) * 255));
    for (const auto& vertex : polygon)
        draw->PrimWriteVtx(ImVec2(center.x + vertex.x, center.y + vertex.y),
                           ImVec2(vertex.u, vertex.v), color);
    draw->PopTexture();
}

constexpr ImU32 radar_theft_color = IM_COL32(255, 95, 215, 255);
void draw_radar_compass(ImDrawList* draw, ImVec2 center, float radius, float heading) {
    const float sine = std::sin(heading), cosine = std::cos(heading);
    auto compass = [&](const char* label, float east, float north) {
        const float forward = east * sine + north * cosine;
        const float right = east * cosine - north * sine;
        const ImVec2 size = ImGui::CalcTextSize(label);
        const float inset = std::max(0.0f, radius - size.y * 0.5f - 5.0f);
        const ImVec2 p(center.x + right * inset - size.x * 0.5f,
                       center.y - forward * inset - size.y * 0.5f);
        draw->AddRectFilled({p.x - 2, p.y - 1}, {p.x + size.x + 2, p.y + size.y + 1},
                            IM_COL32(12, 18, 25, 220), 2.0f);
        draw->AddText(p, label[0] == 'N' ? IM_COL32(255, 225, 95, 255) : IM_COL32_WHITE, label);
    };
    compass("N", 0, 1); compass("S", 0, -1);
    compass("W", -1, 0); compass("E", 1, 0);
}

void draw_radar_theft_badge(ImDrawList* draw, ImVec2 point) {
    draw->AddCircle(point, 7.0f, IM_COL32(20, 15, 25, 255), 16, 3.5f);
    draw->AddCircle(point, 7.0f, radar_theft_color, 16, 1.5f);
    const ImVec2 a(point.x - 5, point.y + 5), b(point.x + 5, point.y - 5);
    draw->AddLine(a, b, IM_COL32(20, 15, 25, 255), 3.5f);
    draw->AddLine(a, b, radar_theft_color, 1.5f);
}

void draw_radar_tab(Swapchain& sc, const Fo4EntitySnapshot& snapshot, OverlaySettings& settings,
                    bool& settings_changed) {
    float player[3]{};
    float heading = 0.0f;
    if (!fo4hud_player_in_snapshot_cell(snapshot) ||
        !fo4hud_read_player_position(snapshot, player) ||
        !fo4hud_read_player_heading(snapshot, heading)) {
        ImGui::TextUnformatted("Waiting for the player and current cell...");
        return;
    }
    float& radar_range = settings.radar_range_for(snapshot.cell_is_interior);
    ImGui::Text("%s range: %.0f world units. Scroll over radar to zoom.",
                snapshot.cell_is_interior ? "Interior" : "Exterior", radar_range);
    Fo4MapInfo world_map;
    ImTextureData* map_texture = nullptr;
    const char* map_status = "";
    if (settings.radar_map_background) {
        if (snapshot.cell_is_interior) {
            const auto local = fo4hud_local_map_request(snapshot);
            if (local) {
                world_map = local->map;
                map_texture = radar_local_map_texture(sc, *local);
            } else map_status = "Waiting for local map...";
        } else if (fo4hud_read_world_map(snapshot, world_map)) {
            (void)fo4hud_local_map_request(snapshot); // Invalidate the previous interior.
            map_texture = radar_map_texture(sc, world_map.texture);
            if (!map_texture) map_status = "Map background unavailable: map asset missing.";
        } else {
            (void)fo4hud_local_map_request(snapshot);
            map_status = "No world map for this area.";
        }
    }
    // Reserve a stable status row so a background refresh cannot resize the canvas.
    ImGui::TextDisabled("%s", *map_status ? map_status : " ");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x < 120.0f || available.y < 120.0f) {
        ImGui::TextUnformatted("Enlarge the window to see the radar.");
        return;
    }
    const ImVec2 top_left = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("radar_canvas", available);
    const ImVec2 center(top_left.x + available.x * 0.5f, top_left.y + available.y * 0.5f);
    const float radius = std::max(1.0f, std::min(available.x, available.y) * 0.5f - 24.0f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mdx = mouse.x - center.x, mdy = mouse.y - center.y;
    if (ImGui::IsItemHovered() && mdx * mdx + mdy * mdy <= radius * radius) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            radar_range = std::clamp(radar_range * std::pow(0.8f, wheel),
                                              100.0f, 100000.0f);
            settings_changed = true;
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int circle_start = draw->VtxBuffer.Size;
    draw->AddCircleFilled(center, radius, IM_COL32(16, 24, 35, 235));
    const Fo4RadarTransform transform{player[0], player[1], heading, radar_range, radius};
    if (map_texture)
        draw_radar_map(draw, map_texture, world_map, transform, center, settings.radar_map_opacity);
    draw->AddCircle(center, radius, IM_COL32(150, 175, 205, 255), 0, 2.0f);
    draw->AddCircle(center, radius * 0.5f, IM_COL32(80, 105, 130, 190));
    draw->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y),
                  IM_COL32(80, 105, 130, 110));
    draw->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius),
                  IM_COL32(80, 105, 130, 110));
    draw_radar_compass(draw, center, radius, heading);
    sc.radar_fade.keep(draw, circle_start);
    const bool enabled[6] = {false, settings.radar_doors, settings.radar_containers,
                             settings.radar_items, settings.radar_actors,
                             settings.radar_crops};
    const ImU32 colors[6] = {0, IM_COL32(90, 205, 245, 255), IM_COL32(245, 180, 75, 255),
                             IM_COL32(125, 225, 145, 255), IM_COL32(240, 95, 110, 255),
                             IM_COL32(115, 245, 90, 255)};
    const char* categories[6] = {"", "Entrance", "Container", "Item", "Actor", "Crop"};
    const Fo4Entity* hovered = nullptr;
    uint8_t hovered_category = 0;
    bool hovered_harvested = false;
    Fo4LootAccess hovered_access = Fo4LootAccess::Unknown;
    bool hovered_loot = false;
    std::vector<std::pair<float, Fo4LootAccessCandidate>> loot_candidates;
    float hovered_distance = 0.0f;
    float closest_mouse = 64.0f;
    size_t shown = 0;
    auto marker = [&](const Fo4Entity& entity, const float position[3], uint8_t category) {
        if (category < 1 || category > 5 || !enabled[category] ||
            !std::isfinite(position[0]) || !std::isfinite(position[1])) return;
        const float east = position[0] - player[0];
        const float north = position[1] - player[1];
        const float distance_squared = east * east + north * north;
        if (!std::isfinite(distance_squared) ||
            distance_squared > radar_range * radar_range) return;
        const auto offset = transform.project(position[0], position[1]);
        const ImVec2 point(center.x + offset.x, center.y + offset.y);
        const bool dead_actor = category == 4 && [&] {
            uint8_t life_state = 0;
            return fo4hud_read_actor_life_state(entity, life_state) &&
                   (life_state == 1 || life_state == 2);
        }();
        // Read live readiness so harvesting and regrowth change the marker
        // immediately, without waiting for the next entity snapshot.
        const bool harvested = category == 5 && !fo4hud_flora_harvestable(entity.address);
        const bool empty = harvested || (entity.display_inventory_state() == Fo4InventoryState::Empty &&
                                        (category == 2 || dead_actor));
        const bool loot = !empty && (category == 2 || category == 3 || category == 5 || dead_actor);
        Fo4LootAccess access = Fo4LootAccess::Unknown;
        if (loot) {
            loot_candidates.push_back({distance_squared, {entity.address, entity.cell_address, entity.form_id}});
            access = fo4hud_combat_loot_access(entity, snapshot.player_address, snapshot.cell_address);
        }
        const ImU32 color = empty ? IM_COL32(100, 105, 112, 175) : colors[category];
        if (category == 1) {
            draw->AddRectFilled(ImVec2(point.x - 4, point.y - 4),
                                ImVec2(point.x + 4, point.y + 4), color);
        } else if (category == 2) {
            draw->AddQuadFilled(ImVec2(point.x, point.y - 5), ImVec2(point.x + 5, point.y),
                                ImVec2(point.x, point.y + 5), ImVec2(point.x - 5, point.y), color);
        } else if (category == 4) {
            draw->AddTriangleFilled(ImVec2(point.x, point.y - 5),
                                    ImVec2(point.x + 4, point.y + 4),
                                    ImVec2(point.x - 4, point.y + 4), color);
        } else if (category == 5) {
            draw->AddCircleFilled(point, 4.5f, color);
            draw->AddCircle(point, 4.5f, harvested ? IM_COL32(45, 48, 50, 190) :
                                                   IM_COL32(20, 65, 30, 255));
        } else {
            draw->AddCircleFilled(point, 3.5f, color);
        }
        if (access == Fo4LootAccess::Stealing) draw_radar_theft_badge(draw, point);
        ++shown;
        const float dx = mouse.x - point.x, dy = mouse.y - point.y;
        const float mouse_distance = dx * dx + dy * dy;
        if (ImGui::IsItemHovered() && mouse_distance <= closest_mouse) {
            closest_mouse = mouse_distance;
            hovered = &entity;
            hovered_category = category;
            hovered_harvested = harvested;
            hovered_access = access;
            hovered_loot = loot;
            hovered_distance = std::sqrt(distance_squared);
        }
    };
    const int markers_start = draw->VtxBuffer.Size;
    for (const Fo4Entity& entity : snapshot.entities)
        if (((entity.radar_category >= 1 && entity.radar_category <= 3) ||
             entity.radar_category == 5) &&
            fo4hud_world_reference_visible(entity))
            marker(entity, entity.position, entity.radar_category);
    for (const Fo4Entity& actor : snapshot.actors) {
        if (actor.address == snapshot.player_address ||
            !fo4hud_actor_in_player_space(snapshot, actor)) continue;
        float position[3]{};
        if (fo4hud_read_actor_position(actor, position)) marker(actor, position, 4);
    }
    std::stable_sort(loot_candidates.begin(), loot_candidates.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Fo4LootAccessCandidate> ownership_checks;
    for (size_t i = 0; i < std::min<size_t>(loot_candidates.size(), 512); ++i)
        ownership_checks.push_back(loot_candidates[i].second);
    fo4hud_combat_queue_loot_access(ownership_checks, snapshot.player_address, snapshot.cell_address);
    draw->AddTriangleFilled(ImVec2(center.x, center.y - 8),
                            ImVec2(center.x + 6, center.y + 6),
                            ImVec2(center.x - 6, center.y + 6), IM_COL32(255, 225, 95, 255));
    sc.radar_fade.keep(draw, markers_start);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::Text("%s: %s", categories[hovered_category],
                    hovered->name.empty() ? "Name unavailable" : hovered->name.c_str());
        ImGui::Text("Form %08X, %.0f units away", hovered->form_id, hovered_distance);
        if (hovered_category == 5)
            ImGui::TextUnformatted(hovered_harvested ? "Harvested" : "Ready to harvest");
        if ((hovered_category == 2 || hovered_category == 4) &&
            hovered->display_inventory_state() == Fo4InventoryState::Empty)
            ImGui::TextUnformatted("Inventory empty");
        if (hovered_loot) {
            if (hovered->vendor_chest && settings.loot_vendor_chests)
                ImGui::TextUnformatted("Vendor chest looting enabled (locks bypassed)");
            if (const char* reason = fo4hud_combat_manual_loot_reason(*hovered))
                ImGui::Text("Manual pickup: %s", reason);
            if (hovered_access == Fo4LootAccess::Stealing)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(radar_theft_color), "Taking this counts as stealing");
            else if (hovered_access == Fo4LootAccess::Allowed)
                ImGui::TextUnformatted("Taking this is permitted");
            else ImGui::TextDisabled("Ownership check pending");
        }
        ImGui::EndTooltip();
    }
    char count_text[64];
    std::snprintf(count_text, sizeof(count_text), "%zu shown", shown);
    draw->AddText(ImVec2(top_left.x + 8, top_left.y + 6), IM_COL32_WHITE, count_text);
}

// Capture only this game's swapchain, after its overlay submission finishes.
// Requested frames are copied back once; ordinary frames allocate nothing.
void capture_control_frame(Swapchain& sc, VkQueue queue, uint32_t image_index) {
    if (!fo4hud_control_begin_capture()) return;
    if (!sc.capturable || (sc.format != VK_FORMAT_B8G8R8A8_UNORM && sc.format != VK_FORMAT_B8G8R8A8_SRGB &&
                          sc.format != VK_FORMAT_R8G8B8A8_UNORM && sc.format != VK_FORMAT_R8G8B8A8_SRGB)) {
        fo4hud_control_finish_capture(0,0,false,nullptr,"swapchain does not support 8-bit screenshot readback");return;
    }
#define CAP_FN(name) auto next_##name = reinterpret_cast<PFN_##name>(device.next(device.handle,#name))
    CAP_FN(vkCreateBuffer);CAP_FN(vkDestroyBuffer);CAP_FN(vkGetBufferMemoryRequirements);
    CAP_FN(vkAllocateMemory);CAP_FN(vkFreeMemory);CAP_FN(vkBindBufferMemory);
    CAP_FN(vkMapMemory);CAP_FN(vkUnmapMemory);CAP_FN(vkCmdPipelineBarrier);
    CAP_FN(vkCmdCopyImageToBuffer);CAP_FN(vkFreeCommandBuffers);
#undef CAP_FN
    auto memory_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instance.next(instance.handle,"vkGetPhysicalDeviceMemoryProperties"));
    VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    bool submitted=false,finished=false;
    std::string error_message;
    try {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size=VkDeviceSize(sc.extent.width)*sc.extent.height*4;
        buffer_info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(next_vkCreateBuffer(device.handle,&buffer_info,nullptr,&buffer)!=VK_SUCCESS)throw std::runtime_error("capture buffer allocation failed");
        VkMemoryRequirements req{};next_vkGetBufferMemoryRequirements(device.handle,buffer,&req);
        VkPhysicalDeviceMemoryProperties props{};memory_properties(device.physical,&props);
        uint32_t type=0;while(type<props.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) ||
            (props.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))++type;
        if(type==props.memoryTypeCount)throw std::runtime_error("no coherent capture memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        if(next_vkAllocateMemory(device.handle,&allocation,nullptr,&memory)!=VK_SUCCESS || next_vkBindBufferMemory(device.handle,buffer,memory,0)!=VK_SUCCESS)throw std::runtime_error("capture memory allocation failed");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command_info.commandPool=sc.pool;command_info.commandBufferCount=1;command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        if(allocate_overlay_commands(device.handle,&command_info,&command)!=VK_SUCCESS)throw std::runtime_error("capture command allocation failed");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(device.begin_command(command,&begin)!=VK_SUCCESS)throw std::runtime_error("capture begin failed");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=sc.images[image_index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        next_vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={sc.extent.width,sc.extent.height,1};
        next_vkCmdCopyImageToBuffer(command,sc.images[image_index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=0;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        next_vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        if(device.end_command(command)!=VK_SUCCESS)throw std::runtime_error("capture command end failed");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if(device.create_fence(device.handle,&fence_info,nullptr,&fence)!=VK_SUCCESS)throw std::runtime_error("capture fence failed");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        if(device.submit(queue,1,&submit,fence)!=VK_SUCCESS)throw std::runtime_error("capture submit failed");submitted=true;
        if(device.wait_fences(device.handle,1,&fence,VK_TRUE,UINT64_MAX)!=VK_SUCCESS)throw std::runtime_error("capture GPU wait failed");finished=true;
        void* pixels=nullptr;if(next_vkMapMemory(device.handle,memory,0,buffer_info.size,0,&pixels)!=VK_SUCCESS)throw std::runtime_error("capture memory mapping failed");
        const bool bgra=sc.format==VK_FORMAT_B8G8R8A8_UNORM||sc.format==VK_FORMAT_B8G8R8A8_SRGB;
        fo4hud_control_finish_capture(sc.extent.width,sc.extent.height,bgra,static_cast<unsigned char*>(pixels));
        next_vkUnmapMemory(device.handle,memory);
    } catch(const std::exception& e) { error_message=e.what(); }
    if(submitted&&!finished)device.wait_idle(device.handle);
    if(fence)device.destroy_fence(device.handle,fence,nullptr);
    if(command)next_vkFreeCommandBuffers(device.handle,sc.pool,1,&command);
    if(buffer)next_vkDestroyBuffer(device.handle,buffer,nullptr);
    if(memory)next_vkFreeMemory(device.handle,memory,nullptr);
    if(!error_message.empty())fo4hud_control_finish_capture(0,0,false,nullptr,error_message);
}

bool draw_overlay(Swapchain& sc, VkQueue queue, uint32_t family,
                  const VkPresentInfoKHR* info, uint32_t image_index, VkSemaphore& done) {
    if (!sc.renderable || (sc.context && image_index >= sc.frames.size())) return false;
    if (!sc.context && !init_overlay(sc, queue, family)) {
        error("overlay initialization failed");
        destroy_overlay(sc);
        sc.renderable = false;
        return false;
    }
    if (image_index >= sc.frames.size()) return false;
    auto& frame = sc.frames[image_index];
    if (frame.submitted) {
        if (device.wait_fences(device.handle, 1, &frame.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    }
    ImGui::SetCurrentContext(sc.context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(sc.extent.width), float(sc.extent.height));
    io.DeltaTime = 1.0f / 60.0f;
    if (fo4hud_wayland_active()) fo4hud_wayland_new_frame(io, sc.extent.width, sc.extent.height);
    // Wine locks the Wayland pointer, so a hardware cursor cannot track the
    // relative motion that drives both Fallout's cursor and ImGui input.
    io.MouseDrawCursor = false;
    static auto previous_frame = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(0.001f, std::chrono::duration<float>(now - previous_frame).count());
    previous_frame = now;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (!settings_loaded) {
        std::string load_error;
        if (!fo4hud_load_settings(overlay_settings, load_error)) settings_error = load_error;
        fo4hud_overlay_set_visible(overlay_settings.overlay_visible);
        settings_loaded = true;
        fo4hud_combat_set_auto_loot_range(overlay_settings.auto_loot_range);
        fo4hud_combat_set_auto_loot_categories(fo4hud_loot_category_mask(overlay_settings));
        fo4hud_combat_set_auto_loot_inventories(overlay_settings.auto_loot_inventories);
        fo4hud_combat_set_auto_loot_steal(overlay_settings.auto_loot_steal);
        if (overlay_settings.loot_vendor_chests) fo4hud_combat_set_loot_vendor_chests(true);
        if (overlay_settings.auto_loot) fo4hud_combat_set_auto_loot(true);
        if (overlay_settings.auto_harvest) fo4hud_combat_set_auto_harvest(true);
    }
    const bool game_menu_cursor_active = fo4hud_game_menu_cursor_active();
    if (fo4hud_combat_active()) {
        if (fo4hud_combat_enabled() != overlay_settings.instant_kill)
            fo4hud_combat_set_enabled(overlay_settings.instant_kill);
        if (fo4hud_combat_invulnerable() != overlay_settings.invulnerable)
            fo4hud_combat_set_invulnerable(overlay_settings.invulnerable);
        if (fo4hud_combat_unlimited_action_points() != overlay_settings.unlimited_action_points)
            fo4hud_combat_set_unlimited_action_points(overlay_settings.unlimited_action_points);
        if (fo4hud_combat_no_recoil() != overlay_settings.no_recoil)
            fo4hud_combat_set_no_recoil(overlay_settings.no_recoil);
        if (fo4hud_combat_loot_vendor_chests() != overlay_settings.loot_vendor_chests)
            fo4hud_combat_set_loot_vendor_chests(overlay_settings.loot_vendor_chests);
        if (fo4hud_combat_auto_loot() != overlay_settings.auto_loot)
            fo4hud_combat_set_auto_loot(overlay_settings.auto_loot);
        if (fo4hud_combat_auto_harvest() != overlay_settings.auto_harvest)
            fo4hud_combat_set_auto_harvest(overlay_settings.auto_harvest);
    }
    static Fo4EntityRefresh entity_refresh;
    const auto& entities = entity_refresh.snapshot();
    auto& world_labels = overlay_settings.world_labels;
    auto& world_skeletons = overlay_settings.world_skeletons;
    auto& aim_assist = overlay_settings.aim_assist;
    auto& aim_radius = overlay_settings.aim_radius;
    auto& aim_distance = overlay_settings.aim_distance;
    auto& hide_dead_actors = overlay_settings.hide_dead_actors;
    auto& label_radius = overlay_settings.label_radius;
    auto& label_limit = overlay_settings.label_limit;
    bool settings_changed_this_frame = false;
    static ImGuiTextFilter name_filter;
    static bool filter_initialized = false;
    if (!filter_initialized) {
        std::snprintf(name_filter.InputBuf, sizeof(name_filter.InputBuf), "%s",
                      overlay_settings.name_filter.c_str());
        name_filter.Build();
        filter_initialized = true;
    }
    static auto last_snapshot = std::chrono::steady_clock::time_point{};
    static uintptr_t last_logged_cell = 0;
    static bool radar_open = false;
    static bool actor_table_open = false;
    const bool radar_active = radar_open && fo4hud_overlay_visible();
    Fo4EntityScanOptions scan_options;
    scan_options.actor_range = std::max({
        overlay_settings.dialogue_affinity ? 2500.0f : 0.0f,
        (world_labels || world_skeletons) ? label_radius : 0.0f,
        aim_assist ? aim_distance : 0.0f,
        (actor_table_open && fo4hud_overlay_visible()) ? 100000.0f : 0.0f,
        (overlay_settings.auto_loot && overlay_settings.auto_loot_inventories) ? overlay_settings.auto_loot_range : 0.0f});
    scan_options.radar_range = radar_active ? overlay_settings.radar_range_for(entities.cell_is_interior) : 0.0f;
    if (radar_active) {
        if (overlay_settings.radar_doors) scan_options.radar_categories |= 1 << 1;
        if (overlay_settings.radar_containers) scan_options.radar_categories |= 1 << 2;
        if (overlay_settings.radar_items) scan_options.radar_categories |= 1 << 3;
        if (overlay_settings.radar_actors) scan_options.radar_categories |= 1 << 4;
        if (overlay_settings.radar_crops) scan_options.radar_categories |= 1 << 5;
    }
    scan_options.actor_names = world_labels || (actor_table_open && fo4hud_overlay_visible()) ||
                               (scan_options.radar_categories & (1 << 4));
    scan_options.corpse_inventories = world_labels && !hide_dead_actors;
    scan_options.loot_range = overlay_settings.auto_loot_range;
    scan_options.loot_items = overlay_settings.auto_loot;
    scan_options.loot_inventories = overlay_settings.auto_loot && overlay_settings.auto_loot_inventories;
    scan_options.harvest = overlay_settings.auto_harvest;
    scan_options.loot_vendor_chests = overlay_settings.loot_vendor_chests;
    scan_options.trap_range = overlay_settings.trap_warnings ? overlay_settings.trap_warning_range : 0;
    if (entity_refresh.update(scan_options)) {
        last_snapshot = now;
        if (entities.status == "Ready" && entities.cell_address != last_logged_cell) {
            // A full formatted entity dump can itself stall presentation.
            // Log only the transition here; explicit MCP reads remain complete.
            fo4hud_log("entities", "incremental view cell=%08X address=%p interior=%d",
                       entities.cell_form_id, reinterpret_cast<void*>(entities.cell_address),
                       int(entities.cell_is_interior));
            last_logged_cell = entities.cell_address;
        }
    }
    static auto last_auto_loot_queue = std::chrono::steady_clock::time_point{};
    static size_t auto_loot_cursor = 0;
    if (((overlay_settings.auto_loot && fo4hud_combat_auto_loot()) ||
         (overlay_settings.auto_harvest && fo4hud_combat_auto_harvest()) ||
         (overlay_settings.loot_vendor_chests && fo4hud_combat_loot_vendor_chests())) &&
        !game_menu_cursor_active && entities.status == "Ready" &&
        fo4hud_player_in_snapshot_cell(entities) &&
        (overlay_settings.loot_vendor_chests || now - last_auto_loot_queue >= kFo4AutoLootInterval)) {
        last_auto_loot_queue = now;
        float player[3]{};
        if (fo4hud_read_player_position(entities, player)) {
            const float range_squared = overlay_settings.auto_loot_range *
                                        overlay_settings.auto_loot_range;
            // Rotate through candidates so an item that cannot be picked up
            // does not prevent other nearby items from being collected.
            const size_t world_count = entities.entities.size();
            const size_t total_count = world_count +
                (overlay_settings.auto_loot_inventories ? entities.actors.size() : 0);
            for (size_t offset = 0; offset < total_count; ++offset) {
                const size_t index = (auto_loot_cursor + offset) % total_count;
                const Fo4Entity& item = index < world_count ? entities.entities[index] :
                    entities.actors[index - world_count];
                if (!item.cell_address) continue;
                const bool loose = overlay_settings.auto_loot && item.radar_category == 3;
                const bool vendor = item.vendor_chest && overlay_settings.loot_vendor_chests;
                const bool container = item.radar_category == 2 &&
                    (vendor || (overlay_settings.auto_loot && overlay_settings.auto_loot_inventories)) &&
                    item.inventory_state != Fo4InventoryState::Empty;
                const bool corpse = item.radar_category == 4 &&
                    overlay_settings.auto_loot &&
                    overlay_settings.auto_loot_inventories &&
                    item.address != entities.player_address &&
                    item.inventory_state != Fo4InventoryState::Empty &&
                    fo4hud_actor_in_player_space(entities, item) && [&] {
                        uint8_t life_state = 0;
                        return fo4hud_read_actor_life_state(item, life_state) &&
                               (life_state == 1 || life_state == 2);
                    }();
                const bool crop = item.radar_category == 5 &&
                    overlay_settings.auto_harvest;
                if (!loose && !container && !corpse && !crop) continue;
                if (crop && !fo4hud_flora_harvestable(item.address)) continue;
                float position[3]{};
                if (corpse) {
                    if (!fo4hud_read_actor_position(item, position)) continue;
                } else {
                    std::copy(item.position, item.position + 3, position);
                }
                float squared = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float delta = position[axis] - player[axis];
                    squared += delta * delta;
                }
                if (!std::isfinite(squared) || squared > range_squared ||
                    (!corpse && !fo4hud_world_reference_visible(item))) continue;
                if (fo4hud_combat_queue_auto_loot(item))
                    auto_loot_cursor = index + 1;
                break;
            }
        }
    }
    if (fo4hud_overlay_visible()) {
        sc.radar_fade.opaque_ranges.clear();
        ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(750, 710), ImGuiCond_FirstUseEver);
        ImGui::Begin("Fallout 4 HUD", nullptr,
                     game_menu_cursor_active ? 0 : ImGuiWindowFlags_NoInputs);
        if (!settings_error.empty()) ImGui::Text("Settings error: %s", settings_error.c_str());
        radar_open = false;
        actor_table_open = false;
        if (ImGui::BeginTabBar("HUD tabs")) {
        if (ImGui::BeginTabItem("radar")) {
            radar_open = true;
            draw_radar_tab(sc, entities, overlay_settings, settings_changed_this_frame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("config")) {
            settings_changed_this_frame |= ImGui::Checkbox("Show correct lockpick angle", &overlay_settings.lockpick_guide);
            ImGui::TextWrapped("Shows the sweet-spot range and current pick angle while lockpicking.");
            settings_changed_this_frame |= ImGui::Checkbox("Actor information nearby / during dialogue", &overlay_settings.companion_information);
            settings_changed_this_frame |= ImGui::Checkbox("Dialogue companion affinity", &overlay_settings.dialogue_affinity);
            settings_changed_this_frame |= ImGui::Checkbox("Trap warnings", &overlay_settings.trap_warnings);
            settings_changed_this_frame |= ImGui::SliderFloat("Trap warning range", &overlay_settings.trap_warning_range, 100, 10000, "%.0f world units");
            settings_changed_this_frame |= ImGui::Checkbox("Highlight terminal password", &overlay_settings.hacking_guide);
            ImGui::TextWrapped("Shows a copy of the hacking board with the correct password highlighted.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("radar config")) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(radar_theft_color), "Ring + slash: taking this counts as stealing");
            settings_changed_this_frame |= ImGui::Checkbox("Map background (world / interior)", &overlay_settings.radar_map_background);
            if (overlay_settings.radar_map_background)
                settings_changed_this_frame |= ImGui::SliderFloat("Map opacity", &overlay_settings.radar_map_opacity, 0.15f, 1.0f, "%.2f");
            ImGui::TextUnformatted("Choose what appears on the radar.");
            const ImU32 colors[5] = {IM_COL32(90, 205, 245, 255),
                                     IM_COL32(245, 180, 75, 255),
                                     IM_COL32(125, 225, 145, 255),
                                     IM_COL32(240, 95, 110, 255),
                                     IM_COL32(115, 245, 90, 255)};
            bool* choices[5] = {&overlay_settings.radar_doors,
                                &overlay_settings.radar_containers,
                                &overlay_settings.radar_items,
                                &overlay_settings.radar_actors,
                                &overlay_settings.radar_crops};
            const char* labels[5] = {"Entrances", "Containers", "Items", "Actors", "Crops"};
            for (int i = 0; i < 5; ++i) {
                settings_changed_this_frame |= ImGui::Checkbox(labels[i], choices[i]);
                ImGui::SameLine();
                char color_id[32];
                std::snprintf(color_id, sizeof(color_id), "##radar_color_%d", i);
                ImGui::ColorButton(color_id, ImGui::ColorConvertU32ToFloat4(colors[i]),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2(12, 12));
            }
            ImGui::TextUnformatted("Radar range is saved when you zoom with the scroll wheel.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("NPCs and Enemies")) {
            actor_table_open = true;
            settings_changed_this_frame |= ImGui::Checkbox("World labels", &world_labels);
            ImGui::SameLine();
            settings_changed_this_frame |= ImGui::SliderFloat("Range", &label_radius, 500.0f, 100000.0f,
                               "%.0f units", ImGuiSliderFlags_Logarithmic);
            settings_changed_this_frame |= ImGui::Checkbox("Skeletons", &world_skeletons);
            settings_changed_this_frame |= ImGui::Checkbox("Hide dead actors", &hide_dead_actors);
            settings_changed_this_frame |= ImGui::SliderInt("Maximum ESP actors", &label_limit, 1, 30);
            ImGui::TextUnformatted("All active non-player actors, nearest first. Refresh to update positions.");
            if (name_filter.Draw("Filter names", 240)) {
                overlay_settings.name_filter = name_filter.InputBuf;
                settings_changed_this_frame = true;
            }
            std::vector<const Fo4Entity*> visible_actors;
            visible_actors.reserve(entities.actors.size());
            size_t total_actors = 0;
            for (const Fo4Entity& actor : entities.actors) {
                if (actor.address == entities.player_address) continue;
                ++total_actors;
                if (name_filter.PassFilter(actor.name.empty() ? "<unnamed>" : actor.name.c_str()))
                    visible_actors.push_back(&actor);
            }
            ImGui::Text("Showing %zu of %zu actors", visible_actors.size(), total_actors);
            if (ImGui::BeginTable("NPC actors", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                  ImVec2(0, 510))) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Position (X, Y, Z)", ImGuiTableColumnFlags_WidthFixed, 265);
                ImGui::TableSetupColumn("Form ID", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(visible_actors.size()));
                while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const Fo4Entity& actor = *visible_actors[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(actor.name.empty() ? "<unnamed>" : actor.name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%.1f, %.1f, %.1f",
                        actor.position[0], actor.position[1], actor.position[2]);
                    ImGui::TableNextColumn(); ImGui::Text("%08X", actor.form_id);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("cheat")) {
            if (ImGui::Checkbox("Loot vendor chests (including locked)", &overlay_settings.loot_vendor_chests)) {
                fo4hud_combat_set_loot_vendor_chests(overlay_settings.loot_vendor_chests);
                settings_changed_this_frame = true;
            }
            ImGui::TextWrapped("Transfers all playable vendor stock within the auto loot range, including owned stock. Independent of auto loot and its category filters.");
            ImGui::Separator();
            settings_changed_this_frame |= ImGui::Checkbox("Shot redirect (hold right mouse button)", &aim_assist);
            settings_changed_this_frame |= ImGui::SliderFloat("Aim radius", &aim_radius, 40.0f, 500.0f, "%.0f px");
            settings_changed_this_frame |= ImGui::SliderFloat("Aim distance", &aim_distance,
                                                               100.0f, 15000.0f, "%.0f world units");
            if (aim_assist && !fo4hud_combat_shot_redirect_active())
                ImGui::TextUnformatted("Shot redirect unavailable for this game build");
            ImGui::TextUnformatted("Redirects shots to the selected exposed body point.");
            ImGui::Separator();
            if (ImGui::Checkbox("instant kill", &overlay_settings.instant_kill)) {
                fo4hud_combat_set_enabled(overlay_settings.instant_kill);
                settings_changed_this_frame = true;
            }
            if (ImGui::Checkbox("invulnerable", &overlay_settings.invulnerable)) {
                fo4hud_combat_set_invulnerable(overlay_settings.invulnerable);
                settings_changed_this_frame = true;
            }
            if (ImGui::Checkbox("unlimited action points", &overlay_settings.unlimited_action_points)) {
                fo4hud_combat_set_unlimited_action_points(overlay_settings.unlimited_action_points);
                settings_changed_this_frame = true;
            }
            if (ImGui::Checkbox("disable weapon recoil", &overlay_settings.no_recoil)) {
                fo4hud_combat_set_no_recoil(overlay_settings.no_recoil);
                settings_changed_this_frame = true;
            }
            ImGui::TextUnformatted(fo4hud_combat_status());
            ImGui::TextUnformatted("Player hits only. Essential and invulnerable actors are excluded.");
            ImGui::Text("Instant hits: %llu; excluded hits: %llu",
                        (unsigned long long)fo4hud_combat_instant_hits(),
                        (unsigned long long)fo4hud_combat_protected_hits());
            ImGui::Text("Player health/limb damage blocked: %llu",
                        (unsigned long long)fo4hud_combat_blocked_damage());
            ImGui::Text("Player radiation gains blocked: %llu",
                        (unsigned long long)fo4hud_combat_blocked_rads());
            ImGui::Text("Player AP drains blocked: %llu",
                        (unsigned long long)fo4hud_combat_blocked_action_point_drains());
            ImGui::Separator();
            int perk_points = 0;
            bool perk_points_available = fo4hud_combat_read_perk_points(perk_points);
            if (perk_points_available)
                ImGui::Text("Available perk points: %d", perk_points);
            else
                ImGui::TextUnformatted("Available perk points: unavailable");
            static int perk_points_edit = 0;
            static std::string perk_message;
            ImGui::BeginDisabled(!perk_points_available);
            if (ImGui::Button("Change perk points...")) {
                perk_points_edit = perk_points;
                perk_message.clear();
                ImGui::OpenPopup("Change perk points");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Change perk points", nullptr,
                                      ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputInt("Available points", &perk_points_edit);
                ImGui::TextUnformatted("Choose a value from 0 to 127.");
                if (ImGui::Button("Apply")) {
                    std::string error;
                    if (fo4hud_combat_set_perk_points(perk_points_edit, error)) {
                        perk_message = "Perk points updated";
                        ImGui::CloseCurrentPopup();
                    } else {
                        perk_message = error;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    perk_message.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (!perk_message.empty()) ImGui::TextUnformatted(perk_message.c_str());
                ImGui::EndPopup();
            }
            if (!perk_message.empty()) ImGui::TextUnformatted(perk_message.c_str());
            ImGui::Separator();
            float carry_weight = 0.0f;
            const bool carry_weight_available = fo4hud_combat_read_carry_weight(carry_weight);
            if (carry_weight_available)
                ImGui::Text("Carry weight: %.0f", carry_weight);
            else
                ImGui::TextUnformatted("Carry weight: unavailable");
            static int carry_weight_edit = 0;
            static std::string carry_weight_message;
            ImGui::BeginDisabled(!carry_weight_available);
            if (ImGui::Button("Change carry weight...")) {
                carry_weight_edit = static_cast<int>(std::lround(carry_weight));
                carry_weight_message.clear();
                ImGui::OpenPopup("Change carry weight");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Change carry weight", nullptr,
                                      ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputInt("Carry weight", &carry_weight_edit);
                ImGui::TextUnformatted("Choose a value from 0 to 100000.");
                if (ImGui::Button("Apply##carry_weight")) {
                    std::string error;
                    if (fo4hud_combat_set_carry_weight(carry_weight_edit, error)) {
                        carry_weight_message.clear();
                        ImGui::CloseCurrentPopup();
                    } else {
                        carry_weight_message = error;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##carry_weight")) {
                    carry_weight_message.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (!carry_weight_message.empty())
                    ImGui::TextUnformatted(carry_weight_message.c_str());
                ImGui::EndPopup();
            }
            if (!carry_weight_message.empty())
                ImGui::TextUnformatted(carry_weight_message.c_str());
            if (*fo4hud_combat_carry_weight_status())
                ImGui::TextUnformatted(fo4hud_combat_carry_weight_status());
            ImGui::Separator();
            float speed_mult = 0.0f;
            const bool speed_mult_available = fo4hud_combat_read_speed_mult(speed_mult);
            if (speed_mult_available)
                ImGui::Text("SpeedMult: %.0f (normal: 100)", speed_mult);
            else
                ImGui::TextUnformatted("SpeedMult: unavailable (normal: 100)");
            static int speed_mult_edit = 100;
            static std::string speed_mult_message;
            ImGui::BeginDisabled(!speed_mult_available);
            if (ImGui::Button("Change SpeedMult...")) {
                speed_mult_edit = static_cast<int>(std::lround(speed_mult));
                speed_mult_message.clear();
                ImGui::OpenPopup("Change SpeedMult");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Change SpeedMult", nullptr,
                                      ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputInt("SpeedMult", &speed_mult_edit);
                ImGui::TextUnformatted("Normal value: 100. Choose a value from 1 to 1000.");
                if (ImGui::Button("Use normal value##speed_mult"))
                    speed_mult_edit = 100;
                if (ImGui::Button("Apply##speed_mult")) {
                    std::string error;
                    if (fo4hud_combat_set_speed_mult(speed_mult_edit, error)) {
                        speed_mult_message.clear();
                        ImGui::CloseCurrentPopup();
                    } else {
                        speed_mult_message = error;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##speed_mult")) {
                    speed_mult_message.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (!speed_mult_message.empty()) ImGui::TextUnformatted(speed_mult_message.c_str());
                ImGui::EndPopup();
            }
            if (!speed_mult_message.empty()) ImGui::TextUnformatted(speed_mult_message.c_str());
            if (*fo4hud_combat_speed_mult_status())
                ImGui::TextUnformatted(fo4hud_combat_speed_mult_status());
            ImGui::Separator();
            int caps = 0;
            const bool caps_available = fo4hud_combat_read_caps(caps);
            if (caps_available)
                ImGui::Text("Caps: %d", caps);
            else
                ImGui::TextUnformatted("Caps: unavailable");
            int game_caps = 0;
            if (fo4hud_combat_game_caps(game_caps))
                ImGui::Text("Game-reported caps: %d", game_caps);
            static int caps_edit = 0;
            static std::string caps_message;
            ImGui::BeginDisabled(!caps_available);
            if (ImGui::Button("Change caps...")) {
                caps_edit = caps;
                caps_message.clear();
                ImGui::OpenPopup("Change caps");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Change caps", nullptr,
                                      ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputInt("Caps", &caps_edit);
                ImGui::TextUnformatted("Choose a value from 0 to 1000000000.");
                if (ImGui::Button("Apply##caps")) {
                    std::string error;
                    if (fo4hud_combat_set_caps(caps_edit, error)) {
                        caps_message.clear();
                        ImGui::CloseCurrentPopup();
                    } else {
                        caps_message = error;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##caps")) {
                    caps_message.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (!caps_message.empty()) ImGui::TextUnformatted(caps_message.c_str());
                ImGui::EndPopup();
            }
            if (!caps_message.empty()) ImGui::TextUnformatted(caps_message.c_str());
            if (*fo4hud_combat_caps_status())
                ImGui::TextUnformatted(fo4hud_combat_caps_status());
            ImGui::Separator();
            float jump_height = 0.0f, safe_fall_height = 0.0f;
            const bool movement_available = fo4hud_combat_read_movement(jump_height, safe_fall_height);
            if (movement_available)
                ImGui::Text("Jump height: %.1f; safe fall height: %.1f", jump_height, safe_fall_height);
            else
                ImGui::TextUnformatted("Jump and fall settings: unavailable");
            static float jump_height_edit = 64.0f;
            static float safe_fall_height_edit = 600.0f;
            static std::string movement_message;
            ImGui::BeginDisabled(!movement_available);
            if (ImGui::Button("Change jump and fall...")) {
                jump_height_edit = jump_height;
                safe_fall_height_edit = safe_fall_height;
                movement_message.clear();
                ImGui::OpenPopup("Change jump and fall");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Change jump and fall", nullptr,
                                      ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputFloat("Jump height", &jump_height_edit, 1.0f, 10.0f, "%.1f");
                ImGui::InputFloat("Safe fall height", &safe_fall_height_edit, 10.0f, 100.0f, "%.1f");
                ImGui::TextUnformatted("Game defaults: jump 64; safe fall 600 (world units).");
                ImGui::TextUnformatted("Higher jumps may need a higher safe fall height.");
                if (ImGui::Button("Use defaults")) {
                    jump_height_edit = 64.0f;
                    safe_fall_height_edit = 600.0f;
                }
                if (ImGui::Button("Apply##movement")) {
                    std::string error;
                    if (fo4hud_combat_set_movement(jump_height_edit, safe_fall_height_edit, error)) {
                        movement_message.clear();
                        ImGui::CloseCurrentPopup();
                    } else {
                        movement_message = error;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##movement")) {
                    movement_message.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (!movement_message.empty()) ImGui::TextUnformatted(movement_message.c_str());
                ImGui::EndPopup();
            }
            if (!movement_message.empty()) ImGui::TextUnformatted(movement_message.c_str());
            if (*fo4hud_combat_movement_status())
                ImGui::TextUnformatted(fo4hud_combat_movement_status());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("auto loot")) {
            if (ImGui::Checkbox("Auto loot nearby items", &overlay_settings.auto_loot)) {
                fo4hud_combat_set_auto_loot(overlay_settings.auto_loot);
                settings_changed_this_frame = true;
            }
            if (ImGui::Checkbox("Auto loot corpses and containers",
                                &overlay_settings.auto_loot_inventories)) {
                fo4hud_combat_set_auto_loot_inventories(overlay_settings.auto_loot_inventories);
                settings_changed_this_frame = true;
            }
            if (ImGui::Checkbox("Auto harvest nearby crops", &overlay_settings.auto_harvest)) {
                fo4hud_combat_set_auto_harvest(overlay_settings.auto_harvest);
                settings_changed_this_frame = true;
            }
            if (ImGui::Checkbox("Steal items", &overlay_settings.auto_loot_steal)) {
                fo4hud_combat_set_auto_loot_steal(overlay_settings.auto_loot_steal);
                settings_changed_this_frame = true;
            }
            if (ImGui::SliderFloat("Auto loot/harvest range", &overlay_settings.auto_loot_range,
                                   50.0f, 1000.0f, "%.0f world units")) {
                fo4hud_combat_set_auto_loot_range(overlay_settings.auto_loot_range);
                settings_changed_this_frame = true;
            }
            ImGui::TextWrapped("Unapproved quest links, lock-bearing objects, blocked activations, and unreviewed or unfinished container scripts stay manual.");
            ImGui::TextWrapped("Books, keys, notes and holotapes default to manual pickup. Category opt-ins cannot override quest protections.");
            ImGui::TextUnformatted("Auto loot categories (loose items and inventories):");
            bool categories_changed = false;
            categories_changed |= ImGui::Checkbox("Junk", &overlay_settings.loot_junk);
            ImGui::SameLine();
            categories_changed |= ImGui::Checkbox("Weapons", &overlay_settings.loot_weapons);
            ImGui::SameLine();
            categories_changed |= ImGui::Checkbox("Apparel", &overlay_settings.loot_apparel);
            categories_changed |= ImGui::Checkbox("Aid", &overlay_settings.loot_aid);
            ImGui::SameLine();
            categories_changed |= ImGui::Checkbox("Ammo", &overlay_settings.loot_ammo);
            ImGui::SameLine();
            categories_changed |= ImGui::Checkbox("Books", &overlay_settings.loot_books);
            categories_changed |= ImGui::Checkbox("Ingredients", &overlay_settings.loot_ingredients);
            ImGui::SameLine();
            categories_changed |= ImGui::Checkbox("Keys", &overlay_settings.loot_keys);
            ImGui::SameLine();
            categories_changed |= ImGui::Checkbox("Notes / holotapes", &overlay_settings.loot_notes);
            categories_changed |= ImGui::Checkbox("Other misc", &overlay_settings.loot_other_misc);
            if (categories_changed) {
                fo4hud_combat_set_auto_loot_categories(fo4hud_loot_category_mask(overlay_settings));
                settings_changed_this_frame = true;
            }
            ImGui::TextUnformatted("Junk has scrap components; other misc includes caps and loose mods.");
            ImGui::TextUnformatted("Off: take only what the game permits. On: also take items marked as theft.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("settlement scrap")) {
            Fo4WorkshopScrapPreview scrap_preview = fo4hud_workshop_scrap_preview();
            static uint64_t selection_generation = 0;
            static std::vector<uint8_t> selected;
            if (selection_generation != scrap_preview.generation ||
                selected.size() != scrap_preview.references.size()) {
                selection_generation = scrap_preview.generation;
                selected.clear();
                selected.reserve(scrap_preview.references.size());
                for (const auto& ref : scrap_preview.references)
                    selected.push_back(fo4hud_workshop_default_selected(ref.name, ref.form_id) ? 1 : 0);
            }
            ImGui::BeginDisabled(scrap_preview.running || !fo4hud_combat_active());
            if (ImGui::Button("Preview settlement scrap"))
                fo4hud_workshop_request_preview();
            ImGui::EndDisabled();
            ImGui::TextUnformatted(scrap_preview.status.c_str());
            if (scrap_preview.workshop_form_id)
                ImGui::Text("Workshop: %08X", scrap_preview.workshop_form_id);
            if (scrap_preview.running || scrap_preview.processed || scrap_preview.skipped)
                ImGui::Text("Scrap calls: %zu; skipped: %zu of %zu",
                            scrap_preview.processed, scrap_preview.skipped,
                            scrap_preview.selected_count);
            if (scrap_preview.running && ImGui::Button("Stop scrap pass"))
                fo4hud_workshop_cancel_scrap();
            if (scrap_preview.ready) {
                ImGui::Text("Eligible loaded references: %zu", scrap_preview.references.size());
                ImGui::TextUnformatted("Common valuable, unnamed, and newly placed objects start unchecked.");
                ImGui::BeginDisabled(scrap_preview.running);
                if (ImGui::Button("Restore default selection")) {
                    for (size_t i = 0; i < selected.size(); ++i) {
                        const auto& ref = scrap_preview.references[i];
                        selected[i] = fo4hud_workshop_default_selected(ref.name, ref.form_id) ? 1 : 0;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Select all"))
                    std::fill(selected.begin(), selected.end(), 1);
                ImGui::SameLine();
                if (ImGui::Button("Select none"))
                    std::fill(selected.begin(), selected.end(), 0);
                if (ImGui::BeginTable("Settlement scrap preview", 5,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY, ImVec2(0, 230))) {
                    ImGui::TableSetupColumn("Scrap", ImGuiTableColumnFlags_WidthFixed, 55);
                    ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthFixed, 95);
                    ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 95);
                    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 150);
                    ImGui::TableHeadersRow();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(scrap_preview.references.size()));
                    while (clipper.Step())
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                            const auto& ref = scrap_preview.references[i];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::PushID(i);
                            bool checked = selected[i] != 0;
                            if (ImGui::Checkbox("##scrap_reference", &checked))
                                selected[i] = checked ? 1 : 0;
                            ImGui::PopID();
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(
                                ref.name.empty() ? "<unnamed>" : ref.name.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%08X", ref.form_id);
                            ImGui::TableNextColumn(); ImGui::Text("%08X", ref.base_form_id);
                            ImGui::TableNextColumn(); ImGui::Text("%.0f, %.0f, %.0f",
                                                                  ref.position[0], ref.position[1],
                                                                  ref.position[2]);
                        }
                    ImGui::EndTable();
                }
                ImGui::EndDisabled();
                const size_t selected_count = std::count_if(selected.begin(), selected.end(),
                    [](uint8_t value) { return value != 0; });
                ImGui::Text("Selected for scrap: %zu", selected_count);
                ImGui::BeginDisabled(selected_count == 0 || scrap_preview.running);
                static std::string scrap_confirm_error;
                if (ImGui::Button("Scrap selected references...")) {
                    scrap_confirm_error.clear();
                    ImGui::OpenPopup("Confirm settlement scrap");
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal("Confirm settlement scrap", nullptr,
                                          ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Scrap %zu selected references at workshop %08X?",
                                selected_count, scrap_preview.workshop_form_id);
                    ImGui::TextUnformatted("Save your game first. This cannot be undone by the overlay.");
                    if (ImGui::Button("Scrap settlement")) {
                        if (fo4hud_workshop_begin_scrap(scrap_preview.generation, selected))
                            ImGui::CloseCurrentPopup();
                        else
                            scrap_confirm_error = "Could not start scrap pass. Refresh the preview.";
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel##settlement_scrap"))
                        ImGui::CloseCurrentPopup();
                    if (!scrap_confirm_error.empty())
                        ImGui::TextUnformatted(scrap_confirm_error.c_str());
                    ImGui::EndPopup();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        ImGui::TextUnformatted("Shift+F8 toggles this window; Shift+F9 stops MCP actions");
        ImDrawList* window_draw = ImGui::GetWindowDrawList();
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        // Rectangle hover still works when the frame has become completely
        // transparent. A hidden game cursor is not an interactive hover.
        const bool hovered = game_menu_cursor_active && ImGui::IsMouseHoveringRect(
            window_pos, ImVec2(window_pos.x + window_size.x, window_pos.y + window_size.y), false);
        ImGui::End();
        sc.radar_fade.apply(window_draw, radar_open, hovered, io.DeltaTime);
    }
    fo4hud_submit_trap_candidates(entities, overlay_settings.trap_warnings);
    if (overlay_settings.trap_warnings)
        fo4hud_draw_trap_overlay(entities, fo4hud_read_trap_markers(), io.DisplaySize, overlay_settings.trap_warning_range);
    if ((world_labels || world_skeletons) && entities.actor_status == "Ready" &&
        fo4hud_player_in_snapshot_cell(entities)) {
        Fo4Projection projection;
        float player[3]{};
        if (fo4hud_read_projection(entities, projection) &&
            fo4hud_read_player_position(entities, player)) {
            struct Label {
                const Fo4Entity* actor;
                float position[3];
                float screen[2];
                float distance;
                bool dead;
                bool screen_valid;
            };
            std::vector<Label> labels;
            float radius_squared = label_radius * label_radius;
            int examined = 0, candidates = 0;
            for (const Fo4Entity& actor : entities.actors) {
                if (actor.address == entities.player_address ||
                    !fo4hud_actor_in_player_space(entities, actor)) continue;
                float stale_squared = 0;
                for (int axis = 0; axis < 3; ++axis) {
                    float delta = actor.position[axis] - player[axis];
                    stale_squared += delta * delta;
                }
                if (stale_squared > (label_radius + 500.0f) * (label_radius + 500.0f)) continue;
                if (++examined > 256) break;
                float position[3]{};
                if (!fo4hud_read_actor_position(actor, position)) continue;
                uint8_t life_state = 0;
                const bool life_state_known = fo4hud_read_actor_life_state(actor, life_state);
                const bool dead = life_state_known && (life_state == 1 || life_state == 2);
                if (hide_dead_actors && (!life_state_known || dead)) continue;
                if (++candidates > 128) break;
                float squared = 0;
                for (int axis = 0; axis < 3; ++axis) {
                    float delta = position[axis] - player[axis];
                    squared += delta * delta;
                }
                if (squared > radius_squared) continue;
                Label label{&actor, {position[0], position[1], position[2]}, {},
                            std::sqrt(squared), dead, false};
                if (world_labels && !actor.name.empty()) {
                    float anchor[3];
                    if (!fo4hud_read_actor_label_position(actor, position, anchor)) {
                        std::copy(position, position + 3, anchor);
                        anchor[2] += 80.0f;
                    }
                    label.screen_valid = fo4hud_project_world(projection, anchor,
                        io.DisplaySize.x, io.DisplaySize.y, label.screen);
                }
                if (world_skeletons || label.screen_valid) labels.push_back(label);
            }
            std::sort(labels.begin(), labels.end(), [](const Label& a, const Label& b) {
                return a.distance < b.distance;
            });
            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            std::vector<ImVec4> occupied;
            int processed = 0;
            for (const Label& label : labels) {
                if (processed++ >= label_limit) break;
                if (world_skeletons) {
                    std::vector<Fo4BoneSegment> segments;
                    if (fo4hud_read_actor_skeleton(*label.actor, label.position, segments)) {
                        const ImU32 color = label.dead ? IM_COL32(255, 80, 80, 200)
                            : IM_COL32(105, 220, 255, 210);
                        for (const Fo4BoneSegment& segment : segments) {
                            float from[2], to[2];
                            if (fo4hud_project_world(projection, segment.from, io.DisplaySize.x,
                                                     io.DisplaySize.y, from) &&
                                fo4hud_project_world(projection, segment.to, io.DisplaySize.x,
                                                     io.DisplaySize.y, to))
                                draw->AddLine(ImVec2(from[0], from[1]), ImVec2(to[0], to[1]), color, 1.6f);
                        }
                    }
                }
                if (!world_labels || !label.screen_valid) continue;
                char caption[320];
                std::snprintf(caption, sizeof(caption), "%s  %.0f", label.actor->name.c_str(), label.distance);
                // Quantize to integer pixels so the font atlas only bakes a
                // bounded set of sizes as the player moves.
                float font_size = std::round(std::clamp(
                    34.0f - 7.0f * std::log2(std::max(label.distance, 150.0f) / 150.0f),
                    8.0f, 34.0f));
                ImFont* font = sc.label_font ? sc.label_font : ImGui::GetFont();
                ImVec2 size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, caption);
                float x = label.screen[0] - size.x * 0.5f;
                float y = label.screen[1] - size.y - 11.0f;
                if (x < 4 || x + size.x + 8 > io.DisplaySize.x || y < 4 || y + size.y + 8 > io.DisplaySize.y) continue;
                ImVec4 box(x - 4, y - 3, x + size.x + 4, y + size.y + 3);
                bool overlaps = false;
                for (const ImVec4& other : occupied)
                    if (box.x < other.z + 4 && box.z + 4 > other.x &&
                        box.y < other.w + 4 && box.w + 4 > other.y) { overlaps = true; break; }
                if (overlaps) continue;
                occupied.push_back(box);
                draw->AddRectFilled(ImVec2(box.x, box.y), ImVec2(box.z, box.w), IM_COL32(0, 0, 0, 175), 3.0f);
                const bool empty_corpse = label.dead &&
                    label.actor->display_inventory_state() == Fo4InventoryState::Empty;
                draw->AddText(font, font_size, ImVec2(x, y),
                              empty_corpse ? IM_COL32(130, 92, 92, 205) :
                              label.dead ? IM_COL32(255, 80, 80, 255) :
                                           IM_COL32(255, 245, 210, 255), caption);
                draw->AddCircleFilled(ImVec2(label.screen[0], label.screen[1]), 2.5f,
                                      empty_corpse ? IM_COL32(130, 92, 92, 205) :
                                      label.dead ? IM_COL32(255, 80, 80, 255) :
                                                   IM_COL32(255, 225, 125, 255));
            }
        }
    }
    const bool aim_overlay_visible = fo4hud_overlay_visible();
    const bool aim_ui_interactive = aim_overlay_visible && game_menu_cursor_active;
    const bool aim_hook_active = fo4hud_wine_hooks_active();
    const bool aim_button_down = fo4hud_wine_hooks_aim_button_down();
    const bool aim_actors_ready = entities.actor_status == "Ready";
    const bool aim_player_in_cell = fo4hud_player_in_snapshot_cell(entities);
    fo4hud_wine_hooks_set_aim_assist(false, 0.0f);
    fo4hud_wine_hooks_set_aim_target(0.0f, 0.0f, false);
    const bool shot_redirect_active = fo4hud_combat_shot_redirect_active();
    static uintptr_t aim_locked_actor = 0;
    static uint8_t aim_locked_part = 255;
    static auto last_aim_trace = std::chrono::steady_clock::time_point{};
    static int last_aim_gates = -1;
    const int aim_gates = (int(aim_assist) << 0) | (int(aim_overlay_visible) << 1) |
        (int(aim_hook_active) << 2) | (int(aim_button_down) << 3) |
        (int(aim_actors_ready) << 4) | (int(aim_player_in_cell) << 5) |
        (int(game_menu_cursor_active) << 6);
    const bool aim_trace = fo4hud_aim_trace_enabled() &&
        (aim_gates != last_aim_gates ||
         now - last_aim_trace >= (aim_assist ? std::chrono::milliseconds(250) :
                                                std::chrono::milliseconds(2000)));
    if (aim_trace) {
        last_aim_trace = now;
        last_aim_gates = aim_gates;
    }
    bool aim_target_valid = false;
    float aim_error_x = 0.0f, aim_error_y = 0.0f;
    float aim_target_screen[2]{};
    float aim_target_world[3]{};
    uint32_t aim_target_form = 0, closest_form = 0;
    uintptr_t aim_target_cell = 0;
    float closest_error = -1.0f;
    int aim_scanned = 0, aim_space_skip = 0, aim_dead_skip = 0;
    int aim_distance_skip = 0, aim_head_fallback = 0, aim_fixed_fallback = 0;
    int aim_projection_skip = 0, aim_target_anchor = 0;
    int aim_radius_skip = 0, aim_hostility_skip = 0, aim_score_skip = 0;
    bool aim_projection_ready = false, aim_position_ready = false;
    if (aim_assist && shot_redirect_active && !aim_ui_interactive && aim_hook_active &&
        aim_button_down && aim_actors_ready && aim_player_in_cell) {
        Fo4Projection projection;
        float player[3]{};
        aim_projection_ready = fo4hud_read_projection(entities, projection);
        aim_position_ready = fo4hud_read_player_position(entities, player);
        if (aim_projection_ready && aim_position_ready) {
            float best_score = aim_radius * aim_radius;
            int examined = 0;
            const uintptr_t previous_target = aim_locked_actor;
            uintptr_t selected_target = 0;
            const Fo4Entity* selected_actor = nullptr;
            std::vector<std::pair<float, Fo4HostilityCandidate>> hostility_candidates;
            for (const Fo4Entity& actor : entities.actors) {
                if (!fo4hud_actor_in_player_space(entities, actor)) {
                    ++aim_space_skip;
                    continue;
                }
                if (actor.address == entities.player_address) continue;
                if (++examined > 256) break;
                ++aim_scanned;
                float origin[3]{}, head[3]{}, screen[2]{};
                uint8_t life_state = 0;
                if (!fo4hud_read_actor_position(actor, origin) ||
                    !fo4hud_read_actor_life_state(actor, life_state) || life_state != 0) {
                    ++aim_dead_skip;
                    continue;
                }
                float distance_squared = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float delta = origin[axis] - player[axis];
                    distance_squared += delta * delta;
                }
                if (distance_squared > aim_distance * aim_distance) {
                    ++aim_distance_skip;
                    continue;
                }
                int anchor_kind = 0; // 0 head, 1 label/torso, 2 reference height
                if (!fo4hud_read_actor_head_position(actor, origin, head)) {
                    ++aim_head_fallback;
                    anchor_kind = 1;
                    if (!fo4hud_read_actor_label_position(actor, origin, head)) {
                        std::copy(origin, origin + 3, head);
                        head[2] += 80.0f;
                        ++aim_fixed_fallback;
                        anchor_kind = 2;
                    }
                }
                if (!fo4hud_project_world(projection, head, io.DisplaySize.x,
                                          io.DisplaySize.y, screen)) {
                    ++aim_projection_skip;
                    continue;
                }
                const float error_x = screen[0] - io.DisplaySize.x * 0.5f;
                const float error_y = screen[1] - io.DisplaySize.y * 0.5f;
                const float error_squared = error_x * error_x + error_y * error_y;
                if (closest_error < 0.0f || error_squared < closest_error) {
                    closest_error = error_squared;
                    closest_form = actor.form_id;
                }
                if (error_squared > aim_radius * aim_radius) {
                    ++aim_radius_skip;
                    continue;
                }
                hostility_candidates.push_back({error_squared,
                    {actor.address, actor.cell_address, actor.form_id}});
                if (!fo4hud_combat_actor_hostile(actor.address, actor.form_id,
                                                  actor.cell_address)) {
                    ++aim_hostility_skip;
                    continue;
                }
                const float score = error_squared *
                    (actor.address == previous_target ? 0.65f : 1.0f);
                if (score >= best_score) {
                    ++aim_score_skip;
                    continue;
                }
                best_score = score;
                aim_error_x = error_x;
                aim_error_y = error_y;
                aim_target_screen[0] = screen[0];
                aim_target_screen[1] = screen[1];
                selected_target = actor.address;
                selected_actor = &actor;
                aim_target_form = actor.form_id;
                aim_target_cell = actor.cell_address;
                aim_target_anchor = anchor_kind;
                aim_target_valid = true;
            }
            std::sort(hostility_candidates.begin(), hostility_candidates.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<Fo4HostilityCandidate> candidates;
            candidates.reserve(std::min<size_t>(hostility_candidates.size(), 64));
            for (size_t i = 0; i < hostility_candidates.size() && i < 64; ++i)
                candidates.push_back(hostility_candidates[i].second);
            fo4hud_combat_queue_hostility(candidates);
            if (selected_actor) {
                std::vector<Fo4BoneSegment> ignored_segments;
                std::vector<Fo4AimPoint> points;
                (void)fo4hud_read_actor_skeleton(*selected_actor,
                                                  selected_actor->position,
                                                  ignored_segments, &points);
                // The high-process head anchor is more accurate than the
                // skeleton node for first-person and non-human actors.
                float live_origin[3]{}, live_head[3]{};
                if (fo4hud_read_actor_position(*selected_actor, live_origin) &&
                    fo4hud_read_actor_head_position(*selected_actor, live_origin, live_head)) {
                    auto head_point = std::find_if(points.begin(), points.end(),
                        [](const Fo4AimPoint& p) { return p.part == Fo4BodyPart::Head; });
                    if (head_point == points.end()) {
                        Fo4AimPoint point;
                        point.part = Fo4BodyPart::Head;
                        std::copy(live_head, live_head + 3, point.position);
                        points.push_back(point);
                    } else std::copy(live_head, live_head + 3, head_point->position);
                }
                float eye[3]{player[0], player[1], player[2] + 68.0f};
                float camera[3]{};
                if (fo4hud_projection_camera_position(projection, camera)) {
                    float camera_distance_squared = 0.0f;
                    for (int axis = 0; axis < 3; ++axis) {
                        const float delta = camera[axis] - player[axis];
                        camera_distance_squared += delta * delta;
                    }
                    if (camera_distance_squared < 300.0f * 300.0f)
                        std::copy(camera, camera + 3, eye);
                }
                fo4hud_combat_queue_visibility(selected_actor->address,
                    selected_actor->form_id, selected_actor->cell_address, eye, points);
                const Fo4VisibilityResult visible = fo4hud_combat_visibility_result();
                aim_target_valid = false;
                if (visible.actor == selected_actor->address &&
                    visible.cell == selected_actor->cell_address &&
                    now - visible.checked < std::chrono::milliseconds(200)) {
                    float best_point_score = aim_radius * aim_radius;
                    bool head_selected = false;
                    for (const Fo4VisibilityPoint& point : visible.points) {
                        if (!point.exposed) continue;
                        float body_screen[2]{};
                        if (!fo4hud_project_world(projection, point.position,
                                io.DisplaySize.x, io.DisplaySize.y, body_screen)) continue;
                        const float dx = body_screen[0] - io.DisplaySize.x * 0.5f;
                        const float dy = body_screen[1] - io.DisplaySize.y * 0.5f;
                        const float score = dx * dx + dy * dy;
                        if (score > aim_radius * aim_radius) continue;
                        const bool is_head = point.part == static_cast<uint8_t>(Fo4BodyPart::Head);
                        const float ranked_score = score *
                            (selected_target == previous_target &&
                             point.part == aim_locked_part ? 0.55f : 1.0f);
                        if (head_selected || (!is_head && ranked_score >= best_point_score))
                            continue;
                        best_point_score = ranked_score;
                        head_selected = is_head;
                        aim_error_x = dx;
                        aim_error_y = dy;
                        aim_target_screen[0] = body_screen[0];
                        aim_target_screen[1] = body_screen[1];
                        std::copy(point.position, point.position + 3, aim_target_world);
                        aim_target_anchor = int(point.part);
                        aim_target_valid = true;
                    }
                }
                if (aim_target_valid) {
                    aim_locked_actor = selected_target;
                    aim_locked_part = static_cast<uint8_t>(aim_target_anchor);
                    ImGui::GetBackgroundDrawList()->AddCircle(
                        ImVec2(aim_target_screen[0], aim_target_screen[1]), 9.0f,
                        IM_COL32(255, 230, 40, 255), 20, 2.5f);
                }
            }
        }
    }
    if (!aim_target_valid) {
        aim_locked_actor = 0;
        aim_locked_part = 255;
        if (!aim_button_down || !aim_assist) fo4hud_combat_clear_visibility();
    }
    Fo4ShotTarget shot_target;
    if (aim_target_valid && aim_locked_actor) {
        shot_target.actor = aim_locked_actor;
        shot_target.cell = aim_target_cell;
        shot_target.form_id = aim_target_form;
        shot_target.part = static_cast<uint8_t>(aim_target_anchor);
        std::copy(aim_target_world, aim_target_world + 3, shot_target.position);
    }
    fo4hud_combat_set_shot_target(shot_target);
    if (aim_trace) {
        const long snapshot_age_ms = last_snapshot == std::chrono::steady_clock::time_point{} ?
            -1 : std::chrono::duration_cast<std::chrono::milliseconds>(now - last_snapshot).count();
        fo4hud_log("aim", "frame enabled=%d overlay=%d menu_cursor=%d ui_blocks_aim=%d "
                   "hook=%d rmb=%d actors=%s cell=%d "
                   "snapshot_age_ms=%ld projection=%d player_pos=%d display=%.0fx%.0f "
                   "radius=%.1f max_distance=%.0f scanned=%d "
                   "skipped_space=%d skipped_dead_or_unreadable=%d "
                   "skipped_distance=%d head_fallback=%d fixed_fallback=%d "
                   "skipped_projection=%d skipped_radius=%d skipped_nonhostile_or_unknown=%d "
                   "skipped_score=%d closest=%08X closest_error_px=%.1f target=%08X "
                   "target_anchor=%d target_screen=(%.1f,%.1f) error_px=(%.1f,%.1f)",
                   int(aim_assist), int(aim_overlay_visible), int(game_menu_cursor_active),
                   int(aim_ui_interactive), int(aim_hook_active),
                   int(aim_button_down), entities.actor_status.c_str(), int(aim_player_in_cell),
                   snapshot_age_ms, int(aim_projection_ready), int(aim_position_ready),
                   io.DisplaySize.x, io.DisplaySize.y, aim_radius, aim_distance,
                   aim_scanned, aim_space_skip, aim_dead_skip, aim_distance_skip,
                   aim_head_fallback, aim_fixed_fallback, aim_projection_skip,
                   aim_radius_skip, aim_hostility_skip, aim_score_skip,
                   closest_form, closest_error < 0.0f ? -1.0f : std::sqrt(closest_error),
                   aim_target_form, aim_target_anchor,
                   aim_target_screen[0], aim_target_screen[1],
                   aim_error_x, aim_error_y);
    }
    if (settings_changed_this_frame) {
        settings_dirty = true;
        settings_changed_at = now;
    }
    if (overlay_settings.overlay_visible != fo4hud_overlay_visible() ||
        (settings_dirty && now - settings_changed_at >= std::chrono::milliseconds(300)))
        flush_settings();
    io.MouseDrawCursor = fo4hud_overlay_visible() && game_menu_cursor_active &&
                         io.WantCaptureMouse;
    Fo4LockpickState lockpick;
    if (overlay_settings.lockpick_guide && fo4hud_read_lockpick_state(lockpick))
        fo4hud_draw_lockpick_overlay(lockpick, io.DisplaySize);
    Fo4CompanionState companion;
    if (overlay_settings.companion_information && fo4hud_read_companion_state(companion))
        fo4hud_draw_companion_overlay(companion, io.DisplaySize);
    if (overlay_settings.dialogue_affinity)
        fo4hud_draw_dialogue_affinity(fo4hud_read_dialogue_affinity(entities), io.DisplaySize, sc.label_font);
    Fo4HackingState hacking;
    if (overlay_settings.hacking_guide && fo4hud_read_hacking_state(hacking))
        fo4hud_draw_hacking_overlay(hacking, io.DisplaySize);
    ImGui::Render();
    fo4hud_wine_hooks_set_capture(
        fo4hud_overlay_visible() && game_menu_cursor_active && io.WantCaptureMouse,
        fo4hud_overlay_visible() && game_menu_cursor_active && io.WantCaptureKeyboard);
    if (device.reset_command(frame.command, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (device.begin_command(frame.command, &begin) != VK_SUCCESS) return false;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = sc.pass;
    pass.framebuffer = frame.framebuffer;
    pass.renderArea.extent = sc.extent;
    device.cmd_begin_pass(frame.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command);
    device.cmd_end_pass(frame.command);
    if (device.end_command(frame.command) != VK_SUCCESS) return false;
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info->waitSemaphoreCount;
    submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.done;
    if (frame.submitted && device.reset_fences(device.handle, 1, &frame.fence) != VK_SUCCESS) return false;
    VkResult result = device.submit(queue, 1, &submit, frame.fence);
    if (result != VK_SUCCESS) { error("overlay submit failed", result); return false; }
    frame.submitted = true;
    capture_control_frame(sc, queue, image_index);
    done = frame.done;
    return true;
}
} // namespace

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fo4hud_vkGetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fo4hud_vkGetDeviceProcAddr(VkDevice, const char*);

VKAPI_ATTR VkResult VKAPI_CALL fo4hud_vkCreateInstance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkInstance* out) {
    trace("CreateInstance enter");
    auto* link = instance_link(info);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(info, allocator, out);
    trace("CreateInstance return", result);
    if (std::getenv("FO4HUD_TRACE") && result == VK_SUCCESS)
        fo4hud_log("layer", "fo4hud trace [%ld]: instance=%p next=%p", long(getpid()), (void*)*out, (void*)next);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(mutex);
        instance = {*out, next};
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL fo4hud_vkDestroyInstance(VkInstance handle, const VkAllocationCallbacks* allocator) {
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("layer", "fo4hud trace [%ld]: DestroyInstance=%p", long(getpid()), (void*)handle);
    auto destroy = load_instance<PFN_vkDestroyInstance>("vkDestroyInstance");
    fo4hud_dialogue_affinity_uninstall();
    fo4hud_control_stop();
    fo4hud_local_map_uninstall();
    fo4hud_wine_hooks_uninstall();
    fo4hud_combat_uninstall();
    fo4hud_wayland_stop();
    destroy(handle, allocator);
    std::lock_guard<std::mutex> lock(mutex);
    if (instance.handle == handle) instance = {};
}
VKAPI_ATTR VkResult VKAPI_CALL fo4hud_vkCreateWaylandSurfaceKHR(VkInstance handle,
    const VkWaylandSurfaceCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* out) {
    auto create = load_instance<PFN_vkCreateWaylandSurfaceKHR>("vkCreateWaylandSurfaceKHR");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = create(handle, info, allocator, out);
    if (result == VK_SUCCESS) {
        wayland_surface_handle = *out;
        fo4hud_wayland_start(info->display, info->surface);
        fo4hud_wine_hooks_install();
        if (!fo4hud_local_map_install()) fo4hud_log("radar", "interior map renderer unavailable");
        if (!fo4hud_dialogue_affinity_install(fo4hud_supported_game_image()))
            fo4hud_log("dialogue", "dialogue affinity capture unavailable");
        if (fo4hud_combat_install()) fo4hud_control_start(fo4hud_supported_game_image());
    }
    trace("CreateWaylandSurfaceKHR", result);
    return result;
}
VKAPI_ATTR void VKAPI_CALL fo4hud_vkDestroySurfaceKHR(VkInstance handle, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) {
    if (surface == wayland_surface_handle) {
        fo4hud_dialogue_affinity_uninstall();
        fo4hud_local_map_uninstall();
        fo4hud_control_stop();
        fo4hud_wine_hooks_uninstall();
        fo4hud_combat_uninstall();
        fo4hud_wayland_stop();
        wayland_surface_handle = VK_NULL_HANDLE;
    }
    auto destroy = load_instance<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR");
    if (destroy) destroy(handle, surface, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL fo4hud_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* out) {
    trace("CreateDevice enter");
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    for (auto* p = reinterpret_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            const auto* loader = reinterpret_cast<const VkLayerDeviceCreateInfo*>(p);
            if (loader->function == VK_LOADER_DATA_CALLBACK)
                set_loader_data = loader->u.pfnSetDeviceLoaderData;
        }
    auto* link = device_link(info);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create = load_instance<PFN_vkCreateDevice>("vkCreateDevice");
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("layer", "fo4hud trace [%ld]: physical=%p instance=%p create=%p", long(getpid()), (void*)physical, (void*)instance.handle, (void*)create);
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(physical, info, allocator, out);
    trace("CreateDevice return", result);
    if (std::getenv("FO4HUD_TRACE") && result == VK_SUCCESS)
        fo4hud_log("layer", "fo4hud trace [%ld]: device=%p next=%p", long(getpid()), (void*)*out, (void*)next);
    if (result != VK_SUCCESS) return result;
    std::lock_guard<std::mutex> lock(mutex);
    device = {};
    device.handle = *out;
    device.physical = physical;
    device.next = next;
    device.set_loader_data = set_loader_data;
#define LOAD(field, type, name) device.field = load_device<type>(name)
    LOAD(destroy, PFN_vkDestroyDevice, "vkDestroyDevice");
    LOAD(create_swapchain, PFN_vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    LOAD(destroy_swapchain, PFN_vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    LOAD(get_images, PFN_vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    LOAD(present, PFN_vkQueuePresentKHR, "vkQueuePresentKHR");
    LOAD(submit, PFN_vkQueueSubmit, "vkQueueSubmit");
    LOAD(wait_idle, PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle");
    LOAD(get_queue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
    LOAD(create_pool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    LOAD(destroy_pool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    LOAD(allocate_commands, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    LOAD(reset_command, PFN_vkResetCommandBuffer, "vkResetCommandBuffer");
    LOAD(begin_command, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer");
    LOAD(end_command, PFN_vkEndCommandBuffer, "vkEndCommandBuffer");
    LOAD(cmd_begin_pass, PFN_vkCmdBeginRenderPass, "vkCmdBeginRenderPass");
    LOAD(cmd_end_pass, PFN_vkCmdEndRenderPass, "vkCmdEndRenderPass");
    LOAD(create_semaphore, PFN_vkCreateSemaphore, "vkCreateSemaphore");
    LOAD(destroy_semaphore, PFN_vkDestroySemaphore, "vkDestroySemaphore");
    LOAD(create_fence, PFN_vkCreateFence, "vkCreateFence");
    LOAD(destroy_fence, PFN_vkDestroyFence, "vkDestroyFence");
    LOAD(wait_fences, PFN_vkWaitForFences, "vkWaitForFences");
    LOAD(reset_fences, PFN_vkResetFences, "vkResetFences");
    LOAD(create_view, PFN_vkCreateImageView, "vkCreateImageView");
    LOAD(destroy_view, PFN_vkDestroyImageView, "vkDestroyImageView");
    LOAD(create_pass, PFN_vkCreateRenderPass, "vkCreateRenderPass");
    LOAD(destroy_pass, PFN_vkDestroyRenderPass, "vkDestroyRenderPass");
    LOAD(create_framebuffer, PFN_vkCreateFramebuffer, "vkCreateFramebuffer");
    LOAD(destroy_framebuffer, PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer");
#undef LOAD
    device.queue_properties = load_instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    device.surface_capabilities = load_instance<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    for (uint32_t family = 0; family < info->queueCreateInfoCount; ++family) {
        const auto& q = info->pQueueCreateInfos[family];
        for (uint32_t index = 0; index < q.queueCount; ++index) {
            VkQueue queue = VK_NULL_HANDLE;
            device.get_queue(*out, q.queueFamilyIndex, index, &queue);
            if (queue) device.queues[queue] = q.queueFamilyIndex;
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL fo4hud_vkDestroyDevice(VkDevice handle, const VkAllocationCallbacks* allocator) {
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("layer", "fo4hud trace [%ld]: DestroyDevice=%p", long(getpid()), (void*)handle);
    std::lock_guard<std::mutex> lock(mutex);
    auto destroy = device.next
        ? reinterpret_cast<PFN_vkDestroyDevice>(device.next(handle, "vkDestroyDevice"))
        : nullptr;
    if (!destroy) return;
    if (device.handle == handle) {
        for (auto& pair : swapchains) destroy_overlay(pair.second);
        swapchains.clear();
        device = {};
    }
    destroy(handle, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL fo4hud_vkCreateSwapchainKHR(VkDevice handle, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
    trace("CreateSwapchainKHR enter");
    std::lock_guard<std::mutex> lock(mutex);
    VkSwapchainCreateInfoKHR patched = *info;
    bool renderable = (patched.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    if (!renderable && device.surface_capabilities) {
        VkSurfaceCapabilitiesKHR caps{};
        if (device.surface_capabilities(device.physical, info->surface, &caps) == VK_SUCCESS &&
            (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            patched.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            renderable = true;
        }
    }
    if (device.surface_capabilities) {
        VkSurfaceCapabilitiesKHR caps{};
        if (device.surface_capabilities(device.physical, info->surface, &caps) == VK_SUCCESS &&
            (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    VkResult result = device.create_swapchain(handle, &patched, allocator, out);
    trace("CreateSwapchainKHR return", result);
    if (result == VK_SUCCESS) {
        Swapchain& sc = swapchains[*out];
        sc.handle = *out;
        sc.device = handle;
        sc.format = info->imageFormat;
        sc.extent = info->imageExtent;
        sc.renderable = renderable;
        sc.capturable = (patched.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL fo4hud_vkDestroySwapchainKHR(VkDevice handle, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = swapchains.find(swapchain);
    if (it != swapchains.end()) { destroy_overlay(it->second); swapchains.erase(it); }
    device.destroy_swapchain(handle, swapchain, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL fo4hud_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    std::lock_guard<std::mutex> lock(mutex);
    auto q = device.queues.find(queue);
    static bool present_logged = false;
    if (!present_logged && std::getenv("FO4HUD_TRACE")) {
        fo4hud_log("layer", "fo4hud trace [%ld]: present queue=%p known=%d count=%u pNext=%p swapchainKnown=%d active=%llu",
            long(getpid()), (void*)queue, int(q != device.queues.end()), info->swapchainCount,
            info->pNext, int(info->swapchainCount && swapchains.count(info->pSwapchains[0])),
            (unsigned long long)active_swapchain);
        present_logged = true;
    }
    if (q != device.queues.end() && info->swapchainCount == 1) {
        uint32_t family_count = 0;
        device.queue_properties(device.physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        device.queue_properties(device.physical, &family_count, families.data());
        if (q->second < family_count && (families[q->second].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            auto sc = swapchains.find(info->pSwapchains[0]);
            if (sc != swapchains.end()) {
                VkSemaphore done = VK_NULL_HANDLE;
                if (draw_overlay(sc->second, queue, q->second, info, info->pImageIndices[0], done)) {
                    VkPresentInfoKHR patched = *info;
                    patched.waitSemaphoreCount = 1;
                    patched.pWaitSemaphores = &done;
                    return device.present(queue, &patched);
                }
            }
        }
    }
    return device.present(queue, info);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fo4hud_vkGetInstanceProcAddr(VkInstance handle, const char* name) {
    if (!name) return nullptr;
#define HOOK(n) if (std::strcmp(name, "vk" #n) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fo4hud_vk##n)
    HOOK(GetInstanceProcAddr); HOOK(GetDeviceProcAddr); HOOK(CreateInstance); HOOK(DestroyInstance);
    HOOK(CreateWaylandSurfaceKHR); HOOK(DestroySurfaceKHR); HOOK(CreateDevice); HOOK(DestroyDevice); HOOK(CreateSwapchainKHR); HOOK(DestroySwapchainKHR); HOOK(QueuePresentKHR);
#undef HOOK
    return handle && instance.next ? instance.next(handle, name) : nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fo4hud_vkGetDeviceProcAddr(VkDevice handle, const char* name) {
    if (!name) return nullptr;
#define HOOK(n) if (std::strcmp(name, "vk" #n) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fo4hud_vk##n)
    HOOK(GetDeviceProcAddr); HOOK(DestroyDevice); HOOK(CreateSwapchainKHR); HOOK(DestroySwapchainKHR); HOOK(QueuePresentKHR);
#undef HOOK
    return handle && device.next ? device.next(handle, name) : nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* negotiation) {
    if (!negotiation || negotiation->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    negotiation->loaderLayerInterfaceVersion = 2;
    negotiation->pfnGetInstanceProcAddr = fo4hud_vkGetInstanceProcAddr;
    negotiation->pfnGetDeviceProcAddr = fo4hud_vkGetDeviceProcAddr;
    negotiation->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
} // extern C
