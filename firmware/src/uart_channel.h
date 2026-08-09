/**
 * @file    uart_channel.h
 * @brief   UART channel task for serial communication.
 *
 * ARCHITECTURE (firmware_architecture.md Section 6.5):
 *   UartChannelTask is a pure transport pump:
 *     - TX path: pop WireFrame from _txQueue → Serial.write(frame.bytes, frame.len)
 *       IES serialisation is done by PacketiserTask; no format knowledge here.
 *     - RX path: Serial read → accumulate IES frame bytes → detect [0xA0..0xC0]
 *       → construct Command → push to GatewayTask._cmdFromUartQueue
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
// UartChannelTask - UART serial communication
// =============================================================================

/**
 * @brief UART channel task — pure transport pump.
 *
 * OPERATION:
 *   TX path:
 *     1. Pop WireFrame from _txQueue (filled by GatewayTask)
 *     2. Serial.write(frame.bytes, frame.len) — IES bytes transmitted as-is
 *
 *   RX path:
 *     1. Read bytes from Serial
 *     2. Detect IES frame boundaries ([0xA0 start] … [0xC0 stop])
 *     3. Extract command payload, construct Command
 *     4. Push to GatewayTask._cmdFromUartQueue
 *
 * THREAD SAFETY:
 *   Serial port access is NOT thread-safe (single owner: UART task).
 *   Queues are thread-safe (IQueue uses rtos::Mutex).
 */
class UartChannelTask : public BaseTask {
private:
    // TX input queue (WireFrames from GatewayTask)
    FifoQueue<WireFrame, UART_TX_QUEUE_SIZE>  _txQueue;

    // Output queue (commands to Gateway)
    IQueue<Command>*  _cmdOutputQueue;
    
    // Serial port (using Arduino Serial, set in constructor)
    // Note: Arduino Serial is BufferedSerial, not UnbufferedSerial
    bool  _useSerial;
    
    // RX state machine — bare-byte iES command parser
    // WAIT_START: waiting for a command byte
    // IN_FRAME:   accumulating payload bytes for the current command
    // FRAME_DONE: all payload bytes received, command ready to dispatch
    enum RxState {
        WAIT_START,
        IN_FRAME,
        FRAME_DONE
    };
    RxState  _rxState;
    uint8_t  _rxBuffer[IES_MAX_FRAME_SIZE + 4];  // [0]=cmd byte, [1..]=payload
    size_t   _rxIndex;          // bytes received so far (including cmd byte)
    size_t   _payloadExpected;  // total payload bytes expected after cmd byte

    // Pending baud-rate change requested by CommandHandlerTask (CMD_SET_HOST_MODE).
    // Applied only once _txQueue is empty, i.e. after the command's own ack has
    // been flushed at the *old* baud. 0 = no change pending.
    volatile uint32_t _pendingBaud;

    bool processTx();
    void processRx();
    bool sendFrame(const WireFrame& frame);
    bool parseFrame();
    void applyPendingBaudChange();

protected:
    /**
     * @brief Main UART communication loop.
     *
     * Runs at osPriorityNormal (0).
     */
    void run() override;

public:
    /**
     * @brief Construct the UART channel task.
     *
     * Uses Arduino Serial global object (BufferedSerial over USB CDC).
     */
    UartChannelTask();
    
    /**
     * @brief Get the TX input queue (for GatewayTask to publish WireFrames).
     */
    IQueue<WireFrame>* getTxQueue() { return &_txQueue; }
    
    /**
     * @brief Set the command output queue (Gateway's command queue).
     */
    void setCmdOutputQueue(IQueue<Command>* queue) { _cmdOutputQueue = queue; }

    /**
     * @brief Request a live baud-rate change (e.g. on CMD_SET_HOST_MODE).
     *
     * Thread-safe (single-writer volatile flag from CommandHandlerTask,
     * consumed by this task's own run() loop). Actual Serial.end()/begin()
     * happens only once the TX queue has drained, guaranteeing any pending
     * ack is transmitted at the *old* baud before switching.
     */
    void requestBaudChange(uint32_t baud) { _pendingBaud = baud; }
};


// -----------------------------------------------------------------------------
// Global instance
// -----------------------------------------------------------------------------

extern UartChannelTask uartChannelTask;
