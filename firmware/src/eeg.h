/**
 * @file    eeg.h
 * @brief   EEG sample data type and FIFO queue type aliases for the ADS1299-4.
 *
 * Defines ADS1299_4_Sample - the EEG data unit produced by the ADS1299-4 driver -
 * and provides type aliases for typed queue instances:
 *
 *   IEegQueue  - capacity-independent interface alias (IQueue<ADS1299_4_Sample>).
 *   EegFifo<N> - concrete alias (FifoQueue<ADS1299_4_Sample, N>).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "fifo_queue.h"
#include "ADS1299_Library.h"  // For SAMPLE_RATE enum and ADS1299_Library class


// -----------------------------------------------------------------------------
// ADS1299_4_Sample  - fundamental EEG data unit (ADS1299-4, 4 channels)
// -----------------------------------------------------------------------------

/**
 * @brief One complete ADS1299-4 sample: the payload type for all EEG queues.
 *
 * The ADS1299-4 is the 4-channel variant of the ADS1299 family.
 * Each sample captures all 4 channels in a single DRDY-triggered SPI read.
 *
 * Fields:
 *
 *   channel[4]
 *       Signed 24-bit ADC values for CH1-CH4, sign-extended to int32_t.
 *       Convert to uV: uV = (float)channel[i] * EEG_SCALE_UV.
 *
 *   sample_number
 *       Monotonically increasing sample counter (uint32_t).
 *       Incremented by producer for every sample.
 *       Used for:
 *         - Gap detection: if sample_number != last + 1 then gap detected
 *         - Timestamp reconstruction: timestamp = sync_time + (sample_number * period)
 *       Rolls over at 4,294,967,296 (approx 198 days at 250 SPS, approx 74 hours at 16 kSPS).
 *
 * TIMING STRATEGY:
 *   No per-sample timestamp. Host reconstructs timestamps using periodic TIME_SYNC
 *   frames (sent by PacketiserTask every ~1 second) containing:
 *     - uint32_t timestamp_us (microseconds since boot)
 *     - uint32_t sample_number (current counter value)
 *
 *   This reduces data overhead from 64 KB/s (at 16 kSPS with timestamps) to
 *   ~10 bytes/s for sync packets - a 99.98% reduction.
 *
 * Memory layout (Cortex-M4, natural alignment):
 *   Offset  0: channel[0-3] (16 B, 4 * int32_t)
 *   Offset 16: sample_number (4 B, uint32_t)
 *   sizeof == 20 bytes (no padding).
 */
struct ADS1299_4_Sample {
    int32_t   channel[4];      ///< CH1..CH4 (ADS1299-4), sign-extended 24-bit ADC values
    uint32_t  sample_number;   ///< Monotonic counter for gap detection & timestamp reconstruction
};
static_assert(sizeof(ADS1299_4_Sample) == 20,
    "ADS1299_4_Sample size mismatch - check field types and compiler alignment.");


// -----------------------------------------------------------------------------
// EEG_SCALE_UV  - ADC LSB to microvolts conversion factor
// -----------------------------------------------------------------------------

/**
 * @brief Scale factor: ADS1299-4 raw LSB to microvolts (uV).
 *
 * Derivation (gain = 1, Vref = 4.5 V, 24-bit two's complement):
 *   EEG_SCALE_UV = 4 500 000 uV / (2^23 - 1) approx 0.5364 uV/LSB
 */
constexpr float EEG_SCALE_UV = 4500000.0f / 8388607.0f;  // approx 0.5364418669 uV/LSB


// -----------------------------------------------------------------------------
// Type aliases
// -----------------------------------------------------------------------------

/** @brief Capacity-independent interface alias for ADS1299_4_Sample queues. */
using IEegQueue = IQueue<ADS1299_4_Sample>;

/** @brief Concrete EEG FIFO parameterised by depth N. */
template<size_t N>
using EegFifo = FifoQueue<ADS1299_4_Sample, N>;


// -----------------------------------------------------------------------------
// SPI EEG data buffer
// -----------------------------------------------------------------------------
#include "config.h"

/**
 * @brief SPI EEG data buffer.
 *
 * Receives ADS1299-4 samples from the SPI acquisition loop.
 * Depth set by FIFO_DEPTH_EEG_BLE in config.h.
 */
extern EegFifo<FIFO_DEPTH_EEG_BLE> spiEegBuffer;


// =============================================================================
// EegAcquisitionTask - RTOS task for real-time EEG data acquisition
// =============================================================================

#include "task.h"

/**
 * @brief EEG data acquisition task for Arduino Nicla Voice (Mbed OS / nRF52832).
 *
 * ARCHITECTURE:
 *   This task implements the highest-priority data producer in the streaming
 *   architecture described in docs/firmware_architecture.md Section 6.2.
 *
 * OPERATION:
 *   1. Wait on semaphore (signaled by DRDY ISR when ADS1299 has new data)
 *   2. Read 4-channel sample from ADS1299 via SPI (updateChannelData())
 *   3. Increment sample counter
 *   4. Package into ADS1299_4_Sample struct
 *   5. Distribute to all subscribed consumer queues (fan-out via ProducerTask)
 *
 * PRIORITY:
 *   osPriorityRealtime (+3) - highest priority ensures minimal DRDY latency
 *
 * THREAD SAFETY:
 *   - Semaphore signaling from ISR context is safe (rtos::Semaphore is IRQ-safe)
 *   - ADS1299 SPI access is protected by SPI.beginTransaction/endTransaction
 *   - Distribution to FIFOs is thread-safe (IQueue uses rtos::Mutex internally)
 *
 * USAGE:
 * @code
 *   // In global scope:
 *   EegAcquisitionTask eegTask;
 *
 *   // In ISR:
 *   void DRDY_ISR() {
 *       eegTask.signalDataReady();
 *   }
 *
 *   // In setup():
 *   ads1299.begin();
 *   eegTask.subscribe(streamingTask.getQueue());
 *   attachInterrupt(ADS_DRDY_PIN, DRDY_ISR, FALLING);
 *   eegTask.start();
 * @endcode
 */
class EegAcquisitionTask : public ProducerTask<ADS1299_4_Sample> {
private:
    ADS1299_Library& _ads;             ///< Reference to ADS1299 driver instance
    rtos::Semaphore  _drdySemaphore;   ///< Signaled by DRDY ISR
    uint32_t         _sampleCounter;   ///< Monotonic sample counter (global, for time sync)

protected:
    /**
     * @brief Main acquisition loop - runs in dedicated thread at osPriorityRealtime.
     *
     * Blocks on _drdySemaphore until DRDY ISR signals new data available.
     * Reads sample from ADS1299, packages into ADS1299_4_Sample, and distributes
     * to all subscribed consumers.
     */
    void run() override;

public:
    /**
     * @brief Construct the EEG acquisition task.
     *
     * Priority and stack size are taken from config.h:
     *   - TASK_PRIORITY_ACQUISITION (osPriorityRealtime)
     *   - STACK_SIZE_ACQUISITION (2048 bytes)
     */
    EegAcquisitionTask();

    /**
     * @brief Signal that new ADS1299 data is ready (called from DRDY ISR).
     *
     * Releases the semaphore to wake the acquisition thread.
     * This method is ISR-safe (rtos::Semaphore::release() is IRQ-safe).
     *
     * IMPORTANT: Attach this to the DRDY interrupt pin (active-low):
     * @code
     *   attachInterrupt(ADS_DRDY_PIN, []() { eegTask.signalDataReady(); }, FALLING);
     * @endcode
     */
    void signalDataReady();

    /**
     * @brief Set ADS1299 sampling rate.
     *
     * @param rate  Sample rate enum value (ADS1299_Library::SAMPLE_RATE_250, etc.)
     *
     * NOTE: This re-initializes the ADS1299, so call before starting streaming.
     */
    void setSampleRate(ADS1299_Library::SAMPLE_RATE rate);

    /**
     * @brief Get current sampling rate as string.
     *
     * @return Sample rate string (e.g., "250", "1000", "16000")
     */
    const char* getSampleRate() const;

    /**
     * @brief Get current monotonic sample counter.
     *
     * Read by PacketiserTask to populate TIME_SYNC frames.
     * The value is a snapshot; no lock needed — uint32_t reads are atomic on Cortex-M4.
     */
    uint32_t getSampleCounter() const { return _sampleCounter; }
};


// -----------------------------------------------------------------------------
// Global EEG acquisition task instance
// -----------------------------------------------------------------------------

/**
 * @brief Global EEG acquisition task instance.
 *
 * Declared here so it can be accessed from the DRDY ISR.
 */
extern EegAcquisitionTask eegAcquisitionTask;
