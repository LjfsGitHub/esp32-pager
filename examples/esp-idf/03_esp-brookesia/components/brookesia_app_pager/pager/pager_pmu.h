/*
 * AXP2101 PMU for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 * The panel, touch and audio rails sit behind the PMU. The official
 * Waveshare init turns every rail off and then enables exactly two:
 *   DCDC1 = 3.3 V (system) and ALDO1 = 3.3 V (peripherals).
 * This must run before the display is reset, or the panel never wakes.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Program the power rails. Call once, before bsp_display_start(). */
esp_err_t pager_pmu_init(void);

/** Battery percentage 0..100, or -1 when no battery is connected. */
int pager_pmu_battery_percent(void);

#ifdef __cplusplus
}
#endif
