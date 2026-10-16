"""Production manual-portal lifecycle/connection worker with explicit SDK shims.
No radio, DHCP or phone PASS is inferred from this host regression.
"""
import re, subprocess, tempfile, unittest
from pathlib import Path
from test_service_runtime import ROOT, extract

HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define BIT0 1
#define BIT1 2
#define BIT2 4
#define BIT3 8
#define BIT4 16
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define AP_STARTED_BIT BIT2
#define AP_STOPPED_BIT BIT3
#define SCAN_DONE_BIT BIT4
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_EVENT_ANY_ID -1
using esp_err_t=int;using esp_event_base_t=int;using esp_event_handler_instance_t=void*;
struct EventGroup {unsigned bits=0;std::mutex mutex;std::condition_variable changed;};
using EventGroupHandle_t=EventGroup*;using httpd_handle_t=void*;using EventBits_t=unsigned;
struct esp_netif_t {bool up=false,dhcp=false;};
struct esp_ip4_addr_t {uint32_t addr;};struct esp_netif_ip_info_t {esp_ip4_addr_t ip;};
struct ip_event_got_ip_t {esp_netif_t *esp_netif;};
struct wifi_ap_record_t {uint8_t ssid[33];uint8_t primary;};
struct wifi_config_t {struct {uint8_t ssid[32],password[64];int ssid_len,channel,max_connection,authmode;} ap;
 struct {uint8_t ssid[32],password[64];int scan_method,failure_retry_cnt;} sta;};
constexpr int WIFI_EVENT=1,IP_EVENT=2,WIFI_EVENT_AP_START=3,WIFI_EVENT_AP_STOP=4,
 WIFI_EVENT_STA_DISCONNECTED=5,WIFI_EVENT_SCAN_DONE=6,IP_EVENT_STA_GOT_IP=7,
 WIFI_AUTH_OPEN=0,WIFI_MODE_APSTA=3,WIFI_IF_AP=1,WIFI_IF_STA=2,WIFI_ALL_CHANNEL_SCAN=0,WIFI_PS_NONE=0,ESP_MAC_WIFI_SOFTAP=1;
static esp_netif_t apnet,stanet;
static unsigned wifi_starts,wifi_stops,scans,http_starts,http_stops,disconnects,saves;
static std::atomic<unsigned> connects{0};
static bool stop_joined=false,connect_failure=false,save_failure=false,hold_connection=false;
static wifi_config_t current;
using Callback=void(*)(void*,esp_event_base_t,int32_t,void*);
struct Handler {Callback fn=nullptr;void *ctx=nullptr;};static Handler wh,ih;
EventGroupHandle_t xEventGroupCreate(){return new EventGroup;}
void vEventGroupDelete(EventGroupHandle_t p){delete p;}
void xEventGroupClearBits(EventGroupHandle_t p,unsigned bits){std::lock_guard<std::mutex> lock(p->mutex);p->bits&=~bits;}
void xEventGroupSetBits(EventGroupHandle_t p,unsigned bits){std::lock_guard<std::mutex> lock(p->mutex);p->bits|=bits;p->changed.notify_all();}
unsigned xEventGroupWaitBits(EventGroupHandle_t p,unsigned bits,int clear,int,int){
 std::unique_lock<std::mutex> lock(p->mutex);p->changed.wait_for(lock,std::chrono::milliseconds(100),[&]{return (p->bits&bits)!=0;});
 unsigned out=p->bits&bits;if(clear)p->bits&=~bits;return out;
}
void vTaskDelay(int){std::this_thread::yield();}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key){return !strcmp(key,"WIFI_AP_DEF")?&apnet:&stanet;}
int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *ip){assert(n==&apnet&&n->dhcp);ip->ip.addr=0x0104a8c0;return 0;}
int esp_event_handler_instance_register(int base,int,Callback fn,void *ctx,void **instance){auto &h=base==WIFI_EVENT?wh:ih;assert(!h.fn);h={fn,ctx};*instance=&h;return 0;}
int esp_event_handler_instance_unregister(int,int,void *instance){auto *h=(Handler*)instance;assert(stop_joined);*h={};return 0;}
int esp_read_mac(uint8_t *mac,int){memset(mac,0,6);mac[4]=0xd6;mac[5]=0x9c;return 0;}
int esp_wifi_set_mode(int m){assert(m==WIFI_MODE_APSTA);return 0;}
int esp_wifi_set_config(int mode,wifi_config_t *v){assert(mode==WIFI_IF_AP||mode==WIFI_IF_STA);if(mode==WIFI_IF_AP)current=*v;return 0;}
int esp_wifi_set_ps(int m){assert(m==WIFI_PS_NONE);return 0;}
int esp_wifi_start(){++wifi_starts;stop_joined=false;apnet.up=apnet.dhcp=true;wh.fn(wh.ctx,WIFI_EVENT,WIFI_EVENT_AP_START,nullptr);return 0;}
int esp_wifi_stop(){assert(http_stops==http_starts);++wifi_stops;apnet.up=apnet.dhcp=false;stop_joined=true;wh.fn(wh.ctx,WIFI_EVENT,WIFI_EVENT_AP_STOP,nullptr);return 0;}
int esp_wifi_disconnect(){++disconnects;wh.fn(wh.ctx,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr);return 0;}
int esp_wifi_connect(){++connects;if(hold_connection)return 0;if(connect_failure){wh.fn(wh.ctx,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr);return 0;}ip_event_got_ip_t ip{&stanet};ih.fn(ih.ctx,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);return 0;}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *r){r->primary=6;return 0;}
int esp_wifi_scan_start(void*,bool){++scans;wh.fn(wh.ctx,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,nullptr);return 0;}
int esp_wifi_scan_get_ap_num(uint16_t *n){*n=1;return 0;}
int esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *r){assert(*n==1);strcpy((char*)r->ssid,"quoted \\\" network");r->primary=6;return 0;}
void esp_wifi_scan_stop(){} void esp_wifi_clear_ap_list(){} int httpd_stop(void*){++http_stops;return 0;}
struct SsidItem {std::string ssid,password;};
class SsidManager {public:std::vector<SsidItem> items;
 static SsidManager& GetInstance(){static SsidManager s;return s;}
 const std::vector<SsidItem>& GetSsidList(){return items;}
 bool AddSsid(const std::string& ssid,const std::string& password,uint8_t){++saves;if(save_failure)return false;items={{ssid,password}};return true;}
};
char portal_csrf[33]="test-session";
// CLASS
void WifiConfigurationAp::StartWebServer(){++http_starts;server_=this;}
// PRODUCTION
int main(int argc,char **argv){
 assert(argc==2);std::string test=argv[1];WifiConfigurationAp ap;ap.SetSsidPrefix("MeowInbox");
 if(test=="cycles"){
  for(int i=0;i<32;++i){ap.Start();ap.Start();assert(apnet.up&&apnet.dhcp);
   assert(current.ap.authmode==WIFI_AUTH_OPEN&&current.ap.password[0]==0&&current.ap.max_connection==4);
   assert(std::string((char*)current.ap.ssid)=="MeowInbox-D69C");assert(scans==0);
   ap.Stop();ap.Stop();assert(!apnet.up&&!apnet.dhcp&&!wh.fn&&!ih.fn);}
  assert(wifi_starts==32&&wifi_stops==32&&http_starts==32&&http_stops==32);
 }else if(test=="scan"){
  ap.Start();auto records=ap.GetAccessPoints();assert(scans==1&&records.size()==1);
  assert(ap.RequestConnection("new","secret"));assert(ap.GetAccessPoints().size()==1&&scans==1);ap.Stop();
  assert(ap.GetAccessPoints().empty()&&scans==1);
 }else if(test=="ip"){
  ap.Start();ip_event_got_ip_t wrong{&apnet};ih.fn(ih.ctx,IP_EVENT,IP_EVENT_STA_GOT_IP,&wrong);
  assert(!ap.IsConnected());ip_event_got_ip_t right{&stanet};ih.fn(ih.ctx,IP_EVENT,IP_EVENT_STA_GOT_IP,&right);
  assert(ap.IsConnected());ap.Stop();assert(!ap.IsConnected());
 }else if(test=="connect"){
  ap.Start();assert(ap.RequestConnection("home","secret"));assert(connects==0&&saves==0);
  assert(ap.RequestConnection("home","secret"));assert(!ap.RequestConnection("other","secret"));
  assert(ap.GetConnectionStatus().state=="connecting");ap.ProcessConnection();
  auto state=ap.GetConnectionStatus();assert(state.connected&&state.saved&&state.state=="connected"&&ap.IsConnected());
  assert(connects==1&&saves==1&&disconnects==1&&apnet.up); // No post-success disconnect.
  ap.ProcessConnection();assert(connects==1&&saves==1);
  esp_wifi_disconnect();assert(!ap.IsConnected()&&ap.GetConnectionStatus().state=="disconnected");
  ap.Stop();assert(ap.GetConnectionStatus().state=="not_configured");
  ap.Start();assert(ap.GetConnectionStatus().ssid=="home");ap.ProcessConnection();assert(ap.IsConnected()&&saves==1);
 }else if(test=="failure"){
  ap.Start();connect_failure=true;assert(ap.RequestConnection("bad","secret"));ap.ProcessConnection();
  assert(ap.GetConnectionStatus().state=="failed"&&!ap.IsConnected()&&saves==0&&apnet.up);
  connect_failure=false;save_failure=true;assert(ap.RequestConnection("home","secret"));ap.ProcessConnection();
  assert(ap.GetConnectionStatus().state=="save_failed"&&!ap.IsConnected()&&apnet.up);
  save_failure=false;assert(ap.RequestConnection("home","secret"));ap.ProcessConnection();assert(ap.IsConnected());
 }else if(test=="cancel"){
  ap.Start();hold_connection=true;assert(ap.RequestConnection("home","secret"));
  std::thread worker([&]{ap.ProcessConnection();});while(!connects.load())std::this_thread::yield();
  ap.Stop();worker.join();assert(saves==0&&!ap.IsConnected()&&!wh.fn&&!ih.fn);
  ap.Start();hold_connection=false;assert(ap.RequestConnection("after","secret"));ap.ProcessConnection();assert(ap.IsConnected());
 }else return 1;
}
'''
class ProvisioningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='manual-portal-',dir=ROOT/'private/host-tests');p=Path(cls.tmp.name)
        source=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
        markers=['WifiConfigurationAp::WifiConfigurationAp()', 'WifiConfigurationAp::~WifiConfigurationAp()',
          'void WifiConfigurationAp::SetSsidPrefix(', 'std::string WifiConfigurationAp::GetSsid()',
          'void WifiConfigurationAp::Start()', 'void WifiConfigurationAp::Stop()',
          'std::vector<wifi_ap_record_t> WifiConfigurationAp::GetAccessPoints()',
          'void WifiConfigurationAp::WifiEventHandler(', 'void WifiConfigurationAp::IpEventHandler(',
          'bool WifiConfigurationAp::RequestConnection(', 'void WifiConfigurationAp::ProcessConnection(',
          'WifiPortalStatus WifiConfigurationAp::GetConnectionStatus(', 'bool WifiConfigurationAp::ConnectToWifi(', 'bool WifiConfigurationAp::Save(']
        header=(ROOT/'components/esp-wifi-connect/include/wifi_configuration_ap.h').read_text()
        header='\n'.join(line for line in header.splitlines() if not line.startswith('#'))
        (p/'test.cpp').write_text(HARNESS.replace('// CLASS',header).replace('// PRODUCTION','\n'.join(extract(source,m) for m in markers)))
        cls.binary=p/'test'
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-pthread',str(p/'test.cpp'),'-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=10)
    def test_start_stop_32_cycles(self):self.run_case('cycles')
    def test_scan_not_during_connect(self):self.run_case('scan')
    def test_only_station_ip_means_connected(self):self.run_case('ip')
    def test_async_commit_keeps_ap_and_sta_and_reopens_saved(self):self.run_case('connect')
    def test_failed_connection_or_save_never_reports_success(self):self.run_case('failure')
    def test_exit_joins_inflight_worker_and_can_reenter(self):self.run_case('cancel')
    def test_http_handler_capacity_covers_all_registered_routes(self):
        source=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
        capacity=int(re.search(r'config.max_uri_handlers = (\d+)',source)[1])
        registrations=re.findall(r'httpd_register_uri_handler\(server_,\s*&([a-z_]+)\)',source)
        self.assertEqual(len(registrations),len(set(registrations)))
        self.assertGreaterEqual(capacity,len(registrations))
    def test_sdk_lru_runs_before_shared_socket_pool_exhaustion(self):
        # Execute the locked SDK accept/LRU body. Only the descriptor allocator
        # and session queue are doubles; this is not an ESP RF/phone stress test.
        ap=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
        sdk=ROOT/'.tools/esp-idf/components/esp_http_server'
        body=extract((sdk/'src/httpd_main.c').read_text(),'static esp_err_t httpd_accept_conn(')
        defaults=(sdk/'include/esp_http_server.h').read_text()
        old_limit=int(re.search(r'\.max_open_sockets\s*=\s*(\d+)',defaults)[1])
        client_limit=int(re.search(r'config.max_open_sockets\s*=\s*(\d+)',ap)[1])
        self.assertIn('config.lru_purge_enable = true;',ap)
        # A fresh host checkout has no generated sdkconfig. Resolve this one
        # unconditional integer from the pinned SDK and each build's defaults.
        kconfig=(ROOT/'.tools/esp-idf/components/lwip/Kconfig').read_text()
        definition=kconfig.split('    config LWIP_MAX_SOCKETS\n',1)[1].split('\n        help',1)[0]
        defaults=re.findall(r'^\s*default (.+)$',definition,re.M)
        self.assertEqual(len(defaults),1)
        self.assertRegex(defaults[0],r'^\d+$')
        totals=[]
        for board in ('vocat16','spotpear'):
            build=(ROOT/f'scripts/build_{board}.sh').read_text()
            self.assertIn(f'-D SDKCONFIG_DEFAULTS=sdkconfig.{board}.defaults',build)
            config=(ROOT/f'sdkconfig.{board}.defaults').read_text()
            overrides=re.findall(r'^CONFIG_LWIP_MAX_SOCKETS=(\d+)$',config,re.M)
            self.assertLessEqual(len(overrides),1)
            total=int(overrides[0] if overrides else defaults[0])
            generated=ROOT/f'sdkconfig.{board}'
            if generated.exists():
                self.assertEqual(int(re.search(r'^CONFIG_LWIP_MAX_SOCKETS=(\d+)',generated.read_text(),re.M)[1]),total)
            totals.append(total)
        self.assertEqual(totals[0],totals[1])
        harness=r'''
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cerrno>
#include <cassert>
#include <cstdio>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,HTTP_SERVER_EVENT_ON_CONNECTED=1;
constexpr int DEFAULT_KEEP_ALIVE_IDLE=5,DEFAULT_KEEP_ALIVE_INTERVAL=5,DEFAULT_KEEP_ALIVE_COUNT=3;
#define TAG "budget"
#define LOG_FMT(x) x
void log_stub(const char*,const char*,...){}
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
struct config_t{int max_open_sockets,recv_wait_timeout=15,send_wait_timeout=15;
 bool lru_purge_enable=true,keep_alive_enable=false;int keep_alive_idle=0,keep_alive_interval=0,keep_alive_count=0;};
struct httpd_data{config_t config;int active=0;bool pending=false;};
static int used,total,accepted,evicted;
bool httpd_is_sess_available(httpd_data* h){return h->active<h->config.max_open_sockets;}
int httpd_sess_close_lru(httpd_data* h){assert(h->active>0&&!h->pending);h->pending=true;return ESP_OK;}
int fake_accept(int,sockaddr*,socklen_t*){if(used==total){errno=ENFILE;return -1;}++used;++accepted;return 100+accepted;}
int fake_setsockopt(int,int,int,const void*,socklen_t){return 0;}
int fake_close(int){--used;return 0;}
int httpd_sess_new(httpd_data* h,int){assert(h->active<h->config.max_open_sockets);++h->active;return ESP_OK;}
void esp_http_server_dispatch_event(int,const void*,size_t){}
#define accept fake_accept
#define setsockopt fake_setsockopt
#define close fake_close
// SDK_BODY
int run(int limit,int other,bool expect_failure){
 httpd_data h{{limit}};used=3+other;accepted=evicted=0;
 for(int i=0;i<100;++i){
  int result=httpd_accept_conn(&h,0);
  if(result==ESP_FAIL){assert(expect_failure&&errno==ENFILE);return accepted;}
  if(h.pending){h.pending=false;--h.active;--used;++evicted;--i;}
 }
 assert(!expect_failure&&accepted==100&&evicted>0&&used<=total);return accepted;
}
int main(){
 total=TOTAL;
 int failure_at=run(OLD_LIMIT,3,true);assert(failure_at==total-3-3);
 for(int other=0;other<=3;++other)assert(run(CLIENT_LIMIT,other,false)==100);
 printf("PASS SDK default exhausts at %d clients with three outbound slots; current LRU supports 100 accepts for each 0..3 outbound load\n",failure_at);
}
'''.replace('// SDK_BODY',body).replace('TOTAL',str(totals[0])).replace('OLD_LIMIT',str(old_limit)).replace('CLIENT_LIMIT',str(client_limit))
        with tempfile.TemporaryDirectory(prefix='http-socket-budget-',dir=ROOT/'private/host-tests') as temp:
            p=Path(temp);(p/'test.cpp').write_text(harness)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=10)

    def test_auto_portal_artifacts_removed(self):
        component=ROOT/'components/esp-wifi-connect'
        self.assertFalse((component/'dns_server.cc').exists());self.assertFalse((component/'include/dns_server.h').exists())
        self.assertFalse((component/'assets/wifi_configuration_done.html').exists())
        source=(component/'wifi_configuration_ap.cc').read_text()
        self.assertNotIn('302 Found',source);self.assertNotIn('httpd_register_err_handler',source)
        self.assertNotIn('dns_server', (component/'CMakeLists.txt').read_text())
if __name__=='__main__':unittest.main(verbosity=2)
