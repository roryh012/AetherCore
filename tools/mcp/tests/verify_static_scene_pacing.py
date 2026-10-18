"""Matched static-scene frame-flow regression: no readbacks/control polling in timed windows."""
import argparse,hashlib,json,os,re,shutil,socket,statistics,subprocess,time,uuid
from pathlib import Path

REPORT=re.compile(r"FrameReport n=(\d+) avg=([\d.]+) median=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+) \| game=([\d.]+) inflight=([\d.]+) present=([\d.]+).+?focused=(\d+) focusflips=(\d+)")
FIELDS=['n','avg','median','p95','p99','max','game','inflight','present','focused','focusflips']
def reports(text):
 return [dict(zip(FIELDS,map(float,m.groups()))) for m in REPORT.finditer(text)]
def assess(baseline,candidate):
 # Absolute allowances keep sub-millisecond differences from becoming large percentages.
 limits={'p95':(1.10,.5),'p99':(1.15,1),'max':(1.25,3),'game':(1.15,.05)}
 result={}
 for key,(ratio,absolute) in limits.items():
  b=statistics.median(x[key] for x in baseline);c=statistics.median(x[key] for x in candidate)
  result[key]=dict(baseline=b,candidate=c,limit=b*ratio+absolute,passed=c<=b*ratio+absolute)
 return result

def prepare(repo,case):
 project=case/'project';(project/'scenes').mkdir(parents=True);(project/'scripts').mkdir();(project/'assets/models').mkdir(parents=True)
 source=repo/'projects/TestingProject'
 shutil.copytree(source/'assets/models/Fox',project/'assets/models/Fox')
 packer=repo/'build/default/tools/RelWithDebInfo/AssetPacker.exe'
 p=subprocess.run([str(packer),'bake',str(project/'assets/models/Fox/Fox.gltf'),str(project)],capture_output=True,text=True)
 (case/'bake.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
 base=(source/'scenes/GfxTest.scene.toml').read_text()
 # Render actual imported, textured static meshes through the ordinary asset-upload path.
 for i in range(6):
  base+=f"\n[[entities]]\nname='Static Fox {i}'\nparent=-1\nposition=[{i*2.5-6.0},0.0,0.0]\nscale=[0.02,0.02,0.02]\nmesh_renderer=true\n[entities.material]\nalbedo='project://assets/models/Fox/Texture.png'\nbase_color=[1.0,1.0,1.0,1.0]\nroughness=0.6\n[entities.mesh]\nkind='model'\npath='project://assets/models/Fox/Fox.gltf'\nindex=0\n"
 empty="[scene]\nkind='3d'\nversion=16\n[[entities]]\nname='Empty camera'\nparent=-1\nposition=[0.0,2.2,9.0]\n[entities.camera]\nmain=true\nfov=60.0\nnear=0.1\nfar=1000.0\nprojection='perspective'\n"
 dense=base
 for z in range(20):
  for x in range(20):
   dense+=f"\n[[entities]]\nname='Static cube {x} {z}'\nparent=-1\nposition=[{x-9.5},0.25,{z-12.0}]\nscale=[0.45,0.5,0.45]\nmesh_renderer=true\n[entities.material]\nbase_color=[0.4,0.65,0.5,1.0]\nroughness=0.65\n[entities.mesh]\nkind='primitive'\npath='cube'\nindex=0\n"
 dense+="\n[[entities]]\nname='Camera motion'\nparent=-1\n[[entities.scripts]]\ntype='StaticSceneMotion'\n"
 reload=base+"\n[[entities]]\nname='Scene reload'\nparent=-1\n[[entities.scripts]]\ntype='StaticSceneReload'\n"
 for name,text in [('Empty',empty),('Static',base),('Motion',dense),('ReloadA',reload),('ReloadB',reload)]:
  (project/'scenes'/f'{name}.scene.toml').write_text(text)
 shutil.copyfile(repo/'tools/mcp/tests/StaticSceneMotion.cs',project/'scripts/StaticSceneMotion.cs')
 sdk=(repo/'managed/AetherCore/AetherCore.csproj').as_posix()
 (project/'scripts/AetherGame.csproj').write_text(f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><AssemblyName>AetherGame</AssemblyName><TargetFramework>net10.0</TargetFramework><Optimize>true</Optimize><Nullable>enable</Nullable></PropertyGroup><ItemGroup><ProjectReference Include="{sdk}" Private="false" ExcludeAssets="runtime" /></ItemGroup></Project>')
 output=case/'managed'
 compiled=subprocess.run(['dotnet','build',str(project/'scripts/AetherGame.csproj'),'-c','Release','-o',str(output)],capture_output=True,text=True)
 (case/'scripts-build.log').write_text(compiled.stdout+compiled.stderr);assert compiled.returncode==0,compiled.stdout+compiled.stderr
 return project

def run(repo,case,project,build,label,scene,mode,index,seconds,port,validation):
 tag=f'{mode}-{scene}-{index}-{label}';folder=case/tag;folder.mkdir();profile=folder/'profile';profile.mkdir()
 (profile/'EditorState.toml').write_text('[editor]\nwindow_width=1280\nwindow_height=720\n')
 settings=f"[app]\nstartupScene='{scene}'\nautoplay=true\ntargetFps=60\nidleFps=0\nautosaveSeconds=0\n[project]\nname='Static frame-flow regression'\nversion=1\n[window]\nwidth=1280\nheight=720\nmode='windowed'\n[graphics]\nvsync=false\nlowLatencyPresent=false\nlatencyPacing=false\nframesInFlight=2\nimguiViewports=false\nrenderScale=1.0\nautoExposure=false\nfilmGrain=0.0\nmotionBlur=0.0\n"
 (project/'ProjectSettings.toml').write_text(settings)
 (profile/'UserSettings.toml').write_text(settings)
 with socket.socket(type=socket.SOCK_DGRAM) as reserved:reserved.bind(('127.0.0.1',port))
 env=os.environ.copy();env.update(AETHER_AGENT_SESSION='1',AETHER_USER_CONFIG_DIR=str(profile),AETHER_SESSION_NAME='Verification - '+tag,AETHER_FRAME_REPORT='2',AETHER_CONTROL_PORT=str(port),AETHER_PROJECT_DIR=str(project))
 exe=build/'src/app/RelWithDebInfo'/('Editor.exe' if mode=='editor' else 'AetherGame.exe')
 ini=build/'imgui.ini';old_ini=ini.read_bytes() if ini.exists() else None
 if mode=='editor':ini.write_bytes(b'')
 staged=build/'src/app/RelWithDebInfo/data/scripts/managed';saved={}
 for source in (case/'managed').glob('AetherGame.*'):
  target=staged/source.name;saved[target]=target.read_bytes() if target.exists() else None
  shutil.copyfile(source,target)
 logpath=folder/'native.log';start=time.monotonic();rows=[]
 with logpath.open('w') as log:
  process=subprocess.Popen([str(exe),'--project',str(project),'--window-center','-1500,800','--validation' if validation else '--no-validation'],cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
  try:
   ready=False
   while time.monotonic()-start<seconds+90:
    assert process.poll() is None,f'{tag} exited: '+str(logpath)
    text=logpath.read_text(errors='replace')
    if re.search(r"Startup scene .* loaded",text) and not ready:
     ready=True;start=time.monotonic()
    if ready and time.monotonic()-start>=seconds:break
    time.sleep(.25)
   assert ready,'Startup scene was not loaded: '+str(logpath)
   text=logpath.read_text(errors='replace');rows=reports(text)
   assert rows, 'No frame reports: '+str(logpath)
   if not validation:assert rows[-1]['n']>=600,'Insufficient warm frame history: '+str(rows[-1])
   assert 'TargetFPS=60' in text and 'VSync=off' in text,'Mismatched resolved settings'
   if validation:assert 'Vulkan validation layer enabled (sync validation + best practices)' in text,'Validation was not enabled'
   findings=[line for line in text.splitlines() if re.search(r'\bERRO(?:R)?\b|VUID-|Validation Error|VK_ERROR_DEVICE_LOST',line)]
   assert not findings,findings[:8]
   assert not re.search(r'ModelSpawn: failed|Scene load:.*failed|Shader.*failed|No startup scene',text),'Missing scene content'
   minimum=1 if scene=='Empty' else 400 if scene=='Motion' else 30
   counts=re.findall(r'Scene apply: (\d+) entities',text);assert counts and int(counts[-1])>=minimum,'Incorrect scene entity count'
   assert rows[-1]['focusflips']==0,'Focus changed in the measured sample'
   if scene=='Motion' or scene.startswith('Reload'):assert 'Loaded 2 C# script type(s)' in text,'Fixture scripts were not loaded'
   if scene.startswith('Reload'):assert text.count('Scene.Load: switched')>=2,'Reload workload did not execute'
   if mode=='editor':
    cli=repo/'build/default/tools/control-client/RelWithDebInfo/aether-ctl.exe'
    # Sampling ended before these queries/readbacks. Retain only the pre-query rows.
    def call(method,args={}):
     p=subprocess.run([str(cli),'--port',str(port),method,json.dumps(args)],capture_output=True,text=True,timeout=25)
     return json.loads(p.stdout)
    info=call('info');assert info['sessionName']==env['AETHER_SESSION_NAME']
    assert not info['idleThrottled'],'Idle throttling contaminated the sample'
    (folder/'info.json').write_text(json.dumps(info,indent=2))
    (folder/'memory.json').write_text(json.dumps(call('render.memory',{'limit':0}),indent=2))
    if scene=='Static' and index in (0,1):
     capture=call('render.capture_texture',{'name':'PostProcess.FinalColor','path':(folder/'scene.png').as_posix()})
     (folder/'capture.json').write_text(json.dumps(capture,indent=2))
     assert 'error' not in capture,capture
     deadline=time.monotonic()+12
     while not (folder/'scene.png').exists() and time.monotonic()<deadline:time.sleep(.1)
     assert (folder/'scene.png').exists(),capture
   result=dict(entityCount=int(counts[-1]),label=label,mode=mode,scene=scene,validation=validation,seconds=seconds,binarySha256=hashlib.sha256(exe.read_bytes()).hexdigest(),reports=rows,last=rows[-1],errors=findings)
   (folder/'result.json').write_text(json.dumps(result,indent=2));return result
  finally:
   if process.poll() is None:process.terminate()
   process.wait(timeout=20)
   for target,contents in saved.items():
    if contents is None:target.unlink(missing_ok=True)
    else:target.write_bytes(contents)
   if mode=='editor':
    if old_ini is None:ini.unlink(missing_ok=True)
    else:ini.write_bytes(old_ini)

def main():
 p=argparse.ArgumentParser();p.add_argument('--baseline-build',type=Path,required=True);p.add_argument('--candidate-build',type=Path,required=True);p.add_argument('--seconds',type=float,default=28);p.add_argument('--port',type=int,default=9064);p.add_argument('--validation',action='store_true');p.add_argument('--scenes',nargs='+',default=['Empty','Static','Motion']);p.add_argument('--modes',nargs='+',default=['runtime','editor']);a=p.parse_args()
 repo=Path(__file__).resolve().parents[3];case=repo/'artifacts/static-scene-pacing'/str(uuid.uuid4());case.mkdir(parents=True)
 project=prepare(repo,case);results=[];gates=[]
 builds={'baseline':a.baseline_build.resolve(),'candidate':a.candidate_build.resolve()}
 for mode in a.modes:
  for scene in a.scenes:
   for index,label in enumerate(['baseline','candidate','candidate','baseline'] if not a.validation else ['baseline','candidate']):
    print('RUN',mode,scene,label,index,flush=True)
    result=run(repo,case,project,builds[label],label,scene,mode,index,a.seconds,a.port,a.validation);results.append(result)
    (case/'results.json').write_text(json.dumps(results,indent=2))
   if not a.validation:
    subset=[x for x in results if x['mode']==mode and x['scene']==scene]
    gate=assess([x['last'] for x in subset if x['label']=='baseline'],[x['last'] for x in subset if x['label']=='candidate']);gates.append(dict(mode=mode,scene=scene,metrics=gate));print('GATE',json.dumps(gate),flush=True)
 summary=dict(validation=a.validation,results=results,gates=gates,passed=all(m['passed'] for g in gates for m in g['metrics'].values()),limits=['Unfocused agent windows, explicitly capped at 60 FPS with VSync and idle throttling disabled; this measures loop pacing, not foreground presentation latency.','Each reported percentile is the last rolling 600-frame window; ABBA replicates are compared by median.','Other live editors and compilation/system load must be recorded externally.'])
 (case/'summary.json').write_text(json.dumps(summary,indent=2));print('STATIC_SCENE_RESULT',summary['passed'],case,flush=True)
 assert summary['passed'],'Frame-flow regression gate failed; inspect '+str(case)

if __name__=='__main__':main()

