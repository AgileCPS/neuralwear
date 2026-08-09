/**
 * @file    ble_channel.h
 * @brief   BLE channel task for wireless communication.
 *
 * ARCHITECTURE (firmware_architecture.md Section 6.5):
 *   BleChannelTask is a pure transport pump (same role as UartChannelTask):
 *     - TX path: pop WireFrame from _txQueue → MTU packetization → BLE notify
 *     - RX path: BLE write callback → detect IES frame boundaries → Command
 *       → push to GatewayTask._cmdFromBleQueue
 *
 * Stage 2: GATT service + advertising (ble.init from run() only — never setup/loop).
 * See ble_channel_design.md §9.2 and config.h BLE_RADIO_INIT_ENABLE.
 *
 * THREAD SAFETY: BLE callbacks run on the EventQueue thread. Never call Serial there.
 * Log via pollStatusLog() from loop() only.
 */

#pragma once

#include "mbed.h"
#include "task.h"
#include "packetiser.h"
#include "cmd.h"
#include "config.h"


// GATT UUIDs (ble_channel_design.md §9.1.1)
#define BLE_SERVICE_UUID     "A9E07020-0001-4A58-B8C9-3F0DAB7E5C1D"
#define BLE_CHAR_UUID_TX     "A9E07020-0002-4A58-B8C9-3F0DAB7E5C1D"
#define BLE_CHAR_UUID_RX     "A9E07020-0003-4A58-B8C9-3F0DAB7E5C1D"

#define BLE_ATT_PAYLOAD_DEFAULT   20
#define BLE_ATT_PAYLOAD_MAX      244


enum class BleInitState : uint8_t {
    Idle = 0,
    Starting,
    Ready,
    Failed,
};

enum class BleConnEvent : uint8_t {
    None = 0,
    Connected,
    Disconnected,
};

class BleChannelTask : public BaseTask {
private:
    FifoQueue<WireFrame, BLE_TX_QUEUE_SIZE>  _txQueue;
    IQueue<Command>*  _cmdOutputQueue;

    bool      _connected;
    uint16_t  _currentMtu;

    volatile BleInitState  _initState;
    volatile uint8_t       _initError;
    volatile BleConnEvent  _connEvent;

    void initBleStack();

protected:
    void run() override;

public:
    BleChannelTask();

    IQueue<WireFrame>* getTxQueue() { return &_txQueue; }
    void setCmdOutputQueue(IQueue<Command>* queue) { _cmdOutputQueue = queue; }

    /** Print init/connection status from loop() — never from BLE callbacks. */
    void pollStatusLog(unsigned long nowMs);

    BleInitState getInitState() const { return _initState; }
    uint8_t      getInitError() const { return _initError; }
    BleConnEvent takeConnEvent();

    bool     isConnected() const { return _connected; }
    uint16_t getCurrentMtu() const { return _currentMtu; }
    void     setCurrentMtu(uint16_t mtu) { _currentMtu = mtu; }

    void onConnect();
    void onDisconnect();
    void onDataReceived(const uint8_t* data, size_t len);

    void reportInitReady();
    void reportInitFailed(uint8_t error);
};


extern BleChannelTask bleChannelTask;
