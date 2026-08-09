/**
 * @file    ble_channel.cpp
 * @brief   BLE channel task implementation.
 *
 * Stage 2: stack init, GATT service, advertising.
 * Stage 3+: TX aggregation loop in run().
 *
 * IMPORTANT: Never call Serial from BLE EventQueue callbacks — USB CDC is not
 * thread-safe and will deadlock if setup()/loop() is printing concurrently.
 */

#include "ble_channel.h"
#include <chrono>

#if FEATURE_BLE_ENABLE
#include <Arduino.h>

#include "ble/BLE.h"
#include "ble/Gap.h"
#include "ble/GattServer.h"
#include "ble/GattCharacteristic.h"
#include "ble/GattService.h"
#include "ble/gap/AdvertisingDataSimpleBuilder.h"
#include "ble/gap/AdvertisingParameters.h"
#include "events/EventQueue.h"
#include "rtos/Thread.h"

using namespace ble;


namespace {

static events::EventQueue bleEventQueue;
static rtos::Thread*      bleEventThread = nullptr;
static bool               bleStackStarted = false;


void scheduleBleEvents(BLE::OnEventsToProcessCallbackContext* context) {
    bleEventQueue.call(mbed::callback(&context->ble, &BLE::processEvents));
}


ble_error_t registerGattAndAdvertise(BLE& ble) {
    // Lazy-init GATT objects on first call (after BLE::init), not at static ctor time.
    static uint8_t txCharBuffer[BLE_ATT_PAYLOAD_MAX];
    static uint8_t rxCharBuffer[BLE_ATT_PAYLOAD_DEFAULT];

    static GattCharacteristic txCharacteristic(
        UUID(BLE_CHAR_UUID_TX),
        txCharBuffer,
        0,
        sizeof(txCharBuffer),
        GattCharacteristic::BLE_GATT_CHAR_PROPERTIES_NOTIFY
    );

    static GattCharacteristic rxCharacteristic(
        UUID(BLE_CHAR_UUID_RX),
        rxCharBuffer,
        0,
        sizeof(rxCharBuffer),
        GattCharacteristic::BLE_GATT_CHAR_PROPERTIES_WRITE_WITHOUT_RESPONSE
    );

    static GattCharacteristic* charTable[] = {
        &txCharacteristic,
        &rxCharacteristic,
    };

    static GattService eegService(
        UUID(BLE_SERVICE_UUID),
        charTable,
        sizeof(charTable) / sizeof(charTable[0])
    );

    ble_error_t err = ble.gattServer().addService(eegService);
    if (err != BLE_ERROR_NONE) {
        return err;
    }

    Gap& gap = ble.gap();
    err = gap.setAdvertisingParameters(
        LEGACY_ADVERTISING_HANDLE,
        AdvertisingParameters());
    if (err != BLE_ERROR_NONE) {
        return err;
    }

    const UUID serviceUuid(BLE_SERVICE_UUID);
    err = gap.setAdvertisingPayload(
        LEGACY_ADVERTISING_HANDLE,
        AdvertisingDataSimpleBuilder<LEGACY_ADVERTISING_MAX_SIZE>()
            .setFlags()
            .setLocalService(serviceUuid)
            .getAdvertisingData());
    if (err != BLE_ERROR_NONE) {
        return err;
    }

    err = gap.setAdvertisingScanResponse(
        LEGACY_ADVERTISING_HANDLE,
        AdvertisingDataSimpleBuilder<LEGACY_ADVERTISING_MAX_SIZE>()
            .setName(BLE_DEVICE_NAME, true)
            .getAdvertisingData());
    if (err != BLE_ERROR_NONE) {
        return err;
    }

    return gap.startAdvertising(LEGACY_ADVERTISING_HANDLE);
}


class BleEventHandler : public GattServer::EventHandler,
                        public Gap::EventHandler {
    connection_handle_t _connHandle = 0;

public:
    void onConnectionComplete(const ConnectionCompleteEvent& event) override {
        if (event.getStatus() != BLE_ERROR_NONE) {
            return;
        }

        _connHandle = event.getConnectionHandle();
        bleChannelTask.onConnect();

        BLE::Instance().gap().updateConnectionParameters(
            _connHandle,
            conn_interval_t(millisecond_t(BLE_CONN_INTERVAL_MIN_MS)),
            conn_interval_t(millisecond_t(BLE_CONN_INTERVAL_MAX_MS)),
            slave_latency_t(BLE_SLAVE_LATENCY),
            supervision_timeout_t(millisecond_t(BLE_CONN_TIMEOUT_MS))
        );
    }

    void onDisconnectionComplete(const DisconnectionCompleteEvent& event) override {
        (void)event;
        _connHandle = 0;
        bleChannelTask.onDisconnect();
        BLE::Instance().gap().startAdvertising(LEGACY_ADVERTISING_HANDLE);
    }

    void onDataSent(const GattDataSentCallbackParams& params) override {
        (void)params;
        // Stage 4: release _txDrainSemaphore for backpressure.
    }

    void onDataWritten(const GattWriteCallbackParams& params) override {
        if (params.len == 0 || params.data == nullptr) return;
        bleChannelTask.onDataReceived(params.data, params.len);
    }

    void onAttMtuChange(connection_handle_t connectionHandle,
                        uint16_t attMtuSize) override {
        (void)connectionHandle;
        const uint16_t payload = (attMtuSize > 3)
            ? static_cast<uint16_t>(attMtuSize - 3)
            : BLE_ATT_PAYLOAD_DEFAULT;
        bleChannelTask.setCurrentMtu(payload);
    }

    ble_error_t onStackReady(BLE& ble) {
        ble.gattServer().setEventHandler(this);
        ble.gap().setEventHandler(this);
        return registerGattAndAdvertise(ble);
    }
};

static BleEventHandler bleEventHandler;


void whenBleInitialized(BLE::InitializationCompleteCallbackContext* context) {
    if (context->error != BLE_ERROR_NONE) {
        bleChannelTask.reportInitFailed(static_cast<uint8_t>(context->error));
        return;
    }

    const ble_error_t err = bleEventHandler.onStackReady(context->ble);
    if (err != BLE_ERROR_NONE) {
        bleChannelTask.reportInitFailed(static_cast<uint8_t>(err));
    } else {
        bleChannelTask.reportInitReady();
    }
}

}  // namespace

#endif  // FEATURE_BLE_ENABLE


BleChannelTask bleChannelTask;


BleChannelTask::BleChannelTask()
    : BaseTask(TASK_PRIORITY_BLE, STACK_SIZE_BLE),
      _cmdOutputQueue(nullptr),
      _connected(false),
      _currentMtu(BLE_ATT_PAYLOAD_DEFAULT),
      _initState(BleInitState::Idle),
      _initError(0),
      _connEvent(BleConnEvent::None)
{
    _txQueue.setOwner(this);
}

void BleChannelTask::reportInitFailed(uint8_t error) {
    _initError = error;
    _initState = BleInitState::Failed;
}

void BleChannelTask::reportInitReady() {
    _initState = BleInitState::Ready;
}

BleConnEvent BleChannelTask::takeConnEvent() {
    const BleConnEvent ev = _connEvent;
    _connEvent = BleConnEvent::None;
    return ev;
}

void BleChannelTask::initBleStack() {
#if FEATURE_BLE_ENABLE && BLE_RADIO_INIT_ENABLE
    // Must run from BleChannelTask::run() only — calling ble.init() from setup()
    // or loop() hangs/crashes the Nicla Voice (tested 2026-07-04).
    if (bleStackStarted) {
        return;
    }
    bleStackStarted = true;

    BLE& ble = BLE::Instance();
    ble.onEventsToProcess(scheduleBleEvents);

    if (bleEventThread == nullptr) {
        bleEventThread = new rtos::Thread(osPriorityNormal, STACK_SIZE_BLE_EVENT);
    }
    if (bleEventThread == nullptr) {
        reportInitFailed(0xFE);
        return;
    }

    const osStatus evtStart = bleEventThread->start(
        mbed::callback(&bleEventQueue, &events::EventQueue::dispatch_forever));
    if (evtStart != osOK) {
        reportInitFailed(static_cast<uint8_t>(evtStart));
        return;
    }

    rtos::ThisThread::sleep_for(std::chrono::milliseconds(10));

    const ble_error_t err = ble.init(whenBleInitialized);
    if (err != BLE_ERROR_NONE) {
        reportInitFailed(static_cast<uint8_t>(err));
    }
#endif
}

void BleChannelTask::run() {
#if FEATURE_BLE_ENABLE
    _initState = BleInitState::Starting;
    // Brief settle so setup() finishes all Serial output before ble.init().
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(200));
#if BLE_RADIO_INIT_ENABLE
    initBleStack();
#endif
#endif

    while (!_stopRequested) {
        sleepUntilNotified(100);
    }
}

void BleChannelTask::pollStatusLog(unsigned long nowMs) {
#if FEATURE_BLE_ENABLE
    (void)nowMs;

    static bool loggedStarting = false;
    static bool loggedReady    = false;
    static bool loggedFailed   = false;
    static bool loggedTaskOnly = false;

    const BleInitState initState = _initState;

    if (initState >= BleInitState::Starting && !loggedStarting) {
        loggedStarting = true;
        Serial.println("[BLE] BleChannelTask run() active");
    }
#if !BLE_RADIO_INIT_ENABLE
    if (loggedStarting && !loggedTaskOnly) {
        loggedTaskOnly = true;
        Serial.println("[BLE] Radio init skipped (BLE_RADIO_INIT_ENABLE=0)");
    }
#endif
    if (initState == BleInitState::Ready && !loggedReady) {
        loggedReady = true;
        Serial.println("[BLE] GATT service registered, advertising as \""
                       BLE_DEVICE_NAME "\"");
        Serial.println("[BLE]   Service: " BLE_SERVICE_UUID);
        Serial.println("[BLE]   TX (notify): " BLE_CHAR_UUID_TX);
        Serial.println("[BLE]   RX (write):  " BLE_CHAR_UUID_RX);
    }
    if (initState == BleInitState::Failed && !loggedFailed) {
        loggedFailed = true;
        Serial.print("[BLE] Init failed, error=");
        Serial.println(_initError);
    }

    const BleConnEvent connEv = takeConnEvent();
    if (connEv == BleConnEvent::Connected) {
        Serial.println("[BLE] Connected");
    } else if (connEv == BleConnEvent::Disconnected) {
        Serial.println("[BLE] Disconnected — advertising restarted");
    }
#else
    (void)nowMs;
#endif
}

void BleChannelTask::onConnect() {
    _connected = true;
    _connEvent = BleConnEvent::Connected;
}

void BleChannelTask::onDisconnect() {
    _connected = false;
    _currentMtu = BLE_ATT_PAYLOAD_DEFAULT;
    _connEvent = BleConnEvent::Disconnected;
}

void BleChannelTask::onDataReceived(const uint8_t* data, size_t len) {
    (void)data;
    (void)len;
    if (!_cmdOutputQueue) return;
    // Stage 4: IES frame parser → push Command to Gateway
}
