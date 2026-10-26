/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Pager"
#include "esp_lib_utils.h"

#include "esp_brookesia_app_pager.hpp"

extern "C" {
#include "pager_net.h"
#include "pager_ui.h"
#include "pager_audio.h"
}

#define APP_NAME "Pager"

using namespace esp_brookesia::systems;
using PhoneApp = phone::App;
using BaseApp = base::App;

LV_IMG_DECLARE(pager_icon_112);

static BaseApp::Config pager_core_config(void)
{
    return BaseApp::Config::SIMPLE_CONSTRUCTOR(APP_NAME, &pager_icon_112, true);
}

static PhoneApp::Config pager_app_config(bool use_status_bar, bool use_navigation_bar)
{
    PhoneApp::Config cfg = PhoneApp::Config::SIMPLE_CONSTRUCTOR(
                               &pager_icon_112, use_status_bar, use_navigation_bar);
    /* Keep the bottom-edge gesture (swipe up returns to the launcher). The
     * indicator bar itself is forced off in the phone manager, it covered
     * the app's own pages. */
    cfg.flags.enable_navigation_gesture = 1;
    return cfg;
}

namespace esp_brookesia::apps {

PagerApp *PagerApp::_instance = nullptr;
bool PagerApp::_net_running = false;

PagerApp *PagerApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new PagerApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

PagerApp::PagerApp(bool use_status_bar, bool use_navigation_bar)
    : PhoneApp(pager_core_config(),
               pager_app_config(use_status_bar,
                                use_navigation_bar))
{
}

PagerApp::~PagerApp()
{
}

void PagerApp::teardownSession(void)
{
    pager_ui_ptt(false);
    pager_audio_set_meeting(false);
    pager_audio_suspend();
    if (_net_running) {
        pager_net_stop();
        _net_running = false;
    }
}

bool PagerApp::run(void)
{
    ESP_UTILS_LOGI("Run");

    if (!_net_running) {
        pager_net_prepare();
    }
    ui_init();
    if (!_net_running) {
        pager_net_start();
        _net_running = true;
    } else {
        pager_net_resume();
        pager_audio_resume();
    }

    return true;
}

bool PagerApp::pause(void)
{
    ESP_UTILS_LOGD("Pause");
    teardownSession();
    return true;
}

bool PagerApp::close(void)
{
    ESP_UTILS_LOGD("Close");
    teardownSession();
    return true;
}

bool PagerApp::back(void)
{
    ESP_UTILS_LOGD("Back");
    teardownSession();
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PagerApp, APP_NAME, []()
{
    /* The app draws its own status row, so the phone status bar stays
     * hidden and the whole screen belongs to the app. */
    return std::shared_ptr<PagerApp>(PagerApp::requestInstance(false, false), [](PagerApp *p) {});
})

} // namespace esp_brookesia::apps
