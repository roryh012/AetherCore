#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <cstring>

#include "vulkan/volk.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/OneShotCmd.hpp"

namespace aether::vkutil
{
	// Host-image-copy destination layouts the device supports, cached once at context
	// init (vkTransitionImageLayout may only target layouts from this list).
	inline std::vector<VkImageLayout> g_hostImageCopyDstLayouts;
	inline bool g_hostImageCopySupported = false;

	// Blocking fallback, each mip transitions independently from undefined to shader-read.
	inline VkResult StagedCopyMip(gpu::Device device, gpu::Queue queue, gpu::CommandPool pool,
	        gpu::Image image, const void* data, std::size_t bytes, std::uint32_t width,
	        std::uint32_t height, std::uint32_t mip = 0)
	{
		const gpu::MappedBufferDesc desc{.size = bytes, .usage = gpu::BufferUsage::TransferSrc, .debugName = "Texture.Staging"};
		const auto staging = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!staging.IsValid())
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(staging);
		if (mapped.mappedPtr == nullptr)
		{
			gpu::ResourceRegistry::Destroy(staging);
			return VK_ERROR_MEMORY_MAP_FAILED;
		}
		std::memcpy(mapped.mappedPtr, data, bytes);
		gpu::ResourceRegistry::FlushMappedBuffer(staging, 0, bytes);
		gpu::OneShotCmd cmd;
		if (!cmd.Begin(device, pool))
		{
			gpu::ResourceRegistry::Destroy(staging);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		const auto command = static_cast<VkCommandBuffer>(cmd.CmdList().GetCommandBuffer());
		const auto texture = static_cast<VkImage>(image);
		VkImageMemoryBarrier2 barrier{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = texture,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1},
		};
		const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
		vkCmdPipelineBarrier2(command, &dependency);
		const VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1}, .imageExtent = {width, height, 1}};
		vkCmdCopyBufferToImage(command, static_cast<VkBuffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(staging)),
		        texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier2(command, &dependency);
		const bool submitted = cmd.EndAndSubmit(queue);
		gpu::ResourceRegistry::Destroy(staging);
		return submitted ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
	}

	inline void SetHostImageCopyDstLayouts(std::vector<VkImageLayout> layouts)
	{
		g_hostImageCopyDstLayouts = std::move(layouts);
	}

	inline bool SupportsHostImageLayout(VkImageLayout layout)
	{
		return std::find(g_hostImageCopyDstLayouts.begin(), g_hostImageCopyDstLayouts.end(), layout) != g_hostImageCopyDstLayouts.end();
	}

	// Host-side layout transition (no queue, no command buffer, returns complete).
	inline VkResult HostTransitionImage(VkDevice device, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, std::uint32_t mipLevels = 1)
	{
		const VkHostImageLayoutTransitionInfo transition{
		        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
		        .image = image,
		        .oldLayout = oldLayout,
		        .newLayout = newLayout,
		        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = 0, .layerCount = 1},
		};
		return vkTransitionImageLayout(device, 1, &transition);
	}

	// Begin a host-copy upload through a backend-neutral call signature.
	inline std::int32_t HostTransitionImageToGeneral(gpu::Device device, gpu::Image image, std::uint32_t mipLevels)
	{
		return static_cast<std::int32_t>(HostTransitionImage(static_cast<VkDevice>(device), static_cast<VkImage>(image),
		        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, mipLevels));
	}

	// Finish a host-copied texture on the CPU: GENERAL -> SHADER_READ_ONLY_OPTIMAL with
	// no queue submission. Only valid when SupportsHostImageLayout(SHADER_READ_ONLY).
	inline std::int32_t HostTransitionImageToShaderRead(gpu::Device device, gpu::ImageView image, std::uint32_t mipLevels = 1)
	{
		return static_cast<std::int32_t>(
		        HostTransitionImage(static_cast<VkDevice>(device), static_cast<VkImage>(image), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mipLevels));
	}

	inline void TransitionImage(VkCommandBuffer cmd,
	        VkImage image,
	        VkImageLayout oldLayout,
	        VkImageLayout newLayout,
	        VkPipelineStageFlags2 srcStage,
	        VkAccessFlags2 srcAccess,
	        VkPipelineStageFlags2 dstStage,
	        VkAccessFlags2 dstAccess,
	        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
	{
		const VkImageMemoryBarrier2 barrier{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		        .srcStageMask = srcStage,
		        .srcAccessMask = srcAccess,
		        .dstStageMask = dstStage,
		        .dstAccessMask = dstAccess,
		        .oldLayout = oldLayout,
		        .newLayout = newLayout,
		        .image = image,
		        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
		};
		const VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	inline void TransitionImages(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers, uint32_t count)
	{
		if (count == 0)
		{
			return;
		}
		const VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = count,
		        .pImageMemoryBarriers = barriers,
		};
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	inline VkResult HostCopyToImage(VkDevice device, VkImage dstImage, const void* hostData, uint32_t width, uint32_t height)
	{
		const VkHostImageLayoutTransitionInfo transition{
		        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
		        .image = dstImage,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
		};
		const VkResult result = vkTransitionImageLayout(device, 1, &transition);
		if (result != VK_SUCCESS)
		{
			return result;
		}

		const VkMemoryToImageCopy region{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
		        .pHostPointer = hostData,
		        .memoryRowLength = 0,
		        .memoryImageHeight = 0,
		        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		        .imageOffset = {.x = 0, .y = 0, .z = 0},
		        .imageExtent = {.width = width, .height = height, .depth = 1},
		};
		const VkCopyMemoryToImageInfo copyInfo{
		        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
		        .dstImage = dstImage,
		        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .regionCount = 1,
		        .pRegions = &region,
		};
		return vkCopyMemoryToImage(device, &copyInfo);
	}

	inline std::int32_t HostCopyToImage(gpu::Device device, gpu::ImageView dstImage, const void* hostData, uint32_t width, uint32_t height)
	{
		return static_cast<std::int32_t>(HostCopyToImage(static_cast<VkDevice>(device), static_cast<VkImage>(dstImage), hostData, width, height));
	}

	// Copy one mip level. Unlike HostCopyToImage above this performs no layout
	// transition, because a mip chain is transitioned once for all its levels and
	// then filled level by level.
	inline VkResult HostCopyMipToImage(VkDevice device, VkImage dstImage, const void* hostData, uint32_t width, uint32_t height, uint32_t mipLevel)
	{
		const VkMemoryToImageCopy region{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
		        .pHostPointer = hostData,
		        .memoryRowLength = 0,
		        .memoryImageHeight = 0,
		        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mipLevel, .baseArrayLayer = 0, .layerCount = 1},
		        .imageOffset = {.x = 0, .y = 0, .z = 0},
		        .imageExtent = {.width = width, .height = height, .depth = 1},
		};
		const VkCopyMemoryToImageInfo copyInfo{
		        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
		        .dstImage = dstImage,
		        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .regionCount = 1,
		        .pRegions = &region,
		};
		return vkCopyMemoryToImage(device, &copyInfo);
	}

	inline std::int32_t HostCopyMipToImage(gpu::Device device, gpu::ImageView dstImage, const void* hostData, uint32_t width, uint32_t height, uint32_t mipLevel)
	{
		return static_cast<std::int32_t>(HostCopyMipToImage(static_cast<VkDevice>(device), static_cast<VkImage>(dstImage), hostData, width, height, mipLevel));
	}

	// Thread-local storage for the debug-utils function pointer.
	inline PFN_vkSetDebugUtilsObjectNameEXT g_setObjectNameFn = nullptr;

	inline void SetObjectNameFunction(PFN_vkSetDebugUtilsObjectNameEXT fn)
	{
		g_setObjectNameFn = fn;
	}

	inline void SetObjectName(VkDevice device, std::uint64_t handle, VkObjectType type, const char* name)
	{
		if (g_setObjectNameFn == nullptr || device == VK_NULL_HANDLE || handle == 0 || name == nullptr)
		{
			return;
		}
		const VkDebugUtilsObjectNameInfoEXT info{
		        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		        .objectType = type,
		        .objectHandle = handle,
		        .pObjectName = name,
		};
		g_setObjectNameFn(device, &info);
	}
} // namespace aether::vkutil
