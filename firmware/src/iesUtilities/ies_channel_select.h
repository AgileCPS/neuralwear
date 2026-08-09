/*
 * ies_channel_select.h
 *
 * Ported from iES_v0.3-master to Arduino Nicla Voice, April 2026
 *
 * Selective channel transmission for massive bandwidth reduction.
 * Transmit only the channels you need, not all 8 channels.
 *
 * In iES v0.3: Only channels 3 & 4 were transmitted (2 out of 8 channels)
 * This saves 75% of channel data bandwidth!
 *
 * Example: For 2-channel EEG (Fp1, Fp2):
 * - All 8 channels sampled at 250 Hz
 * - Only 2 channels transmitted over BLE
 * - Combined with 24-bit packing: 250 Hz × 2 ch × 3 bytes = 1500 bytes/sec
 * - vs full 8 channels: 250 Hz × 8 ch × 3 bytes = 6000 bytes/sec (75% reduction!)
 */

#ifndef IES_CHANNEL_SELECT_H_
#define IES_CHANNEL_SELECT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Channel selection configuration
 * 
 * This struct defines which channels should be transmitted.
 * Channels are numbered 1-8 (following ADS1299 convention).
 */
typedef struct {
    uint8_t num_channels_to_stream;    // How many channels to transmit (1-8)
    uint8_t channel_indices[8];        // Which channels to transmit (0-based)
} channel_select_config_t;

/**
 * @brief Initialize channel selection to default (channels 3 & 4, like iES v0.3)
 */
inline void channel_select_init_default(channel_select_config_t* config) {
    config->num_channels_to_stream = 2;
    config->channel_indices[0] = 2;  // Channel 3 (0-based index 2)
    config->channel_indices[1] = 3;  // Channel 4 (0-based index 3)
}

/**
 * @brief Initialize channel selection to transmit all channels
 */
inline void channel_select_init_all(channel_select_config_t* config) {
    config->num_channels_to_stream = 8;
    for (uint8_t i = 0; i < 8; i++) {
        config->channel_indices[i] = i;
    }
}

/**
 * @brief Initialize channel selection to transmit first N channels
 */
inline void channel_select_init_first_n(channel_select_config_t* config, uint8_t n) {
    if (n > 8) n = 8;
    config->num_channels_to_stream = n;
    for (uint8_t i = 0; i < n; i++) {
        config->channel_indices[i] = i;
    }
}

/**
 * @brief Custom channel selection
 * 
 * Example: To transmit channels 1, 2, and 5:
 *   uint8_t channels[] = {0, 1, 4};  // 0-based indices
 *   channel_select_custom(config, channels, 3);
 */
inline void channel_select_custom(channel_select_config_t* config, 
                                   const uint8_t* channels, uint8_t count) {
    if (count > 8) count = 8;
    config->num_channels_to_stream = count;
    for (uint8_t i = 0; i < count; i++) {
        config->channel_indices[i] = channels[i];
    }
}

/**
 * @brief Initialize channel selection from a 4-bit enable bitmask.
 *
 * Bit n (0-based) → channel n+1 (1-based ADS1299 convention).
 * Example: mask=0b00001100 → CH3 (bit2) and CH4 (bit3) → indices {2, 3}.
 *
 * @param config - [out] channel selection to populate
 * @param mask   - bitmask: bit0=CH1 … bit3=CH4 (ADS1299-4 has 4 channels)
 */
inline void ies_channel_select_init_from_mask(channel_select_config_t* config,
                                               uint8_t mask) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (mask & (1u << i)) {
            config->channel_indices[count++] = i;
        }
    }
    config->num_channels_to_stream = count;
}

/**
 * @brief Copy selected channels from full channel array to output buffer
 * 
 * @param full_data - array of all 8 channel values (int32_t[8])
 * @param output - output buffer for selected channels
 * @param config - channel selection configuration
 */
inline void channel_select_copy(const int32_t* full_data, 
                                 int32_t* output, 
                                 const channel_select_config_t* config) {
    for (uint8_t i = 0; i < config->num_channels_to_stream; i++) {
        output[i] = full_data[config->channel_indices[i]];
    }
}

#ifdef __cplusplus
}
#endif

#endif /* IES_CHANNEL_SELECT_H_ */
