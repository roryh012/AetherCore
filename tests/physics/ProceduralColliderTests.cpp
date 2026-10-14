#include <doctest/doctest.h>

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
