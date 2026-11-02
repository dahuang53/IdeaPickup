#pragma once
#include <stddef.h>
#include <stdint.h>
#include "memo_cloud_config.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Non-echoing bounded line state for the dedicated USB command. */
typedef struct {char bytes[1025];size_t used;int64_t last_ms;bool overflow;} memo_cloud_input_t;
void memo_cloud_input_reset(memo_cloud_input_t *,int64_t);
/* 0 waiting, 1 complete, -1 idle timeout, -2 overflow. Never returns a prefix. */
int memo_cloud_input_push(memo_cloud_input_t *,int ch,int64_t now_ms);
memo_cloud_result_t memo_cloud_input_apply(const char *,size_t);
#ifdef __cplusplus
}
#endif
