# Procedural terrain from C#

`ProceduralMesh.Set(entity, positions, normals, indices, colors, collision: true)`
creates or replaces entity-owned runtime mesh geometry and static triangle collision.
Positions are local to the entity; normals match the positions, indices describe triangles,
and optional RGB colours match the vertex count. Empty arrays clear both geometry and collision.

Call from a game-thread script callback. Arrays are copied before return.
The operation drains submitted render frames, parks rendering, waits for GPU idle,
then rebuilds the mesh and the physics body. Collision is available to raycasts on return.
This deliberately prioritizes correctness over throughput; rebuilding many chunks each frame
will stall rendering. Worker threads may generate arrays but must not call this API.

The mesh is owned by the scene context, replaced without retaining old GPU meshes,
and released after entity destruction or scene teardown. Explicit `ProceduralMesh.Clear`
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

Live mesh upload and repeated scene teardown still require successful rendering on a supported
machine. On the local RX 6900 XT, the editor reached scene loading but rendering lost the device
before terrain scripts ran. See the parent project's GPU compatibility handoff for evidence.
