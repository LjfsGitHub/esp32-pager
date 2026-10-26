#include "pager_audio.h"

#include "pager_ui.h"

#include "bsp/esp-bsp.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pager_mic";

#define RATE 16000
#define CH 2
#define BOUNCE 640

static volatile bool s_ptt;
static volatile int s_meeting;
static bool s_recording;
static bool s_sent_any;
static int s_cap;
static uint8_t *s_buf[2];
static int s_rec_i;
static int s_fill;
static int s_send_i = -1;
static int s_send_n;
static int s_send_why;
static bool s_send_more;
static bool s_mic_ok;
static bool s_spk_ok;
static int s_vol = 75;
static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static uint8_t s_bounce[BOUNCE] __attribute__((aligned(16)));
static uint8_t *s_ring;
#define RING_CAP (RATE * 2 * 8)
static int s_w;
static int s_r;
static volatile int s_pcm_fill;
static volatile int s_play_done;
static volatile int s_gen;
static volatile int s_play_state;
static int s_need_unmute;
static int s_played;
static int s_gap_ms;
static volatile bool s_codec_wanted = true;
static bool s_audio_task_started;

void pager_ui_ptt(bool down)
{
    if (down) {
        s_codec_wanted = true;
    }
    s_ptt = down;
}

void pager_audio_set_meeting(bool on)
{
    __atomic_store_n(&s_meeting, on ? 1 : 0, __ATOMIC_RELEASE);
}

void pager_audio_arm(void)
{
}

static int open_mic(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    if (bsp_audio_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed");
        return -1;
    }
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "mic init failed");
        return -1;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = CH,
        .sample_rate = RATE,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed");
        return -1;
    }
    esp_codec_dev_set_in_gain(s_mic, 30.0);
    /* Reuse the segment buffers from the last session. Reallocating on every
     * PTT press leaked ~768KB of PSRAM each time and the next press then
     * failed to allocate, which put the audio task into a retry loop. The
     * buffers are the only thing reused here — the codec device above MUST
     * be recreated, close_codecs() released it. */
    if (s_buf[0] && s_buf[1] && s_cap) {
        ESP_LOGI(TAG, "mic ready %d Hz, segment %d s x2 (reuse)", RATE,
                 (int)(s_cap / RATE / CH / 2));
        return 0;
    }
    const int seconds[] = {12, 8, 5};
    for (int t = 0; t < 3; t++) {
        int cap = RATE * CH * 2 * seconds[t];
        uint8_t *a = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a) {
            a = malloc(cap);
        }
        if (!b) {
            b = malloc(cap);
        }
        if (a && b) {
            s_buf[0] = a;
            s_buf[1] = b;
            s_cap = cap;
            ESP_LOGI(TAG, "mic ready %d Hz, segment %d s x2", RATE, seconds[t]);
            return 0;
        }
        free(a);
        free(b);
    }
    ESP_LOGE(TAG, "pcm alloc failed");
    return -1;
}

static int open_speaker(void)
{
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return -1;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = CH,
        .sample_rate = RATE,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return -1;
    }
    esp_codec_dev_set_out_vol(s_spk, s_vol);
    esp_codec_dev_set_out_mute(s_spk, false);
    s_ring = s_ring ? s_ring : heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "play buffer failed");
        return -1;
    }
    ESP_LOGI(TAG, "speaker ready vol %d", s_vol);
    return 0;
}

static void close_codecs(void)
{
    s_recording = false;
    __atomic_store_n(&s_play_state, 0, __ATOMIC_RELEASE);
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        s_mic = NULL;
    }
    if (s_spk) {
        esp_codec_dev_set_out_mute(s_spk, true);
        esp_codec_dev_close(s_spk);
        s_spk = NULL;
    }
    s_mic_ok = false;
    s_spk_ok = false;
}

void pager_audio_suspend(void)
{
    s_ptt = false;
    s_codec_wanted = false;
    close_codecs();
}

void pager_audio_resume(void)
{
    s_codec_wanted = true;
}

void pager_audio_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    s_vol = volume;
    /* Never hold the screen lock here: this runs in the websocket task and
     * the codec write is plain I2C, not an LVGL call. Holding the lock froze
     * the whole UI the moment the first volume message arrived. */
    if (s_spk_ok && s_spk) {
        esp_codec_dev_set_out_vol(s_spk, s_vol);
    }
    ESP_LOGI(TAG, "volume %d", s_vol);
}

void pager_audio_begin(void)
{
    if (!s_spk_ok || !s_ring) {
        ESP_LOGW(TAG, "speak begin skipped spk %d", s_spk_ok ? 1 : 0);
        return;
    }
    __atomic_store_n(&s_play_state, 2, __ATOMIC_RELEASE);
    for (int i = 0; i < 50; i++) {
        if (__atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    __atomic_fetch_add(&s_gen, 1, __ATOMIC_RELEASE);
    s_w = 0;
    s_r = 0;
    s_played = 0;
    s_gap_ms = 0;
    s_need_unmute = 1;
    __atomic_store_n(&s_pcm_fill, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_play_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_play_state, 1, __ATOMIC_RELEASE);
}

int pager_audio_feed(const uint8_t *pcm, int n)
{
    if (!pcm || n < 2 || !s_ring) {
        return 0;
    }
    if (__atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE) != 1) {
        return 0;
    }
    n &= ~1;
    int accepted = 0;
    int waited = 0;
    int gen = __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE);
    while (n >= 2 && waited <= 300) {
        if (__atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE) != 1 ||
            __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE) != gen) {
            break;
        }
        int space = RING_CAP - __atomic_load_n(&s_pcm_fill, __ATOMIC_ACQUIRE);
        space &= ~1;
        if (space < 2) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
            continue;
        }
        int take = n < space ? n : space;
        int first = RING_CAP - s_w;
        if (first > take) {
            first = take;
        }
        memcpy(s_ring + s_w, pcm, first);
        if (take > first) {
            memcpy(s_ring, pcm + first, take - first);
        }
        s_w += take;
        if (s_w >= RING_CAP) {
            s_w -= RING_CAP;
        }
        __atomic_fetch_add(&s_pcm_fill, take, __ATOMIC_RELEASE);
        pcm += take;
        n -= take;
        accepted += take;
        waited = 0;
    }
    if (n >= 2) {
        ESP_LOGW(TAG, "speak overrun, dropped %d", n);
    }
    return accepted;
}

void pager_audio_finish(void)
{
    __atomic_store_n(&s_play_done, 1, __ATOMIC_RELEASE);
}

bool pager_audio_playing(void)
{
    return __atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE) == 1;
}

void pager_audio_play(const uint8_t *pcm, int n)
{
    if (!pcm || n < 2) {
        return;
    }
    pager_audio_begin();
    pager_audio_feed(pcm, n);
    pager_audio_finish();
    ESP_LOGI(TAG, "play %d bytes", n);
}

static int read_bounce(uint8_t *dst, int n)
{
    int got = 0;
    while (got < n) {
        int chunk = n - got;
        if (chunk > BOUNCE) {
            chunk = BOUNCE;
        }
        chunk &= ~3;
        if (chunk <= 0) {
            break;
        }
        int rd = esp_codec_dev_read(s_mic, s_bounce, chunk);
        if (rd != ESP_CODEC_DEV_OK) {
            return -1;
        }
        memcpy(dst + got, s_bounce, chunk);
        got += chunk;
    }
    return 0;
}

static int send_busy(void)
{
    return __atomic_load_n(&s_send_i, __ATOMIC_ACQUIRE);
}

/* Downmix the buffer being recorded and hand it to the network task.
 * more: the key is still down, so recording continues in the other block. */
static void seal(bool more)
{
    int frames = s_fill / (CH * 2);
    int16_t *s = (int16_t *)s_buf[s_rec_i];
    int64_t e0 = 0;
    int64_t e1 = 0;
    int pick = 0;
    int peak = 0;
    int why = 3;
    int mono_n = 0;
    if (frames > 0 && s) {
        for (int i = 0; i < frames; i++) {
            int a = s[i * 2];
            int b = s[i * 2 + 1];
            e0 += a < 0 ? -a : a;
            e1 += b < 0 ? -b : b;
        }
        pick = e1 > e0 ? 1 : 0;
        for (int i = 0; i < frames; i++) {
            int v = s[i * 2 + pick];
            s[i] = (int16_t)v;
            int a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        mono_n = frames * 2;
        if (frames * 1000 / RATE < 280) {
            why = 3;
        } else if (peak < 260) {
            /* Quiet rooms sit around 100-200 ambient; 500 rejected real
             * speech and kept telling the user to hold the key again. */
            why = 2;
        } else {
            why = 1;
        }
    }
    ESP_LOGI(TAG, "segment %d ms peak %d ch %d more %d",
             frames * 1000 / RATE, peak, pick, more ? 1 : 0);
    if (why != 1 && more) {
        s_fill = 0;
        return;
    }
    if (why != 1 && s_sent_any) {
        why = 1;
        mono_n = 0;
    }
    s_send_n = mono_n;
    s_send_why = why;
    s_send_more = more;
    if (why == 1 && mono_n > 0) {
        s_sent_any = true;
    }
    __atomic_store_n(&s_send_i, s_rec_i, __ATOMIC_RELEASE);
    if (more) {
        s_rec_i ^= 1;
        s_fill = 0;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    /* Do not hold the display lock here — Pager UI is updated from pager_net task. */
    s_mic_ok = open_mic() == 0;
    s_spk_ok = s_mic_ok && open_speaker() == 0;
    ESP_LOGI(TAG, "mic %s speaker %s, internal largest %u",
             s_mic_ok ? "ok" : "failed", s_spk_ok ? "ok" : "failed",
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bool was = false;
    bool was_hold = false;
    while (1) {
        if (!s_codec_wanted) {
            if (s_mic_ok || s_spk_ok) {
                close_codecs();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!s_mic_ok) {
            s_mic_ok = open_mic() == 0;
            s_spk_ok = s_mic_ok && open_speaker() == 0;
            if (!s_mic_ok) {
                /* Each retry creates fresh codec handles. Back off and tear
                 * them down, or the retry loop eats all of internal RAM. */
                close_codecs();
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        bool down = s_ptt;
        bool meet = __atomic_load_n(&s_meeting, __ATOMIC_ACQUIRE) != 0;
        bool hold = down || meet;
        int play_st = __atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE);
        if (hold) {
            __atomic_store_n(&s_play_state, 0, __ATOMIC_RELEASE);
            play_st = 0;
        } else if (play_st == 2) {
            int expect = 2;
            __atomic_compare_exchange_n(&s_play_state, &expect, 0, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            play_st = __atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE);
        }
        if (play_st == 1 && s_spk_ok && !s_recording) {
            int gen = __atomic_load_n(&s_gen, __ATOMIC_ACQUIRE);
            int fill = __atomic_load_n(&s_pcm_fill, __ATOMIC_ACQUIRE);
            int frames = fill / 2;
            if (frames > BOUNCE / 4) {
                frames = BOUNCE / 4;
            }
            if (frames <= 0) {
                if (__atomic_load_n(&s_play_done, __ATOMIC_ACQUIRE) || s_gap_ms > 15000) {
                    if (s_gap_ms > 15000) {
                        ESP_LOGW(TAG, "speak stalled at %d", s_played);
                    }
                    int expect = 1;
                    if (__atomic_compare_exchange_n(&s_play_state, &expect, 0, false,
                                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                        ESP_LOGI(TAG, "speak played %d bytes", s_played);
                    }
                } else {
                    s_gap_ms += 4;
                    vTaskDelay(pdMS_TO_TICKS(4));
                }
            } else {
                s_gap_ms = 0;
                if (s_need_unmute) {
                    esp_codec_dev_set_out_mute(s_spk, false);
                    esp_codec_dev_set_out_vol(s_spk, s_vol);
                    s_need_unmute = 0;
                }
                int r = s_r;
                int16_t *dst = (int16_t *)s_bounce;
                for (int i = 0; i < frames; i++) {
                    int16_t sample;
                    memcpy(&sample, s_ring + r, 2);
                    r += 2;
                    if (r >= RING_CAP) {
                        r = 0;
                    }
                    dst[i * 2] = sample;
                    dst[i * 2 + 1] = sample;
                }
                if (esp_codec_dev_write(s_spk, s_bounce, frames * 4) == ESP_CODEC_DEV_OK) {
                    if (__atomic_load_n(&s_gen, __ATOMIC_ACQUIRE) == gen &&
                        __atomic_load_n(&s_play_state, __ATOMIC_ACQUIRE) == 1) {
                        s_r = r;
                        __atomic_fetch_sub(&s_pcm_fill, frames * 2, __ATOMIC_RELEASE);
                        s_played += frames * 2;
                    }
                } else if (__atomic_load_n(&s_gen, __ATOMIC_ACQUIRE) == gen) {
                    ESP_LOGW(TAG, "speaker write failed at %d", s_played);
                    int expect = 1;
                    __atomic_compare_exchange_n(&s_play_state, &expect, 0, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                }
            }
            was = down;
            was_hold = hold;
            continue;
        }
        if (!s_mic_ok) {
            if (was && !down && !s_recording && send_busy() < 0) {
                s_send_n = 0;
                s_send_why = 4;
                s_send_more = false;
                __atomic_store_n(&s_send_i, 0, __ATOMIC_RELEASE);
            }
            was = down;
            was_hold = hold;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (!s_recording && hold && !was_hold && send_busy() < 0) {
            s_rec_i = 0;
            s_fill = 0;
            s_sent_any = false;
            s_recording = true;
            ESP_LOGI(TAG, "record start");
            for (int i = 0; i < 4; i++) {
                if (read_bounce(s_bounce, BOUNCE) != 0) {
                    break;
                }
            }
        }
        if (s_recording) {
            bool full = s_fill >= s_cap;
            if (full || !hold) {
                if (send_busy() >= 0) {
                    if (hold) {
                        esp_codec_dev_read(s_mic, s_bounce, BOUNCE);
                    }
                } else {
                    seal(hold && full);
                    if (!hold || !full) {
                        s_recording = false;
                    }
                }
            } else {
                int room = s_cap - s_fill;
                int n = room > BOUNCE ? BOUNCE : room;
                n &= ~3;
                if (n > 0 && read_bounce(s_buf[s_rec_i] + s_fill, n) == 0) {
                    s_fill += n;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        was = down;
        was_hold = hold;
        vTaskDelay(pdMS_TO_TICKS(s_recording ? 1 : 20));
    }
}

void pager_audio_start(void)
{
    if (s_audio_task_started) {
        s_codec_wanted = true;
        return;
    }
    s_codec_wanted = true;
    xTaskCreatePinnedToCore(audio_task, "pager_mic", 8192, NULL, 2, NULL, 1);
    s_audio_task_started = true;
}

int pager_audio_poll(const uint8_t **pcm, int *n, int *rate, int *more)
{
    int slot = send_busy();
    if (slot < 0) {
        return 0;
    }
    if (pcm) {
        *pcm = s_buf[slot];
    }
    if (n) {
        *n = s_send_why == 1 ? s_send_n : 0;
    }
    if (rate) {
        *rate = RATE;
    }
    if (more) {
        *more = s_send_more ? 1 : 0;
    }
    return s_send_why;
}

void pager_audio_done(void)
{
    __atomic_store_n(&s_send_i, -1, __ATOMIC_RELEASE);
}

void pager_audio_chime(int repeat)
{
    if (repeat < 1) {
        repeat = 1;
    }
    if (repeat > 3) {
        repeat = 3;
    }
    if (!s_ring) {
        return;
    }
    const int rate = RATE;
    const int ms = 160;
    const int samples = rate * ms / 1000;
    const int bytes = samples * 2;
    static int16_t *tone;
    if (!tone) {
        tone = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tone) {
            return;
        }
    }
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < samples; i++) {
            float t = (float)i / (float)rate;
            float env = 1.0f;
            if (i < rate / 50) {
                env = (float)i / (float)(rate / 50);
            } else if (i > samples - rate / 40) {
                env = (float)(samples - i) / (float)(rate / 40);
            }
            tone[i] = (int16_t)(18000.0f * env * sinf(2.0f * 3.1415926f * 880.0f * t));
        }
        pager_audio_play((const uint8_t *)tone, bytes);
        vTaskDelay(pdMS_TO_TICKS(ms + 80));
    }
}
