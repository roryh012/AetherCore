#include "scripting/interop/InteropCommon.hpp"

#include <cmath>
#include <cstring>
#include <chrono>

#include "assets/AssetManager.hpp"
#include "material/MaterialRegistry.hpp"
#include "scene/Components.hpp"
#include "scripting/ProceduralStream.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	bool Finite(glm::vec3 value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	bool Finite(Vec2 value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y);
	}
} // namespace

AE_SCRIPT_API std::uint32_t aether_create_procedural_instance(std::uint32_t source)
{
	return SafeExport(
	        [&]() -> std::uint32_t
	        {
		        auto& ctx = ActiveContext();
		        auto& world = ActiveWorld();
		        if (!EntityAlive(source) || !ctx.assets)
		        {
			        return 0;
		        }
		        const auto* material = world.TryGet<aether::MaterialComponent>(aether::Entity{source});
		        const auto* pipeline = world.TryGet<aether::PipelineComponent>(aether::Entity{source});
		        if (!material || !pipeline)
		        {
			        return 0;
		        }
		        const auto materialCopy = *material;
		        const auto pipelineCopy = *pipeline;
		        if (!ctx.assets->GetMaterialRegistry().Retain(materialCopy.handle))
		        {
			        return 0;
		        }
		        const auto entity = world.Create();
		        world.Emplace<aether::TransformComponent>(entity);
		        world.Emplace<aether::MaterialComponent>(entity, materialCopy);
		        world.Emplace<aether::PipelineComponent>(entity, pipelineCopy);
		        ctx.sceneEntities.push_back(entity);
		        return entity.id;
	        });
}

AE_SCRIPT_API std::uint64_t aether_queue_procedural_mesh(
        std::uint32_t entity, const Vec3* positions, const Vec3* normals, const Vec3* colors, int vertexCount, const std::uint32_t* indices, int indexCount, const Vec2* texcoords, const Vec2* secondary, int flags, int priority)
{
	return SafeExport(
	        [&]() -> std::uint64_t
	        {
		        const auto start = std::chrono::steady_clock::now();
		        auto& ctx = ActiveContext();
		        if (!EntityAlive(entity) || vertexCount < 0 || vertexCount > 1000000 || indexCount < 0 || indexCount > 6000000 || indexCount % 3 || ((vertexCount == 0) != (indexCount == 0)) || flags < 0 || flags > 7 || (flags & 1 && flags & 6)
		                || (vertexCount && (!positions || !indices || (!(flags & 2) && !normals))))
		        {
			        return 0;
		        }
		        ProceduralStream::Data data;
		        data.visual = !(flags & 2);
		        data.collision = (flags & 6) != 0;
		        if (data.visual && (!ActiveWorld().Has<aether::MaterialComponent>(aether::Entity{entity}) || !ActiveWorld().Has<aether::PipelineComponent>(aether::Entity{entity})))
		        {
			        return 0;
		        }
		        // Reject oversized work before allocating a native copy.
		        const auto bytes = 3ull * (static_cast<std::uint64_t>(vertexCount) * sizeof(aether::Mesh::Vertex) + static_cast<std::uint64_t>(indexCount) * 4);
		        if (!ctx.proceduralStream.CanQueue(bytes))
		        {
			        return 0;
		        }
		        data.vertices.reserve(vertexCount);
		        data.indices.reserve(indexCount);
		        for (int i = 0; i < vertexCount; ++i)
		        {
			        aether::Mesh::Vertex vertex{};
			        vertex.position = ToGlm(positions[i]);
			        vertex.normal = normals ? ToGlm(normals[i]) : glm::vec3(0, 1, 0);
			        vertex.color = colors ? ToGlm(colors[i]) : glm::vec3(1);
			        vertex.tangent = {1, 0, 0, 1};
			        if (!Finite(vertex.position) || !Finite(vertex.normal) || !Finite(vertex.color) || (texcoords && !Finite(texcoords[i])) || (secondary && !Finite(secondary[i])))
			        {
				        return 0;
			        }
			        if (texcoords)
			        {
				        vertex.uv = ToGlm(texcoords[i]);
			        }
			        if (secondary)
			        {
				        vertex.uv2 = ToGlm(secondary[i]);
			        }
			        if (flags & 1)
			        {
				        vertex.uv2.y = -9183.0f;
			        }
			        data.vertices.push_back(vertex);
		        }
		        for (int i = 0; i < indexCount; ++i)
		        {
			        if (indices[i] >= static_cast<std::uint32_t>(vertexCount))
			        {
				        return 0;
			        }
			        data.indices.push_back(indices[i]);
		        }
		        data.inputCopyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		        return ctx.proceduralStream.Queue(ctx, entity, std::move(data), priority);
	        });
}

AE_SCRIPT_API std::uint64_t aether_queue_procedural_section(std::uint32_t entity, Vec3 origin, float spacing, int cells, int height, const float* density, int count, const float* basalt, int priority)
{
	return SafeExport(
	        [&]() -> std::uint64_t
	        {
		        const auto start = std::chrono::steady_clock::now();
		        auto& ctx = ActiveContext();
		        if (!EntityAlive(entity) || cells < 1 || cells > 64 || height < 1 || height > 1024 || !Finite(ToGlm(origin)) || origin.y != 0 || !std::isfinite(spacing) || spacing <= 0 || !std::isfinite(height * spacing)
		                || count != (cells + 1) * (cells + 1) * (height + 1) || !density || !basalt || !ActiveWorld().Has<aether::MaterialComponent>(aether::Entity{entity}) || !ActiveWorld().Has<aether::PipelineComponent>(aether::Entity{entity}))
		        {
			        return 0;
		        }
		        if (!ctx.proceduralStream.CanQueue(3ull * (400 + 24 + 32 + count * sizeof(float))))
		        {
			        return 0;
		        }
		        for (int i = 0; i < count; ++i)
		        {
			        if (!std::isfinite(density[i]))
			        {
				        return 0;
			        }
		        }
		        for (int i = 0; i < 4; ++i)
		        {
			        if (!std::isfinite(basalt[i]))
			        {
				        return 0;
			        }
		        }
		        const auto span = cells * spacing;
		        if (!std::isfinite(span) || !Finite(ToGlm(origin) + glm::vec3(span, 0, span)))
		        {
			        return 0;
		        }
		        const glm::vec3 positions[] = {{origin.x, 0, origin.z}, {origin.x, 0, origin.z + span}, {origin.x + span, 0, origin.z + span}, {origin.x + span, 0, origin.z}};
		        ProceduralStream::Data data;
		        for (int i = 0; i < 4; ++i)
		        {
			        aether::Mesh::Vertex vertex{};
			        vertex.position = positions[i];
			        vertex.normal = {0, 1, 0};
			        vertex.tangent = {1, 0, 0, 1};
			        vertex.uv = {basalt[i], 2};
			        vertex.uv2 = {-9182, 0};
			        data.vertices.push_back(vertex);
		        }
		        data.indices = {0, 1, 2, 0, 2, 3};
		        const float header[] = {origin.x, 0, origin.z, spacing, float(cells + 1), float(height + 1), float(cells + 1), 0};
		        data.sectionTop = height * spacing;
		        data.section.resize(sizeof(header) + count * sizeof(float));
		        std::memcpy(data.section.data(), header, sizeof(header));
		        std::memcpy(data.section.data() + sizeof(header), density, count * sizeof(float));
		        data.inputCopyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		        return ctx.proceduralStream.Queue(ctx, entity, std::move(data), priority);
	        });
}

AE_SCRIPT_API ProceduralStream::Status aether_procedural_poll(std::uint64_t ticket)
{
	return SafeExport([&] { return ActiveContext().proceduralStream.Poll(ticket); });
}

AE_SCRIPT_API ProceduralStream::Stats aether_procedural_stats()
{
	return SafeExport([&] { return ActiveContext().proceduralStream.GetStats(); });
}

AE_SCRIPT_API int aether_procedural_commit(std::uint64_t ticket)
{
	return SafeExport(
	        [&]() -> int
	        {
		        auto& ctx = ActiveContext();
		        return ctx.proceduralStream.Commit(ctx, ticket);
	        });
}

AE_SCRIPT_API int aether_procedural_cancel(std::uint64_t ticket)
{
	return SafeExport(
	        [&]() -> int
	        {
		        auto& ctx = ActiveContext();
		        return ctx.proceduralStream.Cancel(ctx, ticket);
	        });
}

AE_SCRIPT_API int aether_procedural_streaming_version()
{
	return 1;
}

static_assert(sizeof(ProceduralStream::Status) == 72);
static_assert(sizeof(ProceduralStream::Stats) == 24);
