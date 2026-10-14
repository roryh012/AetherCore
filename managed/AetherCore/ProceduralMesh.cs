using System;
using System.Numerics;

namespace AetherCore;

/// <summary>Entity-owned runtime geometry. Call on the game thread from a script callback.
/// Arrays are copied before return. Updates drain rendering and rebuild static collision
/// synchronously; suitable for prototypes, not continuous high-throughput streaming.
/// Clear before destroying the entity, or let scene teardown release its mesh.</summary>
public static unsafe class ProceduralMesh
{
    public static void Set(Entity entity, ReadOnlySpan<Vector3> positions,
        ReadOnlySpan<Vector3> normals, ReadOnlySpan<uint> indices,
        ReadOnlySpan<Vector3> colors = default, bool collision = true)
    {
        Validate(positions, normals, indices, colors);
        fixed (Vector3* p = positions, n = normals, c = colors)
        fixed (uint* i = indices)
        {
            if (Native.aether_set_procedural_mesh(entity.Id, p, n, c, positions.Length, i, indices.Length, collision ? 1 : 0) == 0)
                throw new InvalidOperationException("Procedural mesh update failed; inspect the engine log.");
        }
    }

    public static void Clear(Entity entity) => Set(entity, default, default, default);

    internal static void Validate(ReadOnlySpan<Vector3> positions, ReadOnlySpan<Vector3> normals,
        ReadOnlySpan<uint> indices, ReadOnlySpan<Vector3> colors)
    {
        if (positions.Length != normals.Length || (!colors.IsEmpty && colors.Length != positions.Length))
            throw new ArgumentException("Normals and optional colors must match the vertex count.");
        if (indices.Length % 3 != 0 || (positions.IsEmpty != indices.IsEmpty))
            throw new ArgumentException("Supply indexed triangles, or empty arrays to clear.");
        for (int k = 0; k < positions.Length; k++)
        {
            if (!Finite(positions[k]) || !Finite(normals[k]) || (!colors.IsEmpty && !Finite(colors[k])))
                throw new ArgumentException("Vertex attributes must be finite.");
        }
        foreach (uint index in indices)
            if (index >= positions.Length) throw new ArgumentException("Triangle index is out of range.");
    }

    private static bool Finite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z);
}
