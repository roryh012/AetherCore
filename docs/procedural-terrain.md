# Procedural terrain from C#

`ProceduralMesh.Set(entity, positions, normals, indices, colors, collision: true)`
creates or replaces entity-owned runtime mesh geometry and static triangle collision.
Positions are local to the entity; normals match the positions, indices describe triangles,
and optional RGB colours match the vertex count. Empty arrays clear both geometry and collision.

`ProceduralMesh.SetTextured(entity, positions, normals, indices, texcoords, colors,
collision: true)` adds explicit `Vector2` texture coordinates, one finite pair per vertex.
The untextured API remains compatible. Both calls use the same native upload,
entity ownership, synchronization, clearing, and collision behavior. Use
`Material.SetTexture(entity, "project://assets/terrain/rock.png")` to bind an albedo.
Mapping and material selection belong to the game; the engine copies supplied UVs.

Call from a game-thread script callback. Arrays are copied before return.
The operation drains queued CPU render packets and parks submission before publishing
new buffers. Replaced mesh owners are retained in a render-completion queue; the old
buffers cannot be released until the submissions that used them have completed.
Completion is polled with frame-fence tickets, including safe handling of reused
frame slots. Each frame collects completed owners while submission is parked;
general device-idle operations and shutdown also drain this ownership queue.
Mesh publication no longer calls device-wide idle for each replacement. Transfers,
CPU render-packet draining and physics replacement are still synchronous, so this
is not a fully asynchronous streaming API. Collision is available to raycasts on
return. Worker threads may generate arrays but must not call this API.

The current mesh is scene-owned. Replacements retain old GPU meshes temporarily
until their render tickets complete, then release them; entity destruction and
scene teardown release the current mesh. Explicit `ProceduralMesh.Clear`
releases it earlier. Terrain geometry is runtime state: regenerate it on play/load;
it is not serialized as a baked model. The call replaces any previous mesh and rigid-body
collision on the target entity. Use dedicated static terrain entities, with unit scale.

Invalid counts, non-finite attributes, and out-of-range indices are rejected before interop.
Native validation also bounds vertex/index counts and rejects malformed arrays.
GPU allocation failures preserve the previous mesh; a collision creation failure reports failure
after publishing the new mesh. It is not a transactional rollback API.

## Verification

- Managed validation tests cover clear, malformed arrays, and non-finite attributes.
- Native Jolt test covers array-sourced mesh creation, replacement, raycast height,
  and removal without loading assets.
- The game repository has an independent C# density-grid/meshing test harness.

The initial RX 6900 XT attempt lost the device before terrain scripts ran. Subsequent
AMD compatibility work enabled live terrain verification; see the parent project's
GPU compatibility handoff and terrain mesh publication notes. Current local checks
include sustained boundary edits, collision comparison, clear/reset, and scene teardown.

## Collision-only replacement

`ProceduralMesh.SetCollision(entity, positions, indices)` replaces static triangle
collision without creating a GPU mesh, touching material/visual components, or
draining render frames. It copies/validates the arrays, waits for the physics step
to finish, removes the previous body and creates the replacement. Raycasts see the
new body on return. Empty arrays remove collision. Use a dedicated static entity;
this is still synchronous physics work, not a worker-thread API or a rollback
transaction. Entity destruction and scene teardown release the body normally.

The As Above So Below game uses this for hidden full-world collision while separate
textured/cut meshes provide the visible surface. Native Jolt array-mesh checks,
managed pre-interop rejection tests and live edited-mesh/raycast comparisons cover
this path. Visible mesh replacement retains its render-drain limitation.

## Deferred mesh ownership verification

Two native ownership tests check pending resource retention, partial completion,
multiple owners sharing one submission, and shutdown cleanup. Live RX 6900 XT
editing exercises repeated replacements through reused frame-fence slots. Old CPU
mesh objects remain valid until the queued CPU packets have been drained and their
GPU tickets complete. GPU submission must be parked when serials/fences are read.
The rest of the engine retains device-idle synchronization for general mutations;
`DrainCpuOnly` is restricted to this explicit ownership-retention path.

## GPU cutaway prototype

`Renderer.SetCutaway(height, enabled)` snapshots a world-space presentation plane
into each render frame, directional cascade and local-light shadow view. Standard
PBR, prepass and shadow fragments above it are discarded; sky materials are exempt.
This never alters physical terrain. Disable it on scene departure. Custom forward
shaders must consume `CutawayDiscard` too or they will disagree with depth/shadows.
This prototype uses fragment clipping; vertex clip-distance optimisation is deferred.

`ProceduralMesh.SetSection(entity, origin, spacing, cells, height, density, basalt)`
uploads a four-vertex section plane and an immutable density lattice. Call only on
chunk load or edit, on the game thread. Origin must have y=0; density is x-fast,
z-next, y-last, with `(cells+1)^2*(height+1)` finite samples. Four basalt blend values
follow the quad's a,d,e,b order. The entity must carry the game's terrain material.
A reserved secondary-UV marker identifies section vertices. Their vertex buffer
contains four 100-byte vertices, two float4 header records, then float samples.
The shader's section address is buffer address + 400 bytes; payload alignment is
16 bytes. Height changes move only the plane in the shader. The fragment stage
interpolates vertical density and the two horizontal triangles matching the old
CPU cap, discarding empty cave space. Bounds include the complete vertical range.

The density payload shares the mesh's GPU allocation, replacement, deferred
retirement and entity/scene cleanup. There are no independent per-frame texture
uploads, descriptors or density resource owners. `Mesh::Create`/`AssetManager::CreateMesh`
accept an optional immutable byte tail for this purpose; vertex/index counts exclude
it. Uploads still synchronously drain CPU packets and complete transfers on actual
chunk changes. Do not call SetSection during ordinary camera motion.

`SetTextured(..., collision:false, horizon:true)` marks coarse heightfield terrain
for shader clamping to the plane, preserving a closed distant silhouette without
rebuilding horizon tiles for depth. Its bounds conservatively extend down to y=0.
This representation remains unsuitable for detailed caves; the game retains full
3D meshes and density-derived section planes near the camera.

## Whole objects across a cut

`MeshRenderer.SetCutawayExempt(entity, true)` keeps that mesh whole across the
presentation plane. The runtime component defaults to false; it does not change
collision or discovery. Apply to each mesh child of an imported character or prop.
Games must still hide actors whose feet/anchor belong to a hidden floor.

The per-instance flag travels through colour, depth prepass, directional shadows
and local shadows. It is independent of shared materials and skinning. Instance
records now occupy 112 bytes (flags at 104), with matching CPU offset assertions
and explicit shader offsets. Recompile project custom/precompiled shaders when
updating from the earlier 104-byte contract; stale stages can dereference invalid
GPU addresses. The section interpolator's reserved tag 1 denotes exemption and is
never sampled as density; real section addresses remain aligned buffer addresses.

## Grouped visual uploads

`ProceduralMesh.SetTexturedBatch` accepts 1-64 collision-free `TexturedUpdate`
records. It validates and pins the managed arrays, calls one native export, drains
the CPU render queue once for the group, and returns after copying the meshes.
Secondary texture coordinates preserve authored foliage sway through colour,
prepass and shadow shaders. Existing mesh retirement tickets remain in force;
this API does not discard queued frames or free in-flight GPU buffers.

Updates are synchronous, and a native failure can leave an uploaded prefix. Publish
into hidden staging entities and expose them only after the call succeeds. Discard
the entire staged target after failure. Managed pins are released on every exit;
the native batch scope resets on failure or exception. This is not an asynchronous
upload API or an unlimited frame budget. Callers should bound both the number and
size of meshes in a group. The game currently uses at most eight spatial batches
per update and pauses scenery streaming work during animated zoom.

The descriptor is 64 bytes on supported 64-bit targets; native and managed layout
checks cover it. The draw-instance/shader contract remains unchanged (112 bytes).


## Streaming API, version 1

Use a matching native editor/runtime and managed SDK. `ProceduralMesh.StreamingApiVersion`
queries the native build: 1 supports this contract, 0 means the old synchronous path.
The older `Set*` APIs remain synchronous and compatible.

- `CreateInstance(materialSource)` creates an empty unit-scale entity sharing a live
  source's material slot and pipelines. It does not load a GLB, allocate a new material
  slot or upload material data. The source can then be destroyed. Material setters
  retain their existing copy-on-write behavior. Use ordinary terrain/PBR material
  sources; this is not a clone of an entity's effect parameters, hierarchy or scripts.
- `QueueTextured(update, collision:false, horizon:false, priority:0)` validates and
  copies input before returning a scene-owned `UploadTicket`. UV0, UV1/wind, normals,
  colors and horizon markers retain the synchronous representation.
- `QueueSection(...)` copies the same immutable density/header/section representation
  as `SetSection`; moving the cutaway plane still requires no new upload.
- `QueueCollision(entity, positions, indices, priority:0)` cooks an immutable static
  triangle shape on a bounded worker; it performs no GPU upload.

All calls, including polling and disposing, require the game thread. Workers may
prepare source arrays, but must not invoke these APIs. Reuse/mutate managed input
arrays immediately after a successful queue call. Two CPU preparations may run at
once. Native GPU allocation/staging and transfer submission run on the render thread,
serialized with the existing resource registry. The transfer timeline, rather than
a CPU wait, establishes completion. Off-screen transfers are excluded from the
frame's transfer dependency until commit publishes their already-completed timeline
value. Uploads on a graphics-queue fallback still share that queue's physical work;
a dedicated transfer queue can overlap it. No render drain or device-idle operation occurs
in the new queue, poll or commit path. A render-frame scheduler uses a soft 2 ms
budget: an individual allocation/copy cannot be preempted, so this is not a frame-time
guarantee. Canceling or exiting may leave already-submitted copies alive until their
transfer completes; shutdown waits for the device and preparation workers safely.

Poll returns `Queued`, `Preparing`, `Uploading`, `Ready`, `Committed`, `Cancelled`
or `Failed`. Collision readiness is observable independently while the visual upload
is pending. `TryCommit()` returns false until the whole ticket is ready, or while the
physics step is busy. It never waits for that step or cooks a shape. New physics bodies
are allocated before removing old ones; body creation/preparation failure preserves
previous collision and geometry. `Failed` tickets cannot commit. A collision-only
commit changes no visual/material components; a visual-only commit changes no collider.
An empty successful commit clears only the requested domains.

A newer accepted request for an entity supersedes its old ticket. Synchronous `Set*`
mutations also invalidate pending work. Disposing a ticket cancels unpublished work;
it never undoes committed geometry or collision. Destroying the entity, switching
scenes, stopping Play/reloading scripts and scene teardown cancel pending tickets.
Unknown/released tickets poll as `Cancelled`. Every ticket must be disposed, including
failed and committed ones. Dispose is idempotent. Previously submitted GPU resources
remain retained across both queued CPU packets and their GPU completion fences.

```csharp
var staged = ProceduralMesh.CreateInstance(materialSource);
var ticket = ProceduralMesh.QueueTextured(update with { Entity = staged }, priority: 10);
// If !ticket.IsValid, defer/retry. Do not expose a partially staged region.
// In subsequent Update callbacks:
if (ticket.Poll().State == ProceduralMesh.UploadState.Ready && ticket.TryCommit())
{
    // Expose only when every required visual/collider ticket for this window committed.
    ticket.Dispose();
}
```

The game still owns complete-window handover, visibility, physical-access gating,
edit generations and priorities. Keep actors out of pending collision; treat a failed
or cancelled edit as uncommitted and do not silently report it as traversable. The
engine never holds or moves the camera. Failure/cancellation retains the previous
usable view. This API is engine support, not automatic integration into existing game
callers and not proof that regional loading or continuous-motion frame targets pass.

### Backpressure and timings

`StreamingStats` reports outstanding jobs, reserved bytes and rejected admissions.
The limits are 64 outstanding jobs and 256 MiB reserved for source + destination +
staging (three times packed geometry/section bytes). Cancelled jobs remain charged
until their owners retire, so repeated cancellation cannot bypass the limit. Active
entity meshes, the registry's deferred destruction ring and Jolt's internal cooked
shape/temporary allocation overhead are separate; use `render.memory` alongside
these queue counters to measure actual retained GPU memory. An invalid ticket means
admission failed: backpressure, unavailable services or an invalid/non-material target.
Use `CreateInstance` to establish a valid visual target. Larger priority values outrank
smaller values; cancellation drops obsolete CPU work and prevents stale publication.

Per-ticket timings separate validated input copy, collider preparation, render-thread
allocation/staging/copy, observed upload completion, collider commit and total commit.
Upload completion includes polling cadence; it is not a GPU timestamp. CPU packet-drain
time on this path is zero by construction. Old synchronous APIs retain their old
synchronization costs. No capture/readback is needed to collect these timings.

Immutable buffer ownership remains simple. Capacity pooling is deferred until a
regional traversal shows allocation dominates: the initial native quad fixture
measured 0.11–0.17 ms allocation/copy versus up to 52 ms observed upload readiness,
with 0.003–0.080 ms total commits on a shared workstation. These small-fixture figures
are contract evidence, not terrain throughput or an end-to-end game benchmark.

### Validation

`python tools/mcp/tests/verify_procedural_streaming.py --build build/default --port 9063`
creates an isolated project/profile and launches only its own editor. It checks native
capability and ABI, material-source destruction, copied-array ownership, supersession,
mesh+collider commit/raycast parity, density-section upload, collision-only replacement,
failed cooking retaining the old world, empty clearing, cancellation and zero ticket
memory after retirement. Unit tests cover worker-cooked shapes and replacements,
material retention without slot writes, stale/repeated cancellation and admission
limits; managed tests cover ABI and rejection before interop.

The game's authored Graihan–Kyne cold/warm traversal, reversal, loading edit, scene exit,
p95/p99/max and full memory acceptance remains a separate integration workload. Do not
replace that workload with this fixture or advertise the original smooth-zoom goal as
verified solely because these engine checks pass.


Final version-1 check: 1,360 native unit cases and eight managed procedural cases
passed, plus the GPU abstraction guard and an isolated native replay. Scene exit
cancelled a started 66,049-vertex / 131,072-triangle request and returned outstanding
jobs/reservations to zero. See `tests/evidence/procedural-streaming/native-summary.json`.
The fixture was unfocused/idle-throttled with another editor open; its small-mesh
commit timings are not a measurement of playable camera frame pacing. Generic
editor/runtime smokes, other build configurations and cross-platform execution were
not run. A clangd refresh was attempted but clang-cl is unavailable on this machine.
