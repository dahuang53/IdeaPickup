#include "memo_transcript.h"
#include <stdint.h>
#include <string.h>
static size_t width(const char *s,size_t n){unsigned c=(unsigned char)*s;return c<128?1:c<224&&n>=2?2:c<240&&n>=3?3:n>=4?4:1;}
static bool space(const char *s,size_t n){
 if(n==1){unsigned c=(unsigned char)*s;return (c>=9&&c<=13)||(c>=28&&c<=32);}
 return (n==2&&(!memcmp(s,"\xc2\x85",2)||!memcmp(s,"\xc2\xa0",2)))||
  (n==3&&(!memcmp(s,"\xe1\x9a\x80",3)||!memcmp(s,"\xe2\x80\xa8",3)||!memcmp(s,"\xe2\x80\xa9",3)||!memcmp(s,"\xe2\x80\xaf",3)||!memcmp(s,"\xe2\x81\x9f",3)||!memcmp(s,"\xe3\x80\x80",3)||
  ((unsigned char)s[0]==0xe2&&(unsigned char)s[1]==0x80&&(unsigned char)s[2]>=0x80&&(unsigned char)s[2]<=0x8a)));
}
static bool separator(const char *s,size_t n,bool colon){
 return space(s,n)||(n==1&&(strchr(",.!?",*s)||(colon&&*s==':')))||
  (n==3&&(!memcmp(s,"，",3)||!memcmp(s,"。",3)||!memcmp(s,"！",3)||!memcmp(s,"？",3)||!memcmp(s,"、",3)||(colon&&!memcmp(s,"：",3))));
}
static size_t skip(const char *s,size_t first,size_t end,bool colon){while(first<end){size_t n=width(s+first,end-first);if(!separator(s+first,n,colon))break;first+=n;}return first;}
static size_t back(const char *s,size_t first,size_t end,bool colon){while(end>first){size_t at=end-1;while(at>first&&((unsigned char)s[at]&0xc0)==0x80)at--;if(!separator(s+at,end-at,colon))break;end=at;}return end;}
static const char *const wakes[]={"你好喵伴","喵喵同学","小鹿小鹿"};
static size_t wake_start(const char *s,size_t first,size_t end){for(unsigned i=0;i<3;i++){size_t n=strlen(wakes[i]);if(n<=end-first&&!memcmp(s+first,wakes[i],n))return first+n;}return first;}
static size_t wake_end(const char *s,size_t first,size_t end){for(unsigned i=0;i<3;i++){size_t n=strlen(wakes[i]);if(n<=end-first&&!memcmp(s+end-n,wakes[i],n))return end-n;}return end;}
size_t memo_transcript_clean(const char *s,size_t n,memo_text_sink_t sink,void *ctx){
 if(!s&&n)return SIZE_MAX;
 size_t first=0,end=n;
 // CONTROL_START: only the first edge wake, then its separators.
 size_t at=skip(s,first,end,false),after=wake_start(s,at,end);
 if(after!=at)first=skip(s,after,end,true);
 // CONTROL_END_COMMAND, preceded by any edge wake phrases.
 at=back(s,first,end,false);static const char *const commands[]={"保存录音","结束录音","停止录音","保存"};
 for(unsigned i=0;i<4;i++){size_t len=strlen(commands[i]);if(len<=at-first&&!memcmp(s+at-len,commands[i],len)){
   end=back(s,first,at-len,true);while(end>first){size_t before=wake_end(s,first,end);if(before==end)break;end=back(s,first,before,true);}break;
 }}
 // CONTROL_END_WAKE: a trailing run of wake phrases and separators.
 at=back(s,first,end,false);size_t before=wake_end(s,first,at);
 if(before!=at){end=back(s,first,before,true);while(end>first){before=wake_end(s,first,end);if(before==end)break;end=back(s,first,before,true);}}
 first=skip(s,first,end,false);end=back(s,first,end,false);
 char block[256];size_t used=0,total=0;
 for(size_t i=first;i<end;i++){char c=s[i];if(c=='\r'){c='\n';if(i+1<end&&s[i+1]=='\n')i++;}block[used++]=c;total++;
  if(used==sizeof(block)){if(sink&&!sink(ctx,block,used))return SIZE_MAX;used=0;}}
 if(used&&sink&&!sink(ctx,block,used))return SIZE_MAX;
 return total;
}
