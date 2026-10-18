#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <cstdint>

namespace aether
{
	// How RunExclusive treats queued CPU frames and in-flight GPU resources.
	enum class QuiesceMode
	{
		Drain,
		Discard,
		// Drain CPU packets and park submission, without waiting for GPU completion.
		// Replaced GPU resources MUST be retained through RetireAfterRendering.
		DrainCpuOnly,
	};

	// Ownership fence: the caller supplies GPU completion status while submission is parked.
	class RenderRetirementQueue
	{
	public:
		void Retire(std::uint64_t ticket, std::shared_ptr<void> owner)
		{ m_pending.push_back({ticket, std::move(owner)}); }
		template<class IsComplete> void Collect(IsComplete complete)
		{ std::erase_if(m_pending, [&](const auto& item) { return complete(item.ticket); }); }
		void Clear() { m_pending.clear(); }
		[[nodiscard]] bool Empty() const { return m_pending.empty(); }
	private:
		struct Pending { std::uint64_t ticket; std::shared_ptr<void> owner; };
		std::vector<Pending> m_pending;
	};

	// Game-thread entry points for safe mutation of renderer-owned resources.
	class IEngineRuntime
	{
	public:
		virtual ~IEngineRuntime() = default;

		// Quiesce submission; the selected mode determines GPU completion/retention.
		virtual void RunExclusive(QuiesceMode mode, std::function<void()> mutation) = 0;
		// Call inside RunExclusive: retain ownership until previously submitted rendering completes.
		virtual void RetireAfterRendering(std::shared_ptr<void> resource) = 0;

		// Game-thread submission; callbacks run only on the render thread. Return true when
		// finished; false schedules a nonblocking poll next frame. Never capture the ECS.
		virtual bool QueueGpuWork(std::function<bool()>, int = 0) { return false; }
		virtual void RetireQueuedRendering(std::shared_ptr<void> resource)
		{
			RunExclusive(QuiesceMode::DrainCpuOnly, [&] { RetireAfterRendering(std::move(resource)); });
		}

		// Real (wall-clock) seconds since the frame loop started, unaffected by the play-speed /
		// pause time scale. Use this to animate UI while the game is frozen (e.g. a pause menu),
		// where the scaled game clock (Time.TotalTime) stops advancing.
		[[nodiscard]] virtual double RealElapsedSeconds() const = 0;
	};
} // namespace aether
