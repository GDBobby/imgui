#include "imgui.h"

#include "EightWinds/LogicalDevice.h"
#include "EightWinds/Logger.h"
#include <vulkan/vulkan_core.h>

#ifndef IMGUI_DISABLE
#include "imgui_impl_vulkan.h"
#include <stdio.h>
#ifndef IM_MAX
#define IM_MAX(A, B)    (((A) >= (B)) ? (A) : (B))
#endif
#undef Status // X11 headers are leaking this.

// Visual Studio warnings
#ifdef _MSC_VER
#pragma warning (disable: 4127) // condition expression is constant
#endif

// Clang/GCC warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wold-style-cast"                 // warning: use of old-style cast
#pragma clang diagnostic ignored "-Wsign-conversion"                // warning: implicit conversion changes signedness
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"  // warning: implicit conversion from 'xxx' to 'float' may lose precision
#pragma clang diagnostic ignored "-Wcast-function-type"             // warning: cast between incompatible function types (for loader)
#endif

// Forward Declarations
struct ImGui_ImplVulkan_FrameRenderBuffers;
struct ImGui_ImplVulkan_WindowRenderBuffers;
bool ImGui_ImplVulkan_CreateDeviceObjects();
void ImGui_ImplVulkan_DestroyDeviceObjects();
void ImGui_ImplVulkan_DestroyFrameRenderBuffers(VkDevice device, ImGui_ImplVulkan_FrameRenderBuffers* buffers, const VkAllocationCallbacks* allocator);
void ImGui_ImplVulkan_DestroyWindowRenderBuffers(VkDevice device, ImGui_ImplVulkan_WindowRenderBuffers* buffers, const VkAllocationCallbacks* allocator);


static PFN_vkCmdBeginRenderingKHR   ImGuiImplVulkanFuncs_vkCmdBeginRenderingKHR;
static PFN_vkCmdEndRenderingKHR     ImGuiImplVulkanFuncs_vkCmdEndRenderingKHR;

// Reusable buffers used for rendering 1 current in-flight frame, for ImGui_ImplVulkan_RenderDrawData()
// [Please zero-clear before use!]
struct ImGui_ImplVulkan_FrameRenderBuffers
{
    VkDeviceMemory      VertexBufferMemory;
    VkDeviceMemory      IndexBufferMemory;
    VkDeviceSize        VertexBufferSize;
    VkDeviceSize        IndexBufferSize;
    VkBuffer            VertexBuffer;
    VkBuffer            IndexBuffer;

    EWE::DeviceAddress  VertexAddress;
};

// Each viewport will hold 1 ImGui_ImplVulkanH_WindowRenderBuffers
// [Please zero-clear before use!]
struct ImGui_ImplVulkan_WindowRenderBuffers
{
    uint32_t            Index;
    uint32_t            Count;
    ImVector<ImGui_ImplVulkan_FrameRenderBuffers> FrameRenderBuffers;
};

struct ImGui_ImplVulkan_Texture
{
    VkDeviceMemory              Memory;
    VkImage                     Image;
    VkImageView                 ImageView;
    EWE::TextureIndex           textureIndex;

    ImGui_ImplVulkan_Texture() { memset((void*)this, 0, sizeof(*this)); }
};

// Vulkan data
struct ImGui_ImplVulkan_Data
{

    ImGui_ImplVulkan_InitInfo   VulkanInitInfo;
    VkDeviceSize                BufferMemoryAlignment;
    VkDeviceSize                NonCoherentAtomSize;

    // Texture management
    VkSampler                   TexSamplerLinear;
    VkCommandPool               TexCommandPool;
    VkCommandBuffer             TexCommandBuffer;

    VkFence                     fence;

    // Render buffers for main window
    ImGui_ImplVulkan_WindowRenderBuffers MainWindowRenderBuffers;

    ImguiPushConstant push;

    ImGui_ImplVulkan_Data()
    {
        memset((void*)this, 0, sizeof(*this));
        BufferMemoryAlignment = 256;
        NonCoherentAtomSize = 64;
    }
};

//-----------------------------------------------------------------------------
// FUNCTIONS
//-----------------------------------------------------------------------------

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
// FIXME: multi-context support is not tested and probably dysfunctional in this backend.
static ImGui_ImplVulkan_Data* ImGui_ImplVulkan_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplVulkan_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

static uint32_t ImGui_ImplVulkan_MemoryType(VkMemoryPropertyFlags properties, uint32_t type_bits)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    VkPhysicalDeviceMemoryProperties prop;
    vkGetPhysicalDeviceMemoryProperties(v->logicalDevice->physicalDevice, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1 << i))
            return i;
    return 0xFFFFFFFF; // Unable to find memoryType
}

static void check_vk_result(VkResult err)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    if (!bd)
        return;
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    if (v->CheckVkResultFn)
        v->CheckVkResultFn(err);
}

// Same as IM_MEMALIGN(). 'alignment' must be a power of two.
static inline VkDeviceSize AlignBufferSize(VkDeviceSize size, VkDeviceSize alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}

static void CreateOrResizeBuffer(VkBuffer& buffer, VkDeviceMemory& buffer_memory, VkDeviceSize& buffer_size, VkDeviceSize new_size, VkBufferUsageFlagBits usage)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    VkResult err;
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(v->logicalDevice->device, buffer, v->Allocator);
    if (buffer_memory != VK_NULL_HANDLE)
        vkFreeMemory(v->logicalDevice->device, buffer_memory, v->Allocator);

    VkDeviceSize buffer_size_aligned = AlignBufferSize(IM_MAX(v->MinAllocationSize, new_size), bd->BufferMemoryAlignment);
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size_aligned;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    err = vkCreateBuffer(v->logicalDevice->device, &buffer_info, v->Allocator, &buffer);
    check_vk_result(err);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->logicalDevice->device, buffer, &req);
    bd->BufferMemoryAlignment = (bd->BufferMemoryAlignment > req.alignment) ? bd->BufferMemoryAlignment : req.alignment;

    VkMemoryAllocateFlagsInfo memAllocFlagInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = nullptr,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        .deviceMask = 0
    };
    

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &memAllocFlagInfo;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = ImGui_ImplVulkan_MemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
    err = vkAllocateMemory(v->logicalDevice->device, &alloc_info, v->Allocator, &buffer_memory);
    check_vk_result(err);

    err = vkBindBufferMemory(v->logicalDevice->device, buffer, buffer_memory, 0);
    check_vk_result(err);
    buffer_size = buffer_size_aligned;
}

static void ImGui_ImplVulkan_SetupRenderState(ImDrawData* draw_data, VkPipeline pipeline, VkCommandBuffer command_buffer, ImGui_ImplVulkan_FrameRenderBuffers* rb, int fb_width, int fb_height)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        bd->VulkanInitInfo.provided_pipeLayout,
        0, 1,
        &bd->VulkanInitInfo.logicalDevice->bindlessDescriptor.set,
        0, nullptr
    );
    

    if (draw_data->TotalVtxCount > 0) {
        //VkBuffer vertex_buffers[1] = { rb->VertexBuffer };
        //VkDeviceSize vertex_offset[1] = { 0 };
        //vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, vertex_offset);
        bd->push.vertices = rb->VertexAddress;
        vkCmdBindIndexBuffer(command_buffer, rb->IndexBuffer, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }

    // Setup viewport:
    VkViewport viewport;
    viewport.x = draw_data->OwnerViewport->Pos.x;
    viewport.y = draw_data->OwnerViewport->Pos.y;
    viewport.width = (float)fb_width;
    viewport.height = (float)fb_height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    
    //scissor?

    // Setup scale and translation:
    // Our visible imgui space lies from draw_data->DisplayPps (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
    {
        //bd->push.vertices = rb->VertexAddress;
        bd->push.scale = lab::vec2{
            2.0f / draw_data->DisplaySize.x,
            2.0f / draw_data->DisplaySize.y
        };
        bd->push.translate = lab::vec2{
            -1.0f - draw_data->DisplayPos.x * bd->push.scale.x,
            -1.0f - draw_data->DisplayPos.y * bd->push.scale.y
        };
    }
}

// Render function
void ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline)
{
    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
    int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0)
        return;

    // Catch up with texture updates. Most of the times, the list will have 1 element with an OK status, aka nothing to do.
    // (This almost always points to ImGui::GetPlatformIO().Textures[] but is part of ImDrawData to allow overriding or disabling texture updates).
    if (draw_data->Textures != nullptr)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplVulkan_UpdateTexture(tex);

    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    if (pipeline == VK_NULL_HANDLE)
        pipeline = bd->VulkanInitInfo.provided_pipeline;

    // Allocate array to store enough vertex/index buffers
    ImGui_ImplVulkan_WindowRenderBuffers* wrb = &bd->MainWindowRenderBuffers;
    if (wrb->FrameRenderBuffers.Size == 0)
    {
        wrb->Index = 0;
        wrb->Count = v->ImageCount;
        wrb->FrameRenderBuffers.resize(wrb->Count);
        memset((void*)wrb->FrameRenderBuffers.Data, 0, wrb->FrameRenderBuffers.size_in_bytes());
    }
    IM_ASSERT(wrb->Count == v->ImageCount);
    wrb->Index = (wrb->Index + 1) % wrb->Count;
    ImGui_ImplVulkan_FrameRenderBuffers* rb = &wrb->FrameRenderBuffers[wrb->Index];

    if (draw_data->TotalVtxCount > 0)
    {
        // Create or resize the vertex/index buffers
        VkDeviceSize vertex_size = AlignBufferSize(draw_data->TotalVtxCount * sizeof(ImDrawVert), bd->BufferMemoryAlignment);
        VkDeviceSize index_size = AlignBufferSize(draw_data->TotalIdxCount * sizeof(ImDrawIdx), bd->BufferMemoryAlignment);
        if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBufferSize < vertex_size){
            CreateOrResizeBuffer(rb->VertexBuffer, rb->VertexBufferMemory, rb->VertexBufferSize, vertex_size, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            
            //EWE::Log::Debug("vertex buffer[%zu] - count{%u} - size{%u}\n", reinterpret_cast<std::size_t>(rb->VertexBuffer), draw_data->TotalVtxCount, vertex_size);

            VkBufferDeviceAddressInfo bdaInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = rb->VertexBuffer
            };
            rb->VertexAddress = vkGetBufferDeviceAddress(v->logicalDevice->device, &bdaInfo);
        }
        if (rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBufferSize < index_size) {
            CreateOrResizeBuffer(rb->IndexBuffer, rb->IndexBufferMemory, rb->IndexBufferSize, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            //EWE::Log::Debug("index buffer[%zu] - count{%u} - size{%u}\n", reinterpret_cast<std::size_t>(rb->IndexBuffer), draw_data->TotalIdxCount, index_size);
        }


        // Upload vertex/index data into a single contiguous GPU buffer
        ImDrawVert* vtx_dst = nullptr;
        ImDrawIdx* idx_dst = nullptr;
        VkResult err = vkMapMemory(v->logicalDevice->device, rb->VertexBufferMemory, 0, vertex_size, 0, (void**)&vtx_dst);
        check_vk_result(err);
        err = vkMapMemory(v->logicalDevice->device, rb->IndexBufferMemory, 0, index_size, 0, (void**)&idx_dst);
        check_vk_result(err);
        for (const ImDrawList* draw_list : draw_data->CmdLists)
        {
            memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
            memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
            vtx_dst += draw_list->VtxBuffer.Size;
            idx_dst += draw_list->IdxBuffer.Size;
        }
        VkMappedMemoryRange range[2] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = rb->VertexBufferMemory;
        range[0].size = VK_WHOLE_SIZE;
        range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[1].memory = rb->IndexBufferMemory;
        range[1].size = VK_WHOLE_SIZE;
        err = vkFlushMappedMemoryRanges(v->logicalDevice->device, 2, range);
        check_vk_result(err);
        vkUnmapMemory(v->logicalDevice->device, rb->VertexBufferMemory);
        vkUnmapMemory(v->logicalDevice->device, rb->IndexBufferMemory);
    }

    // Setup desired Vulkan state
    ImGui_ImplVulkan_SetupRenderState(draw_data, pipeline, command_buffer, rb, fb_width, fb_height);

    // Setup render state structure (for callbacks and custom texture bindings)
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGui_ImplVulkan_RenderState render_state;
    render_state.CommandBuffer = command_buffer;
    render_state.Pipeline = pipeline;
    render_state.PipelineLayout = bd->VulkanInitInfo.provided_pipeLayout;
    platform_io.Renderer_RenderState = &render_state;

    // Will project scissor/clipping rectangles into framebuffer space
    ImVec2 clip_off = draw_data->DisplayPos;         // (0,0) unless using multi-viewports
    ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

    // Render command lists
    // (Because we merged all buffers into a single one, we maintain our own offset into them)
    int global_vtx_offset = 0;
    int global_idx_offset = 0;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback != nullptr)
            {
                // User callback, registered via ImDrawList::AddCallback()
                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    ImGui_ImplVulkan_SetupRenderState(draw_data, pipeline, command_buffer, rb, fb_width, fb_height);
                else
                    pcmd->UserCallback(draw_list, pcmd);
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 scissor_offset{
                    pcmd->ClipRect.x * clip_scale.x, 
                    pcmd->ClipRect.y * clip_scale.y
                };
                ImVec2 scissor_extent{
                    (pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                    (pcmd->ClipRect.w - clip_off.y) * clip_scale.y
                };

                // Clamp to viewport as vkCmdSetScissor() won't accept values that are off bounds
                if (scissor_offset.x < 0.0f) { scissor_offset.x = 0.0f; }
                if (scissor_offset.y < 0.0f) { scissor_offset.y = 0.0f; }
                if (scissor_extent.x > fb_width) { scissor_extent.x = (float)fb_width; }
                if (scissor_extent.y > fb_height) { scissor_extent.y = (float)fb_height; }
                if (scissor_extent.x <= 0.f || scissor_extent.y <= 0.f){
                    continue;
                }

                VkRect2D scissor;
                scissor.offset.x = (int32_t)(scissor_offset.x);
                scissor.offset.y = (int32_t)(scissor_offset.y);
                scissor.extent.width = uint32_t(scissor_extent.x);
                scissor.extent.height = uint32_t(scissor_extent.y);
                vkCmdSetScissor(command_buffer, 0, 1, &scissor);

                //i dont really feel like figuring out why this mf relies on scissor so much. ill leave it as is

                
                bd->push.texIndex = pcmd->GetTexID();
                vkCmdPushConstants(command_buffer, bd->VulkanInitInfo.provided_pipeLayout, VK_SHADER_STAGE_ALL, 0, sizeof(ImguiPushConstant), &bd->push);
                //EWE::Log::Debug("indexed data : count{%u} - first{%u} - vert_offset{%u}\n", pcmd->ElemCount, pcmd->IdxOffset + global_idx_offset, pcmd->VtxOffset + global_vtx_offset);
                vkCmdDrawIndexed(command_buffer, pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset, pcmd->VtxOffset + global_vtx_offset, 0);
            }
        }
        global_idx_offset += draw_list->IdxBuffer.Size;
        global_vtx_offset += draw_list->VtxBuffer.Size;
    }
    platform_io.Renderer_RenderState = nullptr;
}

static void ImGui_ImplVulkan_DestroyTexture(ImTextureData* tex)
{
    if (ImGui_ImplVulkan_Texture* backend_tex = (ImGui_ImplVulkan_Texture*)tex->BackendUserData)
    {
        IM_ASSERT(backend_tex->textureIndex == (VkDescriptorSet)tex->TexID);
        ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
        ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
        ImGui_ImplVulkan_RemoveTexture(backend_tex->textureIndex);
        vkDestroyImageView(v->logicalDevice->device, backend_tex->ImageView, v->Allocator);
        vkDestroyImage(v->logicalDevice->device, backend_tex->Image, v->Allocator);
        vkFreeMemory(v->logicalDevice->device, backend_tex->Memory, v->Allocator);
        IM_DELETE(backend_tex);

        // Clear identifiers and mark as destroyed (in order to allow e.g. calling InvalidateDeviceObjects while running)
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
    }
    tex->SetStatus(ImTextureStatus_Destroyed);
}

void ImGui_ImplVulkan_UpdateTexture(ImTextureData* tex)
{
    if (tex->Status == ImTextureStatus_OK)
        return;
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    VkResult err;

    if (tex->Status == ImTextureStatus_WantCreate)
    {
        // Create and upload new texture to graphics system
        //IMGUI_DEBUG_LOG("UpdateTexture #%03d: WantCreate %dx%d\n", tex->UniqueID, tex->Width, tex->Height);
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        ImGui_ImplVulkan_Texture* backend_tex = IM_NEW(ImGui_ImplVulkan_Texture)();

        // Create the Image:
        {
            VkImageCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R8G8B8A8_UNORM;
            info.extent.width = tex->Width;
            info.extent.height = tex->Height;
            info.extent.depth = 1;
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            err = vkCreateImage(v->logicalDevice->device, &info, v->Allocator, &backend_tex->Image);
            check_vk_result(err);
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(v->logicalDevice->device, backend_tex->Image, &req);
            VkMemoryAllocateInfo alloc_info = {};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.allocationSize = IM_MAX(v->MinAllocationSize, req.size);
            alloc_info.memoryTypeIndex = ImGui_ImplVulkan_MemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
            err = vkAllocateMemory(v->logicalDevice->device, &alloc_info, v->Allocator, &backend_tex->Memory);
            check_vk_result(err);
            err = vkBindImageMemory(v->logicalDevice->device, backend_tex->Image, backend_tex->Memory, 0);
            check_vk_result(err);
        }

        // Create the Image View:
        {
            VkImageViewCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = backend_tex->Image;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = VK_FORMAT_R8G8B8A8_UNORM;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            info.subresourceRange.levelCount = 1;
            info.subresourceRange.layerCount = 1;
            err = vkCreateImageView(v->logicalDevice->device, &info, v->Allocator, &backend_tex->ImageView);
            check_vk_result(err);
        }

        // Create the Descriptor Set
        backend_tex->textureIndex = ImGui_ImplVulkan_AddTexture(bd->TexSamplerLinear, backend_tex->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EWE::Log::Debug("imgui added texture : %d\n", backend_tex->textureIndex.value);

        // Store identifiers
        tex->SetTexID((ImTextureID)backend_tex->textureIndex);
        tex->BackendUserData = backend_tex;
    }

    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
    {
        ImGui_ImplVulkan_Texture* backend_tex = (ImGui_ImplVulkan_Texture*)tex->BackendUserData;

        // Update full texture or selected blocks. We only ever write to textures regions which have never been used before!
        // This backend choose to use tex->UpdateRect but you can use tex->Updates[] to upload individual regions.
        // We could use the smaller rect on _WantCreate but using the full rect allows us to clear the texture.
        const int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
        const int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
        const int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
        const int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;

        // Create the Upload Buffer:
        VkDeviceMemory upload_buffer_memory;

        VkBuffer upload_buffer;
        VkDeviceSize upload_pitch = upload_w * tex->BytesPerPixel;
        VkDeviceSize upload_size = AlignBufferSize(upload_h * upload_pitch, bd->NonCoherentAtomSize);
        {
            VkBufferCreateInfo buffer_info = {};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = upload_size;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            err = vkCreateBuffer(v->logicalDevice->device, &buffer_info, v->Allocator, &upload_buffer);
            check_vk_result(err);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(v->logicalDevice->device, upload_buffer, &req);
            bd->BufferMemoryAlignment = (bd->BufferMemoryAlignment > req.alignment) ? bd->BufferMemoryAlignment : req.alignment;
            VkMemoryAllocateInfo alloc_info = {};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.allocationSize = IM_MAX(v->MinAllocationSize, req.size);
            alloc_info.memoryTypeIndex = ImGui_ImplVulkan_MemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
            err = vkAllocateMemory(v->logicalDevice->device, &alloc_info, v->Allocator, &upload_buffer_memory);
            check_vk_result(err);
            err = vkBindBufferMemory(v->logicalDevice->device, upload_buffer, upload_buffer_memory, 0);
            check_vk_result(err);
        }

        // Upload to Buffer:
        {
            char* map = nullptr;
            err = vkMapMemory(v->logicalDevice->device, upload_buffer_memory, 0, upload_size, 0, (void**)(&map));
            check_vk_result(err);
            for (int y = 0; y < upload_h; y++)
                memcpy(map + upload_pitch * y, tex->GetPixelsAt(upload_x, upload_y + y), (size_t)upload_pitch);
            VkMappedMemoryRange range[1] = {};
            range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range[0].memory = upload_buffer_memory;
            range[0].size = upload_size;
            err = vkFlushMappedMemoryRanges(v->logicalDevice->device, 1, range);
            check_vk_result(err);
            vkUnmapMemory(v->logicalDevice->device, upload_buffer_memory);
        }

        // Start command buffer
        {
            err = vkResetCommandPool(v->logicalDevice->device, bd->TexCommandPool, 0);
            check_vk_result(err);
            VkCommandBufferBeginInfo begin_info = {};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            err = vkBeginCommandBuffer(bd->TexCommandBuffer, &begin_info);
            check_vk_result(err);
        }

        // Copy to Image:
        {
            VkBufferMemoryBarrier upload_barrier[1] = {};
            upload_barrier[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            upload_barrier[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            upload_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            upload_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            upload_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            upload_barrier[0].buffer = upload_buffer;
            upload_barrier[0].offset = 0;
            upload_barrier[0].size = upload_size;

            VkImageMemoryBarrier copy_barrier[1] = {};
            copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            copy_barrier[0].oldLayout = (tex->Status == ImTextureStatus_WantCreate) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            copy_barrier[0].image = backend_tex->Image;
            copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy_barrier[0].subresourceRange.levelCount = 1;
            copy_barrier[0].subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(bd->TexCommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, upload_barrier, 1, copy_barrier);

            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = upload_w;
            region.imageExtent.height = upload_h;
            region.imageExtent.depth = 1;
            region.imageOffset.x = upload_x;
            region.imageOffset.y = upload_y;
            vkCmdCopyBufferToImage(bd->TexCommandBuffer, upload_buffer, backend_tex->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier use_barrier[1] = {};
            use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            use_barrier[0].image = backend_tex->Image;
            use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            use_barrier[0].subresourceRange.levelCount = 1;
            use_barrier[0].subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(bd->TexCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, use_barrier);
        }

        // End command buffer
        {
            VkSubmitInfo end_info = {};
            end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            end_info.commandBufferCount = 1;
            end_info.pCommandBuffers = &bd->TexCommandBuffer;
            err = vkEndCommandBuffer(bd->TexCommandBuffer);
            check_vk_result(err);
            v->renderQueue->Submit(1, &end_info, bd->fence);
            //err = vkQueueSubmit(v->renqueues[0], 1, &end_info, VK_NULL_HANDLE);
            check_vk_result(err);
        }

        err = vkWaitForFences(v->logicalDevice->device, 1, &bd->fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);
        err = vkResetFences(v->logicalDevice->device, 1, &bd->fence);
        check_vk_result(err);

        vkDestroyBuffer(v->logicalDevice->device, upload_buffer, v->Allocator);
        vkFreeMemory(v->logicalDevice->device, upload_buffer_memory, v->Allocator);

        tex->SetStatus(ImTextureStatus_OK);
    }

    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= (int)bd->VulkanInitInfo.ImageCount)
        ImGui_ImplVulkan_DestroyTexture(tex);
}

bool ImGui_ImplVulkan_CreateDeviceObjects()
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    VkResult err;

    if (!bd->TexSamplerLinear)
    {
        // Bilinear sampling is required by default. Set 'io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines' or 'style.AntiAliasedLinesUseTex = false' to allow point/nearest sampling.
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(v->logicalDevice->device, &info, v->Allocator, &bd->TexSamplerLinear);
        check_vk_result(err);
    }
    // Create command pool/buffer for texture upload
    if (!bd->TexCommandPool)
    {
        VkCommandPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = 0;
        info.queueFamilyIndex = v->renderQueue->family.index;
        err = vkCreateCommandPool(v->logicalDevice->device, &info, v->Allocator, &bd->TexCommandPool);
        check_vk_result(err);
    }
    if (!bd->TexCommandBuffer)
    {
        VkCommandBufferAllocateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = bd->TexCommandPool;
        info.commandBufferCount = 1;
        err = vkAllocateCommandBuffers(v->logicalDevice->device, &info, &bd->TexCommandBuffer);
        check_vk_result(err);
    }
    if(bd->fence == VK_NULL_HANDLE){
        VkFenceCreateInfo fenceCreateInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0
        };
        vkCreateFence(v->logicalDevice->device, &fenceCreateInfo, nullptr, &bd->fence);
    }

    return true;
}

void    ImGui_ImplVulkan_DestroyDeviceObjects()
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;
    ImGui_ImplVulkan_DestroyWindowRenderBuffers(v->logicalDevice->device, &bd->MainWindowRenderBuffers, v->Allocator);

    // Destroy all textures
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1)
            ImGui_ImplVulkan_DestroyTexture(tex);

    if (bd->TexCommandBuffer)     { vkFreeCommandBuffers(v->logicalDevice->device, bd->TexCommandPool, 1, &bd->TexCommandBuffer); bd->TexCommandBuffer = VK_NULL_HANDLE; }
    if (bd->TexCommandPool)       { vkDestroyCommandPool(v->logicalDevice->device, bd->TexCommandPool, v->Allocator); bd->TexCommandPool = VK_NULL_HANDLE; }
    if (bd->TexSamplerLinear)     { vkDestroySampler(v->logicalDevice->device, bd->TexSamplerLinear, v->Allocator); bd->TexSamplerLinear = VK_NULL_HANDLE; }
}

static void ImGui_ImplVulkan_LoadDynamicRenderingFunctions(uint32_t api_version, PFN_vkVoidFunction(*loader_func)(const char* function_name, void* user_data), void* user_data)
{
    IM_UNUSED(api_version);

    // Manually load those two (see #5446, #8326, #8365, #8600)
    // - Try loading core (non-KHR) versions first (this will work for Vulkan 1.3+ and the device supports dynamic rendering)
    ImGuiImplVulkanFuncs_vkCmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(loader_func("vkCmdBeginRendering", user_data));
    ImGuiImplVulkanFuncs_vkCmdEndRenderingKHR = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(loader_func("vkCmdEndRendering", user_data));

    // - Fallback to KHR versions if core not available (this will work if KHR extension is available and enabled and also the device supports dynamic rendering)
    if (ImGuiImplVulkanFuncs_vkCmdBeginRenderingKHR == nullptr || ImGuiImplVulkanFuncs_vkCmdEndRenderingKHR == nullptr)
    {
        ImGuiImplVulkanFuncs_vkCmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(loader_func("vkCmdBeginRenderingKHR", user_data));
        ImGuiImplVulkanFuncs_vkCmdEndRenderingKHR = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(loader_func("vkCmdEndRenderingKHR", user_data));
    }
}

bool    ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo& info)
{

#ifndef IMGUI_IMPL_VULKAN_USE_LOADER
        ImGui_ImplVulkan_LoadDynamicRenderingFunctions(
            info.logicalDevice->instance.api_version, 
            [](const char* function_name, void* user_data) { 
                return vkGetDeviceProcAddr((VkDevice)user_data, function_name); 
            }, 
            (void*)info.logicalDevice->device
        );

        IM_ASSERT(ImGuiImplVulkanFuncs_vkCmdBeginRenderingKHR != nullptr);
        IM_ASSERT(ImGuiImplVulkanFuncs_vkCmdEndRenderingKHR != nullptr);
#else
        IM_ASSERT(0 && "Can't use dynamic rendering when neither VK_VERSION_1_3 or VK_KHR_dynamic_rendering is defined.");
#endif

    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

    // Setup backend capabilities flags
    ImGui_ImplVulkan_Data* bd = IM_NEW(ImGui_ImplVulkan_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_vulkan";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.

    bd->VulkanInitInfo = info;

    bd->NonCoherentAtomSize = info.logicalDevice->properties.properties.limits.nonCoherentAtomSize;

    if (!ImGui_ImplVulkan_CreateDeviceObjects())
        IM_ASSERT(0 && "ImGui_ImplVulkan_CreateDeviceObjects() failed!"); // <- Can't be hit yet.

    return true;
}

void ImGui_ImplVulkan_Shutdown()
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    ImGui_ImplVulkan_DestroyDeviceObjects();

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    platform_io.ClearRendererHandlers();
    IM_DELETE(bd);
}

void ImGui_ImplVulkan_NewFrame()
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplVulkan_Init()?");
    IM_UNUSED(bd);
}

// Register a texture by creating a descriptor
// FIXME: This is experimental in the sense that we are unsure how to best design/tackle this problem, please post to https://github.com/ocornut/imgui/pull/914 if you have suggestions.
EWE::TextureIndex ImGui_ImplVulkan_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout) {
    VkDescriptorImageInfo rawDII{
        .sampler = sampler,
        .imageView = image_view,
        .imageLayout = image_layout
    };

    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    auto ret = bd->VulkanInitInfo.logicalDevice->bindlessDescriptor.BindImage(rawDII);
    return ret;

}

void ImGui_ImplVulkan_RemoveTexture(EWE::TextureIndex texture_index)
{
    ImGui_ImplVulkan_Data* bd = ImGui_ImplVulkan_GetBackendData();
    ImGui_ImplVulkan_InitInfo* v = &bd->VulkanInitInfo;

    v->logicalDevice->bindlessDescriptor.UnbindRaw(texture_index);
}

void ImGui_ImplVulkan_DestroyFrameRenderBuffers(VkDevice device, ImGui_ImplVulkan_FrameRenderBuffers* buffers, const VkAllocationCallbacks* allocator)
{
    if (buffers->VertexBuffer) { vkDestroyBuffer(device, buffers->VertexBuffer, allocator); buffers->VertexBuffer = VK_NULL_HANDLE; }
    if (buffers->VertexBufferMemory) { vkFreeMemory(device, buffers->VertexBufferMemory, allocator); buffers->VertexBufferMemory = VK_NULL_HANDLE; }
    if (buffers->IndexBuffer) { vkDestroyBuffer(device, buffers->IndexBuffer, allocator); buffers->IndexBuffer = VK_NULL_HANDLE; }
    if (buffers->IndexBufferMemory) { vkFreeMemory(device, buffers->IndexBufferMemory, allocator); buffers->IndexBufferMemory = VK_NULL_HANDLE; }
    buffers->VertexBufferSize = 0;
    buffers->IndexBufferSize = 0;
}

void ImGui_ImplVulkan_DestroyWindowRenderBuffers(VkDevice device, ImGui_ImplVulkan_WindowRenderBuffers* buffers, const VkAllocationCallbacks* allocator)
{
    for (uint32_t n = 0; n < buffers->Count; n++)
        ImGui_ImplVulkan_DestroyFrameRenderBuffers(device, &buffers->FrameRenderBuffers[n], allocator);
    buffers->FrameRenderBuffers.clear();
    buffers->Index = 0;
    buffers->Count = 0;
}

//-------------------------------------------------------------------------
// Internal / Miscellaneous Vulkan Helpers
// (Used by example's main.cpp. Used by multi-viewport features. PROBABLY NOT used by your own engine/app.)
//-------------------------------------------------------------------------
// You probably do NOT need to use or care about those functions.
// Those functions only exist because:
//   1) they facilitate the readability and maintenance of the multiple main.cpp examples files.
//   2) the upcoming multi-viewport feature will need them internally.
// Generally we avoid exposing any kind of superfluous high-level helpers in the backends,
// but it is too much code to duplicate everywhere so we exceptionally expose them.
//
// Your engine/app will likely _already_ have code to setup all that stuff (swap chain, render pass, frame buffers, etc.).
// You may read this code to learn about Vulkan, but it is recommended you use your own custom tailored code to do equivalent work.
// (The ImGui_ImplVulkanH_XXX functions do not interact with any of the state used by the regular ImGui_ImplVulkan_XXX functions)
//-------------------------------------------------------------------------

VkSurfaceFormatKHR ImGui_ImplVulkanH_SelectSurfaceFormat(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkFormat* request_formats, int request_formats_count, VkColorSpaceKHR request_color_space)
{
    IM_ASSERT(request_formats != nullptr);
    IM_ASSERT(request_formats_count > 0);

    // Per Spec Format and View Format are expected to be the same unless VK_IMAGE_CREATE_MUTABLE_BIT was set at image creation
    // Assuming that the default behavior is without setting this bit, there is no need for separate Swapchain image and image view format
    // Additionally several new color spaces were introduced with Vulkan Spec v1.0.40,
    // hence we must make sure that a format with the mostly available color space, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, is found and used.
    uint32_t avail_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &avail_count, nullptr);
    ImVector<VkSurfaceFormatKHR> avail_format;
    avail_format.resize((int)avail_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &avail_count, avail_format.Data);

    // First check if only one format, VK_FORMAT_UNDEFINED, is available, which would imply that any format is available
    if (avail_count == 1)
    {
        if (avail_format[0].format == VK_FORMAT_UNDEFINED)
        {
            VkSurfaceFormatKHR ret;
            ret.format = request_formats[0];
            ret.colorSpace = request_color_space;
            return ret;
        }
        else
        {
            // No point in searching another format
            return avail_format[0];
        }
    }
    else
    {
        // Request several formats, the first found will be used
        for (int request_i = 0; request_i < request_formats_count; request_i++)
            for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
                if (avail_format[avail_i].format == request_formats[request_i] && avail_format[avail_i].colorSpace == request_color_space)
                    return avail_format[avail_i];

        // If none of the requested image formats could be found, use the first available
        return avail_format[0];
    }
}

VkPresentModeKHR ImGui_ImplVulkanH_SelectPresentMode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkPresentModeKHR* request_modes, int request_modes_count)
{
    IM_ASSERT(request_modes != nullptr);
    IM_ASSERT(request_modes_count > 0);

    // Request a certain mode and confirm that it is available. If not use VK_PRESENT_MODE_FIFO_KHR which is mandatory
    uint32_t avail_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &avail_count, nullptr);
    ImVector<VkPresentModeKHR> avail_modes;
    avail_modes.resize((int)avail_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &avail_count, avail_modes.Data);
    //for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
    //    printf("[vulkan] avail_modes[%d] = %d\n", avail_i, avail_modes[avail_i]);

    for (int request_i = 0; request_i < request_modes_count; request_i++)
        for (uint32_t avail_i = 0; avail_i < avail_count; avail_i++)
            if (request_modes[request_i] == avail_modes[avail_i])
                return request_modes[request_i];

    return VK_PRESENT_MODE_FIFO_KHR; // Always available
}

VkPhysicalDevice ImGui_ImplVulkanH_SelectPhysicalDevice(VkInstance instance)
{
    uint32_t gpu_count;
    VkResult err = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    check_vk_result(err);
    IM_ASSERT(gpu_count > 0);

    ImVector<VkPhysicalDevice> gpus;
    gpus.resize(gpu_count);
    err = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.Data);
    check_vk_result(err);

    // If a number >1 of GPUs got reported, find discrete GPU if present, or use first one available. This covers
    // most common cases (multi-gpu/integrated+dedicated graphics). Handling more complicated setups (multiple
    // dedicated GPUs) is out of scope of this sample.
    for (VkPhysicalDevice& device : gpus)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            return device;
    }

    // Use first GPU (Integrated) is a Discrete one is not available.
    if (gpu_count > 0)
        return gpus[0];
    return VK_NULL_HANDLE;
}


uint32_t ImGui_ImplVulkanH_SelectQueueFamilyIndex(VkPhysicalDevice physical_device)
{
    uint32_t count;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
    ImVector<VkQueueFamilyProperties> queues_properties;
    queues_properties.resize((int)count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queues_properties.Data);
    for (uint32_t i = 0; i < count; i++)
        if (queues_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            return i;
    return (uint32_t)-1;
}

//-----------------------------------------------------------------------------

#endif // #ifndef IMGUI_DISABLE