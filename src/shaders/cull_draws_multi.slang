// GPU-driven indirect draw culling pass (multi-frustum).
//
// Like cull_draws.slang but tests each draw against 3 independent cascade
// frustums and writes 3 separate VkDrawIndexedIndirectCommand outputs (one per
// cascade).  Replaces 3x single-frustum dispatches on 3x duplicated data with
// 1 dispatch on shared data.
//
// See cull_draws.slang for the base data layout.  This shader uses
// CullMultiPushConstants instead of CullPushConstants:
//
//   offset  0 : DevicePtr<FrameConstantsData> frame[3]  per-cascade frame data
//   offset 24 : DevicePtr<DrawInstanceData>   instanceData
//   offset 32 : DevicePtr<CullDrawInput>      inputCmd
//   offset 40 : DevicePtr<uint>               outputCmd   output buffer base
//   offset 48 : DevicePtr<CullBatch>          batchDesc
//   offset 56 : uint                          totalDrawCount
//   offset 60 : uint                          outputCascadeStride  bytes between cascade output regions
//   offset 64 : uint                          debugFlags
//   offset 68 : uint                          _pad0

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/FrustumCull.slangh"

[[vk::push_constant]] CullMultiPushConstants pc;

// ── Compute entry point ──────────────────────────────────────────────────────

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint drawIdx = tid.x;
    if (drawIdx >= pc.totalDrawCount)
        return;

    CullDrawInput    cmd  = pc.inputCmd.Load(drawIdx);
    DrawInstanceData inst = pc.instanceData.Load(cmd.firstInstance);
    CullBatch        batch = pc.batchDesc.Load(cmd.batchIndex);

    const bool forceVisible = (pc.debugFlags & kCullDebugForceVisibleBit) != 0u;
    const uint localOutputSlot = drawIdx - batch.inputStart;
    if (localOutputSlot >= batch.drawCount)
        return;

    // Test against each cascade frustum and write output independently.
    [unroll]
    for (uint cascade = 0; cascade < kCullMultiFrustumCount; ++cascade)
    {
        FrameConstantsData* fc = pc.frame[cascade].Ptr();
        const bool visible = forceVisible || TestSphereFrustum(inst.worldBoundingSphere, fc);

        // Output slot = batch.outputStart + localOutputSlot, offset by cascade * stride.
        // The stride is bytes, but indirect commands are uint-aligned. Keep the
        // physical-storage pointer typed as uint: byte-pointer casts trigger an
        // AMD compiler crash on the RX 6900 XT driver during shader-object creation.
        uint base = cascade * (pc.outputCascadeStride / 4u) + (batch.outputStart + localOutputSlot) * 5u;
        pc.outputCmd.Store(base + 0u, cmd.indexCount);
        pc.outputCmd.Store(base + 1u, visible ? cmd.instanceCount : 0u);
        pc.outputCmd.Store(base + 2u, cmd.firstIndex);
        pc.outputCmd.Store(base + 3u, asuint(cmd.vertexOffset));
        pc.outputCmd.Store(base + 4u, cmd.firstInstance);
    }
}
