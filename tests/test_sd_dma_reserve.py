"""Execute production board adapter and pinned SDK allocation-failure paths.

Only memory-backed sectors and semaphore stubs: not physical SD/heap evidence.
"""
from pathlib import Path
import os, subprocess, tempfile, unittest

ROOT=Path(__file__).resolve().parents[1]

def function(text, signature):
    start=text.index(signature);brace=text.index('{',start);depth=0
    for i in range(brace,len(text)):
        depth+=(text[i]=='{')-(text[i]=='}')
        if depth==0:return text[start:i+1]
    raise ValueError('Function not closed')

PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_STATE 0x103
#define MALLOC_CAP_DMA 8
#define SOC_SDMMC_PSRAM_DMA_CAPABLE 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define DMA_ATTR
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
typedef int esp_err_t;
typedef int StaticSemaphore_t;
typedef int* SemaphoreHandle_t;
typedef struct {struct {size_t sector_size,capacity;} csd;struct {int slot;bool(*check_buffer_alignment)(int,const void*,size_t);}host;} sdmmc_card_t;
static int takes,gives,locked,allocations,reads,writes,fail_sector=-1;
static unsigned char disk[16][512],direct[3*512];
static bool esp_ptr_dma_capable(const void*);
static bool esp_ptr_external_ram(const void*p){return !esp_ptr_dma_capable(p);}
static bool sdmmc_host_check_buffer_alignment(int s,const void*p,size_t n){(void)s;(void)p;return !(n%4);}
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned t){(void)t;assert(s&&!locked);locked=1;takes++;return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s&&locked);locked=0;gives++;}
static void* heap_caps_malloc(size_t n,unsigned caps){assert(n==512&&caps==MALLOC_CAP_DMA);allocations++;return NULL;}
static size_t heap_caps_get_allocated_size(void*p){(void)p;assert(0);return 0;}
static void model_free(void*p){(void)p;assert(0);}
#define free model_free
static esp_err_t sdmmc_read_sectors_dma(sdmmc_card_t*c,void*p,size_t at,size_t count,size_t len){
 assert(c&&at+count<=16&&esp_ptr_dma_capable(p)&&len>=count*512);reads++;
 if((int)at==fail_sector)return 0x999;
 memcpy(p,disk[at],count*512);return ESP_OK;
}
static esp_err_t sdmmc_write_sectors_dma(sdmmc_card_t*c,const void*p,size_t at,size_t count,size_t len){
 assert(c&&at+count<=16&&esp_ptr_dma_capable(p)&&len>=count*512);writes++;
 if((int)at==fail_sector)return 0x999;
 memcpy(disk[at],p,count*512);return ESP_OK;
}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t*,void*,size_t,size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t*,const void*,size_t,size_t);
'''

TEST=r'''
static bool esp_ptr_dma_capable(const void*p){return p==s_sd_sector||p==direct;}
static void reset_counts(void){takes=gives=allocations=reads=writes=0;locked=0;fail_sector=-1;}
int main(void){
 sdmmc_card_t own={.csd={512,16},.host={.slot=0,.check_buffer_alignment=sd_buffer_dma_safe}},foreign=own;
 foreign.host.check_buffer_alignment=sdmmc_host_check_buffer_alignment;
 s_sd_sector_mutex=&s_sd_sector_mutex_storage;
 unsigned char unsafe[1536];
 for(size_t i=0;i<sizeof(disk);i++)((unsigned char*)disk)[i]=(unsigned char)(i*7+3);
 reset_counts();assert(__real_sdmmc_read_sectors(&own,unsafe,2,3)==ESP_ERR_NO_MEM&&allocations==1&&reads==0);
 reset_counts();assert(__wrap_sdmmc_read_sectors(&own,unsafe,2,3)==ESP_OK);assert(!memcmp(unsafe,disk[2],sizeof(unsafe)));assert(allocations==0&&reads==3&&takes==1&&gives==1);
 memset(unsafe,0x53,sizeof(unsafe));reset_counts();assert(__real_sdmmc_write_sectors(&own,unsafe,5,3)==ESP_ERR_NO_MEM&&allocations==1&&writes==0);
 reset_counts();assert(__wrap_sdmmc_write_sectors(&own,unsafe,5,3)==ESP_OK&&!memcmp(disk[5],unsafe,sizeof(unsafe)));assert(allocations==0&&writes==3&&takes==gives);
 reset_counts();assert(__wrap_sdmmc_read_sectors(&own,direct,2,3)==ESP_OK&&reads==1&&allocations==0&&takes==0);
 reset_counts();assert(__wrap_sdmmc_read_sectors(&foreign,unsafe,2,3)==ESP_ERR_NO_MEM&&allocations==1&&takes==0);
 memset(unsafe,0x77,sizeof(unsafe));reset_counts();fail_sector=3;assert(__wrap_sdmmc_read_sectors(&own,unsafe,2,3)==0x999);assert(!memcmp(unsafe,disk[2],512));for(size_t i=512;i<sizeof(unsafe);i++)assert(unsafe[i]==0x77);assert(reads==2&&takes==gives);
 memset(unsafe,0x48,sizeof(unsafe));unsigned char last[512];memcpy(last,disk[7],512);reset_counts();fail_sector=6;assert(__wrap_sdmmc_write_sectors(&own,unsafe,5,3)==0x999);assert(!memcmp(disk[5],unsafe,512)&&!memcmp(disk[7],last,512));assert(writes==2&&takes==gives);
 reset_counts();assert(__wrap_sdmmc_read_sectors(&own,unsafe,15,2)==ESP_ERR_INVALID_SIZE&&reads==0&&takes==0);assert(__wrap_sdmmc_write_sectors(&own,unsafe,SIZE_MAX,1)==ESP_ERR_INVALID_SIZE);assert(__wrap_sdmmc_read_sectors(&own,NULL,0,1)==ESP_ERR_INVALID_SIZE);
 assert(__wrap_sdmmc_read_sectors(&own,unsafe,0,0)==ESP_OK&&allocations==0);
 s_sd_sector_mutex=NULL;assert(__wrap_sdmmc_read_sectors(&own,unsafe,0,1)==ESP_ERR_INVALID_STATE&&reads==0);
 puts("PASS: real SDK no-DMA-heap red; production multi-sector read/write, no allocations, safe/foreign delegation, failure propagation and bounds");
}
'''

class SDReserveTests(unittest.TestCase):
    def test_actual_adapter_against_pinned_sdk_no_memory(self):
        board=(ROOT/'main/board.c').read_text()
        sdk=(ROOT/'.tools/esp-idf/components/sdmmc/sdmmc_cmd.c').read_text()
        start=board.index('static DMA_ATTR uint8_t s_sd_sector[512];')
        adapter=board[start:board.index('\n#endif',start)]
        functions=[]
        for name,signature in [('read','esp_err_t sdmmc_read_sectors('),('write','esp_err_t sdmmc_write_sectors(')]:
            body=function(sdk,signature).replace('sdmmc_'+name+'_sectors(','__real_sdmmc_'+name+'_sectors(',1)
            functions.append(body)
        code=PREFIX+function(board,'static bool sd_buffer_dma_safe(')+adapter+'\n'.join(functions)+TEST
        with tempfile.TemporaryDirectory(prefix='sd-dma-',dir=ROOT/'private') as temp:
            path=Path(temp);source=path/'test.c';source.write_text(code)
            subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined',str(source),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)

if __name__=='__main__':unittest.main()
