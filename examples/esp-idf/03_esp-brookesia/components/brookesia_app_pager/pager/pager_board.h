#pragma once

#include "driver/gpio.h"

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C (round 1.75" 466x466):
 * the side "PWR" button is on GPIO3 and idles LOW (the pin sits low at
 * rest on this board), so the key is active HIGH here. The old 2.16
 * board used GPIO18 active low.
 */
#define PAGER_PTT_GPIO GPIO_NUM_3
#define PAGER_PTT_ACTIVE_LOW 0
