"""Native/MCP recovery fixture. Never opens or modifies a user's project or recovery copies."""
import argparse, hashlib, json, os, subprocess, sys, time, uuid, socket
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--port', type=int, required=True)
parser.add_argument('--expect-click-failure', action='store_true')
a=parser.parse_args()
with socket.socket(type=socket.SOCK_DGRAM) as reserved:
 reserved.bind(('127.0.0.1',a.port))
repo=Path(__file__).resolve().parents[3]
build=a.build.resolve();exe=build/'src/app/RelWithDebInfo/Editor.exe'
case=repo/'artifacts/recovery-control'/str(uuid.uuid4())
project=case/'project';saved=project/'scenes/RecoveryFixture.scene.toml'
recovery=project/'.aether/recovery/RecoveryFixture.scene.toml'
saved.parent.mkdir(parents=True);recovery.parent.mkdir(parents=True)
(project/'assets').mkdir();(project/'scripts').mkdir()
(project/'ProjectSettings.toml').write_text("[app]\nstartupscene='RecoveryFixture'\n[project]\nname='Recovery control fixture'\nversion=1\n[window]\nwidth=960\nheight=720\n")
def scene(name):return f"[scene]\nkind='3d'\nname='RecoveryFixture'\nversion=16\n[[entities]]\nname='{name}'\nparent=-1\nposition=[0.0,0.0,0.0]\n"
saved.write_text(scene('Saved'));recovery.write_text(scene('Unsaved'))
os.utime(saved,(time.time()-60,time.time()-60))
def fingerprint():return [(str(p.relative_to(project)),hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_mtime_ns) for p in (saved,recovery)]
before=fingerprint()
env=os.environ.copy();env.update(AETHER_CONTROL_PORT=str(a.port),AETHER_BUILD_DIR=str(build),AETHER_REPO=str(repo),AETHER_CTL=str(build/'tools/control-client/RelWithDebInfo/aether-ctl.exe'),AETHER_AGENT_SESSION='0',AETHER_SESSION_NAME='Verification - recovery '+case.name[:12],AETHER_USER_CONFIG_DIR=str(case/'profile'))
(case/'profile').mkdir()
(case/'profile/EditorState.toml').write_text('[editor]\nwindow_width=960\nwindow_height=720\n')
server=repo/'tools/mcp/aethercore_mcp.py'
def rpc(method,params):
 request=json.dumps(dict(jsonrpc='2.0',id=1,method=method,params=params))+'\n'
 p=subprocess.run([sys.executable,str(server)],input=request,capture_output=True,text=True,env=env,timeout=25)
 assert p.returncode==0,p.stderr
 response=json.loads(p.stdout.splitlines()[-1]);assert 'error' not in response,response
 return response['result']
def tool(name,**args):
 response=rpc('tools/call',dict(name=name,arguments=args));assert not response.get('isError'),response
 value=json.loads(response['content'][0]['text']);assert 'error' not in value,value
 return value
def wait(check,seconds=45):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  try:
   value=check()
   if value:return value
  except (AssertionError,ValueError):pass
  time.sleep(.15)
 raise AssertionError('Native recovery fixture did not reach expected state')
def button():return tool('ui_query',window='Recover Unsaved Work',label='Decide later')['items']
report=dict(project=str(project),before=before,expectClickFailure=a.expect_click_failure,runs=[])
# shortcut: native placement assumes a left-hand monitor; parameterize it for other desktop layouts.
for mode in ['click','defer']:
 with (case/(mode+'.log')).open('w') as log:
  process=subprocess.Popen([str(exe),'--project',str(project),'--window-center','-1500,800','--no-validation'],cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
  try:
   wait(lambda:tool('engine_info').get('sessionName')==env['AETHER_SESSION_NAME'])
   target=wait(button)[0]
   rect=target['windowRect']
   tool('ui_drag',from_x=rect[0]+60,from_y=rect[1]+10,to_x=-2450,to_y=rect[1]+10)
   target=wait(lambda:next((b for b in button() if b['detachedViewport']),None),10)
   assert target['x']<0,target
   info=tool('engine_info')
   listed=tool('recovery_list');assert len(listed['recoverable'])==1,listed
   tools=rpc('tools/list',{});assert any(t['name']=='recovery_defer' for t in tools['tools']),tools
   record=dict(mode=mode,target=target,focused=info['focused'])
   if mode=='click':
    record['click']=tool('ui_click',window=target['window'],label=target['label'])
    if a.expect_click_failure:
     time.sleep(1);assert button(),'Counterfactual unexpectedly succeeded'
     record['recoveryStillOpen']=True
     record['cleanup']=tool('recovery_defer')
    else:wait(lambda:not button(),8)
   else:
    invalid=rpc('tools/call',dict(name='recovery_defer',arguments={'scene':'RecoveryFixture'}))
    assert invalid['isError'] and button(), 'Invalid arguments dismissed the dialog'
    record['invalidArgumentsRejected']=True
    record['defer']=tool('recovery_defer');assert record['defer']==dict(status='deferred',promptOpen=False,filesUntouched=True),record
    wait(lambda:not button(),8)
    record['repeat']=tool('recovery_defer');assert record['repeat']['status']=='not_open',record
   time.sleep(.2);assert not button(),'Deferred prompt reopened'
   assert len(tool('recovery_list')['recoverable'])==1,'Recovery copy stopped being discoverable'
   assert fingerprint()==before,'Scene or recovery copy changed'
   errors=tool('get_console_log',minimumLevel='error',limit=10);assert errors['matched']==0,errors
   report['runs'].append(record)
  finally:
   process.terminate();process.wait(timeout=15)
assert (case/'profile/EditorState.toml').exists(),'Native verification did not use its isolated preferences profile'
assert fingerprint()==before
(case/'report.json').write_text(json.dumps(report,indent=2))
print('RECOVERY_CONTROL_PASS',case,flush=True)
