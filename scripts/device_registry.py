#!/usr/bin/env python3
"""Two-board installer; no registry or legacy qualification is required.
Explicit ROM port/board and selected bundle; all raw data remain private.
"""
import argparse,json,os,re,subprocess,sys,tempfile,time
from pathlib import Path
from firmware_bundle import ROOT,PROFILES,ARTIFACTS,FLASH_SIZE,digest,require,load_bundle
from device_rom_return import open_serial,reset

ANSI=re.compile(rb'\x1b\[[0-9;]*m')
def request_software_rom(connection,log,clock=time.monotonic,timeout=8):
 require(connection.write(b'bootloader\n')==11,'short software download command')
 deadline=clock()+timeout;pending=bytearray();acknowledged=False
 while clock()<deadline:
  try:chunk=connection.readline()
  except OSError:
   require(acknowledged,'serial lost before software entry acknowledgement');return
  if not chunk:continue
  log.write(chunk);log.flush();pending.extend(chunk);require(len(pending)<65536,'serial response too long')
  while b'\n' in pending:
   line,_,rest=pending.partition(b'\n');pending=bytearray(rest);line=ANSI.sub(b'',line).strip()
   require(not line.startswith((b'BOOTLOADER_ERROR',b'BOOTLOADER_REFUSED')),'software entry refused: '+line.decode(errors='replace'))
   if line.startswith(b'BOOTLOADER_ENTER '):
    acknowledged=True;deadline=clock()+.5
 # ENTER acknowledges a request, not its result. Keep the CDC open through the
 # application's reset window and read late errors; ROM is confirmed separately.
 require(acknowledged,'software entry timed out; no write authorized')


def software_rom(port,run):
 import serial
 # The application CDC gate requires DTR asserted through acknowledgement.
 connection=serial.Serial(port=port,baudrate=115200,timeout=.1,write_timeout=2,exclusive=True)
 try:
  connection.dtr=True
  with (run/'serial.log').open('wb') as log:request_software_rom(connection,log)
 finally:connection.close()

class Installer:
 def __init__(self,manifest,board,port,mode,run,runner=subprocess.run,returner=reset):
  self.value,self.contents=load_bundle(manifest,board);self.board=board;self.port=port;self.mode=mode;self.run=Path(run);self.runner=runner;self.returner=returner;self.mac=None
  require(mode in ['first-install','app-only'],'unsupported install mode')
  self.roles=list(ARTIFACTS) if mode=='first-install' else ['app']
  self.frozen={}
  for role in ARTIFACTS:
   path=self.run/(role+'.bin');path.write_bytes(self.contents[role]);self.frozen[role]=path
 def rom(self,*args):
  command=[str(ROOT/'.venv/bin/python'),'-m','esptool','--chip','esp32s3','--port',self.port,'--baud','115200','--before','no-reset','--after','no-reset','--no-stub',*map(str,args)]
  env=os.environ.copy();env['ESPTOOL_CFGFILE']=str(ROOT/'scripts/esptool-protected.cfg')
  result=self.runner(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180,env=env)
  with (self.run/'rom.log').open('ab') as log:log.write(result.stdout)
  require(result.returncode==0,'esptool failed; raw result in private ROM log')
  return result.stdout.decode(errors='replace')
 def read(self,offset,size,label):
  chunks=[]
  for at in range(0,size,0x40000):
   n=min(0x40000,size-at);p=self.run/(label+'-'+str(at)+'.bin');self.rom('read-flash',hex(offset+at),hex(n),p);data=p.read_bytes();require(len(data)==n,'short ROM read');chunks.append(data)
  return b''.join(chunks)
 def verify(self,offset,data,label):
  for at in range(0,len(data),0x40000):
   p=self.run/(label+'-'+str(at)+'.bin');p.write_bytes(data[at:at+0x40000]);self.rom('verify-flash',hex(offset+at),p)
 def preflight(self):
  text=self.rom('read-mac');match=re.search(r'MAC:\s*([0-9a-fA-F:]{17})',text);require(match,'ROM MAC missing');self.mac=bytes.fromhex(match[1].replace(':',''))
  require('ESP32-S3' in text,'wrong physical chip')
  require('Detected flash size: 16MB' in self.rom('flash-id'),'physical flash must be 16MB')
  security=self.rom('get-security-info')
  flags=re.search(r'Flags: (0x[0-9a-fA-F]+)',security)
  require(flags and not int(flags[1],16)&4 and 'Secure Boot: Disabled' in security and 'Flash Encryption: Disabled' in security,'protected/unknown device security')
  if self.mode=='app-only':
   for role in ['bootloader','partition_table','model']:
    self.verify(ARTIFACTS[role][1],self.contents[role],'dependency-'+role)
 def execute(self):
  self.preflight()
  before={}
  if self.mode=='first-install':
   full=self.read(0,FLASH_SIZE,'original-flash');(self.run/'original-flash.bin').write_bytes(full)
  for role in self.roles:
   offset=ARTIFACTS[role][1];span=(len(self.contents[role])+4095)//4096*4096
   before[role]=full[offset:offset+span] if self.mode=='first-install' else self.read(offset,span,'original-'+role)
   (self.run/('original-'+role+'.bin')).write_bytes(before[role])
  protected=[(0x9000,0x4000),(0xf000,0xd000),(0xe20000,0x40000)]
  baseline={offset:self.read(offset,size,'protected-'+str(offset)) for offset,size in protected}
  written=False
  try:
   args=[]
   for role in self.roles:args.extend([hex(ARTIFACTS[role][1]),self.frozen[role]])
   written=True;self.rom('write-flash','--flash-mode','keep','--flash-freq','keep','--flash-size','keep',*args)
   for role in self.roles:self.verify(ARTIFACTS[role][1],self.contents[role],'written-'+role)
   for offset,size in protected:require(self.read(offset,size,'after-protected-'+str(offset))==baseline[offset],'protected bytes changed')
  except Exception as error:
   if written:
    try:
     args=[]
     for role in self.roles:args.extend([hex(ARTIFACTS[role][1]),self.run/('original-'+role+'.bin')])
     self.rom('write-flash','--flash-mode','keep','--flash-freq','keep','--flash-size','keep',*args)
     for role in self.roles:self.verify(ARTIFACTS[role][1],before[role],'restored-'+role)
     for offset,size in protected:require(self.read(offset,size,'restored-protected-'+str(offset))==baseline[offset],'protected bytes not restored')
     status='FAILED_ORIGINAL_WRITE_REGIONS_RESTORED_IN_ROM'
    except Exception:status='FAILED_RECOVERY_UNCONFIRMED_REMAIN_IN_ROM'
   else:status='FAILED_NO_WRITE'
   (self.run/'result.json').write_text(json.dumps({'status':status,'error':str(error)}));raise
  result={'status':'FLASH_VERIFIED','boot':'NOT_REQUESTED','mode':self.mode,'board':self.board,'app_sha256':digest(self.contents['app']),'data_reset':False}
  (self.run/'result.json').write_text(json.dumps(result,indent=2)+'\n')
  try:result['boot']=self.returner(self.port,self.mac)
  except Exception as error:
   result['boot']='RETURN_FAILED_REMAIN_IN_ROM';result['return_error']=str(error)
   (self.run/'result.json').write_text(json.dumps(result,indent=2)+'\n');raise
  (self.run/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result

def private_run():
 parent=ROOT/'private/install-runs';parent.mkdir(parents=True,exist_ok=True,mode=0o700);return Path(tempfile.mkdtemp(dir=parent))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',choices=PROFILES,required=True);p.add_argument('--port',required=True);p.add_argument('--bundle');p.add_argument('--mode',choices=['first-install','app-only','software-rom'],default='app-only');a=p.parse_args();os.umask(0o077)
 require(a.port and not a.port.startswith('-'),'explicit serial port required')
 if a.mode=='software-rom':
  require(a.board=='vocat16','Spotpear uses its USB Serial/JTAG ROM entry, not the composite CDC command');run=private_run();software_rom(a.port,run);print('ROM_REQUEST_ACKNOWLEDGED; identify the newly enumerated ROM port before flashing');return
 require(a.bundle,'--bundle manifest.json required');load_bundle(a.bundle,a.board)
 import importlib.metadata
 require(importlib.metadata.version('esptool')=='5.1.0','Run project bootstrap: esptool version differs from lock')
 run=private_run();result=Installer(a.bundle,a.board,a.port,a.mode,run).execute();print(json.dumps(result));print('private run:',run)
if __name__=='__main__':main()
