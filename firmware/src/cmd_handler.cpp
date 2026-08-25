/**
 * @file    cmd_handler.cpp
 * @brief   Command handler task implementation.
 *
 * Ground truth for side-effects: iES_v0.3-master/ies_app/ies_task.cpp::btspp_recv_task_fxn()
 */

#include "cmd_handler.h"
#include "gateway.h"
#include "eeg.h"
#include "runtime_state.h"
#include "persistent_config.h"
#include "uart_channel.h"
#include "iesUtilities/ies_checksum.h"
#include "ADS1299_Library.h"
#include <chrono>
#include <string.h>


// ═════════════════════════════════════════════════════════════════════════════
// Global instance
// ═════════════════════════════════════════════════════════════════════════════

CommandHandlerTask cmdHandlerTask;


// ═════════════════════════════════════════════════════════════════════════════
// CommandHandlerTask implementation
// ═════════════════════════════════════════════════════════════════════════════

CommandHandlerTask::CommandHandlerTask()
    : BaseTask(osPriorityNormal, STACK_SIZE_CMD_HANDLER),
      _responseQueue(nullptr)
{
    _cmdQueue.setOwner(this);
}

void CommandHandlerTask::run() {
    #ifdef DEBUG_ENABLE
        static uint32_t dbgLoopCount         = 0;
        static uint32_t dbgLastHealthTime    = millis() + DBG_STAGGER_CMDHDLR_MS;
        static uint32_t dbgMaxLoopDurationMs = 0;
        static const uint32_t DBG_HEALTH_INTERVAL_MS = 5000;
    #endif

    while (!_stopRequested) {
        #ifdef DEBUG_ENABLE
            uint32_t dbgLoopStart = millis();
        #endif

        Command cmd;
        if (_cmdQueue.pop(cmd)) {
            Response resp;
            resp.cmd_id     = cmd.cmd_id;
            resp.status     = CmdStatus::OK;
            resp.payload_len = 0;
            resp.dest       = cmd.source;

            #if DBG_CMD
        {
            char dbgRx[64];
            snprintf(dbgRx, sizeof(dbgRx),
                "[CMD] >> 0x%02X plen=%u src=%s\r\n",
                (unsigned)cmd.cmd_id, (unsigned)cmd.payload_len,
                (cmd.source == CmdSource::UART) ? "UART" : "BLE");
            debugTryPrint(dbgRx);
        }
        #endif

            executeCommand(cmd, resp);

        #if DBG_CMD
        {
            char dbgTx[64];
            snprintf(dbgTx, sizeof(dbgTx),
                "[CMD] << 0x%02X status=0x%02X plen=%u\r\n",
                (unsigned)resp.cmd_id, (unsigned)resp.status,
                (unsigned)resp.payload_len);
            debugTryPrint(dbgTx);
        }
        #endif

            if (_responseQueue) {
                _responseQueue->push(resp);
            }
        } else {
            sleepUntilNotified(10);
        }

        #ifdef DEBUG_ENABLE
        {
            uint32_t loopDuration = millis() - dbgLoopStart;
            if (loopDuration > dbgMaxLoopDurationMs) dbgMaxLoopDurationMs = loopDuration;
            dbgLoopCount++;
            uint32_t now = millis();
            if (now - dbgLastHealthTime >= DBG_HEALTH_INTERVAL_MS) {
                char dbgBuf[80];
                snprintf(dbgBuf, sizeof(dbgBuf),
                    "[CMDHDLR] LoopRate: %lu iter/s | MaxLoopMs: %lu\r\n",
                    (dbgLoopCount * 1000UL) / (now - dbgLastHealthTime),
                    (unsigned long)dbgMaxLoopDurationMs);
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

void CommandHandlerTask::startStreaming() {
    eegAcquisitionTask.setSampleRate(
        (ADS1299_Library::SAMPLE_RATE)g_runtimeState.getSampleRate());
    g_runtimeState.applyToHardware(&ads1299);
    ads1299.startADS();
    g_runtimeState.setStreamingEnabled(true);
}

void CommandHandlerTask::stopStreaming() {
    ads1299.stopADS();
    g_runtimeState.setStreamingEnabled(false);
}


// ═════════════════════════════════════════════════════════════════════════════
// Command dispatch
// ═════════════════════════════════════════════════════════════════════════════

void CommandHandlerTask::executeCommand(const Command& cmd, Response& resp) {
    // While the ADS1299 is actively streaming, only STOP, link keepalive, and
    // time sync are permitted.  All config/status queries must be done before
    // START or after STOP.  This keeps the command path minimal during EEG TX
    // and prevents SPI register writes from contending with EegAcquisitionTask.
    if (g_runtimeState.isStreamingEnabled()) {
        switch (cmd.cmd_id) {
            case CMD_STOP_STREAMING:  // always allowed — stops the stream
            case CMD_HEARTBEAT:       // keepalive, no state change
            case CMD_TIME_SYNC:       // epoch sync during recording
                break;  // fall through to dispatch below
            default:
                resp.status = CmdStatus::ERR_NOT_ALLOWED;
                return;
        }
    }

    switch (cmd.cmd_id) {
        case CMD_START_STREAMING:  cmdStartStreaming(cmd, resp);  break;
        case CMD_STOP_STREAMING:   cmdStopStreaming(cmd, resp);   break;
        case CMD_TIME_SYNC:        cmdTimeSync(cmd, resp);        break;
        case CMD_HEARTBEAT:        cmdHeartbeat(cmd, resp);       break;
        case CMD_UART_PRINT_SEL:   cmdUartPrintSel(cmd, resp);   break;
        case CMD_DOWNSAMPLING:     cmdDownsampling(cmd, resp);    break;
        case CMD_IMPEDANCE_ON:     cmdImpedanceOn(cmd, resp);     break;
        case CMD_IMPEDANCE_OFF:    cmdImpedanceOff(cmd, resp);    break;
        case CMD_SOFT_RESET:       cmdSoftReset(cmd, resp);       break;
        case CMD_SET_ODR:          cmdSetOdr(cmd, resp);          break;
        case CMD_SET_GAIN:         cmdSetGain(cmd, resp);         break;
        case CMD_SET_OUTPUT_MODE:  cmdSetOutputMode(cmd, resp);   break;
        case CMD_SET_CHANNEL_MASK: cmdSetChannelMask(cmd, resp);  break;
        case CMD_SET_HOST_MODE:    cmdSetHostMode(cmd, resp);     break;
        case CMD_ENABLE_UART:      cmdEnableUart(cmd, resp);      break;
        case CMD_ENABLE_BLE:       cmdEnableBle(cmd, resp);       break;
        case CMD_QUERY_STATUS:     cmdQueryStatus(cmd, resp);     break;
        case CMD_SAVE_CONFIG:      cmdSaveConfig(cmd, resp);      break;
        case CMD_GET_VERSION:      cmdGetVersion(cmd, resp);      break;
        case CMD_DEMO:             cmdDemo(cmd, resp);             break;
        case CMD_RESET:            cmdReset(cmd, resp);            break;
        default:
            resp.status = CmdStatus::ERR_UNKNOWN;
            break;
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// iES bare-byte command handlers
// ═════════════════════════════════════════════════════════════════════════════

void CommandHandlerTask::cmdStartStreaming(const Command& cmd, Response& resp) {
    // Apply all pending runtime settings to hardware before starting the ADS.
    // cmdSetOdr (and other config commands) update RuntimeState only; the
    // hardware registers are written here so we never touch CONFIG1/GAIN/etc.
    // while the EegAcquisitionTask is alive and calling updateChannelData().
    startStreaming();
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdStopStreaming(const Command& cmd, Response& resp) {
    stopStreaming();
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdTimeSync(const Command& cmd, Response& resp) {
    // Payload length branches on protocol mode (see uart_channel.cpp
    // payloadLenForCmd()):
    //   4 bytes — legacy (OpenVIBE): epoch only, no CRC. OpenVIBE's
    //             resetBoard() builds a 6-byte buffer ending in '\0' and
    //             sends it via strlen(), so the CRC byte never reaches the
    //             wire (ovasCDriveriES.cpp). "Device response: None" for 't'
    //             per the original iES contract — CRC is meaningless here.
    //   5 bytes — modern: [epoch_BE 4B][CRC-8 1B], validated as before.
    uint32_t epoch;

    if (cmd.payload_len == 4) {
        epoch =
            ((uint32_t)cmd.payload[0] << 24) |
            ((uint32_t)cmd.payload[1] << 16) |
            ((uint32_t)cmd.payload[2] <<  8) |
             (uint32_t)cmd.payload[3];
    } else if (cmd.payload_len == 5) {
        // Verify CRC-8/SHT75 over the 4 epoch bytes
        uint8_t computed_crc = ies_crc8_sht75(cmd.payload, 4);

        // ── DEBUG: print received bytes and CRC comparison ─────────────────
        // Gated by DBG_CMD (DEBUG_ENABLE + DEBUG_COMMAND category).
        #if DBG_CMD
        {
            char dbg[96];
            snprintf(dbg, sizeof(dbg),
                "[CRC_DBG] payload=%02X%02X%02X%02X rx_crc=%02X fw_crc=%02X %s\r\n",
                cmd.payload[0], cmd.payload[1], cmd.payload[2], cmd.payload[3],
                cmd.payload[4], computed_crc,
                (computed_crc == cmd.payload[4]) ? "OK" : "BAD");
            debugTryPrint(dbg);
        }
        #endif
        // ────────────────────────────────────────────────────────────────────

        if (computed_crc != cmd.payload[4]) {
            resp.status = CmdStatus::ERR_BAD_CRC;
            return;
        }

        epoch =
            ((uint32_t)cmd.payload[0] << 24) |
            ((uint32_t)cmd.payload[1] << 16) |
            ((uint32_t)cmd.payload[2] <<  8) |
             (uint32_t)cmd.payload[3];
    } else {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }

    (void)epoch;  // store in RTC when RTC driver is available

    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdHeartbeat(const Command& cmd, Response& resp) {
    // No-op keepalive — iES host sends '.' periodically to check link
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdUartPrintSel(const Command& cmd, Response& resp) {
    // Payload byte: 'o' = switch to iES µV output mode; 'i' = raw/OpenBCI mode
    // iES reference: 'p'+'o' → OUTPUT_IES; 'p'+'i' → OUTPUT_OPENBCI
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    uint8_t sub = cmd.payload[0];
    if (sub == 'o') {
        g_runtimeState.setOutputMode(OutputMode::UV);
    } else if (sub == 'i') {
        g_runtimeState.setOutputMode(OutputMode::RAW);
    }
    // Other sub-commands (log category switches) are no-ops on Nicla
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdDownsampling(const Command& cmd, Response& resp) {
    // Payload: 1-byte factor (1 = no downsampling, 4 = default iES)
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    uint8_t factor = cmd.payload[0];
    if (factor == 0) factor = 1;

    // Legacy (OpenVIBE) guard rail: the ADC already runs at true 250 SPS in
    // legacy mode (see enforceLegacyDefaults()) — any decimation on top of
    // that would desync OpenVIBE's fixed 250 Hz sample-rate declaration.
    if (g_runtimeState.isOpenVibeMode() && factor != 1) {
        resp.status = CmdStatus::ERR_NOT_ALLOWED;
        return;
    }

    g_runtimeState.setDownsamplingFactor(factor);
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdImpedanceOn(const Command& cmd, Response& resp) {
    // Enable lead-off detection on the ADS1299
    // iES reference: sets LOFF register bits and switches to ADSINPUT_LOFF
    g_runtimeState.setImpedanceCheckEnabled(true);
    // TODO: configure ADS1299 LOFF registers when lead-off driver is implemented
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdImpedanceOff(const Command& cmd, Response& resp) {
    g_runtimeState.setImpedanceCheckEnabled(false);
    // TODO: restore ADSINPUT_NORMAL on active channels
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdSoftReset(const Command& cmd, Response& resp) {
    // iES 'v' sends the OpenBCI banner string then resets
    // Nicla: stop streaming, emit banner, then trigger NVIC reset
    stopStreaming();

    // OpenBCI-compatible banner (raw write, bypasses packetiser)
    Serial.println("OpenBCI V3 8bit Board");

    resp.status = CmdStatus::OK;

    // Defer NVIC reset until after response is sent (caller yields after push)
    // NVIC_SystemReset();  // uncomment when immediate reset is desired
}


// ═════════════════════════════════════════════════════════════════════════════
// Nicla binary command handlers
// ═════════════════════════════════════════════════════════════════════════════

void CommandHandlerTask::cmdSetOdr(const Command& cmd, Response& resp) {
    // Payload: 1-byte ADS1299_Library::SAMPLE_RATE enum value
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    uint8_t odr = cmd.payload[0];
    if (odr > (uint8_t)ADS1299_Library::SAMPLE_RATE_250) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }

    // Legacy (OpenVIBE) guard rail: 115200 baud only sustains 250 SPS × 2ch.
    // Anything above SAMPLE_RATE_250 would overrun the wire.
    if (g_runtimeState.isOpenVibeMode() && odr != (uint8_t)ADS1299_Library::SAMPLE_RATE_250) {
        resp.status = CmdStatus::ERR_NOT_ALLOWED;
        return;
    }

    g_runtimeState.setSampleRate(odr);
    // Hardware ODR is applied lazily at stream-start (cmdStartStreaming) to
    // avoid writing ADS1299 CONFIG1 while EegAcquisitionTask may be on the bus.
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdSetGain(const Command& cmd, Response& resp) {
    // Payload: [channel 1-4][gain code ADS_GAINxx]
    if (cmd.payload_len < 2) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    uint8_t channel = cmd.payload[0];
    uint8_t gain    = cmd.payload[1];
    if (!g_runtimeState.setChannelGain(channel, gain)) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    g_runtimeState.applyToHardware(&ads1299);
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdSetOutputMode(const Command& cmd, Response& resp) {
    // Payload: 1B — 0=raw ADC (OpenBCI), 1=µV integers (iES)
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    OutputMode mode = (cmd.payload[0] == 0) ? OutputMode::RAW : OutputMode::UV;

    // Legacy (OpenVIBE) guard rail: CDriveriES never rescales — it treats
    // every sample as an iES µV integer. Switching to RAW would silently
    // desync the driver's own (unused) unit conversion.
    if (g_runtimeState.isOpenVibeMode() && mode == OutputMode::RAW) {
        resp.status = CmdStatus::ERR_NOT_ALLOWED;
        return;
    }

    g_runtimeState.setOutputMode(mode);
    // NOTE: no PersistentConfig::save() here — flash writes block the command
    // handler for 100–500 ms, causing timeouts on back-to-back commands.
    // Persistence is triggered explicitly by CMD_SAVE_CONFIG (0x31).
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdSetChannelMask(const Command& cmd, Response& resp) {
    // Payload: 1B bitmask — bit0=CH1 … bit3=CH4
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    uint8_t mask = cmd.payload[0] & 0x0F;

    // Legacy (OpenVIBE) guard rail: CDriveriES hardcodes EEGValueCountPerSample
    // = 2 and never reads the type/channel nibble — any mask other than
    // exactly 2 active channels would desync its fixed-format parser.
    if (g_runtimeState.isOpenVibeMode()) {
        uint8_t popcount = 0;
        for (uint8_t i = 0; i < 4; i++) {
            if ((mask >> i) & 0x01) popcount++;
        }
        if (popcount != 2) {
            resp.status = CmdStatus::ERR_BAD_PAYLOAD;
            return;
        }
    }

    g_runtimeState.setChannelEnableMask(mask);
    g_runtimeState.applyToHardware(&ads1299);
    // NOTE: no PersistentConfig::save() here — see cmdSetOutputMode comment.
    // Persistence is triggered explicitly by CMD_SAVE_CONFIG (0x31).
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::enforceLegacyDefaults() {
    // Auto-normalize once so a stale mask/output-mode/ODR/downsampling from a
    // previous session can't silently desync OpenVIBE's fixed 2-channel µV
    // parser or overrun the 115200 baud budget.
    uint8_t mask = g_runtimeState.getChannelEnableMask();
    uint8_t popcount = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if ((mask >> i) & 0x01) popcount++;
    }
    if (popcount != 2) {
        g_runtimeState.setChannelEnableMask(0b00001100);  // factory default: CH3+CH4
        g_runtimeState.applyToHardware(&ads1299);
    }
    g_runtimeState.setOutputMode(OutputMode::UV);

    // 115200 baud tops out at ~250 SPS × 2ch of iES µV frames. Run the ADC
    // directly at 250 SPS with no decimation, rather than 1 kSPS ÷ DS×4
    // (same throughput, but avoids the wasted oversampling and the resulting
    // mismatch between the ADS1299's digital filter and the 250 Hz rate
    // OpenVIBE is configured to expect).
    g_runtimeState.setSampleRate((uint8_t)ADS1299_Library::SAMPLE_RATE_250);
    g_runtimeState.setDownsamplingFactor(1);
}

void CommandHandlerTask::cmdSetHostMode(const Command& cmd, Response& resp) {
    // Payload: 1B — 0=MODERN, 1=LEGACY_IES
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    HostProtocolMode mode = (cmd.payload[0] == 0) ? HostProtocolMode::MODERN
                                                   : HostProtocolMode::OPENVIBE;
    g_runtimeState.setHostProtocolMode(mode);

    if (mode == HostProtocolMode::OPENVIBE) {
        enforceLegacyDefaults();
    }

    // Arm the live baud-rate switch. PacketiserTask always forces this ack
    // through (even in legacy mode — see packetiser.cpp), and UartChannelTask
    // only applies the new baud once its TX queue is empty, so the host is
    // guaranteed to receive this OK at the OLD baud before reconnecting.
    uartChannelTask.requestBaudChange(
        (mode == HostProtocolMode::OPENVIBE) ? SERIAL_BAUD_OPENVIBE : SERIAL_BAUD_MODERN);

    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdEnableUart(const Command& cmd, Response& resp) {
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    gatewayTask.setUartEnabled(cmd.payload[0] != 0);
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdEnableBle(const Command& cmd, Response& resp) {
    if (cmd.payload_len < 1) {
        resp.status = CmdStatus::ERR_BAD_PAYLOAD;
        return;
    }
    gatewayTask.setBleEnabled(cmd.payload[0] != 0);
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdQueryStatus(const Command& cmd, Response& resp) {
    // 11-byte status response (dev_log.md §4.2):
    //   [0]   streaming:          0/1
    //   [1]   sample_rate:        ADS1299_Library::SAMPLE_RATE enum value
    //   [2]   downsampling:       factor
    //   [3]   channel_mask:       bitmask
    //   [4]   output_mode:        0=raw, 1=µV
    //   [5]   impedance_active:   0/1
    //   [6]   gain_ch1:           ADS_GAINxx code
    //   [7]   gain_ch2:           ADS_GAINxx code
    //   [8]   gain_ch3:           ADS_GAINxx code
    //   [9]   gain_ch4:           ADS_GAINxx code
    //   [10]  host_protocol_mode: 0=MODERN, 1=LEGACY_IES

    resp.payload[0] = g_runtimeState.isStreamingEnabled() ? 1 : 0;
    resp.payload[1] = g_runtimeState.getSampleRate();
    resp.payload[2] = g_runtimeState.getDownsamplingFactor();
    resp.payload[3] = g_runtimeState.getChannelEnableMask();
    resp.payload[4] = (uint8_t)g_runtimeState.getOutputMode();
    resp.payload[5] = g_runtimeState.isImpedanceCheckEnabled() ? 1 : 0;
    for (uint8_t i = 0; i < RuntimeState::NUM_CHANNELS; i++) {
        resp.payload[6 + i] = g_runtimeState.getChannelGain(i + 1);
    }
    resp.payload[10] = (uint8_t)g_runtimeState.getHostProtocolMode();
    resp.payload_len = 11;
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdSaveConfig(const Command& cmd, Response& resp) {
    // Explicitly flush current RuntimeState to EEPROM
    PersistentConfig::save(g_runtimeState);
    resp.status = CmdStatus::OK;
}

void CommandHandlerTask::cmdGetVersion(const Command& cmd, Response& resp) {
    // Return firmware version as 3 payload bytes: [major][minor][patch]
    // The version string is FW_VERSION_STR (e.g. "v0.2.0"), declared in config.h.
    resp.payload[0]  = FW_VERSION_MAJOR;
    resp.payload[1]  = FW_VERSION_MINOR;
    resp.payload[2]  = FW_VERSION_PATCH;
    resp.payload_len = 3;
    resp.status      = CmdStatus::OK;
}


// ═════════════════════════════════════════════════════════════════════════════
// Text keyword command handlers
// ═════════════════════════════════════════════════════════════════════════════

void CommandHandlerTask::cmdDemo(const Command& cmd, Response& resp) {
    // DEMO mode — runtime only, never persisted.
    //
    // 1. Stop any active stream.
    // 2. Reload default RuntimeState (CH3+CH4, gain×1, 1 kSPS, DS×4, iES µV).
    // 3. Set channel input types to ADSINPUT_TESTSIG via RuntimeState.
    // 4. Apply full state to hardware (POWER_DOWN, GAIN_SET, INPUT_TYPE_SET).
    // 5. Start streaming.
    //
    // payload[0]: 0=use source transport | 1=UART | 2=BLE

    // ── 1. Stop streaming ────────────────────────────────────────────────────
    stopStreaming();

    // ── 2. Load default settings ─────────────────────────────────────────────
    // initialize() resets _inputType[] to ADSINPUT_NORMAL for all channels.
    // NOTE: deliberately NOT calling PersistentConfig::save() — demo is transient.
    // Preserve host_protocol_mode across the reset: initialize() would
    // otherwise silently flip it back to MODERN without switching the
    // physical baud back, leaving RuntimeState and Serial out of sync.
    HostProtocolMode preservedHostMode = g_runtimeState.getHostProtocolMode();
    g_runtimeState.initialize();
    g_runtimeState.setHostProtocolMode(preservedHostMode);
    if (preservedHostMode == HostProtocolMode::OPENVIBE) {
        // initialize() reset sample rate/downsampling to the 1 kSPS/DS×4
        // factory default — re-apply the true-250-SPS legacy requirement.
        enforceLegacyDefaults();
    }

    // ── 3. Route active channels to the internal test signal ─────────────────
    // CONFIG2: enable internal 1× amplitude slow-pulse (≈1 Hz) square wave.
    ads1299.configureInternalTestSignal(ADSTESTSIG_AMP_1X, ADSTESTSIG_PULSE_SLOW);

    // Set INPUT_TYPE via RuntimeState so the state is visible and consistent.
    uint8_t mask = g_runtimeState.getChannelEnableMask();
    for (uint8_t i = 0; i < RuntimeState::NUM_CHANNELS; i++) {
        uint8_t itype = ((mask >> i) & 0x01) ? ADSINPUT_TESTSIG : ADSINPUT_NORMAL;
        g_runtimeState.setChannelInputType(i + 1, itype);
    }

    // ── 4+5. Apply full state to hardware and start streaming ────────────────
    // startStreaming() writes POWER_DOWN, GAIN_SET, and INPUT_TYPE_SET, then
    // starts the ADS and sets the streaming flag.
    startStreaming();

    resp.status = CmdStatus::OK;
    if (cmd.payload_len >= 1 && cmd.payload[0] == 1) {
        resp.dest = CmdSource::UART;
    } else if (cmd.payload_len >= 1 && cmd.payload[0] == 2) {
        resp.dest = CmdSource::BLE;
    }
}

void CommandHandlerTask::cmdReset(const Command& cmd, Response& resp) {
    // Full hardware reboot via ARM Cortex-M NVIC reset vector.
    // The response is queued but may not transmit before the chip resets —
    // this is acceptable for a deliberate reset command.
    resp.status = CmdStatus::OK;

    // Stop ADS1299 cleanly before reset to avoid leaving SPI bus in an
    // undefined state after the MCU comes back up.
    stopStreaming();

    NVIC_SystemReset();  // does not return
}
