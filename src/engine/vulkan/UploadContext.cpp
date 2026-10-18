#include "gpu/UploadContext.hpp"

#include <utility>

#include "gpu/CommandList.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/TransferManager.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>

namespace aether::gpu
{
	struct Impl
	{
		VkDevice device = VK_NULL_HANDLE;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		aether::ResourceRegistry* backendRegistry = nullptr;
		aether::vulkan::TransferManager* transfer = nullptr;
	};

	UploadContext::~UploadContext()
	{
		Destroy();
	}

	UploadContext::UploadContext(UploadContext&& other) noexcept
	      : m_impl(std::exchange(other.m_impl, nullptr))
	{
	}

	UploadContext& UploadContext::operator=(UploadContext&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();
			m_impl = std::exchange(other.m_impl, nullptr);
		}
		return *this;
	}

	UploadContext UploadContext::Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry, void* transferManager)
	{
		AE_PROFILE_ZONE();
		auto* vkDevice = static_cast<VkDevice>(device);

		const VkCommandPoolCreateInfo poolInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = queueFamilyIndex,
		};

		VkCommandPool pool = VK_NULL_HANDLE;
		const VkResult result = vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool);
		if (result != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::Create: vkCreateCommandPool failed (VkResult={}).", static_cast<int32_t>(result));
			return {};
		}

		Impl* impl = new Impl{};
		impl->device = vkDevice;
		impl->commandPool = pool;
		impl->queue = static_cast<VkQueue>(queue);
		impl->backendRegistry = static_cast<aether::ResourceRegistry*>(backendRegistry);
		impl->transfer = static_cast<aether::vulkan::TransferManager*>(transferManager);

		UploadContext ctx;
		ctx.m_impl = impl;
		return ctx;
	}

	void UploadContext::Destroy()
	{
		if (m_impl == nullptr)
		{
			return;
		}

		const Impl* impl = static_cast<Impl*>(m_impl);
		if (impl->device != VK_NULL_HANDLE && impl->commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(impl->device, impl->commandPool, nullptr);
		}

		delete impl;
		m_impl = nullptr;
	}

	void UploadContext::CopyBuffer(BufferHandle src, BufferHandle dst, DeviceSize size)
	{
		if (m_impl == nullptr) { AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffer called on invalid context."); return; }
		const auto ticket = CopyBufferAsync(src, dst, size);
		static_cast<Impl*>(m_impl)->transfer->WaitFor(ticket);
	}

	bool UploadContext::IsComplete(std::uint64_t ticket) const
	{
		return m_impl != nullptr && static_cast<const Impl*>(m_impl)->transfer->IsComplete(ticket);
	}

	std::uint64_t UploadContext::CopyBufferAsync(BufferHandle src, BufferHandle dst, DeviceSize size)
	{
		const BufferCopy copy{src, dst, size};
		return CopyBuffersAsync(std::span(&copy, 1));
	}

	std::uint64_t UploadContext::CopyBuffersAsync(std::span<const BufferCopy> copies, bool background)
	{
		if (!m_impl) { AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffersAsync called on invalid context."); return 0; }
		const auto* impl = static_cast<const Impl*>(m_impl);
		struct ResolvedCopy { VkBuffer src; VkBuffer dst; DeviceSize size; };
		std::vector<ResolvedCopy> resolved;
		for (const auto& copy : copies)
		{
			const auto* src = impl->backendRegistry->Resolve(copy.src);
			const auto* dst = impl->backendRegistry->Resolve(copy.dst);
			if (!src || !dst) { AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffersAsync: failed to resolve src or dst buffer."); return 0; }
			resolved.push_back({src->buffer, dst->buffer, copy.size});
		}
		return impl->transfer->Submit([&](CommandList& list)
		{
			for (const auto& copy : resolved)
			{
				list.CopyBuffer(static_cast<void*>(copy.src), static_cast<void*>(copy.dst), 0, 0, copy.size);
			}
		}, {}, !background);
	}

	bool UploadContext::PublishCompleted(std::uint64_t ticket)
	{
		if (!IsComplete(ticket)) { return false; }
		static_cast<Impl*>(m_impl)->transfer->PublishToGraphics(ticket);
		return true;
	}

	void* UploadContext::GetCommandPool() const
	{
		if (m_impl == nullptr)
		{
			return nullptr;
		}
		const Impl* impl = static_cast<Impl*>(m_impl);
		return static_cast<void*>(impl->commandPool);
	}
} // namespace aether::gpu
