using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace AetherCore;

public static unsafe partial class ProceduralMesh
{
    /// <summary>1 = copied tickets, asynchronous visual uploads and background static colliders.
    /// 0 = an older native build; keep using the synchronous API until it is updated.</summary>
    public static int StreamingApiVersion
    {
        get { try { return Native.aether_procedural_streaming_version(); } catch (EntryPointNotFoundException) { return 0; } }
    }

    public enum UploadState { Queued, Preparing, Uploading, Ready, Committed, Cancelled, Failed }

    /// <summary>Scene-owned readiness ticket. Dispose releases pending or completed ticket storage.
    /// Cancellation never clears the last committed view. A new request for the same entity supersedes the old one.</summary>
    public readonly record struct UploadTicket(ulong Id) : IDisposable
    {
        public bool IsValid => Id != 0;
        public UploadStatus Poll() => Native.aether_procedural_poll(Id);
        public bool TryCommit() => Native.aether_procedural_commit(Id) != 0;
        public void Dispose() { if (Id != 0) Native.aether_procedural_cancel(Id); }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct UploadStatus
    {
        public UploadState State;
        private int visualReady, collisionReady, reserved;
        public double InputCopyMs, ColliderPreparationMs, AllocationCopyMs, UploadCompletionMs, ColliderCommitMs, CommitMs;
        public ulong ReservedBytes;
        public readonly bool VisualReady => visualReady != 0;
        public readonly bool CollisionReady => collisionReady != 0;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct StreamingStatistics { public ulong Jobs, ReservedBytes, Rejected; }
    public static StreamingStatistics StreamingStats => Native.aether_procedural_stats();

    /// <summary>Create an empty, unit-scale entity sharing the source material and pipelines.
    /// The source may be destroyed afterwards. No carrier model is loaded.</summary>
    public static Entity CreateInstance(Entity materialSource)
    {
        uint id = Native.aether_create_procedural_instance(materialSource.Id);
        if (id == 0) throw new InvalidOperationException("Procedural instance requires a live material/pipeline source.");
        return new Entity(id);
    }

    /// <summary>Copy validated input and return immediately with a ticket. An invalid ticket means
    /// admission failed (backpressure or an unavailable/invalid target): retry valid work later. Priority is descending; physical/visible work should rank highest.
    /// Prepare off screen, poll, then TryCommit. No query or commit waits for rendering or collision cooking.</summary>
    public static UploadTicket QueueTextured(TexturedUpdate update, bool collision = false, bool horizon = false, int priority = 0)
    {
        var u = update;
        Validate(u.Positions, u.Normals, u.Indices, u.Colors);
        if (horizon && collision) throw new ArgumentException("Horizon deformation is presentation only.");
        if (u.Texcoords.Length != u.Positions.Length || u.SecondaryTexcoords.Length != u.Positions.Length) throw new ArgumentException("UV arrays must match vertices.");
        foreach (var uv in u.Texcoords) if (!float.IsFinite(uv.X) || !float.IsFinite(uv.Y)) throw new ArgumentException("UVs must be finite.");
        foreach (var uv in u.SecondaryTexcoords) if (!float.IsFinite(uv.X) || !float.IsFinite(uv.Y)) throw new ArgumentException("UVs must be finite.");
        fixed (Vector3* p=u.Positions, n=u.Normals, c=u.Colors)
        fixed (uint* i=u.Indices)
        fixed (Vector2* uv=u.Texcoords, uv2=u.SecondaryTexcoords)
            return new(Native.aether_queue_procedural_mesh(u.Entity.Id,p,n,c,u.Positions.Length,i,u.Indices.Length,uv,uv2,(horizon?1:0)|(collision?4:0),priority));
    }

    public static UploadTicket QueueCollision(Entity entity, ReadOnlySpan<Vector3> positions, ReadOnlySpan<uint> indices, int priority = 0)
    {
        Validate(positions, positions, indices, default);
        fixed (Vector3* p=positions)
        fixed (uint* i=indices)
            return new(Native.aether_queue_procedural_mesh(entity.Id,p,null,null,positions.Length,i,indices.Length,null,null,2,priority));
    }

    public static UploadTicket QueueSection(Entity entity, Vector3 origin, float spacing, int cells, int height, ReadOnlySpan<float> density, ReadOnlySpan<float> basalt, int priority = 0)
    {
        if (cells<1 || cells>64 || height<1 || height>1024 || !Finite(origin) || origin.Y!=0 || !float.IsFinite(spacing) || spacing<=0 || !float.IsFinite(spacing*height)
            || density.Length!=checked((cells+1)*(cells+1)*(height+1)) || basalt.Length!=4) throw new ArgumentException("Invalid section dimensions.");
        float span = cells * spacing;
        if (!float.IsFinite(span) || !Finite(origin + new Vector3(span,0,span))) throw new ArgumentException("Section bounds must be finite.");
        foreach (float value in density) if (!float.IsFinite(value)) throw new ArgumentException("Density must be finite.");
        foreach (float value in basalt) if (!float.IsFinite(value)) throw new ArgumentException("Blend must be finite.");
        fixed (float* d=density,b=basalt)
            return new(Native.aether_queue_procedural_section(entity.Id,origin,spacing,cells,height,d,density.Length,b,priority));
    }
}
