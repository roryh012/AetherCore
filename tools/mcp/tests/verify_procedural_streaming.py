"""Isolated native SDK/ABI streaming fixture; never restarts another editor."""
import argparse,json,os,socket,subprocess,time,uuid
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--port',type=int,required=True);a=p.parse_args()
with socket.socket(type=socket.SOCK_DGRAM) as reserved:reserved.bind(('127.0.0.1',a.port))
repo=Path(__file__).resolve().parents[3];build=a.build.resolve()
case=repo/'artifacts/procedural-streaming'/str(uuid.uuid4());project=case/'project'
(project/'scripts').mkdir(parents=True);(project/'scenes').mkdir();(project/'assets').mkdir();(case/'profile').mkdir()
(project/'ProjectSettings.toml').write_text("[app]\nstartupscene='StreamingFixture'\ntargetFps=60\nidleFps=0\n[project]\nname='Streaming fixture'\nversion=1\n[window]\nwidth=960\nheight=720\n")
(project/'scenes/StreamingFixture.scene.toml').write_text("[scene]\nkind='3d'\nname='StreamingFixture'\nversion=16\n[[entities]]\nname='Streaming verifier'\nparent=-1\nposition=[0.0,0.0,0.0]\n[[entities.scripts]]\ntype='StreamingFixture'\n")
(project/'scenes/StreamingExit.scene.toml').write_text("[scene]\nkind='3d'\nname='StreamingExit'\nversion=16\n[[entities]]\nname='Exit verifier'\nparent=-1\nposition=[0.0,0.0,0.0]\n[[entities.scripts]]\ntype='StreamingExitProbe'\n")
(project/'scripts/StreamingFixture.cs').write_text((repo/'tools/mcp/tests/StreamingFixture.cs').read_text())
(project/'scripts/AetherGame.csproj').write_text(f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><AssemblyName>AetherGame</AssemblyName><TargetFramework>net10.0</TargetFramework><Optimize>true</Optimize><Nullable>enable</Nullable></PropertyGroup><ItemGroup><ProjectReference Include="{repo.as_posix()}/managed/AetherCore/AetherCore.csproj" Private="false" ExcludeAssets="runtime" /></ItemGroup></Project>')
env=os.environ.copy();env.update(AETHER_CONTROL_PORT=str(a.port),AETHER_AGENT_SESSION='1',AETHER_FRAME_REPORT='2',AETHER_SESSION_NAME='Verification - streaming '+case.name[:12],AETHER_USER_CONFIG_DIR=str(case/'profile'),AETHER_STREAM_REPORT=str(case/'result.json'))
cli=build/'tools/control-client/RelWithDebInfo/aether-ctl.exe'
def call(method,args={}):
 r=subprocess.run([str(cli),'--port',str(a.port),method,json.dumps(args)],capture_output=True,text=True,timeout=20)
 return json.loads(r.stdout) if r.stdout.strip() else None
with (case/'native.log').open('w') as log:
 app=subprocess.Popen([str(build/'src/app/RelWithDebInfo/Editor.exe'),'--project',str(project),'--window-center','-1500,800','--validation'],cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
 try:
  end=time.monotonic()+120
  while time.monotonic()<end:
   assert app.poll() is None,'Native editor exited; inspect '+str(case)
   try:
    info=call('info')
    if info and info.get('sessionName')==env['AETHER_SESSION_NAME']:break
   except (ValueError,subprocess.TimeoutExpired):pass
   time.sleep(.2)
  else:raise AssertionError('Control endpoint did not start')
  assert not info.get('focused',False),'Background fixture unexpectedly focused'
  (case/'engine-info.json').write_text(json.dumps(info,indent=2))
  call('engine.play')
  end=time.monotonic()+180
  while time.monotonic()<end and not (case/'result.json.exit').exists():
   assert app.poll() is None,'Native editor exited; inspect '+str(case)
   if (case/'result.json').exists() and '"passed":false' in (case/'result.json').read_text():break
   time.sleep(.2)
  result=json.loads((case/'result.json').read_text());assert result['passed'],result
  assert result['visualBurstStarts']==32 and result['burstFrames']>0,result
  exit_result=json.loads((case/'result.json.exit').read_text());assert exit_result['passed'],exit_result
  (case/'memory-after-exit.json').write_text(json.dumps(call('render.memory',dict(limit=0)),indent=2))
  errors=call('console.logs',dict(minimumLevel='error',limit=20));assert errors['matched']==0,errors
  (case/'errors.json').write_text(json.dumps(errors,indent=2))
  call('engine.stop')
 finally:
  app.terminate();app.wait(timeout=20)
print('PROCEDURAL_STREAMING_PASS',case)
