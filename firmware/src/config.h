/**
 * @file  config.h
 * @brief Centralized firmware configuration. All tunable parameters live here;
 *        no magic numbers in source files. Pin definitions → pinDef.h.
 */

#pragma once

#include "mbed.h"

// =============================================================================
// Firmware Version
// =============================================================================
// Format: vMAJOR.MINOR.PATCH
//   PATCH  — increment on minor bug fixes / small corrections
//   MINOR  — increment on significant bug fixes or completion of an impl. phase
//   MAJOR  — increment on major architecture changes or production readiness
//
// Returned by CMD_GET_VERSION (0x32) as 3 payload bytes [major, minor, patch].

#define FW_VERSION_MAJOR   0
#define FW_VERSION_MINOR   2
#define FW_VERSION_PATCH   0
#define FW_VERSION_STR     "v0.2.0"

// #define DEBUG_ENABLE         // Uncomment: route all ADS1299 channels to internal test signal + enable debug logging.
//                               // DEBUG_ENABLE undefined = production mode: only CH3+CH4 active, CH1+CH2 powered down.


// =============================================================================
// 1. FIFO Queue Depths
// =============================================================================

// QueueStatus reports NEAR_FULL when fill% >= FIFO_NEAR_FULL_PCT.
#define FIFO_NEAR_FULL_PCT        75

// FifoQueue calls notify() on the owner task after each push() when fill% >= this.
// 0 = wake on every push (lowest latency, recommended for streaming).
// >0 = batch wake-ups (reduces context switches for bulk producers).
#define TASK_WAKE_THRESHOLD_PCT    0

// ── EEG sample queues  (ADS1299_4_Sample = 24 B each) ─────────────────────
// Buffering time = depth / SPS.  At 1000 SPS, depth=64 → 64 ms of headroom.
// Increase if the consumer task misses its scheduling slot under heavy load.
#define FIFO_DEPTH_STREAMING      64   // EEG → PacketiserTask  (64 × 24 B = 1536 B)
#define FIFO_DEPTH_EEG_BLE        64   // EEG → BLE task   (future)
#define FIFO_DEPTH_EEG_ML         32   // EEG → ML task    (future)
#define FIFO_DEPTH_EEG_LOG        16   // EEG → log task   (future)

// ── Command queues ─────────────────────────────────────────────────────────
// Commands are rare (human-initiated); 8 slots is always sufficient.
#define FIFO_DEPTH_CMD             8   // channels → Gateway → CmdHandler
#define IES_CMD_PAYLOAD_MAX       12   // max bytes in Command/Response payload (QUERY_STATUS needs 10)

// ── WireFrame queues ───────────────────────────────────────────────────────
// WireFrame = 1 B len + 1 B dest + IES_MAX_FRAME_SIZE payload = 22 B per slot.
#define IES_MAX_FRAME_SIZE        20   // max IES frame body bytes (4-ch EEG = 16 B)
#define FIFO_DEPTH_GATEWAY_DATA   128  // PacketiserTask → Gateway input queue
#define UART_TX_QUEUE_SIZE        64   // Gateway → UART channel TX queue
#define BLE_TX_QUEUE_SIZE        128   // Gateway → BLE channel TX queue

// ── Other queues ───────────────────────────────────────────────────────────
#define FIFO_DEPTH_ML_QUEUE       16   // MLProcessor → PacketiserTask  (future)
#define FIFO_DEPTH_RESPONSE        8   // CmdHandler → PacketiserTask response queue


// =============================================================================
// 2. Task Priorities
// =============================================================================
// Mbed OS: Idle=-3  Low=-2  BelowNormal=-1  Normal=0  AboveNormal=+1  Realtime=+3
//
// Priority rules:
//  • EEG acquisition is Realtime — DRDY fires every 1 ms at 1 kSPS and must
//    not be delayed by any other task.
//  • PacketiserTask is AboveNormal so it drains the EEG queue before Gateway
//    can starve it.  Gateway/UART/CmdHandler share Normal.
//  • Never set two tasks to the same priority if one produces for the other —
//    the consumer must be >= producer priority to keep queues drained.

#define TASK_PRIORITY_ACQUISITION    osPriorityRealtime     // EEG ISR-driven, must be highest
#define TASK_PRIORITY_BLE            osPriorityNormal       // future
#define TASK_PRIORITY_ML             osPriorityAboveNormal  // future
#define TASK_PRIORITY_LOG            osPriorityLow          // future
#define TASK_PRIORITY_COMMAND        osPriorityNormal       // future


// =============================================================================
// 3. Task Stack Sizes (bytes)
// =============================================================================
// Formula: stack = Σ(activation frames on deepest call path)
//                + RTX5 context save (~168 B) + ISR frame (~104 B) + 50% margin.
// Key contributors:
//   Serial.print(float) via USB CDC  → ~400 B (dominates most tasks with DEBUG_ENABLE)
//   SPI call chain (nrfx_spim)       → ~272 B (dominates EegAcquisitionTask)
//   osMutexAcquire / RTX5 scheduler  → ~192 B (every queue push/pop)
// Undersizing causes silent stack overflow → hard fault or watchdog reset.

#define STACK_SIZE_ACQUISITION   4096  // SPI 272 B + ISR nesting headroom
#define STACK_SIZE_PACKETISER    2048  // snprintf(float) path dominates
#define STACK_SIZE_GATEWAY       2048
#define STACK_SIZE_UART          2048  // processTx/Rx + snprintf(float)
#define STACK_SIZE_CMD_HANDLER   2048  // may invoke ADS1299 SPI writes
#define STACK_SIZE_BLE           4096  // nRF52 SoftDevice internal stack ~1.5 KB (gattServer().write)
#define STACK_SIZE_BLE_EVENT     2048  // BLE EventQueue dispatch thread (no GATT writes)
#define STACK_SIZE_ML            2048  // future
#define STACK_SIZE_LOG           1024  // printf-only, no deep calls


// =============================================================================
// 4. Serial / USB-CDC Configuration
// =============================================================================
// Bandwidth guide:  throughput_Bps = SPS × bytes_per_sample
//   CSV  @ 1000 SPS ≈ 60 KB/s  →  230400 baud (28.8 KB/s) is marginal; use binary above ~500 SPS.
//   Binary @ 1000 SPS ≈ 18 KB/s  →  comfortable at 230400 baud.
//   USB FS ceiling ≈ 125 KB/s regardless of baud rate.

#define SERIAL_PORT_USB              1       // 1 = USB CDC (Serial)
#define SERIAL_PORT_HW               0       // 1 = hardware UART (Serial1)
// Baud is now runtime-switchable via CMD_SET_HOST_MODE (see runtime_state.h
// HostProtocolMode + uart_channel.h UartChannelTask::requestBaudChange()).
// SERIAL_BAUD_MODERN is used at boot unless a persisted LEGACY_IES mode
// (persistent_config.h host_protocol_mode) says otherwise.
#define SERIAL_BAUD_MODERN           460800   // 4×legacy — streaming headroom (default)
#define SERIAL_BAUD_OPENVIBE           115200   // OpenVIBE CDriveriES hardcodes CBR_115200
#define SERIAL_CONNECT_TIMEOUT_MS    5000    // max wait for host to open port in setup()
#define SERIAL_CONNECT_POLL_MS       10      // polling interval during that wait
#define SERIAL_WRITE_TIMEOUT_MS      100     // blocking-write deadline
#define SERIAL_BAUD_SWITCH_DELAY_MS  20      // settle time between Serial.end()/begin() on mode switch

// Back off briefly when USB CDC TX is full so overload reduces throughput
// instead of stalling the scheduler in a tight retry loop.
#define UART_BACKPRESSURE_SLEEP_MS   5


// =============================================================================
// 5. Data Stream Format
// =============================================================================

#define STREAM_FORMAT_CSV     0   // human-readable, easy to plot
#define STREAM_FORMAT_BINARY  1   // compact, needed above ~500 SPS
#define STREAM_FORMAT_JSON    2   // too verbose, avoid

#define STREAM_FORMAT  STREAM_FORMAT_CSV

// CSV options (active when STREAM_FORMAT == STREAM_FORMAT_CSV)
#define CSV_INCLUDE_HEADER        1    // emit "# timestamp_ms, ch1, ..." once on connect
#define CSV_FIELD_SEPARATOR       ','
#define CSV_INCLUDE_FRAME_COUNTER 1    // adds a monotonic counter column for drop detection

// Binary options (active when STREAM_FORMAT == STREAM_FORMAT_BINARY)
#define BINARY_PACKET_SYNC_BYTE   0xA5  // framing byte at start of each packet
#define BINARY_INCLUDE_CRC        1     // append CRC-8 for integrity checking


// =============================================================================
// 6. Debug Logging
// =============================================================================

// Per-task/subsystem enable bits — OR them together in DEBUG_DEFAULT_MASK.
// Disabled categories produce zero overhead when DEBUG_ENABLE = 1.
#define DEBUG_ADS1299_INIT    (1 << 0)  // register init sequence
#define DEBUG_ADS1299_SPI     (1 << 1)  // per-transaction SPI trace (very verbose)
#define DEBUG_FIFO_OVERFLOW   (1 << 2)  // log every drop event
#define DEBUG_TASK_TIMING     (1 << 3)  // loop rate / max loop ms per task
#define DEBUG_BLE             (1 << 4)  // future
#define DEBUG_ML              (1 << 5)  // future
#define DEBUG_COMMAND         (1 << 6)  // command receive / dispatch / result / CRC detail
#define DEBUG_UART_CHANNEL    (1 << 7)  // TX/RX stats and fault events
#define DEBUG_STACK_HEALTH    (1 << 8)  // heap free + stack watermark

#define DEBUG_DEFAULT_MASK    (DEBUG_ADS1299_INIT | DEBUG_FIFO_OVERFLOW | \
                               DEBUG_UART_CHANNEL | DEBUG_STACK_HEALTH | \
                               DEBUG_COMMAND)

// Compile-time shorthand: true only when both DEBUG_ENABLE is defined and the
// corresponding category bit is set in DEBUG_DEFAULT_MASK.  Each category gets
// its own macro so callers don't have to repeat the mask arithmetic.
//
//   #if DBG_CMD    ... command-handler trace logs ...  #endif
//   #if DBG_UART   ... UART channel trace logs ...     #endif
//
#if defined(DEBUG_ENABLE) && (DEBUG_DEFAULT_MASK & DEBUG_COMMAND)
  #define DBG_CMD  1
#else
  #define DBG_CMD  0
#endif

#if defined(DEBUG_ENABLE) && (DEBUG_DEFAULT_MASK & DEBUG_UART_CHANNEL)
  #define DBG_UART 1
#else
  #define DBG_UART 0
#endif

// Each task emits a StatBegin/StatEnd block every PERF_MONITOR_INTERVAL_MS.
#define PERF_MONITOR_ENABLE         1
#define PERF_MONITOR_INTERVAL_MS    1000

// Time offsets (ms) so per-task health prints are spread across the interval
// rather than all firing at the same millisecond (reduces mutex contention).
#define DBG_STAGGER_EEG_MS        0
#define DBG_STAGGER_GATEWAY_MS    1000
#define DBG_STAGGER_UART_MS       2000
#define DBG_STAGGER_CMDHDLR_MS    3000


// =============================================================================
// 7. Timing and Polling
// =============================================================================

#define HEARTBEAT_LED_INTERVAL_MS       500   // LED toggle period → 1 Hz blink
// Idle poll period for tasks that sleep when their queue is empty.
// Lower = more responsive but wastes CPU; 1 ms is a safe default.
#define STREAMING_TASK_POLL_INTERVAL_MS   1
#define WATCHDOG_TIMEOUT_MS            5000   // future
#define ADS1299_SPI_TIMEOUT_MS          100   // abort SPI transfer after this
#define BLE_CONNECTION_TIMEOUT_MS     10000   // future


// =============================================================================
// 8. System Limits
// =============================================================================

// Hard upper bounds enforced at subscribe() time; prevents unbounded vector growth.
#define MAX_SUBSCRIBERS_ACQUISITION   8   // max tasks receiving EEG samples
#define MAX_PRODUCERS_PACKETISER      8   // future: multiple sensor producers


// =============================================================================
// 9. BLE Configuration (future)
// =============================================================================

#define BLE_DEVICE_NAME             "NICLA_EEG"
// ATT MTU: 23 B default = 3 B header + 20 B payload. Negotiate higher for throughput.
#define BLE_MTU_SIZE                23
// Connection interval: shorter = lower latency + higher power; 20–40 ms is typical.
#define BLE_CONN_INTERVAL_MIN_MS    20
#define BLE_CONN_INTERVAL_MAX_MS    40
#define BLE_SLAVE_LATENCY            0    // 0 = respond to every connection event
#define BLE_CONN_TIMEOUT_MS        1000   // link considered lost after this
// Canonical UUIDs — see ble_channel_design.md §9.1.1 and ble_channel.h
#define BLE_SERVICE_UUID_EEG       "A9E07020-0001-4A58-B8C9-3F0DAB7E5C1D"
#define BLE_CHAR_UUID_DATA         "A9E07020-0002-4A58-B8C9-3F0DAB7E5C1D"
#define BLE_CHAR_UUID_COMMAND      "A9E07020-0003-4A58-B8C9-3F0DAB7E5C1D"


// =============================================================================
// 10. ML / NDP120 Configuration (future)
// =============================================================================

#define NDP120_FIRMWARE_PACKAGE  "eeg_model.synpkg"  // file on external flash
#define ML_INFERENCE_MODE         1    // 0 = every sample, 1 = sliding window
// Window and stride control the overlap: overlap% = 1 - (stride/window).
#define ML_WINDOW_SIZE           64    // samples per inference window  (64 ms at 1 kSPS)
#define ML_STRIDE_SIZE           32    // samples to advance per step   (50% overlap)


// =============================================================================
// 11. Feature Flags
// =============================================================================
// Set to 1 to compile in the corresponding subsystem; 0 excludes it entirely.

#define FEATURE_STREAMING_ENABLE   1   // USB-CDC data streaming
#define FEATURE_BLE_ENABLE         0
// Stage 2 sub-gate: 0 = BleChannelTask runs, no ble.init() (safe task bring-up).
// 1 = ble.init() from BleChannelTask::run() only — never from setup()/loop().
#define BLE_RADIO_INIT_ENABLE      0
#define FEATURE_ML_ENABLE          0
#define FEATURE_LOG_ENABLE         0
#define FEATURE_COMMAND_ENABLE     0
#define FEATURE_LOFF_ENABLE        0   // ADS1299 lead-off detection
#define FEATURE_IMPEDANCE_ENABLE   0


// =============================================================================
// 12. Compile-time RAM Budget Check
// =============================================================================

#define NRF52832_RAM_TOTAL    65536

#define RAM_USAGE_FIFOS   (FIFO_DEPTH_STREAMING * 24 + \
                           FIFO_DEPTH_EEG_BLE   * 24 + \
                           FIFO_DEPTH_EEG_ML    * 24 + \
                           FIFO_DEPTH_EEG_LOG   * 24 + \
                           FIFO_DEPTH_CMD       * (3 + IES_CMD_PAYLOAD_MAX))

#define RAM_USAGE_STACKS  (STACK_SIZE_ACQUISITION + STACK_SIZE_PACKETISER + \
                           STACK_SIZE_GATEWAY     + STACK_SIZE_UART       + \
                           STACK_SIZE_CMD_HANDLER + STACK_SIZE_BLE        + \
                           STACK_SIZE_BLE_EVENT   + STACK_SIZE_ML         + \
                           STACK_SIZE_LOG)

#define RAM_USAGE_ESTIMATED_TOTAL  (RAM_USAGE_FIFOS + RAM_USAGE_STACKS)

#if RAM_USAGE_ESTIMATED_TOTAL > (NRF52832_RAM_TOTAL * 4 / 5)
    #warning "Estimated RAM usage exceeds 80% of available RAM!"
#endif
