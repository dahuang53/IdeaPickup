"""Host FD-routing regression with locked SDK VFS/console function bodies.

Only synthetic descriptors and payloads are used; no device or actual firmware
is executed. SDK sources stay untouched. This does not validate hardware USB.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IDF = ROOT / ".tools/esp-idf"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for position in range(brace, len(text)):
        depth += (text[position] == "{") - (text[position] == "}")
        if depth == 0:
            return text[start:position + 1]
    raise AssertionError("unterminated SDK function")


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "memo_usb_stdio.h"
#define MAX_FDS 32
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_VFS_FLAG_READONLY_FS 1
#define LEN_PATH_PREFIX_IGNORED 0
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define _lock_acquire(p) ((void)(p))
#define _lock_release(p) ((void)(p))
typedef int esp_err_t;
struct _reent {int error;};
#define __errno_r(r) ((r)->error)
typedef struct {int vfs_index,local_fd;bool permanent,has_pending_select,has_pending_close;} fd_entry_t;
#define FD_TABLE_ENTRY_UNUSED ((fd_entry_t){.vfs_index=-1})
typedef struct {int (*open)(const char*,int,int);int(*close)(int);ssize_t(*write)(int,const void*,size_t);} esp_vfs_t;
typedef struct {int offset,flags;int(*open)(const char*,int,int);int(*close)(int);ssize_t(*write)(int,const void*,size_t);} vfs_entry_t;
static fd_entry_t s_fd_table[MAX_FDS];static int s_fd_table_lock;
static vfs_entry_t *s_vfs[16];static int close_calls;
static unsigned char wire[4096];static size_t wire_size;
static struct _reent reent;
static int model_open(const char*,int,int);
static int model_close(int);
static ssize_t model_write(int,const void*,size_t);
static int console_open(const char*,int,int);
static int console_close(int);
static ssize_t console_write(int,const void*,size_t);
static int plain_open(const char*p,int f,int m){(void)p;(void)f;(void)m;return 0;}
static int plain_close(int f){(void)f;return 0;}
static ssize_t discard(int f,const void*p,size_t n){(void)f;(void)p;return n;}
static ssize_t capture(int f,const void*p,size_t n){(void)f;assert(wire_size+n<=sizeof(wire));memcpy(wire+wire_size,p,n);wire_size+=n;return n;}
static const vfs_entry_t *get_vfs_for_path(const char*p){
 if(!strcmp(p,"/dev/console"))return s_vfs[2];
 if(!strcmp(p,"/dev/secondary"))return s_vfs[1];
 return s_vfs[0];
}
static const vfs_entry_t *get_vfs_for_fd(int fd){return fd>=0&&fd<MAX_FDS&&s_fd_table[fd].vfs_index>=0?s_vfs[s_fd_table[fd].vfs_index]:NULL;}
static int get_local_fd(const vfs_entry_t*v,int fd){return v?s_fd_table[fd].local_fd:-1;}
static const char *translate_path(const vfs_entry_t*v,const char*p){(void)v;return p;}
#define CHECK_AND_CALL(result,r,vfs,member,...) do{(void)(r);(result)=(vfs)->member(__VA_ARGS__);}while(0)
static esp_err_t esp_vfs_register_common(const char*p,int len,const esp_vfs_t*v,void*ctx,int*index){
 (void)p;(void)len;(void)ctx;assert(!s_vfs[3]);s_vfs[3]=calloc(1,sizeof(vfs_entry_t));assert(s_vfs[3]);
 *s_vfs[3]=(vfs_entry_t){.offset=3,.open=v->open,.close=v->close,.write=v->write};*index=3;return ESP_OK;
}
'''

MODEL = r'''
static int model_open(const char*p,int f,int m){return esp_vfs_open(&reent,p,f,m);}
static int model_close(int fd){++close_calls;return esp_vfs_close(&reent,fd);}
static ssize_t model_write(int fd,const void*p,size_t n){return esp_vfs_write(&reent,fd,p,n);}
typedef struct {int fd;} model_file;
static model_file files[3];static unsigned file_count;
typedef struct {model_file *in,*out,*err;} model_reent;
static model_reent global;
#define _GLOBAL_REENT (&global)
#define _REENT_STDIN(r) ((r)->in)
#define _REENT_STDOUT(r) ((r)->out)
#define _REENT_STDERR(r) ((r)->err)
#define ESP_ROM_NEEDS_SWSETUP_WORKAROUND 0
static model_file *model_fopen(const char*p,const char*m){(void)m;assert(file_count<3);files[file_count].fd=model_open(p,O_RDWR,0);return &files[file_count++];}
#define fopen model_fopen
'''

SUFFIX = r'''
#undef fopen
#undef stdin
#undef stdout
#undef stderr
#define stdin global.in
#define stdout global.out
#define stderr global.err
#define fileno(f) ((f)->fd)
#define clearerr(f) ((void)(f))
#undef FD_SETSIZE
#define FD_SETSIZE MAX_FDS
#define close(fd) model_close(fd)
#define ESP_ERR_INVALID_STATE -2
#define ESP_FAIL -3
static int rebind(bool old){
 const esp_vfs_t cdc={.open=plain_open,.close=plain_close,.write=capture};
 if(old){model_close(0);model_close(1);model_close(2);return esp_vfs_register_fd_range(&cdc,NULL,0,3);}
 // PRODUCTION_STDIO_BIND
}
int main(int argc,char**argv){
 assert(argc==2);const char*mode=argv[1];
 for(int i=0;i<MAX_FDS;++i)s_fd_table[i]=FD_TABLE_ENTRY_UNUSED;
 for(int i=0;i<3;++i){s_vfs[i]=calloc(1,sizeof(vfs_entry_t));assert(s_vfs[i]);
  *s_vfs[i]=(vfs_entry_t){.offset=i,.open=plain_open,.close=plain_close,.write=discard};}
 s_vfs[2]->open=console_open;s_vfs[2]->close=console_close;s_vfs[2]->write=console_write;
 bool unrelated=!strcmp(mode,"unrelated");int unrelated_fd=unrelated?model_open("/dev/unrelated",O_RDWR,0):-1;
 esp_libc_init_global_stdio("/dev/console");
 int original[3]={global.in->fd,global.out->fd,global.err->fd};
 bool invalid=!strcmp(mode,"duplicate")||!strcmp(mode,"negative")||!strcmp(mode,"gap")||!strcmp(mode,"limit");
 if(!strcmp(mode,"duplicate"))global.out->fd=global.in->fd;
 if(!strcmp(mode,"negative"))global.out->fd=-1;
 if(!strcmp(mode,"gap"))global.out->fd=global.err->fd+2;
 if(!strcmp(mode,"limit"))global.out->fd=MAX_FDS;
 fd_entry_t before[MAX_FDS];memcpy(before,s_fd_table,sizeof(before));int prior_close=close_calls;
 int result=rebind(!strcmp(mode,"old"));
 if(invalid){assert(result==-2&&close_calls==prior_close&&!memcmp(before,s_fd_table,sizeof(before))&&!s_vfs[3]);
  printf("{\"rejected\":true,\"fd_table_unchanged\":true,\"close_calls\":0}\n");
 }else{
  assert(result==0);if(unrelated)assert(s_fd_table[unrelated_fd].vfs_index==0);
  unsigned char payload[870];for(size_t i=0;i<sizeof(payload);++i)payload[i]='A'+(i/128+i%23)%26;
  memcpy(payload,"INSTALL_STATUS ",15);payload[869]='\n';
  for(size_t i=0;i<sizeof(payload);i+=128){size_t n=sizeof(payload)-i;if(n>128)n=128;assert(model_write(global.out->fd,payload+i,n)==(ssize_t)n);}
  size_t line_size=0;while(line_size<wire_size&&wire[line_size]!='\n')++line_size;if(line_size<wire_size)++line_size;
  unsigned pairs=0;for(unsigned i=0;i<6;++i)if(!memcmp(wire+i*256,wire+i*256+128,128))++pairs;
  bool exact=wire_size==sizeof(payload)&&!memcmp(wire,payload,sizeof(payload));
  printf("{\"stdio_fds\":[%d,%d,%d],\"wire_bytes\":%zu,\"first_line_bytes\":%zu,\"adjacent_128_pairs\":%u,\"exact_single_copy\":%s,\"unrelated_fd_preserved\":%s}\n",
   original[0],original[1],original[2],wire_size,line_size,pairs,exact?"true":"false",unrelated?"true":"false");
 }
 for(unsigned i=0;i<16;++i)free(s_vfs[i]);return 0;
}
'''


class USBStdioSDKTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("clang")
        if not cc or not (IDF / "components/vfs/vfs.c").is_file():
            raise unittest.SkipTest("NOT_RUN: locked SDK/host compiler unavailable")
        lock = json.loads((ROOT / "tools.lock.json").read_text())["esp_idf"]["commit"]
        actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=IDF, text=True).strip()
        if lock != actual:
            raise AssertionError("SDK differs from project lock")
        if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=IDF, text=True).strip():
            raise AssertionError("SDK contains tracked changes")
        parent = ROOT / "private/host-tests/usb-stdio"
        parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        console = (IDF / "components/esp_vfs_console/vfs_console.c").read_text()
        vfs = (IDF / "components/vfs/vfs.c").read_text()
        libc = (IDF / "components/newlib/src/newlib_init.c").read_text()
        source = PREFIX
        source += "\n".join(function(vfs, signature) for signature in (
            "int esp_vfs_open(", "ssize_t esp_vfs_write(", "int esp_vfs_close(", "esp_err_t esp_vfs_register_fd_range("))
        source += "\n#define open model_open\n#define write model_write\n#define close(fd) model_close(fd)\n"
        source += "\ntypedef struct {int fd_primary,fd_secondary;} vfs_console_context_t;\nstatic vfs_console_context_t vfs_console;\nstatic size_t s_open_count;\n"
        source += "\n".join(function(console, signature) for signature in (
            "int console_open(", "ssize_t console_write(", "int console_close("))
        source += "\n#undef open\n#undef write\n#undef close\n" + MODEL
        source += function(libc, "void esp_libc_init_global_stdio(const char *stdio_dev)") + SUFFIX
        production=(ROOT / "main/memo_usb_mic.c").read_text()
        start=production.index("    if (!stdin || !stdout || !stderr)")
        end=production.index("    memo_usb_pcm_t *new_pcm",start)
        bind=production.index("    if (fileno(stdin) != stdio_fds[0]")
        finish=production.index("    portENTER_CRITICAL(&lock);",bind)
        source=source.replace("// PRODUCTION_STDIO_BIND",
            "const esp_vfs_t cdc_vfs=cdc;esp_err_t err;\n"+production[start:end]+production[bind:finish]+"return ESP_OK;")
        cls.binaries = {}
        for secondary in (0, 1):
            code = cls.work / f"secondary-{secondary}.c"
            code.write_text(source)
            binary = cls.work / f"secondary-{secondary}"
            result = subprocess.run([cc, "-std=c11", "-fsanitize=address,undefined", "-DCONFIG_VFS_SUPPORT_IO=1",
                f"-DCONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG={secondary}", "-I" + str(ROOT / "main"), str(code), "-o", str(binary)],
                capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[secondary] = binary

    def run_route(self, secondary, mode):
        result = subprocess.run([str(self.binaries[secondary]), mode], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stderr)
        return json.loads(result.stdout)

    def test_locked_secondary_console_reproduces_128_pairs_then_rebinds_one_copy(self):
        old = self.run_route(1, "old")
        self.assertEqual(old["stdio_fds"], [2, 3, 4])
        self.assertEqual(old["first_line_bytes"], 1638)
        self.assertEqual(old["first_line_bytes"] - 1, 1637)  # parser line excludes LF
        self.assertEqual(old["adjacent_128_pairs"], 6)
        self.assertFalse(old["exact_single_copy"])
        fixed = self.run_route(1, "new")
        self.assertEqual(fixed["stdio_fds"], [2, 3, 4])
        self.assertEqual(fixed["wire_bytes"], 870)
        self.assertTrue(fixed["exact_single_copy"])

    def test_secondary_none_also_rebinds_actual_stdio(self):
        for mode in ("old", "new"):
            result = self.run_route(0, mode)
            self.assertEqual(result["stdio_fds"], [1, 2, 3])
            self.assertTrue(result["exact_single_copy"])

    def test_preexisting_unrelated_fd_is_preserved(self):
        for secondary in (0, 1):
            result = self.run_route(secondary, "unrelated")
            self.assertTrue(result["exact_single_copy"])
            self.assertTrue(result["unrelated_fd_preserved"])

    def test_bad_stdio_descriptor_sets_reject_before_any_close_or_registration(self):
        for secondary in (0, 1):
            for mode in ("duplicate", "negative", "gap", "limit"):
                with self.subTest(secondary=secondary, mode=mode):
                    result = self.run_route(secondary, mode)
                    self.assertTrue(result["rejected"])
                    self.assertTrue(result["fd_table_unchanged"])
                    self.assertEqual(result["close_calls"], 0)


if __name__ == "__main__":
    unittest.main()
