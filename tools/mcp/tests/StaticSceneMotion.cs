using AetherCore;
using System;

public sealed class StaticSceneMotion : EntityScript
{
    Entity camera;
    int ticks;
    public override void OnAttach()
    {
        camera=Camera.CreateOrbit(new(0,12,30),new(0,1,0),60);
        Camera.SetMain(camera);
    }
    public override void OnUpdate(float dt)
    {
        ticks++;
        Camera.SetOrbital(camera,ticks*.12f,24,28+10*MathF.Sin(ticks*.007f));
    }
}
public sealed class StaticSceneReload : EntityScript
{
    int ticks;
    static bool alternate;
    public override void OnUpdate(float dt)
    {
        if(++ticks<180)return;
        alternate=!alternate;
        Scene.Load(alternate?"ReloadB":"ReloadA");
    }
}
