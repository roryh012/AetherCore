#include "scripting/interop/InteropCommon.hpp"

#include <string>
#include <cmath>
#include <string_view>
#include <vector>
#include <cstring>

#include "assets/AssetManager.hpp"
#include "IEngineRuntime.hpp"
#include "physics/PhysicsSystem.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialAuthoring.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

static int SetProceduralMesh(std::uint32_t id, const Vec3* positions,
        const Vec3* normals, const Vec3* colors, int vertexCount,
        const std::uint32_t* indices, int indexCount, int collision, const Vec2* texcoords, std::span<const std::byte> shaderData = {}, float sectionTop = 0.0f, bool horizon = false)
{
	return SafeExport([&]() -> int
	{
		auto& ctx = ActiveContext();
		auto& world = ActiveWorld();
		if (!EntityAlive(id) || ctx.assets == nullptr || ctx.engineRuntime == nullptr ||
		        (collision != 0 && ctx.physics == nullptr) || vertexCount < 0 || vertexCount > 1000000 ||
		        indexCount < 0 || indexCount > 6000000 || indexCount % 3 != 0 ||
		        ((vertexCount == 0) != (indexCount == 0)) ||
		        (vertexCount > 0 && (positions == nullptr || normals == nullptr || indices == nullptr)))
		{
			return 0;
		}
		std::vector<aether::Mesh::Vertex> vertices;
		std::vector<glm::vec3> colliderVertices;
		std::vector<std::uint32_t> triangleIndices;
		vertices.reserve(vertexCount);
		colliderVertices.reserve(vertexCount);
		for (int i = 0; i < vertexCount; ++i)
		{
			const auto p = ToGlm(positions[i]);
			const auto n = ToGlm(normals[i]);
			const auto c = colors != nullptr ? ToGlm(colors[i]) : glm::vec3(1.0f);
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(p[axis]) || !std::isfinite(n[axis]) || !std::isfinite(c[axis]))
				{
					return 0;
				}
			}
			aether::Mesh::Vertex v{};
			v.position = p;
			v.normal = n;
			v.tangent = glm::vec4(1, 0, 0, 1);
			v.color = c;
			if (horizon) { v.uv2.y = -9183.0f; }
			if (!shaderData.empty()) { v.uv2 = {-9182.0f, 0.0f}; }
			if (texcoords != nullptr)
			{
				if (!std::isfinite(texcoords[i].x) || !std::isfinite(texcoords[i].y))
				{
					return 0;
				}
				v.uv = glm::vec2(texcoords[i].x, texcoords[i].y);
			}
			vertices.push_back(v);
			colliderVertices.push_back(p);
		}
		for (int i = 0; i < indexCount; ++i)
		{
			if (indices[i] >= static_cast<std::uint32_t>(vertexCount))
			{
				return 0;
			}
			triangleIndices.push_back(indices[i]);
		}
		const aether::Entity entity{id};
		glm::vec3 boundsMin(0.0f), boundsMax(0.0f);
		if (!vertices.empty())
		{
			boundsMin = boundsMax = vertices.front().position;
			for (const auto& v : vertices)
			{
				boundsMin = glm::min(boundsMin, v.position);
				boundsMax = glm::max(boundsMax, v.position);
			}
		}
		if (horizon) { boundsMin.y = 0.0f; }
		if (sectionTop > 0.0f)
		{
			boundsMin.y = 0.0f;
			boundsMax.y = sectionTop;
		}
		const glm::vec3 sphereCenter = (boundsMin + boundsMax) * 0.5f;
		int succeeded = 1;
		ctx.engineRuntime->RunExclusive(aether::QuiesceMode::DrainCpuOnly, [&]()
		{
			std::unique_ptr<aether::Mesh> mesh;
			if (!vertices.empty())
			{
				mesh = std::make_unique<aether::Mesh>(ctx.assets->CreateMesh(vertices, triangleIndices,
				        &boundsMin.x, &boundsMax.x, &sphereCenter.x, glm::length(boundsMax - sphereCenter), shaderData));
				if (!mesh->IsValid())
				{
					succeeded = 0;
					return;
				}
			}
			if (ctx.physics != nullptr)
			{
				ctx.physics->WaitForStepIdle();
				ctx.physics->RemoveBody(world, entity);
			}
			world.Remove<aether::ColliderComponent>(entity);
			world.Remove<aether::RigidBodyComponent>(entity);
			world.Remove<aether::MeshComponent>(entity);
			world.Remove<aether::MeshSourceComponent>(entity);
			if (auto old = ctx.proceduralMeshes.find(id); old != ctx.proceduralMeshes.end())
			{
				ctx.engineRuntime->RetireAfterRendering(std::shared_ptr<aether::Mesh>(std::move(old->second)));
				ctx.proceduralMeshes.erase(old);
			}
			if (mesh == nullptr)
			{
				return;
			}
			const auto* meshPtr = mesh.get();
			ctx.proceduralMeshes.emplace(id, std::move(mesh));
			world.EmplaceOrReplace<aether::MeshComponent>(entity, aether::MeshComponent{.mesh = meshPtr});
			if (!world.Has<aether::TransformComponent>(entity))
			{
				world.Emplace<aether::TransformComponent>(entity, aether::TransformComponent{});
			}
			if (!world.Has<aether::MaterialComponent>(entity))
			{
				aether::MaterialAsset material{};
				material.baseColorFactor = glm::vec4(1.0f);
				material.roughnessFactor = 0.9f;
				material.modulateVertexColor = true;
				material.doubleSided = true;
				aether::MaterialSystem::AssignMaterial(world, entity, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), material);
			}
			if (collision != 0)
			{
				aether::ColliderComponent collider{};
				collider.shape = aether::PhysicsShapeType::Mesh;
				collider.layer = aether::PhysicsLayer::NonMoving;
				collider.meshVertices = std::move(colliderVertices);
				collider.meshIndices = triangleIndices;
				world.EmplaceOrReplace<aether::ColliderComponent>(entity, std::move(collider));
				world.EmplaceOrReplace<aether::RigidBodyComponent>(entity, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Static});
				ctx.physics->FlushPendingOnly(world);
				const auto* body = world.TryGet<aether::RigidBodyComponent>(entity);
				succeeded = body != nullptr && body->body.IsValid() ? 1 : 0;
			}
		});
		return succeeded;
	});
}

AE_SCRIPT_API int aether_set_procedural_mesh(std::uint32_t id, const Vec3* positions,
        const Vec3* normals, const Vec3* colors, int vertexCount,
        const std::uint32_t* indices, int indexCount, int collision)
{
	return SetProceduralMesh(id, positions, normals, colors, vertexCount, indices, indexCount, collision, nullptr);
}

AE_SCRIPT_API int aether_set_procedural_mesh_uv(std::uint32_t id, const Vec3* positions,
        const Vec3* normals, const Vec3* colors, int vertexCount,
        const std::uint32_t* indices, int indexCount, int collision, const Vec2* texcoords)
{
	return SetProceduralMesh(id, positions, normals, colors, vertexCount, indices, indexCount, collision, texcoords);
}

// Physics-only geometry does not allocate a rendered mesh or drain GPU frames.
// The game thread owns registry mutations; synchronise only the physics worker.
AE_SCRIPT_API int aether_set_procedural_collision(std::uint32_t id, const Vec3* positions,
        int vertexCount, const std::uint32_t* indices, int indexCount)
{
    return SafeExport([&]() -> int
    {
        auto& ctx = ActiveContext();
        auto& world = ActiveWorld();
        if (!EntityAlive(id) || ctx.physics == nullptr || vertexCount < 0 || vertexCount > 1000000 ||
                indexCount < 0 || indexCount > 6000000 || indexCount % 3 != 0 ||
                ((vertexCount == 0) != (indexCount == 0)) ||
                (vertexCount > 0 && (positions == nullptr || indices == nullptr))) return 0;
        aether::ColliderComponent collider{};
        collider.shape = aether::PhysicsShapeType::Mesh;
        collider.layer = aether::PhysicsLayer::NonMoving;
        collider.meshVertices.reserve(vertexCount);
        collider.meshIndices.reserve(indexCount);
        for (int i = 0; i < vertexCount; ++i)
        {
            auto p = ToGlm(positions[i]);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return 0;
            collider.meshVertices.push_back(p);
        }
        for (int i = 0; i < indexCount; ++i)
        {
            if (indices[i] >= static_cast<std::uint32_t>(vertexCount)) return 0;
            collider.meshIndices.push_back(indices[i]);
        }
        const aether::Entity entity{id};
        ctx.physics->WaitForStepIdle();
        ctx.physics->RemoveBody(world, entity);
        world.Remove<aether::ColliderComponent>(entity);
        world.Remove<aether::RigidBodyComponent>(entity);
        if (vertexCount == 0) return 1;
        if (!world.Has<aether::TransformComponent>(entity))
            world.Emplace<aether::TransformComponent>(entity, aether::TransformComponent{});
        world.EmplaceOrReplace<aether::ColliderComponent>(entity, std::move(collider));
        world.EmplaceOrReplace<aether::RigidBodyComponent>(entity, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Static});
        ctx.physics->FlushPendingOnly(world);
        const auto* body = world.TryGet<aether::RigidBodyComponent>(entity);
        return body != nullptr && body->body.IsValid() ? 1 : 0;
    });
}

AE_SCRIPT_API void aether_load_model(std::uint32_t id, const char* pathC)
{
	SafeExport([&] -> void
	{
	// The id is parented under (SpawnModel -> SetParent) and gets a NameComponent,
	// so a dead id would emplace onto the registry before the model even loads.
	if (!EntityAlive(id))
	{
		return;
	}
	auto& ctx = ActiveContext();
	auto& w = ActiveWorld();
	const std::string path = pathC != nullptr ? pathC : "";
	if (ctx.assets == nullptr)
	{
		AE_WARN(aether::LogCategory::App, "load_model: no asset manager");
		return;
	}

	glm::mat4 xform{1.0f};
	if (const auto* tc = w.TryGet<aether::TransformComponent>(aether::Entity{id}))
	{
		xform = tc->localToWorld;
	}

	aether::LoadedModel* modelPtr = aether::app::scene::LoadCachedModel(*ctx.assets, ctx, path);
	if (modelPtr == nullptr)
	{
		return;
	}

	std::string stem = path;
	if (const auto slash = stem.find_last_of("/\\"); slash != std::string::npos)
	{
		stem = stem.substr(slash + 1);
	}
	if (const auto dot = stem.find_last_of('.'); dot != std::string::npos)
	{
		stem = stem.substr(0, dot);
	}
	if (stem.empty())
	{
		stem = "Model";
	}
	w.EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = stem});

	// Replacing a model: the previous model's generated mesh children (MeshSource
	// Kind::Model) are destroyed, not just detached. Detaching left them standing in the
	// world as orphans, so a script swapping a model per frame (an OGI-state crate)
	// grew the scene by a whole model every frame until play crawled. Deferred through
	// pendingDestroys like Entity.Destroy, because this runs inside script callbacks.
	// Other children (script-attached entities) are only detached, as before.
	if (const auto* h = w.TryGet<aether::HierarchyComponent>(aether::Entity{id}))
	{
		const std::vector<aether::Entity> stale = h->children;
		for (const aether::Entity s: stale)
		{
			aether::ecs::DetachFromParent(w, s);
			const auto* source = w.TryGet<aether::MeshSourceComponent>(s);
			if (source != nullptr && source->kind == aether::MeshSourceComponent::Kind::Model)
			{
				std::erase(ctx.sceneEntities, s);
				ctx.pendingDestroys.push_back(s);
			}
		}
	}

	std::vector<aether::Entity> meshEntities = ctx.assets->SpawnModel(*modelPtr, id);
	for (std::size_t i = 0; i < meshEntities.size(); ++i)
	{
		const aether::Entity meshEntity = meshEntities[i];
		if (auto* tc = w.TryGet<aether::TransformComponent>(meshEntity))
		{
			tc->localToWorld = xform * tc->localToWorld;
		}
		w.EmplaceOrReplace<aether::NameComponent>(meshEntity, aether::NameComponent{.name = stem + " mesh"});
		w.EmplaceOrReplace<aether::MeshSourceComponent>(meshEntity, aether::MeshSourceComponent{.kind = aether::MeshSourceComponent::Kind::Model, .path = path, .primitiveIndex = static_cast<std::uint32_t>(i)});
		ctx.sceneEntities.push_back(meshEntity);
	}
	});
}

AE_SCRIPT_API std::uint32_t aether_create_mesh(const char* typeC)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto& ctx = ActiveContext();
	if (ctx.primitives == nullptr)
	{
		return 0;
	}

	aether::PrimitiveMesh primType{};
	const char* displayName = nullptr;
	const std::string_view sv(typeC != nullptr ? typeC : "");
	if (sv == "cube")
	{
		primType = aether::PrimitiveMesh::Cube;
		displayName = "Cube";
	}
	else if (sv == "sphere")
	{
		primType = aether::PrimitiveMesh::Sphere;
		displayName = "Sphere";
	}
	else if (sv == "plane")
	{
		primType = aether::PrimitiveMesh::Plane;
		displayName = "Plane";
	}
	else if (sv == "quad")
	{
		primType = aether::PrimitiveMesh::Quad;
		displayName = "Quad";
	}
	else if (sv == "triangle")
	{
		primType = aether::PrimitiveMesh::Triangle;
		displayName = "Triangle";
	}
	else
	{
		return 0;
	}

	if (!ctx.defaultMaterialInitialized)
	{
		ctx.defaultMaterial = {};
		ctx.defaultMaterial.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
		ctx.defaultMaterial.roughnessFactor = 0.6f;
		ctx.defaultMaterial.metallicFactor = 0.0f;
		ctx.defaultMaterial.modulateVertexColor = true;
		ctx.defaultMaterial.doubleSided = true;
		ctx.defaultMaterialInitialized = true;
	}

	SceneContext::CachedMesh entry;
	entry.mesh = &ctx.primitives->Get(primType);
	entry.materialAsset = ctx.defaultMaterial;
	entry.displayName = displayName;
	entry.kindName = std::string(sv);
	ctx.meshCache.push_back(std::move(entry));
	return static_cast<std::uint32_t>(ctx.meshCache.size() - 1);
	});
}

AE_SCRIPT_API void aether_add_mesh(std::uint32_t entityId, std::uint32_t meshHandle)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	auto& w = ActiveWorld();
	if (!EntityAlive(entityId) || meshHandle >= ctx.meshCache.size())
	{
		return;
	}
	const auto& entry = ctx.meshCache[meshHandle];
	if (entry.mesh == nullptr || ctx.assets == nullptr)
	{
		return;
	}

	const aether::Entity e{entityId};
	w.EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = entry.mesh});
	w.EmplaceOrReplace<aether::MeshSourceComponent>(e, aether::MeshSourceComponent{.kind = aether::MeshSourceComponent::Kind::Primitive, .path = entry.kindName, .primitiveIndex = 0});
	aether::MaterialSystem::AssignMaterial(w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), entry.materialAsset);

	const auto* nc = w.TryGet<aether::NameComponent>(e);
	if (nc == nullptr || nc->name.empty() || nc->name == "Entity")
	{
		w.EmplaceOrReplace<aether::NameComponent>(e, aether::NameComponent{.name = entry.displayName});
	}
	});
}

AE_SCRIPT_API void aether_set_material(std::uint32_t id, Vec3 color, float metallic, float roughness)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	auto& w = ActiveWorld();
	if (!EntityAlive(id) || ctx.assets == nullptr)
	{
		return;
	}
	aether::MaterialAsset asset;
	asset.baseColorFactor = glm::vec4(ToGlm(color), 1.0f);
	asset.metallicFactor = metallic;
	asset.roughnessFactor = roughness;
	const aether::Entity e{id};
	w.EmplaceOrReplace<aether::MaterialInstanceComponent>(e, aether::MaterialInstanceComponent{asset});
	aether::MaterialSystem::AssignMaterial(w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), asset);
	});
}

AE_SCRIPT_API void aether_set_material_color(std::uint32_t id, Vec3 color)
{ SafeExport([&] -> void { aether_set_material(id, color, 0.0f, 0.6f); }); }

AE_SCRIPT_API std::uint32_t aether_make_material(Vec3 color, float metallic, float roughness)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto& ctx = ActiveContext();
	if (ctx.assets == nullptr)
	{
		return aether::MaterialAuthoring::kInvalidId;
	}
	aether::MaterialAsset asset;
	asset.baseColorFactor = glm::vec4(ToGlm(color), 1.0f);
	asset.metallicFactor = metallic;
	asset.roughnessFactor = roughness;
	return ctx.assets->GetMaterialAuthoring().Create(asset);
	});
}

AE_SCRIPT_API void aether_bind_material(std::uint32_t entityId, std::uint32_t materialId)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().Bind(ActiveWorld(), aether::Entity{entityId}, materialId);
	}
	});
}

AE_SCRIPT_API void aether_material_set_color(std::uint32_t materialId, Vec3 color)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetBaseColor(ActiveWorld(), materialId, ToGlm(color));
	}
	});
}

AE_SCRIPT_API void aether_material_set_metallic(std::uint32_t materialId, float value)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetMetallic(ActiveWorld(), materialId, value);
	}
	});
}

AE_SCRIPT_API void aether_material_set_roughness(std::uint32_t materialId, float value)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetRoughness(ActiveWorld(), materialId, value);
	}
	});
}

AE_SCRIPT_API void aether_material_set_emissive(std::uint32_t materialId, Vec3 color)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetEmissive(ActiveWorld(), materialId, ToGlm(color));
	}
	});
}

AE_SCRIPT_API void aether_entity_material_set_color(std::uint32_t entityId, Vec3 color)
{
	SafeExport([&] -> void
	{
	// EntityAlive: every other export in this file already guards a plain entity id
	// this way (see aether_load_model's own comment above) - these four were the one
	// gap, and MaterialSystem's Set* calls straight into entt registry lookups/emplace
	// on whatever entt::entity World::ToEntt derives, which is undefined behaviour on a
	// destroyed id, not a safe no-op.
	if (!EntityAlive(entityId))
	{
		return;
	}
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetBaseColor(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), ToGlm(color));
	}
	});
}

AE_SCRIPT_API void aether_entity_material_set_metallic(std::uint32_t entityId, float value)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(entityId))
	{
		return;
	}
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetMetallic(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), value);
	}
	});
}

AE_SCRIPT_API void aether_entity_material_set_roughness(std::uint32_t entityId, float value)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(entityId))
	{
		return;
	}
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetRoughness(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), value);
	}
	});
}

AE_SCRIPT_API void aether_entity_material_set_emissive(std::uint32_t entityId, Vec3 color)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(entityId))
	{
		return;
	}
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetEmissive(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), ToGlm(color));
	}
	});
}

AE_SCRIPT_API Vec3 aether_entity_material_get_emissive(std::uint32_t entityId)
{
	return SafeExport([&] -> Vec3
	{
	// Same alive guard as the setter above, and for the same reason - reading a dead
	// id's material is exactly as unsafe as writing one, not a query that happens to be
	// harmless because it does not mutate anything.
	if (!EntityAlive(entityId))
	{
		return Vec3{};
	}
	auto& ctx = ActiveContext();
	if (ctx.assets == nullptr)
	{
		return Vec3{};
	}
	return FromGlm(aether::MaterialSystem::GetEmissive(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry()));
	});
}

AE_SCRIPT_API void aether_set_material_texture(std::uint32_t entityId, const char* path)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	if (ctx.assets == nullptr)
	{
		return;
	}
	auto& texReg = ctx.assets->GetTextureRegistry();
	const aether::TextureHandle tex = texReg.Acquire(path != nullptr ? path : "");
	aether::MaterialSystem::SetAlbedoTexture(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), tex);
	texReg.Release(tex);
	});
}

// Section payload is owned by the mesh buffer and retired with it. No independent
// GPU lifetime, descriptors, ECS access on the render thread, or per-frame uploads.
AE_SCRIPT_API int aether_set_procedural_section(std::uint32_t id, Vec3 origin, float spacing,
        int cells, int height, const float* density, int count, const float* basalt)
{
	return SafeExport([&]() -> int
	{
		if (cells < 1 || cells > 64 || height < 1 || height > 1024 ||
		    !std::isfinite(spacing) || spacing <= 0 || !std::isfinite(spacing * height) || density == nullptr || basalt == nullptr ||
		    !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) || origin.y != 0 ||
		    count != (cells + 1) * (cells + 1) * (height + 1))
		{
			return 0;
		}
		for (int i = 0; i < count; ++i)
		{
			if (!std::isfinite(density[i])) { return 0; }
		}
		for (int i = 0; i < 4; ++i)
		{
			if (!std::isfinite(basalt[i])) { return 0; }
		}
		const float span = cells * spacing;
		const Vec3 positions[] = {{origin.x, 0, origin.z}, {origin.x, 0, origin.z + span},
		                         {origin.x + span, 0, origin.z + span}, {origin.x + span, 0, origin.z}};
		const Vec3 normals[] = {{0,1,0},{0,1,0},{0,1,0},{0,1,0}};
		const Vec3 colors[] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0}};
		const Vec2 uv[] = {{basalt[0],2},{basalt[1],2},{basalt[2],2},{basalt[3],2}};
		const std::uint32_t indices[] = {0,1,2,0,2,3};
		const float header[] = {origin.x,origin.y,origin.z,spacing,float(cells+1),float(height+1),float(cells+1),0};
		std::vector<std::byte> payload(sizeof(header) + sizeof(float) * count);
		std::memcpy(payload.data(),header,sizeof(header));
		std::memcpy(payload.data()+sizeof(header),density,sizeof(float)*count);
		return SetProceduralMesh(id,positions,normals,colors,4,indices,6,0,uv,payload,height*spacing);
	});
}

AE_SCRIPT_API int aether_set_procedural_horizon(std::uint32_t id, const Vec3* positions,
        const Vec3* normals, const Vec3* colors, int vertexCount,
        const std::uint32_t* indices, int indexCount, const Vec2* texcoords)
{
	return SetProceduralMesh(id,positions,normals,colors,vertexCount,indices,indexCount,0,texcoords,{},0.0f,true);
}
