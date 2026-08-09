/*
 * ies_packet_format.h
 *
 * Ported from iES_v0.3-master to Arduino Nicla Voice, April 2026
 *
 * Optimized packet header format with nibble packing.
 * The iES native format uses compact headers vs OpenBCI format.
 *
 * PACKET FORMAT COMPARISON:
 *
 * OpenBCI Compatible (fixed 33 bytes for 8 channels):
 *   [0xA0][counter][ch1-24bit][ch2][ch3][ch4][ch5][ch6][ch7][ch8][0xC0]
 *   = 1 + 1 + (8×3) + 1 = 27 bytes minimum, but buffer is 33 bytes (padding)
 *
 * iES Native Format (variable size based on actual channels):
 *   [0xA0][counter][type:4bit|num_ch:4bit][ch1-24bit][ch2]...[0xC0]
 *   = 1 + 1 + 1 + (N×3) + 1 = 4 + (N×3) bytes
 *   
 *   For 2 channels: 4 + 6 = 10 bytes (vs 33 bytes OpenBCI = 70% header reduction!)
 *   For 8 channels: 4 + 24 = 28 bytes (vs 33 bytes OpenBCI = 15% reduction)
 *
 * KEY OPTIMIZATION: Nibble packing in byte 3
 *   Upper 4 bits = sample_type (EEG, IMU, EDA, etc.)
 *   Lower 4 bits = num_of_channels (1-15 max, 0-15 range)
 *   This saves 1 byte vs separate fields!
 */

#ifndef IES_PACKET_FORMAT_H_
#define IES_PACKET_FORMAT_H_

#include <stdint.h>
#include "ies_misc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Packet delimiters
#define IES_PACKET_START  0xA0
#define IES_PACKET_END    0xC0

// Sample types (4 bits = 0-15 max)
typedef enum {
    IES_SAMPLE_EEG       = 0,
    IES_SAMPLE_IMPEDANCE = 1,
    IES_SAMPLE_NECK_IMU  = 2,
    IES_SAMPLE_EAR_IMU   = 3,
    IES_SAMPLE_EDA       = 4,
    IES_SAMPLE_BATT_INFO = 5
} ies_sample_type_t;

// Maximum channels per packet (limited by 4-bit field)
#define IES_MAX_CHANNELS_PER_PACKET  15

/**
 * @brief Build iES native format packet header
 * 
 * @param buffer - output buffer (must have space for entire packet)
 * @param sample_counter - frame counter (0-255, wraps around)
 * @param sample_type - type of sample (see ies_sample_type_t)
 * @param num_channels - number of channels in this packet (1-15)
 * @return number of header bytes written (always 3)
 */
inline uint8_t ies_packet_build_header(uint8_t* buffer, 
                                        uint8_t sample_counter,
                                        ies_sample_type_t sample_type,
                                        uint8_t num_channels) {
    buffer[0] = IES_PACKET_START;
    buffer[1] = sample_counter;
    buffer[2] = (sample_type << 4) | (num_channels & 0x0F);  // Nibble packing!
    return 3;
}

/**
 * @brief Add 24-bit channel data to packet
 * 
 * @param buffer - output buffer (positioned after header)
 * @param channel_data - array of channel values (int32_t)
 * @param num_channels - number of channels to write
 * @return number of bytes written (num_channels × 3)
 */
inline uint8_t ies_packet_add_channels(uint8_t* buffer,
                                        const int32_t* channel_data,
                                        uint8_t num_channels) {
    uint8_t offset = 0;
    for (uint8_t i = 0; i < num_channels; i++) {
        uint24_to_buffer(channel_data[i], &buffer[offset]);
        offset += 3;
    }
    return offset;
}

/**
 * @brief Add packet footer
 * 
 * @param buffer - output buffer (positioned after channel data)
 * @return number of bytes written (always 1)
 */
inline uint8_t ies_packet_add_footer(uint8_t* buffer) {
    buffer[0] = IES_PACKET_END;
    return 1;
}

/**
 * @brief Build complete iES packet
 * 
 * Complete packet assembly in one function.
 * 
 * @param buffer - output buffer (must be >= 4 + num_channels*3 bytes)
 * @param sample_counter - frame counter
 * @param sample_type - type of sample
 * @param channel_data - array of channel values
 * @param num_channels - number of channels
 * @return total packet size in bytes
 */
inline uint8_t ies_packet_build(uint8_t* buffer,
                                 uint8_t sample_counter,
                                 ies_sample_type_t sample_type,
                                 const int32_t* channel_data,
                                 uint8_t num_channels) {
    uint8_t offset = 0;
    
    // Header (3 bytes)
    offset += ies_packet_build_header(buffer, sample_counter, sample_type, num_channels);
    
    // Channel data (num_channels × 3 bytes)
    offset += ies_packet_add_channels(&buffer[offset], channel_data, num_channels);
    
    // Footer (1 byte)
    offset += ies_packet_add_footer(&buffer[offset]);
    
    return offset;  // Total packet size
}

/**
 * @brief Parse iES packet header
 * 
 * @param buffer - input buffer containing packet
 * @param sample_counter - [out] frame counter
 * @param sample_type - [out] sample type
 * @param num_channels - [out] number of channels
 * @return true if header is valid (starts with 0xA0), false otherwise
 */
inline bool ies_packet_parse_header(const uint8_t* buffer,
                                     uint8_t* sample_counter,
                                     ies_sample_type_t* sample_type,
                                     uint8_t* num_channels) {
    if (buffer[0] != IES_PACKET_START) {
        return false;  // Invalid start byte
    }
    
    *sample_counter = buffer[1];
    *sample_type = (ies_sample_type_t)(buffer[2] >> 4);  // Upper nibble
    *num_channels = buffer[2] & 0x0F;                     // Lower nibble
    
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* IES_PACKET_FORMAT_H_ */
