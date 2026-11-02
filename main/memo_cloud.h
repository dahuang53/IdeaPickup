#pragma once
#include <cstddef>
#include <cstdint>

// Protocol operations, not a persistent recording scheduler. One attempt per
// call: the recording owner must persist its attempt budget before calling.
namespace memo_cloud {
enum class Code : int {
    Ok, Empty, Invalid, Source, Cancelled, Timeout, Transport, Tls,
    Credentials, Limited, Rejected, Response, TooLarge, Memory, Uncertain, Busy
};
struct Result {
    Code code=Code::Ok;
    int status=0;
    uint32_t retry_after=0;
    bool may_have_sent=false;
    char provider_code[48]{}; // Validated short code only; never error prose.
};
struct Source {
    void *context=nullptr;
    uint64_t size=0;
    int (*read)(void *,uint64_t,void *,size_t)=nullptr;
};
struct Sink {
    void *context=nullptr;
    bool (*append)(void *,const char *,size_t)=nullptr;
};
// Concrete ESP adapter owns all handles. Callbacks never initiate another
// request or release a handle from the SDK event thread. Host tests substitute
// only this platform boundary, using real local HTTPS/WebSocket peers.
struct IO {
    void *context=nullptr;
    int64_t (*now)(void *)=nullptr;
    bool (*cancelled)(void *)=nullptr;
    bool (*acquire)(void *)=nullptr;
    void (*release)(void *)=nullptr;
    int (*http_open)(void *,const char *,const char *,size_t,int64_t)=nullptr;
    int (*http_write)(void *,const void *,size_t,int)=nullptr;
    int (*http_headers)(void *,uint32_t *,int)=nullptr;
    int (*http_read)(void *,void *,size_t,int)=nullptr;
    bool (*http_complete)(void *)=nullptr;
    bool (*http_sent)(void *)=nullptr;
    void (*http_close)(void *)=nullptr;
    int (*ws_open)(void *,const char *,const char *,int64_t)=nullptr;
    int (*ws_send)(void *,const char *,size_t,int)=nullptr;
    int (*ws_read)(void *,char *,size_t,int)=nullptr;
    void (*ws_close)(void *)=nullptr;
    Code (*failure)(void *)=nullptr;
};
bool valid_workspace(const char *);
uint32_t retry_after(const char *,int64_t unix_seconds);
Result file_asr(const IO &,const char *workspace,const char *key,Source wav,Sink text);
Result deliver(const IO &,const char *token,const char *title,Source text);

class Realtime {
public:
    explicit Realtime(const IO &);
    ~Realtime();
    Realtime(const Realtime &)=delete;
    Realtime &operator=(const Realtime &)=delete;
    Result begin(const char *workspace,const char *key,Sink final_text);
    Result audio(const void *pcm,size_t bytes); // <=3200, even length, includes tail
    Result poll();
    // Caller stops capture and drains its PCM queue before finish(). No new
    // audio after finish. Sink output is provisional until finish()==Ok;
    // the caller must discard it after any failure and must not deliver it.
    Result finish();
    void cancel();
private:
    struct State;
    const IO &io_;
    State *state_=nullptr;
    Result result_{};
    bool leased_=false,opened_=false;
    Result event(int timeout);
    bool send(const char *,size_t,int64_t deadline);
    Result stop(Code);
};
} // namespace memo_cloud
