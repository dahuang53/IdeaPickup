#pragma once

#include <stdbool.h>

typedef enum {
    CAT_ANIM_IDLE,
    CAT_ANIM_RECORDING,
    CAT_ANIM_SAVING,
    CAT_ANIM_SAVED,
    CAT_ANIM_PLAYING,
    CAT_ANIM_ERROR,
    CAT_ANIM_CARDLESS, // Slow-blink motion with a short neutral hold; enum values stay stable.
} cat_anim_state_t;

struct _lv_obj_t;

// Creates the board-sized canvas. The decoder and switching worker only exist on ESP.
bool cat_anim_init(struct _lv_obj_t *parent);
void cat_anim_set(cat_anim_state_t state);

#ifdef CAT_ANIM_TEST
cat_anim_state_t cat_anim_test_state(void);
void cat_anim_test_transform_frame(unsigned short *destination,
                                   const unsigned short *source,
                                   cat_anim_state_t state);
#endif
