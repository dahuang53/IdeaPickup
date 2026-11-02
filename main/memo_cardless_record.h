#pragma once
#include "memo_cardless.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MEMO_CARDLESS_RECORD_SIZE 228U
/* First public format only; byte layout is explicit, no legacy native struct import. */
bool memo_cardless_record_decode(const void *,size_t,memo_cardless_record_t *);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define MEMO_CARDLESS_DIRECT_SIZE 128U
bool memo_cardless_direct_encode(const memo_cardless_record_t *,uint8_t out[MEMO_CARDLESS_DIRECT_SIZE]);
bool memo_cardless_direct_decode(const void *,size_t,memo_cardless_record_t *);
#ifdef __cplusplus
}
#endif
