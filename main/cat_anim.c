#include "cat_anim.h"
#include "ui_layout.h"
#include "lvgl.h"
#include <stdint.h>
#include <string.h>

static void transform_frame(uint16_t *destination, const uint16_t *source,
                            cat_anim_state_t state)
{
#if MEMO_UI_COMPACT
    int source_top = 0;
    int source_bottom = MEMO_CAT_ASSET_HEIGHT - 1;
    int shifted_top = 0;
    if (state == CAT_ANIM_IDLE) {
        source_top = MEMO_CAT_ASSET_HEIGHT;
        source_bottom = -1;
        for (int y = 0; y < MEMO_CAT_ASSET_HEIGHT; ++y) {
            for (int x = 0; x < MEMO_CAT_ASSET_WIDTH; ++x) {
                if (source[y * MEMO_CAT_ASSET_WIDTH + x] != 0) {
                    if (y < source_top) source_top = y;
                    if (y > source_bottom) source_bottom = y;
                    break;
                }
            }
        }
        if (source_bottom < source_top) {
            memset(destination, 0, MEMO_CAT_CANVAS_WIDTH * MEMO_CAT_CANVAS_HEIGHT *
                   sizeof(*destination));
            return;
        }
        shifted_top = (MEMO_CAT_ASSET_HEIGHT - (source_bottom - source_top + 1)) / 2;
    }

    /* Nearest-neighbour sampling matches the former LVGL image scale closely,
     * but runs once in the low-priority decoder task.  LVGL then copies a
     * native 218 x 97 image instead of doing the generic transform while TLS
     * streams a multi-megabyte recording from the same PSRAM. */
    for (int y = 0; y < MEMO_CAT_CANVAS_HEIGHT; ++y) {
        int mapped_y = y * MEMO_CAT_ASSET_HEIGHT / MEMO_CAT_CANVAS_HEIGHT;
        int source_y = mapped_y;
        if (state == CAT_ANIM_IDLE) {
            if (mapped_y < shifted_top || mapped_y > shifted_top + source_bottom - source_top) {
                memset(destination + y * MEMO_CAT_CANVAS_WIDTH, 0,
                       MEMO_CAT_CANVAS_WIDTH * sizeof(*destination));
                continue;
            }
            source_y = source_top + mapped_y - shifted_top;
        }
        for (int x = 0; x < MEMO_CAT_CANVAS_WIDTH; ++x) {
            int source_x = x * MEMO_CAT_ASSET_WIDTH / MEMO_CAT_CANVAS_WIDTH;
            destination[y * MEMO_CAT_CANVAS_WIDTH + x] =
                source[source_y * MEMO_CAT_ASSET_WIDTH + source_x];
        }
    }
#else
    (void)state;
    memcpy(destination, source, MEMO_CAT_ASSET_WIDTH * MEMO_CAT_ASSET_HEIGHT *
           sizeof(*destination));
#endif
}

#ifdef ESP_PLATFORM

#include <inttypes.h>
#include "anim_player.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "memo_cat";
static anim_player_handle_t player;
static QueueHandle_t state_queue;
static lv_obj_t *canvas;
static uint16_t *canvas_buffer;

extern const uint8_t _binary_emotion_sleep_284_126_aaf_start[];
extern const uint8_t _binary_emotion_sleep_284_126_aaf_end[];
extern const uint8_t _binary_emotion_blink_slow_284_126_aaf_start[];
extern const uint8_t _binary_emotion_blink_slow_284_126_aaf_end[];
extern const uint8_t _binary_emotion_blink_cardless_284_126_aaf_start[];
extern const uint8_t _binary_emotion_blink_cardless_284_126_aaf_end[];
extern const uint8_t _binary_emotion_blink1_284_126_aaf_start[];
extern const uint8_t _binary_emotion_blink1_284_126_aaf_end[];
extern const uint8_t _binary_emotion_happy_284_126_aaf_start[];
extern const uint8_t _binary_emotion_happy_284_126_aaf_end[];
extern const uint8_t _binary_emotion_sad_284_126_aaf_start[];
extern const uint8_t _binary_emotion_sad_284_126_aaf_end[];

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *name;
    bool repeat;
    uint8_t fps;
} asset_t;

static const asset_t assets[] = {
    [CAT_ANIM_IDLE] = {_binary_emotion_sleep_284_126_aaf_start,
                       _binary_emotion_sleep_284_126_aaf_end, "sleep", false, 33},
    [CAT_ANIM_RECORDING] = {_binary_emotion_blink_slow_284_126_aaf_start,
                            _binary_emotion_blink_slow_284_126_aaf_end, "slow_blink", true, 33},
    [CAT_ANIM_SAVING] = {_binary_emotion_blink1_284_126_aaf_start,
                         _binary_emotion_blink1_284_126_aaf_end, "blink", true, 33},
    [CAT_ANIM_SAVED] = {_binary_emotion_happy_284_126_aaf_start,
                        _binary_emotion_happy_284_126_aaf_end, "happy", true, 33},
    [CAT_ANIM_PLAYING] = {_binary_emotion_blink_slow_284_126_aaf_start,
                          _binary_emotion_blink_slow_284_126_aaf_end, "slow_blink", true, 33},
    [CAT_ANIM_ERROR] = {_binary_emotion_sad_284_126_aaf_start,
                        _binary_emotion_sad_284_126_aaf_end, "sad", true, 33},
    // Derived from slow blink: same eye art, no repeated still frames.
    [CAT_ANIM_CARDLESS] = {_binary_emotion_blink_cardless_284_126_aaf_start,
                          _binary_emotion_blink_cardless_284_126_aaf_end,
                          "cardless_slow_blink_1hz", false, 99},
};

static volatile cat_anim_state_t active_animation = (cat_anim_state_t)-1;
static volatile uint32_t active_frame_count;
static volatile uint32_t completed_frame_count;
static volatile bool final_frame_flushed;
static volatile uint32_t cardless_cycle_frames;
static volatile int64_t cardless_cycle_first_us;
static volatile int64_t cardless_finished_us;
static volatile uint8_t cardless_cycles_logged;
static volatile int64_t cardless_previous_cycle_us;
static volatile uint8_t cardless_cadences_logged;

/* Target one blink per second even when audio capture changes decode time.
 * Preserve a brief neutral last frame if rendering takes longer than a cycle. */
#define CARDLESS_PERIOD_US INT64_C(1000000)
#define CARDLESS_MIN_HOLD_US INT64_C(80000)

static void animation_updated(anim_player_handle_t handle, player_event_t event)
{
    (void)handle;
    if (event == PLAYER_EVENT_ONE_FRAME_DONE) {
        if (completed_frame_count < active_frame_count) completed_frame_count++;
        if (active_animation == CAT_ANIM_CARDLESS &&
            cardless_cycle_frames < active_frame_count) {
            if (cardless_cycle_frames++ == 0) {
                int64_t now = esp_timer_get_time();
                if (cardless_previous_cycle_us > 0 && cardless_cadences_logged < 6) {
                    ESP_LOGI(TAG, "CAT_ANIM_CADENCE interval_ms=%lld",
                             (long long)((now - cardless_previous_cycle_us) / 1000));
                    cardless_cadences_logged++;
                }
                cardless_previous_cycle_us = now;
                cardless_cycle_first_us = now;
            }
            if (cardless_cycle_frames == active_frame_count) {
                if (cardless_cycles_logged < 3) {
                    int64_t elapsed_us = esp_timer_get_time() - cardless_cycle_first_us;
                    ESP_LOGI(TAG, "CAT_ANIM_RATE name=cardless_slow_blink_1hz frames=%" PRIu32
                             " elapsed_ms=%lld effective_fps=%lld",
                             cardless_cycle_frames, (long long)(elapsed_us / 1000),
                             (long long)(elapsed_us > 0 ?
                             (cardless_cycle_frames - 1) * INT64_C(1000000) / elapsed_us : 0));
                    cardless_cycles_logged++;
                }
                cardless_cycle_frames = 0;
            }
        }
        return;
    }
    cat_anim_state_t state = active_animation;
    if (event == PLAYER_EVENT_ALL_FRAME_DONE && state == CAT_ANIM_CARDLESS) {
        cardless_finished_us = esp_timer_get_time();
    }
    if (event == PLAYER_EVENT_ALL_FRAME_DONE && state == CAT_ANIM_IDLE &&
        active_frame_count &&
        completed_frame_count >= active_frame_count && final_frame_flushed) {
        ESP_LOGI(TAG, "CAT_ANIM_HOLD name=%s frames=%" PRIu32,
                 assets[state].name, active_frame_count);
    }
}

static void flush_frame(anim_player_handle_t handle, int x1, int y1, int x2, int y2,
                        const void *pixels)
{
    (void)handle;
    cat_anim_state_t state = active_animation;
    bool is_final = state == CAT_ANIM_IDLE && active_frame_count &&
                    completed_frame_count + 1 >= active_frame_count;
    // A missed frame is preferable to delaying recording or recognition.
    // The one-shot sleep animation is only used while idle, so its final frame
    // gets a longer lock window: the held closed-eye pose must reach the canvas.
    if (lvgl_port_lock(is_final ? 250 : 10)) {
        const uint16_t *source = pixels;
        if (x1 == 0 && y1 == 0 && x2 == MEMO_CAT_ASSET_WIDTH &&
            y2 == MEMO_CAT_ASSET_HEIGHT) {
            transform_frame(canvas_buffer, source, state);
        }
#if !MEMO_UI_COMPACT
        else {
            for (int y = y1; y < y2; ++y) {
                memcpy(canvas_buffer + y * MEMO_CAT_CANVAS_WIDTH + x1, source,
                       (size_t)(x2 - x1) * sizeof(*canvas_buffer));
                source += x2 - x1;
            }
        }
#endif
        if (is_final) final_frame_flushed = true;
        lv_obj_invalidate(canvas);
        lvgl_port_unlock();
    } else if (is_final) {
        ESP_LOGE(TAG, "sleep final frame missed LVGL lock");
    }
    anim_player_flush_ready(player);
}

static void animation_worker(void *unused)
{
    (void)unused;
    cat_anim_state_t requested;
    cat_anim_state_t active = (cat_anim_state_t)-1;
    for (;;) {
        if (xQueueReceive(state_queue, &requested,
                          active == CAT_ANIM_CARDLESS ? pdMS_TO_TICKS(50) : portMAX_DELAY) != pdTRUE) {
            int64_t finished_us = cardless_finished_us;
            int64_t now = esp_timer_get_time();
            if (active == CAT_ANIM_CARDLESS && finished_us > 0 &&
                now - finished_us >= CARDLESS_MIN_HOLD_US &&
                now - cardless_cycle_first_us >= CARDLESS_PERIOD_US) {
                cardless_finished_us = 0;
                anim_player_update(player, PLAYER_ACTION_START);
            }
            continue;
        }
        if (requested == active || requested < CAT_ANIM_IDLE || requested > CAT_ANIM_CARDLESS) continue;
        const asset_t *asset = &assets[requested];
        size_t size = (size_t)(asset->end - asset->start);
        if (anim_player_set_src_data(player, asset->start, size) != ESP_OK) {
            ESP_LOGE(TAG, "asset rejected name=%s bytes=%zu", asset->name, size);
            continue;
        }
        uint32_t first = 0, last = 0;
        anim_player_get_segment(player, &first, &last);
        anim_player_set_segment(player, first, last, asset->fps, asset->repeat);
        active_frame_count = last - first + 1;
        completed_frame_count = 0;
        final_frame_flushed = false;
        cardless_cycle_frames = 0;
        cardless_cycle_first_us = 0;
        cardless_finished_us = 0;
        cardless_cycles_logged = 0;
        cardless_previous_cycle_us = 0;
        cardless_cadences_logged = 0;
        active_animation = requested;
        anim_player_update(player, PLAYER_ACTION_START);
        active = requested;
        ESP_LOGI(TAG, "CAT_ANIM name=%s frames=%" PRIu32 " fps=%u repeat=%d",
                 asset->name, last - first + 1, asset->fps, asset->repeat);
    }
}

bool cat_anim_init(lv_obj_t *parent)
{
    canvas_buffer = heap_caps_calloc(MEMO_CAT_CANVAS_WIDTH * MEMO_CAT_CANVAS_HEIGHT,
                                     sizeof(*canvas_buffer),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!canvas_buffer) {
        ESP_LOGE(TAG, "no PSRAM for canvas");
        return false;
    }
    canvas = lv_canvas_create(parent);
    /* Both the original 284 x 126 canvas and the compact native-size canvas
     * use their centre pivot, keeping every expression at the screen centre. */
    lv_obj_set_pos(canvas, MEMO_CAT_X, MEMO_CAT_Y);
    lv_canvas_set_buffer(canvas, canvas_buffer, MEMO_CAT_CANVAS_WIDTH,
                         MEMO_CAT_CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_image_set_scale(canvas, MEMO_CAT_SCALE);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    anim_player_config_t config = {
        .flush_cb = flush_frame,
        .update_cb = animation_updated,
        .user_data = canvas,
        /* LVGL 9's RGB565 canvas expects native pixel words. The panel flush
         * swaps bytes once for QSPI; the LVGL 8 example's pre-swap would turn
         * neutral gray into green/cyan here. */
        .flags = {.swap = false},
        .task = ANIM_PLAYER_INIT_CONFIG(),
    };
    config.task.task_priority = 2;
    config.task.task_affinity = 1;
    player = anim_player_init(&config);
    state_queue = xQueueCreate(1, sizeof(cat_anim_state_t));
    if (!player || !state_queue ||
        xTaskCreate(animation_worker, "cat_anim", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "animation worker init failed");
        return false;
    }
    cat_anim_set(CAT_ANIM_IDLE);
    return true;
}

void cat_anim_set(cat_anim_state_t state)
{
    if (state_queue) xQueueOverwrite(state_queue, &state);
}

#else

// The host preview draws lightweight vector eyes; AAF decoding is verified on ESP.
#ifdef CAT_ANIM_TEST
static cat_anim_state_t test_state = CAT_ANIM_IDLE;
#endif

bool cat_anim_init(lv_obj_t *parent)
{
    (void)parent;
#ifdef CAT_ANIM_TEST
    test_state = CAT_ANIM_IDLE;
#endif
    return false;
}

void cat_anim_set(cat_anim_state_t state)
{
#ifdef CAT_ANIM_TEST
    test_state = state;
#else
    (void)state;
#endif
}

#ifdef CAT_ANIM_TEST
cat_anim_state_t cat_anim_test_state(void)
{
    return test_state;
}

void cat_anim_test_transform_frame(unsigned short *destination,
                                   const unsigned short *source,
                                   cat_anim_state_t state)
{
    transform_frame(destination, source, state);
}
#endif

#endif
