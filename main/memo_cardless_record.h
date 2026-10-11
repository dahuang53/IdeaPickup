#pragma once
#include "memo_cardless.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MEMO_CARDLESS_RECORD_SIZE 228U
/* First public format only; byte layout is explicit, no legacy native struct import. */
bool memo_cardless_record_decode(const void *,size_t,memo_cardless_record_t *);
bool memo_cardless_record_encode(const memo_cardless_record_t *,uint8_t out[MEMO_CARDLESS_RECORD_SIZE]);
#ifdef __cplusplus
}
#endif
