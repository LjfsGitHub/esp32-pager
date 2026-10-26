#include "pager_net.h"
#include "pager_audio.h"
#include "sdkconfig.h"

#include "pager_qa.h"
#include "pager_ui.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "nvs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pager";

static void start_httpd(void);

extern const uint8_t pager_cert_pem_start[] asm("_binary_pager_cert_pem_start");

/* SHA-256 of the Bridge certificate pinned in pager_cert.pem. */
static const char *CERT_FP = "b9feb4bb718ba7398488c1372c726ab44ee16b1b7aa09484491f1e76dc11b463";

#define BIT_IP BIT0
#define BIT_FAIL BIT1
#define BIT_CREDS BIT2

#define RX_CAP (192 * 1024)

typedef enum {
    CMD_CODE = 1,
    CMD_ALLOW,
    CMD_DENY,
    CMD_RESET_WIFI,
    CMD_RESET_PAIR,
    CMD_RETRY,
    CMD_PING_PC,
    CMD_MACRO,
    CMD_QUIT,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char text[96];
    bool allow;
} pager_cmd_t;

typedef enum {
    ST_PROVISION = 0,
    ST_JOIN,
    ST_DISCOVER,
    ST_PAIR,
    ST_LINK,
    ST_RUN,
} net_state_t;

static EventGroupHandle_t s_eg;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_mu;
static httpd_handle_t s_httpd;
static esp_websocket_client_handle_t s_ws;

static bool s_wifi_inited;
static bool s_wifi_started;
static volatile bool s_net_quit;
static TaskHandle_t s_net_task;
static bool s_net_task_running;
static bool s_ap_on;
static bool s_expect_sta;
static int s_retry;

static char s_ap_ssid[32];
static char s_ssid[33];
static char s_psk[65];
static char s_token[128];
static char s_host[48];
static uint16_t s_port = 8766;
static bool s_tls = true;

static char *s_rx;
static int s_rx_len;
static volatile bool s_rx_ready;
static volatile bool s_ws_up;
static volatile bool s_ws_connecting;
static volatile int s_ws_gen;
static int64_t s_ws_connect_by;
static int s_quick_fail;
static int64_t s_ws_up_at;
static int64_t s_ws_retry_at;
static int64_t s_last_ping;

static char s_state[48];
static char s_detail[160];
#define REPLY_CAP (16 * 1024)
static char *s_reply;
static char *s_reply_view;

#define TOOL_SLOTS 24
typedef struct {
    char id[40];
    char summary[80];
    char status[16];
} tool_slot_t;
static tool_slot_t s_tools[TOOL_SLOTS];
static int s_tool_n;
static char s_appr_id[40];
static char s_appr_summary[240];
static char s_appr_risk[16];
static char s_appr_reason[96];
static char s_appr_cmd[180];
static bool s_appr_on;
static bool s_appr_can;
static bool s_ui_dirty;
static bool s_home_on;
static bool s_heard_hold;
static volatile bool s_speak_on;
static volatile int s_volume = 75;
static volatile int s_voice;
static volatile bool s_prefs_dirty = true;
static const char *const VOICE_ID[] = {"xiaoxiao", "xiaoyi", "yunxi", "moss"};
#define VOICE_N 4

static int voice_index(const char *id)
{
    if (!id) {
        return 0;
    }
    for (int i = 0; i < VOICE_N; i++) {
        if (strcmp(id, VOICE_ID[i]) == 0) {
            return i;
        }
    }
    return 0;
}
static volatile bool s_meet_tail;
static volatile int s_meeting_req;

static char s_toast_title[48];
static char s_toast_body[280];
static char s_toast_level[8];
static int s_toast_ttl;
static bool s_toast_fire;
static char s_ring_pat[8];
static int s_ring_repeat;
static bool s_ring_fire;
static char s_macro_slot_id[2][32];
static char s_macro_slot_label[2][40];
static bool s_macros_dirty;

static char s_shown_appr[40];
static net_state_t s_st;
static volatile bool s_pair_ui;

static const char PORTAL[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pager</title><style>"
    "body{font-family:sans-serif;background:#141210;color:#f6f1ea;margin:0;padding:24px}"
    "h1{font-size:22px;color:#ff5a1f}p{color:#a89888}"
    "select,input,button{width:100%;box-sizing:border-box;margin:8px 0;padding:12px;border-radius:10px;border:0;font-size:16px}"
    "button{background:#ff5a1f;color:#fff;font-weight:700}"
    "</style></head><body><h1>Pager 配网</h1>"
    "<p id=msg>选择 2.4GHz 网络。5GHz 不会出现在列表里。</p>"
    "<select id=ssid></select>"
    "<input id=manual placeholder=或手动输入热点名>"
    "<input id=pass type=password placeholder=密码，开放网络留空>"
    "<button onclick=save()>连接</button><script>"
    "async function scan(){const r=await fetch('/scan');const j=await r.json();"
    "const s=document.getElementById('ssid');s.innerHTML='';"
    "(j.list||[]).forEach(n=>{const o=document.createElement('option');o.value=n;o.textContent=n;s.appendChild(o);});"
    "if(!s.options.length)document.getElementById('msg').textContent='没有扫到网络，刷新页面再试';}"
    "async function save(){const manual=document.getElementById('manual').value.trim();"
    "const ssid=manual||document.getElementById('ssid').value;"
    "const pass=document.getElementById('pass').value;"
    "document.getElementById('msg').textContent='已交给设备，请看屏幕';"
    "await fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});}"
    "scan();</script></body></html>";

static void utf8_copy(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void nvs_put_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open("pager", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, key, value ? value : "");
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pager", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_ssid);
    nvs_get_str(h, "ssid", s_ssid, &n);
    n = sizeof(s_psk);
    nvs_get_str(h, "psk", s_psk, &n);
    n = sizeof(s_token);
    nvs_get_str(h, "token", s_token, &n);
    n = sizeof(s_host);
    nvs_get_str(h, "host", s_host, &n);
    uint16_t port = 0;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port) {
        s_port = port;
    }
    uint8_t tls = 1;
    if (nvs_get_u8(h, "tls", &tls) == ESP_OK) {
        s_tls = tls != 0;
    }
    nvs_close(h);
}

static void queue_cmd(cmd_type_t type, const char *text, bool allow)
{
    if (!s_q) {
        return;
    }
    pager_cmd_t cmd = {.type = type, .allow = allow};
    if (text) {
        utf8_copy(cmd.text, sizeof(cmd.text), text);
    }
    xQueueSend(s_q, &cmd, 0);
}

void pager_ui_submit_code(const char *code)
{
    queue_cmd(CMD_CODE, code, false);
}

void pager_ui_decide(const char *id, bool allow)
{
    queue_cmd(allow ? CMD_ALLOW : CMD_DENY, id, allow);
}

void pager_ui_reset_wifi(void)
{
    queue_cmd(CMD_RESET_WIFI, NULL, false);
}

void pager_ui_reset_pair(void)
{
    s_pair_ui = true;
    s_home_on = false;
    queue_cmd(CMD_RESET_PAIR, NULL, false);
}

void pager_ui_enter_pair(void)
{
    pager_ui_reset_pair();
}

void pager_ui_leave_pair(void)
{
    s_pair_ui = false;
}

void pager_ui_retry_discover(void)
{
    queue_cmd(CMD_RETRY, NULL, false);
}

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

static void wifi_register_handlers(void)
{
    static bool registered;
    if (registered) {
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL, NULL));
    registered = true;
}

static bool sta_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void sta_sync_if_connected(void)
{
    if (!s_eg || !sta_has_ip()) {
        return;
    }
    ESP_LOGI(TAG, "sta already has ip");
    s_retry = 0;
    xEventGroupClearBits(s_eg, BIT_FAIL);
    xEventGroupSetBits(s_eg, BIT_IP);
}

static void pager_net_ensure_audio(void)
{
    static bool started;
    if (!started) {
        pager_audio_start();
        started = true;
    }
}

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_expect_sta && s_retry < 6) {
            s_retry++;
            ESP_LOGW(TAG, "sta retry %d", s_retry);
            esp_wifi_connect();
        } else {
            s_expect_sta = false;
            xEventGroupSetBits(s_eg, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
#if CONFIG_PAGER_QA
        start_httpd();
        ESP_LOGI(TAG, "qa http " IPSTR, IP2STR(&event->ip_info.ip));
#endif
        s_retry = 0;
        xEventGroupClearBits(s_eg, BIT_FAIL);
        xEventGroupSetBits(s_eg, BIT_IP);
    }
}

static void wifi_init_once(void)
{
    if (s_wifi_inited) {
        return;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        s_wifi_inited = true;
        s_wifi_started = true;
        ESP_LOGI(TAG, "reuse system Wi-Fi (mode %d)", (int)mode);
        wifi_register_handlers();
        return;
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_register_handlers();
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_wifi_inited = true;
}

static void wifi_start(void)
{
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_wifi_started = true;
    }
}

static esp_err_t portal_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan = {0};
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "list");
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
        uint16_t count = 12;
        wifi_ap_record_t rec[12];
        if (esp_wifi_scan_get_ap_records(&count, rec) == ESP_OK) {
            for (int i = 0; i < count; i++) {
                char ssid[33];
                memcpy(ssid, rec[i].ssid, 32);
                ssid[32] = 0;
                if (!ssid[0]) {
                    continue;
                }
                bool seen = false;
                cJSON *it;
                cJSON_ArrayForEach(it, list) {
                    if (strcmp(it->valuestring, ssid) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    cJSON_AddItemToArray(list, cJSON_CreateString(ssid));
                }
            }
        }
    }
    char *printed = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, printed ? printed : "{\"list\":[]}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(printed);
    cJSON_Delete(root);
    return err;
}

static esp_err_t portal_wifi(httpd_req_t *req)
{
    char body[256];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        total += n;
    }
    body[total] = 0;
    cJSON *root = cJSON_Parse(body);
    const char *ssid = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid")) : NULL;
    const char *pass = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "pass")) : NULL;
    bool ok = ssid && ssid[0] && strlen(ssid) <= 32 && (!pass || strlen(pass) <= 64);
    if (ok) {
        utf8_copy(s_ssid, sizeof(s_ssid), ssid);
        utf8_copy(s_psk, sizeof(s_psk), pass ? pass : "");
        nvs_put_str("ssid", s_ssid);
        nvs_put_str("psk", s_psk);
        xEventGroupSetBits(s_eg, BIT_CREDS);
        ESP_LOGI(TAG, "wifi creds saved for %s", s_ssid);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const char *msg = ok ? "{\"ok\":true}" : "{\"ok\":false,\"message\":\"热点名无效\"}";
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static void start_httpd(void)
{
    if (s_httpd) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        s_httpd = NULL;
        return;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = portal_get};
    httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = portal_scan};
    httpd_uri_t save = {.uri = "/wifi", .method = HTTP_POST, .handler = portal_wifi};
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    pager_qa_register_http(s_httpd);
}

static void stop_httpd(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

static void start_ap(void)
{
    wifi_init_once();
    s_expect_sta = false;
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    wifi_start();
    s_ap_on = true;
    start_httpd();
    ESP_LOGI(TAG, "softap %s", s_ap_ssid);
}

static void leave_ap(void)
{
    stop_httpd();
    if (s_ap_on) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_on = false;
    }
}

static void join_sta(void)
{
    wifi_init_once();
    wifi_ap_record_t cur = {0};
    if (s_ssid[0] && esp_wifi_sta_get_ap_info(&cur) == ESP_OK &&
        strncmp((const char *)cur.ssid, s_ssid, sizeof(cur.ssid)) == 0 && sta_has_ip()) {
        ESP_LOGI(TAG, "already on %s", s_ssid);
        s_retry = 0;
        s_expect_sta = true;
        xEventGroupClearBits(s_eg, BIT_FAIL);
        xEventGroupSetBits(s_eg, BIT_IP);
        return;
    }
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, s_psk, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = s_psk[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    if (!s_ap_on) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    wifi_start();
    s_retry = 0;
    s_expect_sta = true;
    xEventGroupClearBits(s_eg, BIT_FAIL);
    if (sta_has_ip()) {
        xEventGroupSetBits(s_eg, BIT_IP);
        ESP_LOGI(TAG, "keep existing ip on %s", s_ssid);
        return;
    }
    xEventGroupClearBits(s_eg, BIT_IP);
    esp_err_t err = esp_wifi_connect();
    ESP_LOGI(TAG, "connecting %s (%s)", s_ssid, esp_err_to_name(err));
    sta_sync_if_connected();
}

static bool discover(void)
{
    esp_err_t init = mdns_init();
    if (init != ESP_OK && init != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns init %s", esp_err_to_name(init));
    }
    mdns_hostname_set("pager");
    int attempts = s_host[0] ? 1 : 2;
    int timeout_ms = s_host[0] ? 800 : 1500;
    for (int attempt = 0; attempt < attempts; attempt++) {
        mdns_result_t *results = NULL;
        esp_err_t err = mdns_query_ptr("_pager", "_tcp", timeout_ms, 4, &results);
        if (err == ESP_OK && results) {
            mdns_result_t *hit = results;
            mdns_ip_addr_t *addr = hit->addr;
            while (addr && addr->addr.type != ESP_IPADDR_TYPE_V4) {
                addr = addr->next;
            }
            if (addr) {
                snprintf(s_host, sizeof(s_host), IPSTR, IP2STR(&addr->addr.u_addr.ip4));
                if (hit->port) {
                    s_port = hit->port;
                }
                s_tls = true;
                if (hit->txt) {
                    for (size_t i = 0; i < hit->txt_count; i++) {
                        if (hit->txt[i].key && strcmp(hit->txt[i].key, "tls") == 0 && hit->txt[i].value) {
                            s_tls = strcmp(hit->txt[i].value, "0") != 0;
                        }
                    }
                }
                ESP_LOGI(TAG, "bridge %s:%u tls=%d", s_host, s_port, s_tls);
                nvs_put_str("host", s_host);
                nvs_handle_t h;
                if (nvs_open("pager", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u16(h, "port", s_port);
                    nvs_set_u8(h, "tls", s_tls ? 1 : 0);
                    nvs_commit(h);
                    nvs_close(h);
                }
                mdns_query_results_free(results);
                return true;
            }
            mdns_query_results_free(results);
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (s_host[0]) {
        ESP_LOGW(TAG, "mdns miss, using saved %s", s_host);
        return true;
    }
    return false;
}

typedef struct {
    char *buf;
    int cap;
    int len;
} http_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_buf_t *dst = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && dst && evt->data_len > 0) {
        int n = evt->data_len;
        if (dst->len + n >= dst->cap) {
            n = dst->cap - 1 - dst->len;
        }
        if (n > 0) {
            memcpy(dst->buf + dst->len, evt->data, n);
            dst->len += n;
            dst->buf[dst->len] = 0;
        }
    }
    return ESP_OK;
}

static bool pair_confirm(const char *code, char *err, size_t err_len)
{
    char url[128];
    char post[256];
    char body[768];
    snprintf(post, sizeof(post),
             "{\"code\":\"%s\",\"name\":\"Pager\",\"platform\":\"esp32s3\",\"certFingerprint\":\"%s\"}",
             code, CERT_FP);
    http_buf_t buf = {.buf = body, .cap = sizeof(body)};
    /* Default HTTP buffer is 512 bytes; the TLS handshake record does not fit.
     * Always use HTTPS here — a saved tls=0 flag POSTs plain HTTP at a TLS port. */
    s_tls = true;
    snprintf(url, sizeof(url), "https://%s:%u/device/pair/confirm", s_host, s_port);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .event_handler = http_evt,
        .user_data = &buf,
        .skip_cert_common_name_check = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .cert_pem = (const char *)pager_cert_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        utf8_copy(err, err_len, "无法发起配对");
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post, strlen(post));
    unsigned largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    esp_err_t rc = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "pair %s status=%d heap=%u url=%s body=%s",
             esp_err_to_name(rc), status, largest, url, body);
    if (rc != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "连不上 %s:%u %s", s_host, s_port, esp_err_to_name(rc));
        utf8_copy(err, err_len, msg);
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    bool ok = root && cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"));
    const char *token = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "token")) : NULL;
    const char *message = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "message")) : NULL;
    if (ok && token && token[0]) {
        utf8_copy(s_token, sizeof(s_token), token);
        nvs_put_str("token", s_token);
        cJSON_Delete(root);
        return true;
    }
    utf8_copy(err, err_len, (message && message[0]) ? message : "配对失败，核对 6 位码");
    cJSON_Delete(root);
    return false;
}

static void ws_stop(void)
{
    esp_websocket_client_handle_t old;
    s_ws_up = false;
    s_ws_connecting = false;
    /* Bump the generation first so a disconnect from the client we are
     * tearing down cannot clear the next connection or show "连不上了". */
    s_ws_gen++;
    old = s_ws;
    s_ws = NULL;
    if (old) {
        esp_websocket_client_stop(old);
        esp_websocket_client_destroy(old);
    }
}

static void apply_approval(cJSON *item)
{
    if (!item) {
        s_appr_on = false;
        return;
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
    const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(item, "summary"));
    const char *risk = cJSON_GetStringValue(cJSON_GetObjectItem(item, "risk"));
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(item, "riskReason"));
    const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(item, "command"));
    cJSON *can = cJSON_GetObjectItem(item, "canApprove");
    utf8_copy(s_appr_id, sizeof(s_appr_id), id ? id : "");
    utf8_copy(s_appr_summary, sizeof(s_appr_summary), summary ? summary : "");
    utf8_copy(s_appr_risk, sizeof(s_appr_risk), risk ? risk : "");
    utf8_copy(s_appr_reason, sizeof(s_appr_reason), reason ? reason : "");
    utf8_copy(s_appr_cmd, sizeof(s_appr_cmd), command ? command : "");
    s_appr_can = !cJSON_IsFalse(can);
    s_appr_on = s_appr_id[0] != 0;
}

static void copy_state(cJSON *root, const char *state_key, const char *detail_key)
{
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(root, state_key));
    const char *detail = cJSON_GetStringValue(cJSON_GetObjectItem(root, detail_key));
    if (state) {
        utf8_copy(s_state, sizeof(s_state), state);
    }
    if (detail) {
        utf8_copy(s_detail, sizeof(s_detail), detail);
    }
}

static void reply_clear(void)
{
    if (s_reply) {
        s_reply[0] = 0;
    }
}

static void reply_set(const char *text)
{
    if (!s_reply) {
        return;
    }
    utf8_copy(s_reply, REPLY_CAP, text ? text : "");
}

static void reply_add(const char *text)
{
    size_t cur;
    size_t add;
    size_t room;
    if (!s_reply || !text || !text[0]) {
        return;
    }
    cur = strlen(s_reply);
    if (cur + 1 >= REPLY_CAP) {
        return;
    }
    room = REPLY_CAP - 1 - cur;
    add = strlen(text);
    if (add > room) {
        add = room;
        while (add > 0 && ((unsigned char)text[add] & 0xC0) == 0x80) {
            add--;
        }
    }
    memcpy(s_reply + cur, text, add);
    s_reply[cur + add] = 0;
}

static void ingest_down(cJSON *root);

static void tools_clear(void)
{
    s_tool_n = 0;
}

static int tool_index(const char *id)
{
    if (!id || !id[0]) {
        return -1;
    }
    for (int i = 0; i < s_tool_n; i++) {
        if (strcmp(s_tools[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static void tool_upsert(const char *id, const char *summary, const char *status)
{
    if (!id || !id[0]) {
        return;
    }
    int idx = tool_index(id);
    if (idx < 0) {
        if (s_tool_n >= TOOL_SLOTS) {
            memmove(&s_tools[0], &s_tools[1], sizeof(tool_slot_t) * (TOOL_SLOTS - 1));
            s_tool_n = TOOL_SLOTS - 1;
        }
        idx = s_tool_n++;
        memset(&s_tools[idx], 0, sizeof(s_tools[idx]));
        utf8_copy(s_tools[idx].id, sizeof(s_tools[idx].id), id);
        utf8_copy(s_tools[idx].status, sizeof(s_tools[idx].status), "pending");
    }
    if (summary && summary[0]) {
        utf8_copy(s_tools[idx].summary, sizeof(s_tools[idx].summary), summary);
    } else if (!s_tools[idx].summary[0]) {
        utf8_copy(s_tools[idx].summary, sizeof(s_tools[idx].summary), "工具");
    }
    if (status && status[0]) {
        utf8_copy(s_tools[idx].status, sizeof(s_tools[idx].status), status);
    }
}

static void tools_pick(char *summary, size_t summary_len, char *status, size_t status_len,
                       int *done_n, int *fail_n)
{
    int done = 0;
    int fail = 0;
    int active = -1;
    int last = -1;
    for (int i = 0; i < s_tool_n; i++) {
        if (strcmp(s_tools[i].status, "completed") == 0) {
            done++;
        } else if (strcmp(s_tools[i].status, "failed") == 0) {
            fail++;
        } else {
            active = i;
        }
        last = i;
    }
    int pick = active >= 0 ? active : last;
    if (summary && summary_len) {
        summary[0] = 0;
        if (pick >= 0) {
            utf8_copy(summary, summary_len, s_tools[pick].summary);
        }
    }
    if (status && status_len) {
        status[0] = 0;
        if (pick >= 0) {
            utf8_copy(status, status_len, s_tools[pick].status);
        }
    }
    if (done_n) {
        *done_n = done;
    }
    if (fail_n) {
        *fail_n = fail;
    }
}

static void handle_json(const char *txt)
{
    cJSON *root = cJSON_Parse(txt);
    if (!root) {
        return;
    }
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "audio_out") == 0) {
        ingest_down(root);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "host") == 0) {
        cJSON *item;
        float cpu = -1, ram_pct = -1, ram_used = -1, ram_total = -1;
        float gpu = -1, vram_used = -1, vram_total = -1, gpu_temp = -1;
        float net_pct = -1, net_down = -1, net_up = -1;
        item = cJSON_GetObjectItem(root, "cpu");
        if (cJSON_IsNumber(item)) {
            cpu = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "ramPct");
        if (cJSON_IsNumber(item)) {
            ram_pct = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "ramUsed");
        if (cJSON_IsNumber(item)) {
            ram_used = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "ramTotal");
        if (cJSON_IsNumber(item)) {
            ram_total = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "gpu");
        if (cJSON_IsNumber(item)) {
            gpu = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "vramUsed");
        if (cJSON_IsNumber(item)) {
            vram_used = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "vramTotal");
        if (cJSON_IsNumber(item)) {
            vram_total = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "gpuTemp");
        if (cJSON_IsNumber(item)) {
            gpu_temp = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "netPct");
        if (cJSON_IsNumber(item)) {
            net_pct = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "netDown");
        if (cJSON_IsNumber(item)) {
            net_down = (float)item->valuedouble;
        }
        item = cJSON_GetObjectItem(root, "netUp");
        if (cJSON_IsNumber(item)) {
            net_up = (float)item->valuedouble;
        }
        ui_set_host(cpu, ram_pct, ram_used, ram_total, gpu, vram_used, vram_total, gpu_temp,
                    net_pct, net_down, net_up);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "meeting") == 0) {
        cJSON *on = cJSON_GetObjectItem(root, "on");
        bool enabled = cJSON_IsTrue(on);
        if (!enabled) {
            s_meet_tail = true;
        }
        pager_audio_set_meeting(enabled);
        ui_set_meeting(enabled);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "volume") == 0) {
        cJSON *item = cJSON_GetObjectItem(root, "value");
        int vol = cJSON_IsNumber(item) ? item->valueint : s_volume;
        if (vol < 0) {
            vol = 0;
        }
        if (vol > 100) {
            vol = 100;
        }
        s_volume = vol;
        pager_audio_set_volume(vol);
        nvs_handle_t nv;
        if (nvs_open("pager", NVS_READWRITE, &nv) == ESP_OK) {
            nvs_set_u8(nv, "vol", (uint8_t)vol);
            nvs_commit(nv);
            nvs_close(nv);
        }
        ui_set_volume(vol);
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type, "voice") == 0) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "value"));
        int idx = voice_index(id);
        if (idx != s_voice) {
            s_voice = idx;
            nvs_handle_t nv;
            if (nvs_open("pager", NVS_READWRITE, &nv) == ESP_OK) {
                nvs_set_u8(nv, "voice", (uint8_t)idx);
                nvs_commit(nv);
                nvs_close(nv);
            }
            ui_set_voice(idx);
            s_prefs_dirty = true;
        }
        cJSON_Delete(root);
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (strcmp(type, "snapshot") == 0) {
        copy_state(root, "state", "stateDetail");
        cJSON *tr = cJSON_GetObjectItem(root, "transcript");
        if (cJSON_IsArray(tr)) {
            reply_clear();
            cJSON *msg;
            cJSON_ArrayForEach(msg, tr) {
                const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
                const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
                if (role && text && strcmp(role, "assistant") == 0 && text[0]) {
                    reply_set(text);
                }
            }
        }
        cJSON *pending = cJSON_GetObjectItem(root, "pending");
        s_appr_on = false;
        if (cJSON_IsArray(pending) && cJSON_GetArraySize(pending) > 0) {
            apply_approval(cJSON_GetArrayItem(pending, 0));
        }
        tools_clear();
        cJSON *tools = cJSON_GetObjectItem(root, "tools");
        if (cJSON_IsArray(tools)) {
            cJSON *item;
            cJSON_ArrayForEach(item, tools) {
                const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
                const char *sum = cJSON_GetStringValue(cJSON_GetObjectItem(item, "summary"));
                const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
                const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
                tool_upsert(tid, (sum && sum[0]) ? sum : title, st);
            }
        }
        cJSON *host = cJSON_GetObjectItem(root, "host");
        if (cJSON_IsObject(host)) {
            cJSON *item;
            float cpu = -1, ram_pct = -1, ram_used = -1, ram_total = -1;
            float gpu = -1, vram_used = -1, vram_total = -1, gpu_temp = -1;
            item = cJSON_GetObjectItem(host, "cpu");
            if (cJSON_IsNumber(item)) {
                cpu = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "ramPct");
            if (cJSON_IsNumber(item)) {
                ram_pct = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "ramUsed");
            if (cJSON_IsNumber(item)) {
                ram_used = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "ramTotal");
            if (cJSON_IsNumber(item)) {
                ram_total = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "gpu");
            if (cJSON_IsNumber(item)) {
                gpu = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "vramUsed");
            if (cJSON_IsNumber(item)) {
                vram_used = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "vramTotal");
            if (cJSON_IsNumber(item)) {
                vram_total = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "gpuTemp");
            if (cJSON_IsNumber(item)) {
                gpu_temp = (float)item->valuedouble;
            }
            float net_pct = -1, net_down = -1, net_up = -1;
            item = cJSON_GetObjectItem(host, "netPct");
            if (cJSON_IsNumber(item)) {
                net_pct = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "netDown");
            if (cJSON_IsNumber(item)) {
                net_down = (float)item->valuedouble;
            }
            item = cJSON_GetObjectItem(host, "netUp");
            if (cJSON_IsNumber(item)) {
                net_up = (float)item->valuedouble;
            }
            ui_set_host(cpu, ram_pct, ram_used, ram_total, gpu, vram_used, vram_total, gpu_temp,
                        net_pct, net_down, net_up);
        }
        cJSON *macros = cJSON_GetObjectItem(root, "macros");
        if (cJSON_IsArray(macros)) {
            cJSON *m0 = cJSON_GetArrayItem(macros, 0);
            cJSON *m1 = cJSON_GetArrayItem(macros, 1);
            const char *id0 = m0 ? cJSON_GetStringValue(cJSON_GetObjectItem(m0, "id")) : NULL;
            const char *l0 = m0 ? cJSON_GetStringValue(cJSON_GetObjectItem(m0, "label")) : NULL;
            const char *id1 = m1 ? cJSON_GetStringValue(cJSON_GetObjectItem(m1, "id")) : NULL;
            const char *l1 = m1 ? cJSON_GetStringValue(cJSON_GetObjectItem(m1, "label")) : NULL;
            utf8_copy(s_macro_slot_id[0], sizeof(s_macro_slot_id[0]), id0 ? id0 : "");
            utf8_copy(s_macro_slot_label[0], sizeof(s_macro_slot_label[0]), l0 ? l0 : "");
            utf8_copy(s_macro_slot_id[1], sizeof(s_macro_slot_id[1]), id1 ? id1 : "");
            utf8_copy(s_macro_slot_label[1], sizeof(s_macro_slot_label[1]), l1 ? l1 : "");
            s_macros_dirty = true;
        }
    } else if (strcmp(type, "toast") == 0) {
        utf8_copy(s_toast_title, sizeof(s_toast_title),
                  cJSON_GetStringValue(cJSON_GetObjectItem(root, "title")));
        utf8_copy(s_toast_body, sizeof(s_toast_body),
                  cJSON_GetStringValue(cJSON_GetObjectItem(root, "body")));
        utf8_copy(s_toast_level, sizeof(s_toast_level),
                  cJSON_GetStringValue(cJSON_GetObjectItem(root, "level")));
        cJSON *ttl = cJSON_GetObjectItem(root, "ttlMs");
        s_toast_ttl = cJSON_IsNumber(ttl) ? ttl->valueint : 6000;
        s_toast_fire = true;
    } else if (strcmp(type, "ring_device") == 0) {
        utf8_copy(s_ring_pat, sizeof(s_ring_pat),
                  cJSON_GetStringValue(cJSON_GetObjectItem(root, "pattern")));
        cJSON *rep = cJSON_GetObjectItem(root, "repeat");
        s_ring_repeat = cJSON_IsNumber(rep) ? rep->valueint : 1;
        s_ring_fire = true;
    } else if (strcmp(type, "state") == 0) {
        copy_state(root, "state", "detail");
    } else if (strcmp(type, "approval") == 0) {
        apply_approval(cJSON_GetObjectItem(root, "approval"));
    } else if (strcmp(type, "approval_resolved") == 0) {
        s_appr_on = false;
    } else if (strcmp(type, "heard") == 0) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        utf8_copy(s_state, sizeof(s_state), "思考中");
        if (text && text[0]) {
            reply_set(text);
            s_heard_hold = true;
        }
    } else if (strcmp(type, "assistant_delta") == 0) {
        if (s_heard_hold) {
            reply_clear();
            s_heard_hold = false;
        }
        reply_add(cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")));
        s_appr_on = false;
    } else if (strcmp(type, "error") == 0) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        copy_state(root, "type", "message");
        utf8_copy(s_state, sizeof(s_state), "出错");
        s_heard_hold = false;
        if (msg && msg[0]) {
            reply_set(msg);
        }
    } else if (strcmp(type, "done") == 0) {
        utf8_copy(s_state, sizeof(s_state), "完成");
        s_heard_hold = false;
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        if (text && text[0]) {
            reply_set(text);
        }
    } else if (strcmp(type, "tool") == 0) {
        cJSON *tool = cJSON_GetObjectItem(root, "tool");
        const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "id"));
        const char *sum = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "summary"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "title"));
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "status"));
        tool_upsert(tid, (sum && sum[0]) ? sum : title, st);
    } else if (strcmp(type, "tool_update") == 0) {
        const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "toolCallId"));
        const char *sum = cJSON_GetStringValue(cJSON_GetObjectItem(root, "summary"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(root, "status"));
        tool_upsert(tid, (sum && sum[0]) ? sum : title, st);
    } else if (strcmp(type, "user") == 0) {
        reply_clear();
        s_heard_hold = false;
    } else if (strcmp(type, "profile") == 0) {
        reply_clear();
        s_heard_hold = false;
        tools_clear();
    }
    s_ui_dirty = true;
    xSemaphoreGive(s_mu);
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    if ((int)(intptr_t)arg != s_ws_gen) {
        return;
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_ws_connecting = false;
        s_ws_up = true;
        s_ws_up_at = esp_timer_get_time();
        s_prefs_dirty = true;
        ESP_LOGI(TAG, "websocket connected");
        ui_hear_status("", "");
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        int64_t dt = s_ws_up_at ? esp_timer_get_time() - s_ws_up_at : 0;
        s_ws_connecting = false;
        s_ws_up = false;
        if (dt > 0 && dt < 2000000) {
            s_quick_fail++;
        }
        ESP_LOGW(TAG, "websocket disconnected quick=%d", s_quick_fail);
        s_ws_retry_at = esp_timer_get_time() + 4000000;
        ui_hear_status("连不上了", "正在重连电脑");
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *evt = data;
        if (evt->op_code != 0x1 && evt->op_code != 0x0) {
            return;
        }
        if (evt->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (s_rx && evt->data_len > 0 && s_rx_len + evt->data_len < RX_CAP - 1) {
            memcpy(s_rx + s_rx_len, evt->data_ptr, evt->data_len);
            s_rx_len += evt->data_len;
        } else if (evt->data_len > 0) {
            ESP_LOGW(TAG, "ws frame dropped len=%d cap=%d", evt->payload_len, RX_CAP);
            s_rx_len = 0;
        }
        if (evt->payload_offset + evt->data_len >= evt->payload_len && s_rx_len > 0) {
            s_rx[s_rx_len] = 0;
            handle_json(s_rx);
        }
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        s_ws_connecting = false;
        ESP_LOGE(TAG, "websocket error");
    }
}

void pager_net_save_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32) {
        return;
    }
    if (pass && strlen(pass) > 64) {
        return;
    }
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        utf8_copy(s_ssid, sizeof(s_ssid), ssid);
        utf8_copy(s_psk, sizeof(s_psk), pass ? pass : "");
        xSemaphoreGive(s_mu);
    } else {
        utf8_copy(s_ssid, sizeof(s_ssid), ssid);
        utf8_copy(s_psk, sizeof(s_psk), pass ? pass : "");
    }
    nvs_put_str("ssid", s_ssid);
    nvs_put_str("psk", s_psk);
    if (s_eg) {
        xEventGroupSetBits(s_eg, BIT_CREDS);
    }
    ESP_LOGI(TAG, "wifi creds saved for %s", ssid);
}

static void ws_start(void)
{
    ws_stop();
    static char uri[128];
    static char headers[220];
    snprintf(uri, sizeof(uri), "%s://%s:%u/device/ws", s_tls ? "wss" : "ws", s_host, s_port);
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", s_token);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = headers,
        .skip_cert_common_name_check = true,
        .buffer_size = 8192,
        .task_stack = 6144,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 20000,
    };
    if (s_tls) {
        cfg.cert_pem = (const char *)pager_cert_pem_start;
    }
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "websocket init failed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event,
                                  (void *)(intptr_t)s_ws_gen);
    s_ws_connecting = true;
    s_ws_connect_by = esp_timer_get_time() + 20000000;
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        s_ws_connecting = false;
        ESP_LOGE(TAG, "websocket start failed, largest internal %u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return;
    }
    s_last_ping = esp_timer_get_time();
    ESP_LOGI(TAG, "websocket start %s", uri);
}

static void send_json(const char *json)
{
    if (s_ws && s_ws_up) {
        esp_websocket_client_send_text(s_ws, json, (int)strlen(json), pdMS_TO_TICKS(2000));
    }
}

static int b64_encode(const uint8_t *in, int n, char *out, int cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int v = in[i] << 16;
        if (i + 1 < n) {
            v |= in[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= in[i + 2];
        }
        if (o + 5 >= cap) {
            return -1;
        }
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_digit(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static int b64_decode(const char *in, uint8_t *out, int cap)
{
    int n = 0;
    while (in[0] && in[1] && in[2] && in[3]) {
        int a = b64_digit(in[0]);
        int b = b64_digit(in[1]);
        int c = in[2] == '=' ? 0 : b64_digit(in[2]);
        int d = in[3] == '=' ? 0 : b64_digit(in[3]);
        if (a < 0 || b < 0 || (in[2] != '=' && c < 0) || (in[3] != '=' && d < 0)) {
            return -1;
        }
        if (n >= cap) {
            return -1;
        }
        out[n++] = (uint8_t)((a << 2) | (b >> 4));
        if (in[2] != '=') {
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)(((b & 15) << 4) | (c >> 2));
        }
        if (in[3] != '=') {
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)(((c & 3) << 6) | d);
        }
        if (in[2] == '=' || in[3] == '=') {
            break;
        }
        in += 4;
    }
    return n;
}

static uint8_t *s_pcm_chunk;
#define PCM_CHUNK 2048
static int s_fed;

static void ingest_down(cJSON *root)
{
    cJSON *seqj = cJSON_GetObjectItem(root, "seq");
    int seq = cJSON_IsNumber(seqj) ? seqj->valueint : 0;
    int last = cJSON_IsTrue(cJSON_GetObjectItem(root, "last"));
    if (seq <= 1 || !pager_audio_playing()) {
        pager_audio_begin();
        s_fed = 0;
    }
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(root, "b64"));
    if (b64 && b64[0] && s_pcm_chunk) {
        int got = b64_decode(b64, s_pcm_chunk, PCM_CHUNK);
        if (got > 1) {
            int took = pager_audio_feed(s_pcm_chunk, got);
            s_fed += took;
            if (took < got) {
                ESP_LOGW(TAG, "speak short %d/%d", took, got);
            }
        }
    }
    if (last) {
        pager_audio_finish();
        ESP_LOGI(TAG, "speak %d bytes", s_fed);
    }
}

bool pager_ui_meeting(bool start)
{
    if (!s_ws_up || s_st != ST_RUN) {
        return false;
    }
    __atomic_store_n(&s_meeting_req, start ? 1 : 2, __ATOMIC_RELEASE);
    return true;
}

void pager_ui_speak(bool on)
{
    s_speak_on = on;
    s_prefs_dirty = true;
}

void pager_ui_voice(int index)
{
    if (index < 0) {
        index = 0;
    }
    if (index >= VOICE_N) {
        index = VOICE_N - 1;
    }
    s_voice = index;
    s_prefs_dirty = true;
}

void pager_ui_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    pager_audio_set_volume(volume);
    s_prefs_dirty = true;
}

void pager_ui_ping_pc(void)
{
    queue_cmd(CMD_PING_PC, NULL, false);
}

void pager_ui_macro(const char *id)
{
    queue_cmd(CMD_MACRO, id, false);
}

static void send_audio(const uint8_t *pcm, int n, int rate, int more)
{
    static char b64[2500];
    static char json[2900];
    int seq = 1;
    int off = 0;
    while (off < n) {
        int chunk = n - off;
        if (chunk > 1800) {
            chunk = 1800;
        }
        int last = off + chunk >= n;
        if (b64_encode(pcm + off, chunk, b64, (int)sizeof(b64)) < 0) {
            break;
        }
        snprintf(json, sizeof(json),
                 "{\"type\":\"audio\",\"seq\":%d,\"rate\":%d,\"last\":%s,\"more\":%s,\"b64\":\"%s\"}",
                 seq, rate, last ? "true" : "false",
                 (last && more) ? "true" : "false", b64);
        send_json(json);
        off += chunk;
        seq++;
    }
    ESP_LOGI(TAG, "sent audio %d bytes in %d chunks more %d", n, seq - 1, more);
}

static void drain_audio(void)
{
    const uint8_t *pcm = NULL;
    int n = 0;
    int rate = 16000;
    int more = 0;
    int why = pager_audio_poll(&pcm, &n, &rate, &more);
    if (!why) {
        return;
    }
    if (why == 1 && s_st == ST_RUN && s_ws_up) {
        bool tail = s_meet_tail;
        if (!more) {
            s_meet_tail = false;
        }
        if (!more && !tail) {
            ui_hear_status("识别中", "发给电脑");
        }
        if (pcm && n > 0) {
            send_audio(pcm, n, rate, more);
        } else if (!more) {
            send_json("{\"type\":\"audio\",\"seq\":1,\"rate\":16000,\"last\":true,\"more\":false,\"b64\":\"\"}");
        }
    } else if (more) {
        /* This block was silence while the key is still held. Keep recording. */
    } else if (why == 1) {
        ui_hear_status("还没连上电脑", "连上后再按住说");
    } else if (why == 2) {
        ui_hear_status("没听到声音", "靠近一点再说");
    } else if (why == 3) {
        ui_hear_status("按短了", "按住再多说一点");
    } else {
        ui_hear_status("麦克风没打开", "请重新开机试试");
    }
    pager_audio_done();
}

static void flush_ui(void)
{
    char state[48], detail[160], id[40], summary[240], risk[16];
    char tool_sum[80], tool_st[16];
    char reason[96], command[180];
    int tool_done = 0, tool_fail = 0;
    bool approval, can;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!s_ui_dirty || s_st != ST_RUN || s_pair_ui) {
        xSemaphoreGive(s_mu);
        return;
    }
    s_ui_dirty = false;
    memcpy(state, s_state, sizeof(state));
    memcpy(detail, s_detail, sizeof(detail));
    if (s_reply && s_reply_view) {
        memcpy(s_reply_view, s_reply, REPLY_CAP);
    }
    memcpy(id, s_appr_id, sizeof(id));
    memcpy(summary, s_appr_summary, sizeof(summary));
    memcpy(risk, s_appr_risk, sizeof(risk));
    memcpy(reason, s_appr_reason, sizeof(reason));
    memcpy(command, s_appr_cmd, sizeof(command));
    approval = s_appr_on;
    can = s_appr_can;
    tools_pick(tool_sum, sizeof(tool_sum), tool_st, sizeof(tool_st), &tool_done, &tool_fail);
    xSemaphoreGive(s_mu);

    if (!s_home_on) {
        ui_show_home();
        s_home_on = true;
    }
    ui_set_state(state, detail);
    ui_set_reply(s_reply_view ? s_reply_view : "");
    ui_set_tools(tool_sum, tool_st, tool_done, tool_fail);
    if (approval) {
        if (strcmp(s_shown_appr, id) != 0) {
            ui_show_approval(id, summary, risk, can, reason, command);
            utf8_copy(s_shown_appr, sizeof(s_shown_appr), id);
        }
    } else if (s_shown_appr[0]) {
        s_shown_appr[0] = 0;
        ui_show_approval(NULL, NULL, NULL, false, NULL, NULL);
    }
    if (s_macros_dirty) {
        s_macros_dirty = false;
        ui_set_macro_slots(
            s_macro_slot_id[0][0] ? s_macro_slot_id[0] : NULL,
            s_macro_slot_label[0][0] ? s_macro_slot_label[0] : NULL,
            s_macro_slot_id[1][0] ? s_macro_slot_id[1] : NULL,
            s_macro_slot_label[1][0] ? s_macro_slot_label[1] : NULL);
    }
    if (s_toast_fire) {
        s_toast_fire = false;
        ui_show_toast(s_toast_title, s_toast_body, s_toast_level, s_toast_ttl);
    }
    if (s_ring_fire) {
        s_ring_fire = false;
        ui_ring_device(s_ring_pat[0] ? s_ring_pat : "short", s_ring_repeat);
    }
}

static void enter_provision(const char *reason)
{
    ws_stop();
    s_home_on = false;
    ui_unblock_wifi();
    s_st = ST_PROVISION;
    if (reason && reason[0]) {
        char line[140];
        snprintf(line, sizeof(line), "%s\n热点 %s\n浏览器打开 192.168.4.1", reason, s_ap_ssid);
        ui_show_wifi(line);
    } else {
        ui_show_provision(s_ap_ssid);
    }
    start_ap();
}

static void clear_token(void)
{
    s_token[0] = 0;
    nvs_put_str("token", "");
    ws_stop();
    s_quick_fail = 0;
}

static void handle_cmd(const pager_cmd_t *cmd)
{
    if (cmd->type == CMD_QUIT) {
        return;
    }
    if (cmd->type == CMD_RESET_WIFI) {
        s_ssid[0] = 0;
        s_psk[0] = 0;
        nvs_put_str("ssid", "");
        nvs_put_str("psk", "");
        enter_provision("重新配网");
        return;
    }
    if (cmd->type == CMD_RESET_PAIR) {
        clear_token();
        s_home_on = false;
        s_pair_ui = true;
        if (s_host[0]) {
            s_st = ST_PAIR;
            ui_show_pair(s_host);
        } else {
            s_st = ST_DISCOVER;
        }
        return;
    }
    if (cmd->type == CMD_RETRY) {
        s_host[0] = 0;
        s_pair_ui = true;
        s_st = ST_DISCOVER;
        ui_pair_status("正在重新寻找电脑");
        return;
    }
    if (cmd->type == CMD_CODE) {
        if (!s_host[0]) {
            ui_pair_status("还在找电脑，稍后再按确定");
            return;
        }
        s_pair_ui = true;
        s_st = ST_PAIR;
        char err[96];
        if (pair_confirm(cmd->text, err, sizeof(err))) {
            s_pair_ui = false;
            ui_pair_status("配对成功，正在连接");
            s_st = ST_LINK;
        } else {
            ui_pair_status(err);
        }
        return;
    }
    if ((cmd->type == CMD_ALLOW || cmd->type == CMD_DENY) && s_st == ST_RUN) {
        if (cmd->allow && (!s_appr_can || strcmp(s_appr_risk, "L3") == 0)) {
            ESP_LOGW(TAG, "refusing device approval for %s risk=%s", cmd->text, s_appr_risk);
            return;
        }
        char json[160];
        snprintf(json, sizeof(json), "{\"type\":\"approval\",\"id\":\"%s\",\"decision\":\"%s\"}",
                 cmd->text, cmd->allow ? "allow" : "deny");
        send_json(json);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_appr_on = false;
        utf8_copy(s_state, sizeof(s_state), cmd->allow ? "已批准" : "已拒绝");
        s_ui_dirty = true;
        xSemaphoreGive(s_mu);
        return;
    }
    if (cmd->type == CMD_PING_PC && s_st == ST_RUN && s_ws_up) {
        send_json("{\"type\":\"ping_pc\"}");
        return;
    }
    if (cmd->type == CMD_MACRO && s_st == ST_RUN && s_ws_up && cmd->text[0]) {
        char json[120];
        snprintf(json, sizeof(json), "{\"type\":\"macro\",\"id\":\"%s\"}", cmd->text);
        send_json(json);
    }
}

/* When the screen task stops ticking, print the saved CPU frame of the
 * blocked tasks so the stall point can be resolved with addr2line. */
static void dump_blocked_frame(TaskHandle_t task, const char *tag)
{
    if (!task) {
        return;
    }
    uint32_t *sp = *(uint32_t **)task;
    ESP_LOGI(TAG, "%s state %d sp %p", tag, (int)eTaskGetState(task), (void *)sp);
    if (!sp || ((uintptr_t)sp & 3) != 0) {
        return;
    }
    ESP_LOGI(TAG, "%s fr %08x %08x %08x %08x %08x %08x %08x %08x", tag,
             (unsigned)sp[0], (unsigned)sp[1], (unsigned)sp[2], (unsigned)sp[3],
             (unsigned)sp[4], (unsigned)sp[5], (unsigned)sp[6], (unsigned)sp[7]);
    /* Windowed ABI: sp[4] is the saved A1 (caller stack pointer). */
    uint32_t *a1 = (uint32_t *)sp[4];
    if (a1 && ((uintptr_t)a1 & 3) == 0 && (uintptr_t)a1 > 0x3FC00000 && (uintptr_t)a1 < 0x3FFE0000) {
        ESP_LOGI(TAG, "%s up %08x %08x %08x %08x %08x %08x %08x %08x", tag,
                 (unsigned)a1[-4], (unsigned)a1[-3], (unsigned)a1[-2], (unsigned)a1[-1],
                 (unsigned)a1[0], (unsigned)a1[1], (unsigned)a1[2], (unsigned)a1[3]);
        ESP_LOGI(TAG, "%s up2 %08x %08x %08x %08x %08x %08x %08x %08x", tag,
                 (unsigned)a1[4], (unsigned)a1[5], (unsigned)a1[6], (unsigned)a1[7],
                 (unsigned)a1[8], (unsigned)a1[9], (unsigned)a1[10], (unsigned)a1[11]);
    }
}

static void net_task(void *arg)
{
    (void)arg;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Pager-%02X%02X", mac[4], mac[5]);
    if (s_ssid[0]) {
        s_st = ST_JOIN;
        wifi_init_once();
        char line[64];
        snprintf(line, sizeof(line), "正在连接 %s", s_ssid);
        ui_show_wifi(line);
        if (sta_has_ip()) {
            xEventGroupSetBits(s_eg, BIT_IP);
        }
        join_sta();
    } else {
        enter_provision(NULL);
    }

    while (!s_net_quit) {
        drain_audio();
        pager_cmd_t cmd;
        if (xQueueReceive(s_q, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (cmd.type == CMD_QUIT) {
                break;
            }
            handle_cmd(&cmd);
        }
        EventBits_t bits = xEventGroupGetBits(s_eg);
        if (bits & BIT_CREDS) {
            xEventGroupClearBits(s_eg, BIT_CREDS);
            char line[64];
            snprintf(line, sizeof(line), "正在连接 %s", s_ssid);
            ui_show_wifi(line);
            s_home_on = false;
            s_st = ST_JOIN;
            join_sta();
        }

        switch (s_st) {
        case ST_PROVISION:
            break;
        case ST_JOIN:
            sta_sync_if_connected();
            bits = xEventGroupGetBits(s_eg);
            if (bits & BIT_IP) {
                xEventGroupClearBits(s_eg, BIT_IP | BIT_FAIL);
                leave_ap();
#if CONFIG_PAGER_QA
                start_httpd();
                esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (sta_netif) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                        ESP_LOGI(TAG, "qa http " IPSTR, IP2STR(&ip_info.ip));
                    }
                }
#endif
                ui_show_wifi("");
                s_st = ST_DISCOVER;
                vTaskDelay(pdMS_TO_TICKS(200));
            } else if (bits & BIT_FAIL) {
                xEventGroupClearBits(s_eg, BIT_FAIL);
                enter_provision("连不上这个热点，请重新填写");
            }
            break;
        case ST_DISCOVER:
            if (s_pair_ui && s_host[0] && !s_token[0]) {
                s_st = ST_PAIR;
                ui_show_pair(s_host);
                break;
            }
            if (discover()) {
                if (s_pair_ui || !s_token[0]) {
                    s_st = ST_PAIR;
                    ui_show_pair(s_host);
                } else {
                    s_st = ST_LINK;
                }
            } else {
                ui_show_pair("");
                ui_pair_status("没找到电脑。先在电脑上打开 Pager Bridge，再按重找");
                s_st = ST_PAIR;
            }
            break;
        case ST_PAIR:
            break;
        case ST_LINK:
            s_pair_ui = false;
            s_ws_retry_at = esp_timer_get_time() + 15000000;
            ws_start();
            s_home_on = false;
            ui_show_home();
            s_home_on = true;
            ui_set_state("连接中", s_host);
            vTaskDelay(pdMS_TO_TICKS(180));
            pager_net_ensure_audio();
            s_st = ST_RUN;
            break;
        case ST_RUN:
            if (!s_ws_up && esp_timer_get_time() > s_ws_retry_at &&
                !(s_ws_connecting && esp_timer_get_time() < s_ws_connect_by)) {
                s_ws_retry_at = esp_timer_get_time() + 15000000;
                ws_start();
            }
            int meet = __atomic_exchange_n(&s_meeting_req, 0, __ATOMIC_ACQ_REL);
            if (meet && s_ws_up) {
                send_json(meet == 2
                              ? "{\"type\":\"meeting\",\"action\":\"stop\"}"
                              : "{\"type\":\"meeting\",\"action\":\"start\"}");
            }
            if (s_ws_up && s_prefs_dirty &&
                esp_timer_get_time() - s_ws_up_at > 800000) {
                char prefs[96];
                int voice = s_voice;
                if (voice < 0 || voice >= VOICE_N) {
                    voice = 0;
                }
                snprintf(prefs, sizeof(prefs),
                         "{\"type\":\"prefs\",\"speak\":%s,\"volume\":%d,\"voice\":\"%s\"}",
                         s_speak_on ? "true" : "false", s_volume, VOICE_ID[voice]);
                send_json(prefs);
                s_prefs_dirty = false;
                ESP_LOGI(TAG, "announce %s vol %d voice %s",
                         s_speak_on ? "on" : "off", s_volume, VOICE_ID[voice]);
            }
            if (s_ws_up && esp_timer_get_time() - s_ws_up_at > 3000000) {
                s_quick_fail = 0;
            }
            if (s_ws_up && esp_timer_get_time() - s_last_ping > 15000000) {
                send_json("{\"type\":\"ping\"}");
                s_last_ping = esp_timer_get_time();
            }
            flush_ui();
            {
                static int64_t beat_at;
                int64_t now = esp_timer_get_time();
                if (now - beat_at > 2000000) {
                    extern volatile uint32_t pager_ui_beat;
                    extern volatile uint32_t pager_lcd_stage;
                    static uint32_t last_beat;
                    static int dumped;
                    uint32_t beat = pager_ui_beat;
                    ESP_LOGI(TAG, "ui beat %u (+%u) int_largest %u lcd %u",
                             (unsigned)beat, (unsigned)(beat - last_beat),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                             (unsigned)pager_lcd_stage);
                    if (beat == last_beat && !dumped) {
                        dumped = 1;
                        TaskHandle_t lvgl = xTaskGetHandle("lvgl");
                        if (lvgl) {
                            dump_blocked_frame(lvgl, "lvgl");
                        }
                        TaskHandle_t ws = xTaskGetHandle("websocket_task");
                        if (ws) {
                            dump_blocked_frame(ws, "ws");
                        }
                    }
                    last_beat = beat;
                    beat_at = now;
                }
            }
            break;
        }
    }
    ws_stop();
    s_net_task = NULL;
    s_net_task_running = false;
    vTaskDelete(NULL);
}

void pager_net_prepare(void)
{
    s_eg = xEventGroupCreate();
    s_q = xQueueCreate(8, sizeof(pager_cmd_t));
    s_mu = xSemaphoreCreateMutex();
    s_reply = heap_caps_malloc(REPLY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_reply_view = heap_caps_malloc(REPLY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_reply) {
        s_reply[0] = 0;
    }
    if (s_reply_view) {
        s_reply_view[0] = 0;
    }
    s_pcm_chunk = heap_caps_malloc(PCM_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_chunk) {
        s_pcm_chunk = malloc(PCM_CHUNK);
    }
    s_rx = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx) {
        s_rx = malloc(RX_CAP);
    }
    nvs_load();
}

void pager_net_start(void)
{
    if (s_net_task_running) {
        return;
    }
    s_net_quit = false;
    s_net_task_running = true;
    xTaskCreatePinnedToCore(net_task, "pager_net", 12288, NULL, 5, &s_net_task, 0);
}

void pager_net_stop(void)
{
    if (!s_net_task_running) {
        return;
    }
    s_net_quit = true;
    pager_cmd_t cmd = { .type = CMD_QUIT };
    if (s_q) {
        xQueueSend(s_q, &cmd, pdMS_TO_TICKS(100));
    }
    ws_stop();
    for (int i = 0; i < 80 && s_net_task_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void pager_net_resume(void)
{
    if (!s_net_task_running) {
        pager_net_start();
    } else {
        s_net_quit = false;
    }
}
