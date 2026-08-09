/**
 * @file    persistent_config.cpp
 * @brief   PersistentConfig implementation — Flash load / save / reset.
 *
 * Platform: Arduino Nicla Voice (nRF52832)
 * Flash backend: Mbed OS FlashIAP (direct NOR flash access, no external library).
 * Config is stored in the last 4 KB sector of the 512 KB internal flash.
 *
 * Write strategy (wear protection):
 *   1. Read the full sector (4096 B) into a static RAM buffer.
 *   2. Compare byte-by-byte with the new layout (18 B).
 *   3. If nothing changed, return immediately — no flash operation.
 *   4. Otherwise: erase sector → program sector from RAM buffer.
 *
 * The static sector buffer (s_sectorBuf[4096]) is allocated at link time (BSS)
 * — no heap allocation in RTOS context. PersistentConfig is only called from
 * cmdHandlerTask (non-reentrant by design), so the buffer sharing is safe.
 *
 * See technical_notes.md NOTE-007 for platform constraints.
 */

#include "persistent_config.h"
#include "ADS1299_Library.h"
#include "ADS1299_Definitions.h"
#include "FlashIAP.h"   // Mbed OS core — no external library required
#include <stddef.h>
#include <string.h>


// =============================================================================
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection)
// =============================================================================

uint16_t PersistentConfig::computeCrc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}


// =============================================================================
// FlashIAP — module-level state
// =============================================================================

// Static sector buffer: 4096 bytes in BSS, no heap allocation.
// Safe because PersistentConfig is only called from cmdHandlerTask (single caller).
static uint8_t s_sectorBuf[4096];

// Mbed FlashIAP instance — init/deinit called per operation to release the
// peripheral while streaming (avoids holding flash during continuous SPI DMA).
static mbed::FlashIAP s_flash;

// Config sector base address — computed once on first use.
static uint32_t s_configAddr = 0;

/**
 * @brief Return the start address of the config sector (last sector in flash).
 *
 * Computed lazily; cached in s_configAddr after the first call.
 * Requires s_flash to be initialised by the caller.
 */
static uint32_t getConfigAddr() {
    if (s_configAddr == 0) {
        uint32_t flashEnd = s_flash.get_flash_start() + s_flash.get_flash_size();
        // Walk back from end of flash to find the last sector boundary
        uint32_t sectorSize = s_flash.get_sector_size(flashEnd - 1);
        s_configAddr = flashEnd - sectorSize;
    }
    return s_configAddr;
}


// =============================================================================
// Flash I/O helpers
// =============================================================================

void PersistentConfig::readLayout(EepromLayout& layout) {
    s_flash.init();
    s_flash.read(&layout, getConfigAddr() + PERSISTENT_CONFIG_EEPROM_ADDR,
                 sizeof(EepromLayout));
    s_flash.deinit();
}

void PersistentConfig::writeLayout(const EepromLayout& layout) {
    s_flash.init();

    uint32_t addr       = getConfigAddr();
    uint32_t sectorSize = s_flash.get_sector_size(addr);

    // 1. Read the full sector into the static buffer
    s_flash.read(s_sectorBuf, addr, sectorSize);

    // 2. Compare the config block against the new layout — skip if unchanged
    const uint8_t* src    = reinterpret_cast<const uint8_t*>(&layout);
    uint32_t       offset = PERSISTENT_CONFIG_EEPROM_ADDR;
    bool changed = (memcmp(s_sectorBuf + offset, src, sizeof(EepromLayout)) != 0);

    if (changed) {
        // 3. Patch the 18-byte config block inside the sector buffer
        memcpy(s_sectorBuf + offset, src, sizeof(EepromLayout));

        // 4. Erase sector, then program the full sector back
        s_flash.erase(addr, sectorSize);
        s_flash.program(s_sectorBuf, addr, sectorSize);
    }

    s_flash.deinit();
}


// =============================================================================
// Factory defaults
// =============================================================================

void PersistentConfig::populateDefaults(EepromLayout& layout) {
    layout.magic               = PERSISTENT_CONFIG_MAGIC;
    layout.schema_version      = PERSISTENT_CONFIG_SCHEMA_VERSION;
    layout.output_mode         = (uint8_t)OutputMode::IES;   // µV integers
    layout.sample_rate         = (uint8_t)ADS1299_Library::SAMPLE_RATE_1000;
    layout.downsampling_factor = 4;                           // 1 kSPS ÷ 4 = 250 SPS
    layout.channel_gain[0]     = ADS_GAIN01;
    layout.channel_gain[1]     = ADS_GAIN01;
    layout.channel_gain[2]     = ADS_GAIN01;
    layout.channel_gain[3]     = ADS_GAIN01;
    layout.channel_enable_mask = 0b00001100;  // CH3+CH4 only (iES default)
    layout.uart_enabled_at_boot = 1;
    layout.ble_enabled_at_boot  = 0;
    layout.host_protocol_mode  = (uint8_t)HostProtocolMode::MODERN;
    layout.crc16 = computeCrc16(
        reinterpret_cast<const uint8_t*>(&layout), offsetof(EepromLayout, crc16));
}


// =============================================================================
// State ↔ layout conversion
// =============================================================================

void PersistentConfig::applyToState(const EepromLayout& layout, RuntimeState& state) {
    state.setOutputMode(static_cast<OutputMode>(layout.output_mode));
    state.setSampleRate(layout.sample_rate);
    state.setDownsamplingFactor(layout.downsampling_factor);
    state.setChannelEnableMask(layout.channel_enable_mask);
    for (uint8_t i = 0; i < RuntimeState::NUM_CHANNELS; i++) {
        state.setChannelGain(i + 1, layout.channel_gain[i]);
    }
    state.setHostProtocolMode(static_cast<HostProtocolMode>(layout.host_protocol_mode));
}

void PersistentConfig::stateToLayout(const RuntimeState& state, EepromLayout& layout) {
    layout.magic               = PERSISTENT_CONFIG_MAGIC;
    layout.schema_version      = PERSISTENT_CONFIG_SCHEMA_VERSION;
    layout.output_mode         = (uint8_t)state.getOutputMode();
    layout.sample_rate         = state.getSampleRate();
    layout.downsampling_factor = state.getDownsamplingFactor();
    layout.channel_enable_mask = state.getChannelEnableMask();
    for (uint8_t i = 0; i < RuntimeState::NUM_CHANNELS; i++) {
        layout.channel_gain[i] = state.getChannelGain(i + 1);
    }
    // uart/ble_enabled_at_boot are not tracked in RuntimeState — preserve existing
    // values by reading from the sector buffer that writeLayout() already holds.
    // We pass an EepromLayout pointer so writeLayout can share the read.
    // For now: read here only if the caller hasn't passed cached data.
    // NOTE: This read uses a SEPARATE s_flash instance so it does init/deinit.
    // The extra init/deinit cost (~50 ms) is acceptable for an explicit save,
    // but NOT for auto-save on every SET_OUTPUT_MODE / SET_CHANNEL_MASK.
    // Those auto-saves have been removed; only explicit CMD_SAVE_CONFIG calls
    // PersistentConfig::save(), so this overhead occurs at most once per save.
    EepromLayout existing;
    readLayout(existing);
    if (existing.magic == PERSISTENT_CONFIG_MAGIC &&
        existing.schema_version == PERSISTENT_CONFIG_SCHEMA_VERSION) {
        layout.uart_enabled_at_boot = existing.uart_enabled_at_boot;
        layout.ble_enabled_at_boot  = existing.ble_enabled_at_boot;
    } else {
        layout.uart_enabled_at_boot = 1;
        layout.ble_enabled_at_boot  = 0;
    }
    layout.host_protocol_mode = (uint8_t)state.getHostProtocolMode();
    layout.crc16 = computeCrc16(
        reinterpret_cast<const uint8_t*>(&layout), offsetof(EepromLayout, crc16));
}


// =============================================================================
// Public API
// =============================================================================

bool PersistentConfig::load(RuntimeState& state) {
    state.initialize();  // always set safe defaults first

    EepromLayout layout;
    readLayout(layout);

    // Validate magic
    if (layout.magic != PERSISTENT_CONFIG_MAGIC) {
        reset(state);
        return false;
    }

    // Validate schema version
    if (layout.schema_version != PERSISTENT_CONFIG_SCHEMA_VERSION) {
        reset(state);
        return false;
    }

    // Validate CRC-16
    uint16_t computed = computeCrc16(
        reinterpret_cast<const uint8_t*>(&layout), offsetof(EepromLayout, crc16));
    if (computed != layout.crc16) {
        reset(state);
        return false;
    }

    applyToState(layout, state);
    return true;
}

void PersistentConfig::save(const RuntimeState& state) {
    EepromLayout layout;
    stateToLayout(state, layout);
    writeLayout(layout);
}

void PersistentConfig::reset(RuntimeState& state) {
    state.initialize();  // loads runtime defaults

    EepromLayout layout;
    populateDefaults(layout);
    writeLayout(layout);
}
