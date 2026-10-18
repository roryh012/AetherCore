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


## Windows frame-cap sleep

`FramePacer` uses an unnamed, per-thread high-resolution waitable timer on Windows,
with a 500 microsecond final spin allowance. The timer handle is reused and closed
at thread exit. Timer creation/set/wait failure falls back to `sleep_until`; other
platforms retain their standard sleep and two millisecond spin allowance. No global
timer resolution or thread-priority policy changes. Frame deadlines, overrun reset,
uncapped operation and the separate event-driven idle wait remain unchanged.

A native actual-class ABBA probe reproduced 60 FPS baseline p95 around 29.8 ms and
120 FPS baseline around 16 ms. The final timer measured 60 FPS p95 16.67–16.85 ms
and 120 FPS 8.35 ms. The initial two millisecond spin cost about 0.61 CPU seconds
per five wall seconds; calibration to 500 microseconds reduced the sampled CPU
cost to 0.016–0.078 seconds at 60 FPS. CPU accounting is quantized; short samples
reporting zero are not proof of zero cost. Handle counts remained stable.

This is limiter evidence, not dense forest or foreground acceptance. A retained
candidate maximum reached 25.10 ms under shared scheduling. Unsupported timer/OS
and non-Windows paths were not run on this machine. Reproduction sources, complete
measurements and limitations are in
`tests/evidence/static-scene-pacing/frame-pacer-timer.json`.


### Agent main-window activation

Focus-on-show hints alone do not prevent Windows from selecting a background
window after the foreground window closes. Agent main windows now receive
`WS_EX_NOACTIVATE` before showing; failure aborts creation after cleaning up the
GLFW window. Normal editor windows retain their existing styles. A hidden native
window test checks normal-style preservation, agent flag installation, repeated
application and invalid-handle failure without activating any window.

This policy covers the engine main HWND only. Background verification currently
uses private `imguiViewports=false` profiles; detached ImGui HWND activation is
not covered by this helper. Do not run old GUI baselines that lack this policy or
claim a complete before/after editor comparison from aborted focus samples. The
runtime sample in `616b6618-00bc-4ee1-9ba8-32dbd5c83dc3` was stopped on focus1/flips1;
it remains invalid. No precise external activation cause has been established.


The rebuilt agent candidate completed runtime/editor static replays unfocused with
zero focus transitions (p95 23.10/22.49 ms, maxima 46.91/81.60 ms retained). These
are candidate-only samples, not a complete matched regression result or a claim
that hitches are eliminated. Final canonical CMake native gate: 1,365 passed,
216,627 assertions, five skipped; GPU guard clean. Earlier gauntlet native-output
pipeline runs each failed one wall-clock networking interpolation assertion;
isolated case/file passed. These failures remain recorded, with no networking
source change or claimed root cause. The gauntlet now invokes the standard CTest
entrypoint and retains raw native output instead of processing it live in PowerShell.
Build and GUI smokes are reported separately when skipped by a partial gauntlet.


## Main-queue culling diagnostic control

`render.culling {}` reads `{scope:"main", forceVisible:bool, applies:"next prepared frame"}`.
`render.culling {"forceVisible":false}` requests the existing GPU frustum path;
true restores the force-visible override. Values must be JSON booleans. The
requested flag is atomic across control/UI and render threads; the getter confirms
requested state immediately, not completion of an already queued GPU frame.
Default startup remains force-visible. No shadow/collider policy changes.

Use a private, unfocused windowed profile with ImGui viewports disabled. Verify
session identity before writing, allow queued frames to drain, compare fixed camera
views, and restore the original value. The historical override hid unresolved
culling issues; CPU plane/clip-space tests do not prove displaced terrain, sky,
billboard, shear or skinned bounds correct. Do not enable a new default from a
performance-only result. The native CLI can exercise this wire method while an
existing MCP server retains an older tool manifest; do not restart other sessions.

Native bool round-trip, invalid-string rejection without state change and restore
passed, followed by the Vulkan streaming/collider/section/cancellation replay.
Evidence: `tests/evidence/static-scene-pacing/main-culling-control.json`.


## UI and swapchain timing boundaries

`FramePhases` includes `uibegin`, `uibuild`, and `uifinal`, which partition the
existing `ui` elapsed interval. Render-side `fence`, `acquire`, and `presentcall`
separate the fence wait, swapchain image acquisition, and native submit/present
call. They are subsets of the existing `FrameReport` present aggregate; small
setup work remains in that aggregate. Means and maxima are reported separately.
Do not add maxima, or sum overlapping producer and render elapsed times as CPU
utilization. These counters change no presentation or queue policy.

Snapshot acquires render completion before reading its published phases and
emits zero for incomplete render records. Slot reuse clears all render waits.
Static runtime/editor validation passed unfocused with zero focus transitions;
this validates attribution, not the integrated forest-to-cave pacing gate.
Evidence: `tests/evidence/static-scene-pacing/pacing-boundaries.json`.


Original-camera game replay (`c21d51c`, diagnostic host `91d22ed2`) still failed
the 25 ms motion p95 gate: forest 32.5349 ms, cave 31.2979 ms. No native/section
or scenery upload counters advanced during either settled zoom window. Native
errors were zero, two-second GPU-buffer cleanup passed, normal save unchanged,
and all FIFO reports stayed unfocused with zero transitions. Fine timings show
fence/image-acquisition waits and UI build work; submit/present calls are short.
Native rolling windows also include exports/travel, so their maxima are not
pure motion samples. The two concurrent external editors make cross-run GPU
comparisons uncontrolled.

An IMMEDIATE follow-up was stopped on its first focus transition, near a
swapchain resize from 1280x720 to 3440x1369. Activation cause remains unknown;
no comparison or pass is claimed, and no speculative window policy was changed.
Do not conflate this baseline attribution with the new camera integration, warm
return acceptance, or foreground pacing. Evidence and retained failures:
`tests/evidence/static-scene-pacing/original-camera-boundaries.json`.


## UI and draw-command overhead candidate

Hierarchy spawn tracking reuses live set nodes, removes retired IDs and reuses
the scan count. Clipped/collapsed SceneList children skip tree reconstruction;
global copy/cut/paste, duplicate/delete, pending edits and popup handling remain
outside the visibility gate. Shared draw addresses are constant within one flush,
so pushes accompany pipeline changes rather than every batch. Each flush starts
with no remembered pipeline; subsequent frame/lighting addresses are republished.
No GPU synchronization or rendering quality policy changed.

The source-based EnTT probe preserved spawn/retire/recycled-ID behavior and
removed steady-state node allocations (12,000 entities, 300 frames: 3,600,001 to
zero). CPU draw-command intent matched across 16 combinations, with fewer pushes.
These probes are not driver or whole-game pacing proof. Build, 1,369 canonical
native cases and the GPU guard passed; the new in-memory ImGui child visibility
test passed. Native GUI/image parity and integrated motion gates remain pending.
Evidence: `tests/evidence/static-scene-pacing/ui-draw-overhead.json`.


## Invisible agent correctness verification

Set `AETHER_AGENT_SESSION=1` and `AETHER_AGENT_HIDE_WINDOW=1` in an isolated
child process to keep the agent main window hidden after startup. `Window.Show`
returns before positioning/showing it; ordinary windows ignore the hide flag.
The existing GLFW HWND, Vulkan swapchain, managed scripts, control endpoint and
FinalColor capture remain operational. This is not a headless/no-HWND backend.
Use private windowed profiles with `imguiViewports=false`, isolated saves and a
unique control port; verify the exact host/SDK hashes before launch. Do not use
older hosts that silently ignore this flag. Keep ownPID visibility/foreground
and frame-focus guards active and stop only the owned test if they fail.

Hidden runtime/editor Vulkan validation completed without any visible/foreground
own window samples or native focus changes. The 1280x720 static capture was
pixel-identical to the earlier 91d22ed2 capture. The complete native streaming
replay also passed hidden, including sections/colliders/cancellation/pending exit.
These verify correctness and isolation, not visible presentation pacing: hidden
window p95 must never satisfy the integrated forest-to-cave/zoom readiness gate.
The user review session must use normal display policy only after those gates.
Evidence: `tests/evidence/static-scene-pacing/hidden-agent-window.json`.
