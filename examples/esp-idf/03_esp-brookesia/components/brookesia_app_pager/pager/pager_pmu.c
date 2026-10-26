/*
 * AXP2101 PMU bring-up for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 * Register map and the output configuration follow Waveshare's official
 * esp-idf example (examples/esp-idf/01_AXP2101).
 */
#include "pager_pmu.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"

#include <stdint.h>

#define AXP2101_I2C_ADDRESS 0x34
#define AXP2101_CHIP_ID 0x4A

#define AXP2101_REG_STATUS1 0x00
#define AXP2101_REG_IC_TYPE 0x03
#define AXP2101_REG_ADC_CHANNEL_CTRL 0x30
#define AXP2101_REG_INTSTS1 0x48
#define AXP2101_REG_INTSTS2 0x49
#define AXP2101_REG_INTSTS3 0x4A
#define AXP2101_REG_IPRECHG_SET 0x61
#define AXP2101_REG_ICC_CHG_SET 0x62
#define AXP2101_REG_ITERM_CHG_SET_CTRL 0x63
#define AXP2101_REG_CV_CHG_VOL_SET 0x64
#define AXP2101_REG_DC_ONOFF_DVM_CTRL 0x80
#define AXP2101_REG_DC_VOL0_CTRL 0x82
#define AXP2101_REG_LDO_ONOFF_CTRL0 0x90
#define AXP2101_REG_LDO_ONOFF_CTRL1 0x91
#define AXP2101_REG_LDO_VOL0_CTRL 0x92
#define AXP2101_REG_BAT_PERCENT_DATA 0xA4

#define AXP2101_DCDC1_MIN_MV 1500
#define AXP2101_DCDC1_STEP_MV 100
#define AXP2101_LDO_MIN_MV 500
#define AXP2101_LDO_STEP_MV 100

/* Charge profile: 400 mA CC, 50 mA precharge, 25 mA stop, 4.2 V. */
#define AXP2101_PRECHARGE_50MA 2
#define AXP2101_CHARGE_CURRENT_400MA 10
#define AXP2101_TERMINATION_CURRENT_25MA 1
#define AXP2101_CHARGE_VOLTAGE_4V2 3

static const char *TAG = "pager_pmu";

static i2c_master_dev_handle_t s_pmu_dev;
static bool s_battery_ok;

static esp_err_t pmu_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pmu_dev, &reg, 1, value, 1, 100);
}

static esp_err_t pmu_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_pmu_dev, buf, sizeof(buf), 100);
}

static esp_err_t pmu_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    esp_err_t ret = pmu_read(reg, &current);
    if (ret != ESP_OK) {
        return ret;
    }
    return pmu_write(reg, (uint8_t)((current & ~mask) | (value & mask)));
}

static esp_err_t pmu_set_bit(uint8_t reg, uint8_t bit, bool on)
{
    return pmu_update(reg, (uint8_t)(1U << bit), on ? (uint8_t)(1U << bit) : 0);
}

static esp_err_t pmu_set_linear_mv(uint8_t reg, uint16_t min_mv, uint16_t step_mv,
                                   uint16_t millivolt)
{
    if (millivolt < min_mv || (millivolt % step_mv) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t current = 0;
    esp_err_t ret = pmu_read(reg, &current);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t code = (uint8_t)((millivolt - min_mv) / step_mv);
    return pmu_write(reg, (uint8_t)((current & 0xE0) | code));
}

/* Non-destructive power plan: make sure the two rails the board needs
 * (DCDC1 3.3 V system, ALDO1 3.3 V peripherals) are at 3.3 V and on.
 * Other rails are left at their power-on state — the factory image boots
 * the panel with them untouched, so switching them off here could kill
 * the touch or audio supply. */
static esp_err_t pmu_configure_outputs(void)
{
    esp_err_t ret = ESP_OK;

    ret |= pmu_set_linear_mv(AXP2101_REG_DC_VOL0_CTRL, AXP2101_DCDC1_MIN_MV,
                             AXP2101_DCDC1_STEP_MV, 3300);
    ret |= pmu_set_bit(AXP2101_REG_DC_ONOFF_DVM_CTRL, 0, true);
    ret |= pmu_set_linear_mv(AXP2101_REG_LDO_VOL0_CTRL, AXP2101_LDO_MIN_MV,
                             AXP2101_LDO_STEP_MV, 3300);
    ret |= pmu_set_bit(AXP2101_REG_LDO_ONOFF_CTRL0, 0, true);

    return ret;
}

esp_err_t pager_pmu_init(void)
{
    if (s_pmu_dev) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no i2c bus for pmu");
        return ESP_FAIL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_pmu_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t chip_id = 0;
    ret = pmu_read(AXP2101_REG_IC_TYPE, &chip_id);
    if (ret != ESP_OK || chip_id != AXP2101_CHIP_ID) {
        ESP_LOGE(TAG, "pmu init failed, chip id 0x%02x", chip_id);
        return ESP_FAIL;
    }

    ret |= pmu_set_bit(AXP2101_REG_ADC_CHANNEL_CTRL, 1, false);
    ret |= pmu_configure_outputs();
    ret |= pmu_write(AXP2101_REG_INTSTS1, 0xFF);
    ret |= pmu_write(AXP2101_REG_INTSTS2, 0xFF);
    ret |= pmu_write(AXP2101_REG_INTSTS3, 0xFF);
    ret |= pmu_update(AXP2101_REG_IPRECHG_SET, 0x03, AXP2101_PRECHARGE_50MA);
    ret |= pmu_update(AXP2101_REG_ICC_CHG_SET, 0x1F, AXP2101_CHARGE_CURRENT_400MA);
    ret |= pmu_update(AXP2101_REG_ITERM_CHG_SET_CTRL, 0x0F, AXP2101_TERMINATION_CURRENT_25MA);
    ret |= pmu_update(AXP2101_REG_CV_CHG_VOL_SET, 0x03, AXP2101_CHARGE_VOLTAGE_4V2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pmu configure failed");
        return ret;
    }

    uint8_t pct = 0;
    s_battery_ok = pmu_read(AXP2101_REG_BAT_PERCENT_DATA, &pct) == ESP_OK;
    ESP_LOGI(TAG, "pmu ready, rails DCDC1+ALDO1 3.3V, battery %s %d%%",
             s_battery_ok ? "yes" : "no", s_battery_ok ? pct : 0);
    return ESP_OK;
}

int pager_pmu_battery_percent(void)
{
    if (!s_pmu_dev) {
        return -1;
    }
    uint8_t pct = 0;
    if (pmu_read(AXP2101_REG_BAT_PERCENT_DATA, &pct) != ESP_OK) {
        return -1;
    }
    return pct;
}
