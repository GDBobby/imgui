// dear imgui: Renderer Backend for Vulkan
// This needs to be used along with a Platform Backend (e.g. GLFW, SDL, Win32, custom..)

// Implemented features:
//  [!] Renderer: User texture binding. Use 'VkDescriptorSet' as texture identifier. Call ImGui_ImplVulkan_AddTexture() to register one. Read the FAQ about ImTextureID/ImTextureRef + https://github.com/ocornut/imgui/pull/914 for discussions.
//  [X] Renderer: Large meshes support (64k+ vertices) even with 16-bit indices (ImGuiBackendFlags_RendererHasVtxOffset).
//  [X] Renderer: Texture updates support for dynamic font atlas (ImGuiBackendFlags_RendererHasTextures).
//  [X] Renderer: Expose selected render state for draw callbacks to use. Access in '(ImGui_ImplXXXX_RenderState*)GetPlatformIO().Renderer_RenderState'.

// The aim of imgui_impl_vulkan.h/.cpp is to be usable in your engine without any modification.
// IF YOU FEEL YOU NEED TO MAKE ANY CHANGE TO THIS CODE, please share them and your feedback at https://github.com/ocornut/imgui/

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// Important note to the reader who wish to integrate imgui_impl_vulkan.cpp/.h in their own engine/app.
// - Common ImGui_ImplVulkan_XXX functions and structures are used to interface with imgui_impl_vulkan.cpp/.h.
//   You will use those if you want to use this rendering backend in your engine/app.
// - Helper ImGui_ImplVulkanH_XXX functions and structures are only used by this example (main.cpp) and by
//   the backend itself (imgui_impl_vulkan.cpp), but should PROBABLY NOT be used by your own engine/app code.
// Read comments in imgui_impl_vulkan.h.

#pragma once
#include <vulkan/vulkan_core.h>
#ifndef IMGUI_DISABLE
#include "imgui.h"      // IMGUI_IMPL_API

#include "EightWinds/VulkanHeader.h"


// [Configuration] in order to use a custom Vulkan function loader:
// (1) You'll need to disable default Vulkan function prototypes.
//     We provide a '#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES' convenience configuration flag.
//     In order to make sure this is visible from the imgui_impl_vulkan.cpp compilation unit:
//     - Add '#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES' in your imconfig.h file
//     - Or as a compilation flag in your build system
//     - Or uncomment here (not recommended because you'd be modifying imgui sources!)
//     - Do not simply add it in a .cpp file!
// (2) Call ImGui_ImplVulkan_LoadFunctions() before ImGui_ImplVulkan_Init() with your custom function.
// If you have no idea what this is, leave it alone!
//#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES

// [Configuration] Convenience support for Volk
// (you can also technically use IMGUI_IMPL_VULKAN_NO_PROTOTYPES + wrap Volk via ImGui_ImplVulkan_LoadFunctions().)
// (When using Volk from directory outside your include directories list you can specify full path to the volk.h header,
//  for example when using Volk from VulkanSDK and using include_directories(${Vulkan_INCLUDE_DIRS})' from 'find_package(Vulkan REQUIRED)')
//#define IMGUI_IMPL_VULKAN_USE_VOLK
//#define IMGUI_IMPL_VULKAN_VOLK_FILENAME    <Volk/volk.h>
//#define IMGUI_IMPL_VULKAN_VOLK_FILENAME    <volk.h>       // Default
// Reminder: make those changes in your imconfig.h file, not here!

// Clang/GCC warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast" // warning: use of old-style cast
#endif

#if defined(IMGUI_IMPL_VULKAN_NO_PROTOTYPES) && !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR) && !defined(NOMINMAX)
#define NOMINMAX
#endif

// Vulkan includes
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#ifdef IMGUI_IMPL_VULKAN_VOLK_FILENAME
#include IMGUI_IMPL_VULKAN_VOLK_FILENAME
#else
#include <volk.h>
#endif
#else
#include <vulkan/vulkan.h>
#endif
#if defined(VK_VERSION_1_3) || defined(VK_KHR_dynamic_rendering)
#define IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
#endif

// Backend uses a small number of descriptors per font atlas + as many as additional calls done to ImGui_ImplVulkan_AddTexture().
#define IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE   (8)     // Minimum per atlas




struct ImguiPushConstant{
    EWE::DeviceAddress vertices;
    EWE::TextureIndex texIndex;
    int _padding;

    lab::vec2 scale;
    lab::vec2 translate;
};

namespace EWE{
    struct LogicalDevice;
    struct Queue;
}
// Initialization data, for ImGui_ImplVulkan_Init()
// [Please zero-clear before use!]
struct ImGui_ImplVulkan_InitInfo
{
    EWE::LogicalDevice*             logicalDevice;
    EWE::Queue*                          renderQueue;

    uint32_t                        ImageCount;                 // >= MinImageCount

    //provide these
    VkPipeline                      provided_pipeline; 
    VkPipelineLayout                provided_pipeLayout;
    //^provide these


    // (Optional) Allocation, Debugging
    const VkAllocationCallbacks*    Allocator;
    void                            (*CheckVkResultFn)(VkResult err);
    VkDeviceSize                    MinAllocationSize;          // Minimum allocation size. Set to 1024*1024 to satisfy zealous best practices validation layer and waste a little memory.
};

// Follow "Getting Started" link and check examples/ folder to learn about using backends!
IMGUI_IMPL_API bool             ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo& info);
IMGUI_IMPL_API void             ImGui_ImplVulkan_Shutdown();
IMGUI_IMPL_API void             ImGui_ImplVulkan_NewFrame();
IMGUI_IMPL_API void             ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline = VK_NULL_HANDLE);
IMGUI_IMPL_API void             ImGui_ImplVulkan_SetMinImageCount(uint32_t min_image_count); // To override MinImageCount after initialization (e.g. if swap chain is recreated)

// (Advanced) Use e.g. if you need to precisely control the timing of texture updates (e.g. for staged rendering), by setting ImDrawData::Textures = nullptr to handle this manually.
IMGUI_IMPL_API void             ImGui_ImplVulkan_UpdateTexture(ImTextureData* tex);

// Register a texture (TextureIndex == ImTextureID)
IMGUI_IMPL_API EWE::TextureIndex  ImGui_ImplVulkan_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout);
IMGUI_IMPL_API void             ImGui_ImplVulkan_RemoveTexture(EWE::TextureIndex texIndex);

// Optional: load Vulkan functions with a custom function loader
// This is only useful with IMGUI_IMPL_VULKAN_NO_PROTOTYPES / VK_NO_PROTOTYPES
IMGUI_IMPL_API bool             ImGui_ImplVulkan_LoadFunctions(uint32_t api_version, PFN_vkVoidFunction(*loader_func)(const char* function_name, void* user_data), void* user_data = nullptr);

// [BETA] Selected render state data shared with callbacks.
// This is temporarily stored in GetPlatformIO().Renderer_RenderState during the ImGui_ImplVulkan_RenderDrawData() call.
// (Please open an issue if you feel you need access to more data)
struct ImGui_ImplVulkan_RenderState
{
    VkCommandBuffer     CommandBuffer;
    VkPipeline          Pipeline;
    VkPipelineLayout    PipelineLayout;
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // #ifndef IMGUI_DISABLE
