#pragma once
#include "esp_http_client.h"
#include "esp_transport.h"

// Per-request decorator of IDF's TCP/TLS transport. The same owning task does
// all TLS I/O and cleanup. A deadline can shut down its owned socket; it never
// closes or frees a TLS handle from another task.
class MemoHttpTransport {
public:
 explicit MemoHttpTransport(esp_http_client_config_t &config);
 ~MemoHttpTransport();
 MemoHttpTransport(const MemoHttpTransport &)=delete;
 MemoHttpTransport &operator=(const MemoHttpTransport &)=delete;
 bool ready() const {return outer_ && inner_ && slot_>=0;}
 void write_window();
 static uint32_t stack_free();
private:
 esp_transport_handle_t outer_=nullptr,inner_=nullptr;
 esp_transport_keep_alive_t keepalive_{};
 int64_t read_deadline_=0,write_deadline_=0;
 int slot_=-1;
 static MemoHttpTransport &self(esp_transport_handle_t);
 static int connect(esp_transport_handle_t,const char *,int,int);
 static int read(esp_transport_handle_t,char *,int,int);
 static int write(esp_transport_handle_t,const char *,int,int);
 static int close(esp_transport_handle_t);
 static int poll_read(esp_transport_handle_t,int);
 static int poll_write(esp_transport_handle_t,int);
 static int budget(int64_t &,int,int);
};
