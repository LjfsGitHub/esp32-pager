#pragma once

#include <stdbool.h>
#include <stdint.h>

void pager_audio_start(void);
void pager_audio_suspend(void);
void pager_audio_resume(void);
void pager_audio_arm(void);
/* Meeting mode keeps the microphone open until it is turned off. */
void pager_audio_set_meeting(bool on);

/* 0 nothing, 1 clip, 2 quiet, 3 too short, 4 mic failed.
 * On 1, pcm stays valid until pager_audio_done().
 * *more is 1 when the key is still held and another segment will follow. */
int pager_audio_poll(const uint8_t **pcm, int *n, int *rate, int *more);
void pager_audio_done(void);

/* Play 16 kHz mono PCM on the speaker. Replaces any clip already playing. */
void pager_audio_begin(void);
int pager_audio_feed(const uint8_t *pcm, int n);
void pager_audio_finish(void);
bool pager_audio_playing(void);
void pager_audio_play(const uint8_t *pcm, int n);
/* Short beep for pager ring (1–3 repeats). */
void pager_audio_chime(int repeat);
/* 0–100. Takes effect immediately, and on the next time the speaker opens. */
void pager_audio_set_volume(int volume);
