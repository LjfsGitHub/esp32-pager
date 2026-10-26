/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class PagerApp : public systems::phone::App {
public:
    static PagerApp *requestInstance(bool use_status_bar = true, bool use_navigation_bar = false);

    ~PagerApp();

protected:
    PagerApp(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool pause(void) override;
    bool close(void) override;

private:
    static PagerApp *_instance;
    static bool _net_running;
    void teardownSession(void);
};

} // namespace esp_brookesia::apps
