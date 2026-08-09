/*
 * ies_misc.h
 *
 *  Created on: Oct 15, 2018
 *      Author: nhatpham
 *  Ported to: Arduino Nicla Voice, April 2026
 *
 * Binary data compression utilities for efficient biosignal transmission.
 * Key feature: 24-bit packing reduces bandwidth by 25% vs 32-bit values.
 */

#ifndef IES_MISC_H_
#define IES_MISC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Buffer-to-integer conversion functions (big-endian / MSB first)
 */
uint16_t buffer_to_uint16(uint8_t* buffer); // MSB first (big-endian)
uint32_t buffer_to_uint24(uint8_t* buffer); // MSB first (big-endian)
uint32_t buffer_to_uint32(uint8_t* buffer); // MSB first (big-endian)
uint64_t buffer_to_uint64(uint8_t* buffer); // MSB first (big-endian)

/**
 * @brief Integer-to-buffer serialization functions (big-endian / MSB first)
 */
void uint16_to_buffer(uint16_t data, uint8_t* buffer); // MSB first
void uint24_to_buffer(uint32_t data, uint8_t* buffer); // MSB first (24-bit compression)
void uint32_to_buffer(uint32_t data, uint8_t* buffer); // MSB first
void uint64_to_buffer(uint64_t data, uint8_t* buffer); // MSB first

/**
 * @brief Float encoding (4 bytes: uint16 integer part + uint16 fractional part)
 *        Fractional part represents .00 to .99 (2 decimal digits)
 */
float buffer_to_float(uint8_t* buffer);  // 4-byte custom encoding
void float_to_buffer(float data, uint8_t* buffer);  // 4-byte custom encoding

#ifdef __cplusplus
}
#endif

#endif /* IES_MISC_H_ */
