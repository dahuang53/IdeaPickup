"""Loopback fixture runs production AP handlers, portal adapter and R03 NVS body.
RF/ESP httpd/NVS/task APIs are explicit shims. No hardware or providers.
Browser loopback Host/Origin maps to the fixed AP address only in this fixture.
"""
from pathlib import Path
import json, subprocess, tempfile, threading, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from test_service_runtime import ROOT, HEADERS, HARNESS, extract


def build(temp):
    for name,text in HEADERS.items():
        p=temp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    source=(ROOT/'main/network_sync.cpp').read_text()
    markers=['class ConfigGuard','class ConfigCommitGuard','bool current_service','void status_update','void refresh_service_status',
        'extern "C" void network_sync_get_status','extern "C" void network_sync_get_service_config',
        'extern "C" memo_config_result_t network_sync_configure_service',
        'extern "C" bool network_sync_prepare_audio','extern "C" void network_sync_audio_done']
    bodies='\n'.join(extract(source,m)+(';' if m.startswith('class ') else '') for m in markers)
    ap=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
    bodies+='\n'+extract(ap,'bool WifiConfigurationAp::Save')
    bodies+='\n'+extract((ROOT/'components/esp-wifi-connect/wifi_manager.cc').read_text(),'bool WifiManager::Initialize')
    h=HARNESS[:HARNESS.index('int main(int argc')]
    h=h.replace('public: bool Save(', 'public: std::function<void()> on_exit_requested_; bool ConnectToWifi(const std::string &,const std::string &){return !wifi_connect_fault;} bool Save(')
    h=h.replace('struct wifi_ap_record_t', 'static bool wifi_connect_fault=false;\nstruct wifi_ap_record_t')
    h=h.replace('// __PRODUCTION_BODIES__',bodies)
    extra=r'''
#include <cJSON.h>
#include "memo_service_portal.h"
extern "C" bool memo_portal_status(char *,size_t);
extern "C" bool memo_portal_apply(const char *,size_t,char *,size_t);
struct httpd_req_t {void *user_ctx=nullptr;size_t content_len=0,at=0;std::string body,response;std::map<std::string,std::string> headers;int status=200;};
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_SOCK_ERR_TIMEOUT -2
int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t size){auto i=r->headers.find(name);if(i==r->headers.end()||i->second.size()>=size)return ESP_FAIL;strcpy(out,i->second.c_str());return ESP_OK;}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name){return r->headers.count(name)?r->headers[name].size():0;}
int httpd_resp_send(httpd_req_t *r,const char *text,int size){r->response=text?std::string(text,size<0?strlen(text):size):"";return ESP_OK;}
int httpd_resp_sendstr(httpd_req_t *r,const char *text){return httpd_resp_send(r,text,-1);}
int httpd_resp_send_err(httpd_req_t *r,int status,const char *text){r->status=status;cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"success",false);cJSON_AddStringToObject(j,"error",text);char *s=cJSON_PrintUnformatted(j);httpd_resp_sendstr(r,s);cJSON_free(s);cJSON_Delete(j);return ESP_FAIL;}
int httpd_resp_send_408(httpd_req_t *r){return httpd_resp_send_err(r,408,"Receive timeout");}
void httpd_resp_set_type(httpd_req_t *,const char *){}
void httpd_resp_set_hdr(httpd_req_t *,const char *,const char *){}
void httpd_resp_set_status(httpd_req_t *r,const char *s){r->status=atoi(s);}
int httpd_req_recv(httpd_req_t *r,char *out,size_t size){size_t n=std::min(size,std::min(size_t(7),r->body.size()-r->at));memcpy(out,r->body.data()+r->at,n);r->at+=n;return (int)n;}
void esp_wifi_disconnect(){}
void vTaskDelete(void *){}
void xTaskCreate(void (*fn)(void *),const char *,int,void *ctx,int,void *){fn(ctx);}
static char portal_csrf[33]="0123456789abcdef0123456789abcdef";
static memo_service_status_fn service_status=memo_portal_status;
static memo_service_apply_fn service_apply=memo_portal_apply;
// __ORIGIN__
static auto service_get=// __GET__
;
static auto service_post=// __POST__
;
static auto wifi_post=// __WIFI__
;
static auto exit_post=// __EXIT__
;
int main(){
    assert(nvs_flash_init()==ESP_OK);
    SsidManager::GetInstance().SetConfigCommitGuard(// __BEGIN__
    ,// __END__
    );
    assert(memo_network_config_nvs_init()==ESP_OK);
    snprintf(status_value.device_id,sizeof(status_value.device_id),"fixture-device");
    status_value.provisioning=true;
    WifiConfigurationAp ap;
    std::string line;
    while(std::getline(std::cin,line)){
        cJSON *j=cJSON_Parse(line.c_str());assert(j);
        auto text=[&](const char *key,const char *fallback){auto *v=cJSON_GetObjectItem(j,key);return cJSON_IsString(v)?v->valuestring:fallback;};
        const char *route=text("route","/service"),*method=text("method","GET");
        httpd_req_t req;req.user_ctx=&ap;req.body=text("body","");req.content_len=req.body.size();
        req.headers={{"Host",text("host","192.168.4.1")},{"Origin",text("origin","http://192.168.4.1")},{"Content-Type",text("type","application/json")},{"X-Memo-CSRF",text("csrf","")}};
        if(!strcmp(route,"/__control")){
            auto *v=cJSON_GetObjectItem(j,"busy");unique_audio=cJSON_IsTrue(v);
            fault_namespace=1;v=cJSON_GetObjectItem(j,"fault");fault=cJSON_IsNumber(v)?v->valueint:0;after_commit=false;
            wifi_connect_fault=cJSON_IsTrue(cJSON_GetObjectItem(j,"wifi_fail"));
            if(cJSON_IsTrue(cJSON_GetObjectItem(j,"paired"))){ConfigCommitGuard commit;ConfigGuard guard;auto generation=guard.value->value.generation;memo_server_uuid_t id{};id.bytes[0]=1;memo_config_bind_instance(guard.value,generation,&id);memo_config_finish_pairing(guard.value,guard.value->value.generation,&id);refresh_service_status();}
            req.response="{\"success\":true}";
        }else if(!strcmp(route,"/service")) {if(!strcmp(method,"POST"))service_post(&req);else service_get(&req);}
        else if(!strcmp(route,"/submit"))wifi_post(&req);
        else if(!strcmp(route,"/exit"))exit_post(&req);
        else httpd_resp_send_err(&req,404,"No route");
        cJSON_Delete(j);j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"status",req.status);cJSON_AddStringToObject(j,"body",req.response.c_str());char *out=cJSON_PrintUnformatted(j);std::cout<<out<<std::endl;cJSON_free(out);cJSON_Delete(j);
    }
}
'''
    def handler(uri):
        at=ap.index('.uri'+('="' if uri=='/service' else ' = "')+uri+'"')
        if uri=='/service' and handler.post:
            at=ap.index('.uri="/service"',at+1)
        start=ap.index('[](httpd_req_t *req)',at)
        return extract(ap[start:],'[](httpd_req_t *req)')
    handler.post=False;extra=extra.replace('// __GET__',handler('/service'))
    handler.post=True;extra=extra.replace('// __POST__',handler('/service'))
    extra=extra.replace('// __WIFI__',handler('/submit')).replace('// __EXIT__',handler('/exit'))
    extra=extra.replace('// __ORIGIN__',extract(ap,'bool portal_origin'))
    for marker,source_marker in [('BEGIN','config.config_commit_begin='),('END','config.config_commit_end=')]:
        extra=extra.replace('// __'+marker+'__',extract(source,source_marker).split('=',1)[1])
    p=temp/'fixture.cpp';p.write_text('#include <iostream>\n'+h+extra)
    includes=['-I',str(temp),'-I',str(ROOT/'main'),'-I',str(ROOT/'components/esp-wifi-connect/include'),'-I',str(ROOT/'.tools/esp-idf/components/json/cJSON')]
    objects=[]
    for source in [ROOT/'main/memo_network_config.c',ROOT/'main/memo_network_config_nvs.c',ROOT/'.tools/esp-idf/components/json/cJSON/cJSON.c']:
        obj=temp/(source.stem+'.o');objects.append(str(obj));subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',*includes,'-c',str(source),'-o',str(obj)],check=True)
    binary=temp/'fixture';subprocess.run(['c++','-std=c++17','-g','-O1','-Wall','-Wextra','-Werror','-DMEMO_HOST_TEST',*includes,str(p),str(ROOT/'main/memo_service_portal.cpp'),str(ROOT/'components/esp-wifi-connect/ssid_manager.cc'),*objects,'-o',str(binary)],check=True)
    return binary

class Native:
    def __init__(self,binary):
        self.process=subprocess.Popen([str(binary)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True);self.lock=threading.Lock()
    def call(self,data):
        with self.lock:
            self.process.stdin.write(json.dumps(data)+'\n');self.process.stdin.flush();line=self.process.stdout.readline()
            if not line:raise RuntimeError('native fixture exited')
            result=json.loads(line);return result['status'],json.loads(result['body'])
    def close(self):self.process.stdin.close();self.process.wait(timeout=5);self.process.stdout.close()

if __name__=='__main__':
    output=Path(sys.argv[1]).resolve();assert output.is_relative_to(ROOT/'private');output.mkdir(parents=True,exist_ok=True)
    native=Native(build(output));
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):self.handle_request()
        def do_POST(self):self.handle_request()
        def handle_request(self):
            if self.path=='/':status=200;body=(ROOT/'components/esp-wifi-connect/assets/wifi_configuration.html').read_bytes();mime='text/html'
            elif self.path=='/scan':status=200;body=json.dumps({'aps':[{'ssid':'Home <&>','rssi':-40},{'ssid':'Home <&>','rssi':-45}]}).encode();mime='application/json'
            else:
                request={'route':self.path,'method':self.command,'body':self.rfile.read(int(self.headers.get('Content-Length','0'))).decode(),'csrf':self.headers.get('X-Memo-CSRF','')}
                if self.path=='/__control':request.update(json.loads(request['body']))
                status,value=native.call(request);body=json.dumps(value).encode();mime='application/json'
            self.send_response(status);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler);(output/'fixture.json').write_text(json.dumps({'url':f'http://127.0.0.1:{server.server_port}/'}));
    try:server.serve_forever()
    finally:native.close()
