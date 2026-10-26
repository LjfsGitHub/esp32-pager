/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_PAGER_QA

#include <cstring>

#include "esp_brookesia.hpp"
#include "esp_lib_utils.h"
#include "lvgl.h"
#include "pager_qa.h"

using namespace esp_brookesia;
using namespace esp_brookesia::systems::phone;
namespace base = esp_brookesia::systems::base;

extern "C" void pager_qa_arm_autostart(void *phone_void)
{
    Phone *phone = static_cast<Phone *>(phone_void);
    if (!phone) {
        return;
    }
    pager_qa_init();
    lv_timer_create(
        [](lv_timer_t *t) {
            Phone *ph = static_cast<Phone *>(t->user_data);
            if (!ph) {
                return;
            }
            esp_brookesia::systems::phone::Manager &mgr = ph->getManager();
            int pager_id = -1;
            for (int id = base::App::APP_ID_MIN; id < 32; id++) {
                base::App *app = mgr.getInstalledApp(id);
                if (app && app->getName() && strcmp(app->getName(), "Pager") == 0) {
                    pager_id = id;
                    break;
                }
            }
            if (pager_id < 0) {
                ESP_UTILS_LOGE("QA: Pager app not installed");
                lv_timer_del(t);
                return;
            }
            ESP_UTILS_LOGI("QA: auto-start Pager (id=%d)", pager_id);
            base::Context::AppEventData app_event = {
                .id = pager_id,
                .type = base::Context::AppEventType::START,
            };
            if (!ph->sendAppEvent(&app_event)) {
                ESP_UTILS_LOGE("QA: startApp failed");
            }
            lv_timer_del(t);
        },
        4000, phone);
}

#endif
