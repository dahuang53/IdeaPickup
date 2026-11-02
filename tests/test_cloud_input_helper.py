"""Dedicated USB helper with FakeSerial; never connects a device."""
import importlib.util,io,json,sys,unittest,warnings,subprocess,tempfile,re
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('configure_cloud',ROOT/'scripts/configure_cloud.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
class Port:
 def __init__(self,lines):self.lines=list(lines);self.writes=[]
 def write(self,data):self.writes.append(data)
 def readline(self):return self.lines.pop(0) if self.lines else b''
class HelperTests(unittest.TestCase):
 def test_ready_then_single_secret_write_and_redacted_success(self):
  p=Port([b'ordinary status\n',b'CLOUD_CONFIG READY\n',b'CLOUD_CONFIG RESULT 0\n']);payload={'qwen_key':'synthetic-secret','siyuan_token':'synthetic-token'}
  output=io.StringIO()
  with patch('sys.stdout',output):helper.exchange(p,payload)
  self.assertEqual(p.writes[0],b'cloud_config\n');self.assertEqual(json.loads(p.writes[1]),payload);self.assertEqual(len(p.writes),2);self.assertNotIn('synthetic',output.getvalue())
 def test_application_open_asserts_dtr_without_reset_sequence(self):
  class AppPort:
   def __init__(self,**kwargs):self.options=kwargs;self.dtr=None;self.rts=None;self.port=None;self.opened=False
   def open(self):
    self.opened=True;self.assertion=(self.dtr is True and self.rts is False and self.port=='synthetic-port')
  port=helper.open_port(AppPort,'synthetic-port');self.assertTrue(port.opened and port.assertion);self.assertIsNone(port.options['port']);self.assertTrue(port.options['exclusive'])
 def test_busy_and_overflow_never_send_secret_prefix(self):
  p=Port([b'CLOUD_CONFIG BUSY\n'])
  with self.assertRaises(RuntimeError):helper.exchange(p,{'qwen_key':'synthetic'})
  self.assertEqual(p.writes,[b'cloud_config\n']);p=Port([])
  with self.assertRaises(RuntimeError):helper.exchange(p,{'qwen_key':'x'*2000})
  self.assertEqual(p.writes,[])
 def test_failure_does_not_retry_and_never_echoes_transport(self):
  p=Port([b'CLOUD_CONFIG READY\n',b'CLOUD_CONFIG RESULT 3\n'])
  with self.assertRaisesRegex(RuntimeError,'未确认'):helper.exchange(p,{'qwen_key':'synthetic'})
  self.assertEqual(len(p.writes),2)
 def test_timeout_is_bounded_and_no_automatic_resend(self):
  p=Port([])
  with patch.object(helper.time,'monotonic',side_effect=[0,1,20]):
   with self.assertRaisesRegex(RuntimeError,'未进入'):helper.exchange(p,{'qwen_key':'synthetic'})
  self.assertEqual(p.writes,[b'cloud_config\n'])
 def test_actual_cdc_capacity_and_sdk_nonblocking_driver(self):
  from test_service_runtime import extract
  usb=(ROOT/'main/memo_usb_mic.c').read_text();sdk=(ROOT/'.tools/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c').read_text()
  capacity=int(re.search(r'#define CDC_RX_CAPACITY (\d+)',usb)[1]);self.assertGreaterEqual(capacity,1025)
  app=(ROOT/'main/app.c').read_text();self.assertGreaterEqual(int(re.search(r'\.rx_buffer_size=(\d+)',app)[1]),1025)
  code=r"""#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/types.h>
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define USJ_LOCAL_FD 0
#define portMAX_DELAY UINT32_MAX
#define NONE -1
typedef unsigned TickType_t;
static int lock;
static size_t rx_head,rx_tail,rx_count;
static uint8_t cdc_rx[CAPACITY],incoming[1025];
static size_t incoming_at;
static uint32_t tud_cdc_n_available(uint8_t i){(void)i;return sizeof(incoming)-incoming_at;}
static uint32_t tud_cdc_n_read(uint8_t i,void *out,size_t n){(void)i;if(n>sizeof(incoming)-incoming_at)n=sizeof(incoming)-incoming_at;memcpy(out,incoming+incoming_at,n);incoming_at+=n;return n;}
static struct {bool non_blocking;} s_ctx;
static unsigned wait_seen;
static int usb_serial_jtag_read_bytes(uint8_t *c,size_t n,unsigned wait){(void)c;(void)n;wait_seen=wait;return 0;}
""".replace('CAPACITY',str(capacity))
  code+=f'\n#define CDC_RX_CAPACITY {capacity}\n'
  code+='\n'.join(extract(usb,m) for m in ['void tud_cdc_rx_cb(','static ssize_t cdc_read('])
  code+='\n'.join(extract(sdk,m) for m in ['static int usb_serial_jtag_fcntl(','static int usbjtag_rx_char_via_driver('])
  code+=r"""
int main(void){for(size_t i=0;i<sizeof(incoming);i++)incoming[i]=(uint8_t)(i%251);tud_cdc_rx_cb(0);assert(rx_count==1025);uint8_t out[1025];assert(cdc_read(0,out,sizeof(out))==sizeof(out)&&!memcmp(out,incoming,sizeof(out)));assert(cdc_read(0,out,1)==-1&&errno==EAGAIN);
int flags=usb_serial_jtag_fcntl(0,F_GETFL,0);assert(!(flags&O_NONBLOCK));assert(!usb_serial_jtag_fcntl(0,F_SETFL,flags|O_NONBLOCK));assert(usbjtag_rx_char_via_driver(0)==NONE&&wait_seen==0);assert(!usb_serial_jtag_fcntl(0,F_SETFL,flags));assert(usbjtag_rx_char_via_driver(0)==NONE&&wait_seen==UINT32_MAX);}
"""
  with tempfile.TemporaryDirectory(prefix='usb-secret-',dir=ROOT/'private/host-tests') as folder:
   f=Path(folder)/'test.c';f.write_text(code);binary=Path(folder)/'test';subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',str(f),'-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
 def test_hidden_input_refuses_echo_fallback(self):
  def fallback(*args):warnings.warn('fallback',helper.getpass.GetPassWarning);self.fail('must stop before echo')
  with patch.object(helper.getpass,'getpass',side_effect=fallback):
   with self.assertRaises(helper.getpass.GetPassWarning):helper.hidden('fixture')
if __name__=='__main__':unittest.main(verbosity=2)
