using AetherCore;
using System;
using System.Numerics;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

public sealed class StreamingFixture : EntityScript
{
    public static ProceduralMesh.UploadTicket ExitTicket;
    Entity target, section;
    ProceduralMesh.UploadTicket ticket, stale;
    int phase, frames, burstFrames;
    List<(Entity Entity, ProceduralMesh.UploadTicket Ticket)> burst = new();
    List<object> timings = new();
    string report = Environment.GetEnvironmentVariable("AETHER_STREAM_REPORT")!;
    static Vector3[] Points(float y) => [new(-2,y,-2),new(-2,y,2),new(2,y,2),new(2,y,-2)];
    static ProceduralMesh.TexturedUpdate Geometry(Entity e,float y) => new(e,Points(y),[Vector3.UnitY,Vector3.UnitY,Vector3.UnitY,Vector3.UnitY],[0,1,2,0,2,3],[Vector2.Zero,Vector2.UnitY,Vector2.One,Vector2.UnitX],[Vector3.One,Vector3.One,Vector3.One,Vector3.One],[Vector2.Zero,Vector2.Zero,Vector2.Zero,Vector2.Zero]);
    static ProceduralMesh.TexturedUpdate LargeGeometry(Entity entity)
    {
        const int side=257;
        var points=new Vector3[side*side];var normals=new Vector3[points.Length];var colors=new Vector3[points.Length];var uv=new Vector2[points.Length];var uv2=new Vector2[points.Length];
        for(int z=0;z<side;z++)for(int x=0;x<side;x++){int i=z*side+x;points[i]=new(x,0,z);normals[i]=Vector3.UnitY;colors[i]=Vector3.One;uv[i]=new(x,z);}
        var indices=new uint[256*256*6];int cursor=0;
        for(int z=0;z<256;z++)for(int x=0;x<256;x++){uint i=(uint)(z*side+x);indices[cursor++]=i;indices[cursor++]=i+side;indices[cursor++]=i+1;indices[cursor++]=i+1;indices[cursor++]=i+side;indices[cursor++]=i+side+1;}
        return new(entity,points,normals,indices,uv,colors,uv2);
    }
    static void Check(bool ok,string message) { if(!ok) throw new Exception(message); }
    void Height(float height) { var hit=Physics.Raycast(new(0,20,0),-Vector3.UnitY,40);Check(hit.DidHit && Math.Abs(hit.Position.Y-height)<.01f,"Collider parity at "+height); }
    public override void OnAttach()
    {
        try
        {
            Check(ProceduralMesh.StreamingApiVersion==1,"Matching native capability");
            var source=World.Create();source.AddMesh(World.CreateMesh("cube"));
            target=ProceduralMesh.CreateInstance(source);section=ProceduralMesh.CreateInstance(source);source.Destroy();
            var camera=Camera.CreateOrbit(new(9,12,15),new(0,2,0),55);Camera.SetMain(camera);
            stale=ProceduralMesh.QueueTextured(Geometry(target,1),collision:true);
            for(int i=0;i<32;i++)
            {
                var entity=ProceduralMesh.CreateInstance(target);
                var upload=ProceduralMesh.QueueTextured(Geometry(entity,5));
                Check(upload.IsValid,"Visual burst admission");burst.Add((entity,upload));
            }
            var input=Geometry(target,3);ticket=ProceduralMesh.QueueTextured(input,collision:true,priority:10);
            input.Positions[0]=new(float.NaN,0,0);
            Check(stale.IsValid && ticket.IsValid && !stale.TryCommit(),"Generation supersession");
        }
        catch(Exception error) { Fail(error); }
    }
    void Fail(Exception error) { File.WriteAllText(report,JsonSerializer.Serialize(new {passed=false,error=error.ToString(),phase}));phase=99; }
    public override void OnUpdate(float deltaTime)
    {
        if(phase==99)return;
        try
        {
            if(++frames>1800)throw new Exception("Streaming timed out at "+phase);
            if(burst.Count>0)
            {
                if(frames==2)
                {
                    foreach(var item in burst)Check(item.Ticket.Poll().State!=ProceduralMesh.UploadState.Queued,"Visual-only burst must bypass collider start limit");
                    var cancelled=burst[^1];cancelled.Ticket.Dispose();
                    Check(!cancelled.Ticket.TryCommit(),"Started visual cancellation");cancelled.Entity.Destroy();burst.RemoveAt(burst.Count-1);
                }
                for(int i=burst.Count-1;i>=0;i--)
                {
                    var item=burst[i];var state=item.Ticket.Poll().State;
                    Check(state!=ProceduralMesh.UploadState.Failed,"Visual burst preparation");
                    if(state!=ProceduralMesh.UploadState.Ready || !item.Ticket.TryCommit())continue;
                    item.Ticket.Dispose();item.Entity.Destroy();burst.RemoveAt(i);
                }
                if(burst.Count>0)return;
                burstFrames=frames;
            }
            if(phase==5)
            {
                if(ProceduralMesh.StreamingStats.Jobs!=0)return;
                Check(ProceduralMesh.StreamingStats.ReservedBytes==0,"Queued bytes released");
                File.WriteAllText(report,JsonSerializer.Serialize(new {passed=true,frames,visualBurstStarts=32,burstFrames,timings,stats=ProceduralMesh.StreamingStats},new JsonSerializerOptions{IncludeFields=true}));
                var exitTarget=World.Create();exitTarget.AddMesh(World.CreateMesh("cube"));
                ExitTicket=ProceduralMesh.QueueTextured(LargeGeometry(exitTarget),collision:true);
                Check(ExitTicket.IsValid,"Pending scene-exit ticket");
                phase=6;return;
            }
            if(phase==6)
            {
                if(ExitTicket.Poll().State==ProceduralMesh.UploadState.Queued)return;
                Scene.Load("StreamingExit");phase=99;return;
            }
            var status=ticket.Poll();
            if(phase==3)
            {
                if(status.State!=ProceduralMesh.UploadState.Failed)return;
                Check(!ticket.TryCommit(),"Failed preparation cannot commit");Height(7);ticket.Dispose();
                ticket=ProceduralMesh.QueueTextured(new(target,[],[],[],[],[],[]),collision:true);phase=4;return;
            }
            if(status.State==ProceduralMesh.UploadState.Failed)throw new Exception("Unexpected preparation failure");
            if(status.State!=ProceduralMesh.UploadState.Ready)return;
            Check(status.VisualReady && status.CollisionReady,"Separate readiness gates");
            if(!ticket.TryCommit())return;
            timings.Add(ticket.Poll());ticket.Dispose();
            switch(phase++)
            {
                case 0:
                    Height(3);
                    ticket=ProceduralMesh.QueueSection(section,new Vector3(5,0,0),1,1,1,new float[]{1,1,1,1,1,1,1,1},[0,0,0,0]);
                    break;
                case 1:
                    Height(3);
                    ticket=ProceduralMesh.QueueCollision(target,Points(7),[0,1,2,0,2,3],priority:100);
                    break;
                case 2:
                    Height(7);
                    ticket=ProceduralMesh.QueueTextured(Geometry(target,9) with {Indices=[0,0,0]},collision:true);
                    break;
                case 4:
                    Check(!Physics.Raycast(new(0,20,0),-Vector3.UnitY,40).DidHit,"Empty commit clears collision");
                    var pending=ProceduralMesh.QueueTextured(Geometry(target,4));pending.Dispose();
                    Check(!pending.TryCommit(),"Cancelled commit rejected");
                    target.Destroy();section.Destroy();
                    break;
            }
        }
        catch(Exception error) { Fail(error); }
    }
    public override void OnDetach() { ticket.Dispose();stale.Dispose();foreach(var item in burst)item.Ticket.Dispose(); }
}

public sealed class StreamingExitProbe : EntityScript
{
    int frames;
    public override void OnUpdate(float dt)
    {
        if(StreamingFixture.ExitTicket.TryCommit())throw new Exception("Scene exit published a stale ticket");
        if(++frames>600)throw new Exception("Scene exit retained ticket memory");
        var stats=ProceduralMesh.StreamingStats;
        if(stats.Jobs!=0 || stats.ReservedBytes!=0)return;
        File.WriteAllText(Environment.GetEnvironmentVariable("AETHER_STREAM_REPORT")+".exit",JsonSerializer.Serialize(new {passed=true,frames,stats},new JsonSerializerOptions{IncludeFields=true}));
    }
}
