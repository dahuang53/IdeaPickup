"""Exercise selections before/during worker publication with production bodies."""
from pathlib import Path
import subprocess,tempfile,unittest,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from test_service_runtime import extract
PRELUDE=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
static void *status_mutex=(void *)1,*detail_task_handle=nullptr,*sync_task_handle=nullptr;
static std::atomic<uint32_t> selected_cardless{0},selected_revision{0},inspect_id{0};
static uint32_t pinned,detail_revision,loaded;static int notices=0;
struct Detail {uint32_t id;int state;};static Detail detail_value;
#define MEMO_CARDLESS_ID_FLAG 0x80000000u
#define MEMO_JOB_CHECKING 3
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_ERR_NO_MEM -1
static unsigned char detail_task_stack[10240],sync_task_stack[16384];static int detail_task_storage,sync_task_storage,sync_core;
void memo_cardless_select(uint32_t id){pinned=id;}
void xSemaphoreTake(void *,int){}void xSemaphoreGive(void *){}
void xTaskNotifyGive(void *h){assert(h);notices++;}
void vTaskDelay(int){}
struct Blocked{};
void ulTaskNotifyTake(int,int){if(!notices)throw Blocked{};notices=0;}
void inspect_record(uint32_t id){loaded=id;}
void sync_task(void*){}
static std::function<void()> during_create;
void *xTaskCreateStaticPinnedToCore(void(*)(void*),const char *,unsigned,void *,int,unsigned char *,int *,int){if(during_create){auto f=during_create;during_create={};f();}return (void *)2;}
'''
class StartupTests(unittest.TestCase):
 def test_selection_during_creation_is_consumed_without_reselect(self):
  source=(ROOT/'main/network_sync.cpp').read_text();a=source.index('    detail_task_handle=xTaskCreateStaticPinnedToCore');b=source.index('    if (!sync_task_handle)',a)
  create='int create_tasks(){\n'+source[a:b]+'\nreturn 0;}'
  code=PRELUDE+extract(source,'extern "C" void network_sync_select_record')+extract(source,'void detail_task(')+create+r'''
void run(){try{detail_task(nullptr);}catch(Blocked&) {}}
void reset(){detail_task_handle=nullptr;notices=loaded=0;inspect_id=0;during_create={};}
int main(){
 reset();network_sync_select_record(61);assert(!notices);assert(!create_tasks());run();assert(loaded==61);
 reset();during_create=[](){network_sync_select_record(61);network_sync_select_record(62);};assert(!create_tasks());run();assert(loaded==62);
 reset();during_create=[](){network_sync_select_record(61);network_sync_select_record(0);};assert(!create_tasks());run();assert(loaded==0&&inspect_id==0);
 reset();assert(!create_tasks());run();assert(loaded==0&&notices==0);
}
'''
  with tempfile.TemporaryDirectory() as d:
   f=Path(d)/'test.cpp';f.write_text(code);binary=Path(d)/'test';subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fsanitize=address,undefined',str(f),'-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main(verbosity=2)
