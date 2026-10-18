#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <future>
#include <chrono>

#include "utils/FrameTimeline.hpp"
#include "IEngineRuntime.hpp"
#include "rendering/RenderThread.hpp"

using namespace aether;

namespace
{
	FrameTiming MakeGameFrame(const std::uint64_t index, const float wallMs)
	{
		FrameTiming timing;
		timing.frameIndex = index;
		timing.wallMs = wallMs;
		timing.simDtMs = wallMs > 33.33f ? 33.33f : wallMs;
		timing.pacerWaitMs = 0.0f;
		timing.inFlightWaitMs = wallMs * 0.5f;
		timing.gameWorkMs = wallMs * 0.5f;
		return timing;
	}
} // namespace

TEST_CASE("A recorded game frame reads back with every phase intact") {
    FrameTimeline timeline;
    timeline.RecordGameFrame(MakeGameFrame(7, 16.0f));

    std::array<FrameTiming, 8> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 1);
    CHECK(out[0].frameIndex == 7);
    CHECK(out[0].wallMs == doctest::Approx(16.0f));
    CHECK(out[0].inFlightWaitMs == doctest::Approx(8.0f));
    CHECK(out[0].gameWorkMs == doctest::Approx(8.0f));
    CHECK_FALSE(out[0].renderComplete);
}

TEST_CASE("The render thread completes a frame the game thread already published") {
    FrameTimeline timeline;
    timeline.RecordGameFrame(MakeGameFrame(3, 16.0f));
    timeline.RecordGameFrame(MakeGameFrame(4, 16.0f));
    // The render thread trails, so it lands on an older, already-published record.
    timeline.RecordRenderFrame(3, 1.25f, 12.5f);

    std::array<FrameTiming, 8> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 2);
    CHECK(out[0].frameIndex == 3);
    CHECK(out[0].renderExecMs == doctest::Approx(1.25f));
    CHECK(out[0].presentWaitMs == doctest::Approx(12.5f));
    CHECK(out[0].renderComplete);
    // The newer frame is still awaiting its render-thread half.
    CHECK(out[1].frameIndex == 4);
    CHECK_FALSE(out[1].renderComplete);
}

TEST_CASE("Snapshot returns the newest frames in order after the ring wraps") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < FrameTimeline::kCapacity + 10; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }

    std::vector<FrameTiming> out(FrameTimeline::kCapacity);
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == FrameTimeline::kCapacity);
    // Oldest surviving record first, newest last, contiguous.
    CHECK(out.front().frameIndex == 10);
    CHECK(out.back().frameIndex == FrameTimeline::kCapacity + 9);
    for (std::size_t i = 1; i < count; ++i)
    {
        CHECK(out[i].frameIndex == out[i - 1].frameIndex + 1);
    }
    CHECK(timeline.FrameCount() == FrameTimeline::kCapacity + 10);
}

TEST_CASE("A render completion for an evicted frame is dropped, not misapplied") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < FrameTimeline::kCapacity + 10; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }
    // Frame 0 was overwritten long ago; its slot now holds a much newer frame and
    // must not be corrupted by a late completion.
    timeline.RecordRenderFrame(0, 99.0f, 99.0f);

    std::vector<FrameTiming> out(FrameTimeline::kCapacity);
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == FrameTimeline::kCapacity);
    for (std::size_t i = 0; i < count; ++i)
    {
        CHECK(out[i].renderExecMs == doctest::Approx(0.0f));
    }
}

TEST_CASE("Snapshot into a buffer smaller than the history returns the newest that fit") {
    FrameTimeline timeline;
    for (std::uint64_t i = 0; i < 50; ++i)
    {
        timeline.RecordGameFrame(MakeGameFrame(i, 16.0f));
    }

    std::array<FrameTiming, 5> out{};
    const std::size_t count = timeline.Snapshot(out);

    REQUIRE(count == 5);
    CHECK(out.front().frameIndex == 45);
    CHECK(out.back().frameIndex == 49);
}

// GPU completion is supplied by the backend; test ownership independently of a GPU.
TEST_CASE("Render retirement retains resources until their submission completes")
{
	aether::RenderRetirementQueue queue;
	auto a=std::make_shared<int>(1), b=std::make_shared<int>(2);
	std::weak_ptr<int> oldA=a, oldB=b;
	queue.Retire(10,std::move(a));queue.Retire(11,std::move(b));
	queue.Collect([](std::uint64_t) { return false; });
	CHECK_FALSE(oldA.expired());CHECK_FALSE(oldB.expired());
	queue.Collect([](std::uint64_t ticket) { return ticket<=10; });
	CHECK(oldA.expired());CHECK_FALSE(oldB.expired());CHECK_FALSE(queue.Empty());
	queue.Collect([](std::uint64_t) { return true; });
	CHECK(oldB.expired());CHECK(queue.Empty());
}
TEST_CASE("Render retirement releases all owners of a completed batch and clears on shutdown")
{
	aether::RenderRetirementQueue queue;
	auto shared=std::make_shared<int>(1);std::weak_ptr<int> weak=shared;
	queue.Retire(4,shared);queue.Retire(4,std::move(shared));
	queue.Collect([](std::uint64_t ticket) { return ticket<4; });
	CHECK_FALSE(weak.expired());
	queue.Clear();CHECK(weak.expired());CHECK(queue.Empty());
	queue.Clear(); // shutdown after an already-drained queue
}


TEST_CASE("Render shutdown wakes frame-completion waiters racing with shutdown")
{
	using namespace std::chrono_literals;
	for (int iteration = 0; iteration < 200; ++iteration)
	{
		RenderThread renderer;
		std::promise<void> started;
		auto entered = started.get_future();
		auto waiter = std::async(std::launch::async, [&]
		        {
			        started.set_value();
			        renderer.WaitUntilFrameCompleted(0);
		        });
		entered.wait();
		renderer.Stop();
		const bool woke = waiter.wait_for(1s) == std::future_status::ready;
		// Repeat the notification on failure so the regression reports instead of
		// leaving the async destructor permanently blocked.
		if (!woke)
		{
			renderer.Stop();
		}
		CHECK(woke);
		waiter.get();
	}
}

TEST_CASE("Producer phase timings survive ring wrap and render completion")
{
	FrameTimeline timeline;
	for (std::uint64_t frame = 0; frame <= FrameTimeline::kCapacity; ++frame)
	{
		FrameTiming timing;
		timing.frameIndex = frame;
		timing.frameBeginMs = 0.1f;
		timing.eventPumpMs = 0.2f;
		timing.updateMs = 0.3f;
		timing.editorUiMs = 0.4f;
		timing.uiBeginMs = 0.1f;
		timing.uiBuildMs = 0.2f;
		timing.uiFinishMs = 0.1f;
		timing.extractionMs = 0.5f;
		timing.sceneUiMs = 0.6f;
		timing.submitMs = 0.7f;
		timeline.RecordGameFrame(timing);
	}
	timeline.RecordRenderFrame(FrameTimeline::kCapacity, 3.0f, 4.0f, 0.75f, 1.25f, 2.0f);
	std::array<FrameTiming, 1> out{};
	REQUIRE(timeline.Snapshot(out) == 1);
	CHECK(out[0].frameBeginMs == doctest::Approx(0.1f));
	CHECK(out[0].eventPumpMs == doctest::Approx(0.2f));
	CHECK(out[0].updateMs == doctest::Approx(0.3f));
	CHECK(out[0].editorUiMs == doctest::Approx(0.4f));
	CHECK(out[0].uiBeginMs == doctest::Approx(0.1f));
	CHECK(out[0].uiBuildMs == doctest::Approx(0.2f));
	CHECK(out[0].uiFinishMs == doctest::Approx(0.1f));
	CHECK(out[0].extractionMs == doctest::Approx(0.5f));
	CHECK(out[0].sceneUiMs == doctest::Approx(0.6f));
	CHECK(out[0].submitMs == doctest::Approx(0.7f));
	CHECK(out[0].renderExecMs == doctest::Approx(3.0f));
	CHECK(out[0].presentWaitMs == doctest::Approx(4.0f));
	CHECK(out[0].swapchainFenceMs == doctest::Approx(0.75f));
	CHECK(out[0].swapchainAcquireMs == doctest::Approx(1.25f));
	CHECK(out[0].nativePresentMs == doctest::Approx(2.0f));
}

TEST_CASE("Reused frame slots do not retain completed swapchain waits")
{
	FrameTimeline timeline;
	FrameTiming timing;
	timeline.RecordGameFrame(timing);
	timeline.RecordRenderFrame(0, 1.0f, 6.0f, 1.0f, 2.0f, 3.0f);
	for (std::uint64_t frame = 1; frame <= FrameTimeline::kCapacity; ++frame)
	{
		timing.frameIndex = frame;
		timeline.RecordGameFrame(timing);
	}
	std::array<FrameTiming, 1> out{};
	REQUIRE(timeline.Snapshot(out) == 1);
	CHECK_FALSE(out[0].renderComplete);
	CHECK(out[0].swapchainFenceMs == 0.0f);
	CHECK(out[0].swapchainAcquireMs == 0.0f);
	CHECK(out[0].nativePresentMs == 0.0f);
}
