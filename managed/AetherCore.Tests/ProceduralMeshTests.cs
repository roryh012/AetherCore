using System;
using System.Numerics;
using Xunit;

namespace AetherCore.Tests;

public sealed class ProceduralMeshTests
{
    [Fact]
    public void EmptyGeometryIsAValidClear()
    {
        ProceduralMesh.Validate([], [], [], []);
    }

    [Fact]
    public void RejectsMismatchedAttributesAndInvalidIndices()
    {
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([Vector3.Zero], [], [0,0,0], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([Vector3.Zero], [Vector3.UnitY], [0,0,1], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([Vector3.Zero], [Vector3.UnitY], [0], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([Vector3.Zero], [Vector3.UnitY], [], []));
    }

    [Fact]
    public void RejectsNonFiniteAttributes()
    {
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([new(float.NaN,0,0)], [Vector3.UnitY], [0,0,0], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([Vector3.Zero], [new(0,float.PositiveInfinity,0)], [0,0,0], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.Validate([Vector3.Zero], [Vector3.UnitY], [0,0,0], [new(0,0,float.NaN)]));
    }

    [Fact]
    public void TexturedUploadRejectsInvalidUvsBeforeInterop()
    {
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetTextured(default,
            [Vector3.Zero], [Vector3.UnitY], [0,0,0], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetTextured(default,
            [Vector3.Zero], [Vector3.UnitY], [0,0,0], [new(float.NaN,0)]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetTextured(default,
            [Vector3.Zero], [Vector3.UnitY], [0,0,0], [new(0,float.PositiveInfinity)]));
    }
    [Fact]
    public void CollisionOnlyRejectsMalformedGeometryBeforeInterop()
    {
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetCollision(default, [Vector3.Zero], [0,0,1]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetCollision(default, [Vector3.Zero], []));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetCollision(default, [Vector3.Zero], [0]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetCollision(default, [new(float.NaN,0,0)], [0,0,0]));
    }

    [Fact]
    public void SectionRejectsMalformedPayloadBeforeInterop()
    {
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetSection(default,Vector3.Zero,1,16,16,[],[0,0,0,0]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetSection(default,Vector3.Zero,float.NaN,1,1,new float[8],[0,0,0,0]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetSection(default,Vector3.Zero,float.MaxValue,1,2,new float[12],[0,0,0,0]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetSection(default,Vector3.UnitY,1,1,1,new float[8],[0,0,0,0]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetSection(default,Vector3.Zero,1,1,1,[float.NaN,0,0,0,0,0,0,0],[0,0,0,0]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetSection(default,Vector3.Zero,1,1,1,new float[8],[0,0,0,float.PositiveInfinity]));
        Assert.Throws<ArgumentException>(() => ProceduralMesh.SetTextured(default,[],[],[],[],collision:true,horizon:true));
    }
}
