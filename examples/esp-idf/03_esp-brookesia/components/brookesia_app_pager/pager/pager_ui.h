#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ui_init(void);

void ui_show_provision(const char *ap_name);
void ui_show_wifi(const char *status);
void ui_unblock_wifi(void);
void ui_show_pair(const char *host);
void ui_pair_status(const char *text);
void ui_show_home(void);
/** toward_right: 1 opens 资源, 0 opens 审批. From the phone gesture reader. */
void pager_ui_swipe(int toward_right);
void ui_qa_request_swipe(int toward_right);
/** Simulate a side-key hold of hold_ms milliseconds (QA only). */
void ui_qa_request_ptt(int hold_ms);
/** Open (1) or close (0) the settings page (QA only). */
void ui_qa_request_settings(int open);
void ui_set_state(const char *state, const char *detail);
void ui_set_reply(const char *text);
void ui_set_tools(const char *summary, const char *status, int done_n, int fail_n);
void ui_hear_status(const char *title, const char *sub);
void ui_show_approval(const char *id, const char *summary, const char *risk, bool can_approve,
                      const char *reason, const char *command);
void ui_set_volume(int volume);
void ui_set_voice(int index);
void ui_set_meeting(bool on);
void ui_set_host(float cpu, float ram_pct, float ram_used, float ram_total,
                 float gpu, float vram_used, float vram_total, float gpu_temp,
                 float net_pct, float net_down, float net_up);
void ui_show_toast(const char *title, const char *body, const char *level, int ttl_ms);
void ui_ring_device(const char *pattern, int repeat);
void ui_set_macro_slots(const char *id0, const char *label0, const char *id1, const char *label1);

/** QA: current on-device scene name (wifi/home/pair/provision/none). */
const char *ui_qa_scene_name(void);

/** QA: 8x8 luminance hex fingerprint; blocks until LVGL thread captures (CONFIG_PAGER_QA). */
bool ui_qa_fingerprint(char *hex_out, size_t hex_size);

/** QA: RGB565 screenshot in PSRAM; caller frees with ui_qa_screenshot_free. */
bool ui_qa_screenshot(uint8_t **rgb565, int *w, int *h, int *stride_bytes);
void ui_qa_screenshot_free(uint8_t *p);

/* Implemented by the network task. UI calls these from the LVGL thread. */
void pager_ui_submit_code(const char *code);
void pager_ui_decide(const char *id, bool allow);
void pager_ui_reset_wifi(void);
void pager_ui_reset_pair(void);
void pager_ui_enter_pair(void);
void pager_ui_leave_pair(void);
void pager_ui_retry_discover(void);
void pager_ui_ptt(bool down);
/* Ask the computer to start or stop meeting recording. False if the watch is offline. */
bool pager_ui_meeting(bool start);
void pager_ui_speak(bool on);
void pager_ui_voice(int index);
void pager_ui_volume(int volume);
void pager_ui_ping_pc(void);
void pager_ui_macro(const char *id);
