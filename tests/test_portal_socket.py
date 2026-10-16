"""Run the production AP socket filter on real loopback TCP accept/getpeer state.

ESP netif lookup is a shim; socket address families and IPv4 mapping are native.
This covers the dual-stack HTTPD path omitted by the handler-only fixture.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdint>
using esp_err_t=int;using httpd_handle_t=void*;
constexpr int ESP_OK=0,ESP_FAIL=-1;
struct esp_netif_t{};struct esp_netif_ip_info_t{struct{uint32_t addr;} ip;};
static esp_netif_t netif;static bool available=true,readable=true;
static uint32_t expected=inet_addr("127.0.0.1");
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key){assert(!strcmp(key,"WIFI_AP_DEF"));return available?&netif:nullptr;}
int esp_netif_get_ip_info(esp_netif_t* n,esp_netif_ip_info_t* out){assert(n==&netif);out->ip.addr=expected;return readable?ESP_OK:ESP_FAIL;}
static auto check = // PRODUCTION
int connect_pair(int family,bool native_v6){
 int listener=socket(family,SOCK_STREAM,0);assert(listener>=0);
 if(family==AF_INET6){
  int only=0;assert(setsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&only,sizeof(only))==0);
  sockaddr_in6 addr{};addr.sin6_family=AF_INET6;addr.sin6_addr=in6addr_any;
  assert(bind(listener,(sockaddr*)&addr,sizeof(addr))==0);
 }else{
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=inet_addr("127.0.0.1");
  assert(bind(listener,(sockaddr*)&addr,sizeof(addr))==0);
 }
 assert(listen(listener,1)==0);sockaddr_storage local{};socklen_t size=sizeof(local);
 assert(getsockname(listener,(sockaddr*)&local,&size)==0);
 uint16_t port=family==AF_INET6?((sockaddr_in6*)&local)->sin6_port:((sockaddr_in*)&local)->sin_port;
 int client=socket(native_v6?AF_INET6:AF_INET,SOCK_STREAM,0);assert(client>=0);
 if(native_v6){
  sockaddr_in6 dst{};dst.sin6_family=AF_INET6;dst.sin6_addr=in6addr_loopback;dst.sin6_port=port;
  assert(connect(client,(sockaddr*)&dst,sizeof(dst))==0);
 }else{
  sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_addr.s_addr=inet_addr("127.0.0.1");dst.sin_port=port;
  assert(connect(client,(sockaddr*)&dst,sizeof(dst))==0);
 }
 int accepted=accept(listener,nullptr,nullptr);assert(accepted>=0);close(client);close(listener);
 return accepted;
}
int main(){
 for(int mode=0;mode<3;++mode){
  int fd=connect_pair(mode==0?AF_INET:AF_INET6,mode==2);
  sockaddr_storage actual{};socklen_t size=sizeof(actual);assert(getsockname(fd,(sockaddr*)&actual,&size)==0);
  assert(actual.ss_family==(mode==0?AF_INET:AF_INET6));
  if(mode==1)assert(IN6_IS_ADDR_V4MAPPED(&((sockaddr_in6*)&actual)->sin6_addr));
  expected=inet_addr("127.0.0.1");int result=check(nullptr,fd);
  printf("mode=%s family=%d length=%u AP_result=%d expected=%d\n",mode==0?"IPv4":mode==1?"mapped-IPv4":"native-IPv6",actual.ss_family,(unsigned)size,result,mode==2?ESP_FAIL:ESP_OK);fflush(stdout);
  assert(result==(mode==2?ESP_FAIL:ESP_OK));
  expected=inet_addr("127.0.0.2");assert(check(nullptr,fd)==ESP_FAIL); // Non-AP destination.
  expected=inet_addr("127.0.0.1");available=false;assert(check(nullptr,fd)==ESP_FAIL);available=true;
  readable=false;assert(check(nullptr,fd)==ESP_FAIL);readable=true;close(fd);
 }
 assert(check(nullptr,-1)==ESP_FAIL);
 puts("PASS real IPv4/mapped IPv4 AP destinations; reject non-AP/native IPv6/netif and socket errors");
}
'''

class PortalSocketTests(unittest.TestCase):
    def test_real_dual_stack_socket_filter(self):
        source=(ROOT/'components/esp-wifi-connect/wifi_configuration_ap.cc').read_text()
        start=source.index('config.open_fn = ')+len('config.open_fn = ')
        end=source.index('\n    };',start)+len('\n    };')
        production=source[start:end]
        with tempfile.TemporaryDirectory(prefix='portal-socket-',dir=ROOT/'private/host-tests') as tmp:
            p=Path(tmp);(p/'test.cpp').write_text(HARNESS.replace('// PRODUCTION',production))
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True,timeout=15)

if __name__=='__main__':unittest.main(verbosity=2)
