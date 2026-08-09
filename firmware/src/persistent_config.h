/**
 * @file    persistent_config.h
 * @brief   EEPROM-backed persistent configuration for ADS1299 firmware.
 *
 * Stores runtime settings that must survive a power cycle.
 * Layout (18 bytes, firmware_architecture.md §12.4):
 *
 *   Offset  Size  Field                    Default
 *   ──────  ────  ───────────────────────  ───────────────────────
 *    0       4    magic                    0xE1E50001
 *    4       1    schema_version           1
 *    5       1    output_mode              1 = iES µV (OutputMode::IES)
 *    6       1    sample_rate              ADS1299_Library::SAMPLE_RATE_1000
 *    7       1    downsampling_factor      4
 *    8       4    channel_gain[4]          ADS_GAIN01 × 4 channels
 *   12       1    channel_enable_mask      0b00001100 (CH3+CH4)
 *   13       1    uart_enabled_at_boot     1
 *   14       1    ble_enabled_at_boot      0
 *   15       1    host_protocol_mode       0 = MODERN (was "reserved", must be
 *                                          0x00 on all pre-existing units —
 *                                          see runtime_state.h HostProtocolMode)
 *   16       2    crc16 (over bytes 0–15)
 *
 * Platform: Arduino Nicla Voice, backed by Mbed OS FlashIAP (last 4 KB NOR
 *           flash sector; wear-protected via read-before-write; NOTE-007).
 */

#pragma once

#include <stdint.h>
#include "runtime_state.h"


// =============================================================================
// Constants
// =============================================================================

static constexpr uint32_t PERSISTENT_CONFIG_MAGIC          = 0xE1E50001;
static constexpr uint8_t  PERSISTENT_CONFIG_SCHEMA_VERSION = 1;
static constexpr uint16_t PERSISTENT_CONFIG_EEPROM_ADDR    = 0;  // byte offset in EEPROM


// =============================================================================
// EepromLayout  — raw 18-byte on-flash struct
// =============================================================================

/**
 * @brief On-flash layout of the persistent configuration block.
 *
 * Must remain exactly 18 bytes (schema v1). If fields are added, bump
 * schema_version and add a migration path in load().
 */
struct __attribute__((packed)) EepromLayout {
    uint32_t magic;                ///< 0xE1E50001 — validates the block is ours
    uint8_t  schema_version;       ///< Must equal PERSISTENT_CONFIG_SCHEMA_VERSION
    uint8_t  output_mode;          ///< OutputMode cast to uint8_t
    uint8_t  sample_rate;          ///< ADS1299_Library::SAMPLE_RATE cast to uint8_t
    uint8_t  downsampling_factor;  ///< 1–255; 0 treated as 1 at runtime
    uint8_t  channel_gain[4];      ///< ADS_GAINxx register code per channel
    uint8_t  channel_enable_mask;  ///< bit0=CH1 … bit3=CH4
    uint8_t  uart_enabled_at_boot; ///< 1 = UART enabled; 0 = disabled
    uint8_t  ble_enabled_at_boot;  ///< 1 = BLE enabled;  0 = disabled
    uint8_t  host_protocol_mode;   ///< HostProtocolMode cast to uint8_t (was "reserved";
                                   ///< 0x00 == MODERN, so pre-existing units migrate for free)
    uint16_t crc16;                ///< CRC-16/CCITT-FALSE over bytes [0..15]
};
static_assert(sizeof(EepromLayout) == 18, "EepromLayout must be exactly 18 bytes");


// =============================================================================
// PersistentConfig — static API
// =============================================================================

/**
 * @brief Static helper that reads/writes the EEPROM config block.
 *
 * All methods are static — there is exactly one config block.
 */
class PersistentConfig {
public:
    /**
     * @brief Load config from EEPROM into RuntimeState.
     *
     * Validates magic number, schema version, and CRC-16.
     * On any validation failure, calls reset() to restore defaults.
     *
     * @param state  RuntimeState to populate
     * @return true  if EEPROM was valid; false if defaults were loaded
     */
    static bool load(RuntimeState& state);

    /**
     * @brief Save current RuntimeState to EEPROM.
     *
     * Reads current EEPROM bytes first; only writes pages with changed bytes
     * (wear-levelling via read-before-write).
     *
     * @param state  RuntimeState to persist
     */
    static void save(const RuntimeState& state);

    /**
     * @brief Reset EEPROM to factory defaults and load defaults into state.
     *
     * @param state  RuntimeState to populate with defaults
     */
    static void reset(RuntimeState& state);

private:
    static void    populateDefaults(EepromLayout& layout);
    static uint16_t computeCrc16(const uint8_t* data, size_t len);
    static void    readLayout(EepromLayout& layout);
    static void    writeLayout(const EepromLayout& layout);
    static void    applyToState(const EepromLayout& layout, RuntimeState& state);
    static void    stateToLayout(const RuntimeState& state, EepromLayout& layout);
};
