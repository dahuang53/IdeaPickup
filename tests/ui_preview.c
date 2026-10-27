// LVGL host checks only. This does not exercise the physical screen/touch driver.
#include "app.h"
#include "cat_anim.h"
#include "network_sync.h"
#include "power_policy.h"
#include "ui_layout.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(memo_font);
static memo_snapshot_t model;
static uint32_t frame[MEMO_UI_WIDTH * MEMO_UI_HEIGHT];
static memo_cmd_t last_command;
static uint32_t last_arg;
static unsigned commands;
static bool storage_view;
static lv_indev_data_t pointer;
static memo_network_status_t network;
static memo_config_public_t service={.enabled=true};
static memo_power_status_t power = {
    .supported = true,
    .initialized = true,
    .battery_valid = true,
    .battery_raw = 2280,
    .battery_percent = 80,
};

#if MEMO_UI_COMPACT
#define TAP_X 120
#define TAP_Y 104
#define ROW0_Y 162
#define ROW0_X 120
#define PRIMARY_Y 104
#define NEXT_X 183
#define PREVIOUS_X 57
#define NAV_Y 188
#define NAV_UPPER_Y 149
#define NAV_LOWER_Y 217
#else
#define TAP_X 180
#define TAP_Y 176
#define ROW0_X 130
#define ROW0_Y 177
#define PRIMARY_Y 93
#define NEXT_X 299
#define PREVIOUS_X 61
#define NAV_Y 253
#define NAV_UPPER_Y 170
#define NAV_LOWER_Y 265
#endif

void app_storage_view(bool visible) {storage_view=visible;}

bool app_command(memo_cmd_t command, uint32_t arg, const char *source)
{
    assert(strcmp(source, "touch") == 0 || strcmp(source, "idle_timer") == 0);
    last_command = command;
    last_arg = arg;
    commands++;
    return true;
}

void app_snapshot(memo_snapshot_t *out) { *out = model; }
void network_sync_get_status(memo_network_status_t *out) { *out = network; }
void network_sync_get_service_config(memo_config_public_t *out) { *out=service; }
static memo_record_sync_t detail_job;
static unsigned retry_calls,cancel_calls;
static bool retry_accept=true;
static unsigned select_calls;
void network_sync_select_record(uint32_t id) { select_calls++;detail_job=(memo_record_sync_t){.id=id,.state=MEMO_JOB_LOCAL,.cardless=(id&UINT32_C(0x80000000))!=0,.can_retry=(id&UINT32_C(0x80000000))==0}; }
void network_sync_record_status(memo_record_sync_t *out) { *out=detail_job; }
bool network_sync_retry_record(uint32_t id) { assert(id==detail_job.id);retry_calls++;if(retry_accept)detail_job.request_pending=true;return retry_accept; }
bool network_sync_cancel_record(uint32_t id) { assert(id==detail_job.id);cancel_calls++;detail_job.can_cancel=false;detail_job.state=MEMO_JOB_CANCELLING;return true; }
void power_policy_note_activity(const char *source) { (void)source; }
void power_policy_get_status(memo_power_status_t *out) { *out = power; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    *data = pointer;
}

static void tick(unsigned ms)
{
    lv_tick_inc(ms);
    lv_timer_handler();
    lv_refr_now(NULL);
}

static void check_object(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    bool draws_rect = lv_obj_check_type(obj, &lv_label_class) ||
                      lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != LV_OPA_TRANSP;
    if (draws_rect && obj != lv_screen_active() &&
        !(lv_obj_get_x(obj) == 0 && lv_obj_get_y(obj) == 0 &&
          lv_obj_get_width(obj) == MEMO_UI_WIDTH && lv_obj_get_height(obj) == MEMO_UI_HEIGHT)) {
        const int xs[] = { a.x1, a.x2 }, ys[] = { a.y1, a.y2 };
        const int radius = (MEMO_UI_WIDTH < MEMO_UI_HEIGHT ? MEMO_UI_WIDTH : MEMO_UI_HEIGHT) / 2;
        for (unsigned x = 0; x < 2; x++) for (unsigned y = 0; y < 2; y++) {
            int dx = xs[x] - MEMO_UI_WIDTH / 2, dy = ys[y] - MEMO_UI_HEIGHT / 2;
            if (dx * dx + dy * dy > radius * radius) {
                fprintf(stderr, "outside round display: %d,%d..%d,%d\n", a.x1, a.y1, a.x2, a.y2);
                abort();
            }
        }
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        const lv_font_t *label_font=lv_obj_get_style_text_font(obj,0);
        int width=0;
        const char *line=text;
        do {
            const char *end=strchr(line,'\n');
            size_t length=end?(size_t)(end-line):strlen(line);
            int line_width=lv_text_get_width(line,length,label_font,0);
            if(line_width>width)width=line_width;
            if(!end)break;
            line=end+1;
        } while(1);
        if (width > lv_obj_get_width(obj)) {
            fprintf(stderr, "label overflow: %s (%d > %ld)\n", text, width, (long)lv_obj_get_width(obj));
            abort();
        }
        const unsigned char *p = (const unsigned char *)text;
        while (*p) {
            uint32_t cp = *p++;
            if (cp == '\n') continue;
            if (cp >= 0xC0 && cp < 0xE0) { cp = (cp & 31) << 6; cp |= *p++ & 63; }
            else if (cp >= 0xE0 && cp < 0xF0) {
                cp = (cp & 15) << 12; cp |= (*p++ & 63) << 6; cp |= *p++ & 63;
            }
            lv_font_glyph_dsc_t glyph;
            if (!label_font->get_glyph_dsc(label_font, &glyph, cp, 0)) {
                fprintf(stderr, "missing glyph U+%04X\n", (unsigned)cp);
                abort();
            }
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) check_object(lv_obj_get_child(obj, i));
}

static void save_frame(const char *name)
{
    tick(220);
    check_object(lv_screen_active());
    char path[1024];
    const char *output=getenv("MEMO_UI_OUTPUT");if(!output)output="evidence";
#if defined(MEMO_UI_VOCAT16_PREVIEW)
    snprintf(path, sizeof(path), "%s/ui-vocat16-%s.ppm", output,name);
#else
    snprintf(path, sizeof(path), MEMO_UI_COMPACT ? "%s/ui-240-%s.ppm" : "%s/ui-%s.ppm", output,name);
#endif
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", MEMO_UI_WIDTH, MEMO_UI_HEIGHT);
    const int radius = (MEMO_UI_WIDTH < MEMO_UI_HEIGHT ? MEMO_UI_WIDTH : MEMO_UI_HEIGHT) / 2;
    for (int y = 0; y < MEMO_UI_HEIGHT; y++) for (int x = 0; x < MEMO_UI_WIDTH; x++) {
        uint32_t pixel = frame[y * MEMO_UI_WIDTH + x];
        int dx = x - MEMO_UI_WIDTH / 2, dy = y - MEMO_UI_HEIGHT / 2;
        if (dx * dx + dy * dy > radius * radius) pixel = 0;
        unsigned char rgb[] = { pixel >> 16, pixel >> 8, pixel };
        assert(fwrite(rgb, 1, 3, f) == 3);
    }
    assert(fclose(f) == 0);
    if (!strncmp(name, "cat-", 4)) {
        const unsigned samples[] = {
            (MEMO_UI_HEIGHT / 2) * MEMO_UI_WIDTH + 12,
            (MEMO_UI_HEIGHT / 2) * MEMO_UI_WIDTH + MEMO_UI_WIDTH - 12,
            12 * MEMO_UI_WIDTH + MEMO_UI_WIDTH / 2,
            (MEMO_UI_HEIGHT - 12) * MEMO_UI_WIDTH + MEMO_UI_WIDTH / 2,
        };
        for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
            assert((frame[samples[i]] & 0xFFFFFF) == 0);
        }
    }
    printf("HOST_UI: %s glyphs, label widths and circle bounds PASS\n", name);
}

static void press(lv_indev_t *indev,int x,int y,bool down) {
    pointer.point.x=x;pointer.point.y=y;
    pointer.state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);tick(240);
}

static void click(lv_indev_t *indev, int x, int y, memo_cmd_t expected, uint32_t arg)
{
#if !MEMO_UI_COMPACT
    if (expected==CMD_START || expected==CMD_STOP || expected==CMD_REFRESH) y=PRIMARY_Y;
#endif
    unsigned before = commands;
    pointer.point.x = x;
    pointer.point.y = y;
    pointer.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    // The 200 ms UI update must not destroy a held button before release.
    tick(240);
    pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    assert(commands == before + 1);
    assert(last_command == expected && last_arg == arg);
}

static void click_ignored(lv_indev_t *indev, int x, int y)
{
    unsigned before = commands;
    pointer.point.x = x; pointer.point.y = y;
    pointer.state = LV_INDEV_STATE_PRESSED; lv_indev_read(indev);
    tick(240);
    pointer.state = LV_INDEV_STATE_RELEASED; lv_indev_read(indev);
    assert(commands == before);
}

static lv_obj_t *find_visible_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found=find_visible_label(lv_obj_get_child(obj, i),text);
        if(found)return found;
    }
    return NULL;
}

static bool contains_visible_label(lv_obj_t *obj, const char *text)
{
    return find_visible_label(obj,text)!=NULL;
}

static void check_provision_url(void)
{
    lv_obj_t *url = find_visible_label(lv_screen_active(), "打开 http://192.168.4.1");
    lv_obj_t *ap = find_visible_label(lv_screen_active(), "热点 MeowInbox-D69C");
    lv_obj_t *exit_text = find_visible_label(lv_screen_active(), "退出配网");
    assert(url && ap && exit_text);
    assert(!find_visible_label(lv_screen_active(), "无需密码"));
    assert(!find_visible_label(lv_screen_active(), "密码"));
    lv_area_t hint, ap_line, button;
    lv_obj_get_coords(url, &hint);
    lv_obj_get_coords(ap, &ap_line);
    lv_obj_get_coords(lv_obj_get_parent(exit_text), &button);
    int stride = hint.y1 - ap_line.y1;
    int line_height = ap_line.y2 - ap_line.y1 + 1;
    assert(stride >= line_height && stride < 2 * line_height);
    assert(ap_line.x1 + ap_line.x2 == hint.x1 + hint.x2);
#if !MEMO_UI_COMPACT
    lv_obj_t *instruction = find_visible_label(lv_screen_active(), "手机连接下面的临时热点");
    assert(instruction);
    lv_area_t instruction_line;
    lv_obj_get_coords(instruction, &instruction_line);
    assert(stride == ap_line.y1 - instruction_line.y1);
    assert(instruction_line.x1 + instruction_line.x2 == ap_line.x1 + ap_line.x2);
#endif
    assert(lv_color_eq(lv_obj_get_style_text_color(url, LV_PART_MAIN),
                       lv_obj_get_style_text_color(ap, LV_PART_MAIN)));
    assert(hint.y2 < button.y1);
}

static unsigned visible_label_count(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;
    unsigned count = lv_obj_check_type(obj, &lv_label_class) ? 1 : 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        count += visible_label_count(lv_obj_get_child(obj, i));
    }
    return count;
}

static void storage_panel_cases(lv_indev_t *indev)
{
    memo_snapshot_t original=model;
    unsigned before=commands;
    assert(!storage_view);
    click_ignored(indev,TAP_X,TAP_Y);
    assert(storage_view);
    // Demand probing completes after the first tap. Only media changes: the
    // recording state remains IDLE, so the controls must stay open.
    model.sd_ready=false;model.cardless=true;model.cardless_ready=false;
    model.item_count=0;model.count=0;model.page_count=0;model.revision++;
    tick(220);
    assert(contains_visible_label(lv_screen_active(),"开始录音"));
    assert(contains_visible_label(lv_screen_active(),"无卡"));
    assert(storage_view);
    model.cardless_ready=true;model.revision++;tick(220);
    assert(contains_visible_label(lv_screen_active(),"开始录音") && storage_view);
    // Existing inactivity timeout still ends the view demand.
    tick(60100);assert(!storage_view && visible_label_count(lv_screen_active())==0);
    click_ignored(indev,TAP_X,TAP_Y);
    model=original;model.revision+=10;tick(220);
    assert(contains_visible_label(lv_screen_active(),"开始录音") && storage_view);
    assert(!contains_visible_label(lv_screen_active(),"无卡"));
    // A real recording transition must still return to the cat immediately.
    model.state=MEMO_RECORDING;model.revision++;tick(220);
    assert(!storage_view && !contains_visible_label(lv_screen_active(),"停止录音"));
    model=original;model.revision+=20;tick(220);
    assert(!storage_view && visible_label_count(lv_screen_active())==0);
    assert(commands==before);
    puts("HOST_UI: first-tap media adoption preserves controls and demand; readiness, insertion, timeout and real state transition PASS");
}

static void panel_timeout_cases(lv_indev_t *indev)
{
    click_ignored(indev,TAP_X,TAP_Y);
    tick(15100);
    assert(storage_view && contains_visible_label(lv_screen_active(),"开始录音"));
    tick(40000);
    assert(storage_view); // Reading for55s no longer closes the page.
    click(indev,NEXT_X,NAV_Y,CMD_NEXT,0); // A normal touch starts a fresh60s.
    tick(55100);
    assert(storage_view && contains_visible_label(lv_screen_active(),"开始录音"));
    tick(5100);
    assert(!storage_view && visible_label_count(lv_screen_active())==0);
    puts("HOST_UI: controls stay at15s/55s, touch renews60s, timeout clears view demand PASS");
}

static void usb_voice_cases(lv_indev_t *indev)
{
    memo_snapshot_t before=model;
#if MEMO_UI_COMPACT
    const int mute_x=120,mute_y=128,exit_y=179,back_y=219;
#else
    const int mute_x=180,mute_y=187,exit_y=263,back_y=328;
#endif
    model=(memo_snapshot_t){.state=MEMO_USB_MIC,.sd_ready=true,.usb_available=true,
        .revision=before.revision+1};
    save_frame("cat-usb-waiting");
    assert(visible_label_count(lv_screen_active())==0);
    assert(cat_anim_test_state()==CAT_ANIM_SAVED);
    tick(850);
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    click_ignored(indev,TAP_X,TAP_Y);
    save_frame("usb-waiting");
    assert(contains_visible_label(lv_screen_active(),
        MEMO_UI_COMPACT?"电脑选喵伴麦克风":"请在电脑选择喵伴麦克风"));
    click(indev,mute_x,mute_y,CMD_USB_MUTE,0);
    model.usb_connected=true;model.usb_streaming=true;model.level=58;model.revision++;
    save_frame("usb-active");
    assert(contains_visible_label(lv_screen_active(),"电脑正听着喵"));
    model.usb_local_muted=true;model.usb_muted=true;model.revision++;
    save_frame("usb-muted");
    assert(contains_visible_label(lv_screen_active(),"取消静音"));
    click_ignored(indev,TAP_X,back_y);
    save_frame("cat-usb-muted");
    assert(visible_label_count(lv_screen_active())==0);
    click_ignored(indev,TAP_X,TAP_Y);
    click(indev,mute_x,mute_y,CMD_USB_UNMUTE,0);
    model.usb_local_muted=false;model.usb_muted=true;model.revision++;
    save_frame("usb-host-muted");
    assert(contains_visible_label(lv_screen_active(),"电脑静音中喵"));
    assert(contains_visible_label(lv_screen_active(),"请在电脑取消静音"));
    click_ignored(indev,mute_x,mute_y);
    model.usb_muted=false;model.usb_connected=false;model.usb_suspended=true;model.revision++;
    save_frame("usb-suspended");
    assert(contains_visible_label(lv_screen_active(),"电脑暂时休息喵"));
    model.usb_suspended=false;model.usb_connected=true;model.revision++;
    tick(220);
    unsigned before_idle=commands;
    tick(121000);
    assert(commands==before_idle);
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    click_ignored(indev,TAP_X,back_y);
    save_frame("cat-usb-active");
    assert(visible_label_count(lv_screen_active())==0);
    model.voice_window=true;model.voice_wake_count++;model.revision++;
    save_frame("cat-usb-wake");
    assert(visible_label_count(lv_screen_active())==0);
    assert(cat_anim_test_state()==CAT_ANIM_SAVED);
    tick(700);
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    model.voice_window=false;model.revision++;
    click_ignored(indev,TAP_X,TAP_Y);
    model.voice_window=true;model.revision++;tick(220);
    click(indev,TAP_X,exit_y,CMD_USB_CLOSE,0);
    model.state=MEMO_IDLE;model.usb_streaming=false;model.voice_window=false;model.revision++;
    save_frame("cat-after-usb");
    assert(cat_anim_test_state()==CAT_ANIM_SAVED);
    tick(850);
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    tick(118150);
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    tick(1000);
    assert(cat_anim_test_state()==CAT_ANIM_IDLE);
    snprintf(model.notice,sizeof(model.notice),"还没连上电脑喵");model.revision++;
    save_frame("control-usb-notice");
    assert(contains_visible_label(lv_screen_active(),"还没连上电脑喵"));
    snprintf(model.notice,sizeof(model.notice),"电脑连接暂停，麦克风已关喵");model.revision++;
    save_frame("control-usb-paused-notice");
    model.notice[0]='\0';model.revision++;
    tick(220);
    model.voice_ready=true;model.voice_window=true;model.voice_wake_count++;model.revision++;
    save_frame("cat-voice-choice");
    assert(visible_label_count(lv_screen_active())==0);
    assert(cat_anim_test_state()==CAT_ANIM_SAVED);
    click_ignored(indev,TAP_X,TAP_Y);
    save_frame("control-voice-choice");
#if MEMO_UI_COMPACT
    assert(contains_visible_label(lv_screen_active(),"说开始录音\n或打开麦克风"));
#else
    assert(contains_visible_label(lv_screen_active(),"请说开始录音或打开麦克风"));
#endif
    tick(700);
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    model.voice_window=false;model.revision++;
    save_frame("cat-after-voice-choice");
    assert(cat_anim_test_state()==CAT_ANIM_RECORDING);
    model=(memo_snapshot_t){.state=MEMO_IDLE,.sd_ready=true,.count=1,.page_count=1,
        .item_count=1,.revision=model.revision+1,.items={{42,5000,160000}}};
    click_ignored(indev,TAP_X,TAP_Y);
    press(indev,ROW0_X,ROW0_Y,true);press(indev,ROW0_X,ROW0_Y,false);
    assert(detail_job.id==42);
    model.state=MEMO_USB_MIC;model.usb_available=true;model.revision++;
    save_frame("cat-usb-from-detail");
    assert(visible_label_count(lv_screen_active())==0);
    click_ignored(indev,TAP_X,TAP_Y);
    assert(contains_visible_label(lv_screen_active(),"电脑麦克风"));
    model.state=MEMO_IDLE;model.revision++;
    tick(220);
    model=before;model.revision++;
    tick(220);
    puts("HOST_UI: USB waiting/active/muted, mute and exit targets, separate cat return, voice choice and notices PASS");
}

static void cardless_cases(lv_indev_t *indev)
{
    const uint32_t cardless_id=UINT32_C(0x80000000)|12345;
    model=(memo_snapshot_t){.state=MEMO_IDLE,.cardless=true,.cardless_ready=true,
        .voice_ready=true,.count=10,.page_count=4,.item_count=1,.revision=model.revision+1};
    model.items[0]=(memo_item_t){.id=cardless_id,.duration_ms=3755000,.file_bytes=7801234,.cardless=true};
    network.configured=true;network.connected=true;network.error[0]='\0';
    tick(220);
    ui_power_show_awake();tick(220);
    assert(cat_anim_test_state()==CAT_ANIM_CARDLESS);
    assert(visible_label_count(lv_screen_active())==0);
    click_ignored(indev,TAP_X,TAP_Y);
    save_frame("cardless-home");
    assert(contains_visible_label(lv_screen_active(),"无卡"));
#if MEMO_UI_COMPACT
    assert(contains_visible_label(lv_screen_active(),"无卡12345"));
#else
    assert(contains_visible_label(lv_screen_active(),"无卡12345\n1:02:35"));
#endif
    click(indev,NEXT_X,NAV_Y,CMD_NEXT,0); // Metadata pages do not require microSD.
    click(indev,TAP_X,TAP_Y,CMD_START,0);
    model.state=MEMO_RECORDING;model.cardless_ready=false;model.elapsed_ms=63000;model.revision++;
    tick(220);assert(cat_anim_test_state()==CAT_ANIM_SAVED);
    tick(1600);assert(cat_anim_test_state()==CAT_ANIM_CARDLESS);
    assert(visible_label_count(lv_screen_active())==0);
    click_ignored(indev,TAP_X,TAP_Y);save_frame("cardless-recording");
    assert(contains_visible_label(lv_screen_active(),"无卡"));
    assert(contains_visible_label(lv_screen_active(),"录音中"));
    click(indev,TAP_X,TAP_Y,CMD_STOP,0); // Busy buffer must never disable stopping.
    model.state=MEMO_SAVED;model.revision++;tick(2800);tick(2800);
    assert(cat_anim_test_state()==CAT_ANIM_CARDLESS);
    click_ignored(indev,TAP_X,TAP_Y);save_frame("cardless-finished");
    assert(contains_visible_label(lv_screen_active(),"已结束"));
    assert(!contains_visible_label(lv_screen_active(),"已保存"));
    click_ignored(indev,TAP_X,PRIMARY_Y); // Awaiting transport: cannot overwrite it.
    model.state=MEMO_IDLE;model.revision++;tick(220);
    click_ignored(indev,TAP_X,TAP_Y);
    press(indev,ROW0_X,ROW0_Y,true);press(indev,ROW0_X,ROW0_Y,false);tick(220);
    assert(detail_job.id==cardless_id);
    save_frame("cardless-detail");
    assert(contains_visible_label(lv_screen_active(),"无卡 #12345"));
    assert(!contains_visible_label(lv_screen_active(),"播放录音"));
    assert(!contains_visible_label(lv_screen_active(),"暂停播放"));
    model.items[0].duration_ms=120000;model.items[0].file_bytes=0;model.revision++;
    tick(220);
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"02:00  0.0 KB":"02:00  ·  0.0 KB"));
#if MEMO_UI_COMPACT
    const int retry_y=192,back_y=222;
#else
    const int retry_y=270,back_y=330;
#endif
    unsigned retries=retry_calls,queries=select_calls;
    press(indev,TAP_X,retry_y,true);press(indev,TAP_X,retry_y,false);tick(220);
    assert(select_calls==queries && retry_calls==retries); // Saved state only; no query or ASR trigger.
    detail_job.state=MEMO_JOB_FAILED;detail_job.can_retry=true;
    snprintf(detail_job.error,sizeof(detail_job.error),"网络中断过久，录音已停止");
    save_frame("cardless-failed-retry");
    assert(contains_visible_label(lv_screen_active(),"继续上传"));
    press(indev,TAP_X,retry_y,true);press(indev,TAP_X,retry_y,false);tick(220);
    assert(retry_calls==retries+1 && detail_job.id==cardless_id);
    detail_job.request_pending=false;detail_job.can_retry=false;detail_job.interrupted=true;
    const char *interruption_errors[]={"断电中断，正在确认已传内容","临时音频已清理，无法重试",
        "断电前未上传，音频已丢失","音频编码失败，录音已中断"};
    for(unsigned i=0;i<sizeof(interruption_errors)/sizeof(interruption_errors[0]);i++) {
        snprintf(detail_job.error,sizeof(detail_job.error),"%s",interruption_errors[i]);
        save_frame("cardless-interruption-reason");
    }
    detail_job.error[0]='\0';detail_job.state=MEMO_JOB_SENT;
    save_frame("cardless-interrupted-sent");
    network.configured=false;network.connected=false;detail_job.state=MEMO_JOB_FAILED;
    save_frame("cardless-saved-failure-offline");
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"处理未完成":"处理未完成喵"));
    network.configured=true;network.connected=true;
    for(int st=MEMO_JOB_UNKNOWN;st<=MEMO_JOB_CANCELLING;st++) {
        detail_job.state=(memo_job_state_t)st;
        save_frame("cardless-detail-status");
    }
    press(indev,TAP_X,back_y,true);press(indev,TAP_X,back_y,false);tick(220);
    assert(detail_job.id==0);
    network.connected=false;model.revision++;tick(220);
    save_frame("cardless-offline");
    click_ignored(indev,TAP_X,PRIMARY_Y);
    assert(contains_visible_label(lv_screen_active(),"无卡"));
    network.connected=true;model.cardless_ready=true;model.revision++;tick(220);
    assert(contains_visible_label(lv_screen_active(),"开始录音"));
    model.cardless=false;model.sd_ready=true;model.revision++;tick(220);
    assert(contains_visible_label(lv_screen_active(),"开始录音"));
    save_frame("cardless-history-with-sd");
    press(indev,ROW0_X,ROW0_Y,true);press(indev,ROW0_X,ROW0_Y,false);tick(220);
    assert(detail_job.id==cardless_id);
    save_frame("cardless-detail-with-sd");
    assert(!contains_visible_label(lv_screen_active(),"播放录音"));
    assert(contains_visible_label(lv_screen_active(),"无卡 #12345"));
    press(indev,TAP_X,back_y,true);press(indev,TAP_X,back_y,false);tick(220);
    puts("HOST_UI: cardless marker/accent, metadata pages, full ID callbacks, ready/start/stop gating, no playback, query versus retry, interruption and mixed SD history PASS");
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(MEMO_UI_WIDTH, MEMO_UI_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, frame, NULL, sizeof(frame), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_pointer);
    pointer.state = LV_INDEV_STATE_RELEASED;
    model = (memo_snapshot_t){ .state=MEMO_IDLE, .sd_ready=true, .count=10,
        .page=0, .page_count=4, .revision=1, .item_count=3,
        .items={{10,120123,3843980},{9,5345,171084},{8,11111,355596}} };
    network = (memo_network_status_t){ .initialized=true, .configured=true, .connected=true };
    ui_init();
#if MEMO_UI_COMPACT
    assert(MEMO_CAT_SCALE == 256);
    assert(MEMO_CAT_CANVAS_WIDTH <= MEMO_UI_WIDTH - 20);
    assert(MEMO_CAT_CANVAS_HEIGHT < MEMO_UI_HEIGHT / 2);
    assert(MEMO_CAT_X + MEMO_CAT_CANVAS_WIDTH / 2 == MEMO_UI_WIDTH / 2);
    assert(MEMO_CAT_Y + MEMO_CAT_CANVAS_HEIGHT / 2 == MEMO_UI_HEIGHT / 2);
    uint16_t *cat_source = calloc(MEMO_CAT_ASSET_WIDTH * MEMO_CAT_ASSET_HEIGHT,
                                  sizeof(*cat_source));
    uint16_t *cat_destination = calloc(MEMO_CAT_CANVAS_WIDTH * MEMO_CAT_CANVAS_HEIGHT,
                                       sizeof(*cat_destination));
    assert(cat_source && cat_destination);
    cat_source[80 * MEMO_CAT_ASSET_WIDTH + 100] = 1;
    cat_source[89 * MEMO_CAT_ASSET_WIDTH + 100] = 2;
    cat_anim_test_transform_frame(cat_destination, cat_source, CAT_ANIM_IDLE);
    assert(cat_destination[45 * MEMO_CAT_CANVAS_WIDTH + 77] == 1);
    assert(cat_destination[52 * MEMO_CAT_CANVAS_WIDTH + 77] == 2);
    cat_anim_test_transform_frame(cat_destination, cat_source, CAT_ANIM_RECORDING);
    assert(cat_destination[62 * MEMO_CAT_CANVAS_WIDTH + 77] == 1);
    assert(cat_destination[69 * MEMO_CAT_CANVAS_WIDTH + 77] == 2);
    free(cat_source);
    free(cat_destination);
#else
    assert(MEMO_CAT_SCALE == 256);
    assert(MEMO_CAT_X == 38);
    assert(MEMO_CAT_Y == 99);
#endif
    model.voice_ready=true;
    unsigned children = lv_obj_get_child_count(lv_screen_active());
    save_frame("cat-idle");
    assert(visible_label_count(lv_screen_active()) == 0);
    assert(cat_anim_test_state() == CAT_ANIM_IDLE);
    ui_power_show_awake();
    assert(cat_anim_test_state() == CAT_ANIM_RECORDING);
    assert(visible_label_count(lv_screen_active()) == 0);
    ui_power_show_sleep();
    assert(cat_anim_test_state() == CAT_ANIM_IDLE);
    storage_panel_cases(indev);
    panel_timeout_cases(indev);
    click_ignored(indev, TAP_X, TAP_Y); // A first tap opens controls and sends no business command.
    save_frame("control-idle");
    assert(contains_visible_label(lv_screen_active(), "开始录音"));
#if !MEMO_UI_COMPACT
    click_ignored(indev,180,5); // Top center, beyond the former return bands.
    assert(!contains_visible_label(lv_screen_active(),"开始录音"));
    click_ignored(indev,TAP_X,TAP_Y);
    click_ignored(indev,80,65); // Left edge of the visible upper arc.
    assert(!contains_visible_label(lv_screen_active(),"开始录音"));
    click_ignored(indev,TAP_X,TAP_Y);
    click_ignored(indev,280,65); // Right edge of the visible upper arc.
    assert(!contains_visible_label(lv_screen_active(),"开始录音"));
    click_ignored(indev,TAP_X,TAP_Y);
#endif
#if MEMO_UI_COMPACT || defined(MEMO_UI_VOCAT16_PREVIEW)
    assert(contains_visible_label(lv_screen_active(), "80"));
    power.battery_raw = 2480;
    power.battery_percent = 100;
    save_frame("control-battery-full");
    assert(contains_visible_label(lv_screen_active(), "100"));
    power.battery_raw = 2280;
    power.battery_percent = 80;
    tick(220);
#if defined(MEMO_UI_VOCAT16_PREVIEW)
    click_ignored(indev, 248, 49);
#else
    click_ignored(indev, 78, 35); // Battery display keeps the old immediate-return hit target.
#endif
    assert(!contains_visible_label(lv_screen_active(), "开始录音"));
    click_ignored(indev, TAP_X, TAP_Y);
#endif
#if !MEMO_UI_COMPACT
    memo_snapshot_t before_six=model;
    model.count=371;model.page_count=62;model.item_count=6;model.revision++;
    for(unsigned i=0;i<6;i++){
        model.items[i].id=12345-i;
        model.items[i].duration_ms=(i==3?3755000:21000+i*1000);
    }
    save_frame("control-six-records");
    assert(contains_visible_label(lv_screen_active(),"371条  1/62"));
    assert(contains_visible_label(lv_screen_active(),"#12342\n1:02:35"));
    press(indev,230,230,true);press(indev,230,230,false);tick(220);
    assert(detail_job.id==12342);
    press(indev,TAP_X,330,true);press(indev,TAP_X,330,false);tick(220);
    model.count=0;model.page_count=0;model.item_count=0;model.revision++;
    save_frame("control-empty");
    assert(contains_visible_label(lv_screen_active(),"第一条想法，等你开口喵"));
    click_ignored(indev,ROW0_X,ROW0_Y);
    power.battery_low=true;power.battery_percent=8;power.external_power=false;
    save_frame("control-low-battery");
    assert(contains_visible_label(lv_screen_active(),"电量低啦，给我充充电喵"));
    power.battery_low=false;power.battery_percent=80;
    model=before_six;model.revision++;
    tick(220);
#endif
    network.configured = false;
    network.connected = false;
    model.revision++;
    save_frame("control-unconfigured");
#if MEMO_UI_COMPACT
    assert(contains_visible_label(lv_screen_active(), "返回后长按表情配网"));
#else
    assert(contains_visible_label(lv_screen_active(), "长按猫脸三秒，设置网络喵"));
#endif
    network.configured = true;
    network.connected = true;
    model.revision++;
    tick(220);
#if MEMO_UI_COMPACT
    model.count = 207;
    model.page = 16;
    model.page_count = 69;
    model.revision++;
    save_frame("control-many-records");
    assert(contains_visible_label(lv_screen_active(), "记录 207 条  17/69"));
    model.count = 10;
    model.page = 0;
    model.page_count = 4;
    model.revision++;
    tick(220);
#endif
    click(indev, TAP_X, TAP_Y, CMD_START, 0);
    unsigned detail_before=commands;
    press(indev,ROW0_X,ROW0_Y,true);press(indev,ROW0_X,ROW0_Y,false);tick(220);
    assert(commands==detail_before && detail_job.id==10);
    save_frame("record-detail-local");
#if MEMO_UI_COMPACT
    const int play_x=72,play_y=142,retry_x=168,retry_y=142,back_y=194;
#else
    const int play_x=180,play_y=190,retry_x=180,retry_y=270,back_y=330;
    assert(contains_visible_label(lv_screen_active(),"已在本机，尚未同步喵"));
    assert(contains_visible_label(lv_screen_active(),"02:00  ·  3.8 MB"));
    lv_obj_t *action_label=find_visible_label(lv_screen_active(),"播放录音");
    assert(action_label);
    lv_area_t action_area;lv_obj_get_coords(action_label,&action_area);
    assert((action_area.x1+action_area.x2)/2>=195 &&
           (action_area.x1+action_area.x2)/2<=205);
    click_ignored(indev,180,302); // Deliberate no-action band between sync and return.
    assert(detail_job.id==10);
#endif
    click(indev,play_x,play_y,CMD_PLAY,10);
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(retry_calls==1);
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(retry_calls==1); // Busy request cannot be duplicated.
    detail_job.request_pending=false;
    for(int st=MEMO_JOB_UNKNOWN;st<=MEMO_JOB_CANCELLING;st++) {
        detail_job.state=(memo_job_state_t)st;
        save_frame(st==MEMO_JOB_SENT?"record-detail-sent":"record-detail-status");
    }
    detail_job.state=MEMO_JOB_QUEUED;detail_job.can_retry=true;detail_job.can_cancel=false;tick(220);
    assert(contains_visible_label(lv_screen_active(),"提交处理"));
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"仅本地保存":"已在本机，尚未同步喵"));
    unsigned legacy_retry=retry_calls;
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(retry_calls==legacy_retry+1 && detail_job.request_pending);
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"已排队，等待处理":"已排队，等待处理喵"));
    detail_job.request_pending=false;
    detail_job.state=MEMO_JOB_RECEIVED;detail_job.can_retry=false;detail_job.can_cancel=true;tick(220);
    assert(contains_visible_label(lv_screen_active(),"取消处理"));
    unsigned prior_cancel=cancel_calls;
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(cancel_calls==prior_cancel+1 && detail_job.state==MEMO_JOB_CANCELLING);
    detail_job.state=MEMO_JOB_SENT;detail_job.can_retry=detail_job.can_cancel=false;tick(220);
    assert(!contains_visible_label(lv_screen_active(),"提交处理")&&!contains_visible_label(lv_screen_active(),"取消处理"));
    detail_job.state=MEMO_JOB_WAITING;tick(220);
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"等待接收确认":"等待服务器确认喵"));
    detail_job.state=MEMO_JOB_CANCELLED;detail_job.can_retry=true;tick(220);
    assert(contains_visible_label(lv_screen_active(),"提交处理"));
    detail_job.state=MEMO_JOB_LOCAL;retry_accept=false;tick(220);
    unsigned rejected_count=retry_calls;
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(500);
    assert(retry_calls==rejected_count+1 && !detail_job.request_pending);
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"任务繁忙，请稍后重试":"任务繁忙，请稍后重试喵"));
    save_frame("record-detail-busy-feedback");tick(2200);retry_accept=true;
    detail_job.state=MEMO_JOB_FAILED;detail_job.can_retry=false;
    tick(220);assert(!contains_visible_label(lv_screen_active(),"刷新状态"));
    unsigned old_queries=select_calls,old_retries=retry_calls;
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(select_calls==old_queries && retry_calls==old_retries);
    detail_job.state=MEMO_JOB_FAILED;detail_job.can_retry=true;
    tick(220);press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(retry_calls==old_retries+1);
    detail_job.request_pending=false;detail_job.state=MEMO_JOB_UNCERTAIN;tick(220);
    old_retries=retry_calls;old_queries=select_calls;
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(retry_calls==old_retries && select_calls==old_queries);
    network.connected=false;tick(220);old_retries=retry_calls;old_queries=select_calls;
    press(indev,retry_x,retry_y,true);press(indev,retry_x,retry_y,false);tick(220);
    assert(retry_calls==old_retries && select_calls==old_queries);
    network.configured=false;detail_job.state=MEMO_JOB_FAILED;
    save_frame("record-saved-failure-offline");
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"处理失败":"同步失败，录音仍在本机喵"));
    detail_job.state=MEMO_JOB_CHECKING;save_frame("record-local-identity-check");
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"正在核对录音":"正在核对录音喵"));
    network.configured=true;
    network.connected=true;detail_job.state=MEMO_JOB_LOCAL;tick(220);
    assert(contains_visible_label(lv_screen_active(),"提交处理"));
#if !MEMO_UI_COMPACT
    detail_job.state=MEMO_JOB_LOCAL;
    model.state=MEMO_PLAYING;model.active_id=10;model.elapsed_ms=63000;model.revision++;
    save_frame("record-detail-playing");
    assert(contains_visible_label(lv_screen_active(),"暂停播放"));
    assert(contains_visible_label(lv_screen_active(),"01:03/02:00  ·  3.8 MB"));
    click(indev,play_x,play_y,CMD_PAUSE,10);
    unsigned retries_while_playing=retry_calls;
    click_ignored(indev,retry_x,retry_y);
    assert(retry_calls==retries_while_playing);
    model.state=MEMO_PAUSED;model.revision++;
    save_frame("record-detail-paused");
    assert(contains_visible_label(lv_screen_active(),"继续播放"));
    assert(contains_visible_label(lv_screen_active(),"01:03/02:00  ·  3.8 MB"));
    click(indev,play_x,play_y,CMD_RESUME,10);
    unsigned before_exit=commands;
    press(indev,TAP_X,350,true);press(indev,TAP_X,350,false);tick(220);
    assert(commands==before_exit+1 && last_command==CMD_STOP);
    assert(detail_job.id==0);
    model.state=MEMO_IDLE;model.active_id=0;model.elapsed_ms=0;model.revision++;
    tick(220);
    assert(contains_visible_label(lv_screen_active(),"开始录音")); // Return stays on the list.
    press(indev,ROW0_X,ROW0_Y,true);press(indev,ROW0_X,ROW0_Y,false);tick(220);
    save_frame("record-detail-reentered");
    assert(contains_visible_label(lv_screen_active(),"播放录音"));
    assert(contains_visible_label(lv_screen_active(),"02:00  ·  3.8 MB"));
#endif
    press(indev,TAP_X,back_y,true);press(indev,TAP_X,back_y,false);tick(220);
    assert(detail_job.id==0);
#if MEMO_UI_COMPACT
    /* Bottom targets retain their width and extend through the last pixel. */
    const int bottom_points[][2]={{69,226},{170,226},{120,239}};
    for(unsigned i=0;i<sizeof(bottom_points)/sizeof(bottom_points[0]);i++) {
        press(indev,bottom_points[i][0],bottom_points[i][1],true);
        press(indev,bottom_points[i][0],bottom_points[i][1],false);tick(220);
        assert(detail_job.id==8);
        click_ignored(indev,69,220); // Outside the back button's left edge.
        click_ignored(indev,170,220); // Outside its right edge.
        click_ignored(indev,120,174); // Do not expand upward into the action gap.
        assert(detail_job.id==8);
        press(indev,120,239,true);press(indev,120,239,false);tick(220);
        assert(detail_job.id==0);
    }
    click_ignored(indev,68,230);
    click_ignored(indev,171,230);
    click_ignored(indev,120,201);
    assert(detail_job.id==0);
    press(indev,120,188,true);press(indev,120,188,false);tick(220);
    assert(detail_job.id==9); // The second row keeps its own target.
    press(indev,70,220,true);press(indev,70,220,false);tick(220);
    assert(detail_job.id==0);
    model.item_count=2;model.revision++;tick(220);
    click_ignored(indev,120,230);assert(detail_job.id==0); // Hidden third row.
    model.item_count=3;model.sd_ready=false;model.revision++;tick(220);
    if(!contains_visible_label(lv_screen_active(),"#8  00:11"))click_ignored(indev,TAP_X,TAP_Y);
    assert(contains_visible_label(lv_screen_active(),"#8  00:11"));
    click_ignored(indev,120,230);assert(detail_job.id==0); // Disabled without SD.
    model.sd_ready=true;model.revision++;tick(220);
    if(!contains_visible_label(lv_screen_active(),"开始录音"))click_ignored(indev,TAP_X,TAP_Y);
    puts("HOST_UI: compact bottom-row/back downward targets, exact width, hidden/disabled and adjacent controls PASS");
#else
    press(indev,84,ROW0_Y,true);press(indev,84,ROW0_Y,false);tick(220);
    assert(detail_job.id==10); // Card starts immediately after the left paw target.
    press(indev,TAP_X,back_y,true);press(indev,TAP_X,back_y,false);tick(220);
    press(indev,276,ROW0_Y,true);press(indev,276,ROW0_Y,false);tick(220);
    assert(detail_job.id==9); // Card ends before the right paw target.
    press(indev,TAP_X,back_y,true);press(indev,TAP_X,back_y,false);tick(220);
#endif
    click(indev, NEXT_X, NAV_Y, CMD_NEXT, 0);
#if !MEMO_UI_COMPACT
    click(indev,345,NAV_Y,CMD_NEXT,0); // Outer edge of the widened right paw zone.
    click(indev,280,NAV_Y,CMD_NEXT,0); // Inner edge beside the card.
#endif
    click(indev, NEXT_X, NAV_UPPER_Y, CMD_NEXT, 0); // Upper side-button area, outside the triangle.
    click(indev, NEXT_X, NAV_LOWER_Y, CMD_NEXT, 0); // Lower side-button area, outside the triangle.
    model.page = 1;
    model.revision++;
    tick(220);
    click(indev, PREVIOUS_X, NAV_Y, CMD_PREV, 0);
#if !MEMO_UI_COMPACT
    click(indev,15,NAV_Y,CMD_PREV,0); // Outer edge of the widened left paw zone.
    click(indev,78,NAV_Y,CMD_PREV,0); // Inner edge beside the card.
#endif
    click(indev, PREVIOUS_X, NAV_LOWER_Y, CMD_PREV, 0); // Lower left target, outside the triangle.
    model.state = MEMO_RECORDING; model.elapsed_ms = 123456; model.level = 63;
    model.voice_window = false; model.revision++;
    save_frame("cat-recording");
    assert(visible_label_count(lv_screen_active()) == 0);
    assert(cat_anim_test_state() == CAT_ANIM_SAVED); // Wake greeting is briefly happy.
    tick(1600);
    assert(cat_anim_test_state() == CAT_ANIM_RECORDING); // Then slow blink while recording.
    click_ignored(indev, TAP_X, TAP_Y);
    save_frame("control-recording");
    click(indev, TAP_X, TAP_Y, CMD_STOP, 0);
    unsigned before = commands;
    pointer.point.x = ROW0_X; pointer.point.y = ROW0_Y;
    pointer.state = LV_INDEV_STATE_PRESSED; lv_indev_read(indev);
    pointer.state = LV_INDEV_STATE_RELEASED; lv_indev_read(indev);
    assert(commands == before); // Recording disables record selection.
    model.voice_window=true; model.elapsed_ms=124000; model.revision++;
    save_frame("cat-command-window");
    assert(visible_label_count(lv_screen_active()) == 0);
    model.voice_window=false;
    model.state = MEMO_SAVING; model.recovering = true; model.revision++;
    save_frame("control-recovering");
    assert(contains_visible_label(lv_screen_active(), "恢复录音"));
#if MEMO_UI_COMPACT
    assert(contains_visible_label(lv_screen_active(), "80"));
#endif
    click_ignored(indev, TAP_X, TAP_Y);
    model.recovering = false; model.revision++; save_frame("cat-saving");
    model.state = MEMO_SAVED; model.revision++; save_frame("cat-saved");
    assert(visible_label_count(lv_screen_active()) == 0);
    assert(cat_anim_test_state() == CAT_ANIM_SAVED);
    lv_tick_inc(2600); lv_timer_handler(); lv_refr_now(NULL);
    assert(visible_label_count(lv_screen_active()) == 0);
    assert(cat_anim_test_state() == CAT_ANIM_RECORDING); // Saved feedback settles to slow blink.
    unsigned before_idle = commands;
    tick(117000);
    assert(commands == before_idle);
    tick(500);
    assert(commands == before_idle + 1);
    assert(last_command == CMD_IDLE);
    model.state = MEMO_IDLE; model.revision++;
    tick(220);
    assert(cat_anim_test_state() == CAT_ANIM_IDLE);
    model.state = MEMO_SAVED; model.revision++;
    tick(220);
    click_ignored(indev, TAP_X, TAP_Y);
    tick(119000);
    assert(commands == before_idle + 1); // Opening controls reset the two-minute timer.
    click_ignored(indev, TAP_X, TAP_Y); // The panel auto-hid after 60 seconds; reopen it.
    click(indev, TAP_X, TAP_Y, CMD_START, 0);
    model.state = MEMO_PLAYING; model.elapsed_ms = 43000; model.level = 42; model.revision++;
    save_frame("cat-playing");
    assert(visible_label_count(lv_screen_active()) == 0);
    assert(cat_anim_test_state() == CAT_ANIM_PLAYING); // Playing uses the same slow-blink asset.
    click_ignored(indev, TAP_X, TAP_Y);
    save_frame("control-playing");
    click(indev, TAP_X, TAP_Y, CMD_STOP, 0);
    model.state = MEMO_IDLE; model.revision++;
    tick(220);
    assert(cat_anim_test_state() == CAT_ANIM_RECORDING); // Stopped playback stays awake.
    unsigned after_play_stop = commands;
    tick(59000);
    assert(commands == after_play_stop);
    assert(cat_anim_test_state() == CAT_ANIM_RECORDING);
    click_ignored(indev, TAP_X, TAP_Y); // Activity restarts the post-playback timer.
    tick(119000);
    assert(commands == after_play_stop);
    assert(cat_anim_test_state() == CAT_ANIM_RECORDING);
    tick(1000);
    assert(commands == after_play_stop); // Already idle: UI only changes the expression.
    assert(cat_anim_test_state() == CAT_ANIM_IDLE);
    model.state = MEMO_ERROR; model.revision++; save_frame("cat-error");
    snprintf(model.error, sizeof(model.error), "空间不足，未保存");
    save_frame("cat-no-space");
    snprintf(model.error, sizeof(model.error), "录音失败，未保存");
    save_frame("cat-write-error");
    snprintf(model.error, sizeof(model.error), "音频初始化失败");
    save_frame("cat-audio-error");
    snprintf(model.error, sizeof(model.error), "保存失败，请检查卡");
    save_frame("cat-save-error");
    // Keep stale rows/page metadata: storage failure must disable every list action.
    model.sd_ready = false; model.revision++;
    snprintf(model.error, sizeof(model.error), "存储卡挂载失败");
    save_frame("cat-storage-mount-error");
    assert(visible_label_count(lv_screen_active()) == 0);
    click_ignored(indev, TAP_X, TAP_Y);
    save_frame("control-storage-mount-error");
    assert(contains_visible_label(lv_screen_active(), "重试存储卡"));
    assert(contains_visible_label(lv_screen_active(), "请插卡后点重试"));
    click(indev, TAP_X, TAP_Y, CMD_REFRESH, 0); // Must not send CMD_START with storage unavailable.
    click_ignored(indev, ROW0_X, ROW0_Y);
    click_ignored(indev, PREVIOUS_X, NAV_Y);
    click_ignored(indev, NEXT_X, NAV_Y);
    snprintf(model.error, sizeof(model.error), "录音目录创建失败");
    save_frame("control-directory-create-error");
    assert(contains_visible_label(lv_screen_active(), "录音目录读取失败"));
    click(indev, TAP_X, TAP_Y, CMD_REFRESH, 0);
    snprintf(model.error, sizeof(model.error), "录音目录读取失败");
    save_frame("control-directory-read-error");
    assert(contains_visible_label(lv_screen_active(), "录音目录读取失败"));
    snprintf(model.error, sizeof(model.error), "存储卡写入失败");
    save_frame("control-storage-write-error");
    assert(contains_visible_label(lv_screen_active(), "存储卡写入失败"));
    const memo_state_t busy_states[] = { MEMO_RECORDING, MEMO_PLAYING, MEMO_SAVING };
    for (unsigned i = 0; i < sizeof(busy_states) / sizeof(busy_states[0]); i++) {
        model.state = busy_states[i]; model.revision++; tick(220);
        click_ignored(indev, TAP_X, TAP_Y); // Open the controls from the cat view.
        click_ignored(indev, TAP_X, PRIMARY_Y); // Retry cannot interrupt a busy operation.
    }
    model.state = MEMO_ERROR; model.item_count = 0; model.count = 0; model.page = 0;
    model.page_count = 0; model.error[0] = '\0'; model.revision++; save_frame("cat-no-sd");
    click_ignored(indev, TAP_X, TAP_Y);
    assert(contains_visible_label(lv_screen_active(), "请插卡后点重试"));
    click(indev, TAP_X, TAP_Y, CMD_REFRESH, 0);
    model.sd_ready = true; model.state = MEMO_IDLE; model.revision++;
    save_frame("cat-storage-recovered");
    assert(!contains_visible_label(lv_screen_active(), "开始录音"));
    click_ignored(indev, TAP_X, TAP_Y);
    assert(contains_visible_label(lv_screen_active(), "开始录音"));
    lv_tick_inc(60100); lv_timer_handler(); lv_refr_now(NULL);
    assert(!contains_visible_label(lv_screen_active(), "开始录音"));
    click_ignored(indev, TAP_X, TAP_Y);
    click(indev, TAP_X, TAP_Y, CMD_START, 0);
    tick(60100); // Return from the unchanged host stub state to the cat view.
    network.configured = false;
    unsigned before_provision = commands;
    pointer.point.x = TAP_X; pointer.point.y = TAP_Y;
    pointer.state = LV_INDEV_STATE_PRESSED; lv_indev_read(indev);
    tick(3100);
    assert(commands == before_provision + 1 && last_command == CMD_PROVISION);
    pointer.state = LV_INDEV_STATE_RELEASED; lv_indev_read(indev);
    assert(commands == before_provision + 1);
    // Actual network errors on the recording controls, not only portal captions.
    click_ignored(indev, TAP_X, TAP_Y);
    const char *network_errors[] = {
        "配对尚未完成，将自动重试；本地录音可用",
        "网络时间同步失败，配对将自动重试",
        "配对码无效、过期或设备已撤销，请重新配对",
        "服务配置保存结果未确认，稍后自动读取恢复"
    };
    for(unsigned i=0;i<sizeof(network_errors)/sizeof(network_errors[0]);++i){
        snprintf(network.error,sizeof(network.error),"%s",network_errors[i]);
        service.pairing=i<2;
        char frame[64];snprintf(frame,sizeof(frame),"control-network-error-%u",i);save_frame(frame);
        const char *caption=service.pairing?(MEMO_UI_COMPACT?"配对中，录音可用":"正在配对，录音可用"):
            (MEMO_UI_COMPACT?"待配对，录音可用":"服务待配对，录音可用");
        lv_obj_t *notice=find_visible_label(lv_screen_active(),caption);assert(notice);
        lv_area_t area;lv_obj_get_coords(notice,&area);
        assert(area.y2 < (MEMO_UI_COMPACT?84:153)); // Primary button / first recording row.
    }
    network.error[0]='\0';service.pairing=false;
    network.provisioning = true;
    snprintf(network.ap_ssid, sizeof(network.ap_ssid), "MeowInbox-D69C");
    tick(220);
    save_frame("provisioning");
    assert(contains_visible_label(lv_screen_active(), "联网设置"));
    assert(contains_visible_label(lv_screen_active(), "热点 MeowInbox-D69C"));
    assert(!contains_visible_label(lv_screen_active(), "连上热点自动打开"));
    assert(!contains_visible_label(lv_screen_active(), "连接热点后自动打开网页"));
    check_provision_url();
    service.pairing=true;save_frame("provisioning-pairing");
    assert(contains_visible_label(lv_screen_active(),network.connected?"正在配对，请稍候":"等待连接 Wi-Fi"));
    check_provision_url();
    network.connected=true;
    snprintf(network.error,sizeof(network.error),"服务接口不兼容，请检查中继版本；更换配对码无法解决");
    save_frame("provisioning-pairing-incompatible");
    assert(contains_visible_label(lv_screen_active(),"配对未完成，请看网页"));
    assert(!contains_visible_label(lv_screen_active(),"正在配对，请稍候"));
    check_provision_url();
    network.error[0]='\0';
    service.pairing=false;service.enabled=false;save_frame("provisioning-local");
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"仅本地，有卡录放可用":"仅本地，有卡录音回放可用喵"));
    check_provision_url();
    service.result=MEMO_CONFIG_STORAGE_ERROR;save_frame("provisioning-save-failure");
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"设置未确认，网页重试":"设置未确认，请在网页重试喵"));
    check_provision_url();
    service=(memo_config_public_t){.enabled=true};
    snprintf(network.error,sizeof(network.error),"配对码无效、过期或设备已撤销，请重新配对");
    save_frame("provisioning-pair-failure");
    assert(contains_visible_label(lv_screen_active(),MEMO_UI_COMPACT?"配对失败，网页重试":"服务未配对，请在网页重试喵"));
    check_provision_url();
    network.error[0]='\0';
    network.configured = true;
    tick(220);
    save_frame("provisioning-enrolled");
#if MEMO_UI_COMPACT
    assert(contains_visible_label(lv_screen_active(), "服务已配对"));
#else
    assert(contains_visible_label(lv_screen_active(), "服务已配对"));
#endif
    check_provision_url();
    lv_area_t exit_area;
    lv_obj_get_coords(lv_obj_get_parent(find_visible_label(lv_screen_active(), "退出配网")), &exit_area);
    click(indev, (exit_area.x1 + exit_area.x2) / 2, (exit_area.y1 + exit_area.y2) / 2, CMD_PROVISION, 0);
    network.provisioning = false;
    tick(220);
    usb_voice_cases(indev);
    cardless_cases(indev);
    assert(children == lv_obj_get_child_count(lv_screen_active()));
    printf("HOST_UI: %dx%d cat default, first tap opens controls without command, 60s return PASS\n",
           MEMO_UI_WIDTH, MEMO_UI_HEIGHT);
    puts("HOST_UI: saved and stopped-playback feedback slow-blinks, touch resets its two-minute sleep timer PASS");
    puts("HOST_UI: text-free cat recording/window/saving/saved/playing/error states PASS");
    puts("HOST_UI: held-pointer start/stop/play/paging, busy/SD gating, stable objects PASS");
    puts("HOST_UI: storage retry sends REFRESH not START; busy retry and stale list disabled; recovery to START PASS");
    puts("HOST_UI: three-second cat hold opens provisioning, open AP details fit, exit command PASS");
    puts("HOST_UI: physical display/touch and listening remain NOT_RUN");
    lv_deinit();
    return 0;
}
