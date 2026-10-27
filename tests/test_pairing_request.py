"""Execute the production HTTP adapter with controlled transport interleavings.

ESP transport is a shim; validates request admission, late-result rejection and
handle cleanup. Protocol/server and NVS behavior are covered by their own tests.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_service_runtime import ROOT, extract

class PairingRequestTests(unittest.TestCase):
    def test_current_and_superseded_requests(self):
        body=extract((ROOT/'main/network_sync.cpp').read_text(),'int pairing_request(')
        harness=r'''
#include "memo_network_config.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
static uint64_t generation=1;
static bool enabled=true,change_during_request=false,fail_transport=false;
static unsigned opens=0,closes=0,cleanups=0;
bool pairing_is_current(const memo_config_snapshot_t &p){return enabled && p.generation==generation;}
#define ESP_OK 0
#define ESP_FAIL 1
#define HTTP_METHOD_POST 1
using esp_err_t=int;
using esp_http_client_handle_t=void *;
struct esp_http_client_config_t {const char *url;void (*crt_bundle_attach)();bool disable_auto_redirect;int timeout_ms,buffer_size;};
struct MemoHttpTransport {explicit MemoHttpTransport(esp_http_client_config_t&){} bool ready(){return true;}};
void esp_crt_bundle_attach(){}
extern "C" memo_config_result_t memo_config_join_url(const char *base,const char *path,char *out,size_t capacity){
 return snprintf(out,capacity,"%s%s",base,path)<(int)capacity?MEMO_CONFIG_OK:MEMO_CONFIG_INVALID;
}
void *esp_http_client_init(esp_http_client_config_t *c){assert(c->disable_auto_redirect && c->timeout_ms==15000 && c->crt_bundle_attach);return (void*)1;}
void esp_http_client_set_method(void *,int){}
void esp_http_client_set_header(void *,const char *,const char *){}
int esp_http_client_open(void *,int){++opens;if(change_during_request)++generation;return fail_transport?ESP_FAIL:ESP_OK;}
int esp_http_client_write(void *,const char *,int n){return n;}
int esp_http_client_fetch_headers(void *){return 2;}
int esp_http_client_get_status_code(void *){return 201;}
int read_http_body(void *,std::string &body){body="{}";return ESP_OK;}
void esp_http_client_close(void *){++closes;}
void esp_http_client_cleanup(void *){++cleanups;}
// BODY
int main(){
 memo_config_snapshot_t pending{};pending.generation=1;char response[64]{};
 auto request=[&](){return pairing_request(&pending,"https://synthetic.example","POST","/v1/enrollments","synthetic-token","{}",response,sizeof(response));};
 assert(request()==201 && !strcmp(response,"{}"));
 change_during_request=true;
 assert(request()==0 && response[0]==0);assert(opens==2 && closes==2 && cleanups==2);
 assert(request()==0 && opens==2); // stale snapshot cannot send another request
 pending.generation=generation;change_during_request=false;enabled=false;
 assert(request()==0 && opens==2); // local-only stops subsequent stages
 enabled=true;fail_transport=true;assert(request()==0);assert(opens==3 && closes==3 && cleanups==3);
 // Production sd_retry passes no pending snapshot: it is an authenticated,
 // already-paired request protected by its caller's existing HTTP lock.
 fail_transport=false;enabled=false;
 assert(pairing_request(nullptr,"https://synthetic.example","POST","/v1/recordings/1/retry","synthetic-token","{}",response,sizeof(response))==201);
 assert(opens==4 && closes==4 && cleanups==4 && !strcmp(response,"{}"));
}
'''.replace('// BODY',body)
        with tempfile.TemporaryDirectory(prefix='pairing-request-',dir=ROOT/'private/host-tests') as tmp:
            source=Path(tmp)/'request.cpp';source.write_text(harness);binary=Path(tmp)/'request'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main(verbosity=2)
