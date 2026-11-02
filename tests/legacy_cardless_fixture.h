#pragma once
#include "memo_cardless_record.h"
#ifdef __cplusplus
extern "C" {
#endif
bool memo_cardless_record_encode(const memo_cardless_record_t *,uint8_t [MEMO_CARDLESS_RECORD_SIZE]);
#ifdef __cplusplus
}
#endif
