"""Execute production media polling, safe handoff and power guards with host I/O.

No physical card, battery-current, endurance, or real touch claim.
"""
from pathlib import Path
import subprocess, tempfile, unittest
from test_sd_dma_reserve import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <setjmp.h>
#include "power_state.h"
enum {MEMO_IDLE,MEMO_RECORDING,MEMO_SAVING,MEMO_SAVED,MEMO_PLAYING,MEMO_ERROR,MEMO_PAUSED,MEMO_USB_MIC};
typedef int esp_err_t;
typedef struct {bool sd_ready,cardless,voice_window;int state;} memo_snapshot_t;
typedef struct {bool initialized,external_power;memo_power_stage_t stage;} memo_power_status_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define VOICE_OFF 0
#define ESP_OK 0
#define ESP_LOGI(tag,...) ((void)(tag),printf(__VA_ARGS__))
#define ESP_LOGE(tag,...) ((void)(tag),printf(__VA_ARGS__))
#define ESP_LOGW(tag,...) ((void)(tag),printf(__VA_ARGS__))
static const char *TAG="host";
static const char*esp_err_to_name(int err){(void)err;return "synthetic";}
static atomic_bool power_listening=true,sd_probe_busy,microphone_open;
static atomic_int sd_probe_ready;
static atomic_bool sd_view_visible,sd_view_requested;
static atomic_uint sd_probe_checks,sd_probe_wakes;
typedef unsigned TickType_t;
#define portMAX_DELAY UINT32_MAX
static unsigned sd_probe_handle=1,notifications;
static int64_t clock_us;
static void xTaskNotifyGive(unsigned handle){assert(handle==1);notifications++;}
static bool background_work;
static bool network_sync_sd_work_pending(void){return background_work;}
typedef struct {bool provisioning,syncing;} memo_network_status_t;
static memo_network_status_t network;
static void network_sync_get_status(memo_network_status_t*out){*out=network;}
static void ulTaskNotifyTake(int clear,TickType_t wait);
static atomic_bool recovery_deferred;
static int sd_probe_mutex=1;
static memo_snapshot_t snapshot;
static memo_power_status_t power;
static bool unique_audio,gate_ok=true,mutex_ok=true,absent=true,audio_ready=true;
static bool sd_lock,claim,sleep_on_claim,record_on_claim;
static int checks,mounts,claims,releases,remounts,adoptions,voice_stops,mic_stops;
static int media_result;
static bool audio_owned;
static int partial_reads,pending_reads,partial_result,pending_result,store;
typedef struct {uint32_t id,data_bytes,duration_ms;} memo_record_t;
static int memo_store_inspect_partial(int*s,uint32_t id,memo_record_t*r){(void)s;assert(audio_owned);partial_reads++;*r=(memo_record_t){id,32,1};return partial_result;}
static int memo_store_pending_id(int*s,uint32_t*id){(void)s;assert(audio_owned);pending_reads++;*id=1;return pending_result;}
static bool board_external_power_present(void){return power.external_power;}
static bool board_battery_protection_raw(uint16_t*r){*r=80;return true;}
static bool power_policy_recovery_ready(void){return true;}
static void set_recovering(bool b){(void)b;}

static void app_snapshot(memo_snapshot_t*out){*out=snapshot;}
static void power_policy_get_status(memo_power_status_t*out){*out=power;}
static bool memo_cardless_busy(void){return unique_audio;}
static int xSemaphoreTake(int m,int t){(void)m;(void)t;if(!mutex_ok)return 0;assert(!sd_lock);sd_lock=true;return 1;}
static void xSemaphoreGive(int m){(void)m;assert(sd_lock);sd_lock=false;}
static bool network_sync_try_sd_probe(void){
 if(!gate_ok){return false;}
 assert(sd_lock&&sd_probe_busy);
 claim=true;claims++;
 if(sleep_on_claim){power_listening=false;}
 if(record_on_claim){snapshot.state=MEMO_RECORDING;}
 return true;
}
static void network_sync_sd_probe_done(void){assert(claim);claim=false;releases++;}
static esp_err_t board_sd_check(void){assert((claim||audio_owned)&&sd_lock);checks++;return media_result;}
static esp_err_t board_sd_mount(void){assert((claim||audio_owned)&&sd_lock);mounts++;return media_result;}
static bool board_sd_absent(void){return absent;}
static bool network_sync_prepare_audio(unsigned ms){(void)ms;if(gate_ok)audio_owned=true;return gate_ok;}
static void network_sync_audio_done(void){assert(audio_owned);audio_owned=false;}
static void voice_set_mode(int mode){assert(mode==VOICE_OFF);voice_stops++;}
static int microphone_stop(void){mic_stops++;return 0;}
static bool storage_prepare(bool remount){
 assert(audio_owned);if(remount)remounts++;else adoptions++;
 snapshot.sd_ready=media_result==0;snapshot.cardless=media_result!=0&&absent;
 return snapshot.sd_ready||snapshot.cardless;
}
static void state(int s,const char*error){(void)error;snapshot.state=s;}

static void recover_pending(void){assert(0);}
static int64_t esp_timer_get_time(void){return clock_us;}
static void vTaskDelay(int ticks){(void)ticks;microphone_open=false;}
'''
TEST=r'''
static void reset(void){
 snapshot=(memo_snapshot_t){.sd_ready=true,.state=MEMO_IDLE};
 sd_view_visible=true;sd_view_requested=false;sd_probe_checks=sd_probe_wakes=0;
 background_work=false;network=(memo_network_status_t){0};clock_us=0;notifications=0;
 power=(memo_power_status_t){.initialized=true,.external_power=true,.stage=MEMO_POWER_ACTIVE};
 power_listening=true;sd_probe_busy=false;sd_probe_ready=0;microphone_open=false;
 unique_audio=false;gate_ok=mutex_ok=absent=audio_ready=true;
 sd_lock=claim=sleep_on_claim=record_on_claim=false;
 checks=mounts=claims=releases=remounts=adoptions=voice_stops=mic_stops=0;media_result=0;
 audio_owned=false;recovery_deferred=false;partial_reads=pending_reads=partial_result=pending_result=0;
}
static jmp_buf task_done;
static unsigned task_phase;
static void ulTaskNotifyTake(int clear,TickType_t wait){
 assert(clear==pdTRUE);
 switch(task_phase++) {
 case 0:assert(wait==portMAX_DELAY&&checks==0);clock_us=10000000;app_storage_view(true);break;
 case 1:assert(wait==5000&&checks==1);clock_us+=5000000;break;
 case 2:assert(wait==5000&&checks==2);app_storage_view(false);break;
 case 3:assert(wait==portMAX_DELAY&&checks==2);power.external_power=false;power.stage=MEMO_POWER_VOICE_SLEEP;app_storage_view(true);break;
 case 4:assert(wait==portMAX_DELAY&&checks==2);power.stage=MEMO_POWER_ACTIVE;break;
 case 5:assert(wait==5000&&checks==3);snapshot.state=MEMO_RECORDING;break;
 case 6:assert(wait==portMAX_DELAY&&checks==3);snapshot.state=MEMO_IDLE;app_storage_view(false);background_work=true;break;
 case 7:assert(wait==15000&&checks==4);background_work=false;break;
 case 8:assert(wait==portMAX_DELAY&&checks==4);longjmp(task_done,1);break;
 default:assert(0);
 }
}
int main(void){
 reset();for(int n=0;n<8;n++)sd_probe_once();
 assert(checks==8&&mounts==0&&sd_probe_ready==0&&!sd_probe_busy&&!sd_lock&&claims==releases);
 assert(!apply_sd_probe()&&voice_stops==0&&mic_stops==0);
 // A removed card schedules one remount at idle, never an immediate switch.
 media_result=0x107;sd_probe_once();assert(checks==9&&sd_probe_ready==2&&remounts==0);
 sd_probe_once();assert(checks==9);snapshot.state=MEMO_RECORDING;assert(!apply_sd_probe());
 snapshot.state=MEMO_IDLE;unique_audio=true;assert(!apply_sd_probe());unique_audio=false;
 gate_ok=false;assert(!apply_sd_probe());gate_ok=true;
 assert(apply_sd_probe()&&remounts==1&&adoptions==0&&sd_probe_ready==0&&voice_stops==1);
 // An absent card does not change mode. Responsive mount errors are adopted
 // by storage_prepare, which retains the existing storage-error classifier.
 reset();snapshot.sd_ready=false;snapshot.cardless=true;media_result=0x107;
 sd_probe_once();assert(mounts==1&&checks==0&&!sd_probe_ready);
 absent=false;sd_probe_once();assert(sd_probe_ready==1);assert(apply_sd_probe()&&adoptions==1&&remounts==0);
 reset();snapshot.sd_ready=false;snapshot.cardless=true;sd_probe_once();assert(sd_probe_ready==1);assert(apply_sd_probe());
 for(int mode=MEMO_RECORDING;mode<=MEMO_USB_MIC;mode++){
  if(mode==MEMO_SAVED||mode==MEMO_ERROR)continue;
  reset();snapshot.state=mode;sd_probe_once();assert(!checks&&!mounts&&!sd_probe_busy);
 }
 reset();unique_audio=true;sd_probe_once();assert(!checks);
 reset();gate_ok=false;sd_probe_once();assert(!checks&&!sd_lock&&!sd_probe_busy);
 reset();mutex_ok=false;sd_probe_once();assert(!checks&&!sd_probe_busy);
 reset();sleep_on_claim=true;sd_probe_once();assert(!checks&&claims==releases&&!sd_probe_busy);
 reset();record_on_claim=true;sd_probe_once();assert(!checks&&claims==releases);
 // Polls cannot reset the inactive timer, wake the device or prevent sleep.
 reset();power.external_power=false;
 for(unsigned ms=0;ms<=600000;ms+=15000){
  power.stage=memo_power_stage(true,false,app_power_is_busy(),ms);
  sd_probe_once();
 }
 assert(checks==8&&mounts==0&&power.stage==MEMO_POWER_BUTTON_SLEEP);
 power.stage=MEMO_POWER_VOICE_SLEEP;sd_probe_ready=2;assert(!apply_sd_probe()&&remounts==0);
 power.stage=MEMO_POWER_ACTIVE;assert(apply_sd_probe()&&remounts==1);
 reset();sd_probe_busy=true;assert(!app_power_is_busy());assert(!app_power_set_listening(false,0)&&power_listening);
 sd_probe_busy=false;assert(app_power_set_listening(false,0)&&!power_listening);sd_probe_once();assert(!checks);
 assert(app_power_set_listening(true,0));sd_probe_once();assert(checks==1);
 reset();gate_ok=false;inspect_partial(1);assert(!partial_reads&&!audio_owned);
 assert(!sd_recovery_ready()&&!pending_reads&&recovery_deferred);
 gate_ok=true;inspect_partial(1);assert(partial_reads==1&&!audio_owned);
 partial_result=-1;inspect_partial(1);assert(partial_reads==2&&!audio_owned);
 assert(!sd_recovery_ready()&&pending_reads==1&&!audio_owned);
 pending_result=-1;assert(!sd_recovery_ready()&&pending_reads==2&&!audio_owned);
 pending_result=1;assert(sd_recovery_ready()&&pending_reads==3&&!audio_owned&&snapshot.state==MEMO_SAVING);
 // Demand: pure cat idle, settings and USB cannot cause an SD transaction.
 reset();app_storage_view(false);sd_probe_once();assert(!checks&&sd_probe_interval(&snapshot)==0);
 background_work=true;assert(sd_probe_interval(&snapshot)==15000);sd_probe_once();assert(checks==1);
 network.provisioning=true;sd_probe_once();assert(checks==1&&sd_probe_interval(&snapshot)==0);
 reset();snapshot.sd_ready=false;snapshot.cardless=false;assert(sd_probe_interval(&snapshot)==0);
 app_storage_view(false);app_storage_view(true);assert(sd_probe_interval(&snapshot)==5000);
 // Same production operation-entry classifier: newly inserted SD wins even
 // when the prior no-card mode could not record due to disconnected service.
 reset();snapshot.sd_ready=false;snapshot.cardless=true;audio_owned=true;media_result=0x107;
 assert(storage_check_for_operation()&&snapshot.cardless&&mounts==1&&adoptions==0);
 media_result=0;assert(storage_check_for_operation()&&snapshot.sd_ready&&!snapshot.cardless&&adoptions==1);
 reset();snapshot.sd_ready=false;snapshot.cardless=true;audio_owned=true;media_result=0x107;absent=false;
 assert(!storage_check_for_operation()&&!snapshot.cardless&&adoptions==1);
 reset();audio_owned=true;media_result=0x107;
 assert(storage_check_for_operation()&&snapshot.cardless&&remounts==1);
 reset();audio_owned=true;unique_audio=true;assert(!storage_check_for_operation()&&!checks&&!mounts);
 reset();snapshot.sd_ready=false;snapshot.cardless=true;audio_owned=true;pending_result=1;
 assert(!storage_check_for_operation()&&recovery_deferred&&snapshot.state==MEMO_ERROR);
 // Execute the production wait loop. No-demand and sleep are indefinite
 // waits, visible UI is 5s, real media work is 15s, recording blocks all I/O.
 reset();app_storage_view(false);task_phase=0;
 if(!setjmp(task_done))sd_probe_task(NULL);
 assert(task_phase==9&&checks==4&&!claim&&!sd_lock);
 puts("PASS: production SD probe status-only cadence, idle handoff, active-mode exclusion, gate races and sleep policy (host shims)");
 return 0;
}
'''
class SDProbeTests(unittest.TestCase):
 def test_production_probe_and_safe_handoff(self):
  app=(ROOT/'main/app.c').read_text()
  names=['void app_storage_view(', 'static bool sd_probe_allowed(', 'static uint32_t sd_probe_interval(', 'static bool storage_check_for_operation(', 'static void inspect_partial(', 'static bool sd_recovery_ready(', 'static bool apply_sd_probe(', 'bool app_power_is_busy(', 'bool app_power_set_listening(', 'static void sd_probe_once(', 'static void sd_probe_task(']
  code=PREFIX+'\n'.join(function(app,n) for n in names)+TEST
  with tempfile.TemporaryDirectory(prefix='sd-probe-',dir=ROOT/'private') as d:
   p=Path(d);(p/'probe.c').write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-O1','-I',str(ROOT/'main'),str(p/'probe.c'),str(ROOT/'main/power_state.c'),'-o',str(p/'probe')],check=True)
   subprocess.run([str(p/'probe')],check=True)
class SyncDemandTests(unittest.TestCase):
 def test_real_discovery_consumes_events_not_idle_ticks(self):
  source=(ROOT/'main/network_sync.cpp').read_text()
  prefix=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include "network_sync_state.h"
static std::atomic<bool> sd_scan_requested{true},sd_media_work{false};
static std::atomic<uint32_t> sd_directory_scans{0};
static memo_sync_queue_t sync_queue{};
static int status_mutex;
#define portMAX_DELAY 0
#define ESP_OK 0
static bool io_ok=true,media_ok=true,scan_ok=true,event_during_scan=false;
static uint32_t found,observed_latest,notify_count;
static unsigned sync_task_handle=1;
static std::atomic<int64_t> latest_change_us{0};
static int64_t esp_timer_get_time(){return 123;}
static void network_sync_observe_latest(uint32_t id){observed_latest=id;}
static void xTaskNotifyGive(unsigned){notify_count++;}
static bool io_owned=false;
static int begin_sd_io(void*){if(io_ok)io_owned=true;return io_ok;}
static void end_sd_io(void*){assert(io_owned);io_owned=false;}
static int board_sd_check(){assert(io_owned);return media_ok?0:1;}
static void xSemaphoreTake(int,int){}
static void xSemaphoreGive(int){}
static uint32_t memo_sd_sync_pending(const char*,const memo_sync_target_t*,uint32_t*,bool*manual,bool*complete){
 assert(io_owned);*manual=false;*complete=scan_ok;
 if(event_during_scan)sd_scan_requested=true;
 return found;
}
'''
  body=function(source,'bool pending_record(')+function(source,'bool discover_sd_work(')+function(source,'extern "C" void network_sync_notify_record(')
  checks=r'''
int main(){
 memo_sync_target_t target{};
 for(int n=0;n<100;n++)discover_sd_work(target);
 assert(sd_directory_scans==1&&!sd_scan_requested&&!sd_media_work);
 network_sync_notify_record(17);
 assert(sd_scan_requested&&sd_media_work&&observed_latest==17&&notify_count==1);
 io_ok=false;discover_sd_work(target);
 assert(sd_scan_requested&&sd_media_work&&sd_directory_scans==1);
 io_ok=true;media_ok=false;discover_sd_work(target);
 assert(sd_scan_requested&&sd_directory_scans==1&&!io_owned);
 media_ok=true;scan_ok=false;discover_sd_work(target);
 assert(sd_scan_requested&&sd_media_work&&sd_directory_scans==2);
 scan_ok=true;found=17;discover_sd_work(target);
 assert(sync_queue.pending_id==17&&sd_media_work&&!sd_scan_requested);
 assert(memo_sync_queue_start(&sync_queue,true));memo_sync_queue_finish(&sync_queue,17);
 sd_scan_requested=true;found=0;event_during_scan=true;discover_sd_work(target);
 assert(sd_scan_requested);event_during_scan=false;discover_sd_work(target);
 assert(!sd_scan_requested&&!sd_media_work&&!io_owned);
}
'''
  with tempfile.TemporaryDirectory(prefix='sd-demand-',dir=ROOT/'private') as d:
   p=Path(d);(p/'demand.cpp').write_text(prefix+body+checks)
   subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-I',str(ROOT/'main'),'-c',str(ROOT/'main/network_sync_state.c'),'-o',str(p/'state.o')],check=True)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(p/'demand.cpp'),str(p/'state.o'),'-o',str(p/'demand')],check=True)
   subprocess.run([str(p/'demand')],check=True)
class ScanErrorTests(unittest.TestCase):
 def test_entry_io_failure_keeps_discovery_pending(self):
  source=(ROOT/'main/memo_sd_sync.cpp').read_text()
  prefix=r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <inttypes.h>
#include "memo_sd_sync.h"
static memo_cache_result_t cache_result=MEMO_CACHE_OK;
static int wav_error;
extern "C" memo_cache_result_t memo_sync_entry_read(const char*,uint32_t id,memo_sync_entry_t*out){
 *out={};out->id=id;out->automatic=true;out->state=MEMO_JOB_QUEUED;return cache_result;
}
extern "C" bool memo_sync_target_matches(const memo_sync_target_t*,const memo_sync_target_t*,bool){return true;}
extern "C" int memo_store_validate(const char*,memo_record_t*){errno=wav_error;return wav_error?-1:0;}
'''
  checks=r'''
int main(int argc,char**argv){
 assert(argc==2);memo_sync_target_t target{};uint32_t after=0;bool manual=false,complete=true;
 cache_result=MEMO_CACHE_IO_ERROR;
 assert(!memo_sd_sync_pending(argv[1],&target,&after,&manual,&complete)&&!complete);
 cache_result=MEMO_CACHE_MISSING;
 assert(!memo_sd_sync_pending(argv[1],&target,&after,&manual,&complete)&&complete);
 cache_result=MEMO_CACHE_OK;wav_error=EIO;
 assert(!memo_sd_sync_pending(argv[1],&target,&after,&manual,&complete)&&!complete);
 wav_error=EINVAL;
 assert(!memo_sd_sync_pending(argv[1],&target,&after,&manual,&complete)&&complete);
 wav_error=0;
 assert(memo_sd_sync_pending(argv[1],&target,&after,&manual,&complete)==1&&complete);
}
'''
  with tempfile.TemporaryDirectory(prefix='sd-errors-',dir=ROOT/'private') as d:
   p=Path(d);(p/'sync_r1_0000000001.job.0').touch();(p/'scan.cpp').write_text(prefix+function(source,'extern "C" uint32_t memo_sd_sync_pending(')+checks)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(p/'scan.cpp'),'-o',str(p/'scan')],check=True)
   subprocess.run([str(p/'scan'),str(p)],check=True)
class PlaybackIdentityTests(unittest.TestCase):
 def test_actual_resume_rejects_same_length_changed_audio_and_hash_owner(self):
  app=(ROOT/'main/app.c').read_text();net=(ROOT/'main/network_sync.cpp').read_text()
  prefix=r'''
#include <cassert>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include "memo_sync_cache.h"
#include "memo_store.h"
/* Crypto backend shim: tests byte traversal/ownership, not SHA strength. */
struct mbedtls_sha256_context {uint8_t sum;};
static void mbedtls_sha256_init(mbedtls_sha256_context*c){c->sum=0;}
static int mbedtls_sha256_starts(mbedtls_sha256_context*c,int){c->sum=0;return 0;}
static int mbedtls_sha256_update(mbedtls_sha256_context*c,const uint8_t*p,size_t n){while(n--)c->sum^=*p++;return 0;}
static int mbedtls_sha256_finish(mbedtls_sha256_context*c,uint8_t*out){memset(out,c->sum,32);return 0;}
static void mbedtls_sha256_free(mbedtls_sha256_context*){}
static void taskYIELD(){}
static bool audio_owned;
static bool audio_busy(){return audio_owned;}
#define MEMO_ACTIVITY_AUDIO 1
static int activity_gate;
static int memo_config_activity(int*){return audio_owned?1:0;}
static std::atomic<bool> file_read_active{false};
static FILE*playing;
static memo_audio_identity_t play_identity;
static uint32_t play_bytes=32,play_remaining=32;
static bool audio_ready=true,gate_ok=true;
static int failures,opened,state_value,store,mutex;
struct memo_snapshot_t {bool sd_ready;uint32_t active_id,revision;char error[96];};
static memo_snapshot_t model{true,1,0,{}};
static void app_snapshot(memo_snapshot_t*s){*s=model;}
static void failure(const char*){failures++;}
static bool network_sync_prepare_audio(int){if(gate_ok)audio_owned=true;return gate_ok;}
static void network_sync_audio_done(){assert(audio_owned);audio_owned=false;}
static int board_sd_check(){return 0;}
static int xSemaphoreTake(int,int){return 1;}
static void xSemaphoreGive(int){}
static void state(int v,const char*){state_value=v;}
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_OK 0
#define MEMO_PLAYING 4
#define portMAX_DELAY 0
static int microphone_stop(){return 0;}
static int board_play_open(){opened++;return 0;}
static char wav_path[256];
static int memo_store_path(int*,uint32_t,char*out,size_t cap){snprintf(out,cap,"%s",wav_path);return 0;}
extern "C" int memo_store_validate(const char*,memo_record_t*out){out->data_bytes=64;return 0;}
'''
  bodies='\n'.join(function(net,n) for n in ['bool sha256_file(', 'bool file_audio_identity(', 'extern "C" bool network_sync_hash_local_audio('])
  bodies+=function((ROOT/'main/memo_sync_cache.c').read_text(),'bool memo_sync_audio_matches(')+function(app,'static void playback_resume(').replace(chr(34)+'PRI',chr(34)+' PRI')
  checks=r'''
int main(int argc,char**argv){
 assert(argc==2);snprintf(wav_path,sizeof(wav_path),"%s",argv[1]);
 memo_audio_identity_t identity{};
 assert(!network_sync_hash_local_audio(wav_path,&identity));
 audio_owned=true;assert(network_sync_hash_local_audio(wav_path,&identity)&&identity.bytes==108);
 char hex[65];uint32_t bytes=0;assert(!sha256_file(wav_path,hex,&bytes));
 file_read_active=true;assert(!network_sync_hash_local_audio(wav_path,&identity));file_read_active=false;
 play_identity=identity;audio_owned=false;
 playback_resume();assert(opened==1&&!failures&&state_value==MEMO_PLAYING&&playing);
 fclose(playing);playing=nullptr;network_sync_audio_done();
 FILE*f=fopen(wav_path,"r+b");assert(f);fseek(f,50,SEEK_SET);fputc(1,f);fclose(f);
 playback_resume();assert(opened==1&&failures==1&&!playing&&!audio_owned);
}
'''
  with tempfile.TemporaryDirectory(prefix='resume-identity-',dir=ROOT/'private') as d:
   p=Path(d);(p/'audio.wav').write_bytes(bytes(108));(p/'resume.cpp').write_text(prefix+bodies+checks)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(p/'resume.cpp'),'-o',str(p/'resume')],check=True)
   subprocess.run([str(p/'resume'),str(p/'audio.wav')],check=True)
class StorageVisibilityTests(unittest.TestCase):
 def test_real_page_transitions_never_publish_stale_demand(self):
  source=(ROOT/'main/ui.c').read_text()
  prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LV_OBJ_FLAG_HIDDEN 1
#define MEMO_PLAYING 4
#define MEMO_PAUSED 6
#define CMD_STOP 1
typedef int lv_obj_t;
static lv_obj_t *record_root,*cat_root,*panel_root,*usb_root,*provision_root;
static bool panel_visible,record_visible,usb_visible,provision_visible,detail_return_after_stop;
static uint32_t panel_activity_tick;
static struct {bool cardless;} selected_record;
typedef struct {int state;} memo_snapshot_t;
static void app_snapshot(memo_snapshot_t*s){s->state=0;}
static bool app_command(int c,int a,const char*s){(void)c;(void)a;(void)s;return true;}
static unsigned true_publications;
static bool demand;
static void app_storage_view(bool v){demand=v;if(v)true_publications++;}
static void network_sync_select_record(int id){(void)id;}
static uint32_t lv_tick_get(void){return 0;}
static void lv_obj_add_flag(lv_obj_t*p,int f){(void)p;(void)f;}
static void lv_obj_remove_flag(lv_obj_t*p,int f){(void)p;(void)f;}
'''
  checks=r'''
int main(void){
 show_panel(true);assert(demand);show_provision(true);assert(!demand);
 true_publications=0;show_provision(false);assert(!demand&&!true_publications);
 show_panel(true);show_usb(true);assert(!demand);
 true_publications=0;show_usb(false);assert(!demand&&!true_publications);
 show_panel(true);record_visible=true;selected_record.cardless=true;publish_storage_view();assert(!demand);
 selected_record.cardless=false;publish_storage_view();assert(demand);
 show_panel(false);assert(!demand);
}
'''
  code=prefix+'\n'.join(function(source,n) for n in ['static void publish_storage_view(', 'static void show_panel(', 'static void show_usb(', 'static void show_provision('])+checks
  with tempfile.TemporaryDirectory(prefix='sd-view-',dir=ROOT/'private') as d:
   p=Path(d);(p/'view.c').write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',str(p/'view.c'),'-o',str(p/'view')],check=True)
   subprocess.run([str(p/'view')],check=True)
if __name__=='__main__':unittest.main()
