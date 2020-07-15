#ifdef __cplusplus
extern "C" {
#endif

#define NUM_ANIMATIONS 7

typedef struct {
    float hue; 
    float saturation; 
    float brightness; 
    bool animate;
    uint32_t animation_id;
} led_strip_t;

// HomeKit         hue 360.0   saturation 100.0   brightness 100.0
// NeoPixelBus     hue   1.0   saturation   1.0   brightness   1.0

void start_animation_task();
void set_strip(led_strip_t led_strip);

#ifdef __cplusplus
}
#endif 