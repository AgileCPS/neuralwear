/**
 * @file    runtime_state.cpp
 * @brief   Implementation of runtime configuration state.
 */

#include "runtime_state.h"
#include "ADS1299_Library.h"    // needed for applyToHardware()
#include "ADS1299_Definitions.h"

// Thread-safety macros
#ifdef MBED_ENABLED
    #define LOCK_MUTEX()   _mutex.lock()
    #define UNLOCK_MUTEX() _mutex.unlock()
#else
    #define LOCK_MUTEX()
    #define UNLOCK_MUTEX()
#endif

// =============================================================================
// Global Instance
// =============================================================================

RuntimeState g_runtimeState;

// =============================================================================
// Initialization
// =============================================================================

void RuntimeState::initialize() {
    LOCK_MUTEX();

    // Gain ×1 and normal input mux for all channels
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        _channelGain[i] = ADS_GAIN01;
        _inputType[i]   = ADSINPUT_NORMAL;
    }

    _sampleRate           = (uint8_t)ADS1299_Library::SAMPLE_RATE_1000;  // 1 kSPS — iES default
    _outputMode           = OutputMode::UV; // µV integers — iES/OpenVIBE compatible
    _channelEnableMask    = 0b00001100;      // CH3+CH4 only (bit2+bit3)
    _streamingEnabled     = false;
    _downsamplingFactor   = 4;               // 1000 SPS ÷ 4 = 250 SPS over-the-wire
    _impedanceCheckEnabled = false;
    _hostProtocolMode     = HostProtocolMode::OPENVIBE;  // legacy is opt-in only

    UNLOCK_MUTEX();
}

// =============================================================================
// ADS1299 Channel Configuration
// =============================================================================

bool RuntimeState::setChannelGain(uint8_t channel, uint8_t gain) {
    if (channel < 1 || channel > NUM_CHANNELS) return false;
    // Valid ADS1299 gain codes: ADS_GAIN01(0)..ADS_GAIN24(6)
    if (gain > ADS_GAIN24) return false;
    LOCK_MUTEX();
    _channelGain[channel - 1] = gain;
    UNLOCK_MUTEX();
    return true;
}

uint8_t RuntimeState::getChannelGain(uint8_t channel) const {
    if (channel < 1 || channel > NUM_CHANNELS) return ADS_GAIN01;
    LOCK_MUTEX();
    uint8_t result = _channelGain[channel - 1];
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Input Multiplexer Type
// =============================================================================

void RuntimeState::setChannelInputType(uint8_t channel, uint8_t inputType) {
    if (channel < 1 || channel > NUM_CHANNELS) return;
    LOCK_MUTEX();
    _inputType[channel - 1] = inputType;
    UNLOCK_MUTEX();
}

uint8_t RuntimeState::getChannelInputType(uint8_t channel) const {
    if (channel < 1 || channel > NUM_CHANNELS) return ADSINPUT_NORMAL;
    LOCK_MUTEX();
    uint8_t result = _inputType[channel - 1];
    UNLOCK_MUTEX();
    return result;
}

void RuntimeState::setAllChannelInputTypes(uint8_t inputType) {
    LOCK_MUTEX();
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        _inputType[i] = inputType;
    }
    UNLOCK_MUTEX();
}

// =============================================================================
// Output Mode
// =============================================================================

void RuntimeState::setOutputMode(OutputMode mode) {
    LOCK_MUTEX();
    _outputMode = mode;
    UNLOCK_MUTEX();
}

OutputMode RuntimeState::getOutputMode() const {
    LOCK_MUTEX();
    OutputMode result = _outputMode;
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Channel Enable Bitmask
// =============================================================================

void RuntimeState::setChannelEnableMask(uint8_t mask) {
    LOCK_MUTEX();
    _channelEnableMask = mask & 0x0F;  // only 4 channels on ADS1299-4
    UNLOCK_MUTEX();
}

uint8_t RuntimeState::getChannelEnableMask() const {
    LOCK_MUTEX();
    uint8_t result = _channelEnableMask;
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Streaming Control (IES_STREAM_START/STOP)
// =============================================================================

void RuntimeState::setStreamingEnabled(bool enable) {
    LOCK_MUTEX();
    _streamingEnabled = enable;
    UNLOCK_MUTEX();
}

bool RuntimeState::isStreamingEnabled() const {
    LOCK_MUTEX();
    bool result = _streamingEnabled;
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Downsampling (IES_BTSPP_DOWNSAMPLING)
// =============================================================================

void RuntimeState::setDownsamplingFactor(uint8_t factor) {
    if (factor == 0) factor = 1;  // 0 is invalid — treat as ÷1 (no downsampling)
    LOCK_MUTEX();
    _downsamplingFactor = factor;
    UNLOCK_MUTEX();
}

uint8_t RuntimeState::getDownsamplingFactor() const {
    LOCK_MUTEX();
    uint8_t result = _downsamplingFactor;
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Sample Rate Configuration
// =============================================================================

void RuntimeState::setSampleRate(uint8_t rate) {
    LOCK_MUTEX();
    _sampleRate = rate;
    UNLOCK_MUTEX();
}

uint8_t RuntimeState::getSampleRate() const {
    LOCK_MUTEX();
    uint8_t result = _sampleRate;
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Impedance Check (IES_IMPEDANCE_CHECK_ON/OFF)
// =============================================================================

void RuntimeState::setImpedanceCheckEnabled(bool enable) {
    LOCK_MUTEX();
    _impedanceCheckEnabled = enable;
    UNLOCK_MUTEX();
}

bool RuntimeState::isImpedanceCheckEnabled() const {
    LOCK_MUTEX();
    bool result = _impedanceCheckEnabled;
    UNLOCK_MUTEX();
    return result;
}

// =============================================================================
// Host Protocol Mode (CMD_SET_HOST_MODE)
// =============================================================================

void RuntimeState::setHostProtocolMode(HostProtocolMode mode) {
    LOCK_MUTEX();
    _hostProtocolMode = mode;
    UNLOCK_MUTEX();
}

HostProtocolMode RuntimeState::getHostProtocolMode() const {
    LOCK_MUTEX();
    HostProtocolMode result = _hostProtocolMode;
    UNLOCK_MUTEX();
    return result;
}

bool RuntimeState::isOpenVibeMode() const {
    return getHostProtocolMode() == HostProtocolMode::OPENVIBE;
}

// =============================================================================
// Apply to Hardware
// =============================================================================

void RuntimeState::applyToHardware(ADS1299_Library* ads) {
    if (!ads) return;

    LOCK_MUTEX();
    // Snapshot values while holding the lock, then release before doing SPI
    uint8_t mask = _channelEnableMask;
    uint8_t gain[NUM_CHANNELS];
    uint8_t inputType[NUM_CHANNELS];
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        gain[i]      = _channelGain[i];
        inputType[i] = _inputType[i];
    }
    UNLOCK_MUTEX();

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        ads->channelSettings[i][POWER_DOWN]     = ((mask >> i) & 0x01) ? NO : YES;
        ads->channelSettings[i][GAIN_SET]       = gain[i];
        ads->channelSettings[i][INPUT_TYPE_SET] = inputType[i];
    }
    ads->writeChannelSettings();
}
