#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t
#include "driver/gpio.h"
#include <sys/param.h>                          // MIN MAX

#include "esp_event.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "wifi.h"
#ifdef CONFIG_IDF_TARGET_ESP32
    #include "esp_netif.h"                      // Must be included before esp_wifi_default.h
    #include "esp_wifi_default.h"               // For esp_netif_create_default_wifi_sta
#endif

#include "button.h"
ESP_EVENT_DEFINE_BASE(BUTTON_EVENT);            // Convert button events into esp event system      

#include "led_status.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#ifdef CONFIG_IDF_TARGET_ESP8266
    #include "mdns.h"                           // ESP8266 RTOS SDK mDNS needs legacy STATUS_EVENT to be sent to it
#endif
ESP_EVENT_DEFINE_BASE(HOMEKIT_EVENT);           // Convert esp-homekit events into esp event system      

#include "lights.h"                             // common struct used for NVS read/write of lights config

#include "animation.h"

#include "esp_log.h"
static const char *TAG = "main";

// Have set lwip sockets from 10 to 16 (maximum allowed)
//   5 for httpd (down from default of 7)
//   12 for HomeKit (up from 8)


static bool paired = false;

// setting a value less than 10 causes 
//     timers.c:795 (prvProcessReceivedCommands)- assert failed!
// on ESP-IDF
static led_status_pattern_t ap_mode = LED_STATUS_PATTERN({1000, -1000});
static led_status_pattern_t not_paired = LED_STATUS_PATTERN({100, -100});
static led_status_pattern_t normal_mode = LED_STATUS_PATTERN({10, -9990});
static led_status_pattern_t identify = LED_STATUS_PATTERN({100, -100, 100, -350, 100, -100, 100, -350, 100, -100, 100, -350});
static led_status_t led_status;


static homekit_accessory_t *accessories[2];


void status_led_identify(homekit_value_t _value) {
    led_status_signal(led_status, &identify);
}

void state_change_on_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    homekit_service_t *tv_service = homekit_service_by_type(_ch->service->accessory, HOMEKIT_SERVICE_TELEVISION);
    homekit_service_t *light_service = homekit_service_by_type(_ch->service->accessory, HOMEKIT_SERVICE_LIGHTBULB);

    homekit_characteristic_t *hue        = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_HUE);
    homekit_characteristic_t *sat        = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_SATURATION);
    homekit_characteristic_t *brightness = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS);
    homekit_characteristic_t *on         = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_ON);
    homekit_characteristic_t *direction  = homekit_service_characteristic_by_type(light_service, "02B77067-DA5D-493C-829D-F6C5DCFE5C28");

    homekit_characteristic_t *active     = homekit_service_characteristic_by_type(tv_service, HOMEKIT_CHARACTERISTIC_ACTIVE);
    homekit_characteristic_t *active_id  = homekit_service_characteristic_by_type(tv_service, HOMEKIT_CHARACTERISTIC_ACTIVE_IDENTIFIER);

    led_strip_t led_strip;
    if (active->value.bool_value && on->value.bool_value) {
        led_strip.hue          = hue->value.float_value/360.0f;
        led_strip.saturation   = sat->value.float_value/100.0f;
        led_strip.brightness   = brightness->value.int_value/100.0f;
        led_strip.animate      = true;
        led_strip.animation_id = active_id->value.int_value;
    } else if (!on->value.bool_value) {
        led_strip.hue          = 0.0f;
        led_strip.saturation   = 0.0f;
        led_strip.brightness   = 0.0f;
        led_strip.animate      = false;
        led_strip.animation_id = 0;
    } else {
        led_strip.hue          = hue->value.float_value/360.0f;
        led_strip.saturation   = sat->value.float_value/100.0f;
        led_strip.brightness   = brightness->value.int_value/100.0f;
        led_strip.animate      = false;
        led_strip.animation_id = 0;
    }

            ESP_LOGI(TAG, "direction %d", direction->value.int_value);

    set_strip(led_strip);
}


void name_change_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    esp_err_t err;

    nvs_handle config_handle;
    err = nvs_open("homekit", NVS_READWRITE, &config_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open err %s", esp_err_to_name(err));
        return;
    }

    homekit_characteristic_t *name_c = homekit_service_characteristic_by_type(
            _ch->service, HOMEKIT_CHARACTERISTIC_NAME
        );
    // Input Source (Animations) have a NAME Characteristic
    if (name_c != NULL) {
        err = nvs_set_str(config_handle, name_c->value.string_value, value.string_value);
    } else {
        err = nvs_set_str(config_handle, "tv_name", value.string_value);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "error nvs_set_str err %s", esp_err_to_name(err));
    }

    err = nvs_commit(config_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "error nvs_commit err %s", esp_err_to_name(err));
    }
    
    nvs_close(config_handle);
 }


// Need to call this function from a task different to the button_callback (executing in Tmr Svc)
// Have had occurrences when, if called from button_callback directly, the scheduler seems
// to lock up. 
static void start_ap_task(void * arg)
{
    ESP_LOGI(TAG, "Start AP task");
    start_ap_prov();
    vTaskDelete(NULL);
}


/* Event handler for Events */
static void main_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
            led_status_set(led_status, &ap_mode);
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            led_status_set(led_status, paired ? &normal_mode : &not_paired);
        } 
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            wifi_mode_t wifi_mode;
            esp_wifi_get_mode(&wifi_mode);
            if (wifi_mode == WIFI_MODE_STA) {
                led_status_set(led_status, paired ? &normal_mode : &not_paired);
            } else {
                led_status_set(led_status, &ap_mode);
            }
        }
    } else if (event_base == HOMEKIT_EVENT) {
        if (event_id == HOMEKIT_EVENT_CLIENT_CONNECTED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_CONNECTED");
        }
        else if (event_id == HOMEKIT_EVENT_CLIENT_DISCONNECTED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_DISCONNECTED");
        }
        else if (event_id == HOMEKIT_EVENT_PAIRING_ADDED || event_id == HOMEKIT_EVENT_PAIRING_REMOVED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_PAIRING_ADDED or HOMEKIT_EVENT_PAIRING_REMOVED");
            paired = homekit_is_paired();
            led_status_set(led_status, paired ? &normal_mode : &not_paired);
        }
    } else if (event_base == BUTTON_EVENT) {

        if (event_id == BUTTON_EVENT_UP) {

        }
        else if (event_id == BUTTON_EVENT_DOWN) {

        }

        else if (event_id == BUTTON_EVENT_DOWN_HOLD) {

        }
        else if (event_id == BUTTON_EVENT_UP_HOLD) {

        }

        else if (event_id == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "button long press event. start AP");  
            //start_ap_prov();        
            xTaskCreate(&start_ap_task, "Start AP", 4096, NULL, tskIDLE_PRIORITY, NULL);
        }
        else {
            // service[0] is the accessory name/manufacturer. 
            // following that are the lights in service[1], service[2], ...
            //uint8_t service_idx = light_idx + 1; 

            // Get the service and characteristics
            //homekit_accessory_t *accessory = accessories[0];
            //homekit_service_t *service = accessory->services[service_idx];

            if (event_id == 1) {

            } 

            else if (event_id == 4) {
                ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));

                char buffer[400];
                vTaskList(buffer);
                ESP_LOGI(TAG, "\n%s", buffer);
            } 
        }
    }
}

void homekit_on_event(homekit_event_t event) {
    esp_event_post(HOMEKIT_EVENT, event, NULL, sizeof(NULL), 10);
}
void button_callback(button_event_t event, void* context) {
    // esp_event_post sends a pointer to a COPY of the data.
    esp_event_post(BUTTON_EVENT, event, context, sizeof(uint8_t), 10);
}


homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = homekit_on_event,
};

void init_accessory() {
    esp_err_t err;

    nvs_handle config_handle;
    err = nvs_open("homekit", NVS_READWRITE, &config_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open err %s", esp_err_to_name(err));
    }

    uint8_t macaddr[6];
    esp_read_mac(macaddr, ESP_MAC_WIFI_SOFTAP);
    int name_len = snprintf( NULL, 0, "esp-%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5] );
    char *name_value = malloc(name_len + 1);
    snprintf( name_value, name_len + 1, "esp-%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5] ); 

    // ACCESSORY_INFORMATION, TELEVISION, LIGHTBULB the ANIMATIONS, and NULL
    homekit_service_t* services[4 + NUM_ANIMATIONS]; 
    homekit_service_t** s = services;


    *(s++) = NEW_HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
        NEW_HOMEKIT_CHARACTERISTIC(NAME, name_value),
        NEW_HOMEKIT_CHARACTERISTIC(MANUFACTURER, "MikeKit"),
        NEW_HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A3BBBF29D"),
        NEW_HOMEKIT_CHARACTERISTIC(MODEL, "AnimLights"),
        NEW_HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
        NEW_HOMEKIT_CHARACTERISTIC(IDENTIFY, status_led_identify),
        NULL
    });

    homekit_service_t* tv_anim_services[NUM_ANIMATIONS+1];
    homekit_service_t** s_tv = tv_anim_services;

    for (int i=0; i < NUM_ANIMATIONS; i++) {
        int anim_name_len = snprintf(NULL, 0, "anim%d", i + 1);
        char *anim_name_val = malloc(anim_name_len + 1);
        snprintf(anim_name_val, anim_name_len + 1, "anim%d", i + 1);

        // Use NVS to retrieve names
        char *conf_name_val;
        size_t required_size;
        err = nvs_get_str(config_handle, anim_name_val, NULL, &required_size); //includes zero-terminator
        if (err == ESP_OK) {
            conf_name_val = malloc(required_size); 
            nvs_get_str(config_handle, anim_name_val, conf_name_val, &required_size);
            
            ESP_LOGI("nvs", "key: %s value: %s", anim_name_val, conf_name_val);

        } else {

            ESP_LOGW(TAG, "error retrieving %s nvs_get_str err %s", anim_name_val, esp_err_to_name(err));

            int conf_name_len = snprintf(NULL, 0, "Animation %d", i + 1);
            conf_name_val = malloc(conf_name_len + 1);
            snprintf(conf_name_val, conf_name_len + 1, "Animation %d", i + 1);
        }

        *(s_tv++) = NEW_HOMEKIT_SERVICE(INPUT_SOURCE, .characteristics=(homekit_characteristic_t*[]){
            NEW_HOMEKIT_CHARACTERISTIC(NAME, anim_name_val),
            NEW_HOMEKIT_CHARACTERISTIC(IDENTIFIER, i + 1),
            NEW_HOMEKIT_CHARACTERISTIC(CONFIGURED_NAME, conf_name_val,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(name_change_callback)
            ),
            NEW_HOMEKIT_CHARACTERISTIC(INPUT_SOURCE_TYPE, HOMEKIT_INPUT_SOURCE_TYPE_HDMI),
            NEW_HOMEKIT_CHARACTERISTIC(IS_CONFIGURED, true),
            NEW_HOMEKIT_CHARACTERISTIC(CURRENT_VISIBILITY_STATE, HOMEKIT_CURRENT_VISIBILITY_STATE_SHOWN),
            NULL
        });
    }
    *(s_tv++) = NULL;

    // Use NVS to retrieve names
    char *conf_name_val;
    size_t required_size;
    err = nvs_get_str(config_handle, "tv_name", NULL, &required_size); //includes zero-terminator
    if (err == ESP_OK) {
        conf_name_val = malloc(required_size); 
        nvs_get_str(config_handle, "tv_name", conf_name_val, &required_size);

        ESP_LOGI("nvs", "key: tv_name value: %s", conf_name_val);

    } else {

        ESP_LOGW(TAG, "error retrieving tv_name nvs_get_str err %s", esp_err_to_name(err));

        int conf_name_len = snprintf(NULL, 0, "Animations");
        conf_name_val = malloc(conf_name_len + 1);
        snprintf(conf_name_val, conf_name_len + 1, "Animations");
    }
    
    *(s++) = NEW_HOMEKIT_SERVICE(TELEVISION, .characteristics=(homekit_characteristic_t*[]) {
        NEW_HOMEKIT_CHARACTERISTIC(
            ACTIVE, false,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(state_change_on_callback)
        ),
        NEW_HOMEKIT_CHARACTERISTIC(
            ACTIVE_IDENTIFIER, 1,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(state_change_on_callback)
        ),
        NEW_HOMEKIT_CHARACTERISTIC(
            CONFIGURED_NAME, conf_name_val,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(name_change_callback)
        ),
        NEW_HOMEKIT_CHARACTERISTIC(
            SLEEP_DISCOVERY_MODE, HOMEKIT_SLEEP_DISCOVERY_MODE_ALWAYS_DISCOVERABLE
        ),
        NULL
        }, .linked = tv_anim_services
    );

    *(s++) = NEW_HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            NEW_HOMEKIT_CHARACTERISTIC(NAME, "LightbulbName"),
            NEW_HOMEKIT_CHARACTERISTIC(
                ON, false,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(state_change_on_callback)
            ),
            NEW_HOMEKIT_CHARACTERISTIC(
                BRIGHTNESS, 100,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(state_change_on_callback)
            ),
            NEW_HOMEKIT_CHARACTERISTIC(
                HUE, 0,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(state_change_on_callback)
            ),
            NEW_HOMEKIT_CHARACTERISTIC(
                SATURATION, 0,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(state_change_on_callback)
            ),
            NEW_HOMEKIT_CHARACTERISTIC(
                CUSTOM, 0,
                .type = "02B77067-DA5D-493C-829D-F6C5DCFE5C28",
                .description = "Fade Direction",
                .format = homekit_format_uint8,
                .min_value = (float[]) {0},
                .max_value = (float[]) {2},
                .permissions = homekit_permissions_paired_read
                             | homekit_permissions_paired_write,
            ),
            NULL
        });

    for (int i=0; i < NUM_ANIMATIONS; i++) {
        *(s++) = tv_anim_services[i];
    }



    *(s++) = NULL;

    accessories[0] = NEW_HOMEKIT_ACCESSORY(.category=homekit_accessory_category_lightbulb, .services=services);
    accessories[1] = NULL;

    nvs_close(config_handle);

}




void app_main(void)
{
    esp_err_t err;

    esp_log_level_set("*", ESP_LOG_DEBUG);      
    esp_log_level_set("httpd", ESP_LOG_INFO); 
    esp_log_level_set("httpd_uri", ESP_LOG_INFO);    
    esp_log_level_set("httpd_txrx", ESP_LOG_INFO);     
//    esp_log_level_set("httpd_sess", ESP_LOG_INFO);
    esp_log_level_set("httpd_parse", ESP_LOG_INFO);  
    esp_log_level_set("vfs", ESP_LOG_INFO);     
    esp_log_level_set("esp_timer", ESP_LOG_INFO);     
 
    // Initialize NVS. 
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {     // can happen if truncated/partition size changed
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

   // esp_event_handler_register is being deprecated
    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(BUTTON_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
    #elif CONFIG_IDF_TARGET_ESP8266
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(BUTTON_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
    #endif

    led_status = led_status_init(2, true);

    wifi_init();

    // 1. button configuration
    button_config_t button_config = {
        .active_level = BUTTON_ACTIVE_LOW,
        .repeat_press_timeout = 300,
        .long_press_time = 10000,
    };
    button_create(0, button_config, button_callback, NULL);


    init_accessory();
    homekit_server_init(&config);
    paired = homekit_is_paired();

    start_animation_task();

}
