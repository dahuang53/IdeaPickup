"""Run the production shared HTTP scheduler with RTOS/media/transport shims.

Verifies no-card progress, cadence, and SD exclusion while the stream owns audio.
Protocol/ACK/persistence are covered separately by test_cardless_runtime.py.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_service_runtime import extract

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include "network_sync.h"
#include "network_sync_state.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <vector>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 10000
#define ESP_LOGI(...) ((void)0)
constexpr int RETRY_DELAY_MS=15000,NEW_RECORD_SETTLE_MS=1000;
static memo_network_status_t status_value{};
static memo_sync_queue_t sync_queue{};
static std::atomic<bool> storage_available{false},power_suspended{false},sd_scan_requested{false};
static std::atomic<int64_t> latest_change_us{0};
static std::atomic<uint32_t> active_record{0},cancel_request{0};
static std::atomic<memo_job_state_t> active_state{MEMO_JOB_LOCAL};
static int status_mutex=1,sync_task_handle=1;
static int steps=0,rf=0,scans=0,sd_jobs=0,pairings=0,remaining=0,stop_after=0;
static int64_t clock_us=1000000;
static bool ap=false,audio=false;
static int cancel_failures=0,cancel_calls=0;
static std::vector<int> waits;
struct Done{};
struct WifiManager {
 static WifiManager& GetInstance(){static WifiManager w;return w;}
 void ProcessConfigConnection(){rf++;}
 bool IsConnected(){return true;}
 bool IsConfigMode(){return ap;}
};
struct HttpGuard {};
static void status_update(const std::function<void(memo_network_status_t&)>& f){f(status_value);}
extern "C" void network_sync_get_status(memo_network_status_t* s){*s=status_value;}
static int64_t esp_timer_get_time(){return clock_us;}
static void recover_pairing(){pairings++;}
static void memo_cardless_network_step(){steps++;if(remaining>0)remaining--;}
static bool memo_cardless_busy(){return remaining!=0;}
static bool audio_busy(){return audio;}
static void xSemaphoreTake(int,int){}
static void xSemaphoreGive(int){}
static void xTaskNotifyGive(int){}
static bool persist_manual(uint32_t){return true;}
static void job_state(uint32_t,memo_job_state_t){}
extern "C" bool network_sync_current_target(memo_sync_target_t*){return true;}
static bool discover_sd_work(const memo_sync_target_t&){assert(!memo_cardless_busy());scans++;return true;}
static bool sync_one(uint32_t,bool,bool=false,bool cancel=false){assert(!memo_cardless_busy());sd_jobs++;if(cancel){cancel_calls++;if(cancel_failures-->0)return false;}active_state=MEMO_JOB_SENT;return true;}
static void ulTaskNotifyTake(int,int ms){waits.push_back(ms);clock_us+=ms*1000;if(steps>=stop_after)throw Done{};}
'''
MAIN = r'''
static void run(int count){stop_after=count;try{sync_task(nullptr);}catch(Done&){}assert(steps==count&&rf==count);}
static void reset(){status_value={};status_value.configured=true;sync_queue={};steps=rf=scans=sd_jobs=pairings=0;remaining=0;power_suspended=false;cancel_request=0;cancel_failures=cancel_calls=0;waits.clear();ap=audio=false;storage_available=false;clock_us=1000000;}
int main(){
 reset();remaining=99;run(4);assert(scans==0&&sd_jobs==0);assert((waits==std::vector<int>{250,250,250,250}));
 reset();run(3);assert(scans==0&&sd_jobs==0);assert((waits==std::vector<int>{15000,15000,15000}));
 reset();remaining=3;storage_available=true;sync_queue.active_id=7;run(3);assert(sd_jobs==1&&scans==1);assert((waits==std::vector<int>{250,250,15000}));
 reset();ap=true;status_value.provisioning=true;storage_available=true;run(2);assert(!scans&&!sd_jobs);assert((waits==std::vector<int>{1000,1000}));
 reset();audio=true;storage_available=true;run(2);assert(!scans&&!sd_jobs&&steps==2);
 reset();storage_available=true;cancel_request=7;ap=true;status_value.provisioning=true;run(2);assert(cancel_calls==0&&cancel_request==7);
 reset();storage_available=true;cancel_request=7;power_suspended=true;run(2);assert(cancel_calls==0&&cancel_request==7);
 reset();storage_available=true;cancel_request=7;remaining=99;run(2);assert(cancel_calls==0&&cancel_request==7);
 reset();storage_available=true;cancel_request=7;cancel_failures=1;run(3);assert(cancel_calls==2&&cancel_request==0);
}
'''

class SharedWorkerTests(unittest.TestCase):
    def test_stream_progress_without_sd_and_exclusion_during_stream(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);source=path/'scheduler.cpp';obj=path/'state.o';binary=path/'scheduler'
            source.write_text(PRELUDE+extract((ROOT/'main/network_sync.cpp').read_text(),'void sync_task(')+MAIN)
            subprocess.run(['cc','-std=c11','-DMEMO_HOST_TEST','-I',str(ROOT/'main'),'-c',str(ROOT/'main/network_sync_state.c'),'-o',str(obj)],check=True)
            subprocess.run(['c++','-std=c++17','-DMEMO_HOST_TEST','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I',str(ROOT/'main'),str(source),str(obj),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main(verbosity=2)
