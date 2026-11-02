#include "memo_cloud.h"
#include "memo_cloud_json_names.h"
#include "cJSON.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <new>
#include <ctime>
#ifdef MEMO_CLOUD_TEST
extern "C" void *memo_cloud_test_allocate(size_t);
extern "C" void memo_cloud_test_free(void *);
#endif
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace memo_cloud {
namespace {
constexpr size_t RESPONSE=65536,TEXT=262144,SEGMENT=16384,JSON_HEAP=196608;
void *allocate(size_t n) {
#ifdef MEMO_CLOUD_TEST
    return memo_cloud_test_allocate(n);
#else
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
#endif
}
void release_memory(void *p){
#ifdef MEMO_CLOUD_TEST
    memo_cloud_test_free(p);
#else
    free(p);
#endif
}
struct Buffer {
    char *p;
    explicit Buffer(size_t n):p(static_cast<char *>(allocate(n))) {}
    ~Buffer(){release_memory(p);}
    Buffer(const Buffer &)=delete;
};
struct Secret {
    char value[288]{};
    ~Secret(){volatile char *p=value;for(size_t i=0;i<sizeof(value);i++)p[i]=0;}
};
bool credentials(const char *p) {
    if(!p)return false;
    size_t n=0;
    for(;p[n] && n<=256;n++)if(static_cast<unsigned char>(p[n])<33 || static_cast<unsigned char>(p[n])>126)return false;
    return n>0 && n<=256;
}
bool utf8(const char *s,size_t n) {
    for(size_t i=0;i<n;){
        unsigned c=static_cast<unsigned char>(s[i++]);
        if(c<128){if(!c)return false;continue;}
        unsigned value;size_t tail;
        if(c>=0xc2 && c<=0xdf){value=c&31;tail=1;}
        else if(c>=0xe0 && c<=0xef){value=c&15;tail=2;}
        else if(c>=0xf0 && c<=0xf4){value=c&7;tail=3;}
        else return false;
        if(n-i<tail)return false;
        for(size_t j=0;j<tail;j++){unsigned b=static_cast<unsigned char>(s[i++]);if((b&0xc0)!=0x80)return false;value=(value<<6)|(b&63);}
        if((tail==1 && value<128)||(tail==2 && value<2048)||(tail==3 && value<65536)||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
    }
    return true;
}
bool blank(const char *p){for(;*p;p++)if(*p!=' '&&*p!='\n'&&*p!='\r'&&*p!='\t')return false;return true;}
bool json_number(const char *p,size_t n){
    size_t i=0;if(i<n&&p[i]=='-')i++;
    if(i==n)return false;
    if(p[i]=='0')i++;
    else {if(p[i]<'1'||p[i]>'9')return false;while(i<n&&p[i]>='0'&&p[i]<='9')i++;}
    if(i<n&&p[i]=='.'){size_t start=++i;while(i<n&&p[i]>='0'&&p[i]<='9')i++;if(i==start)return false;}
    if(i<n&&(p[i]=='e'||p[i]=='E')){i++;if(i<n&&(p[i]=='+'||p[i]=='-'))i++;size_t start=i;while(i<n&&p[i]>='0'&&p[i]<='9')i++;if(i==start)return false;}
    return i==n;
}
// Allocation upper bound before the existing cJSON parser: count every value,
// container AND property string as a node (overestimate). Bound nesting too.
bool json_budget(const char *s,size_t n) {
    size_t nodes=0;int depth=0;bool string=false,escape=false,token=false;
    for(size_t i=0;i<n;i++){
        char c=s[i];
        if(string){if(static_cast<unsigned char>(c)<32)return false;if(escape){if(c=='u'&&i+4<n&&!memcmp(s+i+1,"0000",4))return false;escape=false;}else if(c=='\\')escape=true;else if(c=='"')string=false;continue;}
        if(static_cast<unsigned char>(c)<32&&c!='\t'&&c!='\r'&&c!='\n')return false;
        if(c=='"'){string=true;nodes++;token=false;}
        else if(c=='{'||c=='['){if(++depth>16)return false;nodes++;token=false;}
        else if(c=='}'||c==']'){if(--depth<0)return false;token=false;}
        else if(c==','||c==':'||c==' '||c=='\r'||c=='\n'||c=='\t')token=false;
        else if(!token){
            nodes++;token=true;
            if(c=='-'||(c>='0'&&c<='9')){
                size_t end=i+1;
                while(end<n&&s[end]!=','&&s[end]!='}'&&s[end]!=']'&&s[end]!=' '&&s[end]!='\t'&&s[end]!='\r'&&s[end]!='\n')end++;
                if(!json_number(s+i,end-i))return false;
                i=end-1;
            }
        }
        if(2*n+nodes*sizeof(cJSON)>JSON_HEAP)return false;
    }
    return !string && !depth;
}
struct JsonArena {
    char *data;size_t used=0;bool exhausted=false;
    void *take(size_t n){
        size_t aligned=(used+alignof(max_align_t)-1)&~(alignof(max_align_t)-1);
        if(n>JSON_HEAP-aligned){exhausted=true;return nullptr;}
        void *p=data+aligned;used=aligned+n;return p;
    }
};
thread_local JsonArena *active_json_arena=nullptr;
void *json_take(size_t n){return active_json_arena?active_json_arena->take(n):nullptr;}
void json_give(void *){} // One fixed arena, released by Json after tree use.
struct Json {
    Buffer storage{JSON_HEAP};
    JsonArena arena{storage.p};
    cJSON *root=nullptr;
    Code error=Code::Response;
    Json(const char *p,size_t n){
        if(!storage.p){error=Code::Memory;return;}
        if(!utf8(p,n)||!json_budget(p,n))return;
        // This is a private instance of the unmodified SDK parser, never the
        // shared SDK allocator hooks used by HTTPD/configuration consumers.
        static const bool initialized=[](){cJSON_Hooks hooks{json_take,json_give};cJSON_InitHooks(&hooks);return true;}();
        (void)initialized;
        JsonArena *previous=active_json_arena;active_json_arena=&arena;
        const char *end=nullptr;root=cJSON_ParseWithLengthOpts(p,n+1,&end,false);
        active_json_arena=previous;
        if(arena.exhausted)error=Code::TooLarge;
        if(root){while(end<p+n && (*end==' '||*end=='\t'||*end=='\r'||*end=='\n'))end++;if(end!=p+n){cJSON_Delete(root);root=nullptr;}}
    }
    ~Json(){cJSON_Delete(root);}
};
cJSON *field(cJSON *o,const char *name){
    if(!cJSON_IsObject(o))return nullptr;
    cJSON *found=nullptr;
    for(auto *p=o->child;p;p=p->next)if(p->string && !strcmp(p->string,name)){if(found)return nullptr;found=p;}
    return found;
}
const char *str(cJSON *o,const char *name){auto *p=field(o,name);return cJSON_IsString(p)?p->valuestring:nullptr;}
void provider_code(Result &r,cJSON *root,const char *secret){
    const char *code=str(root,"code");if(!code)code=str(field(root,"error"),"code");
    if(!code||!code[0]||strnlen(code,sizeof(r.provider_code))==sizeof(r.provider_code)||(secret&&strstr(code,secret)))return;
    for(const char *p=code;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return;
    memcpy(r.provider_code,code,strlen(code)+1);
}
Code control(const IO &io,int64_t deadline){
    if(io.cancelled && io.cancelled(io.context))return Code::Cancelled;
    return io.now(io.context)>=deadline?Code::Timeout:Code::Ok;
}
int timeout(const IO &io,int64_t deadline){return static_cast<int>(std::min<int64_t>(10000,std::max<int64_t>(0,deadline-io.now(io.context))));}
Code failure(const IO &io){return io.failure?io.failure(io.context):Code::Transport;}
struct Lease {
    const IO &io;bool ok=false,opened=false;
    explicit Lease(const IO &v):io(v){if(io.acquire)ok=io.acquire(io.context);}
    ~Lease(){if(opened)io.http_close(io.context);if(ok)io.release(io.context);}
};
struct Writer {
    const IO &io;int64_t deadline;Code code=Code::Ok;
    bool put(const void *data,size_t n){
        auto *p=static_cast<const char *>(data);
        while(n){code=control(io,deadline);if(code!=Code::Ok)return false;
            int sent=io.http_write(io.context,p,n,timeout(io,deadline));
            if(sent<=0||static_cast<size_t>(sent)>n){code=failure(io);return false;}
            p+=sent;n-=sent;
        }return true;
    }
};
size_t base64(const uint8_t *p,size_t n,char *out){
    static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at=0;
    for(size_t i=0;i<n;i+=3){uint32_t v=static_cast<uint32_t>(p[i])<<16;if(i+1<n)v|=static_cast<uint32_t>(p[i+1])<<8;if(i+2<n)v|=p[i+2];
        out[at++]=table[v>>18];out[at++]=table[(v>>12)&63];out[at++]=i+1<n?table[(v>>6)&63]:'=';out[at++]=i+2<n?table[v&63]:'=';
    }return at;
}
bool read_exact(Source s,uint64_t at,char *out,size_t n){
    while(n){int got=s.read(s.context,at,out,n);if(got<=0||static_cast<size_t>(got)>n)return false;at+=got;out+=got;n-=got;}return true;
}
size_t escaped(unsigned char c,char *out){
    static const char hex[]="0123456789abcdef";
    if(c=='"'||c=='\\'){out[0]='\\';out[1]=c;return 2;}
    if(c<32){memcpy(out,"\\u00",4);out[4]=hex[c>>4];out[5]=hex[c&15];return 6;}
    out[0]=c;return 1;
}
Code quoted(Source s,Writer *writer,size_t &length){
    char in[3072],out[4096];size_t used=0;length=0;
    // Validate UTF-8 incrementally, including when a scalar spans read blocks.
    uint32_t scalar=0;unsigned remaining=0,total=0;
    for(uint64_t at=0;at<s.size;){
        size_t n=std::min<uint64_t>(sizeof(in),s.size-at);
        if(!read_exact(s,at,in,n))return Code::Source;
        for(size_t i=0;i<n;i++){
            unsigned char c=in[i];
            if(remaining){if((c&0xc0)!=0x80)return Code::Invalid;scalar=(scalar<<6)|(c&63);if(!--remaining && ((total==1&&scalar<128)||(total==2&&scalar<2048)||(total==3&&scalar<65536)||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff)))return Code::Invalid;}
            else if(c>=128){if(c>=0xc2&&c<=0xdf){scalar=c&31;remaining=total=1;}else if(c>=0xe0&&c<=0xef){scalar=c&15;remaining=total=2;}else if(c>=0xf0&&c<=0xf4){scalar=c&7;remaining=total=3;}else return Code::Invalid;}
            char unit[6];size_t k=escaped(c,unit);length+=k;
            if(writer){if(used+k>sizeof(out)){if(!writer->put(out,used))return writer->code;used=0;}memcpy(out+used,unit,k);used+=k;}
        }at+=n;
    }
    if(remaining)return Code::Invalid;
    return writer && used && !writer->put(out,used)?writer->code:Code::Ok;
}
Result response(const IO &io,int64_t deadline,bool delivery,Sink sink,const char *secret){
    Result r;Code c=control(io,deadline);
    if(c!=Code::Ok){r.code=c;return r;}
    r.status=io.http_headers(io.context,&r.retry_after,timeout(io,deadline));
    r.may_have_sent=io.http_sent(io.context);
    if(r.status<=0){r.code=failure(io);return r;}
    bool success=r.status>=200&&r.status<300;
    Code rejection=r.status==401||r.status==403?Code::Credentials:r.status==429?Code::Limited:delivery&&r.status>=500?Code::Uncertain:Code::Rejected;
    size_t limit=success?RESPONSE:8192;
    Buffer body(limit+1);if(!body.p){r.code=success?Code::Memory:rejection;return r;}size_t used=0;
    while(true){c=control(io,deadline);if(c!=Code::Ok){r.code=c;return r;}
        char chunk[4096];int n=io.http_read(io.context,chunk,sizeof(chunk),timeout(io,deadline));
        if(n<0){r.code=success?failure(io):rejection;return r;}if(!n)break;
        if(static_cast<size_t>(n)>sizeof(chunk)||used+static_cast<size_t>(n)>limit){r.code=success?Code::TooLarge:rejection;return r;}
        memcpy(body.p+used,chunk,n);used+=n;
        if(io.http_complete(io.context))break;
    }
    // A fully received authoritative delivery result wins a racing cancel.
    // complete means wire-complete AND all received bytes returned to us.
    if(!io.http_complete(io.context)){r.code=success?Code::Response:rejection;return r;}
    if(!delivery&&control(io,deadline)!=Code::Ok){r.code=control(io,deadline);return r;}
    body.p[used]=0;Json json(body.p,used);
    if(!json.root){r.code=success?json.error:rejection;return r;}
    if(!success){r.code=rejection;provider_code(r,json.root,secret);return r;}
    if(delivery){auto *code=field(json.root,"code");r.code=cJSON_IsNumber(code)&&std::isfinite(code->valuedouble)?(code->valuedouble==0?Code::Ok:Code::Rejected):Code::Response;return r;}
    auto *choices=field(json.root,"choices");auto *choice=cJSON_IsArray(choices)?cJSON_GetArrayItem(choices,0):nullptr;
    const char *finish=str(choice,"finish_reason"),*text=str(field(choice,"message"),"content");
    if(!finish||strcmp(finish,"stop")||!text){r.code=Code::Response;return r;}
    size_t n=strlen(text);if(n>SEGMENT){r.code=Code::TooLarge;return r;}
    if(!utf8(text,n)){r.code=Code::Response;return r;}
    if(blank(text)){r.code=Code::Empty;return r;}
    if(!sink.append(sink.context,text,n))r.code=Code::Source;
    return r;
}
bool http_valid(const IO &i){return i.now&&i.acquire&&i.release&&i.http_open&&i.http_write&&i.http_headers&&i.http_read&&i.http_complete&&i.http_sent&&i.http_close;}
Result attempted(const IO &io,Result r,bool delivery){
    r.may_have_sent=io.http_sent(io.context);
    if(delivery && r.may_have_sent && r.code!=Code::Ok && r.code!=Code::Rejected && r.code!=Code::Credentials && r.code!=Code::Limited)r.code=Code::Uncertain;
    return r;
}
}

bool valid_workspace(const char *p){
    if(!p)return false;
    size_t n=strnlen(p,64);if(!n||n>63)return false;
    auto alnum=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};
    if(!alnum(p[0])||!alnum(p[n-1]))return false;
    for(size_t i=0;i<n;i++)if(!alnum(p[i])&&p[i]!='-')return false;
    return true;
}
uint32_t retry_after(const char *p,int64_t now){
    if(!p||!p[0]||strnlen(p,128)==128)return 0;
    uint64_t seconds=0;bool digits=true;
    for(const char *c=p;*c;c++){if(*c<'0'||*c>'9'){digits=false;break;}seconds=seconds*10+(*c-'0');if(seconds>UINT32_MAX)return UINT32_MAX;}
    if(digits)return static_cast<uint32_t>(seconds);
    tm date{};char *end=strptime(p,"%a, %d %b %Y %H:%M:%S GMT",&date);
    if(!end||*end)return 0;
    // Convert the parsed UTC date without modifying the process timezone.
    int year=date.tm_year+1900;
    bool leap=year%4==0 && (year%100!=0 || year%400==0);
    const int months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(year<1970||year>9999||date.tm_mon<0||date.tm_mon>11||date.tm_mday<1||date.tm_mday>months[date.tm_mon]+(date.tm_mon==1&&leap)||date.tm_sec>59)return 0;
    int64_t days=365LL*(year-1970)+(year-1)/4-(year-1)/100+(year-1)/400-477;
    for(int m=0;m<date.tm_mon;m++)days+=months[m]+(m==1&&leap);
    days+=date.tm_mday-1;
    int64_t difference=days*86400+date.tm_hour*3600+date.tm_min*60+date.tm_sec-now;
    return difference>0?static_cast<uint32_t>(std::min<int64_t>(difference,UINT32_MAX)):0;
}
Result file_asr(const IO &io,const char *workspace,const char *key,Source wav,Sink text){
    if(!http_valid(io)||!valid_workspace(workspace)||!credentials(key)||!wav.read||!text.append||wav.size<46||wav.size>5760044)return {Code::Invalid};
    Lease lease(io);if(!lease.ok)return {Code::Busy};
    int64_t deadline=io.now(io.context)+240000;
    const char *prefix="{\"model\":\"qwen3-asr-flash-2026-02-10\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    const char *suffix="\"}}]}]}";
    size_t length=strlen(prefix)+4*((wav.size+2)/3)+strlen(suffix);
    char url[192];Secret auth;snprintf(url,sizeof(url),"https://%s.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions",workspace);snprintf(auth.value,sizeof(auth.value),"Bearer %s",key);
    Code c=control(io,deadline);if(c!=Code::Ok)return {c};lease.opened=true;
    if(io.http_open(io.context,url,auth.value,length,deadline)<0)return attempted(io,{failure(io)},false);
    Writer w{io,deadline};if(!w.put(prefix,strlen(prefix)))return attempted(io,{w.code},false);
    uint8_t in[3072];char out[4096];
    for(uint64_t at=0;at<wav.size;){size_t n=std::min<uint64_t>(sizeof(in),wav.size-at);
        if(!read_exact(wav,at,reinterpret_cast<char *>(in),n))return attempted(io,{Code::Source},false);
        if(!w.put(out,base64(in,n,out)))return attempted(io,{w.code},false);
        at+=n;
    }
    if(!w.put(suffix,strlen(suffix)))return attempted(io,{w.code},false);
    return attempted(io,response(io,deadline,false,text,key),false);
}
Result deliver(const IO &io,const char *token,const char *title,Source text){
    if(!http_valid(io)||!credentials(token)||!title||strlen(title)>128||!utf8(title,strlen(title))||!text.read||!text.size||text.size>TEXT)return {Code::Invalid};
    Lease lease(io);if(!lease.ok)return {Code::Busy};
    int64_t deadline=io.now(io.context)+30000;
    Source heading{const_cast<char *>(title),strlen(title),[](void *p,uint64_t at,void *to,size_t n){memcpy(to,static_cast<char *>(p)+at,n);return static_cast<int>(n);}};
    size_t title_len=0,text_len=0;Code c=quoted(heading,nullptr,title_len);if(c==Code::Ok)c=quoted(text,nullptr,text_len);if(c!=Code::Ok)return {c};
    c=control(io,deadline);if(c!=Code::Ok)return {c};
    const char *a="{\"title\":\"",*b="\",\"content\":\"",*z="\"}";
    size_t length=strlen(a)+title_len+strlen(b)+text_len+strlen(z);
    Secret auth;snprintf(auth.value,sizeof(auth.value),"token %s",token);lease.opened=true;
    if(io.http_open(io.context,"https://ld246.com/apis/siyuan/inbox/addCloudShorthand",auth.value,length,deadline)<0)return attempted(io,{failure(io)},true);
    Writer w{io,deadline};
    if(!w.put(a,strlen(a)))return attempted(io,{w.code},true);
    c=quoted(heading,&w,title_len);if(c!=Code::Ok)return attempted(io,{c},true);
    if(!w.put(b,strlen(b)))return attempted(io,{w.code},true);
    c=quoted(text,&w,text_len);if(c!=Code::Ok)return attempted(io,{c},true);
    if(!w.put(z,strlen(z)))return attempted(io,{w.code},true);
    return attempted(io,response(io,deadline,true,{},token),true);
}

struct Realtime::State {
    Buffer message{RESPONSE+1},pending{SEGMENT};
    struct Item {char id[97]{},previous[97]{};bool final=false;size_t offset=0,length=0;} items[16];
    size_t count=0,used=0,total=0;
    char last[97]{};
    char completed[16][97]{};size_t completed_at=0;
    Sink sink{};
    bool created=false,updated=false,finished=false,finishing=false;
    int64_t deadline=0;
    uint32_t event_id=0;
};
Realtime::Realtime(const IO &io):io_(io){}
Realtime::~Realtime(){cancel();}
Result Realtime::stop(Code code){
    result_.code=code;
    if(opened_){io_.ws_close(io_.context);opened_=false;}
    if(state_){state_->~State();release_memory(state_);state_=nullptr;}
    if(leased_){io_.release(io_.context);leased_=false;}
    return result_;
}
void Realtime::cancel(){if(state_||leased_||opened_)stop(Code::Cancelled);}
bool Realtime::send(const char *p,size_t n,int64_t deadline){
    Code c=control(io_,deadline);if(c!=Code::Ok){result_.code=c;return false;}
    int sent=io_.ws_send(io_.context,p,n,timeout(io_,deadline));
    c=control(io_,deadline);if(c!=Code::Ok){result_.code=c;return false;}
    if(sent!=static_cast<int>(n)){result_.code=failure(io_);return false;}
    return true;
}
Result Realtime::begin(const char *workspace,const char *key,Sink sink){
    if(state_||leased_)return {Code::Busy};
    if(!valid_workspace(workspace)||!credentials(key)||!sink.append||!io_.now||!io_.acquire||!io_.release||!io_.ws_open||!io_.ws_send||!io_.ws_read||!io_.ws_close)return {Code::Invalid};
    if(!io_.acquire(io_.context))return {Code::Busy};
    leased_=true;result_={};
    void *storage=allocate(sizeof(State));if(!storage)return stop(Code::Memory);
    state_=new(storage) State;
    if(!state_->message.p||!state_->pending.p)return stop(Code::Memory);
    state_->sink=sink;state_->deadline=io_.now(io_.context)+15000;
    char url[192];Secret auth;snprintf(url,sizeof(url),"wss://%s.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime-2026-02-10",workspace);
    int auth_length=snprintf(auth.value,sizeof(auth.value),"Authorization: Bearer %s\r\n",key);
    if(auth_length<0||static_cast<size_t>(auth_length)>=sizeof(auth.value))return stop(Code::Invalid);
    Code admission=control(io_,state_->deadline);if(admission!=Code::Ok)return stop(admission);
    opened_=true;if(io_.ws_open(io_.context,url,auth.value,state_->deadline)<0)return stop(failure(io_));
    while(!state_->updated){Result r=event(100);if(r.code!=Code::Ok)return r;}
    state_->deadline=INT64_MAX;return result_;
}
Result Realtime::event(int wait){
    if(!state_)return result_;
    Code c=control(io_,state_->deadline);if(c!=Code::Ok)return stop(c);
    int n=io_.ws_read(io_.context,state_->message.p,RESPONSE,std::min(wait,timeout(io_,state_->deadline)));
    c=control(io_,state_->deadline);if(c!=Code::Ok)return stop(c);
    if(n<0)return stop(failure(io_));
    if(!n)return result_;
    if(static_cast<size_t>(n)>RESPONSE)return stop(Code::TooLarge);
    state_->message.p[n]=0;Json json(state_->message.p,n);
    if(!json.root)return stop(json.error);
    const char *type=str(json.root,"type");if(!type)return stop(Code::Response);
    if(!strcmp(type,"error")||!strcmp(type,"conversation.item.input_audio_transcription.failed")){
        const char *code=str(field(json.root,"error"),"code");
        return stop(code&&(!strcmp(code,"invalid_api_key")||!strcmp(code,"invalid_api_key_error"))?Code::Credentials:code&&(!strcmp(code,"rate_limit_exceeded")||!strcmp(code,"rate_limit_error"))?Code::Limited:Code::Rejected);
    }
    if(!strcmp(type,"session.created")){
        if(state_->created)return stop(Code::Response);
        state_->created=true;
        const char *update="{\"event_id\":\"config\",\"type\":\"session.update\",\"session\":{\"input_audio_format\":\"pcm\",\"sample_rate\":16000,\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.2,\"silence_duration_ms\":800}}}";
        if(!send(update,strlen(update),state_->deadline))return stop(result_.code);
    }else if(!strcmp(type,"session.updated")){
        if(!state_->created||state_->updated)return stop(Code::Response);
        state_->updated=true;
    }else if(!strcmp(type,"input_audio_buffer.committed")){
        const char *id=str(json.root,"item_id"),*prev=str(json.root,"previous_item_id");
        auto *previous=field(json.root,"previous_item_id");
        if(!state_->updated||!id||!id[0]||strlen(id)>96||(!prev&&!cJSON_IsNull(previous))||(prev&&strlen(prev)>96)||state_->count==16)return stop(Code::Response);
        for(size_t i=0;i<state_->count;i++)if(!strcmp(id,state_->items[i].id))return stop(Code::Response);
        if(!strcmp(id,state_->last))return stop(Code::Response);
        auto &item=state_->items[state_->count++];item={};snprintf(item.id,sizeof(item.id),"%s",id);snprintf(item.previous,sizeof(item.previous),"%s",prev?prev:"");
    }else if(!strcmp(type,"conversation.item.input_audio_transcription.completed")){
        const char *id=str(json.root,"item_id"),*text=str(json.root,"transcript");
        if(!id||!text||!utf8(text,strlen(text)))return stop(Code::Response);
        State::Item *item=nullptr;for(size_t i=0;i<state_->count;i++)if(!strcmp(id,state_->items[i].id))item=&state_->items[i];
        if(!item){for(auto &done:state_->completed)if(!strcmp(id,done))return result_;return stop(Code::Response);}
        if(item->final)return result_;
        size_t length=strlen(text);if(length>SEGMENT||state_->used+length>SEGMENT||state_->total+length>TEXT)return stop(Code::TooLarge);
        item->offset=state_->used;item->length=length;item->final=true;memcpy(state_->pending.p+state_->used,text,length);state_->used+=length;state_->total+=length;
        while(state_->count && state_->items[0].final){
            auto &first=state_->items[0];if(strcmp(first.previous,state_->last))return stop(Code::Response);
            if(first.length && !state_->sink.append(state_->sink.context,state_->pending.p+first.offset,first.length))return stop(Code::Source);
            snprintf(state_->last,sizeof(state_->last),"%s",first.id);
            snprintf(state_->completed[state_->completed_at++%16],sizeof(state_->completed[0]),"%s",first.id);
            size_t offset=first.offset,removed=first.length;
            memmove(state_->pending.p+offset,state_->pending.p+offset+removed,state_->used-offset-removed);state_->used-=removed;
            for(size_t i=1;i<state_->count;i++){if(state_->items[i].final&&state_->items[i].offset>=offset+removed)state_->items[i].offset-=removed;state_->items[i-1]=state_->items[i];}
            state_->count--;
        }
    }else if(!strcmp(type,"session.finished")){
        if(!state_->finishing||state_->count)return stop(Code::Response);
        state_->finished=true;
    }
    return result_;
}
Result Realtime::poll(){return state_?event(0):Result{Code::Invalid};}
Result Realtime::audio(const void *pcm,size_t bytes,uint32_t remaining_ms){
    if(!state_)return {Code::Invalid};
    if(!state_->updated||state_->finishing||!pcm||!bytes||bytes>3200||bytes%2)return stop(Code::Invalid);
    char message[4608];int prefix=snprintf(message,sizeof(message),"{\"event_id\":\"audio_%u\",\"type\":\"input_audio_buffer.append\",\"audio\":\"",static_cast<unsigned>(++state_->event_id));
    size_t n=prefix+base64(static_cast<const uint8_t *>(pcm),bytes,message+prefix);memcpy(message+n,"\"}",2);n+=2;
    if(!remaining_ms)return stop(Code::Timeout);
    if(!send(message,n,io_.now(io_.context)+std::min<uint32_t>(remaining_ms,2000)))return stop(result_.code);
    return event(0);
}
Result Realtime::finish(uint32_t remaining_ms){
    if(!state_||!state_->updated||state_->finishing)return {Code::Invalid};
    if(!remaining_ms)return stop(Code::Timeout);
    state_->finishing=true;state_->deadline=io_.now(io_.context)+std::min<uint32_t>(remaining_ms,30000);
    const char *finish="{\"event_id\":\"finish\",\"type\":\"session.finish\"}";
    if(!send(finish,strlen(finish),state_->deadline))return stop(result_.code);
    while(!state_->finished){Result r=event(100);if(r.code!=Code::Ok)return r;}
    return stop(state_->total?Code::Ok:Code::Empty);
}
} // namespace memo_cloud
