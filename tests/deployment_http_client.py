"""Synthetic request probe from the proxy IP against an isolated Compose stack.
TLS is represented by the trusted proxy headers here; real certificate tests are R09.
No probe/provider/audio endpoint is called. All inputs are fixed test credentials.
"""
import json,sys,urllib.request
from http.cookies import SimpleCookie
from urllib.parse import urlsplit
value=json.load(sys.stdin);prefix=value.get('prefix','/memo');base='http://relay:8765'+prefix;origin=value['public_url'];cookie='';csrf=''
def call(path,body=None):
 headers={'Host':origin.split('://',1)[1].split('/',1)[0],'X-Forwarded-Proto':'https','Origin':urlsplit(origin).scheme+'://'+urlsplit(origin).netloc}
 if cookie:headers['Cookie']=cookie
 if csrf:headers['X-CSRF-Token']=csrf
 if body is not None:headers['Content-Type']='application/json'
 request=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,headers=headers)
 with urllib.request.urlopen(request,timeout=10) as response:
  data=response.read();return response, json.loads(data) if 'application/json' in response.headers.get('Content-Type','') else data
_,page=call('/admin/');assert b'admin.css' in page
_,style=call('/admin/assets/admin.css');assert style
if value.get('code'):
 call('/admin/api/session/initialize',{'code':value['code'],'password':'Synthetic-password-2026'})
response,session=call('/admin/api/session/login',{'password':'Synthetic-password-2026'});csrf=session['csrf'];cookies=SimpleCookie();cookies.load(response.headers['Set-Cookie']);cookie='; '.join(k+'='+m.value for k,m in cookies.items())
_,view=call('/admin/api/config')
if value.get('save'):
 _,view=call('/admin/api/config',{'expected_revision':view['active_revision'],'patch':{'qwen_api_key':{'op':'replace','value':'synthetic-not-a-real-key'},'siyuan_token':{'op':'replace','value':'synthetic-not-a-real-token'}}})
_,state=call('/admin/api/status');assert state['configured'] and state['cleanup_ok']
print(json.dumps({'admin_html':True,'css':True,'initialized_and_login':True,'configured':state['configured'],'instance_uuid':state['instance_uuid'],'active_revision':view['active_revision'],'secrets_redacted':view['active_config']['qwen_api_key_configured'] and view['active_config']['siyuan_token_configured']}))
