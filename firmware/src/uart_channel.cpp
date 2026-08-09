/**
 * @file    uart_channel.cpp
 * @brief   UART channel task implementation.
 */

#include "uart_channel.h"
#include "config.h"
#include "runtime_state.h"
#include <chrono>


// ═════════════════════════════════════════════════════════════════════════════
// Global instance declaration
// ═════════════════════════════════════════════════════════════════════════════

// Create global instance.
// Uses Arduino Serial global object (BufferedSerial over USB CDC).
UartChannelTask uartChannelTask;


// ═════════════════════════════════════════════════════════════════════════════
// UartChannelTask implementation
// ═════════════════════════════════════════════════════════════════════════════

UartChannelTask::UartChannelTask()
    : BaseTask(osPriorityNormal, STACK_SIZE_UART),
      _cmdOutputQueue(nullptr),
      _useSerial(true),
      _rxState(WAIT_START),
      _rxIndex(0),
      _payloadExpected(0),
      _pendingBaud(0)
{
    // Wire the TX queue to this task so Gateway's push() calls notify(),
    // unblocking sleepUntilNotified() in run() immediately.
    _txQueue.setOwner(this);
}

void UartChannelTask::run() {
    #ifdef DEBUG_ENABLE
        static uint32_t dbgLoopCount         = 0;
        static uint32_t dbgLastHealthTime    = millis() + DBG_STAGGER_UART_MS;
        static uint32_t dbgMaxLoopDurationMs = 0;
        static const uint32_t DBG_HEALTH_INTERVAL_MS = 5000;
    #endif

    while (!_stopRequested) {
        #ifdef DEBUG_ENABLE
            uint32_t dbgLoopStart = millis();
        #endif

        // ─────────────────────────────────────────────────────────────────────
        // 1. Process TX queue (WireFrames from GatewayTask)
        // ─────────────────────────────────────────────────────────────────────
        bool txProgress = processTx();

        // ─────────────────────────────────────────────────────────────────────
        // 1b. Apply a pending baud-rate change (CMD_SET_HOST_MODE), but only
        //     once the TX queue is empty — guarantees the command's own ack
        //     was fully flushed at the *old* baud before switching.
        // ─────────────────────────────────────────────────────────────────────
        if (_pendingBaud != 0 && _txQueue.isEmpty()) {
            applyPendingBaudChange();
        }

        // ─────────────────────────────────────────────────────────────────────
        // 2. Process RX (incoming bytes → Commands)
        // ─────────────────────────────────────────────────────────────────────
        processRx();
        
        // ─────────────────────────────────────────────────────────────────────
        // 3. UART-specific loop metrics (every 5 s)
        //    Loop-rate and max-loop-ms reflect this task's scheduling health.
        //    System-wide heap/memory stats are printed by the main loop().
        // ─────────────────────────────────────────────────────────────────────
        #ifdef DEBUG_ENABLE
        {
            uint32_t loopDuration = millis() - dbgLoopStart;
            if (loopDuration > dbgMaxLoopDurationMs) {
                dbgMaxLoopDurationMs = loopDuration;
            }
            dbgLoopCount++;

            uint32_t now = millis();
            if (now - dbgLastHealthTime >= DBG_HEALTH_INTERVAL_MS) {
                char dbgBuf[80];
                snprintf(dbgBuf, sizeof(dbgBuf),
                    "[UART] LoopRate: %lu iter/s | MaxLoopMs: %lu\r\n",
                    (dbgLoopCount * 1000UL) / (now - dbgLastHealthTime),
                    (unsigned long)dbgMaxLoopDurationMs);
                debugTryPrint(dbgBuf);

                dbgLoopCount         = 0;
                dbgLastHealthTime    = now;
                dbgMaxLoopDurationMs = 0;
            }
        }
        #endif

        // -------------------------------------------------------------------------
        // 4. Cooperative scheduling:
        //    If TX makes progress, yield to keep latency low.
        //    If TX is backpressured, sleep briefly so a slow host cannot force
        //    the task into a hot retry loop.
        // -------------------------------------------------------------------------
        if (_txQueue.isEmpty()) {
            sleepUntilNotified(1);
        } else if (txProgress) {
            rtos::ThisThread::yield();
        } else {
            rtos::ThisThread::sleep_for(std::chrono::milliseconds(UART_BACKPRESSURE_SLEEP_MS));
        }
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// Helper methods (stubs — to be implemented)
// ═════════════════════════════════════════════════════════════════════════════

bool UartChannelTask::processTx() {
    #ifdef DEBUG_ENABLE
        static uint32_t debugTxFrameCount     = 0;
        static uint32_t debugLastTxReportTime = millis();
        static const uint32_t DEBUG_TX_REPORT_INTERVAL_MS = 1000;
    #endif

    bool txProgress = false;
    WireFrame frame;
    while (_txQueue.peek(frame)) {
        if (!sendFrame(frame)) {
            break;
        }

        _txQueue.pop(frame);
        txProgress = true;

        #ifdef DEBUG_ENABLE
            debugTxFrameCount++;
            uint32_t now = millis();
            if (now - debugLastTxReportTime >= DEBUG_TX_REPORT_INTERVAL_MS) {
                float txRate = (debugTxFrameCount * 1000.0f) / (now - debugLastTxReportTime);
                char dbgBuf[192];
                int  pos = 0;
                pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                    "[UART TX] StatBegin:\n"
                    "  Frame rate: %.1f frames/s\n"
                    "  Queue: %u/%u | Drops: %lu\n"
                    "[UART TX] StatEnd.\n",
                    txRate,
                    (unsigned)_txQueue.size(),
                    (unsigned)_txQueue.capacity(),
                    (unsigned long)_txQueue.droppedCount());
                debugTryPrint(dbgBuf);
                debugTxFrameCount     = 0;
                debugLastTxReportTime = now;
            }
        #endif
    }

    return txProgress;
}

// ─────────────────────────────────────────────────────────────────────────────
// payloadLenForCmd()
//
// Returns the number of payload bytes that follow a bare-byte iES command.
// Returns 0xFF if the command byte is unrecognised.
//
// Reference: docs/ies_message_protocol.md §4 and
//            code_references/iES_v0.3-master/ies_app/ies_task.cpp::btspp_recv_task_fxn()
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t payloadLenForCmd(uint8_t cmd) {
    switch (cmd) {
        // iES bare-byte commands — no payload
        case IES_CMD_STREAM_START:    // 'b'
        case IES_CMD_STREAM_STOP:     // 's'
        case IES_CMD_HEARTBEAT:       // '.'
        case IES_CMD_IMPEDANCE_ON:    // 'Z'
        case IES_CMD_IMPEDANCE_OFF:   // 'z'
        case IES_CMD_SOFT_RESET:      // 'v'
            return 0;

        // iES bare-byte commands — 1-byte payload
        case IES_CMD_UART_PRINT_SEL:  // 'p' — sub-command byte
        case IES_CMD_DOWNSAMPLING:    // 'd' — ratio byte
            return 1;

        // iES time-sync: legacy (OpenVIBE) sends only 4-byte epoch, no CRC —
        // its sendCommand() uses strlen() on a buffer ending in '\0' at index 5,
        // so the CRC byte never reaches the wire. Modern hosts send the full
        // 4-byte epoch + 1-byte CRC-8. See ovasCDriveriES.cpp resetBoard().
        case IES_CMD_TIME_SYNC:       // 't'
            return g_runtimeState.isOpenVibeMode() ? 4 : 5;

        // Nicla binary config commands — 1-byte payload
        case 0x10:  // CMD_SET_ODR          — 1B ODR enum value
        case 0x12:  // CMD_SET_OUTPUT_MODE  — 1B (0=raw, 1=µV)
        case 0x13:  // CMD_SET_CHANNEL_MASK — 1B bitmask
        case 0x14:  // CMD_SET_HOST_MODE    — 1B (0=MODERN, 1=LEGACY_IES)
        case 0x20:  // CMD_ENABLE_UART      — 1B enable flag
        case 0x21:  // CMD_ENABLE_BLE       — 1B enable flag
            return 1;

        // CMD_SET_GAIN — 2 bytes: [channel 1-4][gain code ADS_GAINxx]
        case 0x11:
            return 2;

        // Nicla binary commands — no payload
        case 0x30:  // CMD_QUERY_STATUS
        case 0x31:  // CMD_SAVE_CONFIG
        case 0x32:  // CMD_GET_VERSION
            return 0;

        // DEMO / RESET
        case 0x40:  // CMD_DEMO — 1B: 0=src channel, 1=UART, 2=BLE
            return 1;
        case 0x41:  // CMD_RESET — no payload
            return 0;

        default:
            return 0xFF;  // unknown
    }
}

void UartChannelTask::processRx() {
    #ifdef DEBUG_ENABLE
        static uint32_t debugRxByteCount      = 0;
        static uint32_t debugRxCmdCount       = 0;
        static uint32_t debugRxErrorCount     = 0;
        static uint32_t debugLastRxReportTime = millis();
        static uint32_t debugRxStateEntryTime = millis();
        static RxState  debugLastLoggedState  = WAIT_START;
        static const uint32_t DEBUG_RX_REPORT_INTERVAL_MS = 1000;
        static const uint32_t DEBUG_RX_STALL_TIMEOUT_MS   = 2000;
    #endif

    // ── Stall detection ───────────────────────────────────────────────────────
    #ifdef DEBUG_ENABLE
    if (_rxState != debugLastLoggedState) {
        debugRxStateEntryTime = millis();
        debugLastLoggedState  = _rxState;
    } else if (_rxState == IN_FRAME) {
        uint32_t stateAge = millis() - debugRxStateEntryTime;
        if (stateAge >= DEBUG_RX_STALL_TIMEOUT_MS) {
            char stallBuf[96];
            snprintf(stallBuf, sizeof(stallBuf),
                "[UART RX] STALL: stuck in IN_FRAME for %lu ms (cmd=0x%02X, got %u/%u) — reset\r\n",
                (unsigned long)stateAge,
                (unsigned)_rxBuffer[0],
                (unsigned)(_rxIndex - 1),
                (unsigned)_payloadExpected);
            debugTryPrint(stallBuf);
            debugRxErrorCount++;
            _rxState = WAIT_START;
            _rxIndex = 0;
            _payloadExpected = 0;
            debugRxStateEntryTime = millis();
            debugLastLoggedState  = WAIT_START;
        }
    }
    #endif

    // Process all bytes currently available (drain hardware FIFO in one call)
    while (_useSerial && Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();
        #ifdef DEBUG_ENABLE
            debugRxByteCount++;
        #endif

        if (_rxState == WAIT_START) {
            // ── First byte of a new command ──────────────────────────────────
            uint8_t plen = payloadLenForCmd(b);
            if (plen == 0xFF) {
                // Unknown command byte — log and discard
                #ifdef DEBUG_ENABLE
                {
                    char errBuf[64];
                    snprintf(errBuf, sizeof(errBuf),
                        "[UART RX] Unknown cmd byte 0x%02X — discarded\r\n", (unsigned)b);
                    debugTryPrint(errBuf);
                    debugRxErrorCount++;
                }
                #endif
                continue;
            }

            _rxBuffer[0]     = b;
            _rxIndex         = 1;
            _payloadExpected = plen;

            if (plen == 0) {
                // No payload — command is complete immediately
                _rxState = FRAME_DONE;
            } else {
                _rxState = IN_FRAME;
            }
        } else if (_rxState == IN_FRAME) {
            // ── Accumulate payload bytes ─────────────────────────────────────
            if (_rxIndex - 1 < _payloadExpected && _rxIndex < sizeof(_rxBuffer)) {
                _rxBuffer[_rxIndex++] = b;
            }
            if ((_rxIndex - 1) >= _payloadExpected) {
                _rxState = FRAME_DONE;
            }
        }

        if (_rxState == FRAME_DONE) {
            // ── Dispatch command ─────────────────────────────────────────────
            if (_cmdOutputQueue) {
                Command cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd_id      = _rxBuffer[0];
                cmd.payload_len = (uint8_t)_payloadExpected;
                cmd.source      = CmdSource::UART;
                for (uint8_t i = 0; i < _payloadExpected && i < IES_CMD_PAYLOAD_MAX; i++) {
                    cmd.payload[i] = _rxBuffer[1 + i];
                }
                _cmdOutputQueue->push(cmd);
                #ifdef DEBUG_ENABLE
                    debugRxCmdCount++;
                #endif
            }
            // Reset for next command
            _rxState         = WAIT_START;
            _rxIndex         = 0;
            _payloadExpected = 0;
        }
    }

    // ── Periodic RX stats ─────────────────────────────────────────────────────
    #ifdef DEBUG_ENABLE
    {
        uint32_t now = millis();
        static const uint32_t DEBUG_RX_REPORT_INTERVAL_MS = 1000;
        if (now - debugLastRxReportTime >= DEBUG_RX_REPORT_INTERVAL_MS) {
            uint32_t elapsed = now - debugLastRxReportTime;
            float byteRate = (debugRxByteCount * 1000.0f) / elapsed;
            float cmdRate  = (debugRxCmdCount  * 1000.0f) / elapsed;
            char dbgBuf[192];
            snprintf(dbgBuf, sizeof(dbgBuf),
                "[UART RX] StatBegin:\n"
                "  Byte rate: %.1f B/s | Cmd rate: %.1f cmds/s\n"
                "  Errors: %lu | state=%d\n"
                "[UART RX] StatEnd.\n",
                byteRate, cmdRate,
                (unsigned long)debugRxErrorCount,
                (int)_rxState);
            debugTryPrint(dbgBuf);
            debugRxByteCount      = 0;
            debugRxCmdCount       = 0;
            debugRxErrorCount     = 0;
            debugLastRxReportTime = now;
        }
    }
    #endif
}

bool UartChannelTask::sendFrame(const WireFrame& frame) {
    if (!_useSerial || frame.len == 0) return true;

    // EEG streaming is the highest-priority requirement.
    // Use direct write to avoid starvation if availableForWrite() under-reports
    // capacity on this USB CDC implementation.
    Serial.write(frame.bytes, frame.len);
    return true;
}

bool UartChannelTask::parseFrame() {
    // Reserved for future framed BLE command parsing.
    // UART RX now uses the bare-byte state machine in processRx().
    return false;
}

void UartChannelTask::applyPendingBaudChange() {
    uint32_t newBaud = _pendingBaud;
    _pendingBaud = 0;
    if (newBaud == 0) return;

    #if DBG_UART
    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "[UART] Switching baud to %lu ...\r\n", (unsigned long)newBaud);
        debugTryPrint(dbg);
    }
    #endif

    // Native USB-CDC (not a bridged hardware UART) — no physical bit-timing
    // risk, but the host must close/reopen its port at the new baud after
    // this call, since the device's line coding changes here.
    Serial.end();
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(SERIAL_BAUD_SWITCH_DELAY_MS));
    Serial.begin(newBaud);
}
