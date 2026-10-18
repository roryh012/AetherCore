#include <doctest/doctest.h>

#include "scripting/ProceduralStream.hpp"
#include "scripting/SceneContext.hpp"
#include "assets/AssetManager.hpp"
#include "IEngineRuntime.hpp"
#include "scene/World.hpp"

namespace
{
	struct StreamingRuntime : aether::IEngineRuntime
	{
		std::vector<std::shared_ptr<void>> retained;

		void RunExclusive(aether::QuiesceMode, std::function<void()>) override
		{
			FAIL("Streaming queue/status/cancel must not drain rendering");
		}

		void RetireAfterRendering(std::shared_ptr<void>) override
		{
			FAIL("Streaming must use queued retirement");
		}

		void RetireQueuedRendering(std::shared_ptr<void> owner) override
		{
			retained.push_back(std::move(owner));
		}

		double RealElapsedSeconds() const override
		{
			return 0;
		}
	};
} // namespace

TEST_CASE("Procedural stream supersession and cancellation remain charged until resources retire")
{
	StreamingRuntime runtime;
	aether::AssetManager assets;
	aether::World world;
	aether::app::scripting::SceneContext ctx;
	ctx.world = &world;
	ctx.assets = &assets;
	ctx.engineRuntime = &runtime;
	const auto entity = world.Create();
	auto& stream = ctx.proceduralStream;
	aether::app::scripting::ProceduralStream::Data data;
	data.vertices.resize(3);
	data.indices = {0, 1, 2};
	const auto first = stream.Queue(ctx, entity.id, data, 0);
	REQUIRE(first != 0);
	CHECK(stream.GetStats().bytes > 0);
	const auto next = stream.Queue(ctx, entity.id, data, 10);
	REQUIRE(next > first);
	CHECK(stream.Poll(first).state == 5);
	CHECK(stream.Poll(next).state == 0);
	CHECK_FALSE(stream.Commit(ctx, first));
	CHECK_FALSE(stream.Commit(ctx, next));
	CHECK(stream.GetStats().jobs == 2);
	CHECK(stream.Cancel(ctx, next));
	CHECK_FALSE(stream.Cancel(ctx, next));
	CHECK(stream.GetStats().jobs == 2);
	runtime.retained.clear();
	CHECK(stream.GetStats().jobs == 0);
	CHECK(stream.GetStats().bytes == 0);
	CHECK_FALSE(stream.CanQueue(stream.kMaxBytes + 1));
	CHECK(stream.GetStats().rejected == 1);
}

TEST_CASE("Cancelling queued jobs cannot bypass streaming backpressure")
{
	StreamingRuntime runtime;
	aether::AssetManager assets;
	aether::World world;
	aether::app::scripting::SceneContext ctx;
	ctx.world = &world;
	ctx.assets = &assets;
	ctx.engineRuntime = &runtime;
	const auto entity = world.Create();
	auto& stream = ctx.proceduralStream;
	std::uint64_t latest = 0;
	for (std::size_t i = 0; i < stream.kMaxJobs; ++i)
	{
		latest = stream.Queue(ctx, entity.id, {}, 0);
		REQUIRE(latest != 0);
	}
	CHECK(stream.Queue(ctx, entity.id, {}, 0) == 0);
	CHECK(stream.Poll(latest).state == 0);
	CHECK(stream.GetStats().jobs == stream.kMaxJobs);
	stream.Clear(ctx);
	runtime.retained.clear();
	CHECK(stream.GetStats().jobs == 0);
}
