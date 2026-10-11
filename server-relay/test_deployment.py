"""Actual current deployment operation tests with explicit Docker substitutes."""
import importlib.util,json,os,tempfile,unittest
from pathlib import Path
from unittest import mock
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('route1_deployment',ROOT/'deploy/manage.py');manage=importlib.util.module_from_spec(spec);spec.loader.exec_module(manage)
OLD='sha256:'+'1'*64;NEW='sha256:'+'2'*64
class FakeDeployment(manage.Deployment):
 def __init__(self,values,path):
  super().__init__(values,path);self.events=[];self.closed=False;self.stopped=False;self.fail=False;self.busy=False;self.schema_ok=True;self.recovery_ok=True;self.active=OLD
 def validate(self):self.events.append('validate');return {'validated':True}
 def image_check(self,image,pull=False):self.events.append(('image',image));return image
 def require_running_image(self,image):self.events.append(('expected',image))
 def control(self,op):
  self.events.append(('control',op))
  if op=='status':return {'ok':True,'instance_uuid':'synthetic-same-instance','configured':True,'cleanup_ok':True,'worker_available':True,'opus_available':True,'maintenance':False,'ready':True,'reason':'closed' if self.closed else None,'safe_to_stop':True}
  if op=='drain':return {'ok':True,'accepting_new':False}
  if op=='drain-status':return {'ok':True,'draining':True,'safe_to_stop':not self.busy,'active_sessions':int(self.busy),'active_jobs':0,'active_requests':0}
  if op=='resume':self.busy=False;return {'ok':True}
  if op=='close':self.closed=True;return {'ok':True,'closed':True}
  raise AssertionError(op)
 def running(self,values=None):return not self.stopped
 def compose(self,*args,values=None,json_output=False):
  values=values or self.values;self.events.append(('compose',args,values.copy()))
  if args[0]=='stop':self.stopped=True
  if args[0]=='up':
   self.active=values['VOICE_MEMO_IMAGE'];self.stopped=False;self.closed=False
   if self.fail and self.active==NEW:raise manage.DeploymentError('synthetic failed candidate startup')
  return ''
 def storage_check(self,image):self.events.append(('read-current',image));return {'compatible':self.schema_ok if image==NEW else self.recovery_ok}

class DeploymentTests(unittest.TestCase):
 def setUp(self):
  parent=ROOT/'private/deployment-tests';parent.mkdir(parents=True,exist_ok=True,mode=0o700);self.temp=tempfile.TemporaryDirectory(dir=parent);self.addCleanup(self.temp.cleanup);self.env=Path(self.temp.name)/'relay.env';self.values={'VOICE_MEMO_IMAGE':OLD,'VOICE_MEMO_PUBLIC_URL':'https://relay.example.invalid/memo','VOICE_MEMO_PROJECT':'synthetic-route1','VOICE_MEMO_DRAIN_TIMEOUT':'60'};manage.atomic_json_or_env(self.env,self.values,env_format=True)
 def test_normal_update_uses_live_drain_and_same_current_database(self):
  d=FakeDeployment(self.values,self.env);result=d.switch(NEW);self.assertFalse(result['database_restored']);self.assertEqual(d.active,NEW);self.assertEqual(manage.read_env(self.env)['VOICE_MEMO_IMAGE'],NEW)
  self.assertLess(d.events.index(('control','close')),d.events.index(('read-current',NEW)))
  self.assertFalse(any('qualification' in str(event) for event in d.events))
 def test_candidate_failure_recovers_old_code_without_restoring_database(self):
  d=FakeDeployment(self.values,self.env);d.fail=True
  with self.assertRaisesRegex(manage.DeploymentError,'previous image restored'):d.switch(NEW)
  self.assertEqual(d.active,OLD);self.assertEqual(manage.read_env(self.env)['VOICE_MEMO_IMAGE'],OLD);self.assertIn(('read-current',OLD),d.events)
  self.assertGreater([x for x in d.events if x[:2]==('control','drain')].__len__(),1)
 def test_unreadable_recovery_leaves_data_and_services_stopped(self):
  d=FakeDeployment(self.values,self.env);d.fail=True;d.recovery_ok=False
  with self.assertRaisesRegex(manage.DeploymentError,'cannot read current data'):d.switch(NEW)
  self.assertTrue(d.stopped);self.assertEqual(manage.read_env(self.env)['VOICE_MEMO_IMAGE'],NEW)
 def test_unreadable_candidate_is_not_started_and_old_code_returns(self):
  d=FakeDeployment(self.values,self.env);d.schema_ok=False
  with self.assertRaises(manage.DeploymentError):d.switch(NEW)
  self.assertEqual(d.active,OLD)
  self.assertFalse(any(x[0]=='compose' and x[1][0]=='up' and x[2]['VOICE_MEMO_IMAGE']==NEW for x in d.events if isinstance(x,tuple)))
 def test_busy_timeout_resumes_old_without_closing_or_switching(self):
  d=FakeDeployment(self.values,self.env);d.busy=True
  with mock.patch.object(manage.time,'monotonic',side_effect=[0,61]),self.assertRaisesRegex(manage.DeploymentError,'timed out'):d.switch(NEW)
  self.assertIn(('control','resume'),d.events);self.assertNotIn(('control','close'),d.events);self.assertEqual(d.active,OLD)
 def test_closed_runtime_start_recreates_only_after_safe_stop(self):
  d=FakeDeployment(self.values,self.env);d.closed=True;d.start();self.assertTrue(any(x[0]=='compose' and '--force-recreate' in x[1] for x in d.events if isinstance(x,tuple)));self.assertFalse(d.closed)
 def test_env_has_only_metadata_defaults_and_supports_encoded_or_utf8_paths(self):
  self.assertEqual(manage.read_env(self.env),self.values)
  values={'VOICE_MEMO_IMAGE':OLD,'VOICE_MEMO_PUBLIC_URL':'https://192.0.2.1:9443/入口'};manage.validate_values(values)
  for extra in ['API_KEY=secret','VOICE_MEMO_LEGACY_UPLOAD_TOKEN_SHA256='+'1'*64,'VOICE_MEMO_IMAGE=$(touch unsafe)']:
   self.env.write_text('VOICE_MEMO_IMAGE='+OLD+'\nVOICE_MEMO_PUBLIC_URL=https://relay.example.invalid\n'+extra+'\n')
   with self.assertRaises(manage.DeploymentError):manage.read_env(self.env)
 def test_compose_and_image_defaults_are_actual_safe_template(self):
  value=json.loads((ROOT/'deploy/compose.yaml').read_text());relay=value['services']['relay'];broker=value['services']['config-broker'];init=value['services']['volume-init']
  self.assertEqual(relay['user'],'10001:10001');self.assertEqual(broker['network_mode'],'none');self.assertEqual(init['network_mode'],'none');self.assertTrue(relay['read_only']);self.assertNotIn('secrets:/var/lib/voice-memo-secrets',relay['volumes']);self.assertIn('data:/var/lib/voice-memo',relay['volumes'])
  self.assertNotIn('VOICE_MEMO_LEGACY_UPLOAD_TOKEN_SHA256',relay['environment']);self.assertNotIn('VOICE_MEMO_START_DRAINED',relay['environment']);self.assertTrue(relay['ports'][0].startswith('127.0.0.1:'))
 def test_current_schema_checker_reads_stopped_real_runtime_and_rejects_unknown(self):
  from test_support import LocalBroker
  from relay.runtime import Runtime
  import sqlite3
  spec=importlib.util.spec_from_file_location('route1_storage_check',ROOT/'deploy/storage-check.py');reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
  folder=Path(self.temp.name)/'actual';folder.mkdir();(folder/'secrets').mkdir(mode=0o700)
  broker=LocalBroker(folder/'secrets');runtime=Runtime(folder/'relay.db',broker,folder/'work',folder/'streams',start_workers=False,min_free_bytes=0)
  runtime.close();broker.close();path=folder/'relay.db';before=path.read_bytes();self.assertTrue(reader.check(path));self.assertEqual(path.read_bytes(),before)
  with sqlite3.connect(path) as db:db.execute('UPDATE relay_runtime_meta SET schema_version=999')
  self.assertFalse(reader.check(path))
  path.write_bytes(b'not-a-sqlite-database')
  with self.assertRaises(sqlite3.DatabaseError):reader.check(path)

 def test_ready_checks_use_actual_runtime_and_allow_only_normal_unconfigured(self):
  from test_support import LocalBroker
  from relay.runtime import Runtime
  folder=Path(self.temp.name)/'readiness';folder.mkdir();(folder/'secrets').mkdir(mode=0o700)
  broker=LocalBroker(folder/'secrets');runtime=Runtime(folder/'relay.db',broker,folder/'work',folder/'streams',start_workers=False,min_free_bytes=0)
  try:
   class Probe(manage.Deployment):
    def control(self,op):return runtime.readiness()
   probe=Probe(self.values,self.env);initial=runtime.readiness();probe.verify_service(initial)
   broker.call('save_activate',expected_revision=None,patch={'qwen_api_key':{'op':'replace','value':'synthetic-key'},'siyuan_token':{'op':'replace','value':'synthetic-token'}})
   before=runtime.readiness();probe.verify_service(before);runtime.streams.opus=None
   with self.assertRaises(manage.DeploymentError):probe.verify_service(before)
  finally:runtime.close();broker.close()
 def test_committed_wal_is_checked_without_touching_source_or_accepting_unknown(self):
  import sqlite3,subprocess,sys,hashlib
  from test_support import LocalBroker
  from relay.runtime import Runtime
  spec=importlib.util.spec_from_file_location('route1_wal_reader',ROOT/'deploy/storage-check.py');reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
  folder=Path(self.temp.name)/'wal';folder.mkdir();(folder/'secrets').mkdir(mode=0o700);path=folder/'relay.db'
  broker=LocalBroker(folder/'secrets');runtime=Runtime(path,broker,folder/'work',folder/'streams',start_workers=False,min_free_bytes=0);runtime.close()
  try:
   code='import sqlite3,os,sys;c=sqlite3.connect(sys.argv[1]);c.execute("PRAGMA journal_mode=WAL");c.execute("PRAGMA wal_autocheckpoint=0");c.execute("CREATE TABLE deployment_marker(value TEXT)");c.execute("INSERT INTO deployment_marker VALUES(?)",("committed-before-crash",));c.commit();os._exit(7)'
   self.assertEqual(subprocess.run([sys.executable,'-c',code,str(path)]).returncode,7)
   wal=path.with_name(path.name+'-wal');self.assertGreater(wal.stat().st_size,0)
   before={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob('relay.db*')}
   self.assertTrue(reader.check(path));self.assertEqual(before,{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob('relay.db*')})
   code='import sqlite3,os,sys;c=sqlite3.connect(sys.argv[1]);c.execute("UPDATE relay_runtime_meta SET schema_version=999");c.commit();os._exit(7)'
   self.assertEqual(subprocess.run([sys.executable,'-c',code,str(path)]).returncode,7)
   self.assertFalse(reader.check(path))
  finally:broker.close()

 def test_init_nonempty_permissions_are_not_repaired(self):
  spec=importlib.util.spec_from_file_location('route1_volume_init',ROOT/'deploy/init-volumes.py');init=importlib.util.module_from_spec(spec);spec.loader.exec_module(init)
  folder=Path(self.temp.name)/'mount';folder.mkdir();(folder/'existing').write_text('synthetic');init.MOUNTS=((str(folder),10001,10001,0o700),)
  with mock.patch.object(init.os,'geteuid',return_value=0),self.assertRaises(RuntimeError):init.initialize()
  self.assertEqual((folder/'existing').read_text(),'synthetic')

if __name__=='__main__':unittest.main(verbosity=2)
