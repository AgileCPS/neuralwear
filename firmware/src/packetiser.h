/**
 * @file    packetiser.h
 * @brief   PacketiserTask — serialises EEG samples, responses, and ML results
 *          to IES native wire format (WireFrame), one item per frame.
 *
 * ARCHITECTURE (firmware_architecture.md Section 6.3):
 *   Consumes three typed input queues with fixed priority:
 *     1. _responseQueue  — Response     (highest: immediate delivery)
 *     2. _eegQueue       — ADS1299_4_Sample
 *     3. _mlQueue        — MLOutput     (future)
 *   Produces WireFrame objects pushed to GatewayTask._dataQueue.
 *   One input item → one WireFrame; no combining.
 *
 * IES FRAME FORMAT (ies_message_protocol.md Section 5.1):
 *   [0xA0 start][frame_count 1B][type_ch 1B: (type<<4)|num_ch]
 *   [ch_data N×3B][0xC0 stop]
 *
 *   Type nibbles (iES types 0–5 reserved; Nicla-only types use 6–8):
 *     0 = EEG        [A0][cnt][0x0N][ch0 3B]...[chN 3B][C0]  (N=num channels)
 *     6 = RESPONSE   [A0][cnt][0x6N][cmd_id][status][len][payload...][C0]  ≤ 15 B
 *     7 = TIME_SYNC  [A0][cnt][0x71][ts_us 4B BE][sample_cnt 4B BE][C0]  = 12 B
 *     8 = ML_OUTPUT  [A0][cnt][0x81][label][confidence 4B][C0]           =  9 B (future)
 *
 * PRIORITY: osPriorityAboveNormal (+1)
 */

#pragma once

#include "mbed.h"
#include "task.h"
#include "eeg.h"
#include "cmd.h"
#include "config.h"
#include "runtime_state.h"
#include "iesUtilities/ies_channel_select.h"
#include "iesUtilities/ies_packet_format.h"
#include <cstdint>


// =============================================================================
// FrameDest — unicast routing target (firmware-internal, never OTA)
// =============================================================================

/**
 * @brief Which channel task should transmit this WireFrame.
 *
 * UART and BLE are never active simultaneously — exactly one dest per frame.
 */
enum class FrameDest : uint8_t {
    UART = 0,
    BLE  = 1,
};

// =============================================================================
// WireFrame — pre-serialised IES wire frame
// =============================================================================

/**
 * @brief Ready-to-transmit IES wire frame.
 *
 * Produced by PacketiserTask; consumed by channel tasks via
 * Serial.write(frame.bytes, frame.len) — no format knowledge required.
 */
struct WireFrame {
    uint8_t    len;                       ///< Valid bytes in bytes[]
    FrameDest  dest;                      ///< Unicast routing target (internal only)
    uint8_t    bytes[IES_MAX_FRAME_SIZE]; ///< IES-format frame bytes
};
static_assert(sizeof(WireFrame) == 2 + IES_MAX_FRAME_SIZE,
    "WireFrame size mismatch — update queue RAM estimates in config.h");


// =============================================================================
// MLOutput — ML inference result (future, Q8)
// =============================================================================

/**
 * @brief ML processor output (placeholder — structure finalised in Q8).
 */
struct MLOutput {
    uint8_t  class_label;   ///< Classification result
    float    confidence;    ///< Confidence score [0.0, 1.0]
};
// sizeof == 8 B (1 B + 3 B padding + 4 B float)


// =============================================================================
// IES frame delimiters
// =============================================================================

#define IES_FRAME_START  0xA0
#define IES_FRAME_STOP   0xC0


// =============================================================================
// PacketiserTask — serialises inputs to IES WireFrames
// =============================================================================

/**
 * @brief Serialises EEG samples, responses, and ML results to IES WireFrames.
 *
 * OPERATION:
 *   Each run() iteration:
 *     1. If TIME_SYNC due: serialise and distribute a TIME_SYNC frame
 *        (suppressed in HostProtocolMode::LEGACY_IES — see packetiser.cpp)
 *     2. Pop _responseQueue (highest priority) — serialise and dispatch
 *        (suppressed in LEGACY_IES except the CMD_SET_HOST_MODE ack itself)
 *     3. Pop _eegQueue — serialise and dispatch
 *     4. Pop _mlQueue (future) — serialise and dispatch
 *     5. sleepUntilNotified() if no data was available
 *
 * THREAD SAFETY:
 *   All queues are thread-safe (IQueue uses rtos::Mutex).
 *   Frame counter (_frameCnt) is private to this task; no external access.
 */
class PacketiserTask : public ProducerTask<WireFrame> {
private:
    // Input queues (owned by this task — all three call notify() on push)
    FifoQueue<ADS1299_4_Sample, FIFO_DEPTH_STREAMING>  _eegQueue;
    FifoQueue<Response, FIFO_DEPTH_RESPONSE>           _responseQueue;
    FifoQueue<MLOutput, FIFO_DEPTH_ML_QUEUE>           _mlQueue;  // future

    // Per-stream frame counter (IES byte 1 — wraps at 255)
    uint8_t   _frameCnt;

    // TIME_SYNC state
    uint32_t  _lastSyncTime_ms;

    // Downsampling counter: incremented each EEG sample; frame emitted only
    // when _sampleCount % getDownsamplingFactor() == 0.
    uint32_t  _sampleCount;

    // IES serialisation helpers — each produces one WireFrame
    WireFrame serialiseEeg(const ADS1299_4_Sample& sample);
    WireFrame serialiseResponse(const Response& resp);
    WireFrame serialiseTimeSync(uint32_t ts_us, uint32_t sample_cnt);
    WireFrame serialiseMl(const MLOutput& ml);       // future

protected:
    void run() override;

public:
    PacketiserTask();

    // Queue accessors — wired during setup() before start()
    IQueue<ADS1299_4_Sample>* getEegQueue()      { return &_eegQueue; }
    IQueue<Response>*         getResponseQueue() { return &_responseQueue; }
    IQueue<MLOutput>*         getMlQueue()       { return &_mlQueue; }
};


// -----------------------------------------------------------------------------
// Global instance
// -----------------------------------------------------------------------------

extern PacketiserTask packetiserTask;
