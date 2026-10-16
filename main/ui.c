// Local-only UI: event handlers enqueue commands; audio and storage stay in app.c.
#include "app.h"
#include "cat_anim.h"
#include "memo_icons.h"
#include "network_sync.h"
#include "power_policy.h"
#include "ui_layout.h"
#include "lvgl.h"
#if MEMO_UI_COMPACT
#include "src/core/lv_obj_event_private.h" // LVGL 9.3 exposes hit-test fields here.
#endif
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(memo_font);
LV_FONT_DECLARE(memo_font20);
LV_FONT_DECLARE(memo_font22);
LV_FONT_DECLARE(memo_font24);

#if MEMO_UI_COMPACT || (defined(CONFIG_MEMO_VOCAT16) && CONFIG_MEMO_VOCAT16)
#define MEMO_UI_BATTERY 1
#else
#define MEMO_UI_BATTERY 0
#endif

static lv_obj_t *status_label, *detail_label, *count_label, *mode_label;
#if !MEMO_UI_COMPACT
static lv_obj_t *record_icon, *stop_icon, *fish_icon;
static lv_obj_t *empty_label;
#endif
static lv_obj_t *primary, *primary_label, *level_bar;
static lv_obj_t *rows[MEMO_PAGE_SIZE], *row_labels[MEMO_PAGE_SIZE];
static lv_obj_t *previous, *next, *face, *ears[2], *eyes[2], *mouth;
static lv_obj_t *cat_root, *panel_root, *cat_level_bar, *cat_state_dot;
static lv_obj_t *usb_root, *usb_status, *usb_hint, *usb_mute, *usb_mute_label;
static lv_obj_t *usb_title_icon;
static lv_obj_t *usb_exit, *usb_back, *usb_cat_mic, *usb_cat_stem, *usb_cat_foot, *usb_cat_slash;
static bool usb_visible;
static uint32_t displayed_voice_wake_count;
#if MEMO_UI_BATTERY
static lv_obj_t *battery_button, *battery_label, *battery_outline;
static lv_obj_t *battery_fill, *battery_terminal;
#endif
static lv_obj_t *provision_root, *provision_ap_label, *provision_detail_label, *provision_back;
static lv_obj_t *cat_fallback[2];
static uint32_t row_ids[MEMO_PAGE_SIZE];
static lv_obj_t *record_root, *record_title, *record_meta, *record_status_label, *record_info, *record_back;
static lv_obj_t *record_play, *record_play_label, *record_retry, *record_retry_label;
#if !MEMO_UI_COMPACT
static lv_obj_t *record_status_dot, *record_cat_icon;
static unsigned record_play_icon_mode; // 0 cat, 1 pause, 2 resume
#endif
static bool record_visible;
static bool detail_return_after_stop;
static uint32_t record_feedback_until;
static memo_item_t selected_record;

static unsigned list_revision = UINT32_MAX;
static memo_state_t displayed_state = (memo_state_t)-1;
static bool displayed_sd_ready;
static bool displayed_cardless, displayed_cardless_ready;
static bool displayed_voice_window;
static bool displayed_recovering;
static bool panel_visible;
static bool provision_visible;
static bool cat_press_active, provision_long_triggered, suppress_cat_click;
static uint32_t cat_pressed_tick;
static bool timed_animation_active;
static uint32_t timed_animation_tick;
static uint32_t timed_animation_ms;
static cat_anim_state_t timed_animation_target;
static uint32_t panel_activity_tick;
static bool saved_idle_active;
static bool saved_idle_requested;
static uint32_t saved_idle_tick;
static memo_cmd_t primary_command = CMD_START;

#define SAVED_IDLE_MS 120000U
#define PANEL_IDLE_MS 60000U

static const uint32_t BG = 0x101D20;
static const uint32_t CAT_BG = 0x000000;
static const uint32_t INK = 0xDCEAE3;
static const uint32_t MUTED = 0xA0B6B0;
static const uint32_t MINT = 0xB5E2CC;
static const uint32_t CORAL = 0xF08472;
static const uint32_t GOLD = 0xFFD187;
static const uint32_t CLOUD = 0x97D1F5;
static const char *wake_display_name = "你好喵伴";

#if MEMO_UI_COMPACT
#define DETAIL_COPY(compact, expanded) (compact)
#else
#define DETAIL_COPY(compact, expanded) (expanded)
#endif

static void note_saved_activity(void);

static uint32_t display_record_id(uint32_t id) { return id & UINT32_C(0x7fffffff); }
static cat_anim_state_t awake_animation(void)
{
    memo_snapshot_t snapshot;app_snapshot(&snapshot);
    return snapshot.cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
}

static void text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static const char *short_notice(const char *notice)
{
    if (strstr(notice,"连接暂停"))
        return DETAIL_COPY("电脑暂停，麦克风已关喵","电脑暂停，麦克风已关喵");
    if (strstr(notice,"功能未就绪")) return "麦克风没准备好喵";
    return notice;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, const char *text,
                       uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_font(obj, &memo_font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *button(lv_obj_t *screen, int x, int y, int w, int h,
                        const char *text, lv_obj_t **out_label)
{
    lv_obj_t *obj = lv_button_create(screen);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(obj, 9, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x243A3C), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x41605C), LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *caption = label(obj, 0, 0, w - 8, text, INK);
    lv_obj_center(caption);
    if (out_label) *out_label = caption;
    return obj;
}

static void enabled(lv_obj_t *obj, bool enable)
{
    if (enable) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void publish_storage_view(void) {
    app_storage_view(!provision_visible && !usb_visible &&
        (record_visible?!selected_record.cardless:panel_visible));
}

static void show_panel(bool show)
{
    if (provision_visible && show) return;
    if (usb_visible && show) return;
    if(record_visible) {
        memo_snapshot_t snapshot;app_snapshot(&snapshot);
        if(snapshot.state==MEMO_PLAYING || snapshot.state==MEMO_PAUSED) {
            if(!app_command(CMD_STOP,0,show?"touch":"idle_timer"))return;
            detail_return_after_stop=show;
        }
        record_visible=false;
        lv_obj_add_flag(record_root,LV_OBJ_FLAG_HIDDEN);
        network_sync_select_record(0);
    }
    panel_visible = show;
    publish_storage_view();
    if (show) {
        panel_activity_tick = lv_tick_get();
        lv_obj_add_flag(cat_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(panel_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cat_root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_usb(bool show)
{
    usb_visible = show;
    if (show) {
        lv_obj_add_flag(cat_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(usb_root, LV_OBJ_FLAG_HIDDEN);
        publish_storage_view();
    } else {
        lv_obj_add_flag(usb_root, LV_OBJ_FLAG_HIDDEN);
        show_panel(false); // Publish only the final cat view, never stale panel demand.
    }
}

static void show_provision(bool show)
{
    provision_visible = show;
    if (show) {
        lv_obj_add_flag(cat_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(provision_root, LV_OBJ_FLAG_HIDDEN);
        publish_storage_view();
    } else {
        lv_obj_add_flag(provision_root, LV_OBJ_FLAG_HIDDEN);
        show_panel(false);
    }
}

static void cat_touch(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        cat_press_active = true;
        provision_long_triggered = false;
        cat_pressed_tick = lv_tick_get();
    } else if (code == LV_EVENT_PRESSING && cat_press_active && !provision_long_triggered &&
               lv_tick_elaps(cat_pressed_tick) >= 3000) {
        provision_long_triggered = true;
        suppress_cat_click = true;
        app_command(CMD_PROVISION, 0, "touch");
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        cat_press_active = false;
    } else if (code == LV_EVENT_CLICKED) {
        if (suppress_cat_click) {
            suppress_cat_click = false;
            return;
        }
        note_saved_activity();
        memo_snapshot_t snapshot;app_snapshot(&snapshot);
        if (snapshot.state == MEMO_USB_MIC) show_usb(true);
        else show_panel(true);
    }
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    note_saved_activity();
    show_panel(false);
}

static void usb_back_clicked(lv_event_t *event)
{
    (void)event;
    note_saved_activity();
    show_usb(false);
}

static void usb_mute_clicked(lv_event_t *event)
{
    (void)event;
    memo_snapshot_t snapshot;app_snapshot(&snapshot);
    if (snapshot.state == MEMO_USB_MIC)
        app_command(snapshot.usb_local_muted ? CMD_USB_UNMUTE : CMD_USB_MUTE, 0, "touch");
}

static void usb_exit_clicked(lv_event_t *event)
{
    (void)event;
    app_command(CMD_USB_CLOSE, 0, "touch");
}

static void provision_back_clicked(lv_event_t *event)
{
    (void)event;
    app_command(CMD_PROVISION, 0, "touch");
}

static void primary_clicked(lv_event_t *event)
{
    (void)event;
    note_saved_activity();
    panel_activity_tick = lv_tick_get();
    app_command(primary_command, 0, "touch");
}

static void record_clicked(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    note_saved_activity();
    panel_activity_tick = lv_tick_get();
    if (index < MEMO_PAGE_SIZE && row_ids[index]) {
        memo_snapshot_t snapshot;app_snapshot(&snapshot);
        if(index>=snapshot.item_count || snapshot.items[index].id!=row_ids[index])return;
        selected_record=snapshot.items[index];
        record_visible=true;
        publish_storage_view();
        lv_obj_add_flag(panel_root,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cat_root,LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(record_root,LV_OBJ_FLAG_HIDDEN);
        record_feedback_until=0;
        network_sync_select_record(selected_record.id);
    }
}

static void record_back_clicked(lv_event_t *event) {
    (void)event;note_saved_activity();show_panel(true);
}
#if MEMO_UI_COMPACT
/* Keep the drawn control unchanged; only its same-width area extends down. */
static void bottom_hit_test(lv_event_t *event)
{
    lv_hit_test_info_t *hit = lv_event_get_hit_test_info(event);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target(event), &area);
    area.y2 = MEMO_UI_HEIGHT - 1;
    hit->res = hit->point->x >= area.x1 && hit->point->x <= area.x2 &&
               hit->point->y >= area.y1 && hit->point->y <= area.y2;
}
#endif
static void record_play_clicked(lv_event_t *event) {
    (void)event;note_saved_activity();panel_activity_tick=lv_tick_get();
    if(selected_record.cardless)return;
    memo_snapshot_t snapshot;app_snapshot(&snapshot);
    memo_cmd_t command=snapshot.state==MEMO_PLAYING?CMD_PAUSE:
                       snapshot.state==MEMO_PAUSED?CMD_RESUME:CMD_PLAY;
    app_command(command,selected_record.id,"touch");
}
static void record_busy_feedback(void) {
    record_feedback_until=lv_tick_get()+2000;
    text_if_changed(record_status_label,DETAIL_COPY("任务繁忙，请稍后重试", "任务繁忙，请稍后重试喵"));
}
static void record_retry_clicked(lv_event_t *event) {
    (void)event;note_saved_activity();panel_activity_tick=lv_tick_get();
    memo_record_sync_t job;network_sync_record_status(&job);
    if(selected_record.cardless) {
        if(job.can_retry) {
            if(!network_sync_retry_record(selected_record.id))
                record_busy_feedback();
        } else network_sync_select_record(selected_record.id);
        return;
    }
    if(job.state==MEMO_JOB_RECEIVED || job.state==MEMO_JOB_TRANSCRIBING ||
       job.state==MEMO_JOB_DELIVERING || job.state==MEMO_JOB_WAITING ||
       job.state==MEMO_JOB_UNKNOWN || (job.state==MEMO_JOB_FAILED && !job.can_retry) || job.state==MEMO_JOB_REJECTED || job.state==MEMO_JOB_UNCERTAIN)
        network_sync_select_record(selected_record.id);
    else if(!network_sync_retry_record(selected_record.id))
        record_busy_feedback();
}
static void format_record_time(char *out,size_t length,uint32_t ms) {
    uint32_t seconds=ms/1000;
    if(seconds>=3600)
        snprintf(out,length,"%" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                 seconds/3600,(seconds/60)%60,seconds%60);
    else snprintf(out,length,"%02" PRIu32 ":%02" PRIu32,seconds/60,seconds%60);
}

#if !MEMO_UI_COMPACT
static void draw_record_play_icon(lv_event_t *event) {
    if(!record_play_icon_mode)return;
    lv_area_t area;lv_obj_get_coords(lv_event_get_target_obj(event),&area);
    uint32_t ink=0x16332D;
    if(record_play_icon_mode==1) {
        lv_draw_rect_dsc_t bar;lv_draw_rect_dsc_init(&bar);
        bar.bg_opa=LV_OPA_COVER;bar.bg_color=lv_color_hex(ink);bar.radius=2;
        lv_area_t left={area.x1+34,area.y1+23,area.x1+39,area.y1+43};
        lv_area_t right={area.x1+45,area.y1+23,area.x1+50,area.y1+43};
        lv_draw_rect(lv_event_get_layer(event),&bar,&left);
        lv_draw_rect(lv_event_get_layer(event),&bar,&right);
    } else {
        lv_draw_triangle_dsc_t triangle;lv_draw_triangle_dsc_init(&triangle);
        triangle.color=lv_color_hex(ink);
        triangle.p[0]=(lv_point_precise_t){area.x1+33,area.y1+22};
        triangle.p[1]=(lv_point_precise_t){area.x1+33,area.y1+45};
        triangle.p[2]=(lv_point_precise_t){area.x1+53,area.y1+34};
        lv_draw_triangle(lv_event_get_layer(event),&triangle);
    }
}
#endif
/* Insert real line breaks so every detail/error remains readable on both round
 * panels. A UTF-8 codepoint is never split between lines. */
static void wrap_detail(char *out,size_t capacity,const char *text,int width,const lv_font_t *font)
{
    size_t used=0,line=0;out[0]='\0';
    bool break_clause=!strchr(text,'\n') && lv_text_get_width(text,strlen(text),font,0)>width;
    for(const unsigned char *p=(const unsigned char *)text;*p;) {
        if(break_clause && !strncmp((const char *)p,"，",3)) {
            if(used+2>=capacity)break;
            out[used++]='\n';line=used;p+=3;out[used]='\0';continue;
        }
        size_t n=*p<0x80?1:(*p<0xe0?2:(*p<0xf0?3:4));
        if(used+n+2>=capacity)break;
        if(*p=='\n') {out[used++]='\n';line=used;p++;out[used]='\0';continue;}
        memcpy(out+used,p,n);out[used+n]='\0';
        if(used>line && lv_text_get_width(out+line,used+n-line,font,0)>width) {
            memmove(out+used+1,out+used,n+1);out[used++]='\n';line=used;
        }
        used+=n;p+=n;
    }
    out[used]='\0';
}

static void refresh_cardless_record(const memo_snapshot_t *snapshot,
                                    const memo_network_status_t *network,
                                    const memo_record_sync_t *job)
{
    char text[160],wrapped[180];
    const char *state=DETAIL_COPY("状态待查询","状态待查询喵");
    const char *action=job->can_retry?"重试处理":"刷新状态";
    uint32_t color=CLOUD;
    switch(job->state) {
    case MEMO_JOB_LOCAL:state=DETAIL_COPY("等待发送","等待发送喵");break;
    case MEMO_JOB_QUEUED:state=DETAIL_COPY("等待传送","等待传送喵");break;
    case MEMO_JOB_CHECKING:state=DETAIL_COPY("正在确认状态","正在确认状态喵");action="查询中";break;
    case MEMO_JOB_UPLOADING:state=DETAIL_COPY("正在传送音频","正在传送音频喵");break;
    case MEMO_JOB_WAITING:state=DETAIL_COPY("等待服务器确认","等待服务器确认喵");break;
    case MEMO_JOB_RECEIVED:state=DETAIL_COPY("服务器已收到","服务器已收到喵");break;
    case MEMO_JOB_TRANSCRIBING:state=DETAIL_COPY("正在转写","正在转写喵");break;
    case MEMO_JOB_DELIVERING:state=DETAIL_COPY("正在投递思源","正在投递思源喵");break;
    case MEMO_JOB_SENT:state=DETAIL_COPY("已同步思源","已同步思源喵");action="已同步";color=MINT;break;
    case MEMO_JOB_FAILED:state=DETAIL_COPY("处理未完成","处理未完成喵");color=CORAL;break;
    case MEMO_JOB_REJECTED:state=DETAIL_COPY("内容未通过检查","内容未通过检查喵");color=GOLD;break;
    case MEMO_JOB_UNCERTAIN:state=DETAIL_COPY("投递待核对","投递待核对喵");color=GOLD;break;
    default:break;
    }
    if(job->error[0])snprintf(text,sizeof(text),"%s",job->error);
    else if(job->interrupted)snprintf(text,sizeof(text),DETAIL_COPY("录音曾中断\n仅处理已接收的部分","录音曾中断\n仅处理已接收的部分喵"));
    else snprintf(text,sizeof(text),DETAIL_COPY("无卡录音\n本机不保留音频","无卡录音\n音频不留在本机喵"));
    if(job->state!=MEMO_JOB_SENT && (!network->configured || !network->connected)) {
        action="等待联网";
        if(!job->error[0])snprintf(text,sizeof(text),DETAIL_COPY("网络离线\n联网后可查询状态","网络离线\n联网后可查询状态喵"));
    }
    wrap_detail(wrapped,sizeof(wrapped),text,MEMO_UI_COMPACT?180:232,
                MEMO_UI_COMPACT?&memo_font:&memo_font20);
    text_if_changed(record_info,wrapped);
    if((int32_t)(record_feedback_until-lv_tick_get())>0)state=DETAIL_COPY("任务繁忙，请稍后重试","任务繁忙，请稍后重试喵");
    text_if_changed(record_status_label,state);
    text_if_changed(record_retry_label,action);
    lv_obj_set_style_text_color(record_status_label,lv_color_hex(color),0);
    bool idle=snapshot->state==MEMO_IDLE || snapshot->state==MEMO_SAVED || snapshot->state==MEMO_ERROR;
    enabled(record_retry,idle && network->configured && network->connected && !job->request_pending &&
            job->state!=MEMO_JOB_CHECKING && job->state!=MEMO_JOB_SENT);
#if !MEMO_UI_COMPACT
    lv_obj_set_style_bg_color(record_status_dot,lv_color_hex(color),0);
    int text_width=lv_text_get_width(state,strlen(state),&memo_font20,0);
    int left=(MEMO_UI_WIDTH-text_width-17)/2;
    lv_obj_set_pos(record_status_dot,left,127);
    lv_obj_set_pos(record_status_label,left+14,114);
    lv_obj_set_width(record_status_label,text_width+2);
#endif
}

static void refresh_record(const memo_snapshot_t *snapshot,const memo_network_status_t *network) {
    if(!record_visible)return;
    for(size_t i=0;i<snapshot->item_count;i++) {
        if(snapshot->items[i].id==selected_record.id) {selected_record=snapshot->items[i];break;}
    }
    char text[128];
    snprintf(text,sizeof(text),selected_record.cardless?"无卡 #%" PRIu32:"录音 #%" PRIu32,
             display_record_id(selected_record.id));text_if_changed(record_title,text);
    if(selected_record.cardless) {
        lv_obj_add_flag(record_play,LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(record_info,LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(record_play,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(record_info,LV_OBJ_FLAG_HIDDEN);
    }
#if MEMO_UI_COMPACT
    lv_obj_set_pos(record_retry,selected_record.cardless?65:123,selected_record.cardless?178:122);
    lv_obj_set_size(record_retry,selected_record.cardless?110:90,selected_record.cardless?28:42);
    lv_obj_set_width(record_retry_label,selected_record.cardless?102:82);
    lv_obj_center(record_retry_label);
    lv_obj_set_pos(record_back,selected_record.cardless?0:70,selected_record.cardless?210:175);
    lv_obj_set_size(record_back,selected_record.cardless?240:100,selected_record.cardless?30:38);
    lv_obj_set_style_bg_opa(record_back,selected_record.cardless?LV_OPA_TRANSP:LV_OPA_COVER,0);
    lv_obj_set_width(lv_obj_get_child(record_back,0),selected_record.cardless?60:92);
    lv_obj_center(lv_obj_get_child(record_back,0));
#endif
    char total[20],current[20];format_record_time(total,sizeof(total),selected_record.duration_ms);
    bool active=(snapshot->state==MEMO_PLAYING || snapshot->state==MEMO_PAUSED) &&
                snapshot->active_id==selected_record.id;
#if MEMO_UI_COMPACT
    if(active) {
        format_record_time(current,sizeof(current),snapshot->elapsed_ms);
        snprintf(text,sizeof(text),"%s/%s",current,total);
    } else snprintf(text,sizeof(text),"%s  %.1f %s",total,
        (double)selected_record.file_bytes/(selected_record.file_bytes>=1048576?1048576:1024),
        selected_record.file_bytes>=1048576?"MB":"KB");
#else
    double bytes=selected_record.file_bytes;
    const char *unit=bytes>=1000000000.0?"GB":bytes>=1000000.0?"MB":"KB";
    double divisor=bytes>=1000000000.0?1000000000.0:bytes>=1000000.0?1000000.0:1000.0;
    if(active) {
        format_record_time(current,sizeof(current),snapshot->elapsed_ms);
        snprintf(text,sizeof(text),"%s/%s  ·  %.1f %s",current,total,bytes/divisor,unit);
        if(lv_text_get_width(text,strlen(text),&memo_font,0)>244)
            snprintf(text,sizeof(text),"%s/%s · %.1f%s",current,total,bytes/divisor,unit);
    } else snprintf(text,sizeof(text),"%s  ·  %.1f %s",total,bytes/divisor,unit);
#endif
    text_if_changed(record_meta,text);
    memo_record_sync_t job;network_sync_record_status(&job);
    if(selected_record.cardless) {refresh_cardless_record(snapshot,network,&job);return;}
    const char *state=DETAIL_COPY("状态待查询","状态待查询喵"), *action="刷新状态";
    uint32_t status_color=GOLD;
    switch(job.state) {
    case MEMO_JOB_LOCAL:state=DETAIL_COPY("仅本地保存","已在本机，尚未同步喵");action="开始同步";break;
    case MEMO_JOB_QUEUED:state=DETAIL_COPY("等待上传","等待上传喵");action="立即重试";break;
    case MEMO_JOB_CHECKING:state=DETAIL_COPY("正在查询状态","正在确认状态喵");action=DETAIL_COPY("刷新状态","查询中");break;
    case MEMO_JOB_UPLOADING:
        snprintf(text,sizeof(text),DETAIL_COPY("正在上传 %u%%","正在上传 %u%%喵"),job.upload_percent);
        state=text;action="上传中";break;
    case MEMO_JOB_WAITING:state=DETAIL_COPY("等待服务器处理","已上传，等处理喵");break;
    case MEMO_JOB_RECEIVED:state=DETAIL_COPY("服务器已接收","服务器已收到喵");break;
    case MEMO_JOB_TRANSCRIBING:state=DETAIL_COPY("正在转写","正在转写喵");break;
    case MEMO_JOB_DELIVERING:state=DETAIL_COPY("正在投递思源","正在投递思源喵");break;
    case MEMO_JOB_SENT:state=DETAIL_COPY("已同步思源","已同步思源喵");action="已同步";status_color=MINT;break;
    case MEMO_JOB_FAILED:state=DETAIL_COPY("处理失败","同步失败，录音仍在本机喵");action=job.can_retry?DETAIL_COPY("重试","重试同步"):"刷新状态";status_color=CORAL;break;
    case MEMO_JOB_REJECTED:state=DETAIL_COPY("内容未通过检查","内容未通过检查喵");break;
    case MEMO_JOB_UNCERTAIN:state=DETAIL_COPY("投递待核对","投递待核对喵");break;
    default:break;
    }
    if(job.state!=MEMO_JOB_SENT && !network->configured) {
        state=DETAIL_COPY("仅本地，未配置同步","未配置同步，录音在本机喵");
        action=DETAIL_COPY("刷新状态","联网后同步");
    } else if(job.state!=MEMO_JOB_SENT && !network->connected) {
        state=DETAIL_COPY("网络离线，等待连接","网络离线，录音在本机喵");
        action=DETAIL_COPY("刷新状态","等待联网");
    } else if(job.request_pending && (job.state==MEMO_JOB_QUEUED || job.state==MEMO_JOB_UNKNOWN)) {
        state=DETAIL_COPY("已排队，等待处理","已排队，等待处理喵");action="排队中";
    }
    if(snapshot->state==MEMO_PAUSED && snapshot->error[0])state=snapshot->error;
    if((int32_t)(record_feedback_until-lv_tick_get())>0)state=DETAIL_COPY("任务繁忙，请稍后重试","任务繁忙，请稍后重试喵");
    text_if_changed(record_status_label,state);
    text_if_changed(record_play_label,snapshot->state==MEMO_PLAYING?"暂停播放":
                    snapshot->state==MEMO_PAUSED?"继续播放":"播放录音");
    text_if_changed(record_retry_label,action);
    bool idle=snapshot->state==MEMO_IDLE || snapshot->state==MEMO_SAVED;
    enabled(record_play,snapshot->sd_ready && (idle || active));
    enabled(record_retry,idle && network->configured && network->connected && !job.request_pending &&
        job.state!=MEMO_JOB_CHECKING && job.state!=MEMO_JOB_UPLOADING && job.state!=MEMO_JOB_SENT);
#if !MEMO_UI_COMPACT
    lv_obj_set_style_bg_color(record_play,lv_color_hex(snapshot->state==MEMO_PLAYING?0xF4B7A8:MINT),0);
    unsigned icon_mode=snapshot->state==MEMO_PLAYING?1:snapshot->state==MEMO_PAUSED?2:0;
    if(record_play_icon_mode!=icon_mode) {
        record_play_icon_mode=icon_mode;
        lv_obj_invalidate(record_play);
    }
    if(icon_mode)lv_obj_add_flag(record_cat_icon,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(record_cat_icon,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(record_status_label,lv_color_hex(status_color),0);
    lv_obj_set_style_bg_color(record_status_dot,lv_color_hex(status_color),0);
    int text_width=lv_text_get_width(state,strlen(state),&memo_font20,0);
    int left=(MEMO_UI_WIDTH-text_width-17)/2;
    lv_obj_set_pos(record_status_dot,left,127);
    lv_obj_set_pos(record_status_label,left+14,114);
    lv_obj_set_width(record_status_label,text_width+2);
#endif
}

static void page_clicked(lv_event_t *event)
{
    memo_cmd_t command = (memo_cmd_t)(uintptr_t)lv_event_get_user_data(event);
    note_saved_activity();
    panel_activity_tick = lv_tick_get();
    app_command(command, 0, "touch");
}

// Draw a solid triangle; the entire tall side button remains the touch target.
static void draw_page_arrow(lv_event_t *event)
{
#if MEMO_UI_COMPACT
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    int direction = (intptr_t)lv_event_get_user_data(event);
    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
#if MEMO_UI_COMPACT
    triangle.color = lv_color_hex(lv_obj_has_state(obj, LV_STATE_DISABLED) ? 0x516763 : INK);
#else
    triangle.color = lv_color_hex(INK);
#endif
    triangle.p[0] = (lv_point_precise_t){x + direction * 10, y};
    triangle.p[1] = (lv_point_precise_t){x - direction * 8, y - 13};
    triangle.p[2] = (lv_point_precise_t){x - direction * 8, y + 13};
    lv_draw_triangle(lv_event_get_layer(event), &triangle);
#else
    (void)event;
#endif
}

static void set_cat_fallback(cat_anim_state_t state)
{
#if MEMO_UI_COMPACT
    int y = 96, height = 33;
    if (state == CAT_ANIM_IDLE) { y = 111; height = 6; }
    else if (state == CAT_ANIM_SAVING) { y = 109; height = 8; }
    else if (state == CAT_ANIM_SAVED) { y = 100; height = 25; }
    else if (state == CAT_ANIM_ERROR) { y = 110; height = 7; }
#else
    int y = 130, height = 64;
    if (state == CAT_ANIM_IDLE) { y = 157; height = 10; }
    else if (state == CAT_ANIM_SAVING) { y = 155; height = 14; }
    else if (state == CAT_ANIM_SAVED) { y = 138; height = 48; }
    else if (state == CAT_ANIM_ERROR) { y = 156; height = 12; }
#endif
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_y(cat_fallback[i], y);
        lv_obj_set_height(cat_fallback[i], height);
    }
}

static void note_saved_activity(void)
{
    power_policy_note_activity("touch");
    if (!saved_idle_active) return;
    saved_idle_active = true;
    saved_idle_requested = false;
    saved_idle_tick = lv_tick_get();
    timed_animation_active = false;
    cat_anim_set(awake_animation());
    set_cat_fallback(awake_animation());
}

void ui_power_show_sleep(void)
{
    show_panel(false);
    saved_idle_active = false;
    saved_idle_requested = true;
    timed_animation_active = false;
    cat_anim_set(CAT_ANIM_IDLE);
    set_cat_fallback(CAT_ANIM_IDLE);
}

void ui_power_show_awake(void)
{
    show_panel(false);
    saved_idle_active = true;
    saved_idle_requested = false;
    saved_idle_tick = lv_tick_get();
    timed_animation_active = false;
    cat_anim_set(awake_animation());
    set_cat_fallback(awake_animation());
}

static void expression(const memo_snapshot_t *s)
{
    bool voice_woke = displayed_voice_wake_count != s->voice_wake_count;
    bool previous_voice_window = displayed_voice_window;
    bool state_changed = displayed_state != s->state || displayed_recovering != s->recovering;
    bool voice_changed = displayed_voice_window != s->voice_window || voice_woke;
    displayed_voice_wake_count = s->voice_wake_count;
    if (displayed_state == s->state && displayed_sd_ready == s->sd_ready &&
        displayed_cardless == s->cardless && displayed_cardless_ready == s->cardless_ready &&
        displayed_voice_window == s->voice_window &&
        displayed_recovering == s->recovering && !voice_woke) return;
    memo_state_t previous_state = displayed_state;
    displayed_state = s->state;
    displayed_sd_ready = s->sd_ready;
    displayed_cardless = s->cardless;
    displayed_cardless_ready = s->cardless_ready;
    displayed_voice_window = s->voice_window;
    displayed_recovering = s->recovering;
    const char *status = "待机";
    const char *action = "开始录音";
    const char *mouth_text = "w";
    uint32_t color = s->cardless?CLOUD:MINT;
    int eye_height = 5;
    cat_anim_state_t cat_state = CAT_ANIM_IDLE;
    bool post_playback_awake = s->state == MEMO_IDLE &&
                               (previous_state == MEMO_PLAYING || previous_state == MEMO_PAUSED);
    bool post_usb_awake=s->state==MEMO_IDLE && previous_state==MEMO_USB_MIC;
    bool voice_awake = (s->state == MEMO_IDLE || s->state == MEMO_SAVED || s->state == MEMO_ERROR) &&
                       (voice_woke || s->voice_window || previous_voice_window);
    timed_animation_active = false;
    saved_idle_active = s->state == MEMO_SAVED || post_playback_awake || post_usb_awake || voice_awake;
    saved_idle_requested = false;
    if (saved_idle_active) saved_idle_tick = lv_tick_get();
    primary_command = CMD_START;
    switch (s->state) {
    case MEMO_RECORDING:
        status = s->cardless?"录音中":"正在录音"; action = "停止录音"; color = s->cardless?CLOUD:CORAL;
        mouth_text = "o"; eye_height = 8; primary_command = CMD_STOP;
        cat_state = s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
        if (previous_state == MEMO_IDLE) {
            cat_state = CAT_ANIM_SAVED;
            timed_animation_active = true;
            timed_animation_tick = lv_tick_get();
            timed_animation_ms = 1500;
            timed_animation_target = s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
        }
        break;
    case MEMO_SAVING:
        status = s->cardless?"传送中":(s->recovering ? "恢复录音" : "正在保存");
        action = "请稍候"; color = GOLD;
        mouth_text = "-"; eye_height = 2;
        cat_state = CAT_ANIM_SAVING;
        break;
    case MEMO_SAVED:
        status = s->cardless?"已结束":"已保存"; action = "再录一条"; mouth_text = "v";
        eye_height = 3;
        cat_state = CAT_ANIM_SAVED;
        timed_animation_active = true;
        timed_animation_tick = lv_tick_get();
        timed_animation_ms = 2500;
        timed_animation_target = s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
        break;
    case MEMO_PLAYING:
        status = "正在播放"; action = "停止播放";
        mouth_text = "o"; eye_height = 7; primary_command = CMD_STOP;
        cat_state = CAT_ANIM_PLAYING;
        break;
    case MEMO_PAUSED:
        status = "已暂停"; action = "继续播放";
        mouth_text = "o"; eye_height = 7; primary_command = CMD_RESUME;
        cat_state = CAT_ANIM_PLAYING;
        break;
    case MEMO_ERROR:
        status = s->cardless?"出错了":"发生错误"; color = GOLD; mouth_text = "!";
        cat_state = CAT_ANIM_ERROR;
        break;
    case MEMO_USB_MIC:
        status = "电脑麦克风"; action = ""; color = GOLD;
        cat_state = CAT_ANIM_RECORDING;
        if(previous_state!=MEMO_USB_MIC) {
            cat_state=CAT_ANIM_SAVED;timed_animation_active=true;
            timed_animation_tick=lv_tick_get();timed_animation_ms=800;
            timed_animation_target=CAT_ANIM_RECORDING;
        }
        break;
    case MEMO_IDLE:
    default:
        if (post_playback_awake || post_usb_awake || voice_awake)
            cat_state = s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
        break;
    }
    if(post_usb_awake) {
        cat_state=CAT_ANIM_SAVED;timed_animation_active=true;
        timed_animation_tick=lv_tick_get();timed_animation_ms=800;
        timed_animation_target=s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
    }
    if (voice_awake && !voice_woke && !post_usb_awake) {
        cat_state=s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING;
        timed_animation_active=false;
    }
    if (voice_woke && s->voice_window &&
        (s->state == MEMO_IDLE || s->state == MEMO_SAVED || s->state == MEMO_ERROR ||
         s->state == MEMO_USB_MIC)) {
        cat_state = CAT_ANIM_SAVED;
        timed_animation_active = true;
        timed_animation_tick = lv_tick_get();
        timed_animation_ms = 650;
        timed_animation_target = s->state==MEMO_USB_MIC?CAT_ANIM_RECORDING:
                                 (s->cardless?CAT_ANIM_CARDLESS:CAT_ANIM_RECORDING);
    }
    if (!s->sd_ready && !s->cardless && s->state != MEMO_USB_MIC) {
        action = "重试存储卡";
        primary_command = CMD_REFRESH;
    }
    text_if_changed(status_label, status);
    text_if_changed(primary_label, action);
    if(s->cardless)lv_obj_remove_flag(mode_label,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(mode_label,LV_OBJ_FLAG_HIDDEN);
#if MEMO_UI_COMPACT
    lv_obj_set_pos(status_label,s->cardless?139:103,s->cardless?29:25);
    lv_obj_set_width(status_label,s->cardless?57:82);
#else
    lv_obj_set_x(status_label,s->cardless?125:110);
    lv_obj_set_width(status_label,s->cardless?80:90);
#endif
#if !MEMO_UI_COMPACT
    if (primary_command == CMD_START && (s->sd_ready || s->cardless)) {
        lv_obj_remove_flag(record_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(stop_icon, LV_OBJ_FLAG_HIDDEN);
    } else if (primary_command == CMD_STOP) {
        lv_obj_add_flag(record_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(stop_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(record_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(stop_icon, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    text_if_changed(mouth, mouth_text);
    lv_obj_set_style_text_color(status_label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(color), 0);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_style_bg_color(ears[i], lv_color_hex(color), 0);
        lv_obj_set_height(eyes[i], eye_height);
    }
    lv_obj_set_style_bg_color(primary, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(primary, lv_color_hex(0xE7EEE8), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(primary_label, lv_color_hex(BG), 0);
    cat_anim_set(cat_state);
    set_cat_fallback(cat_state);
    if (s->state == MEMO_RECORDING) {
        lv_obj_remove_flag(cat_state_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cat_level_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(cat_state_dot,
            lv_color_hex(s->voice_window ? GOLD : (s->cardless?CLOUD:CORAL)), 0);
    } else {
        lv_obj_add_flag(cat_state_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cat_level_bar, LV_OBJ_FLAG_HIDDEN);
    }
    // Media/readiness refreshes update these controls in place. Demand probing
    // may finish just after the first tap; it is not a navigation event.
    bool keep_list=detail_return_after_stop && s->state==MEMO_IDLE;
    if(detail_return_after_stop && s->state!=MEMO_PLAYING && s->state!=MEMO_PAUSED)
        detail_return_after_stop=false;
    if (s->state == MEMO_USB_MIC) {
        if (record_visible) {
            record_visible=false;
            lv_obj_add_flag(record_root,LV_OBJ_FLAG_HIDDEN);
            network_sync_select_record(0);
        }
        if (panel_visible) show_panel(false);
        if (!usb_visible && !provision_visible)
            lv_obj_remove_flag(cat_root,LV_OBJ_FLAG_HIDDEN);
    } else if (previous_state == MEMO_USB_MIC) {
        show_usb(false);
    }
    if((state_changed || voice_changed) && s->state != MEMO_USB_MIC && !keep_list && (!record_visible || s->recovering ||
                      s->state==MEMO_RECORDING || s->state==MEMO_ERROR))
        show_panel(s->recovering);
    // Voice interaction stays on the expressive view. Only a touch opens
    // the controls; waking alone must never force the list/menu on screen.
}

static void detail_text(const memo_snapshot_t *s, const memo_network_status_t *network,
                        char *text, size_t size)
{
    if (s->notice[0]) {
        wrap_detail(text,size,short_notice(s->notice),MEMO_UI_COMPACT?180:284,
                    MEMO_UI_COMPACT?&memo_font:&memo_font20);
        return;
    }
    if (s->voice_window && (s->state == MEMO_IDLE || s->state == MEMO_SAVED || s->state == MEMO_ERROR)) {
        snprintf(text,size,"%s",s->usb_available?
                 DETAIL_COPY("说开始录音\n或打开麦克风","请说开始录音或打开麦克风"):
                 "请说开始录音喵");
        return;
    }
    if(s->cardless && s->state!=MEMO_RECORDING) {
        if(s->error[0]) {
            snprintf(text,size,"%s",s->error);
            if(lv_text_get_width(text,strlen(text),MEMO_UI_COMPACT?&memo_font:&memo_font20,0)>
               (MEMO_UI_COMPACT?180:284))snprintf(text,size,"操作未完成，看详情");
        }
        else if(s->state==MEMO_SAVING)snprintf(text,size,DETAIL_COPY("正在传送，请稍候","正在传送，稍等喵"));
        else if(!network->configured)snprintf(text,size,DETAIL_COPY("长按猫脸设置网络","长按猫脸三秒，设置网络喵"));
        else if(!network->connected)snprintf(text,size,DETAIL_COPY("联网后才能录音","请先联网再录音喵"));
        else if(!s->cardless_ready)snprintf(text,size,DETAIL_COPY("等待传送完成","等这一条传完再录喵"));
        else if(s->state==MEMO_SAVED)snprintf(text,size,DETAIL_COPY("录音已结束，可看详情","录音结束，点开记录看状态喵"));
        else snprintf(text,size,DETAIL_COPY("联网录音，不留音频","联网录音，音频不留在本机喵"));
        return;
    }
    if ((!s->sd_ready && !s->cardless) || s->state == MEMO_ERROR) {
        if (strstr(s->error, "目录")) snprintf(text, size, "录音目录读取失败");
        else if (strstr(s->error, "写入")) snprintf(text, size, "存储卡写入失败");
        else if (strstr(s->error, "挂载") || strstr(s->error, "无卡") ||
                 strstr(s->error, "没有存储卡") || (!s->sd_ready && !s->error[0]))
            snprintf(text, size, "请插卡后点重试");
        else if (strstr(s->error, "空间不足")) snprintf(text, size, "空间不足，未保存");
        else if (strstr(s->error, "未保存")) snprintf(text, size, "录音失败，未保存");
        else if (strstr(s->error, "音频") || strstr(s->error, "麦克风"))
            snprintf(text, size, "音频异常，请重启");
        else if (strstr(s->error, "保存失败")) snprintf(text, size, "保存失败，请检查卡");
        else if (!s->sd_ready) snprintf(text, size, "存储卡未就绪，请重试");
        else snprintf(text, size, "操作失败，请重试");
    } else if (s->state == MEMO_RECORDING && s->voice_window) {
        uint32_t seconds = s->elapsed_ms / 1000;
#if MEMO_UI_COMPACT
        snprintf(text, size, "%02" PRIu32 ":%02" PRIu32 "  可说保存录音",
                 seconds / 60, seconds % 60);
#else
        snprintf(text, size, "请说保存录音 %02" PRIu32 ":%02" PRIu32, seconds / 60, seconds % 60);
#endif
    } else if (s->state == MEMO_RECORDING) {
        uint32_t seconds = s->elapsed_ms / 1000;
#if MEMO_UI_COMPACT
        snprintf(text, size, "%02" PRIu32 ":%02" PRIu32 "  电平%u%%",
                 seconds / 60, seconds % 60, s->level > 100 ? 100 : s->level);
#else
        snprintf(text, size, "%02" PRIu32 ":%02" PRIu32 "   电平 %u%%",
                 seconds / 60, seconds % 60, s->level > 100 ? 100 : s->level);
#endif
    } else if (s->state == MEMO_PLAYING || s->state == MEMO_PAUSED) {
        uint32_t seconds = s->elapsed_ms / 1000;
#if MEMO_UI_COMPACT
        snprintf(text, size, "%02" PRIu32 ":%02" PRIu32 "%s", seconds / 60, seconds % 60,
                 s->state==MEMO_PAUSED?" 已暂停":"");
#else
        snprintf(text, size, "%02" PRIu32 ":%02" PRIu32 "   %s",
                 seconds / 60, seconds % 60,s->state==MEMO_PAUSED?"已暂停":"正在播放");
#endif
    } else if (s->state == MEMO_SAVING) {
#if MEMO_UI_COMPACT
        text[0] = '\0';
#else
        snprintf(text, size, "正在保存，稍等喵");
#endif
    } else if (network->syncing) {
        snprintf(text, size, "正在送往思源喵");
    } else if (network->error[0]) {
        // Network diagnostics belong on the web page, not over the recording cards.
        memo_config_public_t service; network_sync_get_service_config(&service);
        const char *notice = service.pairing ? DETAIL_COPY("配对中，录音可用", "正在配对，录音可用") :
            !network->configured ? DETAIL_COPY("待配对，录音可用", "服务待配对，录音可用") :
            DETAIL_COPY("同步未完成，可录音", "同步未完成，录音可用");
        snprintf(text, size, "%s", notice);
    } else if (!network->configured) {
#if MEMO_UI_COMPACT
        snprintf(text, size, "返回后长按表情配网");
#else
        snprintf(text, size, "长按猫脸三秒，设置网络喵");
#endif
    } else if (!network->connected) {
        snprintf(text, size, "网络离线，稍后自动同步喵");
    } else if (s->state == MEMO_SAVED) {
#if MEMO_UI_COMPACT
        text[0] = '\0';
#else
        snprintf(text, size, "%s", s->voice_ready ? "存好啦，再记一条喵？" : "点按查看最新记录");
#endif
    } else {
#if MEMO_UI_COMPACT
        if (s->voice_ready) snprintf(text,size,"唤醒后选指令喵");
        else text[0] = '\0';
#else
        if (s->voice_ready) snprintf(text, size, "说%s，选择指令喵", wake_display_name);
        else snprintf(text, size, "点按开始录音喵");
#endif
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    memo_snapshot_t s;
    app_snapshot(&s);
    memo_network_status_t network;
    network_sync_get_status(&network);
    memo_power_status_t power;
    power_policy_get_status(&power);
    expression(&s);
    refresh_record(&s,&network);
    bool usb_mode=s.state==MEMO_USB_MIC;
    lv_obj_t *mic_parts[]={usb_cat_mic,usb_cat_stem,usb_cat_foot};
    uint32_t mic_color=s.usb_muted ? CORAL :
                       (s.usb_connected && s.usb_streaming && !s.usb_suspended ? GOLD : MUTED);
    for (unsigned i=0;i<sizeof(mic_parts)/sizeof(mic_parts[0]);i++) {
        lv_obj_set_style_bg_color(mic_parts[i],lv_color_hex(mic_color),0);
        if (usb_mode) lv_obj_remove_flag(mic_parts[i],LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(mic_parts[i],LV_OBJ_FLAG_HIDDEN);
    }
    if (usb_mode && s.usb_muted) lv_obj_remove_flag(usb_cat_slash,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(usb_cat_slash,LV_OBJ_FLAG_HIDDEN);
    if (usb_mode) {
        const char *status, *hint;
        if (s.usb_suspended) {
            status="电脑暂时休息喵";hint="回到电脑继续使用喵";
        } else if (!s.usb_connected) {
            status="等待电脑喵";hint=DETAIL_COPY("电脑选喵伴麦克风","请在电脑选择喵伴麦克风");
        } else if (s.usb_muted && !s.usb_local_muted) {
            status="电脑静音中喵";hint="请在电脑取消静音";
        } else if (s.usb_muted) {
            status="麦克风静音喵";
            hint="轻点下方就能说话喵";
        } else if (s.usb_streaming) {
            status="电脑正听着喵";hint="声音正送往电脑";
        } else {
            status="已连上电脑喵";hint=DETAIL_COPY("电脑选喵伴麦克风","请在电脑选择喵伴麦克风");
        }
        const char *usb_notice=s.notice[0]?short_notice(s.notice):status;
#if MEMO_UI_COMPACT
        if (s.notice[0] && strstr(s.notice,"连接暂停")) usb_notice="电脑暂停喵";
#endif
        text_if_changed(usb_status,usb_notice);
        text_if_changed(usb_hint,hint);
        text_if_changed(usb_mute_label,s.usb_local_muted?"取消静音":"静音麦克风");
        lv_obj_set_style_bg_color(usb_mute,lv_color_hex(s.usb_local_muted?CORAL:MINT),0);
        enabled(usb_mute,!s.usb_muted || s.usb_local_muted);
    } else if (s.notice[0] && !provision_visible && !record_visible && !panel_visible) {
        show_panel(true);
    }
#if MEMO_UI_BATTERY
    char battery_text[12];
    if(power.battery_valid) {
#if CONFIG_MEMO_VOCAT16
        unsigned rounded=power.battery_percent;
#else
        unsigned rounded=((power.battery_percent+5U)/10U)*10U;
#endif
        if(rounded>100U)rounded=100U;
        snprintf(battery_text,sizeof(battery_text),"%u",rounded);
    } else {
        snprintf(battery_text,sizeof(battery_text),"--");
    }
    text_if_changed(battery_label,battery_text);
    uint32_t battery_color=power.battery_low?CORAL:(power.external_power?GOLD:MINT);
    lv_obj_set_style_text_color(battery_label,lv_color_hex(battery_color),0);
#if !MEMO_UI_COMPACT
    lv_obj_set_style_image_recolor(fish_icon,lv_color_hex(battery_color),0);
    lv_obj_set_style_image_recolor_opa(fish_icon, power.battery_low ? LV_OPA_60 : LV_OPA_TRANSP,0);
#else
    lv_obj_set_style_border_color(battery_outline,lv_color_hex(battery_color),0);
    lv_obj_set_style_bg_color(battery_terminal,lv_color_hex(battery_color),0);
    lv_obj_set_style_bg_color(battery_fill,lv_color_hex(battery_color),0);
#if CONFIG_MEMO_VOCAT16
    int fill_width=power.battery_valid?(int)((9U*power.battery_percent+99U)/100U):0;
#else
    int fill_width=power.battery_valid?(int)((7U*power.battery_percent+99U)/100U):0;
#endif
    if(fill_width<1)lv_obj_add_flag(battery_fill,LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_set_width(battery_fill,fill_width);
        lv_obj_remove_flag(battery_fill,LV_OBJ_FLAG_HIDDEN);
    }
#endif
#endif
    bool busy = s.state == MEMO_RECORDING || s.state == MEMO_PLAYING ||
                s.state == MEMO_PAUSED || s.state == MEMO_SAVING || usb_mode;
    enabled(primary,!usb_mode && (s.cardless?(s.state==MEMO_RECORDING || (!busy && s.cardless_ready)):
            (s.state != MEMO_SAVING && (s.sd_ready || !busy))));
    bool list_ready=s.sd_ready || s.cardless;
    enabled(previous, !busy && list_ready && s.page > 0);
    enabled(next, !busy && list_ready && s.page + 1 < s.page_count);
    char text[96];
    detail_text(&s, &network, text, sizeof(text));
#if !MEMO_UI_COMPACT
    if (power.battery_low && !power.external_power && s.sd_ready
        && (s.state == MEMO_IDLE || s.state == MEMO_SAVED)
        && !network.syncing && network.connected && !network.error[0])
        snprintf(text,sizeof(text),"电量低啦，给我充充电喵");
#endif
    text_if_changed(detail_label, text);
    // Bound the network status to its single reserved line; existing explicit
    // voice/notice layouts keep their established two-line region.
    lv_obj_set_height(detail_label, network.error[0] && !s.notice[0] && !s.voice_window ?
        lv_font_get_line_height(MEMO_UI_COMPACT ? &memo_font : &memo_font20) : LV_SIZE_CONTENT);
#if MEMO_UI_COMPACT
    lv_obj_set_y(detail_label,(s.notice[0] || (s.voice_window && s.state!=MEMO_RECORDING))?43:53);
#endif
    bool show_level = s.state == MEMO_RECORDING || s.state == MEMO_PLAYING;
    lv_bar_set_value(level_bar, show_level ? (int)(s.level > 100 ? 100 : s.level) : 0,
                     LV_ANIM_OFF);
#if MEMO_UI_COMPACT
    if (show_level) lv_obj_remove_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_bar_set_value(cat_level_bar, s.state == MEMO_RECORDING ||
                     (usb_mode && s.usb_streaming && !s.usb_muted) ?
                     (int)(s.level > 100 ? 100 : s.level) : 0, LV_ANIM_OFF);
    if (usb_mode && s.usb_streaming && !s.usb_muted)
        lv_obj_remove_flag(cat_level_bar,LV_OBJ_FLAG_HIDDEN);
    else if (usb_mode)
        lv_obj_add_flag(cat_level_bar,LV_OBJ_FLAG_HIDDEN);

    // Never destroy a pressed button during polling; only replace list text on a revision.
    if (list_revision != s.revision) {
        list_revision = s.revision;
#if MEMO_UI_COMPACT
        snprintf(text, sizeof(text), "记录 %zu 条  %zu/%zu", s.count,
                 s.page_count ? s.page + 1 : 0, s.page_count);
        text_if_changed(count_label, text);
#else
        snprintf(text, sizeof(text), "%zu条  %zu/%zu", s.count,
                 s.page_count ? s.page + 1 : 0, s.page_count);
        text_if_changed(count_label, text);
        if (s.item_count == 0 && list_ready) lv_obj_remove_flag(empty_label,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(empty_label,LV_OBJ_FLAG_HIDDEN);
#endif
        for (size_t i = 0; i < MEMO_PAGE_SIZE; ++i) {
            row_ids[i] = i < s.item_count ? s.items[i].id : 0;
            if (i < s.item_count) {
                uint32_t seconds = s.items[i].duration_ms / 1000;
#if MEMO_UI_COMPACT
                if(s.items[i].cardless)snprintf(text,sizeof(text),"无卡%" PRIu32,display_record_id(s.items[i].id));
                else snprintf(text, sizeof(text), "#%" PRIu32 "  %02" PRIu32 ":%02" PRIu32,
                         s.items[i].id, seconds / 60, seconds % 60);
#else
                if (seconds >= 3600) snprintf(text,sizeof(text),"%s%" PRIu32 "\n%" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                    s.items[i].cardless?"无卡":"#",display_record_id(s.items[i].id),seconds/3600,(seconds/60)%60,seconds%60);
                else snprintf(text,sizeof(text),"%s%" PRIu32 "\n%02" PRIu32 ":%02" PRIu32,
                    s.items[i].cardless?"无卡":"#",display_record_id(s.items[i].id),seconds/60,seconds%60);
#endif
                text_if_changed(row_labels[i], text);
                lv_obj_set_style_text_color(row_labels[i],lv_color_hex(s.items[i].cardless?CLOUD:INK),0);
                lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            } else if (i == 0 && MEMO_UI_COMPACT) {
                text_if_changed(row_labels[i], "还没有记录");
                lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    for (size_t i = 0; i < MEMO_PAGE_SIZE; ++i) {
        enabled(rows[i], !busy && row_ids[i] != 0 && (s.items[i].cardless || s.sd_ready));
    }
    if (panel_visible && !record_visible && lv_tick_elaps(panel_activity_tick) >= PANEL_IDLE_MS) {
        show_panel(false);
    }
    if (timed_animation_active && lv_tick_elaps(timed_animation_tick) >= timed_animation_ms) {
        timed_animation_active = false;
        cat_anim_set(timed_animation_target);
        set_cat_fallback(timed_animation_target);
    }
    if (saved_idle_active && !saved_idle_requested &&
        lv_tick_elaps(saved_idle_tick) >= SAVED_IDLE_MS) {
        if (displayed_state == MEMO_SAVED) {
            if (app_command(CMD_IDLE, 0, "idle_timer")) saved_idle_requested = true;
        } else if (displayed_state == MEMO_IDLE || displayed_state == MEMO_ERROR) {
            saved_idle_active = false;
            saved_idle_requested = true;
            cat_anim_set(CAT_ANIM_IDLE);
            set_cat_fallback(CAT_ANIM_IDLE);
        }
    }
    if (network.provisioning) {
        show_provision(true);
        snprintf(text, sizeof(text), "热点 %s", network.ap_ssid[0] ? network.ap_ssid : "正在启动");
        text_if_changed(provision_ap_label, text);
        memo_config_public_t service;network_sync_get_service_config(&service);
        if(service.result!=MEMO_CONFIG_OK)snprintf(text,sizeof(text),DETAIL_COPY("设置未确认，网页重试","设置未确认，请在网页重试喵"));
        else if(!service.enabled)snprintf(text,sizeof(text),DETAIL_COPY("仅本地，有卡录放可用","仅本地，有卡录音回放可用喵"));
        else if(service.pairing)snprintf(text,sizeof(text), "%s", !network.connected ?
            "等待连接 Wi-Fi" : network.error[0] ? "配对未完成，请看网页" : "正在配对，请稍候");
        else if (network.configured) {
#if MEMO_UI_COMPACT
            snprintf(text, sizeof(text), "服务已配对");
#else
            snprintf(text, sizeof(text), "服务已配对");
#endif
        }
        else if (network.error[0]) snprintf(text, sizeof(text),DETAIL_COPY("配对失败，网页重试","服务未配对，请在网页重试喵"));
        else text[0] = 0;
        text_if_changed(provision_detail_label, text);
        lv_obj_set_y(provision_back, lv_obj_get_y(provision_detail_label) +
            (text[0] ? lv_font_get_line_height(&memo_font) + (MEMO_UI_COMPACT ? 12 : 24) : 0));
    } else if (provision_visible) {
        show_provision(false);
    }
}

void ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, &memo_font, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    cat_root = lv_obj_create(screen);
    lv_obj_remove_style_all(cat_root);
    lv_obj_set_pos(cat_root, 0, 0);
    lv_obj_set_size(cat_root, MEMO_UI_WIDTH, MEMO_UI_HEIGHT);
    lv_obj_set_style_bg_color(cat_root, lv_color_hex(CAT_BG), 0);
    lv_obj_set_style_bg_opa(cat_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(cat_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cat_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cat_root, cat_touch, LV_EVENT_ALL, NULL);
    bool animated = cat_anim_init(cat_root);
#if MEMO_UI_COMPACT
    cat_fallback[0] = box(cat_root, 40, 96, 66, 33, 0xF4F4F4, 24);
    cat_fallback[1] = box(cat_root, 134, 96, 66, 33, 0xF4F4F4, 24);
#else
    cat_fallback[0] = box(cat_root, 68, 130, 86, 64, 0xF4F4F4, 38);
    cat_fallback[1] = box(cat_root, 206, 130, 86, 64, 0xF4F4F4, 38);
#endif
    if (animated) {
        lv_obj_add_flag(cat_fallback[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cat_fallback[1], LV_OBJ_FLAG_HIDDEN);
    }
#if MEMO_UI_COMPACT
    cat_state_dot = box(cat_root, 113, 181, 14, 14, CORAL, 7);
#else
    cat_state_dot = box(cat_root, 172, 258, 16, 16, CORAL, 8);
#endif
    lv_obj_add_flag(cat_state_dot, LV_OBJ_FLAG_HIDDEN);
    cat_level_bar = lv_bar_create(cat_root);
#if MEMO_UI_COMPACT
    lv_obj_set_pos(cat_level_bar, 50, 205);
    lv_obj_set_size(cat_level_bar, 140, 4);
#else
    lv_obj_set_pos(cat_level_bar, 100, 286);
    lv_obj_set_size(cat_level_bar, 160, 4);
#endif
    lv_obj_set_style_bg_color(cat_level_bar, lv_color_hex(0x243A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cat_level_bar, lv_color_hex(CORAL), LV_PART_INDICATOR);
    lv_bar_set_range(cat_level_bar, 0, 100);
    lv_obj_add_flag(cat_level_bar, LV_OBJ_FLAG_HIDDEN);

    /* A tiny microphone is drawn over the face without font glyphs or words. */
#if MEMO_UI_COMPACT
    usb_cat_mic=box(cat_root,113,146,14,21,GOLD,7);
    usb_cat_stem=box(cat_root,119,167,2,11,GOLD,1);
    usb_cat_foot=box(cat_root,109,177,22,3,GOLD,2);
    static const lv_point_precise_t slash_points[]={{0,0},{27,32}};
    usb_cat_slash=lv_line_create(cat_root);
    lv_line_set_points(usb_cat_slash,slash_points,2);
    lv_obj_set_pos(usb_cat_slash,106,145);
#else
    usb_cat_mic=box(cat_root,172,218,16,24,GOLD,8);
    usb_cat_stem=box(cat_root,179,242,2,12,GOLD,1);
    usb_cat_foot=box(cat_root,167,253,26,3,GOLD,2);
    static const lv_point_precise_t slash_points[]={{0,0},{31,39}};
    usb_cat_slash=lv_line_create(cat_root);
    lv_line_set_points(usb_cat_slash,slash_points,2);
    lv_obj_set_pos(usb_cat_slash,165,217);
#endif
    lv_obj_set_style_line_color(usb_cat_slash,lv_color_hex(CORAL),0);
    lv_obj_set_style_line_width(usb_cat_slash,3,0);
    lv_obj_add_flag(usb_cat_mic,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usb_cat_stem,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usb_cat_foot,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usb_cat_slash,LV_OBJ_FLAG_HIDDEN);

    panel_root = lv_obj_create(screen);
    lv_obj_remove_style_all(panel_root);
    lv_obj_set_pos(panel_root, 0, 0);
    lv_obj_set_size(panel_root, MEMO_UI_WIDTH, MEMO_UI_HEIGHT);
    lv_obj_remove_flag(panel_root, LV_OBJ_FLAG_SCROLLABLE);
#if !MEMO_UI_COMPACT
    lv_obj_set_style_bg_color(panel_root,lv_color_hex(CAT_BG),0);
    lv_obj_set_style_bg_opa(panel_root,LV_OPA_COVER,0);
#endif
#if MEMO_UI_COMPACT
    battery_button = button(panel_root, 52, 21, 51, 28, "", NULL);
    lv_obj_add_event_cb(battery_button, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_opa(battery_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(battery_button, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_add_flag(lv_obj_get_child(battery_button, 0), LV_OBJ_FLAG_HIDDEN);
    battery_outline = box(battery_button, 2, 9, 11, 10, MINT, 2);
    lv_obj_set_style_bg_opa(battery_outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_outline, 1, 0);
    lv_obj_set_style_border_color(battery_outline, lv_color_hex(MINT), 0);
    battery_fill = box(battery_button, 4, 11, 7, 6, MINT, 1);
    battery_terminal = box(battery_button, 13, 12, 2, 4, MINT, 1);
    /* Three 18 px digits occupy 30 px in memo_font. Leave two extra pixels so
     * a full charge is rendered as 100 instead of being clipped to 10. */
    battery_label = label(battery_button, 16, 6, 32, "--", MINT);
    /* The compact screen keeps one short status in the header.  The full-screen
     * cat remains the primary state feedback, so a repeated title is omitted. */
    ears[0] = box(panel_root, 120, 60, 1, 1, MINT, 0);
    ears[1] = box(panel_root, 120, 60, 1, 1, MINT, 0);
    face = box(panel_root, 120, 60, 1, 1, MINT, 0);
    eyes[0] = box(panel_root, 120, 60, 1, 1, BG, 0);
    eyes[1] = box(panel_root, 120, 60, 1, 1, BG, 0);
    mouth = label(panel_root, 120, 60, 1, "w", BG);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_add_flag(ears[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eyes[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth, LV_OBJ_FLAG_HIDDEN);
    status_label = label(panel_root, 103, 25, 82, "待机", MINT);
    mode_label = label(panel_root,101,29,36,"无卡",CLOUD);
    lv_obj_add_flag(mode_label,LV_OBJ_FLAG_HIDDEN);
    detail_label = label(panel_root, 30, 53, 180, "", MUTED);
    level_bar = lv_bar_create(panel_root);
    lv_obj_set_pos(level_bar, 65, 76);
    lv_obj_set_size(level_bar, 110, 3);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x243A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(CORAL), LV_PART_INDICATOR);
    lv_bar_set_range(level_bar, 0, 100);
    primary = button(panel_root, 48, 84, 144, 40, "开始录音", &primary_label);
    lv_obj_add_event_cb(primary, primary_clicked, LV_EVENT_CLICKED, NULL);
    count_label = label(panel_root, 30, 128, 180, "记录 0 条  0/0", MUTED);
    for (size_t i = 0; i < MEMO_PAGE_SIZE; ++i) {
        rows[i] = button(panel_root, 69, 150 + (int)i * 26, 102, 24, "", &row_labels[i]);
        lv_obj_add_event_cb(rows[i], record_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_set_ext_click_area(rows[MEMO_PAGE_SIZE - 1], 14);
    lv_obj_add_flag(rows[MEMO_PAGE_SIZE - 1], LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(rows[MEMO_PAGE_SIZE - 1], bottom_hit_test, LV_EVENT_HIT_TEST, NULL);
    /* Transparent side sectors give the triangles generous touch targets while
     * leaving the record rows visually uncluttered on the round panel. */
    previous = button(panel_root, 19, 139, 50, 88, "", NULL);
    next = button(panel_root, 171, 139, 50, 88, "", NULL);
    lv_obj_set_style_bg_opa(previous, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(next, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(lv_obj_get_child(previous, 0), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(next, 0), LV_OBJ_FLAG_HIDDEN);
#else
    /* Every visible point above the record key returns to the cat view.
     * The physical round screen clips the rectangular hit area at its edge. */
    lv_obj_t *top_return = button(panel_root,0,0,360,70,"",NULL);
    lv_obj_add_event_cb(top_return,back_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_set_style_bg_opa(top_return,LV_OPA_TRANSP,0);
    lv_obj_set_style_bg_opa(top_return,LV_OPA_TRANSP,LV_STATE_PRESSED);
    lv_obj_add_flag(lv_obj_get_child(top_return,0),LV_OBJ_FLAG_HIDDEN);
#if MEMO_UI_BATTERY
    battery_button=top_return;
    fish_icon=lv_image_create(panel_root);
    lv_image_set_src(fish_icon,&memo_icon_fish);
    lv_obj_set_pos(fish_icon,210,32);
    battery_label=label(panel_root,241,32,38,"--",MINT);
#else
    fish_icon=lv_image_create(panel_root);
    lv_image_set_src(fish_icon,&memo_icon_fish);
    lv_obj_set_pos(fish_icon,210,32);
    label(panel_root,241,32,38,"--",MINT);
#endif
    ears[0]=box(panel_root,180,14,1,1,MINT,0);
    ears[1]=box(panel_root,180,14,1,1,MINT,0);
    face=box(panel_root,180,14,1,1,MINT,0);
    eyes[0]=box(panel_root,180,14,1,1,BG,0);
    eyes[1]=box(panel_root,180,14,1,1,BG,0);
    mouth=label(panel_root,180,14,1,"w",BG);
    for(unsigned i=0;i<2;i++){
        lv_obj_add_flag(ears[i],LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eyes[i],LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(face,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth,LV_OBJ_FLAG_HIDDEN);
    status_label=label(panel_root,110,32,90,"待机",MINT);
    mode_label=label(panel_root,82,34,40,"无卡",CLOUD);
    lv_obj_add_flag(mode_label,LV_OBJ_FLAG_HIDDEN);
    detail_label=label(panel_root,38,122,284,"",MUTED);
    lv_obj_set_style_text_font(status_label,&memo_font20,0);
    lv_obj_set_style_text_font(detail_label,&memo_font20,0);
    level_bar = lv_bar_create(panel_root);
    lv_obj_set_pos(level_bar, 105, 148);
    lv_obj_set_size(level_bar, 150, 3);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x243A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(CORAL), LV_PART_INDICATOR);
    lv_bar_set_range(level_bar, 0, 100);
    lv_obj_add_flag(level_bar,LV_OBJ_FLAG_HIDDEN);
    primary = button(panel_root, 72, 70, 216, 46, "开始录音", &primary_label);
    lv_obj_set_align(primary_label,LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(primary_label,66,12);
    lv_obj_set_width(primary_label,120);
    lv_obj_set_style_text_font(primary_label,&memo_font20,0);
    record_icon=lv_image_create(primary);
    lv_image_set_src(record_icon,&memo_icon_cat);
    lv_obj_set_pos(record_icon,41,9);
    stop_icon=box(primary,49,15,16,16,BG,3);
    lv_obj_add_flag(stop_icon,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(primary, primary_clicked, LV_EVENT_CLICKED, NULL);
    count_label = label(panel_root, 110, 325, 140, "0条  0/0", MUTED);
    empty_label=label(panel_root,66,216,228,"第一条想法，等你开口喵",MUTED);
    lv_obj_set_style_text_font(empty_label,&memo_font20,0);
    lv_obj_add_flag(empty_label,LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < MEMO_PAGE_SIZE; ++i) {
        int x=82+(int)(i%2)*102,y=153+(int)(i/2)*54;
        rows[i] = button(panel_root, x, y, 94, 48, "", &row_labels[i]);
        lv_obj_set_style_bg_color(rows[i],lv_color_hex(0x111B1B),0);
        lv_obj_set_style_bg_color(rows[i],lv_color_hex(0x27423E),LV_STATE_PRESSED);
        lv_obj_set_width(row_labels[i],86);
        lv_obj_add_event_cb(rows[i], record_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    /* Use all side space up to, but not overlapping, the two record columns. */
    previous=button(panel_root,0,165,82,105,"",NULL);
    next=button(panel_root,278,165,82,105,"",NULL);
    lv_obj_t *paw=lv_image_create(previous);
    lv_image_set_src(paw,&memo_icon_paw);lv_obj_set_pos(paw,36,34);
    paw=lv_image_create(next);
    lv_image_set_src(paw,&memo_icon_paw);lv_obj_set_pos(paw,12,34);
#endif
#if MEMO_UI_COMPACT
    lv_obj_add_event_cb(previous, draw_page_arrow, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(next, draw_page_arrow, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)1);
#else
    lv_obj_set_style_bg_opa(previous,LV_OPA_TRANSP,0);
    lv_obj_set_style_bg_opa(next,LV_OPA_TRANSP,0);
    lv_obj_set_style_bg_opa(previous,LV_OPA_TRANSP,LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(next,LV_OPA_TRANSP,LV_STATE_PRESSED);
    lv_obj_add_flag(lv_obj_get_child(previous,0),LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(next,0),LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_add_event_cb(previous, page_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)CMD_PREV);
    lv_obj_add_event_cb(next, page_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)CMD_NEXT);

    provision_root = lv_obj_create(screen);
    lv_obj_remove_style_all(provision_root);
    lv_obj_set_pos(provision_root, 0, 0);
    lv_obj_set_size(provision_root, MEMO_UI_WIDTH, MEMO_UI_HEIGHT);
    lv_obj_set_style_bg_color(provision_root, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(provision_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(provision_root, LV_OBJ_FLAG_SCROLLABLE);
    const int provision_line_height = lv_font_get_line_height(&memo_font);
    const int provision_gap = 6;
    const int provision_step = provision_line_height + provision_gap;
#if MEMO_UI_COMPACT
    const int provision_flow_y = 64;
    const int provision_feedback_y = provision_flow_y + provision_step +
                                     provision_line_height + 2 * provision_gap;
    label(provision_root, 55, provision_flow_y - provision_line_height - 2 * provision_gap,
          130, "联网设置", MINT);
    provision_ap_label = label(provision_root, 20, provision_flow_y, 200, "热点正在启动", INK);
    provision_detail_label = label(provision_root, 30, provision_feedback_y, 180, "", GOLD);
    label(provision_root, 20, provision_flow_y + provision_step, 200, "打开 http://192.168.4.1", INK);
    provision_back = button(provision_root, 58,
        provision_feedback_y + provision_line_height + 2 * provision_gap,
        124, 36, "退出配网", NULL);
#else
    const int provision_flow_y = 91;
    const int provision_feedback_y = provision_flow_y + 2 * provision_step +
                                     provision_line_height + 3 * provision_gap;
    label(provision_root, 70, provision_flow_y - provision_line_height - 3 * provision_gap,
          220, "联网设置", MINT);
    label(provision_root, 55, provision_flow_y, 250, "手机连接下面的临时热点", MUTED);
    provision_ap_label = label(provision_root, 38, provision_flow_y + provision_step, 284, "热点正在启动", INK);
    provision_detail_label = label(provision_root, 38, provision_feedback_y, 284, "", GOLD);
    label(provision_root, 38, provision_flow_y + 2 * provision_step, 284, "打开 http://192.168.4.1", INK);
    provision_back = button(provision_root, 92,
        provision_feedback_y + provision_line_height + 4 * provision_gap,
        176, 44, "退出配网", NULL);
#endif
    lv_obj_add_event_cb(provision_back, provision_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(provision_root, LV_OBJ_FLAG_HIDDEN);
    record_root=box(screen,0,0,MEMO_UI_WIDTH,MEMO_UI_HEIGHT,
                    MEMO_UI_COMPACT?BG:CAT_BG,0);
#if MEMO_UI_COMPACT
    record_title=label(record_root,50,34,140,"录音",MINT);
    record_meta=label(record_root,22,58,196,"",MUTED);
    record_status_label=label(record_root,18,90,204,"状态待查询",GOLD);
    record_info=label(record_root,30,118,180,"",MUTED);
    lv_obj_set_style_text_line_space(record_info,-3,0);
    record_play=button(record_root,27,122,90,42,"播放录音",&record_play_label);
    record_retry=button(record_root,123,122,90,42,"重试",&record_retry_label);
    record_back=button(record_root,70,175,100,38,"返回",NULL);
    lv_obj_set_ext_click_area(record_back, 27);
    lv_obj_add_flag(record_back, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(record_back, bottom_hit_test, LV_EVENT_HIT_TEST, NULL);
#else
    record_title=label(record_root,66,42,228,"录音",INK);
    lv_obj_set_style_text_font(record_title,&memo_font24,0);
    record_meta=label(record_root,58,83,244,"",MUTED);
    record_status_dot=box(record_root,80,127,7,7,GOLD,4);
    record_status_label=label(record_root,94,114,200,"状态待查询喵",GOLD);
    lv_obj_set_style_text_font(record_status_label,&memo_font20,0);
    lv_obj_set_style_text_align(record_status_label,LV_TEXT_ALIGN_LEFT,0);
    record_info=label(record_root,64,164,232,"",MUTED);
    lv_obj_set_style_text_font(record_info,&memo_font20,0);
    record_play=button(record_root,66,155,228,70,"播放录音",&record_play_label);
    lv_obj_set_style_radius(record_play,15,0);
    lv_obj_set_style_bg_color(record_play,lv_color_hex(MINT),0);
    lv_obj_set_style_bg_color(record_play,lv_color_hex(0xE7EEE8),LV_STATE_PRESSED);
    lv_obj_set_style_text_font(record_play_label,&memo_font22,0);
    lv_obj_set_style_text_color(record_play_label,lv_color_hex(0x16332D),0);
    lv_obj_set_width(record_play_label,120);
    lv_obj_set_align(record_play_label,LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(record_play_label,75,22);
    record_cat_icon=lv_image_create(record_play);
    lv_image_set_src(record_cat_icon,&memo_icon_cat);
    lv_obj_set_pos(record_cat_icon,27,21);
    lv_obj_remove_flag(record_cat_icon,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(record_play,draw_record_play_icon,LV_EVENT_DRAW_MAIN_END,NULL);
    record_retry=button(record_root,66,247,228,46,"开始同步",&record_retry_label);
    lv_obj_set_style_bg_opa(record_retry,LV_OPA_TRANSP,0);
    lv_obj_set_style_text_font(record_retry_label,&memo_font20,0);
    lv_obj_set_style_text_color(record_retry_label,lv_color_hex(MUTED),0);
    record_back=button(record_root,0,313,360,47,"返回",NULL);
    lv_obj_set_style_bg_opa(record_back,LV_OPA_TRANSP,0);
    lv_obj_t *back_label=lv_obj_get_child(record_back,0);
    lv_obj_set_style_text_font(back_label,&memo_font20,0);
    lv_obj_set_style_text_color(back_label,lv_color_hex(MUTED),0);
    lv_obj_set_width(back_label,80);
    lv_obj_center(back_label);
#endif
    lv_obj_add_flag(record_info,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(record_play,record_play_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(record_retry,record_retry_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(record_back,record_back_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_flag(record_root,LV_OBJ_FLAG_HIDDEN);

    usb_root=box(screen,0,0,MEMO_UI_WIDTH,MEMO_UI_HEIGHT,BG,0);
#if MEMO_UI_COMPACT
    label(usb_root,84,29,108,"电脑麦克风",GOLD);
    usb_title_icon=lv_image_create(usb_root);
    lv_image_set_src(usb_title_icon,&memo_icon_cat);
    lv_obj_set_pos(usb_title_icon,51,27);
    usb_status=label(usb_root,30,59,180,"等待电脑喵",INK);
    usb_hint=label(usb_root,26,79,188,"电脑选喵伴麦克风",MUTED);
    usb_mute=button(usb_root,45,106,150,48,"静音麦克风",&usb_mute_label);
    usb_exit=button(usb_root,58,162,124,34,"退出麦克风",NULL);
    usb_back=button(usb_root,75,208,90,24,"返回猫脸",NULL);
#else
    label(usb_root,120,49,150,"电脑麦克风",GOLD);
    usb_title_icon=lv_image_create(usb_root);
    lv_image_set_src(usb_title_icon,&memo_icon_cat);
    lv_obj_set_pos(usb_title_icon,91,46);
    usb_status=label(usb_root,70,88,220,"等待电脑喵",INK);
    usb_hint=label(usb_root,53,116,254,"请在电脑选择喵伴麦克风",MUTED);
    usb_mute=button(usb_root,64,154,232,68,"静音麦克风",&usb_mute_label);
    lv_obj_set_style_text_font(usb_mute_label,&memo_font22,0);
    usb_exit=button(usb_root,86,239,188,48,"退出麦克风",NULL);
    usb_back=button(usb_root,130,315,100,28,"返回猫脸",NULL);
#endif
    lv_obj_remove_flag(usb_title_icon,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(usb_mute,lv_color_hex(MINT),0);
    lv_obj_set_style_bg_color(usb_mute,lv_color_hex(0xE7EEE8),LV_STATE_PRESSED);
    lv_obj_set_style_text_color(usb_mute_label,lv_color_hex(BG),0);
    lv_obj_set_style_bg_color(usb_exit,lv_color_hex(0x604446),0);
    lv_obj_set_style_bg_opa(usb_back,LV_OPA_TRANSP,0);
    lv_obj_add_event_cb(usb_mute,usb_mute_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(usb_exit,usb_exit_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(usb_back,usb_back_clicked,LV_EVENT_CLICKED,NULL);
    lv_obj_add_flag(usb_root,LV_OBJ_FLAG_HIDDEN);
    show_panel(false);
    refresh(NULL);
    lv_timer_create(refresh, 200, NULL);
}
