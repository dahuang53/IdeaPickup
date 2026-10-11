#pragma once

/* Production gets its board selection from sdkconfig. Host UI tests override
 * MEMO_UI_WIDTH and MEMO_UI_HEIGHT explicitly and do not need ESP-IDF. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef MEMO_UI_WIDTH
#if defined(CONFIG_MEMO_BOARD_SPOTPEAR_1_28) && CONFIG_MEMO_BOARD_SPOTPEAR_1_28
#define MEMO_UI_WIDTH 240
#define MEMO_UI_HEIGHT 240
#else
#define MEMO_UI_WIDTH 360
#define MEMO_UI_HEIGHT 360
#endif
#endif

#ifndef MEMO_UI_HEIGHT
#define MEMO_UI_HEIGHT MEMO_UI_WIDTH
#endif

#define MEMO_UI_COMPACT (MEMO_UI_WIDTH <= 240)

/* The bundled animations decode at 284 x 126.  On the 240 px board we
 * downsample each decoded frame once into a native-size canvas.  Leaving the
 * 284 x 126 PSRAM canvas for LVGL to scale on every refresh can starve its
 * idle core while TLS is also using PSRAM during a long upload. */
#define MEMO_CAT_ASSET_WIDTH 284
#define MEMO_CAT_ASSET_HEIGHT 126
#if MEMO_UI_COMPACT
#define MEMO_CAT_CANVAS_WIDTH 218
#define MEMO_CAT_CANVAS_HEIGHT 97
#define MEMO_CAT_X ((MEMO_UI_WIDTH - MEMO_CAT_CANVAS_WIDTH) / 2)
#define MEMO_CAT_Y ((MEMO_UI_HEIGHT - MEMO_CAT_CANVAS_HEIGHT + 1) / 2)
#else
#define MEMO_CAT_CANVAS_WIDTH MEMO_CAT_ASSET_WIDTH
#define MEMO_CAT_CANVAS_HEIGHT MEMO_CAT_ASSET_HEIGHT
#define MEMO_CAT_X 38
#define MEMO_CAT_Y 99
#endif
#define MEMO_CAT_SCALE 256
