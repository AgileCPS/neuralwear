/**
 * @file    gateway.h
 * @brief   Gateway task for routing WireFrames and commands between channels.
 *
 * ARCHITECTURE (firmware_architecture.md Section 6.4):
 *   GatewayTask routes:
 *     - WireFrame objects from PacketiserTask to one channel TX queue (UART or BLE)
 *     - Commands from channel RX paths to CommandHandlerTask
 *   Lightweight command validation only; no data transformation.
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
// GatewayTask - Route data and commands between tasks and channels
// =============================================================================

/**
 * @brief Central routing task for WireFrames and commands.
 *
 * OPERATION:
 *   Data path (PacketiserTask → channels):
 *     1. Pop WireFrame from _dataQueue
 *     2. Push to the queue named by frame.dest (unicast — UART or BLE, never both)
 *
 *   Command path (channels → CommandHandler):
 *     1. Pop Command from _cmdFromUartQueue or _cmdFromBleQueue
 *     2. Validate command structure
 *     3. Forward to CommandHandler
 *
 * THREAD SAFETY:
 *   All queues are thread-safe (IQueue uses rtos::Mutex).
 *   Channel enable flags protected by _flagsMutex (mutually exclusive transport).
 */
class GatewayTask : public BaseTask {
private:
    // Input queue (WireFrames from PacketiserTask)
    FifoQueue<WireFrame, FIFO_DEPTH_GATEWAY_DATA>  _dataQueue;
    
    // Input queues (commands from channels)
    FifoQueue<Command, FIFO_DEPTH_CMD>  _cmdFromUartQueue;
    FifoQueue<Command, FIFO_DEPTH_CMD>  _cmdFromBleQueue;
    
    // Output channel TX queues (unicast routing targets)
    IQueue<WireFrame>*  _uartTxQueue;
    IQueue<WireFrame>*  _bleTxQueue;
    
    // Output subscriber (commands to handler)
    IQueue<Command>*  _cmdHandlerQueue;
    
    // Active transport flags (mutually exclusive; protected by mutex)
    rtos::Mutex  _flagsMutex;
    bool         _uartEnabled;
    bool         _bleEnabled;
    
    // Helper methods
    bool validateCommand(const Command& cmd);
    void routeWireFrame(const WireFrame& pkt);

protected:
    /**
     * @brief Main routing loop.
     *
     * Runs at osPriorityNormal (0).
     */
    void run() override;

public:
    /**
     * @brief Construct the Gateway task.
     */
    GatewayTask();
    
    /**
     * @brief Get the data input queue (for PacketiserTask to publish).
     */
    IQueue<WireFrame>* getDataQueue() { return &_dataQueue; }
    
    /**
     * @brief Get the command input queue from UART channel.
     */
    IQueue<Command>* getUartCommandQueue() { return &_cmdFromUartQueue; }
    
    /**
     * @brief Get the command input queue from BLE channel.
     */
    IQueue<Command>* getBleCommandQueue() { return &_cmdFromBleQueue; }
    
    /**
     * @brief Wire UART channel TX queue for unicast routing.
     */
    void setUartChannel(IQueue<WireFrame>* channelQueue) { _uartTxQueue = channelQueue; }
    
    /**
     * @brief Wire BLE channel TX queue for unicast routing.
     */
    void setBleChannel(IQueue<WireFrame>* channelQueue) { _bleTxQueue = channelQueue; }
    
    /**
     * @brief Set the command handler queue.
     */
    void setCmdHandlerQueue(IQueue<Command>* queue) { _cmdHandlerQueue = queue; }
    
    /**
     * @brief Enable UART as the active data transport (disables BLE).
     */
    void setUartEnabled(bool enabled);
    
    /**
     * @brief Enable BLE as the active data transport (disables UART).
     */
    void setBleEnabled(bool enabled);
    
    /**
     * @brief Check if UART is the active data transport.
     */
    bool isUartEnabled();
    
    /**
     * @brief Check if BLE is the active data transport.
     */
    bool isBleEnabled();
};


// -----------------------------------------------------------------------------
// Global instance
// -----------------------------------------------------------------------------

extern GatewayTask gatewayTask;
