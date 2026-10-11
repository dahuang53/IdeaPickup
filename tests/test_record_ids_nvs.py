"""Run the real ID NVS adapter; platform storage/failure cuts are explicit shims."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_service_runtime import HEADERS, ROOT
HARNESS=r'''
#include "memo_record_ids.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
static std::vector<uint8_t> durable,pending;
static int fault=0;static bool committed=false,ready=false;static unsigned writes=0,reads=0;
extern "C" {
SemaphoreHandle_t xSemaphoreCreateMutex(){return (void*)1;}
int xSemaphoreTake(SemaphoreHandle_t,int){return 1;}
int xSemaphoreGive(SemaphoreHandle_t){return 1;}
esp_err_t nvs_flash_init(){ready=true;return ESP_OK;}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){assert(ready&&!strcmp(ns,"memo_ids1"));*h=1;if(fault==1&&mode==NVS_READWRITE)return ESP_FAIL;if(mode==NVS_READWRITE){pending=durable;committed=false;}return ESP_OK;}
void nvs_close(nvs_handle_t){}
esp_err_t nvs_get_blob(nvs_handle_t,const char *key,void *out,size_t *n){assert(!strcmp(key,"floor"));reads++;if(fault==5&&committed)return ESP_FAIL;if(durable.empty())return ESP_ERR_NVS_NOT_FOUND;if(*n<durable.size())return ESP_FAIL;*n=durable.size();memcpy(out,durable.data(),*n);return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t,const char *key,const void *p,size_t n){assert(!strcmp(key,"floor")&&n==16);writes++;if(fault==2)return ESP_FAIL;pending.assign((const uint8_t*)p,(const uint8_t*)p+n);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){committed=true;if(fault==3)return ESP_FAIL;durable=pending;return fault==4?ESP_FAIL:ESP_OK;}
}
int main(){
 assert(memo_record_ids_init());uint32_t id=0;assert(memo_record_ids_reserve(0,&id)&&id==1);
 uint32_t previous=id;
 for(int cut=1;cut<=5;cut++){
  fault=cut;assert(!memo_record_ids_reserve(0,&id)&&id==0);
  unsigned before=writes;fault=5;committed=true;assert(!memo_record_ids_reserve(0,&id)&&id==0&&writes==before);
  fault=0;committed=false;assert(memo_record_ids_reserve(0,&id)&&(cut==5?id>previous:id>previous+1));previous=id;
 }
 assert(memo_record_ids_observe(100));assert(memo_record_ids_reserve(0,&id)&&id==101);
 // Corruption discovered while RAM was valid still blocks future writes.
 durable[4]=2;
 unsigned before=writes;assert(!memo_record_ids_reserve(0,&id)&&id==0&&writes==before);
 assert(!memo_record_ids_observe(0)&&writes==before);
 assert(reads>writes);puts("PASS actual ID NVS adapter: namespace isolation, fresh readback, five faults/two-stage recovery, no rollback, observation, corrupt format never overwritten");
}
'''
class IdNVSTests(unittest.TestCase):
    def test_actual_adapter_faults(self):
        with tempfile.TemporaryDirectory(prefix='ids-nvs-',dir=ROOT/'private/host-tests') as directory:
            temp=Path(directory)
            for name,text in HEADERS.items():
                if name=='freertos/semphr.h':text=text.replace('SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);','SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);\nSemaphoreHandle_t xSemaphoreCreateMutex(void);')
                file=temp/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(text)
            source=temp/'test.cpp';source.write_text(HARNESS);objects=[]
            for name in ('memo_record_ids.c','memo_record_ids_nvs.c'):
                obj=temp/(name+'.o');objects.append(str(obj));subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(temp),'-I',str(ROOT/'main'),'-c',str(ROOT/'main'/name),'-o',str(obj)],check=True)
            binary=temp/'test';subprocess.run(['c++','-std=c++17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(temp),'-I',str(ROOT/'main'),str(source),*objects,'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main(verbosity=2)
