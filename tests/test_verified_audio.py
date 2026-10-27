"""Detail metadata validation has constant audio I/O, independent of recording length."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class DetailIOTests(unittest.TestCase):
 def test_validator_reads_only_wav_header_for_large_recording(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory)
   (p/'probe.c').write_text(r'''#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "memo_store.h"
static size_t audio_bytes;
static size_t measured_read(void*p,size_t a,size_t b,FILE*f){audio_bytes+=a*b;return fread(p,a,b,f);}
#define fread measured_read
#include "memo_store.c"
#undef fread
int main(void){
 uint8_t h[44]={0};memcpy(h,"RIFF",4);memcpy(h+8,"WAVEfmt ",8);memcpy(h+36,"data",4);
 uint32_t n=64*1024*1024;put32(h+4,n+36);put32(h+16,16);put16(h+20,1);put16(h+22,1);
 put32(h+24,16000);put32(h+28,32000);put16(h+32,2);put16(h+34,16);put32(h+40,n);
 FILE*f=fopen("memo_0000000001.wav","wb");assert(f);assert(fwrite(h,1,44,f)==44);assert(!ftruncate(fileno(f),n+44));fclose(f);
 memo_record_t record;assert(!memo_store_validate("memo_0000000001.wav",&record));assert(record.id==1);assert(record.data_bytes==n);assert(audio_bytes==44);
}
''')
   subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DMEMO_HOST_TEST','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(p/'probe.c'),'-o',str(p/'probe')],check=True)
   subprocess.run([str(p/'probe')],cwd=p,check=True)
if __name__=='__main__':unittest.main(verbosity=2)
