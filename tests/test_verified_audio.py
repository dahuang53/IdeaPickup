"""Production bounded identity cache; real filesystem, explicit media/hash shims."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "memo_sync_cache.h"
#include "memo_store.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#define ESP_OK 0
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
static bool busy=false,healthy=true,fail_hash=false,invalidate_during_hash=false;
static unsigned hashes=0,probes=0;
static std::atomic<uint32_t> selected_revision{0};
static bool audio_busy(){return busy;}
static int board_sd_verify_session(){++probes;return healthy?0:1;}
static void invalidate_audio_cache();
static bool file_audio_identity(const char*path,memo_audio_identity_t*out,bool,bool*,uint32_t){
 ++hashes;if(fail_hash)return false;
 FILE*f=fopen(path,"rb");assert(f);*out={};int c;
 while((c=fgetc(f))!=EOF){out->sha256[out->bytes%32]^=(uint8_t)c;out->bytes++;}
 fclose(f);if(invalidate_during_hash)invalidate_audio_cache();return true;
}
'''
MAIN=r'''
static void put(const char*p,const char*s){FILE*f=fopen(p,"wb");assert(f);fputs(s,f);assert(!fclose(f));}
int main(){
 memo_audio_identity_t a{},b{};SDReadContext read{};
 put("a.wav","old audio");assert(hash_audio(&read,"a.wav",&a)&&hashes==1);
 for(int n=0;n<100;n++)assert(hash_audio(&read,"a.wav",&b));
 assert(hashes==1&&probes>=100&&!memcmp(&a,&b,sizeof(a)));
 // Same id/size/timestamps on a different or externally modified card must
 // never reuse an identity across mount notification or failed card status.
 put("a.wav","new audio");invalidate_audio_cache();assert(hash_audio(&read,"a.wav",&b));
 assert(hashes==2&&memcmp(a.sha256,b.sha256,32));
 healthy=false;assert(!hash_audio(&read,"a.wav",&b));healthy=true;
 assert(hash_audio(&read,"a.wav",&b)&&hashes==3);
 put("a.wav","different size");assert(hash_audio(&read,"a.wav",&b)&&hashes==4);
 busy=true;assert(!hash_audio(&read,"a.wav",&b)&&read.interrupted);busy=false;
 read.interrupted=false;selected_revision++;assert(!hash_audio(&read,"a.wav",&b)&&read.interrupted);
 read.selection=selected_revision;read.interrupted=false;
 // Four slots remain fixed. Eviction entails rehash, never stale reuse.
 for(int n=0;n<6;n++){char p[32];snprintf(p,sizeof(p),"%d.wav",n);put(p,p);assert(hash_audio(&read,p,&b));}
 auto before=hashes;assert(hash_audio(&read,"a.wav",&b)&&hashes==before+1);
 invalidate_audio_cache();invalidate_during_hash=true;assert(!hash_audio(&read,"a.wav",&b));
 invalidate_during_hash=false;fail_hash=true;assert(!hash_audio(&read,"a.wav",&b));
 fail_hash=false;before=hashes;assert(hash_audio(&read,"a.wav",&b)&&hashes==before+1);
 assert(sizeof(verified_audio)<2048);
}
'''

class VerifiedAudioTests(unittest.TestCase):
 def test_selected_sd_session_not_merely_successful_cmd13(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'private',prefix='sd-session-') as d:
   p=Path(d);(p/'esp_private').mkdir()
   (p/'esp_private/sdmmc_common.h').write_text('''#pragma once
#include <stdint.h>
typedef int esp_err_t;typedef struct {int unused;} sdmmc_card_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 2
#define MMC_R1_READY_FOR_DATA (1<<8)
#define MMC_R1_CURRENT_STATE_TRAN 4
#define MMC_R1_CURRENT_STATE_STATUS(s) (((s)>>9)&15)
static uint32_t response;static int result;
static int sdmmc_send_cmd_send_status(sdmmc_card_t*c,uint32_t*s){(void)c;*s=response;return result;}
''')
   (p/'test.c').write_text('''#include "memo_sd_session.h"
#include <assert.h>
int main(void){sdmmc_card_t card={0};assert(memo_sd_session_check(0)!=0);
for(unsigned state=0;state<16;state++)for(unsigned ready=0;ready<2;ready++){
response=(state<<9)|(ready<<8);result=0;
assert((memo_sd_session_check(&card)==0)==(state==4&&ready));
result=7;assert(memo_sd_session_check(&card)==7);
}
return 0;
}
''')
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(p),'-I',str(ROOT/'main'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
 def test_bounded_reuse_media_invalidation_and_cancellation(self):
  source=(ROOT/'main/network_sync.cpp').read_text()
  body=source[source.index('struct VerifiedAudio {'):source.index('int sd_status(')]
  with tempfile.TemporaryDirectory(dir=ROOT/'private',prefix='verified-audio-') as d:
   p=Path(d);(p/'test.cpp').write_text(PREFIX+body+MAIN)
   subprocess.run(['c++','-std=c++17','-DMEMO_HOST_TEST','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],cwd=p,check=True)

if __name__=='__main__':unittest.main(verbosity=2)
