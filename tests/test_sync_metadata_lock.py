"""Production sidecar codec under the ESP locking branch and FatFs sharing rules.

RTOS mutexes and FatFs open denial are explicit host shims; no device claim.
"""
import ctypes as C
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_sync_pipeline import Entry, ROOT

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
            subprocess.run([*common,'-DESP_PLATFORM','-Dfopen=fatfs_open','-Dfclose=fatfs_close','-c',str(ROOT/'main/memo_sync_cache.c'),'-o',str(p/'codec.o')],check=True)
            subprocess.run([*common,'-c',str(p/'sharing.c'),'-o',str(p/'sharing.o')],check=True)
            subprocess.run(['cc','-shared',str(p/'codec.o'),str(p/'sharing.o'),'-lpthread','-o',str(p/'codec.so')],check=True)
            lib=C.CDLL(str(p/'codec.so'))
            lib.memo_sync_entry_write.argtypes=[C.c_char_p,C.POINTER(Entry)];lib.memo_sync_entry_write.restype=C.c_bool
            lib.memo_sync_entry_read.argtypes=[C.c_char_p,C.c_uint32,C.POINTER(Entry)]
            path=str(p/'test.job').encode();e=Entry();e.id=1;e.state=2;e.automatic=True
            self.assertTrue(lib.memo_sync_entry_write(path,C.byref(e)))
            def writer():
                for n in range(120):
                    entry=Entry();entry.id=1;entry.state=2 if n%2 else 1;entry.automatic=True
                    self.assertTrue(lib.memo_sync_entry_write(path,C.byref(entry)))
            def reader():
                for _ in range(200):
                    entry=Entry();self.assertEqual(lib.memo_sync_entry_read(path,1,C.byref(entry)),1)
                    self.assertIn(entry.state,(1,2))
            with ThreadPoolExecutor(3) as pool:
                results=[pool.submit(f) for f in (writer,reader,reader)]
                for result in results:result.result()
            self.assertEqual(lib.sharing_conflicts(),0)

if __name__=='__main__':unittest.main(verbosity=2)
