#include "utils/FramePacer.hpp"

#include <thread>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#endif

namespace aether
{
	void FramePacer::SleepUntil(const TimePoint deadline)
	{
#ifdef _WIN32
		struct Timer
		{
			HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
				TIMER_MODIFY_STATE | SYNCHRONIZE);
			~Timer()
			{
				if (handle)
				{
					CloseHandle(handle);
				}
			}
		};
		// Ordinary Windows sleeps can overshoot by a scheduler tick; keep this timer local to the pacing thread.
		thread_local Timer timer;
		const auto remaining = deadline - Clock::now();
		if (remaining <= Duration::zero())
		{
			return;
		}
		if (timer.handle)
		{
			using TimerTick = std::chrono::duration<long long, std::ratio<1, 10000000>>;
			LARGE_INTEGER due;
			due.QuadPart = -std::chrono::ceil<TimerTick>(remaining).count();
			if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)
				&& WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0)
			{
				return;
			}
		}
#endif
		std::this_thread::sleep_until(deadline);
	}
} // namespace aether
