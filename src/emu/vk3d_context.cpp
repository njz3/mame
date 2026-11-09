// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    vk3d_context.cpp

    Vulkan device shared by the GPU renderers of mame3d.

***************************************************************************/

#include "emu.h"
#include "vk3d_context.h"

#include "hires3d.h"

#include <cstring>


vk3d_context::~vk3d_context()
{
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		for (VkShaderModule module : m_modules)
			vkDestroyShaderModule(device, module, nullptr);
		if (fence)
			vkDestroyFence(device, fence, nullptr);
		if (command_pool)
			vkDestroyCommandPool(device, command_pool, nullptr);
		vkDestroyDevice(device, nullptr);
	}
	if (instance && vkDestroyInstance)
		vkDestroyInstance(instance, nullptr);
}


bool vk3d_context::load_instance_functions()
{
#define VK3D_LOAD(f) f = reinterpret_cast<PFN_##f>(vkGetInstanceProcAddr(instance, #f)); if (!f) return false;
	VK3D_INSTANCE_FUNCTIONS(VK3D_LOAD)
#undef VK3D_LOAD
	return true;
}


bool vk3d_context::load_device_functions()
{
#define VK3D_LOAD(f) f = reinterpret_cast<PFN_##f>(vkGetDeviceProcAddr(device, #f)); if (!f) return false;
	VK3D_DEVICE_FUNCTIONS(VK3D_LOAD)
#undef VK3D_LOAD
	return true;
}


bool vk3d_context::init_device(std::string &error, VkPhysicalDeviceFeatures const *wanted)
{
	library = osd::dynamic_module::open({ "vulkan-1.dll", "libvulkan.so.1", "libvulkan.so", "libvulkan.1.dylib" });
	vkGetInstanceProcAddr = library->bind<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr)
	{
		error = "Vulkan loader not found";
		return false;
	}
	vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
	if (!vkCreateInstance)
	{
		error = "vkCreateInstance not found";
		return false;
	}

	// Vulkan 1.1 when the loader has it: the frames can then be shared with
	// the OSD renderer (external memory, adapter LUID)
	u32 api = VK_API_VERSION_1_0;
	auto const enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
	u32 loader_version = 0;
	if (enumerate_version && (enumerate_version(&loader_version) == VK_SUCCESS) && (loader_version >= VK_API_VERSION_1_1))
		api = VK_API_VERSION_1_1;

	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "mame3d";
	app.pEngineName = "mame3d";
	app.apiVersion = api;
	VkInstanceCreateInfo instance_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_info.pApplicationInfo = &app;
	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
	{
		error = "vkCreateInstance failed";
		return false;
	}
	if (!load_instance_functions())
	{
		error = "missing Vulkan instance functions";
		return false;
	}

	// pick a device with a graphics queue, preferring a discrete GPU
	u32 count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	int best_score = -1;
	for (VkPhysicalDevice candidate : devices)
	{
		u32 family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
		for (u32 i = 0; i < family_count; i++)
		{
			if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			VkPhysicalDeviceProperties candidate_properties;
			vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
			int const score = (candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 :
					(candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
			if (score > best_score)
			{
				best_score = score;
				physical = candidate;
				queue_family = i;
				properties = candidate_properties;
				name = candidate_properties.deviceName;
			}
			break;
		}
	}
	if (!physical)
	{
		error = "no Vulkan device with a graphics queue";
		return false;
	}
	vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

	// what sharing the frames needs: Vulkan 1.1 on the device, its LUID and
	// the import of Windows memory handles
	char const *extensions[1];
	u32 extension_count = 0;
	if ((api >= VK_API_VERSION_1_1) && (properties.apiVersion >= VK_API_VERSION_1_1))
	{
		vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
		vkGetPhysicalDeviceImageFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
		bool luid_valid = false;
		if (vkGetPhysicalDeviceProperties2)
		{
			VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
			properties2.pNext = &id;
			vkGetPhysicalDeviceProperties2(physical, &properties2);
			luid_valid = id.deviceLUIDValid;
			std::memcpy(m_luid, id.deviceLUID, sizeof(m_luid));
		}
		u32 available = 0;
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &available, nullptr);
		std::vector<VkExtensionProperties> list(available);
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &available, list.data());
		for (VkExtensionProperties const &extension : list)
			if (!std::strcmp(extension.extensionName, "VK_KHR_external_memory_win32") && luid_valid && vkGetPhysicalDeviceImageFormatProperties2)
			{
				extensions[extension_count++] = "VK_KHR_external_memory_win32";
				m_external_memory = true;
			}
	}

	// enable the wanted features the device has
	if (wanted)
	{
		VkPhysicalDeviceFeatures supported;
		vkGetPhysicalDeviceFeatures(physical, &supported);
		VkBool32 const *const want = &wanted->robustBufferAccess;
		VkBool32 const *const have = &supported.robustBufferAccess;
		VkBool32 *const enable = &m_enabled.robustBufferAccess;
		for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
			enable[i] = (want[i] && have[i]) ? VK_TRUE : VK_FALSE;
	}

	float const priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue_info.queueFamilyIndex = queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	VkDeviceCreateInfo device_info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.pEnabledFeatures = &m_enabled;
	device_info.enabledExtensionCount = extension_count;
	device_info.ppEnabledExtensionNames = extension_count ? extensions : nullptr;
	if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS)
	{
		error = "vkCreateDevice failed";
		return false;
	}
	if (!load_device_functions())
	{
		// the device can only be destroyed with every function loaded
		if (vkDestroyDevice)
			vkDestroyDevice(device, nullptr);
		vkDestroyDevice = nullptr;
		error = "missing Vulkan device functions";
		return false;
	}
	vkGetDeviceQueue(device, queue_family, 0, &queue);
	if (m_external_memory)
	{
		vkGetMemoryWin32HandlePropertiesKHR = vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR");
		m_external_memory = (vkGetMemoryWin32HandlePropertiesKHR != nullptr);
	}

	VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family;
	vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
	VkCommandBufferAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	alloc_info.commandPool = command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;
	vkAllocateCommandBuffers(device, &alloc_info, &command_buffer);
	VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	vkCreateFence(device, &fence_info, nullptr, &fence);
	return true;
}


int vk3d_context::find_memory(u32 type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const
{
	int fallback = -1;
	for (u32 i = 0; i < memory_properties.memoryTypeCount; i++)
	{
		VkMemoryPropertyFlags const flags = memory_properties.memoryTypes[i].propertyFlags;
		if (!(type_bits & (1U << i)) || ((flags & required) != required))
			continue;
		if ((flags & preferred) == preferred)
			return i;
		if (fallback < 0)
			fallback = i;
	}
	return fallback;
}


bool vk3d_context::create_buffer(buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, bool readback_cached)
{
	VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &info, nullptr, &buf.handle) != VK_SUCCESS)
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buf.handle, &requirements);

	// host writes want coherent memory; the read-back is much faster cached
	VkMemoryPropertyFlags const required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	VkMemoryPropertyFlags const preferred = readback_cached
			? (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
			: VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	int const type = find_memory(requirements.memoryTypeBits, required, preferred);
	if (type < 0)
		return false;
	buf.coherent = (memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = requirements.size;
	alloc.memoryTypeIndex = type;
	if (vkAllocateMemory(device, &alloc, nullptr, &buf.memory) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(device, buf.handle, buf.memory, 0);
	if (vkMapMemory(device, buf.memory, 0, VK_WHOLE_SIZE, 0, &buf.mapped) != VK_SUCCESS)
		return false;
	buf.size = size;
	return true;
}


bool vk3d_context::create_device_buffer(buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &info, nullptr, &buf.handle) != VK_SUCCESS)
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buf.handle, &requirements);
	int const type = find_memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0)
		return false;
	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = requirements.size;
	alloc.memoryTypeIndex = type;
	if (vkAllocateMemory(device, &alloc, nullptr, &buf.memory) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(device, buf.handle, buf.memory, 0);
	buf.size = size;
	return true;
}


void vk3d_context::flush_buffer(buffer const &buf, VkDeviceSize offset, VkDeviceSize size)
{
	if (buf.coherent || !size)
		return;
	VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
	range.memory = buf.memory;
	range.offset = offset & ~VkDeviceSize(properties.limits.nonCoherentAtomSize - 1);
	range.size = VK_WHOLE_SIZE;
	vkFlushMappedMemoryRanges(device, 1, &range);
}


void vk3d_context::destroy_buffer(buffer &buf)
{
	// a read-back buffer can be the frame the display is drawing
	if (buf.mapped)
		hires3d_output::frames_freed(buf.mapped, size_t(buf.size));

	if (buf.handle)
		vkDestroyBuffer(device, buf.handle, nullptr);
	if (buf.memory)
		vkFreeMemory(device, buf.memory, nullptr); // unmaps it
	buf = buffer();
}


bool vk3d_context::create_image(image &img, int w, int h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = { u32(w), u32(h), 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device, &info, nullptr, &img.handle) != VK_SUCCESS)
		return false;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(device, img.handle, &requirements);
	int const type = find_memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0)
		return false;
	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = requirements.size;
	alloc.memoryTypeIndex = type;
	if (vkAllocateMemory(device, &alloc, nullptr, &img.memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(device, img.handle, img.memory, 0);
	VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	view.image = img.handle;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = { aspect, 0, 1, 0, 1 };
	return vkCreateImageView(device, &view, nullptr, &img.view) == VK_SUCCESS;
}


void vk3d_context::destroy_image(image &img)
{
	if (img.view)
		vkDestroyImageView(device, img.view, nullptr);
	if (img.handle)
		vkDestroyImage(device, img.handle, nullptr);
	if (img.memory)
		vkFreeMemory(device, img.memory, nullptr);
	img = image();
}


VkShaderModule vk3d_context::create_module(u32 const *code, size_t size)
{
	VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = size;
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	m_modules.push_back(module);
	return module;
}


bool vk3d_context::submit_and_wait()
{
	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffer;
	vkResetFences(device, 1, &fence);
	if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
		return false;
	return vkWaitForFences(device, 1, &fence, VK_TRUE, ~u64(0)) == VK_SUCCESS;
}
