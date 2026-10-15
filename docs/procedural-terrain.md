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
