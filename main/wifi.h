#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MAXIMUM_RETRY 6
#define STOP_AP_DELAY 20000         // how long to keep softAP running after obtaining an IP (in ms)

SemaphoreHandle_t* get_wifi_mutex();

void my_wifi_init();
void start_ap_prov();
esp_err_t my_wifi_deinit();

#ifdef __cplusplus
}
#endif