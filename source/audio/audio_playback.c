// Copyright 2023 elagil

/**
 * @file
 * @brief   Audio playback module.
 * @details Contains audio playback-related functions and structures.
 *
 * @addtogroup audio
 * @{
 */

#include "audio_playback.h"

#include <string.h>

static void audio_playback_reset(void);

/**
 * @brief The state of the feedback correction.
 */
enum AUDIO_FEEDBACK_CORRECTION_STATE {
    AUDIO_PLAYBACK_FEEDBACK_CORRECTION_STATE_OFF,       ///< No feedback correction active.
    AUDIO_PLAYBACK_FEEDBACK_CORRECTION_STATE_DECREASE,  ///< Decrease the feedback value in case of over-filled audio
                                                        ///< buffer.
    AUDIO_PLAYBACK_FEEDBACK_CORRECTION_STATE_INCREASE   ///< Increase the feedback value in case of under-filled audio
                                                        ///< buffer.
};

/**
 * @brief A structure that holds the state of audio playback, as well as the audio buffer.
 */
struct audio_playback {
    size_t  packet_size;                                            ///< The nominal audio packet size.
    uint8_t buffer[AUDIO_MAX_BUFFER_SIZE + AUDIO_MAX_PACKET_SIZE];  ///< The audio sample buffer.
    size_t  buffer_size;                                            ///< The nominal size of the audio buffer.
    size_t  buffer_write_offset;                                    ///< The current write offset in bytes (USB).
    size_t  buffer_read_offset;                                     ///< The current read offset in bytes (I2S).
    size_t  buffer_target_fill_size;  ///< The number of audio sample bytes to collect before starting playback.
    size_t  buffer_fill_size;         ///< The fill size, which is the distance between read (I2S) and write (USB)
                                      ///< memory locations, in bytes.
    bool b_streaming_enabled;         ///< True, if audio streaming is enabled, and
                                      ///< data is being received via USB.
    bool b_playback_enabled;          ///< True, if the audio output is enabled, and data
                                      ///< is being output via I2S.
} g_playback;

/**
 * @brief A pointer to the \a audio module's mailbox.
 */
static mailbox_t *gp_mailbox;

/**
 * @brief Get the audio data buffer.
 *
 * @return uint8_t* The data buffer.
 */
uint8_t *audio_playback_get_buffer(void) { return g_playback.buffer; }

/**
 * @brief Get the audio data buffer size.
 *
 * @return size_t The data buffer size.
 */
size_t audio_playback_get_buffer_size(void) { return g_playback.buffer_size; }

/**
 * @brief Get the audio buffer fill size in bytes.
 *
 * @return size_t The fill size.
 */
size_t audio_playback_get_buffer_fill_size(void) { return g_playback.buffer_fill_size; }

/**
 * @brief Get the audio buffer target fill size in bytes.
 *
 * @return size_t The target fill size.
 */
size_t audio_playback_get_buffer_target_fill_size(void) { return g_playback.buffer_target_fill_size; }

/**
 * @brief Get the audio data packet size.
 *
 * @return size_t The audio data packet size.
 */
size_t audio_playback_get_packet_size(void) { return g_playback.packet_size; }

/**
 * @brief Check if audio playback via I2S is enabled.
 *
 * @return true if audio playback is enabled.
 * @return false if audio playback is disabled.
 */
bool audio_playback_is_enabled(void) { return g_playback.b_playback_enabled; }

/**
 * @brief Check if audio streaming is enabled.
 *
 * @return true if audio streaming is enabled.
 * @return false if audio streaming is disabled.
 */
bool audio_playback_is_streaming_enabled(void) { return g_playback.b_streaming_enabled; }

/**
 * @brief Set a new audio quality, defined by sample rate and resolution.
 *
 * @param sample_rate_hz The selected sample rate in Hz.
 */
void audio_playback_set_sample_rate(uint32_t sample_rate_hz) {
    g_playback.packet_size = AUDIO_COMMON_GET_PACKET_SIZE(AUDIO_CHANNEL_COUNT, sample_rate_hz, AUDIO_SAMPLE_SIZE);
    g_playback.buffer_size = AUDIO_COMMON_GET_BUFFER_SIZE(AUDIO_BUFFER_PACKET_COUNT, g_playback.packet_size);

    // By adding half a packet size, the buffer level is equal to half the buffer size on average. Buffer level is
    // measured only after USB packets have arrived and count towards the buffer level.
    g_playback.buffer_target_fill_size = g_playback.buffer_size / 2u + g_playback.packet_size / 2u;
}

/**
 * @brief Update the audio buffer write offset, taking into account wrap-around of the circular buffer.
 * @details If the nominal buffer size was exceeded by the last packet, the excess is copied to the beginning of the
 * buffer. The audio buffer is large enough to handle excess data of size \a AUDIO_MAX_PACKET_SIZE.
 * @param transaction_size The received audio byte count.
 */
static void audio_playback_update_write_offset(size_t transaction_size) {
    size_t new_buffer_write_offset = g_playback.buffer_write_offset + transaction_size;

    chDbgAssert(new_buffer_write_offset < ARRAY_LENGTH(g_playback.buffer), "Transaction size exceeds audio buffer.");

#if AUDIO_RESOLUTION_BIT == 32u
    // Audio samples are now words (32 bit long).
    // Swap upper and lower 16 bit of received audio samples, as the I2S DMA otherwise transfers them in the wrong
    // order. The I2S DMA handles word transfers as two separate half-word transfers.
    uint32_t    *p_word     = (uint32_t *)(&g_playback.buffer[g_playback.buffer_write_offset]);
    const size_t WORD_COUNT = transaction_size / AUDIO_SAMPLE_SIZE;

    for (size_t word_index = 0; word_index < WORD_COUNT; word_index++) {
        *p_word = SWAP_HALF_WORDS(*p_word);
        p_word++;
    }
#endif

    // Copy excessive data back to the start of the audio buffer.
    if (new_buffer_write_offset > g_playback.buffer_size) {
        size_t excess_byte_count = new_buffer_write_offset - g_playback.buffer_size;
        memcpy((void *)g_playback.buffer, (void *)&g_playback.buffer[g_playback.buffer_size], excess_byte_count);
    }

    g_playback.buffer_write_offset = wrap_unsigned(new_buffer_write_offset, g_playback.buffer_size);
}

/**
 * @brief Determine the I2S DMA's current read offset from the audio buffer start.
 * @details The information is stored in the \a audio_playback structure.
 */
static void audio_playback_update_read_offset(void) {
    size_t ndtr_value = (size_t)(I2S_DRIVER.dmatx->stream->NDTR);

    // For 16 bit audio, the number of data register (NDTR) holds the number of remaining audio samples.
    size_t transferrable_sample_count = ndtr_value;

#if AUDIO_RESOLUTION_BIT == 32u
    // For 32 bit audio, the number of data register still counts 16 bit wide samples.
    transferrable_sample_count /= 2u;
#endif

    if (I2S_DRIVER.state == I2S_ACTIVE) {
        g_playback.buffer_read_offset =
            (size_t)g_playback.buffer_size - (size_t)AUDIO_SAMPLE_SIZE * transferrable_sample_count;
    } else {
        g_playback.buffer_read_offset = 0u;
    }
}

/**
 * @brief Calculate the audio buffer fill size.
 * @details This is the difference in bytes between the write offset (USB) and read offset (I2S DMA) - the number of
 * bytes that can still be written via I2S, before the buffer runs out.
 */
static void audio_playback_update_fill_size(void) {
    // Calculate the distance between the DMA read offset, and the USB driver's write offset in the playback buffer.
    g_playback.buffer_fill_size = subtract_circular_unsigned(g_playback.buffer_write_offset,
                                                             g_playback.buffer_read_offset, g_playback.buffer_size);
}

/**
 * @brief Start playback, when the target audio buffer fill size is reached.
 * @details I2S transfers are started by sending a \a AUDIO_COMMON_MSG_START_PLAYBACK message.
 */
static void audio_playback_start(void) {
    if (audio_playback_is_enabled()) {
        // Playback already enabled.
        return;
    }

    if (g_playback.buffer_fill_size >= g_playback.buffer_target_fill_size) {
        // Signal that the playback buffer is at or above the target fill size. This starts audio playback via I2S.
        g_playback.b_playback_enabled = true;

        chMBPostI(gp_mailbox, AUDIO_COMMON_MSG_START_PLAYBACK);
    }
}

/**
 * @brief Disables audio playback.
 * @details Sends a \a AUDIO_COMMON_MSG_STOP_PLAYBACK message.
 * @note This internally uses I-class functions.
 */
static void audio_playback_stop(void) {
    if (!audio_playback_is_enabled()) {
        // Playback already disabled.
        return;
    }

    audio_playback_reset();

    chMBPostI(gp_mailbox, AUDIO_COMMON_MSG_STOP_PLAYBACK);
}

/**
 * @brief Joint callback for when audio data was received from the host, or the reception failed in the current frame.
 * @note This internally uses I-class functions.
 *
 * @param usbp A pointer to the USB driver structure.
 * @param ep The endpoint, for which the feedback was called.
 */
void audio_playback_received_cb(USBDriver *usbp, usbep_t ep) {
    if (!g_playback.b_streaming_enabled) {
        // Disregard packets, when streaming is disabled.
        return;
    }

    size_t transaction_size = usbGetReceiveTransactionSizeX(usbp, ep);

    chSysLockFromISR();

    if (transaction_size == 0u) {
        // Failed transaction.
        audio_playback_stop();
    } else {
        // Samples were received successfully.
        audio_playback_update_write_offset(transaction_size);
        audio_playback_update_read_offset();
        audio_playback_update_fill_size();
        audio_playback_start();
    }

    usbStartReceiveI(usbp, ep, (uint8_t *)&g_playback.buffer[g_playback.buffer_write_offset], AUDIO_MAX_PACKET_SIZE);

    chSysUnlockFromISR();
}

/**
 * @brief Start streaming audio via USB.
 * @details Is called, when the audio endpoint goes into its operational alternate mode (actual music playback begins).
 *
 * @param usbp The pointer to the USB driver structure.
 */
void audio_playback_start_streaming(USBDriver *usbp) {
    if (g_playback.b_streaming_enabled) {
        // Streaming is already enabled.
        return;
    }

    audio_playback_reset();
    g_playback.b_streaming_enabled = true;

    chSysLockFromISR();

    // Feedback yet unknown, transmit empty packet.
    usbStartTransmitI(usbp, AUDIO_FEEDBACK_ENDPOINT, NULL, 0);

    // Initial audio data reception.
    usbStartReceiveI(usbp, AUDIO_PLAYBACK_ENDPOINT, (uint8_t *)&g_playback.buffer[g_playback.buffer_write_offset],
                     AUDIO_MAX_PACKET_SIZE);

    chSysUnlockFromISR();
}

/**
 * @brief Disable audio streaming and output.
 * @details Is called when the audio endpoint goes into its zero bandwidth alternate mode, or by \a audio_reset() .
 *
 * @param usbp The pointer to the USB driver structure.
 */
void audio_playback_stop_streaming(USBDriver *usbp) {
    (void)usbp;

    if (!g_playback.b_streaming_enabled) {
        // Streaming is already disabled.
        return;
    }

    g_playback.b_streaming_enabled = false;

    chSysLockFromISR();
    audio_playback_stop();
    chSysUnlockFromISR();
}

/**
 * @brief Reset the audio playback structure, initializing it with the current values.
 *
 * @param p_playback The pointer to the structure to reset.
 */
static void audio_playback_reset(void) { audio_playback_init(gp_mailbox, g_playback.b_streaming_enabled); }

/**
 * @brief Initialize the audio playback module.
 *
 * @param b_streaming_enabled If true, USB audio streaming is set to being enabled.
 */
void audio_playback_init(mailbox_t *p_mailbox, bool b_streaming_enabled) {
    gp_mailbox = p_mailbox;

    g_playback.buffer_write_offset = 0u;
    g_playback.buffer_read_offset  = 0u;
    g_playback.buffer_fill_size    = 0u;
    g_playback.b_playback_enabled  = false;
    g_playback.b_streaming_enabled = b_streaming_enabled;
}

/**
 * @}
 */