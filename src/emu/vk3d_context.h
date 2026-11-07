// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    vk3d_context.h

    Vulkan device shared by the GPU renderers of mame3d.

    Loads the Vulkan library at run time, creates an instance and a
    device with one graphics queue, and provides the buffer, image and
    shader helpers the renderers use.  Only files that talk to Vulkan
    include this header.

***************************************************************************/

#ifndef MAME_EMU_VK3D_CONTEXT_H
#define MAME_EMU_VK3D_CONTEXT_H

#pragma once

#include "modules/lib/osdlib.h"

#define VK_NO_PROTOTYPES
#include "bgfx/3rdparty/khronos/vulkan-local/vulkan.h"

#include <string>
#include <vector>


#define VK3D_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceFeatures) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr)

#define VK3D_DEVICE_FUNCTIONS(X) \
	X(vkDestroyDevice) \
	X(vkDeviceWaitIdle) \
	X(vkGetDeviceQueue) \
	X(vkQueueSubmit) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkResetCommandBuffer) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkGetFenceStatus) \
	X(vkCreateQueryPool) \
	X(vkDestroyQueryPool) \
	X(vkGetQueryPoolResults) \
	X(vkCmdResetQueryPool) \
	X(vkCmdWriteTimestamp) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkMapMemory) \
	X(vkFlushMappedMemoryRanges) \
	X(vkInvalidateMappedMemoryRanges) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkBindBufferMemory) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyBuffer) \
	X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdPushConstants) \
	X(vkCmdClearAttachments) \
	X(vkCmdDraw)


class vk3d_context
{
public:
	// a buffer; host buffers stay mapped
	struct buffer
	{
		VkBuffer handle = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void *mapped = nullptr;
		VkDeviceSize size = 0;
		bool coherent = true;
	};

	// a 2D image with one view
	struct image
	{
		VkImage handle = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};

	vk3d_context() = default;
	vk3d_context(vk3d_context const &) = delete;
	vk3d_context &operator=(vk3d_context const &) = delete;
	virtual ~vk3d_context();

	// load Vulkan and create the device, with the optional features in
	// wanted that the GPU supports (enabled_features() tells which ones)
	bool init_device(std::string &error, VkPhysicalDeviceFeatures const *wanted = nullptr);
	VkPhysicalDeviceFeatures const &enabled_features() const { return m_enabled; }

	// memory helpers
	int find_memory(u32 type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const;
	bool create_buffer(buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, bool readback_cached); // host visible, mapped
	bool create_device_buffer(buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage); // device local
	void flush_buffer(buffer const &buf, VkDeviceSize offset, VkDeviceSize size);
	void destroy_buffer(buffer &buf);
	bool create_image(image &img, int w, int h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
	void destroy_image(image &img);

	// shader modules live as long as the context
	VkShaderModule create_module(u32 const *code, size_t size);

	// submit command_buffer and wait for it
	bool submit_and_wait();

	osd::dynamic_module::ptr library;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkCreateInstance vkCreateInstance = nullptr;
#define VK3D_DECLARE(f) PFN_##f f = nullptr;
	VK3D_INSTANCE_FUNCTIONS(VK3D_DECLARE)
	VK3D_DEVICE_FUNCTIONS(VK3D_DECLARE)
#undef VK3D_DECLARE

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory_properties{};
	VkPhysicalDeviceProperties properties{};
	std::string name;
	VkDevice device = VK_NULL_HANDLE;
	u32 queue_family = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

private:
	bool load_instance_functions();
	bool load_device_functions();

	VkPhysicalDeviceFeatures m_enabled{};
	std::vector<VkShaderModule> m_modules;
};

#endif // MAME_EMU_VK3D_CONTEXT_H
