#include <doctest/doctest.h>
#include <future>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

TEST_CASE("Procedural terrain collider replacement changes raycasts without asset files")
{
	aether::World world;
	world.SetSceneKind(aether::SceneKind::Scene3D);
	world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
	auto system = std::make_unique<aether::PhysicsSystem>();
	auto* physics = system.get();
	world.RegisterSystem(std::move(system));
	const auto entity = world.Create();
	world.Emplace<aether::TransformComponent>(entity);
	auto replace = [&](float height)
	{
		physics->RemoveBody(world, entity);
		aether::ColliderComponent collider{};
		collider.shape = aether::PhysicsShapeType::Mesh;
		collider.layer = aether::PhysicsLayer::NonMoving;
		collider.meshVertices = {{-2,height,-2},{-2,height,2},{2,height,2},{2,height,-2}};
		collider.meshIndices = {0,1,2,0,2,3};
		world.EmplaceOrReplace<aether::ColliderComponent>(entity, std::move(collider));
		world.EmplaceOrReplace<aether::RigidBodyComponent>(entity, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Static});
		physics->FlushPendingOnly(world);
	};
	replace(0);
	auto initial = physics->CastRay({0,10,0},{0,-1,0},20);
	REQUIRE(initial.hit);
	CHECK(initial.position.y == doctest::Approx(0));
	replace(3);
	auto edited = physics->CastRay({0,10,0},{0,-1,0},20);
	REQUIRE(edited.hit);
	CHECK(edited.position.y == doctest::Approx(3));
	physics->RemoveBody(world, entity);
	CHECK_FALSE(physics->CastRay({0,10,0},{0,-1,0},20).hit);
}

TEST_CASE("Prepared procedural colliders cook on a worker and replace only at an idle physics boundary")
{
	aether::World world;
	world.SetSceneKind(aether::SceneKind::Scene3D);
	auto system = std::make_unique<aether::PhysicsSystem>();
	auto* physics = system.get();
	world.RegisterSystem(std::move(system));
	const auto entity = world.Create();
	world.Emplace<aether::TransformComponent>(entity);
	std::vector<glm::vec3> vertices{{-2,0,-2},{-2,0,2},{2,0,2},{2,0,-2}};
	std::vector<std::uint32_t> indices{0,1,2,0,2,3};
	auto worker = std::async(std::launch::async, [&] { return aether::PhysicsSystem::PrepareTriangleCollider(vertices, indices); });
	auto shape = worker.get();
	REQUIRE(shape);
	CHECK_FALSE(physics->CastRay({0,10,0},{0,-1,0},20).hit);
	REQUIRE(physics->TryCommitTriangleCollider(world, entity, shape));
	CHECK(physics->CastRay({0,10,0},{0,-1,0},20).position.y == doctest::Approx(0));
	for (auto& vertex : vertices) { vertex.y = 3; }
	auto replacement = aether::PhysicsSystem::PrepareTriangleCollider(vertices, indices);
	REQUIRE(replacement);
	CHECK(physics->CastRay({0,10,0},{0,-1,0},20).position.y == doctest::Approx(0));
	REQUIRE(physics->TryCommitTriangleCollider(world, entity, replacement));
	CHECK(physics->CastRay({0,10,0},{0,-1,0},20).position.y == doctest::Approx(3));
	indices[0] = 999;
	CHECK_FALSE(aether::PhysicsSystem::PrepareTriangleCollider(vertices, indices));
	CHECK(physics->CastRay({0,10,0},{0,-1,0},20).position.y == doctest::Approx(3));
	REQUIRE(physics->TryCommitTriangleCollider(world, entity, {}));
	CHECK_FALSE(physics->CastRay({0,10,0},{0,-1,0},20).hit);
	world.Emplace<aether::CharacterControllerComponent>(entity);
	bool failed = false;
	CHECK_FALSE(physics->TryCommitTriangleCollider(world, entity, shape, &failed));
	CHECK(failed);
}
