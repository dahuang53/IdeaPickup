#include "memo_cloud.h"
#include "cJSON.h"
#include <cstdlib>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
using namespace memo_cloud;
namespace {
struct alignas(max_align_t) Allocation {size_t bytes;};
size_t live=0,peak=0,objects=0;
bool fail_arena=false;
bool fail_scratch=false;
void *take(size_t n){assert(n>0);if((fail_arena&&n<7168)||(fail_scratch&&n==7168))return nullptr;auto *p=static_cast<Allocation *>(malloc(sizeof(Allocation)+n));if(!p)return nullptr;p->bytes=n;live+=n;if(live>peak)peak=live;return p+1;}
void give(void *p){if(!p)return;auto *a=static_cast<Allocation *>(p)-1;live-=a->bytes;free(a);}
}
struct PlainResult {int code,status;uint32_t retry;bool sent;char provider[48];};
static PlainResult plain(Result r){PlainResult out{};out.code=static_cast<int>(r.code);out.status=r.status;out.retry=r.retry_after;out.sent=r.may_have_sent;memcpy(out.provider,r.provider_code,sizeof(out.provider));return out;}
extern "C" {
void *memo_cloud_test_allocate(size_t n){return take(n);}
void memo_cloud_test_free(void *p){give(p);}
void cloud_setup(){cJSON_Hooks hooks{take,give};cJSON_InitHooks(&hooks);}
size_t cloud_live(){return live+objects;}
size_t cloud_peak(){return peak;}
PlainResult cloud_file(const IO *io,const char *key,const Source *source,const Sink *sink){return plain(file_asr(*io,key,*source,*sink));}
PlainResult cloud_deliver(const IO *io,const char *key,const char *title,const Source *source){return plain(deliver(*io,key,title,*source));}
void *cloud_rt_new(const IO *io){objects++;return new Realtime(*io);}
void cloud_rt_delete(void *p){delete static_cast<Realtime *>(p);objects--;}
PlainResult cloud_rt_begin(void *p,const char *key,const Sink *sink){return plain(static_cast<Realtime *>(p)->begin(key,*sink));}
PlainResult cloud_rt_audio(void *p,const void *pcm,size_t n){return plain(static_cast<Realtime *>(p)->audio(pcm,n));}
PlainResult cloud_rt_poll(void *p){return plain(static_cast<Realtime *>(p)->poll());}
PlainResult cloud_rt_finish(void *p){return plain(static_cast<Realtime *>(p)->finish());}
void cloud_rt_cancel(void *p){static_cast<Realtime *>(p)->cancel();}
}
#ifdef CLOUD_UNIT
struct Unit {
    std::string response="{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"native\"}}]}",output;
    std::deque<std::string> events;
    size_t cursor=0;bool cancelled=false,sent=false,leased=false;int64_t now=1;
    IO io{};
    Unit(){
        io.context=this;io.now=[](void*p){return static_cast<Unit*>(p)->now;};
        io.cancelled=[](void*p){return static_cast<Unit*>(p)->cancelled;};
        io.acquire=[](void*p){auto&s=*static_cast<Unit*>(p);if(s.leased)return false;s.leased=true;return true;};
        io.release=[](void*p){static_cast<Unit*>(p)->leased=false;};
        io.http_open=[](void*p,const char*,const char*,size_t,int64_t){auto&s=*static_cast<Unit*>(p);s.cursor=0;s.sent=true;return 0;};
        io.http_write=[](void*,const void*,size_t n,int){return static_cast<int>(n);};
        io.http_headers=[](void*,uint32_t*r,int){*r=0;return 200;};
        io.http_read=[](void*p,void*out,size_t cap,int){auto&s=*static_cast<Unit*>(p);size_t n=std::min<size_t>(std::min<size_t>(7,cap),s.response.size()-s.cursor);memcpy(out,s.response.data()+s.cursor,n);s.cursor+=n;return static_cast<int>(n);};
        io.http_complete=[](void*p){auto&s=*static_cast<Unit*>(p);return s.cursor==s.response.size();};io.http_sent=[](void*p){return static_cast<Unit*>(p)->sent;};io.http_close=[](void*){};
        io.ws_open=[](void*p,const char*,const char*,int64_t){static_cast<Unit*>(p)->events={"{\"type\":\"session.created\"}"};return 0;};
        io.ws_send=[](void*p,const char*text,size_t n,int){auto&s=*static_cast<Unit*>(p);std::string message(text,n);
            if(message.find("session.update")!=std::string::npos)s.events.push_back("{\"type\":\"session.updated\"}");
            if(message.find("session.finish")!=std::string::npos)s.events={"{\"type\":\"input_audio_buffer.committed\",\"item_id\":\"1\",\"previous_item_id\":null}","{\"type\":\"conversation.item.input_audio_transcription.completed\",\"item_id\":\"1\",\"transcript\":\"native\"}","{\"type\":\"session.finished\"}"};
            return static_cast<int>(n);};
        io.ws_read=[](void*p,char*out,size_t cap,int){auto&s=*static_cast<Unit*>(p);if(s.events.empty())return 0;auto message=s.events.front();s.events.pop_front();assert(message.size()<=cap);memcpy(out,message.data(),message.size());return static_cast<int>(message.size());};
        io.ws_close=[](void*p){static_cast<Unit*>(p)->events.clear();};io.failure=[](void*){return Code::Transport;};
    }
    Source source(const char*p){return {const_cast<char*>(p),strlen(p),[](void*v,uint64_t at,void*out,size_t n){memcpy(out,static_cast<char*>(v)+at,n);return static_cast<int>(n);}};}
    Sink sink(){return {this,[](void*p,const char*text,size_t n){static_cast<Unit*>(p)->output.append(text,n);return true;}};}
};
int main(){
    cloud_setup();
    assert(retry_after("10",0)==10);assert(retry_after("301",0)==301);assert(retry_after("bad",0)==0);
    assert(retry_after("Thu, 01 Jan 1970 00:00:10 GMT",0)==10);
    assert(retry_after("Wed, 21 Oct 2015 07:28:00 GMT",1445412470)==10);
    assert(retry_after("Wed, 31 Feb 2024 07:28:00 GMT",0)==0);
    assert(retry_after("42949672960",0)==UINT32_MAX);
    IO io{};Source source{};Sink sink{};
    assert(file_asr(io,"key",source,sink).code==Code::Invalid);
    assert(deliver(io,"key","title",source).code==Code::Invalid);
    {
        Unit u;auto wav=u.source("RIFF....................................................");
        auto *shared=cJSON_Parse("{\"kept\":true}");assert(shared);
        const size_t shared_live=cloud_live();
        fail_scratch=true;
        assert(file_asr(u.io,"key",wav,u.sink()).code==Code::Memory);
        assert(deliver(u.io,"key","title",wav).code==Code::Memory);
        assert(!u.leased&&!u.sent&&cloud_live()==shared_live);
        fail_scratch=false;
        fail_arena=true;assert(file_asr(u.io,"key",wav,u.sink()).code==Code::Memory);fail_arena=false;
        u.response="{\"padding\":[";
        for(int n=0;n<2770;n++)u.response+=n?",\"\"":"\"\"";
        u.response+="]}";
        assert(file_asr(u.io,"key",wav,u.sink()).code==Code::TooLarge);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(shared,"kept")));cJSON_Delete(shared);
        assert(!u.leased&&cloud_live()==0);
    }
    for(int i=0;i<100;i++){Realtime rt(io);assert(rt.begin("key",sink).code==Code::Invalid);rt.cancel();}
    for(int i=0;i<20;i++){
        Unit u;auto wav=u.source("RIFF....................................................");
        assert(file_asr(u.io,"key",wav,u.sink()).code==Code::Ok);assert(!u.leased);
        u.response="{broken";assert(file_asr(u.io,"key",wav,u.sink()).code==Code::Response);assert(!u.leased);
        u.response="{\"code\":0}";assert(deliver(u.io,"token","title",u.source("text")).code==Code::Ok);
        {Realtime rt(u.io);assert(rt.begin("key",u.sink()).code==Code::Ok);assert(rt.audio("\0\0",2).code==Code::Ok);assert(rt.finish().code==Code::Ok);}
        {Realtime rt(u.io);assert(rt.begin("key",u.sink()).code==Code::Ok);u.cancelled=true;assert(rt.poll().code==Code::Cancelled);}
        assert(!u.leased&&cloud_live()==0);
    }
    for(int fault=0;fault<2;fault++){
        Unit u;Realtime rt(u.io);assert(rt.begin("key",u.sink()).code==Code::Ok);
        if(fault==0)u.io.ws_send=[](void*p,const char*,size_t,int){static_cast<Unit*>(p)->now+=2001;return -1;};
        else u.io.ws_send=[](void*p,const char*,size_t,int){static_cast<Unit*>(p)->cancelled=true;return -1;};
        assert(rt.audio("\0\0",2).code==(fault==0?Code::Timeout:Code::Cancelled));
        assert(!u.leased&&cloud_live()==0);
    }
    assert(cloud_live()==0);
}
#endif
