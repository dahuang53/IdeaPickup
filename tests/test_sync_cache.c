#include "memo_sync_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){
 assert(argc==2);const char *p=argv[1];memo_sync_entry_t e={0},out;
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_MISSING);
 e.id=42;e.state=MEMO_JOB_SENT;e.bound=true;e.target.instance.bytes[0]=1;e.target.url_hash[0]=2;
 strcpy(e.target.device_id,"future-board");e.audio.bytes=64044;e.audio.sha256[0]=3;
 assert(memo_sync_entry_write(p,&e));assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&out.state==MEMO_JOB_SENT);
 assert(memo_sync_audio_matches(&e.audio,&out.audio)&&memo_sync_target_matches(&e.target,&out.target,false));
 out.audio.sha256[0]^=1;assert(!memo_sync_audio_matches(&e.audio,&out.audio));out=e;out.target.instance.bytes[0]^=1;assert(!memo_sync_target_matches(&e.target,&out.target,false));
 e.state=MEMO_JOB_FAILED;e.automatic=e.manual=true;assert(memo_sync_entry_write(p,&e));
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&out.state==MEMO_JOB_FAILED&&out.manual&&out.automatic);
 char path[320];snprintf(path,sizeof(path),"%s.1",p);FILE *f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&out.state==MEMO_JOB_SENT); // last complete slot survives
 snprintf(path,sizeof(path),"%s.0",p);f=fopen(path,"r+b");assert(f);fseek(f,128,SEEK_SET);fputc(99,f);fclose(f);
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_INVALID);
 assert(!memo_sync_entry_write("/no-such-parent/memo",&e));
 e.bound=false;e.audio=(memo_audio_identity_t){0};e.target=(memo_sync_target_t){0};assert(memo_sync_entry_write(p,&e));
 assert(memo_sync_entry_read(p,42,&out)==MEMO_CACHE_OK&&!out.bound);
 puts("PASS current sync metadata: full identity, restart/manual intent, independent durable slots, corruption/read errors, no legacy state reuse");
}
