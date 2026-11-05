"""Production sidecar codec under the ESP locking branch and FatFs sharing rules.

RTOS mutexes and FatFs open denial are explicit host shims; no device claim.
"""
import ctypes as C
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class MetadataLockTests(unittest.TestCase):
    def test_two_readers_and_writer_do_not_overlap_sidecar_descriptors(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'private', prefix='metadata-lock-') as d:
            p=Path(d);(p/'freertos').mkdir()
            (p/'freertos/FreeRTOS.h').write_text('''#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
#define portMAX_DELAY 0
#define pdTRUE 1
''')
            (p/'freertos/semphr.h').write_text('''#pragma once
#include "FreeRTOS.h"
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s){return pthread_mutex_init(s,0)?0:s;}
static inline int xSemaphoreTake(SemaphoreHandle_t s,int wait){(void)wait;return pthread_mutex_lock(s)==0;}
static inline void xSemaphoreGive(SemaphoreHandle_t s){pthread_mutex_unlock(s);}
''')
            (p/'sharing.c').write_text('''#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static struct {FILE *f;char path[1024];int write;} slots[8];
static int conflicts;
FILE *fatfs_open(const char *path,const char *mode){
 pthread_mutex_lock(&guard);int w=mode[0]!='r',free_slot=-1;
 for(int i=0;i<8;i++){
  if(!slots[i].f){free_slot=i;continue;}
  if(!strcmp(slots[i].path,path)&&(w||slots[i].write)){
   conflicts++;pthread_mutex_unlock(&guard);errno=EACCES;return 0;
  }
 }
 FILE *f=free_slot<0?0:fopen(path,mode);
 if(f){slots[free_slot].f=f;slots[free_slot].write=w;snprintf(slots[free_slot].path,1024,"%s",path);}
 pthread_mutex_unlock(&guard);if(f)usleep(100);return f;
}
int fatfs_close(FILE *f){
 pthread_mutex_lock(&guard);for(int i=0;i<8;i++)if(slots[i].f==f)slots[i].f=0;
 int r=fclose(f);pthread_mutex_unlock(&guard);return r;
}
int sharing_conflicts(void){return conflicts;}
''')
            common=['cc','-std=c11','-D_DEFAULT_SOURCE','-DMEMO_HOST_TEST','-fPIC','-g','-O1','-Wall','-Wextra','-Werror','-I',str(p),'-I',str(ROOT/'main')]
            (p/'codec.cpp').write_text('''#include <cstdio>
extern "C" FILE *fatfs_open(const char *,const char *);
extern "C" int fatfs_close(FILE *);
#define fopen fatfs_open
#define fclose fatfs_close
#include "memo_direct_cache.cpp"
''')
            subprocess.run(['c++',*common[2:],'-std=c++17','-DESP_PLATFORM','-c',str(p/'codec.cpp'),'-o',str(p/'codec.o')],check=True)
            subprocess.run([*common,'-c',str(p/'sharing.c'),'-o',str(p/'sharing.o')],check=True)
            (p/'wrapper.cpp').write_text('#include "memo_sd_sync.h"\nextern "C" bool direct_write(const char*p,int state){memo_sd::Entry e;e.id=1;e.submission=e.qwen_generation=e.siyuan_generation=1;e.file_bytes=100;e.intent=true;e.state=static_cast<memo_job_state_t>(state);return memo_sd::write(p,e); }\nextern "C" int direct_read(const char*p){memo_sd::Entry e;return memo_sd::read(p,1,&e)==MEMO_CACHE_OK?int(e.state):-1;}\n')
            subprocess.run(['c++',*common[2:],'-std=c++17','-c',str(p/'wrapper.cpp'),'-o',str(p/'wrapper.o')],check=True)
            subprocess.run(['c++','-shared',str(p/'codec.o'),str(p/'sharing.o'),str(p/'wrapper.o'),'-lpthread','-o',str(p/'codec.so')],check=True)
            lib=C.CDLL(str(p/'codec.so'))
            lib.direct_write.argtypes=[C.c_char_p,C.c_int];lib.direct_write.restype=C.c_bool
            lib.direct_read.argtypes=[C.c_char_p];lib.direct_read.restype=C.c_int
            path=str(p).encode();self.assertTrue(lib.direct_write(path,2))
            def writer():
                for n in range(120):self.assertTrue(lib.direct_write(path,2 if n%2 else 5))
            def reader():
                for _ in range(200):self.assertIn(lib.direct_read(path),(2,5))
            with ThreadPoolExecutor(3) as pool:
                results=[pool.submit(f) for f in (writer,reader,reader)]
                for result in results:result.result()
            self.assertEqual(lib.sharing_conflicts(),0)

if __name__=='__main__':unittest.main(verbosity=2)
