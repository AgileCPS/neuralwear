/*
 * ies_misc.cpp
 *
 *  Created on: Oct 15, 2018
 *      Author: nhatpham
 *  Ported to: Arduino Nicla Voice, April 2026
 *
 * Binary data compression utilities for efficient biosignal transmission.
 * All multi-byte integers use big-endian (MSB first) byte order.
 *
 * Key compression technique:
 * - ADS1299 produces 24-bit ADC values
 * - uint24_to_buffer() packs them into 3 bytes instead of 4
 * - Saves 25% bandwidth (critical for BLE/UART transmission)
 */

#include "ies_misc.h"

/*----------------------------------------------------------------------------*/
uint16_t buffer_to_uint16(uint8_t * buffer) {
  uint16_t ret_val;

  ret_val = (*(buffer + 1)) | (*(buffer) << 8);

  return ret_val;
}

/*----------------------------------------------------------------------------*/
uint32_t buffer_to_uint32(uint8_t * buffer) {
  uint32_t ret_val;

  ret_val = (*(buffer + 3)) | (*(buffer + 2) << 8) | (*(buffer + 1) << 16)
      | (*(buffer) << 24);

  return ret_val;
}

/*----------------------------------------------------------------------------*/
uint32_t buffer_to_uint24(uint8_t * buffer) {
  uint32_t ret_val;

  ret_val = (*(buffer + 2)) | (*(buffer + 1) << 8) | (*(buffer) << 16);

  return ret_val;
}

/*----------------------------------------------------------------------------*/
uint64_t buffer_to_uint64(uint8_t * buffer) {
  uint64_t ret_val = 0;

  for (int count = 0; count < 8; count++) {
    ret_val |= (uint64_t) *(buffer + count) << ((7 - count) * 8);
  }

  return ret_val;
}

/*----------------------------------------------------------------------------*/
void uint16_to_buffer(uint16_t data, uint8_t* buffer) {
  *buffer = (uint8_t) (data >> 8);
  *(++buffer) = (uint8_t) (data);
}

/*----------------------------------------------------------------------------*/
void uint32_to_buffer(uint32_t data, uint8_t* buffer) {
  *buffer = (uint8_t) (data >> 24);
  *(++buffer) = (uint8_t) (data >> 16);
  *(++buffer) = (uint8_t) (data >> 8);
  *(++buffer) = (uint8_t) (data);
}

/*----------------------------------------------------------------------------*/
/**
 * @brief Pack 32-bit value into 24 bits (3 bytes, big-endian)
 * 
 * This is the "brilliant compression" - reduces data size by 25%!
 * ADS1299 ADC values fit in 24 bits, so we can safely discard the MSB
 * when transmitting over bandwidth-constrained channels (BLE, UART).
 * 
 * @param data - 32-bit value (only lower 24 bits are transmitted)
 * @param buffer - pointer to output buffer (must have at least 3 bytes)
 */
void uint24_to_buffer(uint32_t data, uint8_t* buffer) {
  *buffer = (uint8_t) (data >> 16);
  *(++buffer) = (uint8_t) (data >> 8);
  *(++buffer) = (uint8_t) (data);
}

/*----------------------------------------------------------------------------*/
void uint64_to_buffer(uint64_t data, uint8_t* buffer) {
  *buffer = (uint8_t) (data >> 56);
  *(++buffer) = (uint8_t) (data >> 48);
  *(++buffer) = (uint8_t) (data >> 40);
  *(++buffer) = (uint8_t) (data >> 32);
  *(++buffer) = (uint8_t) (data >> 24);
  *(++buffer) = (uint8_t) (data >> 16);
  *(++buffer) = (uint8_t) (data >> 8);
  *(++buffer) = (uint8_t) (data);
}

/*----------------------------------------------------------------------------*/
/**
 * @brief Decode 4-byte custom float encoding
 * 
 * Format: [uint16 integer][uint16 fraction]
 * Fraction part represents 0-99 (for .00 to .99)
 * Example: 3.14 = [0x00, 0x03, 0x00, 0x0E]
 */
float buffer_to_float(uint8_t* buffer) {
  float ret_val;
  uint16_t dec;
  uint16_t frac;

  dec = buffer_to_uint16(buffer);
  buffer += 2;
  frac = buffer_to_uint16(buffer);

  ret_val = (float) dec;
  ret_val += ((float) frac) / 100;

  return ret_val;
}

/*----------------------------------------------------------------------------*/
/**
 * @brief Encode float into 4-byte custom format
 * 
 * Format: [uint16 integer][uint16 fraction]
 * Precision limited to 2 decimal places
 */
void float_to_buffer(float data, uint8_t* buffer) {
  uint16_t dec, frac;

  dec = (uint16_t) data;
  frac = ((uint16_t) (data * 100)) % 100;

  uint16_to_buffer(dec, buffer);
  buffer += 2;
  uint16_to_buffer(frac, buffer);
}
