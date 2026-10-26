#pragma once

#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void pager_qa_init(void);

/** Called from main when CONFIG_PAGER_QA; phone is systems::phone::Phone *. */
void pager_qa_arm_autostart(void *phone);

/** Register QA HTTP handlers on the captive portal server. */
void pager_qa_register_http(httpd_handle_t server);

/** Switch UI to a named scene (provision, wifi, pair, boot, home). */
bool pager_qa_set_scene(const char *name);

/** 8x8 average-luminance fingerprint of the active LVGL screen (hex into buf). */
bool pager_qa_screen_fingerprint(char *hex_out, size_t hex_size);

/** JSON status: net + audio flags (best-effort). */
bool pager_qa_status_json(char *out, size_t out_size);

/** Play chime and report audio path readiness. */
bool pager_qa_audio_selftest(char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
