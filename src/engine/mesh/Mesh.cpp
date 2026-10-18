#include "mesh/Mesh.hpp"

#include <cstring>

#include "gpu/ResourceRegistry.hpp"
#include "gpu/UploadContext.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	Mesh Mesh::CreateView(gpu::BufferHandle vertexBuffer,
	        gpu::BufferHandle indexBuffer,
	        std::uint32_t vertexCount,
	        std::uint32_t indexCount,
	        gpu::DeviceSize vertexByteOffset,
	        gpu::DeviceSize indexByteOffset,
	        gpu::DeviceAddress vertexDeviceAddress,
	        gpu::DeviceAddress indexDeviceAddress)
	{
		Mesh mesh;
		mesh.m_aliveSentinel = Mesh::kAliveSentinel;
		mesh.m_buffer = vertexBuffer;
		mesh.m_vertexCount = vertexCount;
		mesh.m_vertexByteOffset = vertexByteOffset;
		mesh.m_vertexDeviceAddress = vertexDeviceAddress;
		mesh.m_indexBuffer = indexBuffer;
		mesh.m_indexCount = indexCount;
		mesh.m_indexByteOffset = indexByteOffset;
		mesh.m_indexDeviceAddress = indexDeviceAddress;
		return mesh;
	}

	Mesh Mesh::Create(gpu::UploadContext& uploadContext, std::span<const Vertex> vertices, std::span<const std::byte> shaderData)
	{
		AE_PROFILE_ZONE();
		const gpu::DeviceSize vertexSize = sizeof(Vertex) * vertices.size();
		const gpu::DeviceSize size = vertexSize + shaderData.size();

		gpu::MappedBufferDesc stagingDesc{};
		stagingDesc.size = size;
		stagingDesc.usage = gpu::BufferUsage::TransferSrc;
		stagingDesc.debugName = "Mesh.Staging";
		const gpu::BufferHandle stagingHandle = gpu::ResourceRegistry::CreateMappedBuffer(stagingDesc);
		if (!stagingHandle.IsValid())
		{
			return {};
		}

		const gpu::MappedBufferView stagingView = gpu::ResourceRegistry::ResolveMappedBuffer(stagingHandle);
		if (stagingView.mappedPtr == nullptr)
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return {};
		}

		std::memcpy(stagingView.mappedPtr, vertices.data(), static_cast<std::size_t>(vertexSize));
		if (!shaderData.empty())
		{
			std::memcpy(static_cast<std::byte*>(stagingView.mappedPtr) + vertexSize, shaderData.data(), shaderData.size());
		}
		gpu::ResourceRegistry::FlushMappedBuffer(stagingHandle, 0, size);

		gpu::BufferDesc bufferDesc{};
		bufferDesc.size = size;
		bufferDesc.usage = gpu::BufferUsage::Vertex | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		bufferDesc.debugName = "Mesh.Vertex";
		const gpu::BufferHandle bufferHandle = gpu::ResourceRegistry::CreateBuffer(bufferDesc);
		if (!bufferHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return {};
		}

		uploadContext.CopyBuffer(stagingHandle, bufferHandle, size);

		gpu::ResourceRegistry::Destroy(stagingHandle);

		const gpu::ResourceRegistry::ResolvedBuffer resolved = gpu::ResourceRegistry::ResolveBuffer(bufferHandle);

		Mesh mesh;
		mesh.m_aliveSentinel = Mesh::kAliveSentinel;
		mesh.m_ownsBuffers = true;
		mesh.m_buffer = bufferHandle;
		mesh.m_vertexCount = static_cast<std::uint32_t>(vertices.size());
		mesh.m_vertexDeviceAddress = resolved.deviceAddress;
		return mesh;
	}

	Mesh Mesh::Create(gpu::UploadContext& uploadContext, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices, const float* aabbMin, const float* aabbMax, const float* sphereCenter, float sphereRadius, std::span<const std::byte> shaderData)
	{
		Mesh mesh = Create(uploadContext, vertices, shaderData);
		if (!mesh.IsValid())
		{
			// Vertex-buffer creation failed: the returned mesh does not own its buffers,
			// so an index buffer created below would never be released by Destroy().
			return mesh;
		}
		mesh.m_indexCount = static_cast<std::uint32_t>(indices.size());

		const gpu::DeviceSize indexSize = sizeof(std::uint32_t) * indices.size();

		gpu::MappedBufferDesc stagingDesc{};
		stagingDesc.size = indexSize;
		stagingDesc.usage = gpu::BufferUsage::TransferSrc;
		stagingDesc.debugName = "Mesh.IndexStaging";
		const gpu::BufferHandle stagingHandle = gpu::ResourceRegistry::CreateMappedBuffer(stagingDesc);
		if (!stagingHandle.IsValid())
		{
			return mesh;
		}

		const gpu::MappedBufferView stagingView = gpu::ResourceRegistry::ResolveMappedBuffer(stagingHandle);
		if (stagingView.mappedPtr == nullptr)
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return mesh;
		}

		std::memcpy(stagingView.mappedPtr, indices.data(), static_cast<std::size_t>(indexSize));
		gpu::ResourceRegistry::FlushMappedBuffer(stagingHandle, 0, indexSize);

		gpu::BufferDesc bufferDesc{};
		bufferDesc.size = indexSize;
		bufferDesc.usage = gpu::BufferUsage::Index | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		bufferDesc.debugName = "Mesh.Index";
		const gpu::BufferHandle bufferHandle = gpu::ResourceRegistry::CreateBuffer(bufferDesc);
		if (!bufferHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return mesh;
		}

		uploadContext.CopyBuffer(stagingHandle, bufferHandle, indexSize);
		gpu::ResourceRegistry::Destroy(stagingHandle);

		const gpu::ResourceRegistry::ResolvedBuffer resolved = gpu::ResourceRegistry::ResolveBuffer(bufferHandle);

		mesh.m_indexBuffer = bufferHandle;
		mesh.m_indexDeviceAddress = resolved.deviceAddress;

		if (aabbMin)
		{
			mesh.m_aabbMin = glm::vec3(aabbMin[0], aabbMin[1], aabbMin[2]);
		}
		if (aabbMax)
		{
			mesh.m_aabbMax = glm::vec3(aabbMax[0], aabbMax[1], aabbMax[2]);
		}
		if (sphereCenter)
		{
			mesh.m_boundingSphere = glm::vec4(sphereCenter[0], sphereCenter[1], sphereCenter[2], sphereRadius);
		}

		return mesh;
	}

	Mesh Mesh::CreateAsync(gpu::UploadContext& uploadContext, std::span<const Vertex> vertices,
	        std::span<const std::uint32_t> indices, std::span<const std::byte> shaderData,
	        std::vector<gpu::BufferHandle>& staging, std::uint64_t& ticket, float sectionTop)
	{
		Mesh mesh;
		if (vertices.empty() || indices.empty()) { return mesh; }
		staging.reserve(2);
		// shortcut: immutable allocations; consider capacity pooling only if measured allocation time dominates traversal.
		const auto vertexBytes = vertices.size_bytes() + shaderData.size();
		const auto indexBytes = indices.size_bytes();
		auto allocate = [&](gpu::DeviceSize size, gpu::BufferUsage usage)
		{
			const auto src = gpu::ResourceRegistry::CreateMappedBuffer({.size = size, .usage = gpu::BufferUsage::TransferSrc, .debugName = "Procedural.Staging"});
			staging.push_back(src);
			return gpu::ResourceRegistry::CreateBuffer({.size = size, .usage = usage | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress, .debugName = "Procedural.Mesh"});
		};
		mesh.m_ownsBuffers = true;
		mesh.m_buffer = allocate(vertexBytes, gpu::BufferUsage::Vertex);
		mesh.m_indexBuffer = allocate(indexBytes, gpu::BufferUsage::Index);
		if (!mesh.m_buffer.IsValid() || !mesh.m_indexBuffer.IsValid() || !staging[0].IsValid() || !staging[1].IsValid())
		{
			return mesh;
		}
		const auto v = gpu::ResourceRegistry::ResolveMappedBuffer(staging[0]);
		const auto i = gpu::ResourceRegistry::ResolveMappedBuffer(staging[1]);
		if (!v.mappedPtr || !i.mappedPtr)
		{
			return mesh;
		}
		std::memcpy(v.mappedPtr, vertices.data(), vertices.size_bytes());
		if (!shaderData.empty())
		{
			std::memcpy(static_cast<std::byte*>(v.mappedPtr) + vertices.size_bytes(), shaderData.data(), shaderData.size());
		}
		std::memcpy(i.mappedPtr, indices.data(), indexBytes);
		gpu::ResourceRegistry::FlushMappedBuffer(staging[0], 0, vertexBytes);
		gpu::ResourceRegistry::FlushMappedBuffer(staging[1], 0, indexBytes);
		const gpu::UploadContext::BufferCopy copies[] = {{staging[0], mesh.m_buffer, vertexBytes}, {staging[1], mesh.m_indexBuffer, indexBytes}};
		mesh.m_vertexCount = static_cast<std::uint32_t>(vertices.size());
		mesh.m_indexCount = static_cast<std::uint32_t>(indices.size());
		mesh.m_vertexDeviceAddress = gpu::ResourceRegistry::ResolveBuffer(mesh.m_buffer).deviceAddress;
		mesh.m_indexDeviceAddress = gpu::ResourceRegistry::ResolveBuffer(mesh.m_indexBuffer).deviceAddress;
		mesh.m_aabbMin = mesh.m_aabbMax = vertices.front().position;
		for (const auto& vertex : vertices)
		{
			mesh.m_aabbMin = glm::min(mesh.m_aabbMin, vertex.position);
			mesh.m_aabbMax = glm::max(mesh.m_aabbMax, vertex.position);
		}
		if (vertices.front().uv2.y == -9183.0f) { mesh.m_aabbMin.y = 0; }
		if (sectionTop > 0) { mesh.m_aabbMin.y = 0; mesh.m_aabbMax.y = sectionTop; }
		const auto center = (mesh.m_aabbMin + mesh.m_aabbMax) * 0.5f;
		mesh.m_boundingSphere = glm::vec4(center, glm::length(mesh.m_aabbMax - center));
		ticket = uploadContext.CopyBuffersAsync(copies, true);
		return mesh;
	}

	Mesh::~Mesh()
	{
		Destroy();
	}

	Mesh::Mesh(Mesh&& other) noexcept
	      : m_ownsBuffers(other.m_ownsBuffers),
	        m_buffer(other.m_buffer),
	        m_vertexCount(other.m_vertexCount),
	        m_indexBuffer(other.m_indexBuffer),
	        m_indexCount(other.m_indexCount),
	        m_vertexByteOffset(other.m_vertexByteOffset),
	        m_indexByteOffset(other.m_indexByteOffset),
	        m_vertexDeviceAddress(other.m_vertexDeviceAddress),
	        m_indexDeviceAddress(other.m_indexDeviceAddress),
	        m_aabbMin(other.m_aabbMin),
	        m_aabbMax(other.m_aabbMax),
	        m_boundingSphere(other.m_boundingSphere),
	        m_aliveSentinel(other.m_aliveSentinel),
	        m_generation(other.m_generation)
	{
		other.m_ownsBuffers = false;
		other.m_buffer = {};
		other.m_vertexCount = 0;
		other.m_indexBuffer = {};
		other.m_indexCount = 0;
		other.m_vertexByteOffset = 0;
		other.m_indexByteOffset = 0;
		other.m_vertexDeviceAddress = 0;
		other.m_indexDeviceAddress = 0;
		other.m_aabbMin = glm::vec3(0.0f);
		other.m_aabbMax = glm::vec3(0.0f);
		other.m_boundingSphere = glm::vec4(0.0f);
		other.m_aliveSentinel = 0;
		other.m_generation = 0;
	}

	Mesh& Mesh::operator=(Mesh&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();

			m_ownsBuffers = other.m_ownsBuffers;
			m_buffer = other.m_buffer;
			m_vertexCount = other.m_vertexCount;
			m_indexBuffer = other.m_indexBuffer;
			m_indexCount = other.m_indexCount;
			m_vertexByteOffset = other.m_vertexByteOffset;
			m_indexByteOffset = other.m_indexByteOffset;
			m_vertexDeviceAddress = other.m_vertexDeviceAddress;
			m_indexDeviceAddress = other.m_indexDeviceAddress;
			m_aabbMin = other.m_aabbMin;
			m_aabbMax = other.m_aabbMax;
			m_boundingSphere = other.m_boundingSphere;
			m_aliveSentinel = other.m_aliveSentinel;
			m_generation = other.m_generation;

			other.m_ownsBuffers = false;
			other.m_buffer = {};
			other.m_vertexCount = 0;
			other.m_indexBuffer = {};
			other.m_indexCount = 0;
			other.m_vertexByteOffset = 0;
			other.m_indexByteOffset = 0;
			other.m_vertexDeviceAddress = 0;
			other.m_indexDeviceAddress = 0;
			other.m_aabbMin = glm::vec3(0.0f);
			other.m_aabbMax = glm::vec3(0.0f);
			other.m_boundingSphere = glm::vec4(0.0f);
			other.m_aliveSentinel = 0;
			other.m_generation = 0;
		}
		return *this;
	}

	void Mesh::Destroy()
	{
		++m_generation;
		if (m_indexBuffer.IsValid() && m_ownsBuffers)
		{
			gpu::ResourceRegistry::Destroy(m_indexBuffer);
			m_indexBuffer = {};
		}
		m_indexCount = 0;
		if (m_buffer.IsValid() && m_ownsBuffers)
		{
			gpu::ResourceRegistry::Destroy(m_buffer);
			m_buffer = {};
		}
		m_ownsBuffers = false;
		m_vertexCount = 0;
	}
} // namespace aether
