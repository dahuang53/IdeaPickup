"""Production AP guards, redacted direct configuration and real NVS adapter."""
import json,tempfile,unittest
from pathlib import Path
from service_portal_fixture import ROOT,build,Native
class PortalTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='portal-',dir=ROOT/'private/host-tests');cls.binary=build(Path(cls.temp.name))
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def setUp(self):self.native=Native(self.binary)
 def tearDown(self):self.native.close()
 def call(self,route='/service',body=None,**extra):return self.native.call({'route':route,'method':'POST' if body is not None else 'GET','body':body if isinstance(body,str) else json.dumps(body) if body is not None else '',**extra})
 def state(self):return self.call()[1]
 def patch(self,**extra):
  d=self.state();return {'local_only':d['local_only'],'qwen_generation':d['qwen_generation'],'siyuan_generation':d['siyuan_generation'],**extra}
 def credentials(self):self.native.call({'route':'/__control','credentials':True})
 def test_flags_only_and_no_false_cloud_verification(self):
  d=self.state();self.assertTrue(d['local_only']);self.assertFalse(d['configured']);self.assertEqual(d['phase'],'local');self.assertNotIn('server_url',d);self.assertNotIn('pairing',d)
  self.credentials();d=self.state();self.assertTrue(d['configured']);self.assertTrue(d['local_only']);self.assertEqual(d['task_state'],0);self.assertIn('尚未验证',d['message']);self.assertNotIn('synthetic',json.dumps(d));
  self.assertEqual(self.call(body=self.patch(local_only=False),csrf=d['csrf'])[0],200);self.assertEqual(self.state()['phase'],'ready');self.assertEqual(self.state()['task_state'],0)
 def test_origin_session_and_type_guards(self):
  d=self.state();body=self.patch()
  for over in [{},{'csrf':'wrong'},{'csrf':d['csrf'],'host':'evil.example'},{'csrf':d['csrf'],'origin':'http://evil.example'},{'csrf':d['csrf'],'type':'text/plain'}]:self.assertEqual(self.call(body=body,**over)[0],403)
  self.assertEqual(self.call(host='evil.example')[0],403)
 def test_legacy_secret_unknown_duplicate_and_invalid_fields_rejected(self):
  d=self.state();csrf=d['csrf'];good=self.patch()
  for body in [{**good,'server_url':'https://old.example'},{**good,'qwen_key':False},{**good,'local_only':'false'},{**good,'qwen_generation':-1},{**good,'siyuan_generation':1.5},'{','{"local_only":true,"local_only":false,"qwen_generation":0,"siyuan_generation":0}',json.dumps(good)+' extra',{'local_only':True}]:self.assertEqual(self.call(body=body,csrf=csrf)[0],409)
  self.assertEqual(self.call(body=' '*1025,csrf=csrf)[0],400);self.assertFalse(self.state()['configured'])
 def test_credentials_save_replace_and_remain_redacted(self):
  d=self.state();initial=self.patch(qwen_key='fixture-web-key',siyuan_token='fixture-web-tok');initial.pop('local_only')
  self.assertEqual(self.call(body=initial)[0],403)
  status,response=self.call(body=initial,csrf=d['csrf']);self.assertEqual(status,200)
  current=self.state();self.assertTrue(current['configured']);self.assertTrue(current['local_only'])
  self.assertNotIn('fixture-web',json.dumps([response,current]))
  self.assertEqual(self.call(body=initial,csrf=d['csrf'])[0],409)
  self.assertEqual(self.call(body=self.patch(local_only=False),csrf=d['csrf'])[0],200)
  replacement=self.patch(qwen_key='fixture-replacement',siyuan_token='');replacement.pop('local_only')
  self.assertEqual(self.call(body=replacement,csrf=d['csrf'])[0],200)
  updated=self.state();self.assertFalse(updated['local_only']);self.assertEqual(updated['qwen_generation'],current['qwen_generation']+1)
  self.assertEqual(updated['siyuan_generation'],current['siyuan_generation'])
 def test_clear_is_explicit_and_generation_conflicts_keep_current(self):
  self.credentials();d=self.state();old=self.patch();self.assertEqual(self.call(body=self.patch(clear_qwen=True),csrf=d['csrf'])[0],200);current=self.state();self.assertFalse(current['qwen_set']);self.assertTrue(current['siyuan_set']);self.assertTrue(current['local_only']);self.assertEqual(current['qwen_generation'],2)
  self.assertEqual(self.call(body=old,csrf=d['csrf'])[0],409);self.assertEqual(self.state()['qwen_generation'],2)
 def test_busy_and_uncertain_storage_do_not_claim_success(self):
  d=self.state();body=self.patch();self.native.call({'route':'/__control','busy':True});self.assertEqual(self.call(body=body,csrf=d['csrf'])[0],409)
  self.native.call({'route':'/__control'});self.credentials();body=self.patch(local_only=False);self.native.call({'route':'/__control','fault':4});status,result=self.call(body=body,csrf=d['csrf']);self.assertEqual(status,409);self.assertIn('未确认',result['error']);self.native.call({'route':'/__control'});self.assertEqual(self.state()['phase'],'ready')
 def test_not_configured_mode_does_not_trigger_business(self):
  d=self.state();status,result=self.call(body=self.patch(local_only=False),csrf=d['csrf']);self.assertEqual(status,409);self.assertIn('凭据',result['error']);self.assertEqual(self.state()['task_state'],0)
 def test_task_results_are_separate_from_saved_credentials(self):
  self.credentials();d=self.state();self.call(body=self.patch(local_only=False),csrf=d['csrf']);self.native.call({'route':'/__control','task_state':9});self.assertEqual(self.state()['task_state'],9);self.assertEqual(self.state()['phase'],'ready')
 def test_wifi_and_exit_keep_original_request_boundaries(self):
  d=self.state();self.assertEqual(self.call('/submit',{'ssid':'fixture','password':'synthetic'},csrf=d['csrf'])[0],202);self.native.call({'route':'/__control','wifi_process':True});self.assertTrue(self.state()['wifi']['saved']);self.assertEqual(self.call('/exit',{})[0],403);self.assertTrue(self.call('/exit',{},csrf=d['csrf'])[1]['success'])
if __name__=='__main__':unittest.main(verbosity=2)
