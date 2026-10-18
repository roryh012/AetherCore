#include <doctest/doctest.h>

#include "utils/FramePacer.hpp"

using namespace aether;

TEST_CASE("Frame pacer waits for its first scheduled deadline")
{
	FramePacer pacer;
	pacer.SetTargetFps(100.0f);
	CHECK(pacer.GetTargetFps() == doctest::Approx(100.0f));
	const auto start = FramePacer::Clock::now();
	pacer.Wait();
	pacer.Wait();
	CHECK(FramePacer::Clock::now() - start >= std::chrono::milliseconds(10));
}

TEST_CASE("Frame pacer can disable and restart its cadence")
{
	FramePacer pacer;
	pacer.SetTargetFps(100.0f);
	pacer.Wait();
	pacer.SetTargetFps(0.0f);
	CHECK(pacer.GetTargetFps() == 0.0f);
	pacer.Wait();
	pacer.SetTargetFps(-1.0f);
	CHECK(pacer.GetTargetFps() == 0.0f);
	pacer.Wait();
	pacer.SetTargetFps(200.0f);
	const auto start = FramePacer::Clock::now();
	pacer.Wait();
	pacer.Wait();
	CHECK(FramePacer::Clock::now() - start >= std::chrono::milliseconds(5));
}
