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

    /// <summary>Replace only static triangle collision. No rendered mesh or GPU drain.
    /// Call on the game thread; arrays are copied and the physics worker is synchronised.
    /// Empty arrays remove collision. Existing visual geometry is unchanged.</summary>
    public static void SetCollision(Entity entity, ReadOnlySpan<Vector3> positions, ReadOnlySpan<uint> indices)
    {
        Validate(positions, positions, indices, default);
        fixed (Vector3* p = positions)
        fixed (uint* i = indices)
            if (Native.aether_set_procedural_collision(entity.Id, p, positions.Length, i, indices.Length) == 0)
                throw new InvalidOperationException("Procedural collision update failed; inspect the engine log.");
    }

    /// <summary>Uploads an immutable density section, x-fast then z then y. The four-vertex
    /// plane follows Renderer.SetCutaway in shaders. Existing authored terrain material is retained.
    /// Call only on loading/editing a chunk. Density is copied before returning.</summary>
    public static void SetSection(Entity entity,Vector3 origin,float spacing,int cells,int height,ReadOnlySpan<float> density,ReadOnlySpan<float> basalt)
    {
        if(cells<1 || cells>64 || height<1 || height>1024 || !Finite(origin) || origin.Y!=0 || !float.IsFinite(spacing) || spacing<=0 || !float.IsFinite(spacing*height)
            || density.Length!=checked((cells+1)*(cells+1)*(height+1)) || basalt.Length!=4)
            throw new ArgumentException("Invalid section density dimensions.");
        foreach(float value in density) if(!float.IsFinite(value)) throw new ArgumentException("Section density must be finite.");
        foreach(float value in basalt) if(!float.IsFinite(value)) throw new ArgumentException("Section blend must be finite.");
        fixed(float* d=density,b=basalt)
            if(Native.aether_set_procedural_section(entity.Id,origin,spacing,cells,height,d,density.Length,b)==0)
                throw new InvalidOperationException("Procedural section upload failed; inspect the engine log.");
    }

    public static void Clear(Entity entity) => Set(entity, default, default, default);

    /// <summary>Upload textured geometry with explicit UVs. Existing ownership and collision rules apply.</summary>
    public static void SetTextured(Entity entity, ReadOnlySpan<Vector3> positions,
        ReadOnlySpan<Vector3> normals, ReadOnlySpan<uint> indices, ReadOnlySpan<Vector2> texcoords,
        ReadOnlySpan<Vector3> colors = default, bool collision = true, bool horizon = false)
    {
        if(horizon && collision) throw new ArgumentException("Horizon deformation is presentation only.");
        Validate(positions, normals, indices, colors);
        if (texcoords.Length != positions.Length)
            throw new ArgumentException("Texture coordinates must match the vertex count.");
        foreach (Vector2 uv in texcoords)
            if (!float.IsFinite(uv.X) || !float.IsFinite(uv.Y))
                throw new ArgumentException("Texture coordinates must be finite.");
        fixed (Vector3* p = positions, n = normals, c = colors)
        fixed (Vector2* uv = texcoords)
        fixed (uint* i = indices)
        {
            if ((horizon ? Native.aether_set_procedural_horizon(entity.Id,p,n,c,positions.Length,i,indices.Length,uv) : Native.aether_set_procedural_mesh_uv(entity.Id, p, n, c, positions.Length, i, indices.Length, collision ? 1 : 0, uv)) == 0)
                throw new InvalidOperationException("Textured procedural mesh update failed; inspect the engine log.");
        }
    }

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
