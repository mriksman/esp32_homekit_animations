#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t
#include "driver/gpio.h"
#include <sys/param.h>                          // MIN MAX
#include <string.h>                             // strcmp

#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_mac.h"
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

ESP_EVENT_DEFINE_BASE(HOMEKIT_EVENT);           // Convert esp-homekit events into esp event system      

#include "animation.h"

#include "esp_log.h"
static const char *TAG = "main";

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
    // static variables retain value between calls (like global)
    static bool last_on_state = false;
    static int last_brightness = 100;

    ESP_LOGI(TAG, "%s", _ch->description);

    homekit_service_t *light_service     = homekit_service_by_type(_ch->service->accessory, HOMEKIT_SERVICE_LIGHTBULB);
    homekit_service_t *tv_service        = homekit_service_by_type(_ch->service->accessory, HOMEKIT_SERVICE_TELEVISION);

    homekit_characteristic_t *active     = homekit_service_characteristic_by_type(tv_service, HOMEKIT_CHARACTERISTIC_ACTIVE);
    homekit_characteristic_t *active_id  = homekit_service_characteristic_by_type(tv_service, HOMEKIT_CHARACTERISTIC_ACTIVE_IDENTIFIER);

    homekit_characteristic_t *brightness = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS);
    homekit_characteristic_t *hue        = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_HUE);
    homekit_characteristic_t *sat        = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_SATURATION);
    homekit_characteristic_t *on         = homekit_service_characteristic_by_type(light_service, HOMEKIT_CHARACTERISTIC_ON);
    homekit_characteristic_t *custom_id  = homekit_service_characteristic_by_type(light_service, "02B77067-DA5D-493C-829D-F6C5DCFE5C28");

    // when using Siri, 
    //    turning ON or OFF only triggers the ON characteristic
    //    when changing BRIGHTNESS, ON follows BRIGHTNESS

    // when using the Home app, 
    //    you must use the slider to turn on and off. Both ON followed by BRIGHTNESS characteristics trigger
    //    when changing BRIGHTNESS, ON precedes BRIGHTNESS

    
    // when color changes, hue and saturation events are sent. 
    //   we only need the latter one (which is hue)
    if (strcmp(_ch->type, HOMEKIT_CHARACTERISTIC_SATURATION) == 0) {
        ESP_LOGW(TAG, "SATURATION characteristic. no action.");
        return;
    }
    // BRIGHTNESS always includes ON (before or after). ignore, unless there is a change of state, allowing the BRIGHTNESS trigger to do the work
    else if (strcmp(_ch->type, HOMEKIT_CHARACTERISTIC_ON) == 0 && last_on_state == on->value.bool_value) {
        ESP_LOGW(TAG, "ON bool has not changed. no action.");
        return;
    } 
    else if (strcmp(_ch->type, HOMEKIT_CHARACTERISTIC_ON) == 0) {
        ESP_LOGW(TAG, "ON bool has changed. update and continue.");
        last_on_state = on->value.bool_value;
    }

    led_strip_t led_strip;

    // turn off
    if (!on->value.bool_value) {
        led_strip.hue               = 0.0f;
        led_strip.saturation        = 0.0f;
        led_strip.brightness        = 0;
        led_strip.animate           = false;
        led_strip.custom_id         = custom_id->value.int_value; 

        // turning off in Home app sends ON and BRIGHTNESS
        // turning off remotely sends only ON
        if (strcmp(_ch->type, HOMEKIT_CHARACTERISTIC_ON) == 0) {
            ESP_LOGW(TAG, "set_strip off and save last brightness %d if[#1]", brightness->value.int_value);
            last_brightness = brightness->value.int_value;                 // saved for the above case when turning off light from Home app
            set_strip(led_strip);
        }  
        else {
            // when you use the Home app to turn off the light, the BRIGHTNESS slider is pulled to 0%. When you
            //   subsequently use Siri to turn it back on, the BRIGHTNESS stays at 0%. We need to restore the 
            //   BRIGHTNESS value it was before being pulled to 0%.
            if (brightness->value.int_value != last_brightness) {
                ESP_LOGW(TAG, "restore last_brightness %d to characteristic if[#1]", last_brightness);
                brightness->value.int_value = last_brightness;
                homekit_characteristic_notify(brightness, brightness->value);       // notify/update the Home app
            }
            else {
                ESP_LOGW(TAG, "ignore. this is caused by the homekit_characteristic_notify above");
            }
            return;
        }
    } 
    // turn on animate
    else if (active->value.bool_value) {
        led_strip.hue               = hue->value.float_value/360.0f;
        led_strip.saturation        = sat->value.float_value/100.0f;
        led_strip.brightness        = brightness->value.int_value;
        led_strip.animate           = true;
        led_strip.animation_id      = active_id->value.int_value;

        // if a remote button is the cause, then turn off animations
        if (custom_id->value.int_value != 0) {
            active->value = HOMEKIT_UINT8(false);
            // this event will fire again; this time turning on the lights normally
            homekit_characteristic_notify(active, active->value);
            return;
        }

        if (strcmp(_ch->type, HOMEKIT_CHARACTERISTIC_BRIGHTNESS) == 0) {
            ESP_LOGW(TAG, "set_brightness animate active elseif[#2]");
            set_brightness(brightness->value.int_value); 
        } 
        else {
            ESP_LOGW(TAG, "set_strip animate active elseif[#2]");
            set_strip(led_strip);
        }
             
    } 
    // turn light on, modify brightness/hue/saturation or turn off animate
    else {
        led_strip.hue               = hue->value.float_value/360.0f;
        led_strip.saturation        = sat->value.float_value/100.0f;
        led_strip.brightness        = brightness->value.int_value;
        led_strip.animate           = false;
        led_strip.animation_id      = 0;
        led_strip.custom_id         = custom_id->value.int_value; 

        // sent on both ON, BRIGHTNESS and HUE
        ESP_LOGW(TAG, "set_strip on elseif[#3]");
        set_strip(led_strip);
    }

    // reset remote custom id back to 0 (local).
    custom_id->value = HOMEKIT_INT(0);
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

                char buffer[600];
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

    esp_app_desc_t app_desc;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_get_partition_description(running, &app_desc);

    *(s++) = NEW_HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
        NEW_HOMEKIT_CHARACTERISTIC(NAME, name_value),
        NEW_HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Riksman"),
        NEW_HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, name_value),
        NEW_HOMEKIT_CHARACTERISTIC(MODEL, "TinyPICO Nano"),
        NEW_HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, app_desc.version),
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
                .description = "Remote Switch ID",
                .format = homekit_format_int,
                .min_value = (float[]) {0},
                .max_value = (float[]) {6},
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
    esp_log_level_set("httpd_sess", ESP_LOG_INFO);
    esp_log_level_set("httpd_parse", ESP_LOG_INFO);  
    esp_log_level_set("vfs", ESP_LOG_INFO);     
    esp_log_level_set("esp_timer", ESP_LOG_INFO);  
    esp_log_level_set("esp_netif_lwip", ESP_LOG_INFO);     
 
    // Initialize NVS. 
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {     // can happen if truncated/partition size changed
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(BUTTON_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));

    esp_app_desc_t app_desc;
    const esp_partition_t *ota0_partition;
    ota0_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    esp_ota_get_partition_description(ota0_partition, &app_desc);
    ESP_LOGI(TAG, "ota0 version: %s", app_desc.version);

    const esp_partition_t *ota1_partition;
    ota1_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    err = esp_ota_get_partition_description(ota1_partition, &app_desc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ota1 version: %s", app_desc.version);
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "ota1 not found. no ota is likely to have occurred before");
    } else {
        ESP_LOGE(TAG, "ota1 error");
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%" PRIx32 ")",
             running->type, running->subtype, running->address);

    led_status = led_status_init(2, true);

    my_wifi_init();

    // 1. button configuration
    button_config_t button_config = {
        .active_level = BUTTON_ACTIVE_LOW,
        .repeat_press_timeout = 300,
        .long_press_time = 10000,
    };
    button_create(0, button_config, button_callback, NULL);


    init_accessory();
    homekit_server_init(&config);
    // homekit_server_init starts a task which then initialises the storage, so sometimes this call causes an assert partition != null
    // therefore, delay it
    vTaskDelay(pdMS_TO_TICKS(50));
    paired = homekit_is_paired();

    start_animation_task();

    vTaskDelay(pdMS_TO_TICKS(50));

    // I have had my program work on a DevKit module, but fail on a TinyPico module.
    //   App rollback ensures everything starts OK and sets the image valid. Otherwise, on the next reboot, it will rollback.
    esp_ota_mark_app_valid_cancel_rollback();

}
