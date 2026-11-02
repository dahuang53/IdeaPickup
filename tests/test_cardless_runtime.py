"""Production direct cardless runtime + protocol/codec with explicit platform shims.
No real provider, microphone, radio, power or device memory claims.
"""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
HEADER=r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_TYPE_BLOB 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
typedef int nvs_handle_t;
typedef void *SemaphoreHandle_t;
typedef struct {uint8_t *data;size_t cap,head,used;} StaticStreamBuffer_t;
typedef StaticStreamBuffer_t *StreamBufferHandle_t;
typedef void *nvs_iterator_t;
typedef struct {char key[16];} nvs_entry_info_t;
typedef struct {size_t free_entries;} nvs_stats_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
StreamBufferHandle_t xStreamBufferCreateStatic(size_t,size_t,uint8_t *,StaticStreamBuffer_t *);
void vStreamBufferDelete(StreamBufferHandle_t);
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t);
size_t xStreamBufferSend(StreamBufferHandle_t,const void *,size_t,unsigned);
size_t xStreamBufferReceive(StreamBufferHandle_t,void *,size_t,unsigned);
void *heap_caps_malloc(size_t,int);
void *heap_caps_calloc(size_t,size_t,int);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned);
esp_err_t nvs_flash_init_partition(const char *);
esp_err_t nvs_open_from_partition(const char *,const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_get_stats(const char *,nvs_stats_t *);
esp_err_t nvs_entry_find(const char *,const char *,int,nvs_iterator_t *);
esp_err_t nvs_entry_next(nvs_iterator_t *);
esp_err_t nvs_entry_info(nvs_iterator_t,nvs_entry_info_t *);
void nvs_release_iterator(nvs_iterator_t);
#ifdef __cplusplus
}
#endif
'''
ERRORS='''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_NO_MEM 4
'''
class CardlessRuntimeTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory(prefix='cardless-direct-',dir=ROOT/'private/host-tests');p=Path(cls.temp.name);cls.binary=p/'run'
  (p/'esp_err.h').write_text(ERRORS);(p/'shim.h').write_text(HEADER)
  for name in ['esp_heap_caps.h','esp_timer.h','nvs.h','nvs_flash.h','freertos/FreeRTOS.h','freertos/semphr.h','freertos/stream_buffer.h','freertos/task.h']:
   f=p/name;f.parent.mkdir(exist_ok=True);f.write_text('#include "shim.h"\n')
  objs=[]
  for i,name in enumerate(['main/memo_cloud_json.c','main/memo_cloud.cpp','main/memo_cardless_record.c','main/memo_transcript.c','tests/legacy_cardless_fixture.c','tests/cardless_direct_native.cpp']):
   out=p/f'{i}.o';objs.append(str(out));cpp=name.endswith('.cpp')
   subprocess.run(['c++' if cpp else 'cc','-std=c++17' if cpp else '-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-DMEMO_CLOUD_TEST','-DMEMO_CARDLESS_TEST','-DMEMO_HOST_TEST','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'tests'),'-I'+str(ROOT/'main'),'-I'+str(ROOT/'.tools/esp-idf/components/json/cJSON'),'-c',str(ROOT/name),'-o',str(out)],check=True)
  subprocess.run(['c++','-fsanitize=address,undefined',*objs,'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def check(self,case):subprocess.run([str(self.binary),case],check=True)
for case in ['success','no_network','credentials','queue_full','send_block','disconnect','text_limit','final_timeout','drain_timeout','cancel_capture','cancel_delivery','cancel_success','uncertain','delivery_retry','delivery_limited','metadata_begin','metadata_terminal','capacity','history','boot','iterations','memory','metadata_commit','mixed_full','config_stop','orphan','cancel_audio','cross_rate','cancel_handshake','slow_handshake','handshake_timeout','mixed_capacity','stop_interleave','capture_error']:
 setattr(CardlessRuntimeTests,'test_'+case,lambda self,c=case:self.check(c))
if __name__=='__main__':unittest.main(verbosity=2)
