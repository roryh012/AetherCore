#include "ProceduralStream.hpp"

#include <chrono>
#include <algorithm>

#include "scripting/SceneContext.hpp"
#include "assets/AssetManager.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "IEngineRuntime.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::app::scripting
{
	using Clock = std::chrono::steady_clock;

	static double Milliseconds(Clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	struct ProceduralStream::Job
	{
		std::shared_ptr<Budget> budget;

		~Job()
		{
			if (worker.valid())
			{
				worker.wait();
			}
			for (auto handle: staging)
			{
				if (handle.IsValid())
				{
					gpu::ResourceRegistry::Destroy(handle);
				}
			}
			if (budget)
			{
				--budget->jobs;
				budget->bytes -= bytes;
			}
		}

		std::uint32_t entity;
		int priority;
		std::uint64_t bytes;
		Data data;
		std::atomic<State> state{State::Queued};
		std::atomic<bool> cancelled{false};
		std::future<void> worker;
		std::shared_ptr<const PreparedTriangleCollider> collider;
		std::unique_ptr<Mesh> mesh;
		std::vector<gpu::BufferHandle> staging;
		std::uint64_t upload = 0;
		bool failed = false;
		std::atomic<bool> visualReady{false};
		std::atomic<bool> collisionReady{false};
		double preparationMs = 0, allocationCopyMs = 0, uploadMs = 0, colliderCommitMs = 0, commitMs = 0;
		Clock::time_point submitted;
	};

	ProceduralStream::Stats ProceduralStream::GetStats() const
	{
		return {m_budget->jobs.load(), m_budget->bytes.load(), m_rejected};
	}

	bool ProceduralStream::CanQueue(std::uint64_t bytes)
	{
		const auto stats = GetStats();
		if (stats.jobs >= kMaxJobs || bytes > kMaxBytes || stats.bytes > kMaxBytes - bytes)
		{
			++m_rejected;
			return false;
		}
		return true;
	}

	std::uint64_t ProceduralStream::Queue(SceneContext& ctx, std::uint32_t entity, Data data, int priority)
	{
		// Count source, destination and staging until the ticket is explicitly released.
		const auto bytes = 3 * (data.vertices.size() * sizeof(Mesh::Vertex) + data.indices.size() * sizeof(std::uint32_t) + data.section.size());
		if (!ctx.assets || !ctx.engineRuntime || (data.collision && !ctx.physics) || !CanQueue(bytes))
		{
			return 0;
		}
		Invalidate(ctx, entity);
		auto job = std::make_shared<Job>();
		job->budget = m_budget;
		++m_budget->jobs;
		m_budget->bytes += bytes;
		job->entity = entity;
		job->priority = priority;
		job->bytes = bytes;
		job->data = std::move(data);
		const auto ticket = m_next++;
		m_jobs.emplace(ticket, job);
		m_current[entity] = ticket;
		return ticket;
	}

	ProceduralStream::Status ProceduralStream::Poll(std::uint64_t ticket) const
	{
		const auto found = m_jobs.find(ticket);
		if (found == m_jobs.end())
		{
			return {.state = static_cast<int>(State::Cancelled)};
		}
		const auto& job = *found->second;
		const auto state = job.state.load(std::memory_order_acquire);
		Status result{.state = static_cast<int>(state), .bytes = job.bytes};
		result.inputCopyMs = job.data.inputCopyMs;
		result.visualReady = job.visualReady.load(std::memory_order_acquire);
		result.collisionReady = job.collisionReady.load(std::memory_order_acquire);
		if (result.collisionReady || state == State::Failed)
		{
			result.preparationMs = job.preparationMs;
		}
		if (state == State::Uploading || state == State::Ready || state == State::Committed || state == State::Failed)
		{
			result.allocationCopyMs = job.allocationCopyMs;
		}
		if (state == State::Ready || state == State::Committed || state == State::Failed)
		{
			result.uploadMs = job.uploadMs;
			result.colliderCommitMs = job.colliderCommitMs;
			result.commitMs = job.commitMs;
		}
		return result;
	}

	void ProceduralStream::Tick(SceneContext& ctx)
	{
		std::size_t active = m_budget->workers.load();
		std::vector<std::uint64_t> dead;
		std::vector<std::shared_ptr<Job>> queued;
		for (const auto& [ticket, job]: m_jobs)
		{
			if (!ctx.world->GetRegistry().valid(World::ToEntt(Entity{job->entity})))
			{
				dead.push_back(ticket);
				continue;
			}
			const auto state = job->state.load();

			if (state == State::Queued && !job->cancelled)
			{
				queued.push_back(job);
			}
		}
		for (auto ticket: dead)
		{
			Cancel(ctx, ticket);
		}
		std::stable_sort(queued.begin(), queued.end(), [](const auto& a, const auto& b) { return a->priority > b->priority; });
		for (auto& job: queued)
		{
			if (active >= 2)
			{
				break;
			}
			++active;
			++m_budget->workers;
			job->state = State::Preparing;
			try
			{
				job->worker = std::async(std::launch::async,
				        [job = job.get()]
				        {
					        const auto start = Clock::now();
					        try
					        {
						        if (job->data.collision && !job->data.vertices.empty() && !job->cancelled)
						        {
							        std::vector<glm::vec3> positions;
							        positions.reserve(job->data.vertices.size());
							        for (const auto& vertex: job->data.vertices)
							        {
								        positions.push_back(vertex.position);
							        }
							        job->collider = PhysicsSystem::PrepareTriangleCollider(positions, job->data.indices);
							        job->failed = !job->collider;
						        }
					        }
					        catch (...)
					        {
						        job->failed = true;
					        }
					        job->preparationMs = Milliseconds(start);
					        job->collisionReady.store(!job->data.collision || job->data.vertices.empty() || static_cast<bool>(job->collider), std::memory_order_release);
					        --job->budget->workers;
				        });
			}
			catch (...)
			{
				--m_budget->workers;
				job->state = State::Failed;
				continue;
			}
			auto* uploads = &ctx.assets->GetUploadContext();
			const bool accepted = ctx.engineRuntime->QueueGpuWork(
			        [job, uploads]
			        {
				        if (job->state == State::Preparing)
				        {
					        if (job->worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					        {
						        return false;
					        }
					        job->worker.get();
					        const auto start = Clock::now();
					        if (!job->cancelled && !job->failed && job->data.visual && !job->data.vertices.empty())
					        {
						        try
						        {
							        job->mesh = std::make_unique<Mesh>();
							        *job->mesh = Mesh::CreateAsync(*uploads, job->data.vertices, job->data.indices, job->data.section, job->staging, job->upload, job->data.sectionTop);
							        job->failed = !job->mesh->IsValid() || !job->mesh->IsIndexed() || job->upload == 0;
						        }
						        catch (...)
						        {
							        job->failed = true;
						        }
					        }
					        job->allocationCopyMs = Milliseconds(start);
					        job->submitted = Clock::now();
					        job->state = State::Uploading;
				        }
				        if (!uploads->IsComplete(job->upload))
				        {
					        return false;
				        }
				        for (auto handle: job->staging)
				        {
					        if (handle.IsValid())
					        {
						        gpu::ResourceRegistry::Destroy(handle);
					        }
				        }
				        job->staging.clear();
				        job->uploadMs = Milliseconds(job->submitted);
				        job->visualReady = !job->failed;
				        if (job->cancelled || job->failed)
				        {
					        job->mesh.reset();
				        }
				        job->state.store(job->cancelled ? State::Cancelled : job->failed ? State::Failed : State::Ready, std::memory_order_release);
				        return true;
			        },
			        job->priority);
			if (!accepted)
			{
				job->worker.wait();
				job->state = State::Failed;
			}
		}
	}

	bool ProceduralStream::Commit(SceneContext& ctx, std::uint64_t ticket)
	{
		const auto found = m_jobs.find(ticket);
		if (found == m_jobs.end())
		{
			return false;
		}
		auto& job = *found->second;
		const Entity entity{job.entity};
		if (job.state.load(std::memory_order_acquire) != State::Ready || job.cancelled || m_current[job.entity] != ticket || !ctx.world->GetRegistry().valid(World::ToEntt(entity)))
		{
			return false;
		}
		const auto start = Clock::now();
		if (job.upload && !ctx.assets->GetUploadContext().PublishCompleted(job.upload)) { return false; }
		bool failed = false;
		if (job.data.collision && !ctx.physics->TryCommitTriangleCollider(*ctx.world, entity, job.collider, &failed))
		{
			if (failed)
			{
				job.state = State::Failed;
			}
			return false;
		}
		job.colliderCommitMs = Milliseconds(start);
		if (job.data.visual)
		{
			ctx.world->Remove<MeshComponent>(entity);
			ctx.world->Remove<MeshSourceComponent>(entity);
			if (auto old = ctx.proceduralMeshes.find(job.entity); old != ctx.proceduralMeshes.end())
			{
				ctx.engineRuntime->RetireQueuedRendering(std::shared_ptr<Mesh>(std::move(old->second)));
				ctx.proceduralMeshes.erase(old);
			}
			if (job.mesh)
			{
				const auto* ptr = job.mesh.get();
				ctx.proceduralMeshes.emplace(job.entity, std::move(job.mesh));
				ctx.world->EmplaceOrReplace<MeshComponent>(entity, MeshComponent{.mesh = ptr});
			}
		}
		job.commitMs = Milliseconds(start);
		job.state = State::Committed;
		return true;
	}

	bool ProceduralStream::Cancel(SceneContext& ctx, std::uint64_t ticket)
	{
		const auto found = m_jobs.find(ticket);
		if (found == m_jobs.end())
		{
			return false;
		}
		auto job = found->second;
		job->cancelled = true;
		if (const auto current = m_current.find(job->entity); current != m_current.end() && current->second == ticket)
		{
			m_current.erase(current);
		}
		// Queued work owns its data until its render callback observes cancellation.
		ctx.engineRuntime->RetireQueuedRendering(job);
		m_jobs.erase(found);
		return true;
	}

	void ProceduralStream::Invalidate(SceneContext& ctx, std::uint32_t entity)
	{
		const auto found = m_current.find(entity);
		if (found != m_current.end())
		{
			Cancel(ctx, found->second);
		}
	}

	void ProceduralStream::Clear(SceneContext& ctx)
	{
		while (!m_jobs.empty())
		{
			Cancel(ctx, m_jobs.begin()->first);
		}
	}
} // namespace aether::app::scripting
