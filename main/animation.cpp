#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/param.h>   
#include <atomic>                       // note: this is a cpp file, so use <atomic>, not <stdatomic.h>

#include "nvs_flash.h"

#include "esp_log.h"
static const char *TAG = "anim";

#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>
#include "NeoStripTopology.h"

#include "animation.h"

class MyRingsLayout 
{
public:
    void Begin() {
        esp_err_t err;

        nvs_handle config_handle;
        err = nvs_open("lights", NVS_READWRITE, &config_handle);
        if (err == ESP_OK) {
            // Get configured number of rings/strips
            uint8_t num_rings = 0;
            err = nvs_get_u8(config_handle, "num_rings", &num_rings);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "error nvs_get_u8 num_rings err %d", err);
            }

            if (num_rings > 0) {
                uint16_t pixel_layout[num_rings];
                size_t size = num_rings * sizeof(uint16_t);
                err = nvs_get_blob(config_handle, "pixel_layout", pixel_layout, &size);
                // the device has been configured properly
                if (err == ESP_OK) {
                    RingCount = num_rings + 1;

                    Rings = new uint16_t[RingCount];
                    Rings[0] = 0;

                    for (uint16_t i = 1; i < RingCount; i++) {
                        Rings[i] = pixel_layout[i-1] + Rings[i-1];
                    }
                }
                else {
                    ESP_LOGW(TAG, "error nvs_get_u8 pixel_layout err %d", err);
                }
            }
            nvs_close(config_handle);
        }
    }

protected:
    uint16_t* Rings; 
    uint8_t RingCount = 0;

    uint8_t _ringCount() const
    {
        return RingCount;
    }
};

NeoDynamicRingTopology<MyRingsLayout> segment;


// Default is NeoEsp32Rmt6Ws2812xMethod (channel 6)
//NeoPixelBus<NeoGrbwFeature, Neo800KbpsMethod> strip(PixelCount, PixelPin);

//NeoPixelBus<NeoGrbwFeature, NeoEsp32I2s1Sk6812Method> strip(PixelCount, PixelPin); // using i2s
//NeoPixelBus<NeoGrbwFeature, NeoEsp32Rmt0Sk6812Method> strip(PixelCount, PixelPin); // using RMT
NeoPixelBus<NeoGrbwFeature, NeoEsp32Rmt0Sk6812Method>* strip = NULL;

//NeoPixelAnimator animations(PixelCount, NEO_CENTISECONDS);
NeoPixelAnimator* animations = NULL;


static QueueHandle_t s_led_message_queue;

// let the compiler know that this variable can be updated from another thread at any time
// this is faster than using a mutex
std::atomic<int> atomic_brightness (100);



// *********** This is the standard animation for on/off ******************
void FadeAnimationSet(HsbColor targetColor, int8_t direction)
{
    // convert Hsb to Rgbw
    RgbwColor rgbwTargetColor = targetColor;
    // create white channel
    rgbwTargetColor.W = MIN( rgbwTargetColor.R, MIN(rgbwTargetColor.G, rgbwTargetColor.B));
    rgbwTargetColor.R = rgbwTargetColor.R - rgbwTargetColor.W; 
    rgbwTargetColor.G = rgbwTargetColor.G - rgbwTargetColor.W; 
    rgbwTargetColor.B = rgbwTargetColor.B - rgbwTargetColor.W; 
    rgbwTargetColor.W *= 0.8;

    // can gamma correct using Rgbw compatible NeoGamma
    NeoGamma<NeoGammaTableMethod> colorGamma;
    rgbwTargetColor = colorGamma.Correct(rgbwTargetColor);

    // use pixel color of pixel(0) as the start color to transition from
    RgbwColor originalColor = strip->GetPixelColor(0);

    // this runs AFTER the fade animation below. Normally, when an animation finishes, the last pixel colour stays forever. 
    //   Due to wiring issue, I sometimes get odd pixel colours appear, as the data wire I use is effectively acting as an antenna.
    //   So, I need to keep refreshing all the pixel colours.
    AnimUpdateCallback refreshColour = [=](const AnimationParam& param)
    {
        // only update the pixels once (at the start of the animation)
        if (param.state == AnimationState_Started) {
            uint8_t NumSteps = segment.getCountOfRings();
            for (uint8_t j = 0; j < NumSteps; j++) {
                uint16_t StepWidth = segment.getPixelCountAtRing(j);
                for (uint16_t i = 0; i < StepWidth; i++) {
                    strip->SetPixelColor(segment.Map(j, i), rgbwTargetColor);
                }
            }
        }

        if (param.state == AnimationState_Completed) {
            animations->RestartAnimation(param.index);
        }
    };

    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        float step_progress;

        uint8_t NumSteps = segment.getCountOfRings();

        // use 'direction' to start fade from top or bottom
        for (uint8_t j = 0; j < NumSteps; j++) {
            if (direction > 0) {
                step_progress = fmin(1.0f,fmax(0.0f,( 2.0f*param.progress - 1.0f*j/(NumSteps-1.0f))));
            } else if (direction < 0) {
                step_progress = fmin(1.0f,fmax(0.0f,( 2.0f*param.progress - (1.0f*NumSteps-1.0f-j)/(NumSteps-1.0f))));
            } else {
                step_progress = param.progress;
            }

            RgbwColor updatedColor = RgbwColor::LinearBlend(originalColor, rgbwTargetColor, 
                step_progress  
            );

            uint16_t StepWidth = segment.getPixelCountAtRing(j);
            for (uint16_t i = 0; i < StepWidth; i++) {
                strip->SetPixelColor(segment.Map(j, i), updatedColor);
            }
        }

        // once fade is complete, don't restart
        if (param.state == AnimationState_Completed) {
            animations->StopAnimation(param.index);

            // see above. I need to keep refreshing the pixel colours periodically
            animations->StartAnimation(0, 500, refreshColour);

        }
    };

    animations->StartAnimation(0, direction != 0 ? 300 : 100, animUpdate);

}
// ************************************************************************


// Stores NUM_COLOR_CYCLE colors and cycle up the segment
void ColorCycleAnimationSet(float hue, float saturation)
{
    // static variables maintain values between function calls
    static HsbColor selectedColors[NUM_COLOR_CYCLE];
    static uint8_t next_index = 0;

    if (next_index == NUM_COLOR_CYCLE) {
        next_index = 0;
    }
 
    // select target brightness. gamma corrected 
    float brightness = atomic_brightness/100.0f;
    brightness = pow(brightness,2.2);

    selectedColors[next_index] = HsbColor(hue, saturation, brightness);
    next_index++;

    // spend more time at start/end (to see the color), rather than during the linear blend
    AnimEaseFunction easing =  NeoEase::ExponentialInOut;

    uint8_t NumSteps = segment.getCountOfRings();
    for (uint8_t j = 0; j < NumSteps; j++) {

        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
            uint8_t this_color = 0;
            uint8_t i;
            // divide total progress up by the number of colors to display
            for (i = 0 ; i < NUM_COLOR_CYCLE ; i++) {
                if (param.progress <= (float)(i+1)/(float)NUM_COLOR_CYCLE) {
                    this_color = i;
                    break;
                }    
            }
            // then offset for each step
            this_color += j % NUM_COLOR_CYCLE;
            if (this_color >= NUM_COLOR_CYCLE) {
                this_color -= NUM_COLOR_CYCLE;
            }
            // find out the next color to blend to
            uint8_t next_color = this_color + 1;
            if (next_color >= NUM_COLOR_CYCLE) {
                next_color -= NUM_COLOR_CYCLE;
            }

            // stretch the overall progress to 0.0 -> 1.0 for use in linear blend
            float progress = easing(param.progress * NUM_COLOR_CYCLE - i);

            // update brightness from global
            float brightness = atomic_brightness/100.0f;
            brightness = pow(brightness,2.2);
            for (uint8_t i = 0; i < NUM_COLOR_CYCLE; i++) {
                selectedColors[i].B = brightness;
            }

            // LinearBlend can work with hsb color objects
            RgbwColor color = RgbwColor::LinearBlend(selectedColors[this_color], selectedColors[next_color], progress);

            uint16_t StepWidth = segment.getPixelCountAtRing(j);
            for (uint16_t i = 0; i < StepWidth; i++) {
                strip->SetPixelColor(segment.Map(j, i), color);
            }

            if (param.state == AnimationState_Completed) {
                animations->RestartAnimation(j);
            }
        };

        animations->StartAnimation(j, 200*NUM_COLOR_CYCLE, animUpdate);
    }

}

// Color cycle up each step
void RainbowFadeAnimationSet()
{
    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        uint8_t NumSteps = segment.getCountOfRings();
        for (uint8_t j = 0; j < NumSteps; j++) {
            float hue = param.progress + (1.0*j/NumSteps);
            if (hue > 1) {
                hue -= 1;
            }

            // select target brightness. don't let brightness go higher than global brightness
            float brightness = atomic_brightness/100.0f;
            // gamma corrected
            brightness = pow(brightness,2.2);

            HsbColor color = HsbColor(hue, 1.0f, brightness);

            uint16_t StepWidth = segment.getPixelCountAtRing(j);
            for (uint16_t i = 0; i < StepWidth; i++) {
                strip->SetPixelColor(segment.Map(j, i), color);
            }
        }

        // no need to call parent setup function RainbowFadeAnimationSet(). just restart animation
        if (param.state == AnimationState_Completed) {
            animations->RestartAnimation(param.index);
        }
    };

    animations->StartAnimation(0, 1000, animUpdate);

}

// User selected color. Brightness fades in/out
void FlickerAnimationSet(float hue, float saturation)
{
    // Every pixel is a standalone animation
    for (uint16_t pixel = 0; strip->PixelCount(); pixel++)
    {
        // we need the current brightness of the pixel at the start of the animation
        RgbwColor startColorRgbw = strip->GetPixelColor(pixel);

        // we need hsb color to set start color, so use the retrieved rgbw pixel and convert to hsb
        HsbColor startColor = HsbColor(RgbColor(startColorRgbw.R, startColorRgbw.G, startColorRgbw.B));
        // set the color to chosen hue/saturation, and the current pixel brightness 'originalColor.B'
        startColor = HsbColor(hue, saturation, startColor.B);

        // select target brightness. don't let brightness go higher than global brightness
        float brightness = atomic_brightness/100.0f;
        brightness = fmin(brightness, (1.0*esp_random()/UINT32_MAX));
        // gamma corrected
        brightness = pow(brightness,2.2);

        HsbColor targetColor = HsbColor(startColor.H, startColor.S, brightness);

        AnimEaseFunction easing;

        switch (esp_random()%6)
        {
        case 0:
            easing = NeoEase::CubicIn;
            break;
        case 1:
            easing = NeoEase::CubicOut;
            break;
        case 2:
            easing = NeoEase::QuadraticIn;
            break;
        case 3:
            easing = NeoEase::QuadraticOut;
            break;
        case 4:
            easing = NeoEase::QuinticIn;
            break;
        case 5:
            easing = NeoEase::QuinticOut;
            break;
        }



        easing = NeoEase::Linear;



        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
            float progress = easing(param.progress);

            RgbwColor updatedColor = RgbwColor::LinearBlend(startColor, targetColor, progress);
            strip->SetPixelColor(pixel, updatedColor);

            // once ALL animations have completed, run it all again
            if (!animations->IsAnimating()) {
                FlickerAnimationSet(hue, saturation);
            }
        };

        animations->StartAnimation(pixel, 200, animUpdate);
    }
}

// Randomly selected color. Brightness fades in/out
void GlitterAnimationSet()
{
    // Every pixel is a standalone animation
    for (uint16_t pixel = 0; pixel < strip->PixelCount(); pixel++)
    {
        // each animation starts with the color that was present
        RgbwColor startColorRgbw = strip->GetPixelColor(pixel);

        // select target brightness. don't let brightness go higher than global brightness
        float brightness = atomic_brightness/100.0f;
        brightness = fmin(brightness, (1.0*esp_random()/UINT32_MAX));
        brightness = pow(brightness,2.2);

        // and a random color
        float hue = (float)(1.0*esp_random()/UINT32_MAX);

        HsbColor targetColor = HsbColor(hue, 1.0, brightness);

        // with the random ease function
        AnimEaseFunction easing;

        switch (esp_random()%6)
        {
        case 0:
            easing = NeoEase::CubicIn;
            break;
        case 1:
            easing = NeoEase::CubicOut;
            break;
        case 2:
            easing = NeoEase::QuadraticIn;
            break;
        case 3:
            easing = NeoEase::QuadraticOut;
            break;
        case 4:
            easing = NeoEase::QuinticIn;
            break;
        case 5:
            easing = NeoEase::QuinticOut;
            break;
        }

        // we must supply a function that will define the animation, in this example
        // we are using "lambda expression" to define the function inline, which gives
        // us an easy way to "capture" the originalColor and targetColor for the call back.
        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
            float progress = easing(param.progress);

            // LinearBlend can accept Rgbw and Hsb color objects
            RgbwColor updatedColor = RgbwColor::LinearBlend(startColorRgbw, targetColor, progress);
            strip->SetPixelColor(pixel, updatedColor);

            // once ALL animations have completed, run it all again
            if (!animations->IsAnimating()) {
                GlitterAnimationSet();
            }
        };

        animations->StartAnimation(pixel, 200, animUpdate);

    }
}


void CylonAnimationSet() 
{
    static uint16_t s_last_pixel;
    static int8_t s_direction;
    static float hue;

    s_last_pixel = 0;
    s_direction = 1;

    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        if (param.state == AnimationState_Started) {
            hue = (float)(1.0*esp_random()/UINT32_MAX);
        }

        float brightness = atomic_brightness/100.0f;
        brightness = pow(brightness,2.2);
        brightness = fmax(0.03, brightness);

        HsbColor hsbColor = HsbColor(hue, 1.0, brightness); 

        AnimEaseFunction easing = NeoEase::QuarticInOut;
        float progress = easing(param.progress);

        // darken all pixels
        int darken_by = 50 * hsbColor.B + 1;
        for (uint16_t i = 0; i < strip->PixelCount(); i++) {
            RgbwColor pixel_color = strip->GetPixelColor(i);
            pixel_color.Darken(darken_by);
            strip->SetPixelColor(i, pixel_color);
        }

        // use the curved progress to calculate the pixel to effect.
        uint16_t next_pixel;
        if (s_direction > 0) {
            next_pixel = progress * strip->PixelCount();
        }
        else {
            next_pixel = (1.0f - progress) * strip->PixelCount();
        }
        if (next_pixel == strip->PixelCount()) {
            next_pixel -= 1;
        }

        // how many pixels missed?
        uint8_t pixel_diff = abs(next_pixel - s_last_pixel);

        uint8_t i = 0;
        do {
            uint16_t i_pixel = next_pixel - i * s_direction;
            strip->SetPixelColor(i_pixel, hsbColor);
            i++;
        } while ( i < pixel_diff);

        s_last_pixel = next_pixel;
        
        if (param.state == AnimationState_Completed) {
            s_direction *= -1;

            // time is centiseconds
            uint16_t time = 1000 + esp_random()%1000;
            animations->AnimationDuration(time);
            animations->RestartAnimation(param.index);
        }
    };

    // start animation for the first time
    uint16_t time = 1000 + esp_random()%1000;
    animations->StartAnimation(0, time, animUpdate);
}

// Each step has an animated back and forth 'Cylon' transition
void StepCylonAnimationSet()
{
    uint8_t NumSteps = segment.getCountOfRings();
    for (uint8_t j = 0; j < NumSteps; j++) {
        
        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
             if (param.state == AnimationState_Started) {
                float hue = (float)(1.0*esp_random()/UINT32_MAX);

                float brightness = atomic_brightness/100.0f;
                brightness = pow(brightness, 2.2);
                // do not go lower than this. it will cause CalculateBrightness to return 0
                brightness = fmax(0.03, brightness);

                HsbColor hsbColor = HsbColor(hue, 1.0, brightness);                
                strip->SetPixelColor(segment.Map(j, 0), hsbColor);
           }

            float progress;
            int8_t direction;
            // half is one way, the other half is the other way
            if (param.progress > 0.50) {
                direction = -1;
                progress = 2 * param.progress - 1;
            } else {
                direction = 1;
                progress = 2 * param.progress;
            }
            AnimEaseFunction easing = NeoEase::QuarticInOut;
            progress = easing(progress);

            uint16_t StepWidth = segment.getPixelCountAtRing(j);

            int16_t next_pixel, last_pixel;
            // use the curved progress to calculate next pixel. pixels are 0 -> StepWidth-1
            if (direction > 0) {
                next_pixel = progress * StepWidth;
            } else {
                next_pixel = (1.0f - progress) * StepWidth;
            }
            // progress = 1 is so brief, that we include it with 0.90 - > 0.99 
            if (next_pixel == StepWidth) {
                next_pixel -= 1;
            }

            RgbwColor color;
            // not storing s_last_pixel, so iterate backwards and find the leading edge of the trail
            for (last_pixel = next_pixel; ; last_pixel -= direction) {
                // GetPixelColor returns a Rgbw color object
                uint8_t this_brightness = strip->GetPixelColor(segment.Map(j, last_pixel)).CalculateBrightness();
                uint8_t prev_brightness = strip->GetPixelColor(segment.Map(j, last_pixel + direction)).CalculateBrightness();

                if (last_pixel == 0 || last_pixel == StepWidth - 1) {
                    prev_brightness = 0;
                }

                if (this_brightness > prev_brightness ) {
                    color = strip->GetPixelColor(segment.Map(j, last_pixel));
                    break;
                } 
            }

            // determine brightness by converting to Hsb
            HsbColor colorHsb = HsbColor(RgbColor(color.R, color.G, color.B));
            int darken_by = 40 * colorHsb.B + 1;
            // darken the pixels on the strip
            for (uint16_t i = 0; i < StepWidth; i++)
            {
                RgbwColor pixel_color = strip->GetPixelColor(segment.Map(j, i));
                pixel_color.Darken(darken_by);
                strip->SetPixelColor(segment.Map(j, i), pixel_color);
            }

            // how many pixels missed?
            uint8_t pixel_diff = abs(next_pixel - last_pixel);

            uint8_t i = 0;
            do {
                uint16_t i_pixel = next_pixel - i * direction;
                strip->SetPixelColor(segment.Map(j, i_pixel), color);
                i++;
            } while ( i < pixel_diff);

            if (param.state == AnimationState_Completed) {
                uint16_t time = 400 + esp_random()%600;
                animations->ChangeAnimationDuration(j, time);
                animations->RestartAnimation(j);
            }
        };

        // start the animation for the first time
        uint16_t time = 400 + esp_random()%600;
        animations->StartAnimation(j, time, animUpdate);
    }
}

// similar to Cylon, but go left-right-left
void SnakeAnimationSet()
{
    static uint16_t s_last_pixel;
    static int8_t s_direction;
    static float hue;

    s_last_pixel = 0;
    s_direction = 1;
 
    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        if (param.state == AnimationState_Started) {
            hue = (float)(1.0*esp_random()/UINT32_MAX);
        }

        float brightness = atomic_brightness/100.0f;
        brightness = pow(brightness,2.2);
        brightness = fmax(0.03, brightness);

        HsbColor hsbColor = HsbColor(hue, 1.0, brightness); 
        
        AnimEaseFunction easing = NeoEase::QuadraticInOut;
        float progress = easing(param.progress);

        // darken all pixels
        int darken_by = 50 * hsbColor.B + 1;
        for (uint16_t i = 0; i < strip->PixelCount(); i++) {
            RgbwColor pixel_color = strip->GetPixelColor(i);
            pixel_color.Darken(darken_by);
            strip->SetPixelColor(i, pixel_color);
        }

        // work out which pixel is next
        uint16_t next_pixel;
        if (s_direction > 0) {
            next_pixel = progress * strip->PixelCount();
        }
        else {
            next_pixel = (1.0f - progress) * strip->PixelCount();
        }
        if (next_pixel == strip->PixelCount()) {
            next_pixel -= 1;
        }

        // how many pixels missed?
        uint8_t pixel_diff = abs(next_pixel - s_last_pixel);

        uint8_t i = 0;
        do {
            uint8_t step_num = 0;
            uint8_t pixel_num = 0;
            uint16_t pixel_count = 0;
            uint16_t i_pixel = next_pixel - i * s_direction;

            uint8_t NumSteps = segment.getCountOfRings();
            for (step_num = 0; step_num < NumSteps; step_num++ ) {
                pixel_num = i_pixel - pixel_count;
                pixel_count += segment.getPixelCountAtRing(step_num);  
                if (pixel_count > i_pixel) {
                    break;
                }
            }

            if(step_num % 2 == 0) {
                pixel_num = segment.getPixelCountAtRing(step_num) - pixel_num -1;
            }

            strip->SetPixelColor(segment.Map(step_num, pixel_num), hsbColor);

            i++;
        } while ( i < pixel_diff);

        s_last_pixel = next_pixel;
        
        if (param.state == AnimationState_Completed) {     
            s_direction *= -1;

            uint16_t time = 1000 + esp_random()%1000;
            animations->AnimationDuration(time);
            animations->RestartAnimation(param.index);
        }
    };

    uint16_t time = 1000 + esp_random()%1000;
    animations->StartAnimation(0, time, animUpdate);
}


void FireworksAnimationSetHsb() 
{
    // Set random pixels on (exclude bottom and top step)

    for (uint16_t indexPixel = segment.getPixelCountAtRing(0); indexPixel < strip->PixelCount() - segment.getPixelCountAtRing(segment.getCountOfRings()-1); indexPixel++)  {
        if(esp_random()%300 == 0) {
            HsbColor hsbColor = HsbColor(1.0f*esp_random()/UINT32_MAX, 1.0, ( 0.2f + (1.0f*esp_random()/UINT32_MAX)/2.0f ));
            strip->SetPixelColor(indexPixel, hsbColor);
        }
    }

    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        uint8_t inc = 0;
        bool turn = false;

        // Darken all pixels a little. Do it here; as the loop code below increments up to
        // the brightest pixel, and then decrements away. However, the decrement comes after
        // the brightest pixel, so if you dim that and then move to the next pixel, it would be a
        // different value than the left pixel used as a value.
        for (uint16_t i = 0; i < strip->PixelCount(); i++ ) {
            RgbwColor color = strip->GetPixelColor(i);
            HsbColor hsbColor = HsbColor(RgbColor(color.R, color.G, color.B));
            hsbColor = HsbColor(hsbColor.H, 1.0, hsbColor.B/1.1f);
            strip->SetPixelColor(i, hsbColor);
        }

        // Left to Right first
        uint8_t NumSteps = segment.getCountOfRings();
        for (uint16_t j = 0; j < NumSteps; j++ ) {
            uint16_t StepWidth = segment.getPixelCountAtRing(j);
            for (uint16_t i = 0; i < StepWidth; i++) {
                RgbwColor this_pixel, left_pixel, right_pixel;
                HsbColor hsb_this_pixel, hsb_left_pixel, hsb_right_pixel;
                float brightness = 0.0, hue = 0.0;

                this_pixel = strip->GetPixelColor(segment.Map(j, i));
                left_pixel = strip->GetPixelColor(segment.Map(j, i-1));
                right_pixel = strip->GetPixelColor(segment.Map(j, i+1));

                hsb_this_pixel = HsbColor(RgbColor(this_pixel.R, this_pixel.G, this_pixel.B));
                hsb_left_pixel = HsbColor(RgbColor(left_pixel.R, left_pixel.G, left_pixel.B));
                hsb_right_pixel = HsbColor(RgbColor(right_pixel.R, right_pixel.G, right_pixel.B));
                                                
                if (hsb_right_pixel.B > hsb_this_pixel.B) {
                    brightness = hsb_this_pixel.B / 6.0f + hsb_right_pixel.B / 3.0f;
                    hue = hsb_right_pixel.H;
                    inc++;
                    turn = true;
                } else if (turn) {
                    // brightest pixel. do nothing. already dimmed.
                    brightness = hsb_this_pixel.B;
                    hue = hsb_this_pixel.H;
                    turn = false;
                } else if (inc > 0) {
                    brightness = hsb_this_pixel.B / 6.0f + hsb_left_pixel.B / 3.0f;
                    hue = hsb_left_pixel.H;
                    inc--;
                }
                brightness = brightness > 1.0 ? 1.0 : brightness;

                HsbColor color = HsbColor(hue, 1.0f, brightness);
                strip->SetPixelColor(segment.Map(j, i), color);
            }
        }

        // Bottom to Top
        uint16_t StepWidth = segment.getPixelCountAtRing(0);
        for (uint16_t i = 0; i < StepWidth; i++) {
            for (uint16_t j = 0; j < NumSteps; j++ ) {
                RgbwColor this_pixel, bottom_pixel, top_pixel;
                HsbColor hsb_this_pixel, hsb_bottom_pixel, hsb_top_pixel;
                float brightness = 0.0, hue = 0.0;

                this_pixel = strip->GetPixelColor(segment.Map(j, i));
                bottom_pixel = strip->GetPixelColor(segment.Map(j-1, i));
                top_pixel = strip->GetPixelColor(segment.Map(j+1, i));

                hsb_this_pixel = HsbColor(RgbColor(this_pixel.R, this_pixel.G, this_pixel.B));
                hsb_bottom_pixel = HsbColor(RgbColor(bottom_pixel.R, bottom_pixel.G, bottom_pixel.B));
                hsb_top_pixel = HsbColor(RgbColor(top_pixel.R, top_pixel.G, top_pixel.B));

                if (hsb_top_pixel.B > hsb_this_pixel.B) {
                    brightness = hsb_this_pixel.B / 6.0f + hsb_top_pixel.B / 3.0f;
                    hue = hsb_top_pixel.H;
                    inc++;
                    turn = true;
                } else if (turn) {
                    // brightest pixel. do nothing. already dimmed.
                    brightness = hsb_this_pixel.B ;
                    hue = hsb_this_pixel.H;
                    turn = false;
                } else if (inc > 0) {
                    brightness = hsb_this_pixel.B / 6.0f + hsb_bottom_pixel.B / 3.0f;
                    hue = hsb_bottom_pixel.H;
                    inc--;
                }
                brightness = brightness > 1.0 ? 1.0 : brightness;

                HsbColor color = HsbColor(hue, 1.0f, brightness);
                strip->SetPixelColor(segment.Map(j, i), color);
            }
        }

        // animation doesn't use 'progress'
        // pixels will still be lit when this animation completes, causing additional
        // pixels (fireworks) to be generated before the previous have faded away
        if (param.state == AnimationState_Completed) {
            FireworksAnimationSetHsb();
        }
    };

    // fire off the animation even before the previous firework has faded away
    // this ensures fireworks are being generated at random times
    animations->StartAnimation(0, 50, animUpdate);
    
}



void animation_task(void * param)
{
    strip->Begin();   
    strip->Show();

    while(1) {
        if (animations->IsAnimating()) {
            animations->UpdateAnimations();
            strip->Show();
        }
        vTaskDelay(3);
    }
}

void animation_select_task(void * param)
{
    s_led_message_queue = xQueueCreate( 10, sizeof(led_strip_t));
    led_strip_t led_strip;

    led_strip.hue               = 0.0f;
    led_strip.saturation        = 0.0f;
    led_strip.brightness        = 0;
    led_strip.animate           = false;

    set_strip(led_strip);

    while(1) {
        if (xQueueReceive(s_led_message_queue, (void *) &led_strip, portMAX_DELAY) == pdTRUE) {
            if (led_strip.animate) {
                animations->StopAll();
                strip->ClearTo(HsbColor(0.0, 0.0, 0.0));
                strip->Show();

                vTaskDelay(50);

                atomic_brightness = led_strip.brightness;
                
                switch(led_strip.animation_id) {
                    case 1:
                        CylonAnimationSet();
                        break;
                    case 2:
                        GlitterAnimationSet();
                        break;
                    case 3:
                        StepCylonAnimationSet();
                        break;
                    case 4:
                        RainbowFadeAnimationSet();
                        break;
                    case 5:
                        FireworksAnimationSetHsb();
                        break;
                    case 6:
                        FlickerAnimationSet(led_strip.hue, led_strip.saturation);
                        break;  
                    case 7:
                        SnakeAnimationSet();
                        break;  
                    case 8:
                        ColorCycleAnimationSet(led_strip.hue, led_strip.saturation);
                        break;         
                }
            } 
            
            else {
                if (animations->IsAnimating()) {
                    animations->StopAll();
                }
                // look at custom/switch id and determine direction for fade
                int8_t direction = 0;
                switch (led_strip.custom_id) {
                    case 0:
                        direction = 0;
                        break;
                    case 1:
                        direction = 1;
                        break;
                    case 2:
                        direction = -1;
                        break;
                }
                FadeAnimationSet(HsbColor(led_strip.hue, led_strip.saturation, led_strip.brightness/100.0f), direction);
            }
        }
    }
}

esp_err_t start_animation_task() {

    esp_err_t err;
    nvs_handle config_handle;
    uint8_t data_gpio;
    err = nvs_open("lights", NVS_READWRITE, &config_handle);
    if (err == ESP_OK) {
        // Data GPIO
        err = nvs_get_u8(config_handle, "data_gpio", &data_gpio); 
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "error nvs_get_u8 data_gpio err %d", err);
        }
        nvs_close(config_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unable to create strip or animations object. data pin not defined");
        return ESP_ERR_NOT_FOUND;
    }

    // read in pixel_layout
    segment.Begin();

    if (segment.getCountOfRings() == 0) {
        ESP_LOGE(TAG, "unable to create strip or animations object. ring/strip not defined");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Ring/segment size %d.     Num Pixels % d", segment.getCountOfRings(), segment.getPixelCount());

    if (strip != NULL) {  
       delete strip;
    }
    if (animations != NULL) {  
       delete animations;
    }

    strip = new NeoPixelBus<NeoGrbwFeature, NeoEsp32Rmt0Sk6812Method>(segment.getPixelCount(), data_gpio);   // using RMT
    animations = new NeoPixelAnimator(segment.getPixelCount(), NEO_CENTISECONDS);

    if (strip == NULL || animations == NULL) {
        ESP_LOGE(TAG, "unable to create strip or animations object. out of memory");
        return ESP_ERR_NO_MEM;
    }



    xTaskCreatePinnedToCore(&animation_task, "anim", 4096, NULL, 10, NULL, 1);

    xTaskCreate(&animation_select_task, "anim_select", 4096, NULL, 5, NULL);

    return ESP_OK;
}

void set_strip(led_strip_t led_strip) {
    xQueueSendToBack(s_led_message_queue, (void *) &led_strip, (TickType_t) 0);
}

void set_brightness(int brightness) {
    atomic_brightness = brightness;
}
