"""Actual AP handler/callback/NVS behavior with explicit platform shims."""
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
    def call(self,route='/service',body=None,**extra):
        return self.native.call({'route':route,'method':'POST' if body is not None else 'GET','body':body if isinstance(body,str) else json.dumps(body) if body is not None else '',**extra})
    def csrf(self):return self.call()[1]['csrf']
    def test_redacted_fresh_state_and_request_guards(self):
        status,state=self.call();self.assertEqual(status,200);self.assertTrue(state['local_only']);self.assertFalse(state['configured']);
        self.assertEqual(set(state),{'success','server_url','local_only','configured','pairing','connected','message','csrf'})
        body={'server_url':'https://one.example','pairing_code':'23456789AB','local_only':False}
        for overrides in [{},{'csrf':'wrong'},{'csrf':state['csrf'],'host':'evil.example'},{'csrf':state['csrf'],'origin':'http://evil.example'},{'csrf':state['csrf'],'type':'text/plain'}]:
            self.assertEqual(self.call(body=body,**overrides)[0],403)
        self.assertEqual(self.call(host='evil.example')[0],403)
        self.assertEqual(self.call(body=body,csrf=state['csrf'])[0],200)
    def test_invalid_input_never_replaces_saved_target(self):
        csrf=self.csrf();good={'server_url':'https://one.example:9443/memo','pairing_code':'23456789AB','local_only':False}
        self.assertEqual(self.call(body=good,csrf=csrf)[0],200)
        for body in ['{','{"server_url":"https://evil.example\\u0000junk","pairing_code":"23456789AB","local_only":false}',json.dumps({**good,'local_only':'false'}),json.dumps({**good,'server_url':'http://invalid.example'}),'['*17+']'*17,'{} extra']:
            self.assertEqual(self.call(body=body,csrf=csrf)[0],409)
            self.assertEqual(self.call()[1]['server_url'],good['server_url'])
        self.assertEqual(self.call(body=' '*1025,csrf=csrf)[0],400)
    def test_busy_failure_uncertain_save_retry_and_noop(self):
        csrf=self.csrf();body={'server_url':'https://one.example','pairing_code':'23456789AB','local_only':False}
        self.native.call({'route':'/__control','busy':True})
        self.assertEqual(self.call(body=body,csrf=csrf)[0],409)
        self.native.call({'route':'/__control','fault':4})
        status,state=self.call(body=body,csrf=csrf);self.assertEqual(status,409);self.assertIn('未确认',state['error'])
        self.native.call({'route':'/__control'})
        self.assertEqual(self.call(body=body,csrf=csrf)[0],200)
        self.assertEqual(self.call(body=body,csrf=csrf)[0],200)
        self.assertTrue(self.call()[1]['pairing']);self.assertFalse(self.call()[1]['configured'])
        self.native.call({'route':'/__control','paired':True});self.assertTrue(self.call()[1]['configured'])
        self.assertEqual(self.call(body={**body,'pairing_code':'','local_only':True},csrf=csrf)[0],200)
        self.assertTrue(self.call()[1]['local_only'])
    def test_wifi_separate_save_and_session_protection(self):
        csrf=self.csrf();body={'ssid':'Home <&>','password':'synthetic-only'}
        self.assertEqual(self.call('/submit',body)[0],403)
        self.assertEqual(self.call('/submit',body,csrf=csrf)[1],{'success':True})
        self.assertTrue(self.call()[1]['local_only'])
        self.assertFalse(self.call('/submit',{'ssid':'','password':''},csrf=csrf)[1]['success'])
        self.assertFalse(self.call('/submit',{'ssid':'x','password':{}},csrf=csrf)[1]['success'])
        self.assertEqual(self.call('/exit',{})[0],403)
        self.assertEqual(self.call('/exit',{},csrf=csrf)[1],{'success':True})

if __name__=='__main__':unittest.main(verbosity=2)
