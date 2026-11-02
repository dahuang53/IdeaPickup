#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {MEMO_ACTIVITY_IDLE,MEMO_ACTIVITY_AUDIO,MEMO_ACTIVITY_COMMIT,MEMO_ACTIVITY_MEDIA} memo_config_activity_t;
typedef struct {uint32_t owner;} memo_config_activity_gate_t;
bool memo_config_activity_acquire(memo_config_activity_gate_t *,memo_config_activity_t);
bool memo_config_activity_release(memo_config_activity_gate_t *,memo_config_activity_t);
memo_config_activity_t memo_config_activity(const memo_config_activity_gate_t *);
#ifdef __cplusplus
}
#endif
