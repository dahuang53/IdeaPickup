"""Compile actual direct configuration/secret parser with NVS/RTOS boundaries."""
from pathlib import Path
import subprocess
from test_service_runtime import ROOT

def compile_objects(temp,includes):
    objects=[]
    sources=['main/memo_activity.c','main/memo_cloud_config.cpp','main/memo_cloud_settings.cpp','main/memo_cloud_input.cpp','main/memo_cloud.cpp','main/memo_cloud_json.c','.tools/esp-idf/components/json/cJSON/cJSON.c']
    for i,name in enumerate(sources):
        obj=temp/f'config-{i}.o';objects.append(str(obj));cpp=name.endswith('.cpp')
        defines=['-DESP_PLATFORM'] if name=='main/memo_cloud_config.cpp' else ['-DMEMO_CLOUD_TEST'] if name=='main/memo_cloud.cpp' else []
        subprocess.run(['c++' if cpp else 'cc','-std=c++17' if cpp else '-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-DMEMO_HOST_TEST','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*defines,*includes,'-c',str(ROOT/name),'-o',str(obj)],check=True)
    return objects
