#pragma once
#include "esp_http_client.h"
#include "esp_transport.h"
#include <atomic>

// Per-request decorator of IDF's TCP/TLS transport. HTTP is serial; official WS
// may receive and send concurrently. Cleanup runs only after its worker stops.
// A deadline can shut down the owned socket; it never closes/frees TLS cross-task.
class MemoHttpTransport {
public:
 explicit MemoHttpTransport(esp_http_client_config_t &config,int64_t hard_deadline_us=0,bool streaming=false);
 ~MemoHttpTransport();
 MemoHttpTransport(const MemoHttpTransport &)=delete;
 MemoHttpTransport &operator=(const MemoHttpTransport &)=delete;
 bool ready() const {return outer_ && inner_ && slot_>=0;}
 void write_window();
 void read_window();
 bool may_have_written() const {return written_;}
 int tls_verify_flags() const;
 esp_transport_handle_t handle() const {return outer_;}
 void interrupt(); // Request socket shutdown; never close/free TLS cross-task.
 void send_until(int64_t deadline_us); // Entire SDK send, including TX lock/frames; 0 clears.
 static uint32_t stack_free();
private:
 esp_transport_handle_t outer_=nullptr,inner_=nullptr;
 esp_transport_keep_alive_t keepalive_{};
 int64_t read_deadline_=0,write_deadline_=0;
 int64_t hard_deadline_=0;
 bool written_=false;
 bool streaming_=false;
 std::atomic<bool> interrupted_{false};
 int slot_=-1;
 static MemoHttpTransport &self(esp_transport_handle_t);
 static int connect(esp_transport_handle_t,const char *,int,int);
 static int read(esp_transport_handle_t,char *,int,int);
 static int write(esp_transport_handle_t,const char *,int,int);
 static int close(esp_transport_handle_t);
 static int poll_read(esp_transport_handle_t,int);
 static int poll_write(esp_transport_handle_t,int);
 static int budget(int64_t &,int,int);
 int window(int64_t &,int,int);
};
