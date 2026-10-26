#include "pager_ui.h"

#include "pager_audio.h"

#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "pager_board.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "src/display/lv_display_private.h"

extern const lv_font_t pager_font_16;
extern const lv_font_t pager_font_24;

static lv_coord_t s_layout_w = BSP_LCD_H_RES;
static lv_coord_t s_layout_h = BSP_LCD_V_RES;

/* 2.16 / 480：正文 16、标题 24。1.83 / 窄屏仍用 12。 */
static lv_font_t s_ui_font;
static lv_font_t s_ui_font_24;

static bool ui_wide(void)
{
    return s_layout_w >= 360;
}

/* Brookesia 状态栏叠在可视区域顶部，标题必须从这之下开始。 */
static lv_coord_t ui_top(void)
{
    /* The phone status bar is hidden inside the app now; only a small
     * inset keeps content off the rounded glass edge. */
    (void)0;
    return 8;
}

static int cat_s(int v)
{
    return ui_wide() ? (v * 3 / 2) : v;
}

static const lv_font_t *ui_font(void)
{
    if (s_ui_font.get_glyph_dsc == NULL) {
        s_ui_font = pager_font_16;
        s_ui_font.fallback = &lv_font_montserrat_16;
    }
    return &s_ui_font;
}

static const lv_font_t *ui_font_24(void)
{
    if (s_ui_font_24.get_glyph_dsc == NULL) {
        s_ui_font_24 = pager_font_24;
        s_ui_font_24.fallback = &lv_font_montserrat_16;
    }
    return &s_ui_font_24;
}

#define UI_W s_layout_w
#define UI_H s_layout_h
#define CARD_W ((UI_W) > 28 ? (lv_coord_t)((UI_W) - 24) : (lv_coord_t)(UI_W))
#define CARD_X ((lv_coord_t)12)
#define BAR_H 28
#define DOT_H 12
#define BODY_H (UI_H - BAR_H - DOT_H)

static void layout_sync(void)
{
    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_coord_t w = lv_obj_get_width(scr);
        lv_coord_t h = lv_obj_get_height(scr);
        if (w > 0 && h > 0) {
            s_layout_w = w;
            s_layout_h = h;
            return;
        }
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        s_layout_w = lv_display_get_horizontal_resolution(disp);
        s_layout_h = lv_display_get_vertical_resolution(disp);
    }
}
#define FONT_DETAIL &lv_font_montserrat_10
#define FONT_CAPTION &lv_font_montserrat_12
#define FONT_RING_PCT &lv_font_montserrat_12
#define FONT_MICRO &lv_font_montserrat_8

#define COL_BG 0x0B0D0E
#define COL_BG_TOP 0x111517
#define COL_BG_BOTTOM 0x07090A
#define COL_CARD 0x15191B
#define COL_CARD_HI 0x1B2022
#define COL_ORANGE 0xFF6B3D
#define COL_ORANGE_DARK 0x2A1711
#define COL_TEXT 0xF5F0E6
#define COL_TEXT_DIM 0xC9C3B8
#define COL_MUTED 0x8B8B84
#define COL_DENY 0x3A2222
#define COL_DENY_BG 0x251414
#define COL_MINT 0x8AE0C6
#define COL_AMBER 0xFFC857
#define COL_DANGER 0xFF7070
#define COL_LINE 0x242728
#define COL_LINE_SOFT 0x1D2123

static lv_obj_t *s_code_label;
static lv_obj_t *s_hint;
static lv_obj_t *s_state;
static lv_obj_t *s_detail;
static lv_obj_t *s_reply;
static char s_code[8];
static char s_approval_id[48];

static bool s_shell;
static int s_page;
static bool s_pending;
static const char *const VOICE_NAME[] = {"晓晓", "晓伊", "云希", "MOSS"};
#define VOICE_N 4
static int s_voice_i;
static bool s_announce;
static int s_volume = 75;
static bool s_hold;
static bool s_offline;
static bool s_hw_ready;
static int s_kind;
static int s_listen;
static bool s_meeting;
static char s_note[48];
static char s_note_sub[48];
static lv_timer_t *s_key_timer;

static lv_obj_t *s_toast;
static lv_obj_t *s_toast_bar;
static lv_obj_t *s_toast_title;
static lv_obj_t *s_toast_body;
static lv_timer_t *s_toast_timer;

static lv_obj_t *s_macro_btn;
static char s_macro_id[2][32];
static char s_macro_label[2][40];
static int s_macro_n;

static lv_obj_t *s_bar;
static lv_obj_t *s_body;
static lv_obj_t *s_stats_body;
static lv_obj_t *s_appr_body;
static lv_obj_t *s_dots;
static lv_obj_t *s_dot0;
static lv_obj_t *s_dot1;
static lv_obj_t *s_dot2;
typedef struct {
    lv_obj_t *card;
    lv_obj_t *ring_box;
    lv_obj_t *arc_bg;
    lv_obj_t *arc_glow;
    lv_obj_t *arc;
    lv_obj_t *title;
    lv_obj_t *pct;
    lv_obj_t *sub;
    bool hot;
} host_ring_t;
static host_ring_t s_host_cpu;
static host_ring_t s_host_ram;
static host_ring_t s_host_gpu;
static host_ring_t s_host_net;
static lv_timer_t *s_host_fx_timer;
static lv_obj_t *s_settings;
static lv_obj_t *s_wait;
static lv_obj_t *s_wait_title;
static lv_obj_t *s_said;
static lv_obj_t *s_said_box;
static lv_obj_t *s_home_line;
static lv_obj_t *s_home_tool;
static lv_obj_t *s_home_tool_accent;
static lv_obj_t *s_home_tool_name;
static lv_obj_t *s_home_tool_mark;
static char s_tool_counts[64];
static char *s_said_buf;
static char *s_reply_raw;
static char *s_reply_scratch;
static char *s_reply_latest;
static SemaphoreHandle_t s_reply_mu;
static volatile int s_reply_gen;
static int s_reply_seen;
static bool s_reply_due;
static uint32_t s_said_rev;
static uint32_t s_said_painted;
static int s_said_y;
static int s_said_h;
static lv_obj_t *s_wait_sub;
static lv_obj_t *s_net_line;
static lv_obj_t *s_nav_prov;
static lv_obj_t *s_nav_pair;
static lv_obj_t *s_prov_page;
static lv_obj_t *s_prov_name;
static lv_obj_t *s_pair_page;
static lv_obj_t *s_pair_host;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_wifi_page;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_appr_card;
static lv_obj_t *s_appr_mark;
static lv_obj_t *s_appr_allow;
static lv_obj_t *s_appr_deny;
static lv_obj_t *s_appr_hint;
static lv_obj_t *s_appr_cmd_btn;
static lv_obj_t *s_appr_cmd_text;
static char s_cmd_store[180];
static lv_obj_t *s_appr_meta;
static bool s_appr_can;
static lv_obj_t *s_wifi;
static lv_obj_t *s_batt;
static lv_obj_t *s_risk_pill;
static char s_risk_code[8];
static bool s_risk_l3;
static lv_obj_t *s_appr_title;
static lv_obj_t *s_appr_risk;
static lv_obj_t *s_speak_value;
static lv_obj_t *s_voice_value;
static lv_obj_t *s_vol_value;
static lv_obj_t *s_face_box;
static lv_obj_t *s_face_l;
static lv_obj_t *s_face_r;
static lv_obj_t *s_face_m;
static lv_obj_t *s_big_box;
static lv_obj_t *s_big_l;
static lv_obj_t *s_big_r;
static lv_obj_t *s_big_m;
static lv_obj_t *s_cat_head;
static lv_obj_t *s_cat_ear_l;
static lv_obj_t *s_cat_ear_r;
static lv_obj_t *s_cat_in_l;
static lv_obj_t *s_cat_in_r;
static lv_obj_t *s_cat_pupil_l;
static lv_obj_t *s_cat_pupil_r;
static lv_obj_t *s_cat_tail;
static lv_timer_t *s_cat_timer;
static int s_cat_tick;
static lv_obj_t *s_brand;
static lv_obj_t *s_mini;
static lv_obj_t *s_mini_ear_l;
static lv_obj_t *s_mini_ear_r;
static lv_obj_t *s_mini_head;
static lv_obj_t *s_mini_eye_l;
static lv_obj_t *s_mini_eye_r;
static lv_obj_t *s_tool_row;
static lv_obj_t *s_tool_accent;
static lv_obj_t *s_tool_name;
static lv_obj_t *s_tool_mark;
static lv_obj_t *s_tool_sum;
static bool s_has_text;
static bool s_has_tools;
static int s_done_n;
static int s_fail_n;
static char s_bar_detail[96];
static lv_obj_t *s_face_ring;
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_label;
static lv_obj_t *s_dock_hint;
static lv_obj_t *s_reply_card;
static lv_obj_t *s_appr_badge;
static lv_obj_t *s_confirm;
static lv_obj_t *s_confirm_title;
static lv_obj_t *s_confirm_hint;
static int s_confirm_kind;

static void vis(lv_obj_t *obj, bool on);
static bool settings_open(void);
static void layout_actions(void);
static void layout_feed(void);
static void build_shell(void);
static void build_shell_extra(void);
static void approval_paint_empty(void);
static void ui_set_net_line(const char *text);
static lv_obj_t *s_app_scr;
static int s_ui_scene;
static bool s_shell_extra;
static bool s_block_wifi;
static lv_timer_t *s_scene_flush_timer;
static lv_obj_t *s_wifi_btn;
static lv_obj_t *s_wifi_title;
static lv_obj_t *s_wifi_sub;
#define UI_SCENE_NONE 0
#define UI_SCENE_WIFI 1
#define UI_SCENE_HOME 2
#define UI_SCENE_PAIR 3
#define UI_SCENE_PROV 4

static const char *ui_scene_name(int scene)
{
    switch (scene) {
    case UI_SCENE_WIFI:
        return "wifi";
    case UI_SCENE_HOME:
        return "home";
    case UI_SCENE_PAIR:
        return "pair";
    case UI_SCENE_PROV:
        return "provision";
    default:
        return "none";
    }
}

static void ui_bind_app_screen(void)
{
    if (!s_app_scr || !lv_obj_is_valid(s_app_scr)) {
        s_app_scr = lv_scr_act();
    }
}

static void ui_log_heap(const char *where)
{
    ESP_LOGI("pager_ui", "%s scene=%s heap int=%u psram=%u",
             where ? where : "?",
             ui_scene_name(s_ui_scene),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void ui_refresh_now(void)
{
    ui_bind_app_screen();
    if (s_app_scr && lv_obj_is_valid(s_app_scr) && lv_scr_act() != s_app_scr) {
        lv_screen_load(s_app_scr);
    }
    /* A synchronous refresh from inside an LVGL timer re-enters the
     * flush and stalls the UI task. The normal refresh cycle paints
     * the dirty area on its own. */
}

static lv_obj_t *screen(void)
{
    if (s_cat_timer) {
        lv_timer_del(s_cat_timer);
        s_cat_timer = NULL;
    }
    ui_bind_app_screen();
    lv_obj_t *scr = s_app_scr ? s_app_scr : lv_scr_act();
    lv_obj_clean(scr);
    s_ui_scene = UI_SCENE_NONE;
    s_shell_extra = false;
    /* s_block_wifi is owned by home/provision, not cleared here. */
    s_code_label = NULL;
    s_wifi_btn = NULL;
    s_wifi_title = NULL;
    s_wifi_sub = NULL;
    s_hint = NULL;
    s_state = NULL;
    s_detail = NULL;
    s_reply = NULL;
    s_shell = false;
    s_bar = s_body = s_stats_body = s_appr_body = s_dots = s_settings = NULL;
    s_dot2 = NULL;
    memset(&s_host_cpu, 0, sizeof(s_host_cpu));
    memset(&s_host_ram, 0, sizeof(s_host_ram));
    memset(&s_host_gpu, 0, sizeof(s_host_gpu));
    memset(&s_host_net, 0, sizeof(s_host_net));
    s_wait = s_wait_title = s_wait_sub = s_net_line = NULL;
    s_said = s_said_box = NULL;
    s_home_line = s_home_tool = s_home_tool_accent = NULL;
    s_home_tool_name = s_home_tool_mark = NULL;
    s_toast = NULL;
    s_nav_prov = s_nav_pair = NULL;
    s_prov_page = s_prov_name = NULL;
    s_pair_page = s_pair_host = s_pair_hint = NULL;
    s_wifi_page = s_wifi_status = NULL;
    s_appr_card = s_appr_mark = s_wifi = s_batt = s_risk_pill = NULL;
    s_appr_allow = s_appr_deny = s_appr_hint = s_appr_meta = NULL;
    s_appr_can = false;
    s_appr_title = s_appr_risk = s_speak_value = s_voice_value = s_vol_value = NULL;
    s_face_box = s_face_l = s_face_r = s_face_m = NULL;
    s_big_box = s_big_l = s_big_r = s_big_m = NULL;
    s_cat_head = s_cat_ear_l = s_cat_ear_r = s_cat_in_l = s_cat_in_r = NULL;
    s_cat_pupil_l = s_cat_pupil_r = s_cat_tail = NULL;
    s_dot0 = s_dot1 = NULL;
    s_brand = s_face_ring = s_dock = s_dock_label = s_dock_hint = NULL;
    s_mini = s_mini_ear_l = s_mini_ear_r = s_mini_head = NULL;
    s_mini_eye_l = s_mini_eye_r = NULL;
    s_tool_row = s_tool_accent = s_tool_name = s_tool_mark = s_tool_sum = NULL;
    s_has_text = s_has_tools = false;
    s_done_n = s_fail_n = 0;
    s_bar_detail[0] = 0;
    s_reply_card = s_appr_badge = NULL;
    s_confirm = s_confirm_title = s_confirm_hint = NULL;
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(COL_BG_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    if (lv_scr_act() != scr) {
        lv_screen_load(scr);
    }
    return scr;
}

static lv_obj_t *text_font(lv_obj_t *parent, const char *str, uint32_t color,
                           const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, str ? str : "");
    lv_obj_set_style_text_font(label, font ? font : ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *text(lv_obj_t *parent, const char *str, uint32_t color)
{
    return text_font(parent, str, color, ui_font());
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t bg, uint32_t border, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    if (bg) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    }
    if (border) {
        lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
    } else {
        lv_obj_set_style_border_width(obj, 0, 0);
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *str, uint32_t bg, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD_HI), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    lv_obj_t *label = text(btn, str, COL_TEXT);
    lv_obj_center(label);
    return btn;
}

static int s_lock_depth;
static TaskHandle_t s_ui_owner;

static bool on_ui_task(void)
{
    return s_ui_owner && xTaskGetCurrentTaskHandle() == s_ui_owner;
}

static bool lock(void)
{
    if (s_lock_depth > 0) {
        s_lock_depth++;
        return true;
    }
    /* Brookesia already holds this recursive mutex when calling App::run(). */
    if (on_ui_task()) {
        s_lock_depth = 1;
        return true;
    }
    if (esp_lv_adapter_lock(8000) != ESP_OK) {
        ESP_LOGW("pager_ui", "display lock timeout");
        return false;
    }
    s_lock_depth = 1;
    return true;
}

static void unlock(void)
{
    if (s_lock_depth <= 0) {
        return;
    }
    s_lock_depth--;
    if (s_lock_depth == 0 && !on_ui_task()) {
        esp_lv_adapter_unlock();
    }
}

typedef void (*ui_disp_cb_t)(void *);

typedef struct {
    ui_disp_cb_t cb;
    uint16_t len;
    uint8_t data[];
} ui_disp_msg_t;

static QueueHandle_t s_disp_q;
static lv_timer_t *s_disp_timer;

static void reply_service(void);
static volatile int s_qa_swipe_req;
volatile uint32_t pager_ui_beat;
#if CONFIG_PAGER_QA
static volatile bool s_qa_shot_req;
static void ui_qa_shot_cb(void *);
#endif

void ui_qa_request_swipe(int toward_right)
{
    s_qa_swipe_req = toward_right ? 1 : -1;
}

/* Simulated side-key hold for QA: same apply_ptt path as the GPIO key. */
static volatile int s_qa_ptt_ms;
static volatile int s_qa_settings_req;
static void apply_ptt(bool down);
static void open_settings(lv_event_t *e);
static void close_settings(lv_event_t *e);
static void qa_ptt_release_cb(lv_timer_t *timer)
{
    (void)timer;
    apply_ptt(false);
}

void ui_qa_request_ptt(int hold_ms)
{
    if (hold_ms < 300) {
        hold_ms = 1200;
    }
    if (hold_ms > 11000) {
        hold_ms = 11000;
    }
    s_qa_ptt_ms = hold_ms;
}

void ui_qa_request_settings(int open)
{
    s_qa_settings_req = open ? 1 : -1;
}

static void ui_dispatch_pump(lv_timer_t *t)
{
    (void)t;
    pager_ui_beat++;
    if (s_qa_swipe_req) {
        int toward_right = s_qa_swipe_req > 0;
        s_qa_swipe_req = 0;
        pager_ui_swipe(toward_right);
    }
    if (s_qa_ptt_ms > 0) {
        int ms = s_qa_ptt_ms;
        s_qa_ptt_ms = 0;
        apply_ptt(true);
        lv_timer_t *rel = lv_timer_create(qa_ptt_release_cb, ms, NULL);
        lv_timer_set_repeat_count(rel, 1);
    }
    if (s_qa_settings_req) {
        int open = s_qa_settings_req > 0;
        s_qa_settings_req = 0;
        if (open) {
            open_settings(NULL);
        } else {
            close_settings(NULL);
        }
    }
#if CONFIG_PAGER_QA
    if (s_qa_shot_req) {
        s_qa_shot_req = false;
        ui_qa_shot_cb(NULL);
    }
#endif
    /* Latest reply is applied here, not through the queue. A stream of
     * tokens used to fill the queue and redraw the whole label every time,
     * so the screen task never got back to touch. */
    reply_service();
    ui_disp_msg_t *m;
    /* One tick used to drain the whole queue and repaint every time.
     * That held the UI task long enough for the screen and the side key
     * to look frozen. Two messages per tick is enough. */
    for (int n = 0; n < 2; n++) {
        if (!s_disp_q || xQueueReceive(s_disp_q, &m, 0) != pdTRUE) {
            break;
        }
        if (!lock()) {
            xQueueSendToFront(s_disp_q, &m, 0);
            break;
        }
        m->cb(m->data);
        unlock();
        free(m);
    }
}

static void ui_dispatch_begin(void)
{
    if (s_disp_q) {
        return;
    }
    s_ui_owner = xTaskGetCurrentTaskHandle();
    s_disp_q = xQueueCreate(8, sizeof(ui_disp_msg_t *));
    s_disp_timer = lv_timer_create(ui_dispatch_pump, 20, NULL);
}

static void ui_dispatch(ui_disp_cb_t cb, const void *data, size_t len)
{
    if (!s_disp_q || xTaskGetCurrentTaskHandle() == s_ui_owner) {
        if (!lock()) {
            return;
        }
        cb((void *)data);
        unlock();
        return;
    }
    ui_disp_msg_t *m = heap_caps_malloc(sizeof(*m) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) {
        m = malloc(sizeof(*m) + len);
    }
    if (!m) {
        return;
    }
    m->cb = cb;
    m->len = (uint16_t)len;
    if (len && data) {
        memcpy(m->data, data, len);
    }
    if (xQueueSend(s_disp_q, &m, 0) != pdTRUE) {
        ESP_LOGW("pager_ui", "dispatch queue full");
        free(m);
    }
}

static void hide_confirm(void)
{
    if (s_confirm && lv_obj_is_valid(s_confirm)) {
        lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_confirm_cancel(lv_event_t *e)
{
    (void)e;
    hide_confirm();
}

static void on_confirm_ok(lv_event_t *e)
{
    (void)e;
    int kind = s_confirm_kind;
    hide_confirm();
    if (kind == 1) {
        pager_ui_reset_wifi();
    } else if (kind == 2) {
        pager_ui_reset_pair();
    }
}

static void show_confirm(int kind)
{
    s_confirm_kind = kind;
    if (!s_confirm || !lv_obj_is_valid(s_confirm)) {
        lv_obj_t *scr = lv_scr_act();
        s_confirm = panel(scr, 0, 0, UI_W, UI_H, 0x000000, 0, 0);
        lv_obj_set_style_bg_opa(s_confirm, LV_OPA_70, 0);
        lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *card = panel(s_confirm, 16, 68, 208, 148, COL_CARD, COL_LINE, 14);
        s_confirm_title = text(card, "", COL_TEXT);
        lv_obj_align(s_confirm_title, LV_ALIGN_TOP_LEFT, 14, 14);
        s_confirm_hint = text(card, "", COL_MUTED);
        lv_label_set_long_mode(s_confirm_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_confirm_hint, 180);
        lv_obj_align(s_confirm_hint, LV_ALIGN_TOP_LEFT, 14, 40);

        lv_obj_t *cancel = button(card, "取消", COL_CARD_HI, on_confirm_cancel, NULL);
        lv_obj_set_size(cancel, 86, 32);
        lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 12, -12);
        lv_obj_set_style_border_color(cancel, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_border_width(cancel, 1, 0);

        lv_obj_t *ok = button(card, "确认", COL_ORANGE, on_confirm_ok, NULL);
        lv_obj_set_size(ok, 86, 32);
        lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
        lv_obj_set_style_text_color(lv_obj_get_child(ok, 0), lv_color_hex(0x1D0B05), 0);
    }
    if (kind == 1) {
        lv_label_set_text(s_confirm_title, "重新配网？");
        lv_label_set_text(s_confirm_hint, "会断开当前网络，要用手机再连一次热点。");
    } else {
        lv_label_set_text(s_confirm_title, "重新配对？");
        lv_label_set_text(s_confirm_hint, "会断开这台电脑，之后要重新输入配对码。");
    }
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_confirm);
}

static void on_reset_wifi(lv_event_t *e)
{
    (void)e;
    show_confirm(1);
}

static void on_reset_pair(lv_event_t *e)
{
    (void)e;
    show_confirm(2);
}

static void on_retry(lv_event_t *e)
{
    (void)e;
    pager_ui_retry_discover();
}

static void set_approval_buttons_enabled(bool enabled)
{
    if (!s_appr_allow || !s_appr_deny) {
        return;
    }
    if (enabled) {
        lv_obj_clear_state(s_appr_allow, LV_STATE_DISABLED);
        lv_obj_clear_state(s_appr_deny, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_appr_allow, LV_STATE_DISABLED);
        lv_obj_add_state(s_appr_deny, LV_STATE_DISABLED);
    }
}

static void finish_approval(bool allow)
{
    if (!s_pending || !s_approval_id[0]) {
        return;
    }
    if (allow && !s_appr_can) {
        if (s_appr_hint) {
            lv_label_set_text(s_appr_hint, "这项操作只能在电脑上批准");
        }
        return;
    }
    pager_ui_decide(s_approval_id, allow);
    s_pending = false;
    set_approval_buttons_enabled(false);
    vis(s_appr_allow, false);
    vis(s_appr_deny, false);
    if (s_appr_title) {
        lv_label_set_text(s_appr_title, allow ? "已批准" : "已拒绝");
        lv_obj_set_style_text_color(s_appr_title, lv_color_hex(COL_TEXT), 0);
    }
    if (s_appr_badge) {
        lv_label_set_text(s_appr_badge, allow ? "OK" : "NO");
        lv_obj_set_style_bg_color(s_appr_badge,
                                  lv_color_hex(allow ? 0x123027 : COL_DENY_BG), 0);
        lv_obj_set_style_text_color(s_appr_badge,
                                    lv_color_hex(allow ? COL_MINT : COL_DANGER), 0);
        vis(s_appr_badge, true);
    }
    if (s_appr_risk) {
        lv_label_set_text(s_appr_risk, allow ? "已发送给电脑" : "已拒绝，未执行");
        lv_obj_set_style_text_color(s_appr_risk,
                                    lv_color_hex(allow ? COL_MINT : COL_DANGER), 0);
    }
    if (s_appr_card) {
        lv_obj_set_style_bg_color(s_appr_card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(s_appr_card,
                                      lv_color_hex(allow ? COL_MINT : COL_DENY), 0);
    }
    if (s_appr_mark) {
        lv_obj_set_style_bg_color(s_appr_mark,
                                  lv_color_hex(allow ? COL_MINT : COL_DANGER), 0);
    }
    if (s_appr_hint) {
        lv_label_set_text(s_appr_hint, allow ? "正在等待电脑执行" : "已拒绝，操作不会执行");
        lv_obj_set_style_text_color(s_appr_hint, lv_color_hex(COL_MUTED), 0);
        vis(s_appr_hint, true);
    }
    snprintf(s_bar_detail, sizeof(s_bar_detail), "%s", allow ? "已发送给电脑" : "已拒绝");
    if (s_detail) {
        lv_label_set_text(s_detail, s_bar_detail);
    }
    layout_actions();
}

static void on_approve(lv_event_t *e)
{
    (void)e;
    finish_approval(true);
}

static void on_deny(lv_event_t *e)
{
    (void)e;
    finish_approval(false);
}

static void on_cmd_toggle(lv_event_t *e)
{
    (void)e;
    if (!s_appr_cmd_text || !s_appr_cmd_btn) {
        return;
    }
    bool open = lv_obj_has_flag(s_appr_cmd_text, LV_OBJ_FLAG_HIDDEN);
    vis(s_appr_cmd_text, open);
    lv_label_set_text(lv_obj_get_child(s_appr_cmd_btn, 0), open ? "收起命令" : "查看命令");
}

static void refresh_code(void)
{
    char shown[16];
    size_t n = strlen(s_code);
    if (n == 0) {
        strcpy(shown, "--- ---");
    } else if (n <= 3) {
        snprintf(shown, sizeof(shown), "%s", s_code);
    } else {
        snprintf(shown, sizeof(shown), "%c%c%c %s", s_code[0], s_code[1], s_code[2], s_code + 3);
    }
    if (s_code_label) {
        lv_label_set_text(s_code_label, shown);
    }
}

static char s_keys[12];

/* LVGL waits forever for the SPI interrupt to finish a strip. If that
 * interrupt does not arrive, the cat and the last sentence stay on the
 * glass and later replies never show. Give the interrupt a short window,
 * then let the screen task continue. */
static void pager_flush_wait(lv_display_t *disp)
{
    uint32_t t0 = lv_tick_get();
    while (disp->flushing && lv_tick_elaps(t0) < 40) {
    }
    if (disp->flushing) {
        ESP_LOGW("pager_ui", "flush wait timeout");
    }
}

static void on_key(lv_event_t *e)
{
    char key = *(const char *)lv_event_get_user_data(e);
    size_t n = strlen(s_code);
    if (key == 'C') {
        s_code[0] = 0;
    } else if (key == 'K') {
        if (n == 6) {
            pager_ui_submit_code(s_code);
            lv_obj_t *lab = (s_pair_hint && lv_obj_is_valid(s_pair_hint)) ? s_pair_hint : s_hint;
            if (lab) {
                lv_label_set_text(lab, "正在配对…");
            }
        }
    } else if (n < 6) {
        s_code[n] = key;
        s_code[n + 1] = 0;
    }
    refresh_code();
}

void ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_set_flush_wait_cb(disp, pager_flush_wait);
    }
    ui_dispatch_begin();
    if (!lock()) {
        return;
    }
    layout_sync();
    /* Paint the real home first, while internal heap is still ~78KB
     * and a 480x50 QSPI strip can allocate. Later audio must not
     * rebuild this tree. */
    build_shell();
    unlock();
}

static void vis_home_chrome(bool on)
{
    vis(s_big_box, on);
    vis(s_wait_title, on);
    vis(s_wait_sub, on);
    vis(s_nav_prov, on);
    vis(s_nav_pair, on);
    if (s_cat_timer) {
        if (on) {
            lv_timer_resume(s_cat_timer);
        } else {
            lv_timer_pause(s_cat_timer);
        }
    }
    if (!on) {
        ui_set_net_line(NULL);
    }
}

static void scene_flush_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (s_ui_scene == UI_SCENE_HOME && s_page == 0 && !settings_open()) {
        pager_audio_resume();
    }
}

static void ui_schedule_flush(void)
{
    if (!s_scene_flush_timer) {
        s_scene_flush_timer = lv_timer_create(scene_flush_cb, 120, NULL);
    } else {
        lv_timer_reset(s_scene_flush_timer);
        lv_timer_resume(s_scene_flush_timer);
    }
}

static void ui_apply_scene(int scene)
{
    s_ui_scene = scene;
    /* Keep the already-painted home canvas. Full hide/show of s_wait
     * dirties 480x480 and the QSPI flush then fails (ESP_ERR_NO_MEM),
     * which is the leftover cat / 配网 pixels on the pair keypad. */
    vis(s_wait, true);
    vis_home_chrome(scene == UI_SCENE_HOME);
    vis(s_prov_page, scene == UI_SCENE_PROV);
    vis(s_pair_page, scene == UI_SCENE_PAIR);
    vis(s_wifi_page, scene == UI_SCENE_WIFI);
    if (s_settings && lv_obj_is_valid(s_settings)) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
    if (scene == UI_SCENE_HOME) {
        s_block_wifi = true;
        pager_audio_suspend();
    } else {
        pager_audio_suspend();
        vis(s_body, false);
        vis(s_stats_body, false);
        vis(s_appr_body, false);
        vis(s_dots, false);
    }
    lv_obj_t *front = NULL;
    if (scene == UI_SCENE_PROV) {
        front = s_prov_page;
    } else if (scene == UI_SCENE_PAIR) {
        front = s_pair_page;
    } else if (scene == UI_SCENE_WIFI) {
        front = s_wifi_page;
    } else if (scene == UI_SCENE_HOME) {
        front = s_wait;
    }
    if (front && lv_obj_is_valid(front)) {
        lv_obj_move_foreground(front);
        lv_obj_invalidate(front);
    }
    ui_schedule_flush();
}

static void on_back_home(lv_event_t *e)
{
    (void)e;
    pager_ui_leave_pair();
    ui_apply_scene(UI_SCENE_HOME);
    ui_refresh_now();
}

static void on_home_prov(lv_event_t *e)
{
    (void)e;
    ui_apply_scene(UI_SCENE_PROV);
    ui_refresh_now();
}

static void on_home_pair(lv_event_t *e)
{
    (void)e;
    s_code[0] = 0;
    if (s_code_label && lv_obj_is_valid(s_code_label)) {
        lv_label_set_text(s_code_label, "--- ---");
    }
    if (s_pair_host && lv_obj_is_valid(s_pair_host)) {
        lv_label_set_text(s_pair_host, "正在寻找电脑");
    }
    if (s_pair_hint && lv_obj_is_valid(s_pair_hint)) {
        lv_label_set_text(s_pair_hint, "电脑网页点生成配对码，再输入 6 位");
    }
    ui_apply_scene(UI_SCENE_PAIR);
    ui_refresh_now();
    pager_ui_enter_pair();
}

static void on_settings_prov(lv_event_t *e)
{
    if (s_settings) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
    on_home_prov(e);
}

static void on_settings_pair(lv_event_t *e)
{
    if (s_settings) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
    on_home_pair(e);
}

static void on_do_provision(lv_event_t *e)
{
    (void)e;
    pager_ui_reset_wifi();
}

typedef struct {
    char ap[64];
} ui_provision_args_t;

static void ui_show_provision_body(void *p)
{
    const char *ap_name = ((ui_provision_args_t *)p)->ap;
    if (s_prov_page && lv_obj_is_valid(s_prov_page)) {
        if (s_prov_name && lv_obj_is_valid(s_prov_name)) {
            lv_label_set_text(s_prov_name, (ap_name && ap_name[0]) ? ap_name : "Pager");
        }
        ui_apply_scene(UI_SCENE_PROV);
        ui_log_heap("provision-page");
        ui_refresh_now();
        return;
    }
    layout_sync();
    lv_obj_t *scr = screen();
    const bool wide = ui_wide();

    lv_coord_t top = ui_top();
    lv_obj_t *title = text_font(scr, "配网", COL_ORANGE, ui_font_24());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, top);

    lv_obj_t *sub = text(scr, "手机连这个热点后打开地址", COL_MUTED);
    lv_obj_set_width(sub, UI_W - 48);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, top + (wide ? 36 : 28));

    lv_coord_t card_y = top + (wide ? 76 : 56);
    lv_coord_t card_h = wide ? 200 : 150;
    lv_obj_t *card = panel(scr, CARD_X, card_y, CARD_W, card_h, COL_CARD, COL_LINE, 22);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_opa(card, 90, 0);

    lv_obj_t *hint = text(card, "热点名称", COL_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, wide ? 22 : 14);

    lv_obj_t *name = text_font(card, ap_name ? ap_name : "Pager", COL_TEXT, ui_font_24());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, CARD_W - 40);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, wide ? 52 : 38);

    lv_obj_t *url = text_font(card, "192.168.4.1", COL_ORANGE, &lv_font_montserrat_28);
    lv_obj_align(url, LV_ALIGN_TOP_MID, 0, wide ? 104 : 76);

    lv_obj_t *note = text(card, "浏览器打开上面的地址", COL_MUTED);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, wide ? 154 : 116);

    lv_obj_t *foot = text(scr, "只支持 2.4GHz · 配网后自动连接", COL_MUTED);
    lv_obj_set_width(foot, UI_W - 48);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, wide ? -28 : -18);
    s_ui_scene = UI_SCENE_PROV;
    ui_log_heap("provision");
    ui_refresh_now();
}

static void ui_show_provision_cb(void *p)
{
    ui_show_provision_body(p);
}

void ui_show_provision(const char *ap_name)
{
    ui_provision_args_t args = {0};
    if (ap_name) {
        strncpy(args.ap, ap_name, sizeof(args.ap) - 1);
    }
    ui_dispatch(ui_show_provision_cb, &args, sizeof(args));
}

typedef struct {
    char status[256];
} ui_wifi_args_t;

static void ui_set_net_line(const char *text)
{
    if (!s_net_line || !lv_obj_is_valid(s_net_line)) {
        return;
    }
    if (text && text[0]) {
        lv_label_set_text(s_net_line, text);
        vis(s_net_line, true);
    } else {
        lv_label_set_text(s_net_line, "");
        vis(s_net_line, false);
    }
}

static void ui_show_wifi_body(void *p)
{
    const char *status = ((ui_wifi_args_t *)p)->status;
    if (s_wifi_page && lv_obj_is_valid(s_wifi_page) && !s_block_wifi) {
        if (s_wifi_status && lv_obj_is_valid(s_wifi_status)) {
            lv_label_set_text(s_wifi_status, status ? status : "");
        }
        ui_apply_scene(UI_SCENE_WIFI);
        ui_log_heap("wifi-page");
        ui_refresh_now();
        return;
    }
    /* Home already painted: only refresh the small net line. Never
     * rebuild the old WiFi card after boot — that leftover chrome is
     * what the panel still showed. */
    if (s_shell && s_wait && lv_obj_is_valid(s_wait) && s_block_wifi) {
        if (s_ui_scene == UI_SCENE_HOME) {
            ui_set_net_line(status);
            ui_log_heap("wifi-line");
            ui_refresh_now();
        }
        return;
    }
    if (s_block_wifi) {
        ESP_LOGW("pager_ui", "block wifi after home: %s", status ? status : "");
        return;
    }
    if (s_ui_scene == UI_SCENE_WIFI && s_hint && lv_obj_is_valid(s_hint)) {
        lv_label_set_text(s_hint, status ? status : "");
        ui_log_heap("wifi-inplace");
        ui_refresh_now();
        return;
    }
    layout_sync();
    lv_obj_t *scr = screen();
    const bool wide = ui_wide();

    lv_coord_t top = ui_top();
    s_wifi_title = text_font(scr, "Wi-Fi", COL_ORANGE, ui_font_24());
    lv_obj_align(s_wifi_title, LV_ALIGN_TOP_MID, 0, top);
    s_wifi_sub = text(scr, "连不上时可以重新配网", COL_MUTED);
    lv_obj_set_width(s_wifi_sub, UI_W - 48);
    lv_obj_set_style_text_align(s_wifi_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_sub, LV_ALIGN_TOP_MID, 0, top + (wide ? 36 : 28));

    lv_coord_t card_y = top + (wide ? 80 : 56);
    lv_coord_t card_h = wide ? 140 : 110;
    lv_obj_t *card = panel(scr, CARD_X, card_y, CARD_W, card_h, COL_CARD, COL_LINE, 22);
    s_hint = text(card, status ? status : "", COL_TEXT);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, CARD_W - 40);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 0);

    s_wifi_btn = button(scr, "重新配网", COL_ORANGE, on_reset_wifi, NULL);
    lv_obj_set_size(s_wifi_btn, CARD_W, wide ? 52 : 42);
    lv_obj_align(s_wifi_btn, LV_ALIGN_BOTTOM_MID, 0, wide ? -28 : -18);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wifi_btn, 0), lv_color_hex(0x1D0B05), 0);
    s_ui_scene = UI_SCENE_WIFI;
    ui_log_heap("wifi");
    ui_refresh_now();
}

static void ui_show_wifi_cb(void *p)
{
    ui_show_wifi_body(p);
}

void ui_show_wifi(const char *status)
{
    ui_wifi_args_t args = {0};
    if (status) {
        strncpy(args.status, status, sizeof(args.status) - 1);
    }
    ui_dispatch(ui_show_wifi_cb, &args, sizeof(args));
}

void ui_unblock_wifi(void)
{
    s_block_wifi = false;
}

typedef struct {
    char host[64];
} ui_pair_args_t;

static void ui_show_pair_body(void *p)
{
    const char *host = ((ui_pair_args_t *)p)->host;
    if (s_pair_page && lv_obj_is_valid(s_pair_page)) {
        char host_line[80];
        if (host && host[0]) {
            snprintf(host_line, sizeof(host_line), "电脑 %s", host);
        } else {
            snprintf(host_line, sizeof(host_line), "正在寻找电脑");
        }
        if (s_pair_host && lv_obj_is_valid(s_pair_host)) {
            lv_label_set_text(s_pair_host, host_line);
        }
        if (!s_code[0] && s_code_label && lv_obj_is_valid(s_code_label)) {
            lv_label_set_text(s_code_label, "--- ---");
        }
        ui_apply_scene(UI_SCENE_PAIR);
        ui_log_heap("pair-page");
        ui_refresh_now();
        return;
    }
    layout_sync();
    s_code[0] = 0;
    lv_obj_t *scr = screen();
    const bool wide = ui_wide();

    lv_coord_t top = ui_top();
    lv_obj_t *title = text_font(scr, "配对", COL_ORANGE, ui_font_24());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, top);

    char host_line[80];
    if (host && host[0]) {
        snprintf(host_line, sizeof(host_line), "电脑 %s", host);
    } else {
        snprintf(host_line, sizeof(host_line), "正在寻找电脑");
    }
    lv_obj_t *host_label = text(scr, host_line, COL_MUTED);
    lv_label_set_long_mode(host_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(host_label, UI_W - (wide ? 140 : 96));
    lv_obj_align(host_label, LV_ALIGN_TOP_LEFT, 20, top + (wide ? 36 : 28));

    lv_obj_t *retry = button(scr, "重找", COL_CARD, on_retry, NULL);
    lv_obj_set_size(retry, wide ? 88 : 60, wide ? 40 : 28);
    lv_obj_align(retry, LV_ALIGN_TOP_RIGHT, -16, top);
    lv_obj_set_style_border_color(retry, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(retry, 1, 0);

    s_code_label = text_font(scr, "--- ---", COL_TEXT, &lv_font_montserrat_40);
    lv_obj_set_style_text_letter_space(s_code_label, 2, 0);
    lv_obj_align(s_code_label, LV_ALIGN_TOP_MID, 0, top + (wide ? 68 : 64));

    s_hint = text(scr, "输入电脑上的 6 位码", COL_MUTED);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, UI_W - 40);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, top + (wide ? 118 : 108));

    static const char *labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "删除", "0", "确定"};
    memcpy(s_keys, "123456789C0K", 12);
    lv_coord_t pad = wide ? 20 : 14;
    lv_coord_t gap = wide ? 10 : 10;
    lv_coord_t key_w = (UI_W - 2 * pad - 2 * gap) / 3;
    lv_coord_t key_h = wide ? 46 : 32;
    lv_coord_t grid_top = wide ? 196 : 152;
    for (int i = 0; i < 12; i++) {
        uint32_t bg = (s_keys[i] == 'K') ? COL_ORANGE : COL_CARD;
        lv_obj_t *key = button(scr, labels[i], bg, on_key, &s_keys[i]);
        lv_obj_set_size(key, key_w, key_h);
        lv_obj_set_pos(key, pad + (i % 3) * (key_w + gap), grid_top + (i / 3) * (key_h + gap));
        if (s_keys[i] == 'K') {
            lv_obj_set_style_text_color(lv_obj_get_child(key, 0), lv_color_hex(0x1D0B05), 0);
        } else {
            lv_obj_set_style_border_color(key, lv_color_hex(COL_LINE_SOFT), 0);
            lv_obj_set_style_border_width(key, 1, 0);
        }
    }
    s_ui_scene = UI_SCENE_PAIR;
    ui_log_heap("pair");
    ui_refresh_now();
}

static void ui_show_pair_cb(void *p)
{
    ui_show_pair_body(p);
}

void ui_show_pair(const char *host)
{
    ui_pair_args_t args = {0};
    if (host) {
        strncpy(args.host, host, sizeof(args.host) - 1);
    }
    ui_dispatch(ui_show_pair_cb, &args, sizeof(args));
}

typedef struct {
    char text[128];
} ui_pair_status_args_t;

static void ui_pair_status_body(void *p)
{
    const char *text = ((ui_pair_status_args_t *)p)->text;
    lv_obj_t *lab = (s_pair_hint && lv_obj_is_valid(s_pair_hint)) ? s_pair_hint : s_hint;
    if (lab) {
        lv_label_set_text(lab, text ? text : "");
    }
}

static void ui_pair_status_cb(void *p)
{
    ui_pair_status_body(p);
}

void ui_pair_status(const char *text)
{
    ui_pair_status_args_t args = {0};
    if (text) {
        strncpy(args.text, text, sizeof(args.text) - 1);
    }
    ui_dispatch(ui_pair_status_cb, &args, sizeof(args));
}

static bool settings_open(void)
{
    return s_settings && !lv_obj_has_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

static void vis(lv_obj_t *obj, bool on)
{
    if (!obj) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static int utf8_next(const char *s, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80) {
        *cp = u[0];
        return u[0] ? 1 : 0;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
              ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    *cp = 0;
    return 1;
}

static bool glyph_ok(uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    if (cp == '\n' || cp == ' ') {
        return true;
    }
    return lv_font_get_glyph_dsc(ui_font(), &dsc, cp, 0);
}

static bool technical_line(const char *s, const char *end)
{
    while (s < end && (*s == ' ' || *s == '\t' || *s == '>' || *s == '#')) {
        s++;
    }
    if (s >= end || *s == '$' || *s == '`') {
        return s < end;
    }
    char tmp[160];
    size_t n = (size_t)(end - s);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, s, n);
    tmp[n] = 0;
    if (strstr(tmp, "powershell") || strstr(tmp, ".exe") || strstr(tmp, ":\\") || strstr(tmp, "cmd.exe")) {
        return true;
    }
    int other = 0;
    int slash = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)tmp[i];
        if (c == '/' || c == '\\') {
            slash++;
        }
        if (c >= 0x80) {
            other++;
        }
    }
    return other == 0 && n > 18 && slash > 0;
}

static void clean_reply(char *out, size_t cap, const char *src)
{
    size_t o = 0;
    bool fence = false;
    out[0] = 0;
    if (!src || cap < 8) {
        return;
    }
    for (const char *p = src; *p && o + 4 < cap; ) {
        if (p[0] == '`' && p[1] == '`' && p[2] == '`') {
            fence = !fence;
            p += 3;
            if (o > 0 && out[o - 1] != '\n' && o + 1 < cap) {
                out[o++] = '\n';
            }
            continue;
        }
        if (*p == '\r' || *p == '`') {
            p++;
            continue;
        }
        uint32_t cp = 0;
        int adv = utf8_next(p, &cp);
        if (adv <= 0) {
            break;
        }
        if (glyph_ok(cp)) {
            if (o + (size_t)adv < cap) {
                memcpy(out + o, p, (size_t)adv);
                o += (size_t)adv;
            }
        }
        p += adv;
        (void)fence;
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) {
        o--;
    }
    out[o] = 0;
}

static uint32_t kind_color(int kind)
{
    if (kind == 1) {
        return COL_ORANGE;
    }
    if (kind == 2 || kind == 4) {
        return COL_MINT;
    }
    if (kind == 3) {
        return COL_AMBER;
    }
    if (kind == 5) {
        return COL_DANGER;
    }
    return 0x686F70;
}

static lv_obj_t *cat_shape(lv_obj_t *parent, int w, int h, int x, int y, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void paint_cat(int kind)
{
    uint32_t tone = kind_color(kind);
    if (kind == 0) {
        tone = 0x6E7576;
    }
    if (s_face_ring) {
        bool show_ring = kind != 0 && kind != 4;
        vis(s_face_ring, show_ring);
        lv_obj_set_style_border_color(s_face_ring, lv_color_hex(tone), 0);
        lv_obj_set_style_shadow_color(s_face_ring, lv_color_hex(tone), 0);
        lv_obj_set_style_shadow_width(s_face_ring, show_ring ? 12 : 0, 0);
        lv_obj_set_style_shadow_opa(s_face_ring, show_ring ? 40 : 0, 0);
    }
    if (s_cat_head) {
        lv_obj_set_style_border_color(s_cat_head, lv_color_hex(tone), 0);
    }
    if (s_cat_ear_l) {
        lv_obj_set_style_border_color(s_cat_ear_l, lv_color_hex(tone), 0);
        lv_obj_set_style_border_color(s_cat_ear_r, lv_color_hex(tone), 0);
    }
    if (s_big_l) {
        lv_obj_set_style_border_color(s_big_l, lv_color_hex(tone), 0);
        lv_obj_set_style_border_color(s_big_r, lv_color_hex(tone), 0);
    }
    if (s_cat_pupil_l) {
        lv_obj_set_style_bg_color(s_cat_pupil_l, lv_color_hex(tone), 0);
        lv_obj_set_style_bg_color(s_cat_pupil_r, lv_color_hex(tone), 0);
    }
    if (s_big_m) {
        lv_obj_set_style_bg_color(s_big_m, lv_color_hex(tone), 0);
    }
    if (s_cat_tail) {
        lv_obj_set_style_border_color(s_cat_tail, lv_color_hex(tone), 0);
    }
    if (s_mini_head) {
        lv_obj_set_style_border_color(s_mini_head, lv_color_hex(tone), 0);
        lv_obj_set_style_border_color(s_mini_ear_l, lv_color_hex(tone), 0);
        lv_obj_set_style_border_color(s_mini_ear_r, lv_color_hex(tone), 0);
    }
    if (s_mini_eye_l) {
        lv_obj_set_style_bg_color(s_mini_eye_l, lv_color_hex(tone), 0);
        lv_obj_set_style_bg_color(s_mini_eye_r, lv_color_hex(tone), 0);
    }
    if (s_cat_ear_l) {
        int droop = kind == 5 ? 4 : 0;
        lv_obj_set_style_translate_y(s_cat_ear_l, droop, 0);
        lv_obj_set_style_translate_y(s_cat_ear_r, droop, 0);
    }
}

static void cat_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_big_box || !s_big_l || !s_big_r || lv_obj_has_flag(s_big_box, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    s_cat_tick++;
    int bob = 0;
    if (s_kind == 1 || s_kind == 2) {
        static const int wave[8] = {0, -1, -2, -3, -3, -2, -1, 0};
        bob = wave[(s_cat_tick / 2) & 7];
    } else if (s_kind == 3) {
        bob = (s_cat_tick & 4) ? -2 : 0;
    }
    static int last_bob = -999;
    static int last_eh = -1;
    static int last_pupil = -1;
    static int last_dx = -999;
    static int last_hop = -999;
    if (bob != last_bob) {
        last_bob = bob;
        lv_obj_set_style_translate_y(s_big_box, bob, 0);
    }

    bool blink = (s_cat_tick % 12) == 0;
    bool sleepy = s_kind == 0 || s_kind == 4;
    int eh = blink ? cat_s(2) : (sleepy ? cat_s(7) : cat_s(12));
    if (eh != last_eh) {
        last_eh = eh;
        lv_obj_set_height(s_big_l, eh);
        lv_obj_set_height(s_big_r, eh);
    }
    int pupil = (blink || sleepy) ? 0 : 1;
    if (s_cat_pupil_l && pupil != last_pupil) {
        last_pupil = pupil;
        lv_opa_t opa = pupil ? LV_OPA_COVER : LV_OPA_TRANSP;
        lv_obj_set_style_bg_opa(s_cat_pupil_l, opa, 0);
        lv_obj_set_style_bg_opa(s_cat_pupil_r, opa, 0);
    }
    if (s_cat_pupil_l && s_kind == 1) {
        int dx = ((s_cat_tick / 2) % 3) - 1;
        if (dx != last_dx) {
            last_dx = dx;
            lv_obj_set_x(s_cat_pupil_l, cat_s(3) + dx);
            lv_obj_set_x(s_cat_pupil_r, cat_s(3) + dx);
        }
    }
    if (s_kind == 1 && s_cat_ear_l) {
        int hop = (s_cat_tick % 8) == 0 ? -3 : 0;
        if (hop != last_hop) {
            last_hop = hop;
            lv_obj_set_style_translate_y(s_cat_ear_l, hop, 0);
        }
    }
}

static void make_cat(lv_obj_t *parent)
{
    s_big_box = lv_obj_create(parent);
    lv_obj_set_size(s_big_box, cat_s(96), cat_s(88));
    lv_obj_set_style_bg_opa(s_big_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_big_box, 0, 0);
    lv_obj_set_style_pad_all(s_big_box, 0, 0);
    lv_obj_clear_flag(s_big_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_big_box, LV_ALIGN_TOP_MID, 0, ui_wide() ? (ui_top() + 8) : 34);

    s_cat_tail = lv_obj_create(s_big_box);
    lv_obj_set_size(s_cat_tail, cat_s(28), cat_s(26));
    lv_obj_set_pos(s_cat_tail, cat_s(4), cat_s(50));
    lv_obj_set_style_radius(s_cat_tail, cat_s(14), 0);
    lv_obj_set_style_bg_opa(s_cat_tail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cat_tail, ui_wide() ? 4 : 3, 0);
    lv_obj_set_style_border_side(s_cat_tail, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_opa(s_cat_tail, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_cat_tail, 0, 0);
    lv_obj_clear_flag(s_cat_tail, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_cat_ear_l = cat_shape(s_big_box, cat_s(22), cat_s(28), cat_s(16), cat_s(8), cat_s(12));
    s_cat_ear_r = cat_shape(s_big_box, cat_s(22), cat_s(28), cat_s(58), cat_s(8), cat_s(12));
    lv_obj_set_style_bg_color(s_cat_ear_l, lv_color_hex(0x2A3133), 0);
    lv_obj_set_style_bg_color(s_cat_ear_r, lv_color_hex(0x2A3133), 0);
    lv_obj_set_style_border_width(s_cat_ear_l, ui_wide() ? 3 : 2, 0);
    lv_obj_set_style_border_width(s_cat_ear_r, ui_wide() ? 3 : 2, 0);
    s_cat_in_l = cat_shape(s_cat_ear_l, cat_s(10), cat_s(14), cat_s(6), cat_s(6), cat_s(6));
    s_cat_in_r = cat_shape(s_cat_ear_r, cat_s(10), cat_s(14), cat_s(6), cat_s(6), cat_s(6));
    lv_obj_set_style_bg_color(s_cat_in_l, lv_color_hex(0xD39278), 0);
    lv_obj_set_style_bg_color(s_cat_in_r, lv_color_hex(0xD39278), 0);

    s_cat_head = cat_shape(s_big_box, cat_s(60), cat_s(54), cat_s(18), cat_s(28), cat_s(26));
    lv_obj_set_style_bg_color(s_cat_head, lv_color_hex(0x2A3133), 0);
    lv_obj_set_style_border_width(s_cat_head, ui_wide() ? 3 : 2, 0);

    s_big_l = cat_shape(s_cat_head, cat_s(12), cat_s(12), cat_s(10), cat_s(16), LV_RADIUS_CIRCLE);
    s_big_r = cat_shape(s_cat_head, cat_s(12), cat_s(12), cat_s(36), cat_s(16), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(s_big_l, lv_color_hex(0x0C0E10), 0);
    lv_obj_set_style_bg_color(s_big_r, lv_color_hex(0x0C0E10), 0);
    lv_obj_set_style_border_width(s_big_l, 2, 0);
    lv_obj_set_style_border_width(s_big_r, 2, 0);
    s_cat_pupil_l = cat_shape(s_big_l, cat_s(4), cat_s(7), cat_s(3), cat_s(2), LV_RADIUS_CIRCLE);
    s_cat_pupil_r = cat_shape(s_big_r, cat_s(4), cat_s(7), cat_s(3), cat_s(2), LV_RADIUS_CIRCLE);

    s_big_m = cat_shape(s_cat_head, cat_s(8), cat_s(5), cat_s(26), cat_s(34), 2);

    if (s_cat_timer) {
        lv_timer_del(s_cat_timer);
    }
    s_cat_tick = 0;
    s_cat_timer = lv_timer_create(cat_timer_cb, 400, NULL);
}

static void make_mini_cat(lv_obj_t *parent)
{
    s_mini = lv_obj_create(parent);
    lv_obj_set_size(s_mini, 16, 14);
    lv_obj_set_style_bg_opa(s_mini, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mini, 0, 0);
    lv_obj_set_style_pad_all(s_mini, 0, 0);
    lv_obj_clear_flag(s_mini, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_mini, LV_ALIGN_LEFT_MID, 9, 0);

    s_mini_ear_l = cat_shape(s_mini, 6, 6, 1, 0, 3);
    s_mini_ear_r = cat_shape(s_mini, 6, 6, 9, 0, 3);
    lv_obj_set_style_bg_color(s_mini_ear_l, lv_color_hex(0x171C1E), 0);
    lv_obj_set_style_bg_color(s_mini_ear_r, lv_color_hex(0x171C1E), 0);
    lv_obj_set_style_border_width(s_mini_ear_l, 1, 0);
    lv_obj_set_style_border_width(s_mini_ear_r, 1, 0);
    lv_obj_set_style_border_color(s_mini_ear_l, lv_color_hex(0x6E7576), 0);
    lv_obj_set_style_border_color(s_mini_ear_r, lv_color_hex(0x6E7576), 0);

    s_mini_head = cat_shape(s_mini, 14, 9, 1, 5, 6);
    lv_obj_set_style_bg_color(s_mini_head, lv_color_hex(0x14181A), 0);
    lv_obj_set_style_border_width(s_mini_head, 1, 0);
    lv_obj_set_style_border_color(s_mini_head, lv_color_hex(0x6E7576), 0);

    s_mini_eye_l = cat_shape(s_mini_head, 2, 2, 3, 3, LV_RADIUS_CIRCLE);
    s_mini_eye_r = cat_shape(s_mini_head, 2, 2, 9, 3, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(s_mini_eye_l, lv_color_hex(0x6E7576), 0);
    lv_obj_set_style_bg_color(s_mini_eye_r, lv_color_hex(0x6E7576), 0);
}

static void paint_one_face(lv_obj_t *box, lv_obj_t *l, lv_obj_t *r, lv_obj_t *m, int kind)
{
    if (!box) {
        return;
    }
    if (box == s_big_box) {
        paint_cat(kind);
        return;
    }
    uint32_t c = kind_color(kind);
    lv_obj_set_style_border_color(box, lv_color_hex(kind == 0 ? 0x3D4547 : c), 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(c), 0);
}

#define HOST_PAD 4
#define HOST_GAP 4
#define HOST_RING_SZ 72
#define HOST_FOOT_H 34

static uint32_t host_ring_color(float pct)
{
    if (pct < 0) {
        return COL_LINE;
    }
    if (pct >= 90) {
        return COL_DANGER;
    }
    if (pct >= 70) {
        return COL_AMBER;
    }
    return COL_ORANGE;
}

#define HOST_ARC_START 135
#define HOST_ARC_END 45

static void host_style_arc_track(lv_obj_t *arc)
{
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, HOST_ARC_START, HOST_ARC_END);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x3A4246), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_INDICATOR);
}

static void host_style_arc(lv_obj_t *arc, uint32_t color, int width, lv_opa_t opa)
{
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, HOST_ARC_START, HOST_ARC_END);
    lv_arc_set_value(arc, 0);
    lv_arc_set_change_rate(arc, 42);
    lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc, opa, LV_PART_INDICATOR);
}

static void host_ring_apply_color(host_ring_t *ring, float pct)
{
    if (!ring || !ring->arc) {
        return;
    }
    uint32_t c = host_ring_color(pct);
    ring->hot = pct >= 85;
    lv_obj_set_style_arc_color(ring->arc, lv_color_hex(c), LV_PART_INDICATOR);
    if (ring->arc_glow) {
        lv_obj_set_style_arc_color(ring->arc_glow, lv_color_hex(c), LV_PART_INDICATOR);
        lv_obj_set_style_opa(ring->arc_glow, ring->hot ? LV_OPA_40 : LV_OPA_30,
                             LV_PART_INDICATOR);
    }
}

static void host_ring_set_pct(host_ring_t *ring, float pct, const char *sub)
{
    if (!ring || !ring->arc || !ring->pct) {
        return;
    }
    char buf[16];
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%.0f%%", pct);
        lv_label_set_text(ring->pct, buf);
    } else {
        lv_label_set_text(ring->pct, "--");
    }
    int v = 0;
    if (pct >= 0) {
        v = (int)(pct + 0.5f);
        if (v > 100) {
            v = 100;
        }
    }
    lv_arc_set_value(ring->arc, v);
    if (ring->arc_glow) {
        lv_arc_set_value(ring->arc_glow, v);
    }
    host_ring_apply_color(ring, pct);
    if (ring->sub) {
        lv_label_set_text(ring->sub, sub ? sub : "");
    }
}

static void host_fx_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_page != -1) {
        return;
    }
    static uint8_t phase;
    phase = (phase + 1) % 24;
    lv_opa_t pulse = (lv_opa_t)(50 + (phase < 12 ? phase * 6 : (24 - phase) * 6));
    host_ring_t *rings[] = {&s_host_cpu, &s_host_ram, &s_host_gpu, &s_host_net};
    for (int i = 0; i < 4; i++) {
        host_ring_t *ring = rings[i];
        /* Only hot rings breathe. Writing the style on idle rings every
         * tick invalidated them forever and pinned the render loop. */
        if (!ring->arc_glow || !ring->hot) {
            continue;
        }
        lv_obj_set_style_opa(ring->arc_glow, pulse, LV_PART_INDICATOR);
    }
}

static void build_host_ring(lv_obj_t *parent, int x, int y, int w, int h, const char *label,
                            host_ring_t *ring)
{
    ring->card = panel(parent, x, y, w, h, COL_CARD, COL_LINE, 14);
    lv_obj_clear_flag(ring->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ring->card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_grad_color(ring->card, lv_color_hex(0x0E1011), 0);
    lv_obj_set_style_bg_grad_dir(ring->card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ring->card, LV_OPA_COVER, 0);

    /* Scale the ring to the cell. The old fixed 72px ring was sized for the
     * narrow preview; on the 480-wide watch the cells are ~214x176. */
    int arc_sz = w - 24;
    if (arc_sz > h - HOST_FOOT_H - 14) {
        arc_sz = h - HOST_FOOT_H - 14;
    }
    if (arc_sz < 48) {
        arc_sz = 48;
    }
    const int track_w = arc_sz / 22 + 2;
    const int fill_w = track_w + 2;
    const int glow_w = fill_w + 4;

    ring->ring_box = lv_obj_create(ring->card);
    lv_obj_set_size(ring->ring_box, arc_sz, arc_sz);
    lv_obj_clear_flag(ring->ring_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ring->ring_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring->ring_box, 0, 0);
    lv_obj_set_style_pad_all(ring->ring_box, 0, 0);
    lv_obj_align(ring->ring_box, LV_ALIGN_TOP_MID, 0, 8);

    ring->arc_bg = lv_arc_create(ring->ring_box);
    lv_obj_set_size(ring->arc_bg, arc_sz, arc_sz);
    host_style_arc_track(ring->arc_bg);
    lv_obj_set_style_arc_width(ring->arc_bg, track_w, LV_PART_MAIN);
    lv_obj_center(ring->arc_bg);

    ring->arc_glow = lv_arc_create(ring->ring_box);
    lv_obj_set_size(ring->arc_glow, arc_sz, arc_sz);
    host_style_arc(ring->arc_glow, COL_ORANGE, glow_w, LV_OPA_30);
    lv_obj_center(ring->arc_glow);

    ring->arc = lv_arc_create(ring->ring_box);
    lv_obj_set_size(ring->arc, arc_sz, arc_sz);
    host_style_arc(ring->arc, COL_ORANGE, fill_w, LV_OPA_COVER);
    /* No shadow blur on the indicator: the pulse animation would force a
     * full blur re-render every step and pin the screen task. */
    lv_obj_set_style_shadow_width(ring->arc, 0, LV_PART_INDICATOR);
    lv_obj_center(ring->arc);

    ring->pct = text_font(ring->ring_box, "--", COL_TEXT,
                          ui_wide() ? &lv_font_montserrat_28 : FONT_RING_PCT);
    lv_obj_align(ring->pct, LV_ALIGN_CENTER, 0, -2);

    ring->title = text(ring->card, label, COL_TEXT_DIM);
    lv_obj_set_style_text_align(ring->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ring->title, w - 8);
    lv_obj_align(ring->title, LV_ALIGN_BOTTOM_MID, 0, -32);

    ring->sub = text(ring->card, "", COL_MUTED);
    lv_label_set_long_mode(ring->sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ring->sub, w - 12);
    lv_obj_set_style_text_align(ring->sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ring->sub, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void paint_dots(void)
{
    if (!s_dot0 || !s_dot1 || !s_dot2) {
        return;
    }
    bool stats = s_page == -1;
    bool home = s_page == 0;
    bool appr = s_page == 1;
    lv_obj_set_size(s_dot0, stats ? 11 : 4, 4);
    lv_obj_set_size(s_dot1, home ? 11 : 4, 4);
    lv_obj_set_size(s_dot2, appr ? 11 : 4, 4);
    lv_obj_set_style_bg_color(s_dot0, lv_color_hex(stats ? COL_ORANGE : 0x303638), 0);
    lv_obj_set_style_bg_color(s_dot1, lv_color_hex(home ? COL_ORANGE : 0x303638), 0);
    lv_obj_set_style_bg_color(s_dot2, lv_color_hex(appr ? COL_ORANGE : 0x303638), 0);
    lv_obj_align(s_dot0, LV_ALIGN_CENTER, -18, 0);
    lv_obj_align(s_dot1, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(s_dot2, LV_ALIGN_CENTER, 18, 0);
}

static const char *state_word(const char *state, int kind)
{
    if (state && strcmp(state, "BOOTING") == 0) {
        return "启动中";
    }
    if (kind == 1) {
        return "思考中";
    }
    if (kind == 2) {
        return "执行工具";
    }
    if (kind == 3) {
        return "等待批准";
    }
    if (kind == 4) {
        return "完成";
    }
    if (kind == 5) {
        return (state && strcmp(state, "OFFLINE") == 0) ? "离线" : "错误";
    }
    return "就绪";
}

static bool home_shows_words(void)
{
    return s_ui_scene == UI_SCENE_HOME && s_page == 0 && !settings_open() && s_listen == 0 &&
           s_has_text && s_said_buf && s_said_buf[0];
}

static void layout_home_words(void)
{
    bool home = s_page == 0 && s_ui_scene == UI_SCENE_HOME && !settings_open();
    bool words = home_shows_words();
    vis(s_said_box, words);
    if (!home) {
        return;
    }
    /* The face stays on the home page. The reply is drawn under it.
     * Covering the face with a full-screen black panel left a blank
     * screen whenever the paragraph had not been painted yet. */
    vis(s_big_box, true);
    vis(s_wait_title, !words);
    vis(s_wait_sub, !words);
    if (s_cat_timer) {
        lv_timer_resume(s_cat_timer);
    }
    /* Web-parity strip: tool row + counters, or the pending hint. */
    bool hint = words && s_pending && s_listen == 0;
    bool show_tool = s_has_tools && !hint && s_listen == 0;
    bool show_line = hint || (s_tool_counts[0] && s_listen == 0);
    if (s_home_line) {
        if (hint) {
            lv_label_set_text_static(s_home_line, "有待批事项 · 左滑查看审批 →");
            lv_obj_set_style_text_color(s_home_line, lv_color_hex(COL_AMBER), 0);
        } else {
            lv_label_set_text_static(s_home_line, s_tool_counts);
            lv_obj_set_style_text_color(s_home_line, lv_color_hex(COL_MUTED), 0);
        }
    }
    vis(s_home_tool, show_tool);
    vis(s_home_line, show_line);
    if (!words || !s_said || !s_said_buf) {
        return;
    }
    if (s_said_painted == s_said_rev) {
        return;
    }
    lv_label_set_text_static(s_said, s_said_buf);
    s_said_painted = s_said_rev;
    s_said_y = 0;
    lv_obj_set_pos(s_said, 4, 0);
    /* Measuring the paragraph here runs inside the screen task and stalls
     * it, so later replies never appear. A line estimate is enough to scroll. */
    s_said_h = 24 * (1 + (int)strlen(s_said_buf) / 16);
    if (s_said_box) {
        lv_obj_move_foreground(s_said_box);
    }
}

static void refresh_wait(void)
{
    const char *title = "按住侧键说话";
    const char *sub = "或输入指令";
    const char *dock = "按住说话";
    const char *hint = "PTT";
    uint32_t dock_bg = COL_CARD;
    uint32_t dock_border = COL_LINE;
    uint32_t dock_text = COL_TEXT;
    const char *detail = NULL;

    if (!s_wait_title) {
        return;
    }
    if (s_listen == 1) {
        title = "正在听";
        sub = "说完松开";
        dock = "松开发送";
        hint = "REC";
        dock_bg = COL_ORANGE;
        dock_border = COL_ORANGE;
        dock_text = 0x1D0B05;
        detail = "正在录音 · 松手发送";
        if (s_state) {
            lv_label_set_text(s_state, "正在听");
        }
    } else if (s_listen == 2) {
        title = "识别中";
        sub = "发给电脑";
        dock = "识别中…";
        hint = "STT";
        dock_bg = COL_CARD_HI;
        dock_border = COL_ORANGE;
        detail = "语音转文字，马上发送";
        if (s_state) {
            lv_label_set_text(s_state, "识别中");
        }
    } else if (s_meeting && (s_kind == 0 || s_kind == 4)) {
        title = "会议记录中";
        sub = "再双击侧键结束";
        detail = "会议记录中";
        if (s_state) {
            lv_label_set_text(s_state, "会议");
        }
    } else if (s_note[0]) {
        title = s_note;
        sub = s_note_sub[0] ? s_note_sub : "再按住说一次";
        dock = "再按一次";
        hint = "RETRY";
        dock_border = COL_LINE_SOFT;
        detail = "再按住侧键说一次";
    } else if (s_kind == 1) {
        title = "正在思考";
        sub = "让我想想";
        dock = "正在处理…";
        hint = "BUSY";
        dock_border = COL_LINE_SOFT;
        dock_text = COL_MUTED;
    } else if (s_kind == 2) {
        title = "正在执行";
        sub = "完成后会显示结果";
        dock = "正在处理…";
        hint = "BUSY";
        dock_border = COL_LINE_SOFT;
        dock_text = COL_MUTED;
    } else if (s_kind == 3) {
        title = "有操作需要确认";
        sub = "左滑查看审批";
        dock = "左滑去审批";
        hint = "REVIEW";
        dock_bg = COL_ORANGE_DARK;
        dock_border = COL_ORANGE;
        dock_text = COL_ORANGE;
    } else if (s_kind == 4) {
        title = "已完成";
        sub = "按住侧键继续";
    } else if (s_kind == 5) {
        title = s_offline ? "已离线" : "出错了";
        sub = s_offline ? "检查 WiFi 和电脑" : "看一下电脑上的提示";
        dock = "检查连接";
        hint = "OFFLINE";
        dock_bg = COL_DENY_BG;
        dock_border = COL_DENY;
        dock_text = COL_DANGER;
    }

    if (s_wait_sub && s_wait_sub != s_wait_title) {
        lv_label_set_text(s_wait_title, title);
        lv_label_set_text(s_wait_sub, sub);
    } else {
        char both[96];
        snprintf(both, sizeof(both), "%s\n%s", title, sub);
        lv_label_set_text(s_wait_title, both);
    }
    if (s_detail) {
        if (s_listen == 1 || s_listen == 2 || s_note[0] || (s_meeting && (s_kind == 0 || s_kind == 4))) {
            lv_label_set_text(s_detail, detail ? detail : "");
        } else if (s_bar_detail[0]) {
            lv_label_set_text(s_detail, s_bar_detail);
        }
    }
    if (s_dock) {
        lv_obj_set_style_bg_color(s_dock, lv_color_hex(dock_bg), 0);
        lv_obj_set_style_border_color(s_dock, lv_color_hex(dock_border), 0);
    }
    if (s_dock_label) {
        lv_label_set_text(s_dock_label, dock);
        lv_obj_set_style_text_color(s_dock_label, lv_color_hex(dock_text), 0);
    }
    if (s_dock_hint) {
        lv_label_set_text(s_dock_hint, hint);
        lv_obj_set_style_text_color(s_dock_hint, lv_color_hex(dock_text == COL_TEXT ? COL_MUTED : dock_text), 0);
    }
    if (s_state && s_listen == 0) {
        lv_label_set_text(s_state, state_word(NULL, s_kind));
    }
    paint_cat(s_kind);
    layout_home_words();
}

static void paint_status_icons(void)
{
    bool risk = s_pending && s_risk_code[0];
    vis(s_wifi, !risk);
    vis(s_batt, !risk);
    vis(s_risk_pill, risk);
    if (risk && s_risk_pill) {
        lv_label_set_text(s_risk_pill, s_risk_code);
        lv_obj_set_style_bg_opa(s_risk_pill, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_risk_pill, 1, 0);
        lv_obj_set_style_border_color(s_risk_pill, lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
        lv_obj_set_style_radius(s_risk_pill, 3, 0);
        lv_obj_set_style_text_color(s_risk_pill, lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
    }
}

static int s_raised_page = -99;
static bool s_raised_reply;

static void ui_raise_page(void)
{
    if (settings_open() || s_ui_scene != UI_SCENE_HOME) {
        return;
    }
    /* Spoken text stays on the home page. Side pages cover it. */
    bool cover = s_page != 0;
    if (s_raised_page == s_page && s_raised_reply == cover) {
        layout_home_words();
        return;
    }
    s_raised_page = s_page;
    s_raised_reply = cover;
    /* Leave the already-flushed home canvas in place. Cover it with an
     * opaque page so the panel cannot keep the cat or the keypad. */
    vis(s_wait, s_page == 0);
    vis_home_chrome(s_page == 0);
    vis(s_body, false);
    vis(s_stats_body, s_page == -1);
    vis(s_appr_body, s_page == 1);
    if (s_page == 1 && !s_pending) {
        approval_paint_empty();
    }
    lv_obj_t *front = NULL;
    if (s_page == 1) {
        front = s_appr_body;
    } else if (s_page == -1) {
        front = s_stats_body;
    }
    if (front && lv_obj_is_valid(front)) {
        lv_obj_move_foreground(front);
        lv_obj_invalidate(front);
    }
    if (s_bar && lv_obj_is_valid(s_bar)) {
        lv_obj_move_foreground(s_bar);
    }
    if (s_dots && lv_obj_is_valid(s_dots)) {
        lv_obj_move_foreground(s_dots);
    }
    /* Reply text used to close the codecs. The side key then did nothing. */
    if (s_page == 0) {
        pager_audio_resume();
    } else {
        pager_audio_suspend();
    }
    layout_home_words();
    ui_schedule_flush();
}

static void layout_actions(void)
{
    bool settings = settings_open();
    vis(s_bar, !settings);
    vis(s_dots, !settings);
    vis(s_wait, !settings);
    vis(s_prov_page, !settings && s_ui_scene == UI_SCENE_PROV);
    vis(s_pair_page, !settings && s_ui_scene == UI_SCENE_PAIR);
    vis(s_wifi_page, !settings && s_ui_scene == UI_SCENE_WIFI);
    if (!settings) {
        ui_raise_page();
    }
    vis(s_dock, false);
    if (settings) {
        return;
    }
    if (s_detail) {
        lv_label_set_text(s_detail, s_page == 0 || s_page == 1 ? s_bar_detail : "");
    }
    paint_status_icons();
    paint_dots();
}

static lv_point_t s_ptr0;
static bool s_ptr_down;
static lv_timer_t *s_swipe_timer;
static bool s_poll_down;
static lv_point_t s_poll_origin;
static int s_poll_dx;
static int s_poll_dy;

static void gesture_bubble(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(obj, i);
        lv_obj_add_flag(ch, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        gesture_bubble(ch);
    }
}

static void on_gesture(lv_event_t *e);

static void watch_gesture(lv_obj_t *root)
{
    if (!root) {
        return;
    }
    lv_obj_add_event_cb(root, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(root, on_gesture, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, on_gesture, LV_EVENT_RELEASED, NULL);
    gesture_bubble(root);
}

static uint32_t s_swiped_tick;

static void apply_horizontal_page(int toward_right)
{
    if (!s_shell || settings_open() || s_ui_scene != UI_SCENE_HOME) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (lv_tick_elaps(s_swiped_tick) < 350) {
        return;
    }
    int next = s_page;
    if (toward_right) {
        if (s_page == 1) {
            next = 0;
        } else if (s_page == 0) {
            next = -1;
        }
    } else if (s_page == -1) {
        next = 0;
    } else if (s_page == 0) {
        next = 1;
    }
    if (next == s_page) {
        return;
    }
    s_swiped_tick = now;
    /* Do not build the heavy stats/settings tree here. That runs on the
     * display task and drains internal RAM until the screen task stalls. */
    s_page = next;
    ESP_LOGI("pager_ui", "swipe page %d", s_page);
    if (s_state) {
        lv_label_set_text(s_state, s_page == -1 ? "电脑资源" : s_page == 1 ? "审批" : state_word(NULL, s_kind));
    }
    layout_actions();
}

void pager_ui_swipe(int toward_right)
{
    apply_horizontal_page(toward_right ? 1 : 0);
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        /* Finger left opens 审批, finger right opens 资源. Same as the web. */
        if (dir == LV_DIR_LEFT) {
            apply_horizontal_page(0);
        } else if (dir == LV_DIR_RIGHT) {
            apply_horizontal_page(1);
        }
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        s_ptr0 = p;
        s_ptr_down = true;
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_ptr_down) {
        return;
    }
    s_ptr_down = false;
    int dx = (int)p.x - (int)s_ptr0.x;
    int dy = (int)p.y - (int)s_ptr0.y;
    if (LV_ABS(dx) < 36 || LV_ABS(dx) < LV_ABS(dy)) {
        return;
    }
    apply_horizontal_page(dx > 0);
}

/* Widget gestures get swallowed by scrolling and by the phone shell.
 * Poll the touch position directly so a horizontal finger move always
 * changes page. */
static void swipe_poll(lv_timer_t *t)
{
    (void)t;
    lv_indev_t *indev = NULL;
    lv_indev_t *pressed = NULL;
    for (lv_indev_t *it = lv_indev_get_next(NULL); it; it = lv_indev_get_next(it)) {
        if (lv_indev_get_type(it) != LV_INDEV_TYPE_POINTER) {
            continue;
        }
        if (!indev) {
            indev = it;
        }
        if (lv_indev_get_state(it) == LV_INDEV_STATE_PRESSED) {
            pressed = it;
            break;
        }
    }
    if (pressed) {
        indev = pressed;
    }
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        if (!s_poll_down) {
            s_poll_origin = p;
            s_poll_dx = 0;
            s_poll_dy = 0;
            s_poll_down = true;
        } else {
            int dx = (int)p.x - (int)s_poll_origin.x;
            int dy = (int)p.y - (int)s_poll_origin.y;
            if (LV_ABS(dx) + LV_ABS(dy) >= LV_ABS(s_poll_dx) + LV_ABS(s_poll_dy)) {
                s_poll_dx = dx;
                s_poll_dy = dy;
            }
        }
        return;
    }
    if (!s_poll_down) {
        return;
    }
    s_poll_down = false;
    if (LV_ABS(s_poll_dx) >= 36 && LV_ABS(s_poll_dx) > LV_ABS(s_poll_dy)) {
        ESP_LOGI("pager_ui", "finger dx=%d dy=%d", s_poll_dx, s_poll_dy);
        apply_horizontal_page(s_poll_dx > 0);
        return;
    }
    /* Vertical drag reads more of the reply. The box itself is not
     * scrollable, so this cannot start an LVGL scroll that swallows
     * the next horizontal swipe. */
    if (LV_ABS(s_poll_dy) < 28 || LV_ABS(s_poll_dy) <= LV_ABS(s_poll_dx) || !s_said || !s_said_box) {
        return;
    }
    int box_h = lv_obj_get_height(s_said_box);
    int min_y = (s_said_h > box_h) ? (box_h - s_said_h) : 0;
    int y = s_said_y + s_poll_dy;
    if (y > 0) {
        y = 0;
    }
    if (y < min_y) {
        y = min_y;
    }
    if (y == s_said_y) {
        return;
    }
    s_said_y = y;
    lv_obj_set_pos(s_said, 4, y);
}

static void open_settings(lv_event_t *e)
{
    (void)e;
    if (!s_settings) {
        return;
    }
    lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings);
    layout_actions();
}

static void close_settings(lv_event_t *e)
{
    (void)e;
    hide_confirm();
    if (s_settings) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
    layout_actions();
}

static void show_voice(void)
{
    if (s_voice_value && s_voice_i >= 0 && s_voice_i < VOICE_N) {
        lv_label_set_text(s_voice_value, VOICE_NAME[s_voice_i]);
    }
}

static void on_voice(lv_event_t *e)
{
    (void)e;
    s_voice_i = (s_voice_i + 1) % VOICE_N;
    show_voice();
    nvs_handle_t h;
    if (nvs_open("pager", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "voice", (uint8_t)s_voice_i);
        nvs_commit(h);
        nvs_close(h);
    }
    pager_ui_voice(s_voice_i);
}

typedef struct {
    int index;
} ui_voice_args_t;

static void ui_set_voice_body(void *p)
{
    int index = ((ui_voice_args_t *)p)->index;
    if (index < 0 || index >= VOICE_N) {
        index = 0;
    }
    s_voice_i = index;
    show_voice();
}

static void ui_set_voice_cb(void *p)
{
    ui_set_voice_body(p);
}

void ui_set_voice(int index)
{
    ui_voice_args_t args = {.index = index};
    ui_dispatch(ui_set_voice_cb, &args, sizeof(args));
}

static void on_speak(lv_event_t *e)
{
    (void)e;
    s_announce = !s_announce;
    if (s_speak_value) {
        lv_label_set_text(s_speak_value, s_announce ? "开" : "关");
    }
    nvs_handle_t h;
    if (nvs_open("pager", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "speak", s_announce ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    pager_ui_speak(s_announce);
}

static void show_volume(void)
{
    char buf[8];
    unsigned v = (unsigned)s_volume;
    if (s_volume < 0) {
        v = 0;
    } else if (s_volume > 100) {
        v = 100;
    }
    snprintf(buf, sizeof(buf), "%u", v);
    if (s_vol_value) {
        lv_label_set_text(s_vol_value, buf);
    }
}

static void store_volume(void)
{
    nvs_handle_t h;
    if (nvs_open("pager", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", (uint8_t)s_volume);
        nvs_commit(h);
        nvs_close(h);
    }
    pager_ui_volume(s_volume);
}

static void on_vol_down(lv_event_t *e)
{
    (void)e;
    if (s_volume < 5) {
        s_volume = 0;
    } else {
        s_volume -= 5;
    }
    show_volume();
    store_volume();
}

static void on_vol_up(lv_event_t *e)
{
    (void)e;
    if (s_volume > 95) {
        s_volume = 100;
    } else {
        s_volume += 5;
    }
    show_volume();
    store_volume();
}

static void host_net_sub(char *buf, size_t len, float down, float up)
{
    if (down >= 0 && up >= 0) {
        snprintf(buf, len, "D%.0f U%.0fM", down, up);
    } else if (down >= 0) {
        snprintf(buf, len, "D%.0fM", down);
    } else if (up >= 0) {
        snprintf(buf, len, "U%.0fM", up);
    } else {
        snprintf(buf, len, "--");
    }
}

typedef struct {
    float cpu, ram_pct, ram_used, ram_total;
    float gpu, vram_used, vram_total, gpu_temp;
    float net_pct, net_down, net_up;
} ui_host_args_t;

static void ui_set_host_body(void *p)
{
    ui_host_args_t *a = p;
    float cpu = a->cpu;
    float ram_pct = a->ram_pct;
    float ram_used = a->ram_used;
    float ram_total = a->ram_total;
    float gpu = a->gpu;
    float vram_used = a->vram_used;
    float vram_total = a->vram_total;
    float gpu_temp = a->gpu_temp;
    float net_pct = a->net_pct;
    float net_down = a->net_down;
    float net_up = a->net_up;
    if (!s_shell || !s_stats_body) {
        return;
    }
    char sub[48];
    host_ring_set_pct(&s_host_cpu, cpu, "");
    if (ram_used >= 0 && ram_total >= 0) {
        snprintf(sub, sizeof(sub), "%.1f/%.0fG", ram_used, ram_total);
    } else {
        snprintf(sub, sizeof(sub), "--");
    }
    host_ring_set_pct(&s_host_ram, ram_pct, sub);
    if (gpu >= 0 || vram_used >= 0 || gpu_temp >= 0) {
        if (vram_used >= 0 && vram_total >= 0 && gpu_temp >= 0) {
            snprintf(sub, sizeof(sub), "%.1f/%.0fG·%.0f°", vram_used, vram_total, gpu_temp);
        } else if (vram_used >= 0 && vram_total >= 0) {
            snprintf(sub, sizeof(sub), "%.1f/%.0fG", vram_used, vram_total);
        } else if (gpu_temp >= 0) {
            snprintf(sub, sizeof(sub), "%.0f°", gpu_temp);
        } else {
            sub[0] = 0;
        }
        host_ring_set_pct(&s_host_gpu, gpu, sub);
    } else {
        host_ring_set_pct(&s_host_gpu, -1, "无GPU");
    }
    host_net_sub(sub, sizeof(sub), net_down, net_up);
    host_ring_set_pct(&s_host_net, net_pct, sub);
}

static void ui_set_host_cb(void *p)
{
    ui_set_host_body(p);
}

void ui_set_host(float cpu, float ram_pct, float ram_used, float ram_total,
                 float gpu, float vram_used, float vram_total, float gpu_temp,
                 float net_pct, float net_down, float net_up)
{
    ui_host_args_t args = {
        .cpu = cpu, .ram_pct = ram_pct, .ram_used = ram_used, .ram_total = ram_total,
        .gpu = gpu, .vram_used = vram_used, .vram_total = vram_total, .gpu_temp = gpu_temp,
        .net_pct = net_pct, .net_down = net_down, .net_up = net_up,
    };
    ui_dispatch(ui_set_host_cb, &args, sizeof(args));
}

typedef struct {
    bool on;
} ui_meeting_args_t;

static void ui_set_meeting_body(void *p)
{
    bool on = ((ui_meeting_args_t *)p)->on;
    s_meeting = on;
    if (!on && s_state && s_listen == 0) {
        lv_label_set_text(s_state, state_word(s_offline ? "OFFLINE" : "IDLE", s_kind));
    }
    refresh_wait();
    layout_feed();
}

static void ui_set_meeting_cb(void *p)
{
    ui_set_meeting_body(p);
}

void ui_set_meeting(bool on)
{
    ui_meeting_args_t args = {.on = on};
    ui_dispatch(ui_set_meeting_cb, &args, sizeof(args));
}

typedef struct {
    int volume;
} ui_volume_args_t;

static void ui_set_volume_body(void *p)
{
    int volume = ((ui_volume_args_t *)p)->volume;
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    show_volume();
}

static void ui_set_volume_cb(void *p)
{
    ui_set_volume_body(p);
}

void ui_set_volume(int volume)
{
    ui_volume_args_t args = {.volume = volume};
    ui_dispatch(ui_set_volume_cb, &args, sizeof(args));
}

static void listen_refresh_cb(void *p)
{
    (void)p;
    refresh_wait();
    layout_feed();
}

static void show_listening(bool on)
{
    if (on) {
        s_listen = 1;
        s_note[0] = 0;
        s_note_sub[0] = 0;
    } else if (s_listen == 1) {
        s_listen = 2;
    }
    /* The side key runs on the other core. Painting from there races the
     * reply label and the old sentence stays on screen. */
    if (on_ui_task()) {
        refresh_wait();
        layout_feed();
    } else {
        ui_dispatch(listen_refresh_cb, NULL, 0);
    }
}

static bool ptt_allowed(void)
{
    bool busy = s_kind == 1 || s_kind == 2 || s_offline;
    return s_page == 0 && !s_pending && !settings_open() && !busy;
}

static void apply_ptt(bool down)
{
    if (down && !ptt_allowed()) {
        return;
    }
    if (down == s_hold) {
        return;
    }
    s_hold = down;
    ESP_LOGI("pager", "ptt %s", down ? "down" : "up");
    if (!s_meeting) {
        show_listening(down);
    }
    pager_ui_ptt(down);
}

static void toast_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    lock();
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
    unlock();
}

static uint32_t toast_level_color(const char *level)
{
    if (!level) {
        return COL_ORANGE;
    }
    if (strcmp(level, "ok") == 0) {
        return COL_MINT;
    }
    if (strcmp(level, "warn") == 0) {
        return COL_AMBER;
    }
    if (strcmp(level, "err") == 0) {
        return COL_DANGER;
    }
    return COL_ORANGE;
}

typedef struct {
    char title[64];
    char body[160];
    char level[16];
    int ttl_ms;
} ui_toast_args_t;

static void ui_show_toast_body(void *p)
{
    if (!s_shell || !s_toast) {
        return;
    }
    ui_toast_args_t *a = p;
    const char *title = a->title;
    const char *body = a->body;
    const char *level = a->level;
    int ttl_ms = a->ttl_ms;
    if (!s_toast || !s_toast_title || !s_toast_body) {
        return;
    }
    if (ttl_ms < 1500) {
        ttl_ms = 1500;
    }
    if (ttl_ms > 60000) {
        ttl_ms = 60000;
    }
    lv_label_set_text(s_toast_title, title && title[0] ? title : "通知");
    lv_label_set_text(s_toast_body, body && body[0] ? body : "");
    if (s_toast_bar) {
        lv_obj_set_style_bg_color(s_toast_bar, lv_color_hex(toast_level_color(level)), 0);
    }
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) {
        lv_timer_set_period(s_toast_timer, ttl_ms);
        lv_timer_reset(s_toast_timer);
        lv_timer_resume(s_toast_timer);
    }
}

static void ui_show_toast_cb(void *p)
{
    ui_show_toast_body(p);
}

void ui_show_toast(const char *title, const char *body, const char *level, int ttl_ms)
{
    ui_toast_args_t args = {0};
    if (title) {
        strncpy(args.title, title, sizeof(args.title) - 1);
    }
    if (body) {
        strncpy(args.body, body, sizeof(args.body) - 1);
    }
    if (level) {
        strncpy(args.level, level, sizeof(args.level) - 1);
    }
    args.ttl_ms = ttl_ms;
    ui_dispatch(ui_show_toast_cb, &args, sizeof(args));
}

typedef struct {
    char pattern[32];
    int repeat;
} ui_ring_args_t;

static void ui_ring_device_body(void *p)
{
    ui_ring_args_t *a = p;
    int repeat = a->repeat;
    if (repeat < 1) {
        repeat = 1;
    }
    if (repeat > 3) {
        repeat = 3;
    }
    pager_audio_chime(repeat);
    ui_show_toast("电脑在找你", "听提示音", "info", 3500);
}

static void ui_ring_device_cb(void *p)
{
    ui_ring_device_body(p);
}

void ui_ring_device(const char *pattern, int repeat)
{
    ui_ring_args_t args = {0};
    if (pattern) {
        strncpy(args.pattern, pattern, sizeof(args.pattern) - 1);
    }
    args.repeat = repeat;
    ui_dispatch(ui_ring_device_cb, &args, sizeof(args));
}

typedef struct {
    char id0[32];
    char label0[40];
    char id1[32];
    char label1[40];
} ui_macro_slots_args_t;

static void ui_set_macro_slots_body(void *p)
{
    ui_macro_slots_args_t *a = p;
    const char *id0 = a->id0[0] ? a->id0 : NULL;
    const char *label0 = a->label0[0] ? a->label0 : NULL;
    const char *id1 = a->id1[0] ? a->id1 : NULL;
    const char *label1 = a->label1[0] ? a->label1 : NULL;
    s_macro_n = 0;
    s_macro_id[0][0] = s_macro_id[1][0] = 0;
    s_macro_label[0][0] = s_macro_label[1][0] = 0;
    if (id0 && label0) {
        snprintf(s_macro_id[0], sizeof(s_macro_id[0]), "%s", id0);
        snprintf(s_macro_label[0], sizeof(s_macro_label[0]), "%s", label0);
        s_macro_n = 1;
    }
    if (id1 && label1) {
        snprintf(s_macro_id[1], sizeof(s_macro_id[1]), "%s", id1);
        snprintf(s_macro_label[1], sizeof(s_macro_label[1]), "%s", label1);
        s_macro_n = 2;
    }
    if (s_macro_btn) {
        if (s_macro_n > 0) {
            lv_obj_clear_flag(s_macro_btn, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_macro_btn, 0), s_macro_label[0]);
        } else {
            lv_obj_add_flag(s_macro_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_set_macro_slots_cb(void *p)
{
    ui_set_macro_slots_body(p);
}

void ui_set_macro_slots(const char *id0, const char *label0, const char *id1, const char *label1)
{
    ui_macro_slots_args_t args = {0};
    if (id0) {
        strncpy(args.id0, id0, sizeof(args.id0) - 1);
    }
    if (label0) {
        strncpy(args.label0, label0, sizeof(args.label0) - 1);
    }
    if (id1) {
        strncpy(args.id1, id1, sizeof(args.id1) - 1);
    }
    if (label1) {
        strncpy(args.label1, label1, sizeof(args.label1) - 1);
    }
    ui_dispatch(ui_set_macro_slots_cb, &args, sizeof(args));
}

static void on_ping_pc(lv_event_t *e)
{
    (void)e;
    pager_ui_ping_pc();
}

static void on_macro_run(lv_event_t *e)
{
    (void)e;
    if (s_macro_id[0][0]) {
        pager_ui_macro(s_macro_id[0]);
    }
}

static void key_task(void *arg)
{
    (void)arg;
    bool level = false;
    int same = 0;
    bool talking = false;
    int ticks = 0;
    while (1) {
        bool key = gpio_get_level(PAGER_PTT_GPIO) == 0;
        if (key == level) {
            same = 0;
        } else if (++same < 2) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else {
            level = key;
            same = 0;
            if (!level) {
                ticks = 0;
                if (talking) {
                    talking = false;
                    pager_ui_ptt(false);
                    ESP_LOGI("pager", "key task up");
                }
            }
        }
        if (level && !talking) {
            ticks += 30;
            if (ticks >= 280) {
                talking = true;
                pager_ui_ptt(true);
                ESP_LOGI("pager", "key task down");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void key_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_hw_ready || !s_shell) {
        return;
    }
    bool key = gpio_get_level(PAGER_PTT_GPIO) == 0;
    static int same;
    static bool level;
    /* 0 idle, 1 short press, 2 push-to-talk, 3 waiting for the second tap, 4 swallow until release */
    static int phase;
    static int ticks;
    if (key == level) {
        same = 0;
    } else if (++same < 2) {
        return;
    } else {
        level = key;
        same = 0;
    }
    if (phase == 2 && !ptt_allowed()) {
        phase = 0;
        apply_ptt(false);
    }
    if (level) {
        if (phase == 3) {
            phase = 4;
            bool home = s_page == 0 && !settings_open();
            if ((s_meeting || home) && !pager_ui_meeting(!s_meeting)) {
                snprintf(s_note, sizeof(s_note), "%s", "还没连上电脑");
                snprintf(s_note_sub, sizeof(s_note_sub), "%s", "连上后再双击侧键");
                refresh_wait();
                layout_feed();
            }
        } else if (phase == 0) {
            phase = 1;
            ticks = 0;
        } else if (phase == 1) {
            ticks += 30;
            if (ticks >= 300) {
                if (!s_meeting && ptt_allowed()) {
                    phase = 2;
                    apply_ptt(true);
                } else {
                    phase = 4;
                }
            }
        }
    } else if (phase == 1) {
        phase = 3;
        ticks = 0;
    } else if (phase == 2) {
        phase = 0;
        apply_ptt(false);
    } else if (phase == 3) {
        ticks += 30;
        if (ticks >= 450) {
            phase = 0;
        }
    } else if (phase == 4) {
        phase = 0;
    }
}

static void build_settings(lv_obj_t *scr)
{
    s_settings = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
    lv_obj_set_style_bg_grad_color(s_settings, lv_color_hex(COL_BG_BOTTOM), 0);
    lv_obj_set_style_bg_grad_dir(s_settings, LV_GRAD_DIR_VER, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_settings, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings, LV_SCROLLBAR_MODE_AUTO);

    const bool wide = ui_wide();
    const lv_coord_t row_h = wide ? 56 : 42;
    const lv_coord_t btn_h = wide ? 48 : 34;
    const lv_coord_t gap = wide ? 12 : 6;
    lv_coord_t y = wide ? 72 : 60;

    lv_coord_t top = ui_top();
    lv_obj_t *title = text_font(s_settings, "设置", COL_ORANGE, ui_font_24());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, wide ? top : 12);
    lv_obj_t *sub = text(s_settings, "长按主页，或点右上角设置", COL_MUTED);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 20, wide ? (top + 36) : 44);
    if (wide) {
        y = top + 76;
    }

    lv_obj_t *speak = panel(s_settings, CARD_X, y, CARD_W, row_h, COL_CARD, COL_LINE, 14);
    lv_obj_add_flag(speak, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(speak, on_speak, LV_EVENT_CLICKED, NULL);
    lv_obj_t *speak_name = text(speak, "自动播报", COL_TEXT);
    lv_obj_align(speak_name, LV_ALIGN_LEFT_MID, 16, 0);
    s_speak_value = text(speak, "关", COL_AMBER);
    lv_obj_align(s_speak_value, LV_ALIGN_RIGHT_MID, -16, 0);
    y += row_h + gap;

    lv_obj_t *voice = panel(s_settings, CARD_X, y, CARD_W, row_h, COL_CARD, COL_LINE, 14);
    lv_obj_add_flag(voice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(voice, on_voice, LV_EVENT_CLICKED, NULL);
    lv_obj_t *voice_name = text(voice, "声音", COL_TEXT);
    lv_obj_align(voice_name, LV_ALIGN_LEFT_MID, 16, 0);
    s_voice_value = text(voice, "晓晓", COL_AMBER);
    lv_obj_align(s_voice_value, LV_ALIGN_RIGHT_MID, -16, 0);
    y += row_h + gap;

    lv_obj_t *ping = button(s_settings, "呼叫电脑", COL_CARD, on_ping_pc, NULL);
    lv_obj_set_size(ping, CARD_W, btn_h);
    lv_obj_set_pos(ping, CARD_X, y);
    lv_obj_set_style_border_color(ping, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(ping, 1, 0);
    lv_obj_align(lv_obj_get_child(ping, 0), LV_ALIGN_LEFT_MID, 16, 0);
    y += btn_h + gap;

    s_macro_btn = button(s_settings, "宏", COL_CARD, on_macro_run, NULL);
    lv_obj_set_size(s_macro_btn, CARD_W, btn_h);
    lv_obj_set_pos(s_macro_btn, CARD_X, y);
    lv_obj_set_style_border_color(s_macro_btn, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(s_macro_btn, 1, 0);
    lv_obj_align(lv_obj_get_child(s_macro_btn, 0), LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_flag(s_macro_btn, LV_OBJ_FLAG_HIDDEN);

    y += btn_h + gap;
    lv_obj_t *vol = panel(s_settings, CARD_X, y, CARD_W, wide ? 60 : 48, COL_CARD, COL_LINE, 14);
    lv_obj_t *vol_name = text(vol, "音量", COL_TEXT);
    lv_obj_align(vol_name, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *down = button(vol, "-", COL_BG, on_vol_down, NULL);
    lv_obj_set_size(down, wide ? 40 : 30, wide ? 40 : 30);
    lv_obj_align(down, LV_ALIGN_RIGHT_MID, wide ? -108 : -86, 0);
    s_vol_value = text_font(vol, "75", COL_AMBER, &lv_font_montserrat_16);
    lv_obj_set_width(s_vol_value, 36);
    lv_obj_set_style_text_align(s_vol_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_vol_value, LV_ALIGN_RIGHT_MID, wide ? -58 : -46, 0);
    lv_obj_t *up = button(vol, "+", COL_BG, on_vol_up, NULL);
    lv_obj_set_size(up, wide ? 40 : 30, wide ? 40 : 30);
    lv_obj_align(up, LV_ALIGN_RIGHT_MID, -16, 0);
    y += (wide ? 60 : 48) + gap;

    lv_obj_t *prov = button(s_settings, "配网", COL_CARD, on_settings_prov, NULL);
    lv_obj_set_size(prov, CARD_W, btn_h);
    lv_obj_set_pos(prov, CARD_X, y);
    lv_obj_set_style_border_color(prov, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(prov, 1, 0);
    lv_obj_align(lv_obj_get_child(prov, 0), LV_ALIGN_LEFT_MID, 16, 0);
    y += btn_h + gap;

    lv_obj_t *pair_open = button(s_settings, "配对", COL_CARD, on_settings_pair, NULL);
    lv_obj_set_size(pair_open, CARD_W, btn_h);
    lv_obj_set_pos(pair_open, CARD_X, y);
    lv_obj_set_style_border_color(pair_open, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(pair_open, 1, 0);
    lv_obj_align(lv_obj_get_child(pair_open, 0), LV_ALIGN_LEFT_MID, 16, 0);
    y += btn_h + gap;

    lv_obj_t *wifi = button(s_settings, "重新配网", COL_CARD, on_reset_wifi, NULL);
    lv_obj_set_size(wifi, CARD_W, btn_h);
    lv_obj_set_pos(wifi, CARD_X, y);
    lv_obj_set_style_border_color(wifi, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(wifi, 1, 0);
    lv_obj_align(lv_obj_get_child(wifi, 0), LV_ALIGN_LEFT_MID, 16, 0);
    y += btn_h + gap;

    lv_obj_t *pair = button(s_settings, "重新配对", COL_CARD, on_reset_pair, NULL);
    lv_obj_set_size(pair, CARD_W, btn_h);
    lv_obj_set_pos(pair, CARD_X, y);
    lv_obj_set_style_border_color(pair, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(pair, 1, 0);
    lv_obj_align(lv_obj_get_child(pair, 0), LV_ALIGN_LEFT_MID, 16, 0);
    y += btn_h + gap;

    lv_obj_t *back = button(s_settings, "返回", COL_ORANGE, close_settings, NULL);
    lv_obj_set_size(back, CARD_W, wide ? 52 : 32);
    lv_obj_set_pos(back, CARD_X, y);
    lv_obj_set_style_text_color(lv_obj_get_child(back, 0), lv_color_hex(0x1D0B05), 0);

    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *nav_chip(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = button(parent, label, COL_CARD, cb, NULL);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    return b;
}

static void build_scene_pages(lv_obj_t *scr)
{
    const bool wide = ui_wide();
    const lv_coord_t top = ui_top();
    const lv_coord_t back_w = wide ? 88 : 60;
    const lv_coord_t back_h = wide ? 40 : 28;

    s_prov_page = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
    lv_obj_t *prov_title = text_font(s_prov_page, "配网", COL_ORANGE, ui_font_24());
    lv_obj_align(prov_title, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_t *prov_back = nav_chip(s_prov_page, "返回", on_back_home);
    lv_obj_set_size(prov_back, back_w, back_h);
    lv_obj_align(prov_back, LV_ALIGN_TOP_RIGHT, -16, top);
    lv_obj_t *prov_sub = text(s_prov_page, "手机连这个热点后打开地址", COL_MUTED);
    lv_obj_set_width(prov_sub, UI_W - 48);
    lv_obj_set_style_text_align(prov_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(prov_sub, LV_ALIGN_TOP_MID, 0, top + (wide ? 40 : 28));
    lv_obj_t *prov_card = panel(s_prov_page, CARD_X, top + (wide ? 84 : 56), CARD_W,
                               wide ? 188 : 150, COL_CARD, COL_LINE, 22);
    lv_obj_t *prov_hint = text(prov_card, "热点名称", COL_MUTED);
    lv_obj_align(prov_hint, LV_ALIGN_TOP_MID, 0, wide ? 18 : 14);
    s_prov_name = text_font(prov_card, "Pager", COL_TEXT, ui_font_24());
    lv_label_set_long_mode(s_prov_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_prov_name, CARD_W - 40);
    lv_obj_set_style_text_align(s_prov_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_prov_name, LV_ALIGN_TOP_MID, 0, wide ? 48 : 38);
    lv_obj_t *prov_url = text_font(prov_card, "192.168.4.1", COL_ORANGE, &lv_font_montserrat_28);
    lv_obj_align(prov_url, LV_ALIGN_TOP_MID, 0, wide ? 96 : 76);
    lv_obj_t *prov_note = text(prov_card, "浏览器打开上面的地址", COL_MUTED);
    lv_obj_align(prov_note, LV_ALIGN_TOP_MID, 0, wide ? 144 : 116);
    lv_obj_t *prov_start = button(s_prov_page, "开始配网", COL_ORANGE, on_do_provision, NULL);
    lv_obj_set_size(prov_start, CARD_W, wide ? 52 : 42);
    lv_obj_align(prov_start, LV_ALIGN_BOTTOM_MID, 0, wide ? -28 : -18);
    lv_obj_set_style_text_color(lv_obj_get_child(prov_start, 0), lv_color_hex(0x1D0B05), 0);
    lv_obj_t *prov_foot = text(s_prov_page, "只支持 2.4GHz · 会断开当前网络", COL_MUTED);
    lv_obj_set_width(prov_foot, UI_W - 48);
    lv_obj_set_style_text_align(prov_foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(prov_foot, LV_ALIGN_BOTTOM_MID, 0, wide ? -88 : -64);
    vis(s_prov_page, false);

    s_wifi_page = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
    s_wifi_title = text_font(s_wifi_page, "Wi-Fi", COL_ORANGE, ui_font_24());
    lv_obj_align(s_wifi_title, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_t *wifi_back = nav_chip(s_wifi_page, "返回", on_back_home);
    lv_obj_set_size(wifi_back, back_w, back_h);
    lv_obj_align(wifi_back, LV_ALIGN_TOP_RIGHT, -16, top);
    s_wifi_sub = text(s_wifi_page, "连不上时可以重新配网", COL_MUTED);
    lv_obj_set_width(s_wifi_sub, UI_W - 48);
    lv_obj_set_style_text_align(s_wifi_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_sub, LV_ALIGN_TOP_MID, 0, top + (wide ? 40 : 28));
    lv_obj_t *wifi_card = panel(s_wifi_page, CARD_X, top + (wide ? 84 : 56), CARD_W,
                               wide ? 140 : 110, COL_CARD, COL_LINE, 22);
    s_wifi_status = text(wifi_card, "", COL_TEXT);
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_status, CARD_W - 40);
    lv_obj_set_style_text_align(s_wifi_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_status, LV_ALIGN_CENTER, 0, 0);
    s_wifi_btn = button(s_wifi_page, "重新配网", COL_ORANGE, on_do_provision, NULL);
    lv_obj_set_size(s_wifi_btn, CARD_W, wide ? 52 : 42);
    lv_obj_align(s_wifi_btn, LV_ALIGN_BOTTOM_MID, 0, wide ? -28 : -18);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wifi_btn, 0), lv_color_hex(0x1D0B05), 0);
    vis(s_wifi_page, false);

    s_pair_page = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
    lv_obj_add_flag(s_pair_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_pair_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *pair_title = text_font(s_pair_page, "配对", COL_ORANGE, ui_font_24());
    lv_obj_align(pair_title, LV_ALIGN_TOP_LEFT, 20, top);
    lv_obj_t *pair_back = nav_chip(s_pair_page, "返回", on_back_home);
    lv_obj_set_size(pair_back, back_w, back_h);
    lv_obj_align(pair_back, LV_ALIGN_TOP_RIGHT, -16, top);
    lv_obj_t *retry = nav_chip(s_pair_page, "重找", on_retry);
    lv_obj_set_size(retry, back_w, back_h);
    lv_obj_align(retry, LV_ALIGN_TOP_RIGHT, -(16 + back_w + 8), top);
    s_pair_host = text(s_pair_page, "正在寻找电脑", COL_MUTED);
    lv_label_set_long_mode(s_pair_host, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_pair_host, UI_W - 40);
    lv_obj_align(s_pair_host, LV_ALIGN_TOP_LEFT, 20, top + (wide ? 40 : 28));
    s_code_label = text_font(s_pair_page, "--- ---", COL_TEXT, &lv_font_montserrat_40);
    lv_obj_set_style_text_letter_space(s_code_label, 2, 0);
    lv_obj_align(s_code_label, LV_ALIGN_TOP_MID, 0, top + (wide ? 68 : 56));
    s_pair_hint = text(s_pair_page, "输入电脑网页上的 6 位码", COL_TEXT_DIM);
    lv_label_set_long_mode(s_pair_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_pair_hint, UI_W - 40);
    lv_obj_set_style_text_align(s_pair_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pair_hint, LV_ALIGN_TOP_MID, 0, top + (wide ? 114 : 98));
    static const char *labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "删除", "0", "确定"};
    memcpy(s_keys, "123456789C0K", 12);
    lv_coord_t pad = wide ? 20 : 14;
    lv_coord_t gap = wide ? 8 : 8;
    lv_coord_t key_w = (UI_W - 2 * pad - 2 * gap) / 3;
    lv_coord_t key_h = wide ? 38 : 32;
    lv_coord_t grid_top = wide ? 198 : 148;
    for (int i = 0; i < 12; i++) {
        uint32_t bg = (s_keys[i] == 'K') ? COL_ORANGE : COL_CARD;
        lv_obj_t *key = button(s_pair_page, labels[i], bg, on_key, &s_keys[i]);
        lv_obj_set_size(key, key_w, key_h);
        lv_obj_set_pos(key, pad + (i % 3) * (key_w + gap), grid_top + (i / 3) * (key_h + gap));
        if (s_keys[i] == 'K') {
            lv_obj_set_style_text_color(lv_obj_get_child(key, 0), lv_color_hex(0x1D0B05), 0);
        } else {
            lv_obj_set_style_border_color(key, lv_color_hex(COL_LINE_SOFT), 0);
            lv_obj_set_style_border_width(key, 1, 0);
        }
    }
    vis(s_pair_page, false);
}

static void build_light_pages(lv_obj_t *scr)
{
    if (!s_stats_body) {
        /* Web parity for the 资源 page: a 2x2 grid of CPU / 内存 / GPU / 网络
         * rings, each with the percent inside and label + sub below. Scaled
         * for the 480-wide watch screen instead of the 240px preview. */
        s_stats_body = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
        lv_obj_clear_flag(s_stats_body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_stats_body, LV_SCROLLBAR_MODE_OFF);
        const int pad = 20;
        const int gap = 12;
        int col_w = (UI_W - pad * 2 - gap) / 2;
        int y0 = ui_top() + 16;
        int row_h = (UI_H - DOT_H - 8 - y0 - gap) / 2;
        build_host_ring(s_stats_body, pad, y0, col_w, row_h, "CPU", &s_host_cpu);
        build_host_ring(s_stats_body, pad + col_w + gap, y0, col_w, row_h, "内存",
                        &s_host_ram);
        build_host_ring(s_stats_body, pad, y0 + row_h + gap, col_w, row_h, "GPU",
                        &s_host_gpu);
        build_host_ring(s_stats_body, pad + col_w + gap, y0 + row_h + gap, col_w, row_h,
                        "网络", &s_host_net);
        vis(s_stats_body, false);
        watch_gesture(s_stats_body);
    }
    if (!s_host_fx_timer) {
        s_host_fx_timer = lv_timer_create(host_fx_timer_cb, 150, NULL);
    }

    if (!s_appr_body) {
        /* Web parity for the 审批 page: one amber card with the risk badge,
         * summary, reason and the two buttons; empty state handled by
         * ui_show_approval. */
        const bool wide = ui_wide();
        const lv_coord_t card_w = wide ? 440 : 224;
        const lv_coord_t card_h = wide ? 300 : 188;
        const lv_coord_t card_x = wide ? 20 : 8;
        const lv_coord_t card_y = wide ? (ui_top() + 44) : 8;
        s_appr_body = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
        s_appr_card = panel(s_appr_body, card_x, card_y, card_w, card_h, 0x1A1610, COL_AMBER, 16);
        lv_obj_set_style_shadow_width(s_appr_card, 0, 0);
        lv_obj_set_style_border_opa(s_appr_card, 80, 0);

        s_appr_mark = panel(s_appr_card, 0, 0, 4, card_h, COL_AMBER, 0, 0);

        s_appr_badge = text_font(s_appr_card, "L2", COL_AMBER,
                                 wide ? &lv_font_montserrat_16 : FONT_MICRO);
        lv_obj_set_style_bg_opa(s_appr_badge, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_appr_badge, 1, 0);
        lv_obj_set_style_border_color(s_appr_badge, lv_color_hex(COL_AMBER), 0);
        lv_obj_set_style_radius(s_appr_badge, 5, 0);
        lv_obj_set_style_pad_left(s_appr_badge, 8, 0);
        lv_obj_set_style_pad_right(s_appr_badge, 8, 0);
        lv_obj_set_style_pad_top(s_appr_badge, 3, 0);
        lv_obj_set_style_pad_bottom(s_appr_badge, 3, 0);
        lv_obj_align(s_appr_badge, LV_ALIGN_TOP_LEFT, 14, 12);

        s_appr_risk = text(s_appr_card, "等待你批准", COL_AMBER);
        lv_label_set_long_mode(s_appr_risk, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_appr_risk, card_w - 90);
        lv_obj_align(s_appr_risk, LV_ALIGN_TOP_LEFT, 52, 12);

        s_appr_title = text(s_appr_card, "暂无待批", COL_TEXT);
        lv_label_set_long_mode(s_appr_title, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_appr_title, card_w - 32);
        lv_obj_align(s_appr_title, LV_ALIGN_TOP_LEFT, 14, 40);

        s_appr_meta = text(s_appr_card, "", COL_MUTED);
        lv_label_set_long_mode(s_appr_meta, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_appr_meta, card_w - 32);
        lv_obj_set_style_bg_opa(s_appr_meta, LV_OPA_TRANSP, 0);
        lv_obj_align(s_appr_meta, LV_ALIGN_TOP_LEFT, 14, 96);
        vis(s_appr_meta, false);

        s_appr_hint = text(s_appr_card, "", COL_MUTED);
        lv_label_set_long_mode(s_appr_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_appr_hint, card_w - 32);
        lv_obj_align(s_appr_hint, LV_ALIGN_TOP_LEFT, 14, 96);

        s_appr_deny = button(s_appr_card, "拒绝", COL_DENY_BG, on_deny, NULL);
        lv_obj_set_size(s_appr_deny, (card_w - 40) / 2, wide ? 48 : 28);
        lv_obj_set_style_radius(s_appr_deny, 10, 0);
        lv_obj_align(s_appr_deny, LV_ALIGN_BOTTOM_LEFT, 14, wide ? -18 : -12);
        lv_obj_set_style_border_color(s_appr_deny, lv_color_hex(COL_DENY), 0);
        lv_obj_set_style_border_width(s_appr_deny, 1, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_appr_deny, 0), lv_color_hex(COL_DANGER), 0);

        s_appr_allow = button(s_appr_card, "批准", COL_ORANGE, on_approve, NULL);
        lv_obj_set_size(s_appr_allow, (card_w - 40) / 2, wide ? 48 : 28);
        lv_obj_set_style_radius(s_appr_allow, 10, 0);
        lv_obj_align(s_appr_allow, LV_ALIGN_BOTTOM_RIGHT, -14, wide ? -18 : -12);
        lv_obj_set_style_text_color(lv_obj_get_child(s_appr_allow, 0), lv_color_hex(0x1D0B05), 0);

        s_appr_cmd_btn = button(s_appr_card, "查看命令", 0x14181A, on_cmd_toggle, NULL);
        lv_obj_set_size(s_appr_cmd_btn, 96, 24);
        lv_obj_set_style_radius(s_appr_cmd_btn, 6, 0);
        lv_obj_align(s_appr_cmd_btn, LV_ALIGN_BOTTOM_LEFT, 14, wide ? -78 : -52);
        lv_obj_set_style_border_color(s_appr_cmd_btn, lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_border_width(s_appr_cmd_btn, 1, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_appr_cmd_btn, 0), lv_color_hex(COL_MUTED), 0);
        vis(s_appr_cmd_btn, false);

        s_appr_cmd_text = text(s_appr_card, "", COL_TEXT_DIM);
        lv_label_set_long_mode(s_appr_cmd_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_appr_cmd_text, card_w - 32);
        lv_obj_align(s_appr_cmd_text, LV_ALIGN_TOP_LEFT, 14, 140);
        vis(s_appr_cmd_text, false);

        vis(s_appr_allow, false);
        vis(s_appr_deny, false);
        vis(s_appr_body, false);
        watch_gesture(s_appr_body);
    }

    if (!s_dots) {
        /* Page indicator, like the web page-dots. */
        s_dots = panel(scr, 0, UI_H - DOT_H, UI_W, DOT_H, 0, 0, 0);
        s_dot0 = lv_obj_create(s_dots);
        s_dot1 = lv_obj_create(s_dots);
        s_dot2 = lv_obj_create(s_dots);
        lv_obj_set_style_border_width(s_dot0, 0, 0);
        lv_obj_set_style_border_width(s_dot1, 0, 0);
        lv_obj_set_style_border_width(s_dot2, 0, 0);
        lv_obj_set_style_radius(s_dot0, 2, 0);
        lv_obj_set_style_radius(s_dot1, 2, 0);
        lv_obj_set_style_radius(s_dot2, 2, 0);
        lv_obj_set_style_bg_opa(s_dot0, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(s_dot1, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(s_dot2, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dot0, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_dot1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_dot2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    approval_paint_empty();
}

static void build_shell(void)
{
    layout_sync();
    lv_obj_t *scr = screen();

    /* Own status row: mini expression + state + detail, like the web.
     * The phone status bar is hidden; this row sits at the very top. */
    s_bar = panel(scr, 0, 0, UI_W, 30, 0x0B0D0E, 0, 0);
    /* Subtle bottom edge, same as the web status bar. */
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(COL_LINE), 0);
    make_mini_cat(s_bar);
    s_state = text(s_bar, "就绪", COL_MUTED);
    lv_obj_align(s_state, LV_ALIGN_LEFT_MID, 32, 0);
    s_detail = text(s_bar, "", COL_MUTED);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail, UI_W - 250);
    lv_obj_align(s_detail, LV_ALIGN_RIGHT_MID, -80, 0);
    /* Visible way into settings: a chip in the top row (long-press on the
     * home page still works too). */
    lv_obj_t *gear = button(s_bar, "设置", 0x14181A, open_settings, NULL);
    lv_obj_set_size(gear, 62, 22);
    lv_obj_set_style_radius(gear, 11, 0);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(gear, 0), lv_color_hex(COL_MUTED), 0);

    s_wait = panel(scr, 0, 0, UI_W, UI_H, COL_BG, 0, 0);
    lv_obj_add_flag(s_wait, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wait, open_settings, LV_EVENT_LONG_PRESSED, NULL);
    make_cat(s_wait);

    const bool wide = ui_wide();
    /* No system bar: the face starts right under the status row and the
     * reply gets the room it frees up. */
    lv_coord_t title_y = wide ? (ui_top() + 148) : 128;
    lv_coord_t sub_y = wide ? (ui_top() + 188) : 146;

    s_wait_title = text_font(s_wait, "按住侧键说话", COL_TEXT, wide ? ui_font_24() : ui_font());
    lv_label_set_long_mode(s_wait_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wait_title, UI_W - 48);
    lv_obj_set_style_text_align(s_wait_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_wait_title, 4, 0);
    lv_obj_align(s_wait_title, LV_ALIGN_TOP_MID, 0, title_y);

    s_wait_sub = text(s_wait, "或输入指令", COL_MUTED);
    lv_obj_set_style_text_color(s_wait_sub, lv_color_hex(0x8B8B84), 0);
    lv_label_set_long_mode(s_wait_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wait_sub, UI_W - 64);
    lv_obj_set_style_text_align(s_wait_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wait_sub, LV_ALIGN_TOP_MID, 0, sub_y);

    /* Center page shows what was said, the same role as the web body. */
    {
        /* Under the face. A full-height box starting higher covers the
         * expression and, before the glyphs are drawn, looks like a black screen. */
        lv_coord_t said_y = ui_top() + (wide ? 152 : 118);
        lv_coord_t said_h = UI_H - said_y - DOT_H - 8 - 52;
        if (said_h < 80) {
            said_h = 80;
        }
        s_said_box = panel(s_wait, 16, said_y, UI_W - 32, said_h, COL_BG, 0, 0);
        /* A scrollable label steals the finger after the first reply.
         * Horizontal swipes then never reach the page switch, and each
         * scroll step redraws the paragraph until the screen task stalls. */
        lv_obj_clear_flag(s_said_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_said_box, LV_SCROLLBAR_MODE_OFF);
        /* Font 24 only has the home prompts. Spoken text uses the full
         * 16px font; the 24px fallback draws those characters as garbage. */
        s_said = text_font(s_said_box, "", COL_TEXT, ui_font());
        lv_label_set_long_mode(s_said, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_said, UI_W - 48);
        lv_obj_set_style_text_align(s_said, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_line_space(s_said, 6, 0);
        lv_obj_clear_flag(s_said, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_said, 4, 0);
        vis(s_said_box, false);
    }

    /* Tool row + summary/hint strip under the reply, web home style. */
    s_home_tool = panel(s_wait, 24, UI_H - DOT_H - 8 - 52, UI_W - 48, 24, 0x14181A, 0, 6);
    s_home_tool_accent = panel(s_home_tool, 0, 0, 3, 24, 0x353B3D, 0, 2);
    s_home_tool_name = text(s_home_tool, "", COL_TEXT_DIM);
    lv_label_set_long_mode(s_home_tool_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_home_tool_name, UI_W - 120);
    lv_obj_align(s_home_tool_name, LV_ALIGN_LEFT_MID, 10, 0);
    s_home_tool_mark = text_font(s_home_tool, "", COL_MUTED, FONT_DETAIL);
    lv_obj_align(s_home_tool_mark, LV_ALIGN_RIGHT_MID, -10, 0);
    vis(s_home_tool, false);

    s_home_line = text_font(s_wait, "", COL_MUTED, ui_font());
    lv_label_set_long_mode(s_home_line, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_home_line, UI_W - 96);
    lv_obj_set_style_text_align(s_home_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_home_line, LV_ALIGN_TOP_MID, 0, UI_H - DOT_H - 8 - 26);
    vis(s_home_line, false);

    s_net_line = text(s_wait, "", COL_MUTED);
    lv_obj_set_width(s_net_line, UI_W - 64);
    lv_obj_set_style_text_align(s_net_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_net_line, LV_ALIGN_BOTTOM_MID, 0, -(DOT_H + 8));
    vis(s_net_line, false);

    build_scene_pages(scr);

    nvs_handle_t h;
    uint8_t speak_on = 1;
    uint8_t saved_vol = 75;
    uint8_t saved_voice = 0;
    if (nvs_open("pager", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "speak", &speak_on) != ESP_OK) {
            speak_on = 1;
        }
        if (nvs_get_u8(h, "vol", &saved_vol) != ESP_OK || saved_vol > 100) {
            saved_vol = 75;
        }
        if (nvs_get_u8(h, "voice", &saved_voice) != ESP_OK || saved_voice >= VOICE_N) {
            saved_voice = 0;
        }
        nvs_close(h);
    }
    s_announce = speak_on != 0;
    s_volume = saved_vol;
    s_voice_i = saved_voice;
    pager_ui_speak(s_announce);
    pager_ui_volume(s_volume);
    pager_ui_voice(s_voice_i);

    if (!s_hw_ready) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << PAGER_PTT_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        s_key_timer = lv_timer_create(key_timer_cb, 30, NULL);
        /* If the display task stalls, this still starts and stops recording. */
        xTaskCreatePinnedToCore(key_task, "pager_key", 3072, NULL, 6, NULL, 0);
        s_hw_ready = true;
    }

    s_page = 0;
    s_raised_page = -99;
    s_raised_reply = false;
    s_kind = 0;
    if (s_big_box) {
        paint_one_face(s_big_box, s_big_l, s_big_r, s_big_m, 0);
    }
    build_light_pages(scr);
    /* Settings + toast come with the shell — no lazy memory gate. */
    build_shell_extra();
    s_shell = true;
    s_block_wifi = true;
    s_ui_scene = UI_SCENE_HOME;
    watch_gesture(s_wait);
    if (!s_swipe_timer) {
        s_swipe_timer = lv_timer_create(swipe_poll, 20, NULL);
    }
    if (s_bar) {
        lv_obj_move_foreground(s_bar);
    }
    layout_actions();
    refresh_wait();
    if (s_cat_timer) {
        lv_timer_pause(s_cat_timer);
    }
    ui_log_heap("home-core");
    ui_refresh_now();
    if (s_cat_timer) {
        lv_timer_resume(s_cat_timer);
    }
}

static void build_shell_extra(void)
{
    if (s_shell_extra) {
        return;
    }
    ui_bind_app_screen();
    lv_obj_t *scr = s_app_scr;
    if (!scr || !lv_obj_is_valid(scr)) {
        return;
    }

    /* Settings and the toast used to be built lazily behind an internal
     * memory gate that never opened, which made settings impossible to
     * open. They are small: build them with the shell at boot. The old
     * reply card and the bottom PTT dock are gone — the home page shows
     * the reply and the tool strip directly. */

    s_toast = panel(scr, 8, 34, UI_W - 16, 58, COL_CARD_HI, COL_LINE, 10);
    lv_obj_set_style_shadow_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(s_toast, 10, 0);
    lv_obj_set_style_shadow_opa(s_toast, 60, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    s_toast_bar = panel(s_toast, 0, 0, 3, LV_PCT(100), COL_ORANGE, 0, 0);
    s_toast_title = text(s_toast, "", COL_TEXT);
    lv_label_set_long_mode(s_toast_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_toast_title, UI_W - 36);
    lv_obj_align(s_toast_title, LV_ALIGN_TOP_LEFT, 10, 6);
    s_toast_body = text(s_toast, "", COL_TEXT_DIM);
    lv_label_set_long_mode(s_toast_body, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_toast_body, UI_W - 36);
    lv_obj_align(s_toast_body, LV_ALIGN_TOP_LEFT, 10, 24);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, 5000, NULL);
    lv_timer_pause(s_toast_timer);

    build_settings(scr);
    if (s_speak_value) {
        lv_label_set_text(s_speak_value, s_announce ? "开" : "关");
    }
    show_voice();
    show_volume();
    s_shell_extra = true;
    ui_log_heap("home-extra");
}

static void ui_show_home_body(void *p)
{
    (void)p;
    if (s_shell && s_wait && lv_obj_is_valid(s_wait)) {
        ui_set_net_line(NULL);
        if (s_ui_scene != UI_SCENE_HOME) {
            s_page = 0;
        }
        ui_apply_scene(UI_SCENE_HOME);
        refresh_wait();
        layout_actions();
        ui_log_heap("home-ready");
        ui_refresh_now();
        return;
    }
    build_shell();
    ui_set_net_line(NULL);
    s_page = 0;
    if (s_settings) {
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    }
    s_ui_scene = UI_SCENE_HOME;
    layout_actions();
    ui_refresh_now();
}

static void ui_show_home_cb(void *p)
{
    ui_show_home_body(p);
}

void ui_show_home(void)
{
    ui_dispatch(ui_show_home_cb, NULL, 0);
}

static int classify_state(const char *state)
{
    if (!state || !state[0]) {
        return 0;
    }
    if (strcmp(state, "OFFLINE") == 0) {
        return 5;
    }
    if (strcmp(state, "ERROR") == 0 || strcmp(state, "出错") == 0) {
        return 5;
    }
    if (strcmp(state, "BOOTING") == 0 || strcmp(state, "THINKING") == 0) {
        return 1;
    }
    if (strcmp(state, "RUNNING_TOOL") == 0) {
        return 2;
    }
    if (strcmp(state, "WAITING_APPROVAL") == 0) {
        return 3;
    }
    if (strcmp(state, "DONE") == 0) {
        return 4;
    }
    if (strcmp(state, "IDLE") == 0) {
        return 0;
    }
    return 0;
}

static const char *detail_text(const char *detail)
{
    static char buf[96];
    const char *p = detail ? detail : "";
    if (p[0] == 'L' && p[1] >= '0' && p[1] <= '3') {
        p += 2;
        while (*p == ' ') {
            p++;
        }
        if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7) {
            p += 2;
        } else if ((unsigned char)p[0] >= 0xE0) {
            p += 3;
        }
        while (*p == ' ') {
            p++;
        }
    }
    snprintf(buf, sizeof(buf), "%s", p);
    return buf;
}

typedef struct {
    char state[48];
    char detail[96];
} ui_state_args_t;

static void ui_set_state_body(void *p)
{
    ui_state_args_t *a = p;
    const char *state = a->state;
    const char *detail = a->detail;
    if (!s_shell) {
        return;
    }
    s_offline = state && strcmp(state, "OFFLINE") == 0;
    s_kind = classify_state(state);
    if (s_pending) {
        s_kind = 3;
    }
    if (s_state && s_listen == 0) {
        lv_label_set_text(s_state, state_word(state, s_kind));
    }
    snprintf(s_bar_detail, sizeof(s_bar_detail), "%s", detail_text(detail));
    paint_one_face(s_face_box, s_face_l, s_face_r, s_face_m, s_kind);
    paint_one_face(s_big_box, s_big_l, s_big_r, s_big_m, s_kind);
    if (s_listen == 2 && state && (strcmp(state, "THINKING") == 0 || strcmp(state, "RUNNING_TOOL") == 0 ||
                                   strcmp(state, "DONE") == 0 || strcmp(state, "ERROR") == 0 ||
                                   strcmp(state, "出错") == 0)) {
        s_listen = 0;
        if (s_state) {
            lv_label_set_text(s_state, state_word(state, s_kind));
        }
    }
    refresh_wait();
    layout_feed();
    layout_actions();
}

static void ui_set_state_cb(void *p)
{
    ui_set_state_body(p);
}

void ui_set_state(const char *state, const char *detail)
{
    static char prev_state[48];
    static char prev_detail[96];
    const char *st = state ? state : "";
    const char *dt = detail ? detail : "";
    if (strcmp(prev_state, st) == 0 && strcmp(prev_detail, dt) == 0) {
        return;
    }
    snprintf(prev_state, sizeof(prev_state), "%s", st);
    snprintf(prev_detail, sizeof(prev_detail), "%s", dt);
    ui_state_args_t args = {0};
    memcpy(args.state, prev_state, sizeof(args.state));
    memcpy(args.detail, prev_detail, sizeof(args.detail));
    ui_dispatch(ui_set_state_cb, &args, sizeof(args));
}

static void layout_feed(void)
{
    /* The scene reply card is gone; the home page shows the reply and the
     * tool strip directly. Only the page raise still matters here. */
    if (s_reply_card) {
        ui_raise_page();
        return;
    }
    ui_raise_page();
}

static bool reply_bufs(void)
{
    if (s_reply_mu && s_reply_raw && s_reply_scratch && s_reply_latest) {
        return true;
    }
    if (!s_reply_mu) {
        s_reply_mu = xSemaphoreCreateMutex();
    }
    if (!s_reply_mu) {
        return false;
    }
    if (xSemaphoreTake(s_reply_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (!s_reply_raw) {
        s_reply_raw = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_reply_raw) {
            s_reply_raw = malloc(2048);
        }
        if (s_reply_raw) {
            s_reply_raw[0] = 0;
        }
    }
    if (!s_reply_scratch) {
        s_reply_scratch = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_reply_scratch) {
            s_reply_scratch = malloc(2048);
        }
    }
    if (!s_reply_latest) {
        s_reply_latest = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_reply_latest) {
            s_reply_latest = malloc(2048);
        }
        if (s_reply_latest) {
            s_reply_latest[0] = 0;
        }
    }
    xSemaphoreGive(s_reply_mu);
    return s_reply_raw && s_reply_scratch && s_reply_latest;
}

static void reply_commit(void)
{
    if (!s_reply_due || !s_reply_latest || !s_shell || !s_wait) {
        return;
    }
    if (!s_said_buf) {
        s_said_buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_said_buf) {
            s_said_buf = malloc(2048);
        }
        if (!s_said_buf) {
            return;
        }
        s_said_buf[0] = 0;
    }
    s_reply_due = false;
    if (strcmp(s_said_buf, s_reply_latest) == 0) {
        return;
    }
    memcpy(s_said_buf, s_reply_latest, 2048);
    s_said_buf[2047] = 0;
    s_said_rev++;
    ESP_LOGI("pager_ui", "reply paint rev=%u len=%u", (unsigned)s_said_rev, (unsigned)strlen(s_said_buf));
    s_has_text = s_said_buf[0] != 0;
    if (s_has_text) {
        s_listen = 0;
        s_note[0] = 0;
        s_note_sub[0] = 0;
    }
    layout_home_words();
    ESP_LOGI("pager_ui", "reply shown");
}

static void reply_service(void)
{
    if (!s_shell || !reply_bufs()) {
        return;
    }
    if (s_reply_gen != s_reply_seen &&
        xSemaphoreTake(s_reply_mu, 0) == pdTRUE) {
        int gen = s_reply_gen;
        memcpy(s_reply_scratch, s_reply_raw, 2048);
        s_reply_seen = gen;
        xSemaphoreGive(s_reply_mu);
        clean_reply(s_reply_latest, 2048, s_reply_scratch);
        s_reply_due = true;
        ESP_LOGI("pager_ui", "reply ready gen=%d", gen);
    }
    reply_commit();
}

void ui_set_reply(const char *text)
{
    if (!reply_bufs()) {
        return;
    }
    if (!text) {
        text = "";
    }
    if (xSemaphoreTake(s_reply_mu, pdMS_TO_TICKS(30)) != pdTRUE) {
        return;
    }
    if (strcmp(s_reply_raw, text) == 0) {
        xSemaphoreGive(s_reply_mu);
        return;
    }
    snprintf(s_reply_raw, 2048, "%s", text);
    s_reply_gen++;
    xSemaphoreGive(s_reply_mu);
    if (on_ui_task()) {
        reply_service();
    }
}

typedef struct {
    char summary[80];
    char status[16];
    int done_n;
    int fail_n;
} ui_tools_args_t;

static void ui_set_tools_body(void *p)
{
    ui_tools_args_t *a = p;
    const char *summary = a->summary;
    const char *status = a->status;
    int done_n = a->done_n;
    int fail_n = a->fail_n;
    if (!s_shell) {
        return;
    }
    s_has_tools = summary && summary[0];
    s_done_n = done_n > 0 ? done_n : 0;
    s_fail_n = fail_n > 0 ? fail_n : 0;
    const char *mark = "..";
    uint32_t col = COL_MUTED;
    uint32_t bar = 0x353B3D;
    if (status && strcmp(status, "completed") == 0) {
        mark = "OK";
        col = COL_MINT;
        bar = COL_MINT;
    } else if (status && strcmp(status, "failed") == 0) {
        mark = "X";
        col = COL_DANGER;
        bar = COL_DANGER;
    }
    /* Home strip: row with the active tool + counters line below. */
    {
        char counts[64];
        counts[0] = 0;
        if (s_done_n > 0 && s_fail_n > 0) {
            snprintf(counts, sizeof(counts), "%d 个工具已完成 · %d 失败", s_done_n, s_fail_n);
        } else if (s_done_n > 0) {
            snprintf(counts, sizeof(counts), "%d 个工具已完成", s_done_n);
        } else if (s_fail_n > 0) {
            snprintf(counts, sizeof(counts), "%d 失败", s_fail_n);
        }
        snprintf(s_tool_counts, sizeof(s_tool_counts), "%s", counts);
        const char *mark = "..";
        uint32_t col = COL_MUTED;
        uint32_t bar = 0x353B3D;
        if (status && strcmp(status, "completed") == 0) {
            mark = "OK";
            col = COL_MINT;
            bar = COL_MINT;
        } else if (status && strcmp(status, "failed") == 0) {
            mark = "X";
            col = COL_DANGER;
            bar = COL_DANGER;
        }
        if (s_home_tool_name) {
            lv_label_set_text(s_home_tool_name, s_has_tools ? summary : "");
        }
        if (s_home_tool_mark) {
            lv_label_set_text(s_home_tool_mark, s_has_tools ? mark : "");
            lv_obj_set_style_text_color(s_home_tool_mark, lv_color_hex(col), 0);
        }
        if (s_home_tool_accent) {
            lv_obj_set_style_bg_color(s_home_tool_accent,
                                      lv_color_hex(s_has_tools ? bar : 0x353B3D), 0);
        }
    }
    layout_home_words();
    layout_feed();
}

static void ui_set_tools_cb(void *p)
{
    ui_set_tools_body(p);
}

void ui_set_tools(const char *summary, const char *status, int done_n, int fail_n)
{
    ui_tools_args_t args = {0};
    if (summary) {
        strncpy(args.summary, summary, sizeof(args.summary) - 1);
    }
    if (status) {
        strncpy(args.status, status, sizeof(args.status) - 1);
    }
    args.done_n = done_n;
    args.fail_n = fail_n;
    ui_dispatch(ui_set_tools_cb, &args, sizeof(args));
}

typedef struct {
    char title[48];
    char sub[48];
} ui_hear_args_t;

static void ui_hear_status_body(void *p)
{
    ui_hear_args_t *a = p;
    const char *title = a->title;
    const char *sub = a->sub;
    if (!s_shell || !s_wait_title) {
        return;
    }
    if (title && strcmp(title, "识别中") == 0) {
        s_listen = 2;
        s_note[0] = 0;
    } else {
        s_listen = 0;
        snprintf(s_note, sizeof(s_note), "%s", title ? title : "");
        snprintf(s_note_sub, sizeof(s_note_sub), "%s", sub ? sub : "");
        if (s_state) {
            lv_label_set_text(s_state, "就绪");
        }
    }
    refresh_wait();
    layout_feed();
}

static void ui_hear_status_cb(void *p)
{
    ui_hear_status_body(p);
}

void ui_hear_status(const char *title, const char *sub)
{
    ui_hear_args_t args = {0};
    if (title) {
        strncpy(args.title, title, sizeof(args.title) - 1);
    }
    if (sub) {
        strncpy(args.sub, sub, sizeof(args.sub) - 1);
    }
    ui_dispatch(ui_hear_status_cb, &args, sizeof(args));
}

static void split_summary(const char *src, char *action, size_t action_size,
                          char *target, size_t target_size)
{
    action[0] = 0;
    target[0] = 0;
    const char *p = src ? src : "";
    if (p[0] == 'L' && p[1] >= '0' && p[1] <= '3' && p[2] == ' ') {
        p += 3;
    }
    const char *dot = strstr(p, " · ");
    size_t sep = 3;
    if (!dot) {
        dot = strstr(p, "·");
        sep = 2;
    }
    if (!dot) {
        snprintf(action, action_size, "%.*s", (int)action_size - 1, p);
        return;
    }
    size_t n = (size_t)(dot - p);
    if (n >= action_size) {
        n = action_size - 1;
    }
    memcpy(action, p, n);
    action[n] = 0;
    const char *t = dot + sep;
    while (*t == ' ') {
        t++;
    }
    snprintf(target, target_size, "%.*s", (int)target_size - 1, t);
}

typedef struct {
    char id[40];
    char summary[160];
    char risk[8];
    bool can_approve;
    char reason[96];
    char command[180];
    bool has_id;
} ui_approval_args_t;

static bool s_appr_empty_shown;

/* Neutral empty state for the approval page, matching the web client. */
static void approval_paint_empty(void)
{
    if (!s_appr_card || s_appr_empty_shown) {
        return;
    }
    s_appr_empty_shown = true;
    set_approval_buttons_enabled(false);
    vis(s_appr_allow, false);
    vis(s_appr_deny, false);
    vis(s_appr_cmd_btn, false);
    vis(s_appr_cmd_text, false);
    vis(s_appr_hint, true);
    vis(s_appr_meta, false);
    vis(s_appr_badge, false);
    if (s_appr_title) {
        lv_label_set_text(s_appr_title, "暂无待批");
        lv_obj_set_style_text_color(s_appr_title, lv_color_hex(COL_TEXT), 0);
        lv_obj_align(s_appr_title, LV_ALIGN_TOP_LEFT, 14, 44);
    }
    if (s_appr_risk) {
        lv_label_set_text(s_appr_risk, "设备空闲");
        lv_obj_set_style_text_color(s_appr_risk, lv_color_hex(COL_MUTED), 0);
    }
    if (s_appr_hint) {
        lv_label_set_text(s_appr_hint, "需要你确认时会出现在这里");
        lv_obj_set_style_text_color(s_appr_hint, lv_color_hex(COL_MUTED), 0);
        lv_obj_align(s_appr_hint, LV_ALIGN_TOP_LEFT, 14, 92);
    }
    lv_obj_set_style_bg_color(s_appr_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(s_appr_card, lv_color_hex(COL_LINE), 0);
    if (s_appr_mark) {
        lv_obj_set_style_bg_color(s_appr_mark, lv_color_hex(COL_LINE), 0);
    }
}

static void ui_show_approval_body(void *p)
{
    ui_approval_args_t *a = p;
    const char *id = a->has_id ? a->id : NULL;
    const char *summary = a->summary;
    const char *risk = a->risk;
    bool can_approve = a->can_approve;
    const char *reason = a->reason;
    const char *command = a->command;
    if (!s_shell) {
        build_shell();
    }
    if (!id || !id[0]) {
        s_pending = false;
        s_approval_id[0] = 0;
        s_risk_code[0] = 0;
        s_risk_l3 = false;
        s_appr_can = false;
        s_appr_empty_shown = false;
        approval_paint_empty();
        layout_actions();
        return;
    }
    snprintf(s_approval_id, sizeof(s_approval_id), "%s", id);
    snprintf(s_risk_code, sizeof(s_risk_code), "%s", (risk && risk[0]) ? risk : "L2");
    s_pending = true;
    s_appr_empty_shown = false;
    s_risk_l3 = strcmp(s_risk_code, "L3") == 0;
    s_appr_can = can_approve;
    s_kind = 3;
    char line[160];
    snprintf(line, sizeof(line), "%s", summary && summary[0] ? summary : "需要批准");
    if (s_appr_title) {
        lv_label_set_text(s_appr_title, line);
        lv_obj_set_style_text_color(s_appr_title, lv_color_hex(COL_TEXT), 0);
        lv_obj_align(s_appr_title, LV_ALIGN_TOP_LEFT, 12, 32);
    }
    if (s_appr_hint) {
        lv_label_set_text(s_appr_hint, reason && reason[0] ? reason : "");
        vis(s_appr_hint, reason && reason[0]);
    }
    vis(s_appr_meta, false);
    s_cmd_store[0] = 0;
    if (command && command[0]) {
        snprintf(s_cmd_store, sizeof(s_cmd_store), "%s", command);
    }
    if (s_appr_cmd_text) {
        lv_label_set_text(s_appr_cmd_text, s_cmd_store);
        vis(s_appr_cmd_text, false);
    }
    if (s_appr_cmd_btn) {
        lv_label_set_text(lv_obj_get_child(s_appr_cmd_btn, 0), "查看命令");
        vis(s_appr_cmd_btn, s_cmd_store[0] != 0);
    }
    if (s_appr_badge) {
        lv_label_set_text(s_appr_badge, s_risk_code);
        lv_obj_set_style_bg_opa(s_appr_badge, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_appr_badge, 1, 0);
        lv_obj_set_style_border_color(s_appr_badge, lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
        lv_obj_set_style_text_color(s_appr_badge,
                                    lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
        vis(s_appr_badge, true);
    }
    if (s_appr_risk) {
        lv_label_set_text(s_appr_risk, s_appr_can ? "等待你批准" : "需要回电脑确认");
        lv_obj_set_style_text_color(s_appr_risk, lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
    }
    if (s_appr_card) {
        lv_obj_set_style_bg_color(s_appr_card, lv_color_hex(s_risk_l3 ? 0x1A1212 : 0x1A1610), 0);
        lv_obj_set_style_border_color(s_appr_card, lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
    }
    if (s_appr_mark) {
        lv_obj_set_style_bg_color(s_appr_mark, lv_color_hex(s_risk_l3 ? COL_DANGER : COL_AMBER), 0);
    }
    if (s_appr_allow) {
        lv_label_set_text(lv_obj_get_child(s_appr_allow, 0), s_appr_can ? "批准" : "回电脑批准");
        if (s_appr_can) {
            lv_obj_set_style_bg_color(s_appr_allow, lv_color_hex(COL_ORANGE), 0);
            lv_obj_set_style_border_width(s_appr_allow, 0, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_appr_allow, 0), lv_color_hex(0x1D0B05), 0);
            lv_obj_clear_state(s_appr_allow, LV_STATE_DISABLED);
        } else {
            lv_obj_set_style_bg_color(s_appr_allow, lv_color_hex(COL_CARD), 0);
            lv_obj_set_style_border_color(s_appr_allow, lv_color_hex(COL_LINE), 0);
            lv_obj_set_style_border_width(s_appr_allow, 1, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_appr_allow, 0), lv_color_hex(COL_MUTED), 0);
            lv_obj_add_state(s_appr_allow, LV_STATE_DISABLED);
        }
        vis(s_appr_allow, true);
    }
    if (s_appr_deny) {
        lv_obj_clear_state(s_appr_deny, LV_STATE_DISABLED);
        vis(s_appr_deny, true);
    }
    if (s_detail) {
        snprintf(s_bar_detail, sizeof(s_bar_detail), "%.*s", (int)sizeof(s_bar_detail) - 1, line);
        lv_label_set_text(s_detail, s_bar_detail);
    }
    paint_one_face(s_face_box, s_face_l, s_face_r, s_face_m, 3);
    if (s_state) {
        lv_label_set_text(s_state, "等待批准");
    }
    if (!settings_open()) {
        s_page = 1;
    }
    refresh_wait();
    layout_actions();
}

static void ui_show_approval_cb(void *p)
{
    ui_show_approval_body(p);
}

void ui_show_approval(const char *id, const char *summary, const char *risk, bool can_approve,
                      const char *reason, const char *command)
{
    ui_approval_args_t args = {0};
    args.can_approve = can_approve;
    if (id && id[0]) {
        args.has_id = true;
        strncpy(args.id, id, sizeof(args.id) - 1);
    }
    if (summary) {
        strncpy(args.summary, summary, sizeof(args.summary) - 1);
    }
    if (risk) {
        strncpy(args.risk, risk, sizeof(args.risk) - 1);
    }
    if (reason) {
        strncpy(args.reason, reason, sizeof(args.reason) - 1);
    }
    if (command) {
        strncpy(args.command, command, sizeof(args.command) - 1);
    }
    ui_dispatch(ui_show_approval_cb, &args, sizeof(args));
}

const char *ui_qa_scene_name(void)
{
    return ui_scene_name(s_ui_scene);
}

#if CONFIG_PAGER_QA
#include "sdkconfig.h"
#include "freertos/semphr.h"
#if LV_USE_SNAPSHOT
#include "src/others/snapshot/lv_snapshot.h"
#endif

static struct {
    char *hex;
    size_t hex_size;
    bool ok;
    SemaphoreHandle_t done;
} s_qa_fp;

static uint8_t ui_qa_color_lum(lv_color_t c)
{
    return lv_color32_luminance(lv_color_to_32(c, LV_OPA_COVER));
}

static lv_obj_t *ui_qa_grid_obj(lv_obj_t *scr, int gx, int gy)
{
    uint32_t n = lv_obj_get_child_cnt(scr);
    if (n == 0) {
        return scr;
    }
    return lv_obj_get_child(scr, (uint32_t)((gy * 8 + gx) % n));
}

static void ui_qa_fp_cb(void *unused)
{
    (void)unused;
    s_qa_fp.ok = false;
    if (!s_qa_fp.hex || s_qa_fp.hex_size < 8 * 8 * 2 + 1) {
        goto out;
    }
    s_qa_fp.hex[0] = 0;
    ui_bind_app_screen();
    lv_obj_t *scr = (s_app_scr && lv_obj_is_valid(s_app_scr)) ? s_app_scr : lv_scr_act();
    if (!scr) {
        goto out;
    }
    if (lv_obj_get_width(scr) < 8 || lv_obj_get_height(scr) < 8) {
        goto out;
    }
    size_t pos = 0;
    for (int gy = 0; gy < 8; gy++) {
        for (int gx = 0; gx < 8; gx++) {
            lv_obj_t *obj = ui_qa_grid_obj(scr, gx, gy);
            lv_color_t bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
            lv_color_t tx = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
            uint8_t lum = (uint8_t)(ui_qa_color_lum(bg) ^ (ui_qa_color_lum(tx) >> 1));
            lum ^= (uint8_t)((lv_obj_get_width(obj) + lv_obj_get_height(obj)) & 0xff);
            if (pos + 2 >= s_qa_fp.hex_size) {
                goto out;
            }
            pos += (size_t)snprintf(s_qa_fp.hex + pos, s_qa_fp.hex_size - pos, "%02x", lum);
        }
    }
    s_qa_fp.ok = true;
out:
    if (s_qa_fp.done) {
        xSemaphoreGive(s_qa_fp.done);
    }
}

bool ui_qa_fingerprint(char *hex_out, size_t hex_size)
{
    if (!hex_out || hex_size < 8 * 8 * 2 + 1) {
        return false;
    }
    ui_dispatch_begin();
    s_qa_fp.hex = hex_out;
    s_qa_fp.hex_size = hex_size;
    s_qa_fp.ok = false;
    s_qa_fp.done = xSemaphoreCreateBinary();
    if (!s_qa_fp.done) {
        return false;
    }
    ui_dispatch(ui_qa_fp_cb, NULL, 0);
    if (xSemaphoreTake(s_qa_fp.done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        vSemaphoreDelete(s_qa_fp.done);
        s_qa_fp.done = NULL;
        return false;
    }
    bool ok = s_qa_fp.ok;
    vSemaphoreDelete(s_qa_fp.done);
    s_qa_fp.done = NULL;
    return ok;
}

static struct {
    uint8_t *rgb;
    int w;
    int h;
    int stride;
    bool ok;
    SemaphoreHandle_t done;
} s_qa_shot;

static void ui_qa_blit_nonzero(uint8_t *dst, int dw, int dh, int dstride,
                               const lv_draw_buf_t *src)
{
    if (!dst || !src || !src->data) {
        return;
    }
    int sw = src->header.w;
    int sh = src->header.h;
    int sstride = src->header.stride;
    int h = dh < sh ? dh : sh;
    int w = dw < sw ? dw : sw;
    for (int y = 0; y < h; y++) {
        const uint16_t *srow = (const uint16_t *)(src->data + y * sstride);
        uint16_t *drow = (uint16_t *)(dst + y * dstride);
        for (int x = 0; x < w; x++) {
            if (srow[x] != 0) {
                drow[x] = srow[x];
            }
        }
    }
}

static void ui_qa_shot_cb(void *unused)
{
    (void)unused;
    /* A shot that timed out on the HTTP side still finishes here later.
     * Free its buffer before starting a new one, or every failed request
     * leaks a full 480x480 frame. */
    if (s_qa_shot.rgb) {
        free(s_qa_shot.rgb);
        s_qa_shot.rgb = NULL;
    }
    s_qa_shot.ok = false;
    ui_bind_app_screen();
    lv_obj_t *scr = (s_app_scr && lv_obj_is_valid(s_app_scr)) ? s_app_scr : lv_scr_act();
    if (!scr) {
        goto out;
    }
    lv_draw_buf_t *app = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    if (!app || !app->data) {
        if (app) {
            lv_draw_buf_destroy(app);
        }
        goto out;
    }
    int w = app->header.w;
    int h = app->header.h;
    int stride = app->header.stride;
    size_t bytes = (size_t)stride * (size_t)h;
    uint8_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = malloc(bytes);
    }
    if (!copy) {
        lv_draw_buf_destroy(app);
        goto out;
    }
    memcpy(copy, app->data, bytes);
    lv_draw_buf_destroy(app);

    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *sys = disp ? lv_display_get_layer_sys(disp) : NULL;
    if (sys) {
        lv_draw_buf_t *ov = lv_snapshot_take(sys, LV_COLOR_FORMAT_RGB565);
        if (ov) {
            ui_qa_blit_nonzero(copy, w, h, stride, ov);
            lv_draw_buf_destroy(ov);
        }
    }

    s_qa_shot.rgb = copy;
    s_qa_shot.w = w;
    s_qa_shot.h = h;
    s_qa_shot.stride = stride;
    s_qa_shot.ok = true;
out:
    if (s_qa_shot.done) {
        xSemaphoreGive(s_qa_shot.done);
    }
}

bool ui_qa_screenshot(uint8_t **rgb565, int *w, int *h, int *stride_bytes)
{
    if (!rgb565 || !w || !h || !stride_bytes) {
        return false;
    }
    *rgb565 = NULL;
    ui_dispatch_begin();
    /* Do not clear s_qa_shot.rgb here: a previous shot that timed out may
     * still be finishing on the screen task, and it frees its own buffer. */
    s_qa_shot.ok = false;
    s_qa_shot.done = xSemaphoreCreateBinary();
    if (!s_qa_shot.done) {
        return false;
    }
    s_qa_shot_req = true;
    if (xSemaphoreTake(s_qa_shot.done, pdMS_TO_TICKS(8000)) != pdTRUE) {
        vSemaphoreDelete(s_qa_shot.done);
        s_qa_shot.done = NULL;
        return false;
    }
    vSemaphoreDelete(s_qa_shot.done);
    s_qa_shot.done = NULL;
    if (!s_qa_shot.ok || !s_qa_shot.rgb) {
        return false;
    }
    *rgb565 = s_qa_shot.rgb;
    *w = s_qa_shot.w;
    *h = s_qa_shot.h;
    *stride_bytes = s_qa_shot.stride;
    s_qa_shot.rgb = NULL;
    return true;
}

void ui_qa_screenshot_free(uint8_t *p)
{
    free(p);
}
#else
bool ui_qa_fingerprint(char *hex_out, size_t hex_size)
{
    (void)hex_out;
    (void)hex_size;
    return false;
}

bool ui_qa_screenshot(uint8_t **rgb565, int *w, int *h, int *stride_bytes)
{
    (void)rgb565;
    (void)w;
    (void)h;
    (void)stride_bytes;
    return false;
}

void ui_qa_screenshot_free(uint8_t *p)
{
    (void)p;
}
#endif
