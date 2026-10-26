#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void pager_net_prepare(void);
void pager_net_start(void);
void pager_net_stop(void);
void pager_net_resume(void);

/** Save Wi-Fi credentials and join (same path as the provisioning portal). */
void pager_net_save_wifi(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
