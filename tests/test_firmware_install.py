"""Execute actual bundle/installer/ROM-entry code against synthetic flash/serial.
No real port is opened, no firmware/ROM/hardware acceptance is claimed.
"""
import io,json,hashlib,struct,sys,tempfile,unittest,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from firmware_bundle import ARTIFACTS,PROFILES,FLASH_SIZE,SCHEMAS,ROOT,load_bundle,partition_rows,digest,BundleError
from device_registry import Installer,request_software_rom
from device_rom_return import controlled_return,open_serial


def app_image():
 header=bytearray(24);header[0]=0xe9;header[1]=1;header[12:14]=struct.pack('<H',9);header[23]=1
 payload=bytearray(256);payload[:4]=struct.pack('<I',0xabcd5432);payload[16:20]=b'1.0\0';payload[48:59]=b'meow_inbox\0'
 data=header+struct.pack('<II',0x3f000000,len(payload))+payload
 checksum=0xef
 for byte in payload:checksum^=byte
 end=(len(data)+16)//16*16;data+=b'\0'*(end-len(data)-1)+bytes([checksum]);return bytes(data+hashlib.sha256(data).digest())
def make_bundle(path,board):
 from firmware_bundle import image_identity
 files={}
 for role,(_,offset,_) in ARTIFACTS.items():
  if role=='app':data=app_image()
  elif role=='partition_table':
   data=b''.join(struct.pack('<HBBII16sI',0x50aa,kind,subtype,start,size,label.encode(),flags) for label,kind,subtype,start,size,flags in partition_rows(board));data+=b'\xeb\xeb'+b'\xff'*14+hashlib.md5(data).digest();data=data.ljust(0xc00,b'\xff')
  else:data=(role.encode()+b'-synthetic')*8
  name=role+'.bin';(path/name).write_bytes(data);files[role]={'name':name,'size':len(data),'offset':offset,'sha256':digest(data)}
 value={'manifest_version':1,'board':board,'chip':'esp32s3','flash_size':FLASH_SIZE,'flash_mode':PROFILES[board]['mode'],'console':PROFILES[board]['console'],'schemas':SCHEMAS,'app_identity':image_identity(app_image()),'files':files};(path/'manifest.json').write_text(json.dumps(value));return path/'manifest.json'

class FakeROM:
 def __init__(self,contents):
  self.flash=bytearray(b'\xff'*FLASH_SIZE);self.commands=[];self.chip='ESP32-S3';self.size='16MB';self.protected=False;self.fail_writes=0;self.returns=0
  for role,data in contents.items():self.flash[ARTIFACTS[role][1]:ARTIFACTS[role][1]+len(data)]=data
  self.flash[0x9000:0xa000]=b'private-config'.ljust(4096,b'\xee');self.flash[0xf000:0x10000]=b'factory-secret'.ljust(4096,b'\xdd');self.flash[0xe20000:0xe21000]=b'record-history'.ljust(4096,b'\xcc')
 def __call__(self,command,**kwargs):
  assert '--before' in command and command[command.index('--before')+1]=='no-reset';assert command[command.index('--after')+1]=='no-reset';assert kwargs['env']['ESPTOOL_CFGFILE'].endswith('esptool-protected.cfg')
  args=command[command.index('--no-stub')+1:];action=args[0];self.commands.append(args)
  out=b'';status=0
  if action=='read-mac':out=f'Chip is {self.chip}\nMAC: 01:02:03:04:05:06\n'.encode()
  elif action=='flash-id':out=f'Detected flash size: {self.size}\n'.encode()
  elif action=='get-security-info':out=(b'Flags: 0x00000000\nSecure Boot: Enabled\nFlash Encryption: Disabled\n' if self.protected else b'Flags: 0x00000000\nSecure Boot: Disabled\nFlash Encryption: Disabled\n')
  elif action=='read-flash':offset,size=int(args[1],0),int(args[2],0);Path(args[3]).write_bytes(self.flash[offset:offset+size])
  elif action=='verify-flash':offset=int(args[1],0);data=Path(args[2]).read_bytes();status=int(self.flash[offset:offset+len(data)]!=data)
  elif action=='write-flash':
   assert args[1:7]==['--flash-mode','keep','--flash-freq','keep','--flash-size','keep']
   for at in range(7,len(args),2):
    offset=int(args[at],0);data=Path(args[at+1]).read_bytes();span=(len(data)+4095)//4096*4096
    assert offset in [ARTIFACTS[x][1] for x in ARTIFACTS]
    self.flash[offset:offset+span]=b'\xff'*span;self.flash[offset:offset+len(data)]=data
    if self.fail_writes:self.fail_writes-=1;status=1;break
  else:raise AssertionError(args)
  return subprocess.CompletedProcess(command,status,out)
 def reset(self,port,mac):self.returns+=1;assert mac==bytes([1,2,3,4,5,6]);return 'RESET_REQUESTED_BOOT_NOT_CONFIRMED'

class InstallTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(dir=ROOT/'private/host-tests');self.addCleanup(self.temp.cleanup);self.folder=Path(self.temp.name);self.bundle=self.folder/'bundle';self.bundle.mkdir();self.manifest=make_bundle(self.bundle,'vocat16');self.value,self.contents=load_bundle(self.manifest,'vocat16');self.rom=FakeROM(self.contents);self.run=self.folder/'run';self.run.mkdir()
 def installer(self,mode='app-only'):return Installer(self.manifest,'vocat16','FAKE_ROM_ONLY',mode,self.run,runner=self.rom,returner=self.rom.reset)
 def test_first_install_and_app_update_preserve_project_factory_and_meta(self):
  for mode in ['first-install','app-only']:
   with self.subTest(mode=mode):
    self.rom=FakeROM(self.contents);before=bytes(self.rom.flash);result=self.installer(mode).execute();self.assertEqual(result['status'],'FLASH_VERIFIED');self.assertNotIn('PASS',result['boot']);self.assertFalse(result['data_reset']);
    for offset,size in [(0x9000,0x4000),(0xf000,0xd000),(0xe20000,0x40000)]:self.assertEqual(self.rom.flash[offset:offset+size],before[offset:offset+size])
    writes=[c for c in self.rom.commands if c[0]=='write-flash'];self.assertEqual(len(writes),1);self.assertEqual(len(writes[0][7:])//2,5 if mode=='first-install' else 1)
 def test_wrong_board_artifact_range_tamper_and_schema_reject_before_rom(self):
  for kind in ['board','offset','hash','schema','path','size']:
   value=json.loads(self.manifest.read_text())
   if kind=='board':value['board']='spotpear'
   elif kind=='schema':value['schemas']={**SCHEMAS,'configuration':2}
   else:value['files']['app'][{'offset':'offset','hash':'sha256','path':'name','size':'size'}[kind]]={'offset':0x9000,'hash':'0'*64,'path':'../app.bin','size':0x500000}[kind]
   self.manifest.write_text(json.dumps(value))
   with self.assertRaises((BundleError,KeyError)):self.installer()
   self.assertFalse(self.rom.commands);self.manifest.write_text(json.dumps(self.value))
 def test_real_rom_chip_capacity_security_and_dependencies_refuse_before_write(self):
  for kind in ['chip','size','security','dependency']:
   self.rom=FakeROM(self.contents)
   if kind=='chip':self.rom.chip='ESP32'
   if kind=='size':self.rom.size='32MB'
   if kind=='security':self.rom.protected=True
   if kind=='dependency':self.rom.flash[0x410000]^=1
   with self.assertRaises(BundleError):self.installer().execute()
   self.assertFalse(any(c[0]=='write-flash' for c in self.rom.commands));self.assertEqual(self.rom.returns,0)
 def test_partial_write_restores_exact_original_sectors_and_never_resets_failed_candidate(self):
  before=bytes(self.rom.flash);self.rom.fail_writes=1
  with self.assertRaises(BundleError):self.installer().execute()
  self.assertEqual(bytes(self.rom.flash),before);self.assertEqual(self.rom.returns,0);self.assertEqual(json.loads((self.run/'result.json').read_text())['status'],'FAILED_ORIGINAL_WRITE_REGIONS_RESTORED_IN_ROM')
 def test_recovery_failure_stays_unconfirmed_and_in_rom(self):
  self.rom.fail_writes=2
  with self.assertRaises(BundleError):self.installer().execute()
  self.assertEqual(self.rom.returns,0);self.assertEqual(json.loads((self.run/'result.json').read_text())['status'],'FAILED_RECOVERY_UNCONFIRMED_REMAIN_IN_ROM')
 def test_verified_flash_return_failure_is_not_claimed_as_booted(self):
  def fail(*args):raise BundleError('GPIO0 held')
  installer=self.installer();installer.returner=fail
  with self.assertRaises(BundleError):installer.execute()
  self.assertEqual(json.loads((self.run/'result.json').read_text())['boot'],'RETURN_FAILED_REMAIN_IN_ROM')
 def test_spotpear_bundle_accepted_and_other_board_refused(self):
  spot=self.folder/'spot';spot.mkdir();manifest=make_bundle(spot,'spotpear');load_bundle(manifest,'spotpear')
  with self.assertRaises(BundleError):load_bundle(manifest,'vocat16')

class CLIEntryTests(unittest.TestCase):
 def test_unsupported_legacy_and_missing_arguments_stop_before_devices(self):
  for script,args in [('flash.sh',['FAKE_PORT']),('flash_vocat16.sh',['FAKE_PORT','--usb-console-upgrade']),('flash_spotpear.sh',[])]:
   result=subprocess.run(['bash',str(ROOT/'scripts'/script),*args],capture_output=True,text=True)
   self.assertEqual(result.returncode,2);self.assertTrue(result.stderr)

class PackageTests(unittest.TestCase):
 def test_actual_receipt_package_rejects_stale_source_and_artifact(self):
  from unittest.mock import patch
  import package_firmware,firmware_bundle
  with tempfile.TemporaryDirectory(dir=ROOT/'private/host-tests') as folder:
   root=Path(folder);(root/'main').mkdir();(root/'main/app.c').write_text('synthetic source')
   sdk=root/'.tools/esp-idf';sdk.mkdir(parents=True)
   for where in [root,sdk]:
    subprocess.run(['git','init','-q',str(where)],check=True)
    subprocess.run(['git','-C',str(where),'-c','user.name=Synthetic','-c','user.email=test@example.invalid','commit','--allow-empty','-qm','test'],check=True)
   commit=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
   profile=PROFILES['vocat16']
   for name in ['sdkconfig.vocat16.defaults','sdkconfig.defaults','partitions.csv','partitions.vocat16.csv','dependencies.lock','CMakeLists.txt']:(root/name).write_text('synthetic')
   (root/'sdkconfig.vocat16').write_text('\n'.join(profile['flags']+['CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y','CONFIG_SPIRAM_MODE_OCT=y','CONFIG_ESP_CONSOLE_NONE=y']))
   (root/'tools.lock.json').write_text(json.dumps({'esp_idf':{'commit':commit}}))
   bundle=root/'seed';bundle.mkdir();make_bundle(bundle,'vocat16')
   build=root/profile['build'];build.mkdir();(build/'compile_commands.json').write_text('synthetic compile recipe')
   hashes={}
   for role,(name,_,_) in ARTIFACTS.items():
    target=build/name;target.parent.mkdir(exist_ok=True,parents=True);target.write_bytes((bundle/(role+'.bin')).read_bytes());hashes[name]=digest(target.read_bytes())
   inputs=firmware_bundle.build_inputs(root,'vocat16');(build/'route1-build.json').write_text(json.dumps({**inputs,'artifacts':hashes}))
   with patch.object(package_firmware,'ROOT',root):
    output=package_firmware.package('vocat16',root/'candidate');load_bundle(output,'vocat16')
    (root/'main/app.c').write_text('changed source')
    with self.assertRaises(BundleError):package_firmware.package('vocat16',root/'stale-source')
    (root/'main/app.c').write_text('synthetic source');(build/ARTIFACTS['app'][0]).write_bytes(b'corrupt')
    with self.assertRaises(BundleError):package_firmware.package('vocat16',root/'stale-artifact')

class SerialTests(unittest.TestCase):
 def test_fragmented_ack_refusal_timeout_and_dtr_open(self):
  class Connection:
   def __init__(self,*args,**kwargs):self.values=[b'BOOTLOADER_',b'ENTER explicit_idle_maintenance\n'];self.rts=True;self.dtr=True;self.now=0;self.writes=[]
   def write(self,v):self.writes.append(v);return len(v)
   def readline(self):self.now+=.1;return self.values.pop(0) if self.values else b''
   def open(self):assert not self.rts and not self.dtr
  c=Connection();request_software_rom(c,io.BytesIO(),clock=lambda:c.now);self.assertEqual(c.writes,[b'bootloader\n']);self.assertGreaterEqual(c.now,.7)
  c=Connection();c.values=[b'BOOTLOADER_ERROR busy\n']
  with self.assertRaises(BundleError):request_software_rom(c,io.BytesIO(),clock=lambda:c.now)
  c=Connection();c.values=[]
  with self.assertRaises(ValueError):request_software_rom(c,io.BytesIO(),clock=lambda:c.now,timeout=.3)
  open_serial(Connection,'FAKE_ONLY')
 def test_ack_does_not_hide_delayed_or_same_buffer_failure(self):
  class Connection:
   def __init__(self,lines):self.lines=lines;self.now=0
   def write(self,data):return len(data)
   def readline(self):
    self.now+=.1
    if self.lines:return self.lines.pop(0)
    raise OSError('synthetic USB disconnect')
  for lines in [[b'BOOTLOADER_ENTER explicit_idle_maintenance\n',b'BOOTLOADER_ERROR ESP_ERR_NOT_SUPPORTED\n'],[b'BOOTLOADER_ENTER explicit_idle_maintenance\nBOOTLOADER_ERROR ESP_ERR_INVALID_STATE\n']]:
   c=Connection(lines)
   with self.assertRaises(BundleError):request_software_rom(c,io.BytesIO(),clock=lambda:c.now)
  c=Connection([b'BOOTLOADER_ENTER explicit_idle_maintenance\n']);request_software_rom(c,io.BytesIO(),clock=lambda:c.now)
  c=Connection([])
  with self.assertRaises(BundleError):request_software_rom(c,io.BytesIO(),clock=lambda:c.now)

 def test_actual_rom_return_clears_only_force_bit_and_requests_reset(self):
  class Connection:rts=False;dtr=False
  class ROM:
   CHIP_NAME='ESP32-S3';IS_STUB=False;RTC_CNTL_OPTION1_REG=1;RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK=1
   value=0x101;resets=0;writes=[]
   def connect(self,**kwargs):assert kwargs['mode']=='no-reset'
   def read_mac(self):return bytes([1,2,3,4,5,6])
   def get_security_info(self,**kwargs):return {'parsed_flags':{'SECURE_BOOT_EN':False,'SECURE_DOWNLOAD_ENABLE':False},'flash_crypt_cnt':0}
   def get_uart_no(self):return 4
   def read_reg(self,address):return self.value if address==1 else 1
   def write_reg(self,address,value,mask):assert address==1;self.writes.append((address,value,mask));self.value=self.value&~mask|value&mask
   def watchdog_reset(self):self.resets+=1
  rom=ROM();self.assertIn('NOT_CONFIRMED',controlled_return(Connection(),rom,rom.read_mac()));self.assertEqual(rom.value,0x100);self.assertEqual(rom.resets,1)
  rom=ROM()
  with self.assertRaises(BundleError):controlled_return(Connection(),rom,b'wrong!')
  self.assertEqual(rom.resets,0)

if __name__=='__main__':unittest.main(verbosity=2)
