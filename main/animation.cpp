#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/param.h>   

#include "esp_log.h"
static const char *TAG = "anim";

#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>

#include "animation.h"

const uint8_t PixelPin = 13;  
const uint16_t PixelCount = 300;

class MyRingsLayout
{
protected:
    const uint16_t Rings[15] = {0, 20, 40, 60, 80, 100, 120, 145, 170, 190, 210, 230, 255, 280, PixelCount}; 
};
// use the MyRingsLayout to declare the topo object
NeoRingTopology<MyRingsLayout> stairs;

// Default is NeoEsp32Rmt6Ws2812xMethod (channel 6)
NeoPixelBus<NeoGrbwFeature, Neo800KbpsMethod> strip(PixelCount, PixelPin);
NeoPixelAnimator animations(PixelCount, NEO_CENTISECONDS);

static QueueHandle_t s_led_message_queue;


// Transistion between colours wasn't smooth when trying to have each pixel a 
// separate animation
void FadeAnimationSet(HsbColor targetColor, int8_t direction)
{
    RgbwColor rgbwTargetColor = targetColor;
    rgbwTargetColor.W = MIN( rgbwTargetColor.R, MIN(rgbwTargetColor.G, rgbwTargetColor.B));
    rgbwTargetColor.R = rgbwTargetColor.R - rgbwTargetColor.W; 
    rgbwTargetColor.G = rgbwTargetColor.G - rgbwTargetColor.W; 
    rgbwTargetColor.B = rgbwTargetColor.B - rgbwTargetColor.W; 

    NeoGamma<NeoGammaTableMethod> colorGamma;
    rgbwTargetColor = colorGamma.Correct(rgbwTargetColor);
    RgbwColor originalColor = strip.GetPixelColor(0);
    AnimEaseFunction easing =  NeoEase::Linear;
    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        float progress = easing(param.progress);
        float step_progress;

        uint8_t NumSteps = stairs.getCountOfRings();

        for (uint8_t j = 0; j < NumSteps; j++) {
            if (direction > 0) {
                step_progress = fmin(1.0f,fmax(0.0f,( 2.0f*progress - 1.0f*j/(NumSteps-1.0f))));
            } else if (direction < 0) {
                step_progress = fmin(1.0f,fmax(0.0f,( 2.0f*progress - (1.0f*NumSteps-1.0f-j)/(NumSteps-1.0f))));
            } else {
                step_progress = progress;
            }

            RgbwColor updatedColor = RgbwColor::LinearBlend(originalColor, rgbwTargetColor, 
                step_progress  
            );

            uint16_t StepWidth = stairs.getPixelCountAtRing(j);

            for (uint16_t i = 0; i < StepWidth; i++) {
                strip.SetPixelColor(stairs.Map(j, i), updatedColor);
            }
        }

        if (param.state == AnimationState_Completed) {
            animations.StopAnimation(param.index);
        }
    };

    animations.StartAnimation(0, 100, animUpdate);

}


void RainbowFadeAnimationSet()
{
    AnimEaseFunction easing =  NeoEase::Linear;

    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        float progress = easing(param.progress);

        uint8_t NumSteps = stairs.getCountOfRings();

        for (uint8_t j = 0; j < NumSteps; j++) {
            float hue = progress + (1.0*j/NumSteps);
            if (hue > 1) {
                hue -= 1;
            }

            HsbColor color = HsbColor(hue, 1.0f, 0.1f);

            uint16_t StepWidth = stairs.getPixelCountAtRing(j);

            for (uint16_t i = 0; i < StepWidth; i++) {
                strip.SetPixelColor(stairs.Map(j, i), color);
            }
        }

        if (param.state == AnimationState_Completed) {
//            animations.RestartAnimation(param.index);
            RainbowFadeAnimationSet();
        }
    };

    animations.StartAnimation(0, 1000, animUpdate);

}


void FlickerAnimationSet(HsbColor pickedColor)
{
    // Every pixel is a standalone animation
    for (uint16_t pixel = 0; pixel < PixelCount; pixel++)
    {
        // We need the current brightness of the pixel.
        RgbwColor originalColorRgbw = strip.GetPixelColor(pixel);
        HsbColor originalColor = HsbColor(RgbColor(originalColorRgbw.R, originalColorRgbw.G, originalColorRgbw.B));
        originalColor = HsbColor(pickedColor.H, pickedColor.S, originalColor.B);

        // don't let brightness go higher than picked color
        float brightness = fmin( pickedColor.B, (1.0*esp_random()/UINT32_MAX));
        brightness = pow(brightness,2.2);

        HsbColor targetColor = HsbColor(originalColor.H, originalColor.S, brightness);

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
            easing = NeoEase::QuadraticInOut;
            break;
        case 3:
            easing = NeoEase::ExponentialIn;
            break;
        case 4:
            easing = NeoEase::SinusoidalIn;
            break;
        case 5:
            easing = NeoEase::QuarticOut;
            break;
        }

        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
            float progress = easing(param.progress);

            RgbwColor updatedColor = RgbwColor::LinearBlend(originalColor, targetColor, progress);
            strip.SetPixelColor(pixel, updatedColor);

            if (!animations.IsAnimating()) {
                FlickerAnimationSet(pickedColor);
            }
        };

        animations.StartAnimation(pixel, 200, animUpdate);
    }
}


void GlitterAnimationSet(float max_brightness)
{
    // Every pixel is a standalone animation
    for (uint16_t pixel = 0; pixel < PixelCount; pixel++)
    {
        // each animation starts with the color that was present
        RgbwColor originalColor = strip.GetPixelColor(pixel);
        // and ends with a random color
        float hue = (float)(1.0*esp_random()/UINT32_MAX);

        float brightness = fmin(max_brightness, (1.0*esp_random()/UINT32_MAX));
        brightness = pow(brightness,2.2);

        HsbColor targetColorHsb = HsbColor(hue, 1.0, brightness);
        RgbwColor targetColor = RgbwColor(targetColorHsb);

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
            easing = NeoEase::QuadraticInOut;
            break;
        case 3:
            easing = NeoEase::ExponentialIn;
            break;
        case 4:
            easing = NeoEase::SinusoidalIn;
            break;
        case 5:
            easing = NeoEase::QuarticOut;
            break;
        }

        // we must supply a function that will define the animation, in this example
        // we are using "lambda expression" to define the function inline, which gives
        // us an easy way to "capture" the originalColor and targetColor for the call back.
        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
            float progress = easing(param.progress);

            RgbwColor updatedColor = RgbwColor::LinearBlend(originalColor, targetColor, progress);
            strip.SetPixelColor(pixel, updatedColor);

            // luckily, this will return false whilst executing the final animation
            if (!animations.IsAnimating()) {
                GlitterAnimationSet(max_brightness);
            }
        };

        // now use the animation properties we just calculated and start the animation
        // which will continue to run and call the update function until it completes
        animations.StartAnimation(pixel, 200, animUpdate);

    }
}


void CylonAnimationSet()
{
    // static variables are only created the first time a function is called. 
    // they retain values between calls. they act like global variables.
    static uint16_t s_last_pixel;
    static int8_t s_direction;
    static RgbwColor color;

    s_last_pixel = 0;
    s_direction = 1;

    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        if (param.state == AnimationState_Started) {
            color = RgbwColor(esp_random()%255, esp_random()%255, esp_random()%255, 0);
        }
        NeoGamma<NeoGammaTableMethod> colorGamma;

        AnimEaseFunction easing = NeoEase::QuarticInOut;
        // progress will start at 0.0 and end at 1.0. we convert to the curve we want
        float progress = easing(param.progress);

        // darken all pixels
        RgbwColor pixel_color;
        for (uint16_t i = 0; i < PixelCount; i++)
        {
            pixel_color = strip.GetPixelColor(i);
            pixel_color.Darken(20);
            strip.SetPixelColor(i, pixel_color);
        }

        // use the curved progress to calculate the pixel to effect.
        // include ALL stairs
        uint16_t next_pixel;
        if (s_direction > 0) {
            next_pixel = progress * PixelCount;
        }
        else {
            next_pixel = (1.0f - progress) * PixelCount;
        }
        if (next_pixel == PixelCount) {
            next_pixel -= 1;
        }

        // how many pixels missed?
        uint8_t pixel_diff = abs(next_pixel - s_last_pixel);

        uint8_t i = 0;
        do {
            uint16_t i_pixel = next_pixel - i * s_direction;
            strip.SetPixelColor(i_pixel, colorGamma.Correct(color));
            i++;
        } while ( i < pixel_diff);

        s_last_pixel = next_pixel;
        
        if (param.state == AnimationState_Completed) {
            s_direction *= -1;

            // time is centiseconds
            uint16_t time = 1000 + esp_random()%1000;

            animations.AnimationDuration(time);
            animations.RestartAnimation(param.index);
        }
    };

    animations.StartAnimation(0, 1000, animUpdate);
}


void StepCylonAnimationSet()
{
    uint8_t NumSteps = stairs.getCountOfRings();
    for (uint8_t j = 0; j < NumSteps; j++) {
        
        uint16_t time = 400 + esp_random()%600;

        AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
        {
            RgbwColor color;
            int16_t next_pixel, last_pixel;
            int8_t direction;
            float progress;

            if (param.state == AnimationState_Started) {
                HsbColor hsbColor = HsbColor(1.0f*esp_random()/UINT32_MAX, 1.0, ( 0.2f + (1.0f*esp_random()/UINT32_MAX)/2.0f ));                strip.SetPixelColor(stairs.Map(j, 0), hsbColor);
            }

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

            uint16_t StepWidth = stairs.getPixelCountAtRing(j);

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

            // not storing s_last_pixel, so iterate backwards and find the leading edge of the trail
            for (last_pixel = next_pixel; ; last_pixel -= direction) {
                uint8_t this_brightness = strip.GetPixelColor(stairs.Map(j, last_pixel)).CalculateBrightness();
                uint8_t prev_brightness = strip.GetPixelColor(stairs.Map(j, last_pixel + direction)).CalculateBrightness();

                if (last_pixel == 0 || last_pixel == StepWidth - 1) {
                    prev_brightness = 0;
                }

                if (this_brightness > prev_brightness ) {
                    color = strip.GetPixelColor(stairs.Map(j, last_pixel));
                    break;
                } 
            }

            // darken the pixels on the strip
            for (uint16_t i = 0; i < StepWidth; i++)
            {
                RgbwColor pixel_color = strip.GetPixelColor(stairs.Map(j, i));
                pixel_color.Darken(20);
                strip.SetPixelColor(stairs.Map(j, i), pixel_color);
            }

            // how many pixels missed?
            uint8_t pixel_diff = abs(next_pixel - last_pixel);

            uint8_t i = 0;
            do {
                uint16_t i_pixel = next_pixel - i * direction;
                strip.SetPixelColor(stairs.Map(j, i_pixel), color);
                i++;
            } while ( i < pixel_diff);

            if (param.state == AnimationState_Completed) {
                uint16_t time = 400 + esp_random()%600;
                
                animations.ChangeAnimationDuration(j, time);
                animations.RestartAnimation(j);
            }
        };

        animations.StartAnimation(j, time, animUpdate);
    }
}


// similar to Cylon, but go left-right-left
void SnakeAnimationSet()
{
    static uint16_t s_last_pixel;
    static int8_t s_direction;
    static RgbwColor color;

    s_last_pixel = 0;
    s_direction = 1;
 
    AnimUpdateCallback animUpdate = [=](const AnimationParam& param)
    {
        if (param.state == AnimationState_Started) {
            color = RgbwColor(esp_random()%255, esp_random()%255, esp_random()%255, 0);
        }

        NeoGamma<NeoGammaTableMethod> colorGamma;

        AnimEaseFunction easing = NeoEase::QuarticInOut;
        // progress will start at 0.0 and end at 1.0. we convert to the curve we want
        float progress = easing(param.progress);

        // darken all pixels
        RgbwColor pixel_color;
        for (uint16_t i = 0; i < PixelCount; i++)
        {
            pixel_color = strip.GetPixelColor(i);
            pixel_color.Darken(20);
            strip.SetPixelColor(i, pixel_color);
        }

        // work out which pixel is next
        uint16_t next_pixel;
        if (s_direction > 0) {
            next_pixel = progress * PixelCount;
        }
        else {
            next_pixel = (1.0f - progress) * PixelCount;
        }
        if (next_pixel == PixelCount) {
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

            uint8_t NumSteps = stairs.getCountOfRings();
            for (step_num = 0; step_num < NumSteps; step_num++ ) {
                pixel_num = i_pixel - pixel_count;
                pixel_count += stairs.getPixelCountAtRing(step_num);  
                if (pixel_count > i_pixel) {
                    break;
                }
            }

            if(step_num % 2 == 0) {
                pixel_num = stairs.getPixelCountAtRing(step_num) - pixel_num -1;
            }

            strip.SetPixelColor(stairs.Map(step_num, pixel_num), colorGamma.Correct(color));

            i++;
        } while ( i < pixel_diff);

        s_last_pixel = next_pixel;
        
        if (param.state == AnimationState_Completed) {     
            s_direction *= -1;

            uint16_t time = 1000 + esp_random()%1000;

            animations.AnimationDuration(time);
            animations.RestartAnimation(param.index);
        }
    };

    animations.StartAnimation(0, 1000, animUpdate);
}


void FireworksAnimationSetHsb() {
    // Set random pixels on (exclude bottom and top step)

    for (uint16_t indexPixel = stairs.getPixelCountAtRing(0); indexPixel < PixelCount - stairs.getPixelCountAtRing(stairs.getCountOfRings()-1); indexPixel++)  {
        if(esp_random()%300 == 0) {
            HsbColor hsbColor = HsbColor(1.0f*esp_random()/UINT32_MAX, 1.0, ( 0.2f + (1.0f*esp_random()/UINT32_MAX)/2.0f ));
            strip.SetPixelColor(indexPixel, hsbColor);
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
        for (uint16_t i = 0; i < PixelCount; i++ ) {
            RgbwColor color = strip.GetPixelColor(i);
            HsbColor hsbColor = HsbColor(RgbColor(color.R, color.G, color.B));
            hsbColor = HsbColor(hsbColor.H, 1.0, hsbColor.B/1.1f);
            strip.SetPixelColor(i, hsbColor);
        }

        // Left to Right first
        uint8_t NumSteps = stairs.getCountOfRings();
        for (uint16_t j = 0; j < NumSteps; j++ ) {
            uint16_t StepWidth = stairs.getPixelCountAtRing(j);
            for (uint16_t i = 0; i < StepWidth; i++) {
                RgbwColor this_pixel, left_pixel, right_pixel;
                HsbColor hsb_this_pixel, hsb_left_pixel, hsb_right_pixel;
                float brightness = 0.0, hue = 0.0;

                this_pixel = strip.GetPixelColor(stairs.Map(j, i));
                left_pixel = strip.GetPixelColor(stairs.Map(j, i-1));
                right_pixel = strip.GetPixelColor(stairs.Map(j, i+1));

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
                strip.SetPixelColor(stairs.Map(j, i), color);
            }
        }

        // Bottom to Top
        uint16_t StepWidth = stairs.getPixelCountAtRing(0);
        for (uint16_t i = 0; i < StepWidth; i++) {
            for (uint16_t j = 0; j < NumSteps; j++ ) {
                RgbwColor this_pixel, bottom_pixel, top_pixel;
                HsbColor hsb_this_pixel, hsb_bottom_pixel, hsb_top_pixel;
                float brightness = 0.0, hue = 0.0;

                this_pixel = strip.GetPixelColor(stairs.Map(j, i));
                bottom_pixel = strip.GetPixelColor(stairs.Map(j-1, i));
                top_pixel = strip.GetPixelColor(stairs.Map(j+1, i));

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
                strip.SetPixelColor(stairs.Map(j, i), color);
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
    animations.StartAnimation(0, 50, animUpdate);
    
}






void animation_task(void * param)
{
    strip.Begin();   
    strip.Show();

    while(1) {
        if (animations.IsAnimating()) {
            animations.UpdateAnimations();
            strip.Show();
        }
        vTaskDelay(3);
    }
}

void animation_select_task(void * param)
{
    s_led_message_queue = xQueueCreate( 10, sizeof(led_strip_t));

    led_strip_t led_strip;

    while(1) {
        if (xQueueReceive(s_led_message_queue, (void *) &led_strip, portMAX_DELAY) == pdTRUE) {
             if (led_strip.animate) {
                animations.StopAll();
                strip.ClearTo(HsbColor(0.0, 0.0, 0.0));
                strip.Show();

                switch(led_strip.animation_id) {
                    case 1:
                        CylonAnimationSet();
                        break;
                    case 2:
                        GlitterAnimationSet(led_strip.brightness);
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
                        FlickerAnimationSet(HsbColor(led_strip.hue, led_strip.saturation, led_strip.brightness));
                        break;  
                    case 7:
                        SnakeAnimationSet();
                        break;              
                }
            } else {
                if (animations.IsAnimating()) {
                    animations.StopAll();
                }
                // use direction when a sensor detects person at top or bottom. How?
                FadeAnimationSet(HsbColor(led_strip.hue, led_strip.saturation, led_strip.brightness), 1);
            }
        }
    }
}

void start_animation_task() {
    xTaskCreatePinnedToCore(&animation_task, "anim", 4096, NULL, 10, NULL, 1);

    xTaskCreate(&animation_select_task, "anim_select", 4096, NULL, 5, NULL);
}

void set_strip(led_strip_t led_strip) {
    xQueueSendToBack(s_led_message_queue, (void *) &led_strip, (TickType_t) 0);
}
