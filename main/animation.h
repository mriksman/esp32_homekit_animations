#ifdef __cplusplus
extern "C" {
#endif

#define NUM_ANIMATIONS          8
#define NUM_COLOR_CYCLE         4


typedef struct {
    float hue; 
    float saturation; 
    int brightness; 
    bool only_brightness;       // set true when only brightness was modified
    bool animate;
    uint32_t animation_id;
    uint8_t custom_id;
} led_strip_t;

// HomeKit         hue 360.0f   saturation 100.0f   brightness   100(int)
// NeoPixelBus     hue   1.0f    saturation   1.0f  brightness   1.0f

esp_err_t start_animation_task();
void set_strip(led_strip_t led_strip);
void set_brightness(int brightness);

#ifdef __cplusplus
}
#endif 