/**
 * @file    runtime_state.h
 * @brief   Thread-safe runtime configuration for ADS1299 and streaming control.
 *
 * Tracks runtime-modifiable settings based on IES protocol commands:
 *   - ADS1299 channel configuration (active/inactive, gain per channel)
 *   - Output mode (iES µV integers vs raw ADC counts)
 *   - Channel enable bitmask (which channels to stream)
 *   - Streaming state (on/off per IES_STREAM_START/STOP commands)
 *   - Downsampling factor (per IES_BTSPP_DOWNSAMPLING command)
 *   - Impedance check mode (per IES_IMPEDANCE_CHECK_ON/OFF commands)
 *   - Sample rate configuration
 *
 * Thread Safety: All public methods protected by mutex when MBED_ENABLED.
 */

#pragma once

#include <cstdint>

#define MBED_ENABLED

#ifdef MBED_ENABLED
    #include "mbed.h"
#endif

#include "ADS1299_Definitions.h"

// Forward declaration to avoid circular includes; ADS1299_Library.h included in .cpp
class ADS1299_Library;


// =============================================================================
// OutputMode — selects the EEG data format sent over the wire
// =============================================================================

/**
 * @brief EEG output format sent by PacketiserTask.
 *
 * UV  (1): integer µV values (default, iES/OpenVIBE compatible).
 * RAW  (0): raw 24-bit ADC counts, pass-through (OpenBCI compatible).
 */
enum class OutputMode : uint8_t {
    RAW = 0,  ///< Raw ADC counts (OpenBCI compatible)
    UV = 1   ///< Integer µV (iES/OpenVIBE compatible, default)
};


// =============================================================================
// HostProtocolMode — selects the wire-format contract for connected host
// =============================================================================

/**
 * @brief Host protocol contract enforced by CommandHandlerTask/PacketiserTask.
 *
 * MODERN     (0): current Nicla behavior — RESPONSE (type 6) and TIME_SYNC
 *                 (type 7) frames are emitted; 't' command expects a 5-byte
 *                 payload (4-byte epoch + 1-byte CRC-8); baud 460800.
 * OPENVIBE (1): byte-for-byte compatible with the original OpenVIBE
 *                 `CDriveriES` driver — only type-0 EEG frames are emitted,
 *                 no command acks, 't' expects a 4-byte payload (no CRC);
 *                 baud 115200 (OpenVIBE hardcodes CBR_115200).
 */
enum class HostProtocolMode : uint8_t {
    MODERN     = 0,
    OPENVIBE = 1
};


// =============================================================================
// RuntimeState Class
// =============================================================================

/**
 * @brief Runtime configuration state for ADS1299 and streaming control.
 */
class RuntimeState {
public:
    static constexpr uint8_t NUM_CHANNELS = 4;  ///< ADS1299-4 has 4 channels

    // Initialization
    void initialize();

    // ADS1299 Channel Configuration
    bool setChannelGain(uint8_t channel, uint8_t gain);
    uint8_t getChannelGain(uint8_t channel) const;

    // Input multiplexer type per channel (ADSINPUT_NORMAL, ADSINPUT_TESTSIG, …)
    // Default: ADSINPUT_NORMAL. Persisted through applyToHardware().
    void    setChannelInputType(uint8_t channel, uint8_t inputType);
    uint8_t getChannelInputType(uint8_t channel) const;
    void    setAllChannelInputTypes(uint8_t inputType);  ///< Convenience: set all channels at once.

    // Output Mode (iES µV vs raw ADC)
    void setOutputMode(OutputMode mode);
    OutputMode getOutputMode() const;

    // Channel Enable Bitmask (bit0=CH1 … bit3=CH4)
    void setChannelEnableMask(uint8_t mask);
    uint8_t getChannelEnableMask() const;

    // Streaming Control (IES_STREAM_START/STOP commands)
    void setStreamingEnabled(bool enable);
    bool isStreamingEnabled() const;

    // Downsampling (IES_BTSPP_DOWNSAMPLING command)
    void setDownsamplingFactor(uint8_t factor);
    uint8_t getDownsamplingFactor() const;

    // Sample Rate Configuration
    void setSampleRate(uint8_t rate);
    uint8_t getSampleRate() const;

    // Impedance Check (IES_IMPEDANCE_CHECK_ON/OFF commands)
    void setImpedanceCheckEnabled(bool enable);
    bool isImpedanceCheckEnabled() const;

    // Host Protocol Mode (CMD_SET_HOST_MODE) — Modern vs Legacy/OpenVIBE
    void setHostProtocolMode(HostProtocolMode mode);
    HostProtocolMode getHostProtocolMode() const;
    bool isOpenVibeMode() const;  ///< Convenience: getHostProtocolMode() == LEGACY_IES

    /**
     * @brief Apply current runtime state to the ADS1299 hardware registers.
     *
     * Writes channel power-down bits and gain settings for every channel.
     * Call once after PersistentConfig::load() and after each channel/gain change.
     */
    void applyToHardware(ADS1299_Library* ads);

private:
    // ADS1299 Channel State
    uint8_t _channelGain[NUM_CHANNELS];
    uint8_t _inputType[NUM_CHANNELS];   ///< ADSINPUTxx mux code per channel
    uint8_t _sampleRate;  // ADS1299 sample rate code

    // Output format and channel selection
    OutputMode _outputMode;
    uint8_t    _channelEnableMask;  // bit0=CH1 … bit3=CH4

    // Streaming State
    bool    _streamingEnabled;
    uint8_t _downsamplingFactor;
    bool    _impedanceCheckEnabled;

    // Host protocol contract (Modern vs Legacy/OpenVIBE)
    HostProtocolMode _hostProtocolMode;

    // Thread Safety (MbedOS only)
#ifdef MBED_ENABLED
    mutable rtos::Mutex _mutex;
#endif
};

// =============================================================================
// Global Instance
// =============================================================================

extern RuntimeState g_runtimeState;
