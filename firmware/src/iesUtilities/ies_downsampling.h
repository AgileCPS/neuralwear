/*
 * ies_downsampling.h
 *
 * Ported from iES_v0.3-master to Arduino Nicla Voice, April 2026
 *
 * Temporal downsampling/decimation for bandwidth reduction.
 * Allows full-rate SD card logging while transmitting reduced-rate data over BLE/UART.
 *
 * Example: With downsampling factor = 4, only 1 out of every 4 samples is transmitted.
 * - Original rate: 250 Hz × 8 channels × 3 bytes = 6000 bytes/sec
 * - Downsampled:    62.5 Hz × 8 channels × 3 bytes = 1500 bytes/sec (75% reduction)
 */

#ifndef IES_DOWNSAMPLING_H_
#define IES_DOWNSAMPLING_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Simple downsample counter for temporal decimation
 * 
 * Usage pattern:
 *   static uint32_t ds_counter = 0;
 *   
 *   while (streaming) {
 *       acquire_sample();
 *       
 *       if (should_transmit(ds_counter, downsampling_factor)) {
 *           transmit_sample();
 *       }
 *       
 *       ds_counter++;
 *   }
 */

/**
 * @brief Check if current sample should be transmitted based on downsampling factor
 * 
 * @param counter - monotonic sample counter (incremented each sample)
 * @param factor - downsampling factor (1=no downsampling, 2=every 2nd, 4=every 4th, etc.)
 * @return true if this sample should be transmitted, false otherwise
 */
inline bool should_transmit(uint32_t counter, uint8_t factor) {
    if (factor <= 1) return true;  // No downsampling
    return (counter % factor) == 0;
}

/**
 * @brief Calculate effective sample rate after downsampling
 * 
 * @param base_rate_hz - original sampling rate in Hz
 * @param factor - downsampling factor
 * @return effective rate in Hz after downsampling
 */
inline float get_downsampled_rate(float base_rate_hz, uint8_t factor) {
    if (factor <= 1) return base_rate_hz;
    return base_rate_hz / factor;
}

/**
 * @brief Calculate bandwidth reduction percentage
 * 
 * @param factor - downsampling factor
 * @return percentage of data eliminated (0-100)
 */
inline uint8_t get_reduction_percent(uint8_t factor) {
    if (factor <= 1) return 0;
    return (uint8_t)((factor - 1) * 100 / factor);
}

#ifdef __cplusplus
}
#endif

#endif /* IES_DOWNSAMPLING_H_ */
