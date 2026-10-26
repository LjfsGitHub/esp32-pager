#include "pager_qa.h"

#include "pager_audio.h"
#include "pager_net.h"
#include "pager_ui.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pager_qa";

#if CONFIG_PAGER_QA

static esp_err_t qa_ping(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"service\":\"pager_qa\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t qa_scene(httpd_req_t *req)
{
    char body[128];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, (int)sizeof(body) - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    body[total] = 0;
    const char *name = body;
    if (strstr(body, "provision")) {
        name = "provision";
    } else if (strstr(body, "wifi")) {
        name = "wifi";
    } else if (strstr(body, "pair")) {
        name = "pair";
    } else if (strstr(body, "home")) {
        name = "home";
    } else if (strstr(body, "boot")) {
        name = "boot";
    }
    bool ok = pager_qa_set_scene(name);
    vTaskDelay(pdMS_TO_TICKS(500));
    httpd_resp_set_type(req, "application/json");
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"scene\":\"%.64s\"}", ok ? "true" : "false", name);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t qa_fp(httpd_req_t *req)
{
    char hex[256];
    bool ok = pager_qa_screen_fingerprint(hex, sizeof(hex));
    httpd_resp_set_type(req, "application/json");
    char resp[320];
    if (!ok) {
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"grid\":8,\"hex\":\"%s\"}", hex);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t qa_status(httpd_req_t *req)
{
    char st[384];
    if (!pager_qa_status_json(st, sizeof(st))) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, st, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t qa_swipe(httpd_req_t *req)
{
    char body[32];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, (int)sizeof(body) - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    body[total] = 0;
    int toward_right = strstr(body, "left") ? 0 : 1;
    ui_qa_request_swipe(toward_right);
    vTaskDelay(pdMS_TO_TICKS(200));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
                           toward_right ? "{\"ok\":true,\"page\":\"stats\"}" : "{\"ok\":true,\"page\":\"approval\"}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t qa_pairtry(httpd_req_t *req)
{
    pager_ui_submit_code("000000");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"queued\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Simulate holding the side key for N ms (default 1200) and record. */
static esp_err_t qa_ptt(httpd_req_t *req)
{
    char body[16];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, (int)sizeof(body) - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    body[total] = 0;
    int ms = atoi(body);
    ui_qa_request_ptt(ms);
    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"held_ms\":%d}",
             (ms < 300) ? 1200 : (ms > 11000 ? 11000 : ms));
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* Open or close the settings page: body "close" closes, anything else opens. */
static esp_err_t qa_settings(httpd_req_t *req)
{
    char body[16];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, (int)sizeof(body) - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    body[total] = 0;
    int open = strstr(body, "close") ? 0 : 1;
    ui_qa_request_settings(open);
    vTaskDelay(pdMS_TO_TICKS(200));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
                           open ? "{\"ok\":true,\"settings\":\"open\"}"
                                : "{\"ok\":true,\"settings\":\"closed\"}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t qa_audio(httpd_req_t *req)
{
    char detail[128];
    bool ok = pager_qa_audio_selftest(detail, sizeof(detail));
    httpd_resp_set_type(req, "application/json");
    char resp[192];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"detail\":\"%s\"}", ok ? "true" : "false", detail);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static esp_err_t qa_shot(httpd_req_t *req)
{
    uint8_t *rgb = NULL;
    int w = 0, h = 0, stride = 0;
    if (!ui_qa_screenshot(&rgb, &w, &h, &stride) || !rgb || w <= 0 || h <= 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"err\":\"shot\"}", HTTPD_RESP_USE_STRLEN);
    }
    int row_bgr = w * 3;
    int row_pad = (4 - (row_bgr % 4)) % 4;
    int row_size = row_bgr + row_pad;
    uint32_t pixel_bytes = (uint32_t)row_size * (uint32_t)h;
    uint32_t file_size = 54 + pixel_bytes;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_le32(hdr + 2, file_size);
    put_le32(hdr + 10, 54);
    put_le32(hdr + 14, 40);
    put_le32(hdr + 18, (uint32_t)w);
    put_le32(hdr + 22, (uint32_t)h);
    put_le16(hdr + 26, 1);
    put_le16(hdr + 28, 24);
    put_le32(hdr + 34, pixel_bytes);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (const char *)hdr, 54);
    uint8_t *row = malloc((size_t)row_size);
    if (!row) {
        ui_qa_screenshot_free(rgb);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_ERR_NO_MEM;
    }
    memset(row, 0, (size_t)row_size);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = rgb + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            uint16_t p = (uint16_t)src[x * 2] | ((uint16_t)src[x * 2 + 1] << 8);
            uint8_t r = (uint8_t)(((p >> 11) & 0x1f) * 255 / 31);
            uint8_t g = (uint8_t)(((p >> 5) & 0x3f) * 255 / 63);
            uint8_t b = (uint8_t)((p & 0x1f) * 255 / 31);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        if (httpd_resp_send_chunk(req, (const char *)row, row_size) != ESP_OK) {
            break;
        }
    }
    free(row);
    ui_qa_screenshot_free(rgb);
    return httpd_resp_send_chunk(req, NULL, 0);
}

void pager_qa_register_http(httpd_handle_t server)
{
    if (!server) {
        return;
    }
    httpd_uri_t ping = {.uri = "/qa/ping", .method = HTTP_GET, .handler = qa_ping};
    httpd_uri_t scene = {.uri = "/qa/scene", .method = HTTP_POST, .handler = qa_scene};
    httpd_uri_t fp = {.uri = "/qa/fp", .method = HTTP_GET, .handler = qa_fp};
    httpd_uri_t status = {.uri = "/qa/status", .method = HTTP_GET, .handler = qa_status};
    httpd_uri_t audio = {.uri = "/qa/audio", .method = HTTP_GET, .handler = qa_audio};
    httpd_uri_t pairtry = {.uri = "/qa/pairtry", .method = HTTP_POST, .handler = qa_pairtry};
    httpd_uri_t shot = {.uri = "/qa/shot", .method = HTTP_GET, .handler = qa_shot};
    httpd_uri_t swipe = {.uri = "/qa/swipe", .method = HTTP_POST, .handler = qa_swipe};
    httpd_uri_t ptt = {.uri = "/qa/ptt", .method = HTTP_POST, .handler = qa_ptt};
    httpd_uri_t settings = {.uri = "/qa/settings", .method = HTTP_POST, .handler = qa_settings};
    httpd_register_uri_handler(server, &ping);
    httpd_register_uri_handler(server, &scene);
    httpd_register_uri_handler(server, &fp);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &audio);
    httpd_register_uri_handler(server, &pairtry);
    httpd_register_uri_handler(server, &shot);
    httpd_register_uri_handler(server, &swipe);
    httpd_register_uri_handler(server, &ptt);
    httpd_register_uri_handler(server, &settings);
    ESP_LOGI(TAG, "HTTP /qa/* registered");
}

bool pager_qa_set_scene(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    if (strcmp(name, "provision") == 0) {
        ui_show_provision("Pager-QA");
        return true;
    }
    if (strcmp(name, "wifi") == 0) {
        ui_unblock_wifi();
        ui_show_wifi("QA: WiFi 状态页");
        return true;
    }
    if (strcmp(name, "approval") == 0) {
        ui_show_approval("qa", "检查这条操作", "L2", true, "只是界面测试", NULL);
        return true;
    }
    if (strcmp(name, "pair") == 0) {
        ui_show_pair("");
        ui_pair_status("QA 配对页");
        return true;
    }
    if (strcmp(name, "home") == 0) {
        ui_show_home();
        return true;
    }
    if (strcmp(name, "boot") == 0) {
        ui_show_home();
        return true;
    }
    return false;
}

bool pager_qa_screen_fingerprint(char *hex_out, size_t hex_size)
{
    if (!ui_qa_fingerprint(hex_out, hex_size)) {
        ESP_LOGW(TAG, "fp: capture failed");
        return false;
    }
    return true;
}

bool pager_qa_status_json(char *out, size_t out_size)
{
    if (!out || out_size < 64) {
        return false;
    }
    snprintf(out, out_size,
             "{\"ok\":true,\"qa\":true,\"scene\":\"%s\",\"note\":\"use /qa/scene to change\"}",
             ui_qa_scene_name());
    return true;
}

bool pager_qa_audio_selftest(char *detail, size_t detail_size)
{
    if (detail && detail_size) {
        detail[0] = 0;
    }
    pager_audio_chime(1);
    if (detail && detail_size) {
        snprintf(detail, detail_size, "chime");
    }
    return true;
}

/* Console line handler: lets the bench inject Wi-Fi credentials without
 * walking through the phone portal, and pull the current frame over the
 * UART when the device is not on Wi-Fi yet. */
static void qa_serial_shot(void)
{
    uint8_t *rgb = NULL;
    int w = 0, h = 0, stride = 0;
    if (!ui_qa_screenshot(&rgb, &w, &h, &stride) || !rgb || w <= 0 || h <= 0) {
        printf("qa.shot fail\n");
        fflush(stdout);
        return;
    }
    printf("qa.shot %d %d %d rgb565\n", w, h, stride);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = rgb + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            printf("%02x%02x", row[x * 2], row[x * 2 + 1]);
        }
        printf("\n");
    }
    printf("qa.shot end\n");
    fflush(stdout);
    ui_qa_screenshot_free(rgb);
}

static void qa_serial_line(char *line)
{
    if (strncmp(line, "qa.wifi ", 8) == 0) {
        char *ssid = line + 8;
        char *pass = strchr(ssid, ' ');
        if (pass) {
            *pass++ = 0;
        }
        if (ssid[0]) {
            ESP_LOGI(TAG, "qa.wifi %s", ssid);
            pager_net_save_wifi(ssid, pass);
        }
        return;
    }
    if (strncmp(line, "qa.shot", 7) == 0) {
        qa_serial_shot();
        return;
    }
    if (strncmp(line, "qa.scene ", 9) == 0) {
        const char *name = line + 9;
        ESP_LOGI(TAG, "qa.scene %s", name);
        if (!pager_qa_set_scene(name)) {
            printf("qa.scene fail\n");
            fflush(stdout);
        } else {
            printf("qa.scene ok\n");
            fflush(stdout);
        }
        return;
    }
    if (strncmp(line, "qa.swipe ", 9) == 0) {
        ui_qa_request_swipe(strstr(line + 9, "left") ? 0 : 1);
        return;
    }
    if (strncmp(line, "qa.ptt ", 7) == 0) {
        ui_qa_request_ptt(atoi(line + 7));
        return;
    }
    if (strncmp(line, "qa.settings", 11) == 0) {
        ui_qa_request_settings(strstr(line, "close") == NULL);
        return;
    }
    ESP_LOGI(TAG, "qa: unknown cmd");
}

static void qa_serial_task(void *arg)
{
    static char line[160];
    size_t n = 0;
    while (1) {
        /* The round board's console primary is UART0 (unwired); its USB
         * serial is only the secondary. The console owns the peripheral,
         * so poll the RX FIFO directly at the register level instead of
         * installing a second driver on it. */
        if (!usb_serial_jtag_ll_rxfifo_data_available()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint8_t c = 0;
        usb_serial_jtag_ll_read_rxfifo(&c, 1);
        if (c == '\n' || c == '\r') {
            if (n > 0) {
                line[n] = 0;
                n = 0;
                qa_serial_line(line);
            }
            continue;
        }
        if (c >= 0x20 && n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
}

void pager_qa_init(void)
{
    ESP_LOGI(TAG, "QA harness enabled");
    xTaskCreate(qa_serial_task, "qa_uart", 4096, NULL, 3, NULL);
}

#else

void pager_qa_register_http(httpd_handle_t server)
{
    (void)server;
}

bool pager_qa_set_scene(const char *name)
{
    (void)name;
    return false;
}

bool pager_qa_screen_fingerprint(char *hex_out, size_t hex_size)
{
    (void)hex_out;
    (void)hex_size;
    return false;
}

bool pager_qa_status_json(char *out, size_t out_size)
{
    (void)out;
    (void)out_size;
    return false;
}

bool pager_qa_audio_selftest(char *detail, size_t detail_size)
{
    (void)detail;
    (void)detail_size;
    return false;
}

void pager_qa_init(void)
{
}

void pager_qa_arm_autostart(void *phone)
{
    (void)phone;
}

#endif
