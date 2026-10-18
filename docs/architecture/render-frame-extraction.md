# Render-frame extraction (the Frostbite model)

**Status:** adopted 2026-07-12. Migration in progress.

## The invariant

> **The render thread never reads mutable game state. The producer (game) thread
> extracts everything the renderer needs into an immutable `RenderFramePacket`
> once per frame; the render thread consumes only that packet.**

This is the "immediate / extract-and-consume" architecture used by Frostbite and
most console engines, as opposed to Unreal's "retained proxy + render command
queue" model. We chose it because:

- The engine was already ~80% there (`RenderFramePacket` + double-buffered draw
  slots), so this *finishes* a pattern instead of introducing a new framework.
- It makes the invariant **structural, not disciplinary**: once
  `ExecuteRenderFrame` has no `World*`, the render thread *cannot* touch the ECS,
  and the whole class of "render thread reads the world while the producer
  mutates it" races is gone by construction. No `RunExclusive`-for-reads, no
  per-system gating, no "is this toggle on?" reasoning.
- It scales along axes that don't require a rewrite: parallelize extraction
  across worker threads (one extractor per subsystem), and cache expensive
  extractors incrementally where a profiler says to. Unreal's retained model pays
  for delta-updates we don't need at this scale.

Structural ECS changes (spawn / destroy / add-remove-component), which are a
*different* problem, are handled by a deferred command buffer applied at a sync
point (see "Future: WorldCommandBuffer"), NOT by the render packet.

## Where the extract happens

`AetherCore::PrepareFrame` (producer thread, called from `RunFrameLoop`) is the
extract phase. It already builds, into per-slot buffers + the packet:

- mesh + skinned draw lists (`WorldRenderer::Flush` -> `RenderQueue`)
- render-target-service queues, directional + local shadow queues
- point/spot light lists (`packet.pointLights` / `packet.spotLights`)
- camera view/proj/near (`packet.view` / `packet.proj` / ...)

`ExecuteRenderFrame` / `EndFrame` / every `RenderGraph` pass consume the packet
and the per-slot buffers. `LightingManager::PrepareForRenderGraph` and both
shadow services' `BuildFrameShadowData` already read from the packet.

## Audit: render-thread world reads (2026-07-12)

| Site | Reads world on render thread? | Status |
|------|-------------------------------|--------|
| `WorldRenderer::Flush` (mesh/shadow draw extraction) | No - runs in `PrepareFrame` (producer) | OK |
| `LightingManager::PrepareForRenderGraph` | No - reads `packet.pointLights/spotLights` | OK |
| `ShadowService::BuildFrameShadowData` | No - packet only | OK |
| RenderGraph pass execute lambdas | No | OK |
| `UiRenderer::BuildFrame` -> `ResolveCanvases` | **Was yes** | Fixed `ef13e852` (moved to producer) |
| `LocalShadowService::BuildFrameShadowData(World&)` | No - param is `(void) world;`, dead | Remove dead param |
| `PhysicsDebugRenderer::DrawPhysicsDebugShapes` | **Yes** - `world->View<Collider,PhysicsState,RigidBody>().each` in the pass (render thread), gated `s_physicsDebugShapesEnabled` off by default | Extract instances to packet |

Net: the migration is surgical, not a rewrite.

## Migration phases

1. **Drop the dead `World&`** from `LocalShadowService::BuildFrameShadowData`
   and its call site.
2. **Extract physics-debug shapes on the producer**: build a
   `std::vector<PhysicsDebugInstance{model, tint, shapeType}>` into the packet;
   the pass replays it (picking the static per-shape vertex buffer by type) with
   no world access.
3. **Remove `World&` / `SceneSubsystem::GetWorld()` from `ExecuteRenderFrame`**
   (and the `debugRenderer.SetWorld` call). The render thread now holds no world
   pointer - the invariant is compiler-enforced. Add a one-line assert/comment
   at the top of `ExecuteRenderFrame` documenting it.

## Future: WorldCommandBuffer (structural changes)

Deferred structural mutations - the Unity DOTS `EntityCommandBuffer` idea - so
scripts/systems can `spawn`/`destroy`/`add`/`remove` during a frame without
racing readers. Commands are recorded during the frame and flushed at one sync
point on the producer. This is also the clean home for the "better script entity
spawning" API. Additive; not required for the invariant above, but it closes the
structural-change race class the same way extraction closes the read race class.


## Pause/completion wakeup coordination

The render pause and frame-completion condition variables protect their predicates
with the same mutex used by the waiter. Atomic flags still support inexpensive
observations, but do not replace this protocol: a predicate store and notification
outside the mutex can land between a waiter's predicate check and sleep, leaving
both producer and renderer asleep. This occurred during repeated terrain
publication/travel (`RunExclusive` -> `WaitPaused`).

Reload requests and idle transitions use `m_reloadMutex`; the renderer checks the
reload flag and claims a packet under that mutex. Frame-completion publication
uses `m_completionMutex`. Shutdown acquires both mutexes and wakes all waiters.
These locks cover only state changes, never rendering, GPU waits or mutations.
The completion/shutdown race unit check needs no GPU. Live repeated publication
is still required to exercise renderer pause/resume with real work.


Verification (2026-10-16): RelWithDebInfo Editor/GameRuntime/EngineTests build,
GPU abstraction guard and all 1,352 unit cases passed. Both validation smokes
reached their scenes; the strict gauntlet remained red for a startup
`VK_SUBOPTIMAL_KHR` presentation warning and the enabled-layer banner classified
as an advisory. A paired smoke using the previous 1dd0c9ff executable reported
the same findings. This is not a zero-validation-findings pass. Smoke evidence
now uses a private per-run redirected console log, avoiding locked/interleaved
logs from other live editors; smoke windows launch hidden.


## Frame-report phase boundaries

Set `AETHER_FRAME_REPORT=2` for periodic wall-frame and phase reports. The fixed
600-frame timeline retains the producer measurements; `FramePhases` prints each
phase's mean/maximum in milliseconds from that same snapshot:

| Field | Measured interval |
| --- | --- |
| `begin` | Pre-frame retirement/control dispatch after the frame cap |
| `pump` | OS event pumping |
| `update` | Input tick, scene and application update |
| `ui` | Editor UI, overlay frame lock/build and snapshot |
| `extract` | `PrepareFrame` world/shadow/preview extraction |
| `sceneui` | Packet setup and scene UI command-buffer build |
| `submit` | Producer channel submission |
| `render` | Render-thread elapsed time excluding main swapchain acquire/submit/present |

`FrameReport`'s `present` now covers main swapchain acquire/fence work and the native
`GpuDevice::SubmitAndPresent` call. It previously measured the entire `EndFrame`,
incorrectly charging lighting, render-graph execution, editor overlay locking and
secondary viewports to presentation. Old reports cannot establish a driver stall.
The render elapsed phase includes overlay and secondary-viewport work/waits; it is
not a GPU timestamp or a measurement of thread CPU utilization. Producer and render
threads overlap, so their totals must not be added to infer wall latency.

The report prints actual swapchain `mode` beside `requested`, and the FIFO occlusion
heuristic uses the actual mode. A driver can reject MAILBOX and select FIFO. An
unfocused FIFO sample is explicitly marked `foreground pacing unverified`, even
when present-timing support is unavailable and the cadence heuristic cannot run.
Do not turn that background diagnostic into a foreground acceptance result.

Changing VSync, focus, competing editor workload, render resolution or quality can
change timings independently. Retain those conditions and rejected samples. No
presentation policy, render quality, focus behavior or frame budget changes are
implied by enabling this report.

Agent sessions additionally clear ImGui's OS platform-focus callback after backend
initialization. This preserves internal dock/tab focus and leaves non-agent editor
callbacks unchanged. The GLFW backend already creates secondary windows with focus
on show disabled. External focus changes can still invalidate a background sample;
the pacing harness stops only its own test session when it observes one. This guard
is not evidence that the earlier transient focus event had a known cause.
