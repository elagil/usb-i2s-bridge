// Copyright 2023 elagil

/**
 * @file
 * @brief   The main application module.
 * @details Contains the main application thread, and from there sets up
 * - general USB handling,
 * - the audio module,
 * - volume potentiometer ADC readout,
 * - amplifier/DAC controls, and
 * - reporting functionality.
 *
 * @addtogroup main
 * @{
 */

#include <string.h>

#include "audio.h"
#include "ch.h"
#include "chprintf.h"
#include "hal.h"
#include "tas2780.h"
#include "usb.h"

/**
 * @brief The volume potentiometer ADC sample (12 bit long).
 */
adcsample_t g_adc_sample;

/**
 * @brief Settings structure for the TAS2780 I2C driver.
 */
static const I2CConfig g_tas2780_i2c_config = {
    .op_mode = OPMODE_I2C, .clock_speed = 100000u, .duty_cycle = STD_DUTY_CYCLE};

/**
 * @brief Starts continuous sampling of the volume potentiometer ADC.
 * @note The result is currently unused.
 */
void start_volume_adc(void) {
    // ADC conversion group:
    // - continuous conversion
    // - 480 samples conversion time
    // - Channel 9
    static const ADCConversionGroup adc_conversion_group = {.circular     = TRUE,
                                                            .num_channels = 1,
                                                            .end_cb       = NULL,
                                                            .error_cb     = NULL,
                                                            .cr1          = 0u,
                                                            .cr2          = ADC_CR2_SWSTART,
                                                            .smpr1        = 0u,
                                                            .smpr2        = ADC_SMPR2_SMP_AN9(ADC_SAMPLE_480),
                                                            .htr          = 0u,
                                                            .ltr          = 0u,
                                                            .sqr1         = 0u,
                                                            .sqr2         = 0u,
                                                            .sqr3         = ADC_SQR3_SQ1_N(ADC_CHANNEL_IN9)};

    // Start continuous conversion.
    adcStart(&ADCD1, NULL);
    adcStartConversion(&ADCD1, &adc_conversion_group, &g_adc_sample, 1u);
}

#if ENABLE_REPORTING == TRUE
static THD_WORKING_AREA(wa_reporting_thread, 128);

/**
 * @brief A reporting thread that outputs status information via UART.
 */
static THD_FUNCTION(reporting_thread, arg) {
    (void)arg;
    static BaseSequentialStream *p_stream = (BaseSequentialStream *)&SD2;

    sdStart(&SD2, NULL);
    chRegSetThreadName("reporting");

    chprintf(p_stream, "Using %u byte per sample at %lu Hz, %lu byte per frame.\n", AUDIO_SAMPLE_SIZE,
             AUDIO_SAMPLE_RATE_HZ, AUDIO_PACKET_SIZE);
    chprintf(p_stream, "Audio buffer holds %lu bytes (%lu packets).\n", AUDIO_BUFFER_SIZE, AUDIO_BUFFER_PACKET_COUNT);

    while (true) {
        tas2780_ensure_active_all();

        uint8_t noise_gate_mask = tas2780_get_noise_gate_mask_all();
        chprintf(p_stream, "Noise gate: %u\n", noise_gate_mask);

        chprintf(p_stream, "Potentiometer: %u\n",
                 g_adc_sample >> 4);  // Convert to an 8 bit number.

        chprintf(p_stream, "Volume: %li / %li dB\n", (audio_channel_get_volume(AUDIO_CHANNEL_LEFT) >> 8),
                 (audio_channel_get_volume(AUDIO_CHANNEL_RIGHT) >> 8));

        chprintf(p_stream, "Audio buffer fill level: %lu / %lu\n", audio_get_fill_level(), AUDIO_BUFFER_SIZE);

        chThdSleepMilliseconds(1000);
    }
}
#endif

/**
 * @brief Application entry point.
 *
 * @return int The return code.
 */
int main(void) {
    halInit();
    chSysInit();

    event_source_t *p_audio_event_source = audio_get_event_source();

    // Initialize audio module.
    audio_setup();

    // Initialize the USB module.
    usb_setup();

    // Setup amplifiers.
    i2cStart(&I2CD1, &g_tas2780_i2c_config);
    tas2780_setup_all();

    // Begin reading volume potentiometer ADC.
    start_volume_adc();

    // Registers this thread for audio events.
    static event_listener_t audio_event_listener;
    chEvtRegisterMaskWithFlags(p_audio_event_source, &audio_event_listener, AUDIO_EVENT,
                               AUDIO_EVENT_RESET_VOLUME | AUDIO_EVENT_SET_MUTE_STATE | AUDIO_EVENT_SET_VOLUME);

#if ENABLE_REPORTING == TRUE
    // Create reporting thread.
    chThdCreateStatic(wa_reporting_thread, sizeof(wa_reporting_thread), NORMALPRIO, reporting_thread, NULL);
#endif

    // Wait for an audio event.
    while (true) {
        chEvtWaitOne(AUDIO_EVENT);
        eventflags_t event_flags = chEvtGetAndClearFlags(&audio_event_listener);

        if (event_flags & AUDIO_EVENT_RESET_VOLUME) {
            // Restore volume levels to maximum when instructed (after streaming ends).
            tas2780_set_volume_all(TAS2780_VOLUME_MAX, TAS2780_CHANNEL_BOTH);
        }

        // Joint handling of volume and mute controls. Only adjust volume, when streaming over USB. Other audio sources
        // must not be affected by USB volume adjustments.
        if (((event_flags & AUDIO_EVENT_SET_MUTE_STATE) || (event_flags & AUDIO_EVENT_SET_VOLUME)) &&
            audio_is_streaming()) {
            if (audio_channel_is_muted(AUDIO_CHANNEL_LEFT)) {
                tas2780_set_volume_all(TAS2780_VOLUME_MUTE, TAS2780_CHANNEL_LEFT);
            } else {
                tas2780_set_volume_all(audio_channel_get_volume(AUDIO_CHANNEL_LEFT), TAS2780_CHANNEL_LEFT);
            }

            if (audio_channel_is_muted(AUDIO_CHANNEL_RIGHT)) {
                tas2780_set_volume_all(TAS2780_VOLUME_MUTE, TAS2780_CHANNEL_RIGHT);
            } else {
                tas2780_set_volume_all(audio_channel_get_volume(AUDIO_CHANNEL_RIGHT), TAS2780_CHANNEL_RIGHT);
            }
        }
    }
}

/**
 * @}
 */
