#!/usr/bin/env python3
"""Package a completed, unchanged two-board candidate; no private device data."""
import argparse,json,shutil
from pathlib import Path
from firmware_bundle import ROOT,PROFILES,ARTIFACTS,FLASH_SIZE,SCHEMAS,build_inputs,digest,require,image_identity,validate_table

def package(board,output):
 profile=PROFILES[board];build=ROOT/profile['build'];receipt=json.loads((build/'route1-build.json').read_text())
 current=build_inputs(ROOT,board);require(all(receipt.get(k)==v for k,v in current.items()),'stale build; rerun documented build')
 files={}
 for role,(name,offset,limit) in ARTIFACTS.items():
  data=(build/name).read_bytes();require(digest(data)==receipt['artifacts'].get(name),'artifact differs from completed build')
  require(0<len(data)<=limit,'artifact exceeds permitted range')
  files[role]={'name':Path(name).name,'offset':offset,'size':len(data),'sha256':digest(data)}
 validate_table((build/ARTIFACTS['partition_table'][0]).read_bytes(),board)
 identity=image_identity((build/ARTIFACTS['app'][0]).read_bytes());require(identity['project']=='meow_inbox','wrong application')
 output=Path(output);require(not output.exists(),'output must be a new directory');output.mkdir(parents=True)
 for role,(name,_,_) in ARTIFACTS.items():shutil.copyfile(build/name,output/files[role]['name'])
 manifest={'manifest_version':1,'board':board,'chip':'esp32s3','flash_size':FLASH_SIZE,'flash_mode':profile['mode'],'console':profile['console'],'app_identity':identity,'schemas':SCHEMAS,'build':current,'files':files,'validation':'CANDIDATE_NOT_DEVICE_ACCEPTED'}
 (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');return output/'manifest.json'
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=PROFILES,required=True);p.add_argument('--output',required=True);a=p.parse_args();print(package(a.target,a.output))
