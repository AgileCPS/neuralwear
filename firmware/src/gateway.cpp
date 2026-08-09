/**
 * @file    gateway.cpp
 * @brief   Gateway task implementation.
 */

#include "gateway.h"
#include <chrono>


// ═════════════════════════════════════════════════════════════════════════════
// Global instance
// ═════════════════════════════════════════════════════════════════════════════

GatewayTask gatewayTask;


// ═════════════════════════════════════════════════════════════════════════════
// GatewayTask implementation
// ═════════════════════════════════════════════════════════════════════════════

GatewayTask::GatewayTask()
    : BaseTask(osPriorityNormal, STACK_SIZE_GATEWAY),
      _cmdHandlerQueue(nullptr),
      _uartTxQueue(nullptr),
      _bleTxQueue(nullptr),
      _uartEnabled(true),   // UART active by default (Phase 2)
      _bleEnabled(false)
{
    // Wire all input queues to this task so any producer immediately
    // unblocks the run() loop via INotifiable::notify().
    _dataQueue.setOwner(this);
    _cmdFromUartQueue.setOwner(this);
    _cmdFromBleQueue.setOwner(this);
}

void GatewayTask::run() {
    #ifdef DEBUG_ENABLE
        static uint32_t dbgLoopCount         = 0;
        static uint32_t dbgLastHealthTime    = millis() + DBG_STAGGER_GATEWAY_MS;
        static uint32_t dbgMaxLoopDurationMs = 0;
        static const uint32_t DBG_HEALTH_INTERVAL_MS = 1000;
    #endif

    while (!_stopRequested) {
        #ifdef DEBUG_ENABLE
            uint32_t dbgLoopStart = millis();
        #endif

        bool hasWork = false;
        
        // ─────────────────────────────────────────────────────────────────────
        // 1. Check for commands from channels (higher priority)
        // ─────────────────────────────────────────────────────────────────────
        Command cmd;
        
        // UART command path
        if (_cmdFromUartQueue.pop(cmd)) {
            if (validateCommand(cmd)) {
                if (_cmdHandlerQueue) {
                    _cmdHandlerQueue->push(cmd);
                }
            }
            hasWork = true;
        }
        
        // BLE command path
        if (_cmdFromBleQueue.pop(cmd)) {
            if (validateCommand(cmd)) {
                if (_cmdHandlerQueue) {
                    _cmdHandlerQueue->push(cmd);
                }
            }
            hasWork = true;
        }
        
        // ─────────────────────────────────────────────────────────────────────
        // 2. Route data packets to enabled channels — drain ALL queued frames
        //    in one pass before yielding.  Processing one frame per loop
        //    iteration and calling yield() each time costs ~1 OS reschedule
        //    per frame (~998/s at 1kSPS), which was the source of the ~8%
        //    throughput deficit seen on the GatewayTask input queue.
        // ─────────────────────────────────────────────────────────────────────
        WireFrame pkt;
        while (_dataQueue.pop(pkt)) {
            routeWireFrame(pkt);
            hasWork = true;
        }
        
        // ─────────────────────────────────────────────────────────────────
        // 3. Cooperative scheduling:
        //    Block until a queue signals via INotifiable::notify()
        //    (up to 1 ms safety timeout) when there is no work.
        //    Do NOT yield when there was work — yielding after every frame
        //    causes per-frame OS reschedule overhead that limits throughput.
        // ─────────────────────────────────────────────────────────────────
        if (!hasWork) {
            sleepUntilNotified(1);
        }

        // ─────────────────────────────────────────────────────────────────────
        // Loop metrics (DEBUG_ENABLE, every 5 s)
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
                char dbgBuf[384];
                int  pos = 0;
                pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                    "[GATEWAY] LoopRate: %lu iter/s | MaxLoopMs: %lu\n"
                    "[GATEWAY] StatBegin:\n",
                    (dbgLoopCount * 1000UL) / (now - dbgLastHealthTime),
                    (unsigned long)dbgMaxLoopDurationMs);

                // Input queue (WireFrames from PacketiserTask)
                if (pos < (int)sizeof(dbgBuf)) {
                    pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                        "  Input queue: %u/%u | Drops: %lu\n",
                        (unsigned)_dataQueue.size(),
                        (unsigned)_dataQueue.capacity(),
                        (unsigned long)_dataQueue.droppedCount());
                }

                // Gateway → channel output queues
                if (pos < (int)sizeof(dbgBuf) && _uartTxQueue) {
                    pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                        "  UART TX queue: %u/%u | Drops: %lu\n",
                        (unsigned)_uartTxQueue->size(),
                        (unsigned)_uartTxQueue->capacity(),
                        (unsigned long)_uartTxQueue->droppedCount());
                }
                if (pos < (int)sizeof(dbgBuf) && _bleTxQueue) {
                    pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                        "  BLE TX queue:  %u/%u | Drops: %lu\n",
                        (unsigned)_bleTxQueue->size(),
                        (unsigned)_bleTxQueue->capacity(),
                        (unsigned long)_bleTxQueue->droppedCount());
                }

                if (pos < (int)sizeof(dbgBuf)) {
                    snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                        "[GATEWAY] StatEnd.\n");
                }
                debugTryPrint(dbgBuf);
                dbgLoopCount         = 0;
                dbgLastHealthTime    = now;
                dbgMaxLoopDurationMs = 0;
            }
        }
        #endif
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// Public methods
// ═════════════════════════════════════════════════════════════════════════════

void GatewayTask::routeWireFrame(const WireFrame& pkt) {
    switch (pkt.dest) {
        case FrameDest::UART:
            if (_uartTxQueue) {
                _uartTxQueue->push(pkt);
            }
            break;
        case FrameDest::BLE:
            if (_bleTxQueue) {
                _bleTxQueue->push(pkt);
            }
            break;
    }
}

void GatewayTask::setUartEnabled(bool enabled) {
    _flagsMutex.lock();
    if (enabled) {
        _uartEnabled = true;
        _bleEnabled  = false;
    } else {
        _uartEnabled = false;
    }
    _flagsMutex.unlock();
}

void GatewayTask::setBleEnabled(bool enabled) {
    _flagsMutex.lock();
    if (enabled) {
        _bleEnabled  = true;
        _uartEnabled = false;
    } else {
        _bleEnabled = false;
    }
    _flagsMutex.unlock();
}

bool GatewayTask::isUartEnabled() {
    _flagsMutex.lock();
    bool enabled = _uartEnabled;
    _flagsMutex.unlock();
    return enabled;
}

bool GatewayTask::isBleEnabled() {
    _flagsMutex.lock();
    bool enabled = _bleEnabled;
    _flagsMutex.unlock();
    return enabled;
}


// ═════════════════════════════════════════════════════════════════════════════
// Helper methods
// ═════════════════════════════════════════════════════════════════════════════

bool GatewayTask::validateCommand(const Command& cmd) {
    // TODO: Implement lightweight command validation
    // - Check payload_len <= MAX_COMMAND_PAYLOAD
    // - Check cmd_id is a known CommandId value
    // - Optional: CRC check if implemented
    
    // For now, accept all commands
    return true;
}
