/**
 * @file    cmd_handler.h
 * @brief   Command handler task for processing control commands.
 *
 *   ARCHITECTURE (firmware_architecture.md Section 6.6):
 *   CommandHandlerTask implements command execution:
 *     - Consumes: Command (from GatewayTask)
 *     - Produces: Response (to PacketiserTask)
 *
 *   Responsibilities:
 *     - Parse and execute commands
 *     - Generate Response packets
 *     - Control system state (start/stop, ODR, gain, channel enable)
 *
 * PRIORITY: osPriorityNormal (0)
 */

#pragma once

#include "mbed.h"
#include "task.h"
#include "packetiser.h"
#include "cmd.h"
#include "config.h"


// =============================================================================
// Command IDs (iES v0.3 + Nicla extensions)
// =============================================================================

enum CommandId : uint8_t {
    // ── iES bare-byte commands ─────────────────────────────────────────────
    CMD_START_STREAMING   = 'b',   ///< Start EEG streaming       (no payload)
    CMD_STOP_STREAMING    = 's',   ///< Stop EEG streaming        (no payload)
    CMD_TIME_SYNC         = 't',   ///< Time sync (4B epoch BE + 1B CRC-8)
    CMD_HEARTBEAT         = '.',   ///< Keepalive ping            (no payload)
    CMD_UART_PRINT_SEL    = 'p',   ///< Log category / mode-switch (1B sub-cmd)
    CMD_DOWNSAMPLING      = 'd',   ///< Set downsampling factor    (1B ratio)
    CMD_IMPEDANCE_ON      = 'Z',   ///< Enable  impedance check   (no payload)
    CMD_IMPEDANCE_OFF     = 'z',   ///< Disable impedance check   (no payload)
    CMD_SOFT_RESET        = 'v',   ///< Soft reset / OpenBCI banner (no payload)

    // ── Nicla binary config commands ──────────────────────────────────────
    CMD_SET_ODR           = 0x10,  ///< Set ADS1299 output data rate  (1B)
    CMD_SET_GAIN          = 0x11,  ///< Set ADS1299 gain              (1B)
    CMD_SET_OUTPUT_MODE   = 0x12,  ///< Set output mode 0=raw 1=µV    (1B)
    CMD_SET_CHANNEL_MASK  = 0x13,  ///< Set channel enable bitmask    (1B)
    CMD_SET_HOST_MODE     = 0x14,  ///< Set host protocol 0=MODERN 1=LEGACY_IES (1B)
                                   ///<   Triggers a live baud change (see uart_channel.h);
                                   ///<   ack is always sent even while entering legacy mode.
    CMD_ENABLE_UART       = 0x20,  ///< Enable/disable UART channel   (1B)
    CMD_ENABLE_BLE        = 0x21,  ///< Enable/disable BLE channel    (1B)
    CMD_QUERY_STATUS      = 0x30,  ///< Query system status           (no payload)
    CMD_SAVE_CONFIG       = 0x31,  ///< Flush RuntimeState to EEPROM  (no payload)
    CMD_GET_VERSION       = 0x32,  ///< Query firmware version         (no payload)
                                   ///<   Response payload: [major][minor][patch] (3 B)

    // ── Nicla binary extension commands ───────────────────────────────────
    CMD_DEMO              = 0x40,  ///< DEMO mode: load defaults, enable test signal,
                                   ///<   start streaming. payload[0]: 0=keep src,
                                   ///<   1=UART, 2=BLE. Runtime only — not saved.
    CMD_RESET             = 0x41,  ///< NVIC_SystemReset() — full hardware reboot.
};


// =============================================================================
// CommandHandlerTask — Execute commands and generate responses
// =============================================================================

/**
 * @brief Command handler task for processing control commands.
 *
 * OPERATION:
 *   1. Pop Command from input queue
 *   2. Dispatch to per-command handler
 *   3. Push Response to PacketiserTask response queue
 *
 * THREAD SAFETY:
 *   Queues are thread-safe (IQueue uses rtos::Mutex).
 *   State changes go through RuntimeState (mutex-protected).
 */
class CommandHandlerTask : public BaseTask {
private:
    FifoQueue<Command, FIFO_DEPTH_CMD>  _cmdQueue;
    IQueue<Response>*                   _responseQueue;

    /** Apply pending runtime config to hardware and start ADS1299 acquisition. */
    void startStreaming();
    /** Stop ADS1299 acquisition and clear the streaming flag. Idempotent. */
    void stopStreaming();

    void executeCommand(const Command& cmd, Response& resp);

    /**
     * @brief Force legacy-safe channel mask, output mode, sample rate, and
     * downsampling factor.
     *
     * 115200 baud cannot sustain more than ~250 SPS × 2 channels of iES
     * µV-integer frames, so legacy mode always runs the ADS1299 ADC directly
     * at 250 SPS with no decimation (RuntimeState default is 1 kSPS ÷ DS×4 —
     * same effective throughput, but via wasteful 4x oversampling that also
     * shifts the ADS1299's internal digital filter response away from what
     * OpenVIBE's declared 250 Hz sample rate expects).
     *
     * Called on every transition into LEGACY_IES (CMD_SET_HOST_MODE) and
     * after cmdDemo()'s RuntimeState::initialize(), which would otherwise
     * silently revert to the 1 kSPS/DS×4 factory default.
     */
    void enforceLegacyDefaults();

    // iES commands
    void cmdStartStreaming(const Command& cmd, Response& resp);
    void cmdStopStreaming(const Command& cmd, Response& resp);
    void cmdTimeSync(const Command& cmd, Response& resp);
    void cmdHeartbeat(const Command& cmd, Response& resp);
    void cmdUartPrintSel(const Command& cmd, Response& resp);
    void cmdDownsampling(const Command& cmd, Response& resp);
    void cmdImpedanceOn(const Command& cmd, Response& resp);
    void cmdImpedanceOff(const Command& cmd, Response& resp);
    void cmdSoftReset(const Command& cmd, Response& resp);

    // Nicla binary commands
    void cmdSetOdr(const Command& cmd, Response& resp);
    void cmdSetGain(const Command& cmd, Response& resp);
    void cmdSetOutputMode(const Command& cmd, Response& resp);
    void cmdSetChannelMask(const Command& cmd, Response& resp);
    void cmdSetHostMode(const Command& cmd, Response& resp);
    void cmdEnableUart(const Command& cmd, Response& resp);
    void cmdEnableBle(const Command& cmd, Response& resp);
    void cmdQueryStatus(const Command& cmd, Response& resp);
    void cmdSaveConfig(const Command& cmd, Response& resp);
    void cmdGetVersion(const Command& cmd, Response& resp);

    // Nicla binary extension commands (continued)
    void cmdDemo(const Command& cmd, Response& resp);
    void cmdReset(const Command& cmd, Response& resp);

protected:
    void run() override;

public:
    CommandHandlerTask();

    IQueue<Command>* getCommandQueue()           { return &_cmdQueue; }
    void setResponseQueue(IQueue<Response>* q)   { _responseQueue = q; }

};


// -----------------------------------------------------------------------------
// Global instance
// -----------------------------------------------------------------------------

extern CommandHandlerTask cmdHandlerTask;
