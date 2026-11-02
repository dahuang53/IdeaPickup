#pragma once
#include "memo_cloud.h"
#include <atomic>
extern memo_cloud::IO cardless_test_io;
class MemoCloudEsp {
 memo_cloud::IO callbacks;
 public:
 explicit MemoCloudEsp(const std::atomic<bool> &):callbacks(cardless_test_io){}
 const memo_cloud::IO &io()const{return callbacks;}
};
