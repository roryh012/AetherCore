#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <unordered_map>
#include <vector>

#include "mesh/Mesh.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether::app::scripting
{
	struct SceneContext;

	// Scene-owned tickets; public calls are game-thread only, GPU work never touches ECS.
	class ProceduralStream
	{
	public:
		enum class State : int
		{
			Queued,
			Preparing,
			Uploading,
			Ready,
			Committed,
			Cancelled,
			Failed
		};

		struct Data
		{
			std::vector<Mesh::Vertex> vertices;
			std::vector<std::uint32_t> indices;
			std::vector<std::byte> section;
			double inputCopyMs = 0;
			float sectionTop = 0;
			bool visual = true;
			bool collision = false;
		};

		struct Status
		{
			int state = 0;
			int visualReady = 0;
			int collisionReady = 0;
			int reserved = 0;
			double inputCopyMs = 0;
			double preparationMs = 0;
			double allocationCopyMs = 0;
			double uploadMs = 0;
			double colliderCommitMs = 0;
			double commitMs = 0;
			std::uint64_t bytes = 0;
		};

		struct Stats
		{
			std::uint64_t jobs = 0, bytes = 0, rejected = 0;
		};

		static constexpr std::uint64_t kMaxBytes = 256ull * 1024 * 1024;
		static constexpr std::size_t kMaxJobs = 64;
		std::uint64_t Queue(SceneContext& ctx, std::uint32_t entity, Data data, int priority);
		Status Poll(std::uint64_t ticket) const;
		bool Commit(SceneContext& ctx, std::uint64_t ticket);
		bool Cancel(SceneContext& ctx, std::uint64_t ticket);
		void Invalidate(SceneContext& ctx, std::uint32_t entity);
		void Tick(SceneContext& ctx);
		void Clear(SceneContext& ctx);
		Stats GetStats() const;
		bool CanQueue(std::uint64_t bytes);

	private:
		struct Budget
		{
			std::atomic<std::uint64_t> jobs{0}, bytes{0}, workers{0};
		};

		std::shared_ptr<Budget> m_budget = std::make_shared<Budget>();
		struct Job;
		std::unordered_map<std::uint64_t, std::shared_ptr<Job>> m_jobs;
		std::unordered_map<std::uint32_t, std::uint64_t> m_current;
		std::uint64_t m_next = 1;
		std::uint64_t m_rejected = 0;
	};
} // namespace aether::app::scripting
