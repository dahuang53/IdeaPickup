#pragma once
#include "memo_network_config.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    MEMO_PAIR_OK, MEMO_PAIR_NETWORK, MEMO_PAIR_REJECTED, MEMO_PAIR_INVALID_RESPONSE,
    MEMO_PAIR_STORAGE, MEMO_PAIR_BUSY, MEMO_PAIR_INSTANCE_CHANGED
} memo_pair_result_t;
typedef struct {
    void *context;
    int (*request)(void *, const char *base, const char *method, const char *path,
                   const char *token, const char *payload, char *response, size_t capacity);
    /* Durably bind the observed instance BEFORE activation. On success replace
     * pending with the actual committed snapshot; short audio exclusion only. */
    memo_config_result_t (*bind)(void *, memo_config_snapshot_t *pending, const memo_server_uuid_t *);
} memo_pairing_io_t;
typedef struct { memo_server_uuid_t instance; char error_code[48]; } memo_pairing_receipt_t;
memo_pair_result_t memo_pairing_run(memo_config_snapshot_t pending, const memo_pairing_io_t *, memo_pairing_receipt_t *);
#ifdef __cplusplus
}
#endif
