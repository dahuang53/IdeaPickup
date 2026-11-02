"""Actual direct SD engine + checkpoint codec + cloud protocol, synthetic I/O.

Relay provider tests stay in server-relay; the old no-card consumer has its own
shared fixture until step 4. No live provider, serial or personal audio is used.
"""
from pathlib import Path
import atexit
import base64
import io
import json
import subprocess
import tempfile
import unittest
import wave

ROOT=Path(__file__).resolve().parents[1]
_build=None
def native_binary():
    global _build
    if _build is not None:return Path(_build.name)/'sd'
    (ROOT/'private/host-tests').mkdir(parents=True,exist_ok=True)
    _build=tempfile.TemporaryDirectory(prefix='sd-direct-',dir=ROOT/'private/host-tests')
    atexit.register(_build.cleanup)
    p=Path(_build.name);sdk=ROOT/'.tools/esp-idf/components/json/cJSON'
    (p/'nvs.h').write_text('''#pragma once
#include <stddef.h>
#define NVS_READONLY 0
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
typedef int nvs_handle_t;
typedef int esp_err_t;
#ifdef __cplusplus
extern "C" {
#endif
int nvs_open(const char*,int,int*);int nvs_get_blob(int,const char*,void*,size_t*);void nvs_close(int);
#ifdef __cplusplus
}
#endif
''')
    (p/'fault.h').write_text('''#include <cstdio>
#include <unistd.h>
extern "C" FILE*sd_test_fopen(const char*,const char*);
extern "C" int sd_test_fclose(FILE*);
extern "C" size_t sd_test_fread(void*,size_t,size_t,FILE*);
extern "C" size_t sd_test_fwrite(const void*,size_t,size_t,FILE*);
extern "C" int sd_test_fsync(int);
extern "C" int sd_test_unlink(const char*);
#define fopen sd_test_fopen
#define fclose sd_test_fclose
#define fread sd_test_fread
#define fwrite sd_test_fwrite
#define fsync sd_test_fsync
#define unlink sd_test_unlink
''')
    objects=[]
    names=['main/memo_transcript.c','main/memo_store.c','main/memo_sync_cache.c','tests/legacy_sync_fixture.c','main/network_sync_state.c',
           'main/memo_cloud_json.c','main/memo_cloud.cpp','main/memo_cloud_config.cpp','main/memo_direct_cache.cpp',
           'main/memo_sd_sync.cpp','tests/sd_direct_native.cpp']
    for n,name in enumerate(names):
        cpp=name.endswith('.cpp');obj=p/f'{n}.o';objects.append(str(obj))
        extra=['-DESP_PLATFORM'] if name=='main/memo_cloud_config.cpp' else []
        if name in ('main/memo_direct_cache.cpp','main/memo_sd_sync.cpp'):extra+=['-include',str(p/'fault.h')]
        subprocess.run(['c++' if cpp else 'cc','-std=c++17' if cpp else '-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE',
                        '-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'main'),
                        '-I'+str(ROOT/'tests'),'-I'+str(sdk),'-I'+str(p),*extra,'-c',str(ROOT/name),'-o',str(obj)],check=True)
    subprocess.run(['c++','-fsanitize=address,undefined',*objects,'-o',str(p/'sd')],check=True)
    return p/'sd'

def run_case(name):
    binary=native_binary()
    with tempfile.TemporaryDirectory(prefix='sd-case-',dir=ROOT/'private/host-tests') as folder:
        subprocess.run([str(binary),name,folder],check=True)
        if name=='segments':
            p=Path(folder)
            with wave.open(str(p/'memo_0000000001.wav'),'rb') as original:
                total=original.getnframes();offset=0;parts=sorted(p.glob('asr-*.json'))
                assert len(parts)>=3
                for path in parts:
                    body=json.loads(path.read_text());encoded=body['messages'][0]['content'][0]['input_audio']['data']
                    raw=base64.b64decode(encoded.split(',',1)[1],validate=True)
                    with wave.open(io.BytesIO(raw),'rb') as segment:
                        assert segment.getparams()[:3]==(1,2,16000)
                        n=segment.getnframes();assert 0<n<=180*16000
                        assert segment.readframes(n)==original.readframes(n)
                        offset+=n
                assert offset==total
            delivered=json.loads((p/'delivery.json').read_text())
            assert delivered['title']=='喵伴语音笔记 #1'
            assert delivered['content'].startswith('**喵伴 #1** · 云端导入时间 ')
            assert delivered['content'].endswith('\n\n'.join(f'segment {n+1}' for n in range(len(parts))))

class SyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):native_binary()
for _name in ('segments','offline','details','legacy','retry','clock','limited','limited_boot','limited_long',
              'credentials','invalid','empty','uncertain','lost','delivery_retry','crash_asr','crash_delivery',
              'cancel_offline','cancel_asr','cancel_success','cancel_lost','config','qwen_config','pause',
              'storage','cleanup','codec','scan','cycles','config_blob','failed_rebind','text_limit','scan_partial','legacy_future','audio_read_fail','media_401','media_301','media_headers_401','media_headers_403','id_floor','wait_429','wait_503','wait_408','wait_429_reboot','wait_503_reboot','wait_408_reboot','timeout_401','timeout_403','timeout_400','limited_clock','clean','clean_empty'):
    setattr(SyncTests,'test_'+_name,lambda self,name=_name:run_case(name))

if __name__=='__main__':unittest.main(verbosity=2)
