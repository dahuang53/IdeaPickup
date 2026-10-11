#include "memo_record_ids.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t mutex;
static memo_ids_t ids;
static const char *space="memo_ids1";
static memo_ids_result_t read_floor(void *ctx,uint32_t *floor){
 (void)ctx;nvs_handle_t h;esp_err_t e=nvs_open(space,NVS_READONLY,&h);
 if(e==ESP_ERR_NVS_NOT_FOUND)return MEMO_IDS_MISSING;
 if(e!=ESP_OK)return MEMO_IDS_IO_ERROR;
 uint8_t bytes[16];size_t n=sizeof(bytes);e=nvs_get_blob(h,"floor",bytes,&n);nvs_close(h);
 if(e==ESP_ERR_NVS_NOT_FOUND)return MEMO_IDS_MISSING;
 if(e!=ESP_OK)return MEMO_IDS_IO_ERROR;
 return memo_record_ids_decode_fact(bytes,n,floor)?MEMO_IDS_OK:MEMO_IDS_INVALID;
}
static memo_ids_result_t write_floor(void *ctx,uint32_t floor){
 (void)ctx;uint8_t bytes[16];if(!memo_record_ids_encode_fact(floor,bytes,sizeof(bytes)))return MEMO_IDS_INVALID;
 nvs_handle_t h;if(nvs_open(space,NVS_READWRITE,&h)!=ESP_OK)return MEMO_IDS_IO_ERROR;
 esp_err_t e=nvs_set_blob(h,"floor",bytes,sizeof(bytes));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);
 uint32_t actual=0;return e==ESP_OK&&read_floor(NULL,&actual)==MEMO_IDS_OK&&actual==floor?MEMO_IDS_OK:MEMO_IDS_IO_ERROR;
}
bool memo_record_ids_init(void){
 if(mutex)return ids.loaded;
 mutex=xSemaphoreCreateMutex();if(!mutex)return false;
 const memo_ids_storage_t io={NULL,read_floor,write_floor};ids.storage=io;
 if(nvs_flash_init()!=ESP_OK)return false;
 return memo_ids_load(&ids,&io)==MEMO_IDS_OK;
}
bool memo_record_ids_reserve(uint32_t floor,uint32_t *id){
 if(!id)return false;
 *id=0;if(!mutex)return false;
 xSemaphoreTake(mutex,portMAX_DELAY);
 bool ok=nvs_flash_init()==ESP_OK&&memo_ids_reserve(&ids,floor,id)==MEMO_IDS_OK;
 xSemaphoreGive(mutex);return ok;
}

bool memo_record_ids_observe(uint32_t floor){
 if(!mutex||floor>MEMO_RECORD_ID_MAX)return false;
 xSemaphoreTake(mutex,portMAX_DELAY);uint32_t durable=0;
 memo_ids_result_t r=nvs_flash_init()==ESP_OK?read_floor(NULL,&durable):MEMO_IDS_IO_ERROR;
 bool ok=false;ids.loaded=false;
 if(r==MEMO_IDS_OK||(r==MEMO_IDS_MISSING&&!ids.highwater)){
  uint32_t maximum=ids.highwater>durable?ids.highwater:durable;
  if(floor>maximum)maximum=floor;
  ids.highwater=maximum;ok=write_floor(NULL,maximum)==MEMO_IDS_OK;ids.loaded=ok;
 }
 xSemaphoreGive(mutex);return ok;
}
