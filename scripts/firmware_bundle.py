"""Fixed two-board bundle/layout checks shared by build, package and installer.
No device history, card registry, credentials or migration contracts.
"""
from pathlib import Path
import hashlib,json,struct,subprocess
from source_identity import source_identity

ROOT=Path(__file__).resolve().parents[1]
PROFILES={
 'vocat16':{'build':'build-vocat16','config':'sdkconfig.vocat16','mode':'qio','console':'usb-composite-cdc','flags':['CONFIG_MEMO_BOARD_V1_2=y','CONFIG_MEMO_VOCAT16=y','CONFIG_ESPTOOLPY_FLASHMODE_QIO=y']},
 'spotpear':{'build':'build-spotpear','config':'sdkconfig.spotpear','mode':'dio','console':'usb-serial-jtag','flags':['CONFIG_MEMO_BOARD_SPOTPEAR_1_28=y','CONFIG_ESPTOOLPY_FLASHMODE_DIO=y']}}
ARTIFACTS={'bootloader':('bootloader/bootloader.bin',0,0x8000),'partition_table':('partition_table/partition-table.bin',0x8000,0x1000),'otadata':('ota_data_initial.bin',0xd000,0x2000),'app':('meow_inbox.bin',0x20000,0x3f0000),'model':('srmodels/srmodels.bin',0x410000,0x7f0000)}
FLASH_SIZE=0x1000000
SCHEMAS={'configuration':1,'record_ids':1,'sd_tasks':1,'cardless':1}
class BundleError(ValueError):pass

def require(value,reason):
 if not value:raise BundleError(reason)
def digest(data):return hashlib.sha256(data).hexdigest()
def partition_rows(board):
 rows=[('nvs',1,2,0x9000,0x4000,0),('otadata',1,0,0xd000,0x2000,0)]
 rows+= [('nvs_key',1,4,0xf000,0x1000,0),('fctry',1,2,0x10000,0x6000,0),('legacy_nvs',1,2,0x16000,0x6000,0)] if board=='vocat16' else [('phy_init',1,1,0xf000,0x1000,0)]
 return rows+[('factory',0,0,0x20000,0x3f0000,0),('model',1,0x82,0x410000,0x7f0000,0),('memo_meta',1,2,0xe20000,0x40000,0)]
def validate_table(data,board):
 require(len(data)==0xc00,'partition table payload size')
 rows=[]
 for at in range(0,len(data),32):
  row=data[at:at+32]
  if row[:2]==b'\xeb\xeb':
   require(row[:16]==b'\xeb\xeb'+b'\xff'*14 and row[16:]==hashlib.md5(data[:at]).digest(),'partition checksum')
   require(data[at+32:]==b'\xff'*(len(data)-at-32),'partition tail')
   require(rows==partition_rows(board),'partition layout differs from board');return
  require(row[:2]==b'\xaa\x50','partition entry')
  _,kind,subtype,start,size,label,flags=struct.unpack('<HBBII16sI',row)
  rows.append((label.rstrip(b'\0').decode('ascii'),kind,subtype,start,size,flags))
 raise BundleError('partition checksum absent')

def image_identity(data):
 require(len(data)>=288 and data[0]==0xe9 and 1<=data[1]<=16 and data[23]==1,'ESP image header')
 require(struct.unpack_from('<H',data,12)[0]==9,'image chip is not ESP32-S3')
 at,checksum=24,0xef
 for _ in range(data[1]):
  require(at+8<=len(data),'truncated segment');_,size=struct.unpack_from('<II',data,at);at+=8
  require(size<=len(data)-at,'truncated payload')
  for byte in data[at:at+size]:checksum^=byte
  at+=size
 end=(at+16)//16*16
 require(end+32==len(data) and data[end-1]==checksum,'ESP checksum/size')
 require(hashlib.sha256(data[:end]).digest()==data[end:],'ESP digest')
 require(struct.unpack_from('<I',data,32)[0]==0xabcd5432,'app descriptor')
 field=lambda start:data[start:start+32].split(b'\0',1)[0].decode('ascii')
 return {'image_sha256':data[end:].hex(),'version':field(48),'project':field(80)}

def build_inputs(root,board):
 root=Path(root);profile=PROFILES[board]
 paths=[]
 for directory in ['main','components','managed_components']:
  paths.extend(p for p in (root/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
 paths.extend(root/p for p in ['CMakeLists.txt',profile['config'],profile['config']+'.defaults','sdkconfig.defaults','partitions.csv','partitions.vocat16.csv','dependencies.lock','tools.lock.json'])
 inputs={str(p.relative_to(root)):digest(p.read_bytes()) for p in sorted(set(paths))}
 sdk=root/'.tools/esp-idf';expected=json.loads((root/'tools.lock.json').read_text())['esp_idf']['commit']
 require(subprocess.check_output(['git','rev-parse','HEAD'],cwd=sdk,text=True).strip()==expected,'SDK differs from lock')
 require(not subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=sdk,text=True).strip(),'SDK has tracked edits')
 text=(root/profile['config']).read_text().splitlines()
 for flag in profile['flags']+['CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y','CONFIG_SPIRAM_MODE_OCT=y']:
  require(flag in text,'wrong effective board config: '+flag)
 require(('CONFIG_ESP_CONSOLE_NONE=y' if board=='vocat16' else 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y') in text,'wrong console')
 return {'board':board,'source_commit':source_identity(root),'idf_commit':expected,'inputs_sha256':digest(json.dumps(inputs,sort_keys=True).encode()),'sdkconfig_sha256':inputs[profile['config']],'compile_recipe_sha256':digest((root/profile['build']/'compile_commands.json').read_bytes())}

def load_bundle(manifest,board):
 path=Path(manifest).resolve();value=json.loads(path.read_text());profile=PROFILES[board]
 require(value.get('manifest_version')==1 and value.get('board')==board,'wrong bundle/board')
 require(value.get('chip')=='esp32s3' and value.get('flash_size')==FLASH_SIZE and value.get('flash_mode')==profile['mode'] and value.get('console')==profile['console'],'wrong chip/flash/console')
 require(value.get('schemas')==SCHEMAS,'unsupported persistent format; use a compatible application')
 require(set(value['files'])==set(ARTIFACTS),'unexpected bundle artifacts')
 contents={}
 for role,(_,offset,limit) in ARTIFACTS.items():
  item=value['files'][role];name=item['name']
  require(isinstance(name,str) and Path(name).name==name and name not in ['.','..'],'artifact path')
  file=path.parent/name;require(file.is_file() and not file.is_symlink(),'missing/symlink artifact')
  data=file.read_bytes();require(item['offset']==offset and item['size']==len(data) and 0<len(data)<=limit,'artifact bounds')
  require(digest(data)==item['sha256'],'artifact hash mismatch');contents[role]=data
 validate_table(contents['partition_table'],board)
 identity=image_identity(contents['app'])
 require(identity==value['app_identity'] and identity['project']=='meow_inbox','app identity')
 # Bootloader/model are bound by their exact recorded bytes; do not attribute
 # extra digest/checksum validation to esptool write_flash. Updates independently
 # verify their actual ROM bytes against these recorded dependencies.
 return value,contents
