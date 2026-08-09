/**
 * @file ADS1299NiclaFW.ino
 * @brief ADS1299 multi-task streaming architecture for Arduino Nicla Voice.
 *
 * See firmware_architecture.md for full architecture details.
 * See technical_notes.md NOTE-002 for DRDY interrupt design.
 */

#include "Nicla_System.h"      // nicla::begin() — Nicla peripherals init, enables VDDIO_EXT 3.3V output
#include "config.h"            // Centralized application configuration
#include "ADS1299_Library.h"   // ADS1299 driver (ported from iES_v0.3-master)
#include "eeg.h"               // ADS1299_4_Sample, EegAcquisitionTask, eegAcquisitionTask
#include "packetiser.h"        // PacketiserTask, packetiserTask
#include "gateway.h"           // GatewayTask, gatewayTask
#include "uart_channel.h"      // UartChannelTask, uartChannelTask
#include "ble_channel.h"       // BleChannelTask, bleChannelTask
#include "cmd_handler.h"       // CommandHandlerTask, cmdHandlerTask
#include "runtime_state.h"     // g_runtimeState
#include "persistent_config.h" // PersistentConfig::load/save/reset

// Expected ADS1299-4 device ID (0x3C)
static const byte EXPECTED_ID = ADS_ID;

// DRDY interrupt: signals EegAcquisitionTask when sample ready
void DRDY_ISR(void) {
    eegAcquisitionTask.signalDataReady();
}

void setup() {
    // Initialize Nicla Voice system (enables VDDIO_EXT 3.3V)
    nicla::begin();

    // ── Load persistent config BEFORE starting Serial ───────────────────────
    // Must happen first so the persisted HostProtocolMode (runtime_state.h)
    // picks the correct boot baud below — a unit left in OPENVIBE mode must
    // come up at SERIAL_BAUD_OPENVIBE, not the modern default. PersistentConfig
    // only touches FlashIAP, so it has no Serial dependency; the diagnostic
    // messages are simply deferred until after Serial.begin() (see below).

    // bool configLoadedFromEeprom = PersistentConfig::load(g_runtimeState);
    g_runtimeState.initialize();
    bool configLoadedFromEeprom = false;
    uint32_t bootBaud = g_runtimeState.isOpenVibeMode() ? SERIAL_BAUD_OPENVIBE : SERIAL_BAUD_MODERN;

    // Start USB serial (wait for connection up to timeout)
    Serial.begin(bootBaud);
    {
        unsigned long startMs = millis();
        while (!Serial && (millis() - startMs) < SERIAL_CONNECT_TIMEOUT_MS) {
            delay(SERIAL_CONNECT_POLL_MS);
        }
    }

    Serial.println();
    Serial.println("=== NICLA EEG Firmware - Multi-Task Architecture ===");
    Serial.println("Architecture: firmware_architecture.md");
    Serial.print("Host protocol mode: ");
    Serial.println(g_runtimeState.isOpenVibeMode() ? "OPENVIBE (115200 baud)" : "MODERN (460800 baud)");
    Serial.println();

    // Enable verbose ADS1299 driver logging
    ads1299.verbosity = true;

    // Report the persistent-config load result captured above (before Serial
    // was available) — see the reordering note at the top of setup().
    if (!configLoadedFromEeprom) {
        Serial.println("[CONFIG] EEPROM invalid or missing — factory defaults loaded.");
    } else {
        Serial.println("[CONFIG] Loaded from EEPROM.");
    }
    Serial.println();

    // Turn on battery charging (if USB connected) — Nicla System library handles the rest.
    Serial.println("Turning on battery charging at 200 mA (if USB connected).");
    nicla::enableCharging(200);  // 200 mA charge current

    // Initialize ADS1299 (SPI, reset, register config)
    Serial.println("Initialising ADS1299 ...");
    ads1299.initialize();
    Serial.println("Initialisation complete.");
    Serial.println();

    // Read and verify device ID
    Serial.println("Reading device ID ...");
    byte id = ads1299.ADS_getDeviceID(BOARD_ADS);

    Serial.print("Device ID read: 0x");
    Serial.println(id, HEX);
    Serial.println();

    if (id != EXPECTED_ID) {
        Serial.print("FAIL: Unexpected device ID 0x");
        Serial.print(id, HEX);
        Serial.print(" (expected 0x");
        Serial.print(EXPECTED_ID, HEX);
        Serial.println(").");
        Serial.println();
        Serial.println("Troubleshooting hints:");
        Serial.println("  • Check 3.3 V supply on AVDD / DVDD pins.");
        Serial.println("  • Verify wiring: CS=6 MISO=7 MOSI=8 SCK=9 RST=10.");
        Serial.println("  • Confirm SPI mode is Mode 1 (CPOL=0, CPHA=1).");
        Serial.println("  • This design uses ADS1299-4 (expected 0x3C).");
        Serial.println("  • 0xFF usually means MISO is floating (no device).");
        Serial.println("  • 0x00 usually means MISO is stuck low.");
        Serial.println();
        Serial.println("Streaming not started — fix hardware first.");
        return;  // stay in setup(); loop() will just blink LED
    }

    Serial.print("PASS: ADS1299 identified correctly (expected 0x");
    Serial.print(EXPECTED_ID, HEX);
    Serial.println(").");
    Serial.println();

    // Apply sample rate from PersistentConfig (loaded above); factory default is 1 kSPS.
    eegAcquisitionTask.setSampleRate(
        (ADS1299_Library::SAMPLE_RATE)g_runtimeState.getSampleRate());
    Serial.print("Sample rate configured: ");
    Serial.print(eegAcquisitionTask.getSampleRate());
    Serial.println(" SPS");
    Serial.println();

    // ── Apply persistent config to ADS1299 hardware ─────────────────────────
    // Sets channel power-down bits and gain registers from g_runtimeState.
    // Must be called after ads1299.initialize() and PersistentConfig::load().
    g_runtimeState.applyToHardware(&ads1299);

    // ── Channel input routing ───────────────────────────────────────────────
    // In production, g_runtimeState.applyToHardware() above already powers down
    // CH1/CH2 based on _channelEnableMask (0b00001100 by default).
    //
    // In debug mode, override all channel inputs to the ADS1299 internal test
    // signal generator so the full firmware pipeline can be exercised without
    // real hardware.
    #ifdef DEBUG_ENABLE
    ads1299.configureInternalTestSignal(ADSTESTSIG_AMP_2X, ADSTESTSIG_PULSE_FAST);
    g_runtimeState.setAllChannelInputTypes(ADSINPUT_TESTSIG);
    g_runtimeState.applyToHardware(&ads1299);
    Serial.println("Channel routing: Internal test signal (DEBUG_ENABLE — all channels).");
    #else
    Serial.println("Channel routing: Analog, CH3+CH4 active; CH1+CH2 powered down (production).");
    #endif  // DEBUG_ENABLE

    // Wire tasks together (subscription pattern)
    Serial.println("Wiring task architecture ...");
    
    // EegAcquisitionTask → PacketiserTask (EEG samples)
    eegAcquisitionTask.subscribe(packetiserTask.getEegQueue());
    
    // PacketiserTask → GatewayTask (IES wire frames)
    packetiserTask.subscribe(gatewayTask.getDataQueue());
    
    // GatewayTask → channel TX queues (unicast routing via WireFrame.dest)
    gatewayTask.setUartChannel(uartChannelTask.getTxQueue());
    gatewayTask.setBleChannel(bleChannelTask.getTxQueue());
    
    // UartChannelTask → GatewayTask (commands from UART)
    uartChannelTask.setCmdOutputQueue(gatewayTask.getUartCommandQueue());

#if FEATURE_BLE_ENABLE
    bleChannelTask.setCmdOutputQueue(gatewayTask.getBleCommandQueue());
#endif
    
    gatewayTask.setCmdHandlerQueue(cmdHandlerTask.getCommandQueue());
    
    // CommandHandlerTask → PacketiserTask (responses)
    cmdHandlerTask.setResponseQueue(packetiserTask.getResponseQueue());
    
    Serial.println("Task wiring complete.");
    Serial.println();

    // Attach DRDY interrupt (pin 11, falling edge)
    ads1299.verbosity = false;
    pinMode(ADS_DRDY_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(ADS_DRDY_PIN), DRDY_ISR, FALLING);
    Serial.println("DRDY interrupt attached on pin 11 (FALLING edge).");

    // Start tasks (lowest to highest priority)
    Serial.println("Starting tasks ...");
    
    cmdHandlerTask.start();
    Serial.println("  [1/6] CommandHandlerTask started (priority: Normal)");
    
    gatewayTask.start();
    Serial.println("  [2/6] GatewayTask started (priority: Normal)");

#if FEATURE_BLE_ENABLE
    bleChannelTask.start();
    Serial.println("  [3/6] BleChannelTask started (priority: Normal)");
#endif
    
    uartChannelTask.start();
    Serial.println("  [4/6] UartChannelTask started (priority: Normal)");
    
    packetiserTask.start();
    Serial.println("  [5/6] PacketiserTask started (priority: AboveNormal)");
    
    eegAcquisitionTask.start();
    Serial.println("  [6/6] EegAcquisitionTask started (priority: Realtime)");
    
    Serial.println();
    Serial.println("All tasks started successfully.");
    Serial.println();

    // Streaming always starts on explicit CMD_START_STREAMING ('b').
    // DEBUG_ENABLE only gates debug logging — do NOT auto-start here.
    // Auto-starting would fill the USB CDC TX buffer with EEG frames,
    // saturating availableForWrite() so that debugTryPrint() silently
    // drops all debug messages, and delaying command responses behind
    // the EEG frame backlog (~1.5 s per command).
    Serial.println("ADS1299 ready. Send 'b' to start streaming.");
#ifdef DEBUG_ENABLE
    Serial.println("System ready. Debug logging ENABLED.");
#else
    Serial.println("System ready. Waiting for stream-start command.");
#endif
    Serial.println("========================================");
#if FEATURE_BLE_ENABLE
    Serial.println("[BLE] Stage 2: init deferred to BleChannelTask::run()");
#if !BLE_RADIO_INIT_ENABLE
    Serial.println("[BLE] Radio OFF (BLE_RADIO_INIT_ENABLE=0) — task bring-up only");
#endif
#endif
    Serial.println();
}

void loop() {
    unsigned long now = millis();

#if FEATURE_BLE_ENABLE
    bleChannelTask.pollStatusLog(now);
#endif

    // ─────────────────────────────────────────────────────────────────────────
    // Heartbeat LED (1 Hz blink)
    // ─────────────────────────────────────────────────────────────────────────
    static unsigned long lastToggleMs = 0;
    static bool          ledOn        = false;

    if (now - lastToggleMs >= HEARTBEAT_LED_INTERVAL_MS) {
        lastToggleMs = now;
        ledOn        = !ledOn;
        nicla::leds.begin();
        nicla::leds.setColor(ledOn ? red : off);
        nicla::leds.end();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // System-wide memory health report (DEBUG_ENABLE, every 5 s)
    //
    // Reported here (main loop / idle task) rather than inside any RTOS task
    // because heap figures are system-wide — printing them once is enough.
    //
    // heapClaimed = bytes from mbed_heap_start to current _sbrk break.
    //   Includes: RTX thread stacks, live malloc/new, freed-but-retained blocks.
    //   Monotonically advancing — a stable reading means no leak.
    // heapFree    = arena bytes beyond the break (not yet touched).
    // ─────────────────────────────────────────────────────────────────────────
#ifdef DEBUG_ENABLE
    {
        static unsigned long lastMemReportMs = 0;
        if (now - lastMemReportMs >= 5000UL) {
            lastMemReportMs = now;
            uint32_t heapClaimed = (uint32_t)((char*)_sbrk(0) - (char*)mbed_heap_start);
            uint32_t heapFree    = (heapClaimed <= mbed_heap_size)
                                       ? (mbed_heap_size - heapClaimed) : 0;
            char dbgBuf[96];
            snprintf(dbgBuf, sizeof(dbgBuf),
                "[SYS] HeapFree: %lu B (claimed: %lu/%lu B)\r\n",
                (unsigned long)heapFree,
                (unsigned long)heapClaimed,
                (unsigned long)mbed_heap_size);
            debugTryPrint(dbgBuf);
        }
    }
#endif

    delay(10);
}
