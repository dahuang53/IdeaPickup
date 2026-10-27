#pragma once
#include "esp_private/sdmmc_common.h"

// Both supported boards use native SDMMC. Reuse IDF's CMD13 implementation,
// retaining the R1 state that sdmmc_get_status discards. A card in IDLE/STBY
// after removal cannot validate an identity from the former selected session.
// This is session continuity, not a replacement for the full audio SHA.
static inline esp_err_t memo_sd_session_check(sdmmc_card_t *card) {
 if(!card)return ESP_ERR_INVALID_STATE;
 uint32_t status=0;
 esp_err_t err=sdmmc_send_cmd_send_status(card,&status);
 if(err!=ESP_OK)return err;
 return (status&MMC_R1_READY_FOR_DATA) &&
        MMC_R1_CURRENT_STATE_STATUS(status)==MMC_R1_CURRENT_STATE_TRAN?ESP_OK:ESP_ERR_INVALID_STATE;
}
