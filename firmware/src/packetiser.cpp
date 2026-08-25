/**
 * @file    packetiser.cpp
 * @brief   PacketiserTask implementation.
 */

#include "packetiser.h"
#include "eeg.h"    // eegAcquisitionTask._sampleCounter — read by serialiseTimeSync
#include "gateway.h"
#include "runtime_state.h"
#include "cmd_handler.h"  // CMD_SET_HOST_MODE — always emitted even in legacy mode
#include "iesUtilities/ies_channel_select.h"
#include "iesUtilities/ies_packet_format.h"
#include <cmath>   // roundf()

#define TIME_SYNC_INTERVAL_MS  1000  // send TIME_SYNC frame every 1 s


namespace {

FrameDest cmdSourceToFrameDest(CmdSource src) {
    return (src == CmdSource::BLE) ? FrameDest::BLE : FrameDest::UART;
}

FrameDest activeStreamDest() {
    return gatewayTask.isBleEnabled() ? FrameDest::BLE : FrameDest::UART;
}

}  // namespace


// =============================================================================
// Global instance
// =============================================================================

PacketiserTask packetiserTask;


// =============================================================================
// PacketiserTask implementation
// =============================================================================

PacketiserTask::PacketiserTask()
    : ProducerTask(osPriorityAboveNormal, STACK_SIZE_PACKETISER),
      _frameCnt(0),
      _lastSyncTime_ms(0),
      _sampleCount(0)
{
    // Wire all three input queues to this task so any producer
    // unblocks run() immediately after a push().
    _eegQueue.setOwner(this);
    _responseQueue.setOwner(this);
    _mlQueue.setOwner(this);
}

void PacketiserTask::run() {
    #ifdef DEBUG_ENABLE
        static uint32_t dbgEegCount    = 0;
        static uint32_t dbgRespCount   = 0;
        static uint32_t dbgMlCount     = 0;
        static uint32_t dbgSyncCount   = 0;
        static uint32_t dbgLoopCount   = 0;
        static uint32_t dbgMaxLoopMs   = 0;
        static uint32_t dbgLastReport  = millis();
        static const uint32_t DBG_INTERVAL_MS = 1000;
    #endif

    while (!_stopRequested) {
        #ifdef DEBUG_ENABLE
            uint32_t dbgLoopStart = millis();
        #endif

        bool hasData = false;

        // ─────────────────────────────────────────────────────────────────────
        // 1. Periodic TIME_SYNC frame (inserted ahead of any data frame)
        //    Suppressed in legacy (OpenVIBE) mode — its fixed-format parser
        //    always expects a 2-channel EEG frame and never reads the type
        //    nibble, so a stray type-7 frame would desync it. The timer still
        //    advances so switching back to Modern mid-session doesn't burst-
        //    emit a backlog of overdue TIME_SYNC frames.
        // ─────────────────────────────────────────────────────────────────────
        uint32_t now = millis();
        if ((now - _lastSyncTime_ms) >= TIME_SYNC_INTERVAL_MS) {
            if (!g_runtimeState.isOpenVibeMode()) {
                WireFrame frame = serialiseTimeSync(micros(), eegAcquisitionTask.getSampleCounter());
                distribute(frame);
                hasData = true;
                #ifdef DEBUG_ENABLE
                    dbgSyncCount++;
                #endif
            }
            _lastSyncTime_ms = now;
        }

        // ─────────────────────────────────────────────────────────────────────
        // 2. Priority 1 — command responses (immediate delivery before EEG/ML)
        //    Suppressed in legacy mode — the original iES contract sends no
        //    acknowledgement byte for any command, and OpenVIBE's CDriveriES
        //    sends several commands fire-and-forget, so a stray type-6
        //    RESPONSE frame would land mid-stream and desync its parser.
        //    Exception: CMD_SET_HOST_MODE's own ack is always forced through
        //    — it's the only command a legacy (OpenVIBE) session can never
        //    send, so the modern tool that issued it still needs the ack
        //    before it reconnects at the new baud (see uart_channel.h).
        // ─────────────────────────────────────────────────────────────────────
        Response resp;
        if (_responseQueue.pop(resp)) {
            if (!g_runtimeState.isOpenVibeMode() || resp.cmd_id == CMD_SET_HOST_MODE) {
                WireFrame frame = serialiseResponse(resp);
                distribute(frame);
                #ifdef DEBUG_ENABLE
                    dbgRespCount++;
                #endif
            }
            hasData = true;
            continue;
        }

        // ─────────────────────────────────────────────────────────────────────
        // 3. Priority 2 — EEG samples (downsampling applied here)
        // ─────────────────────────────────────────────────────────────────────
        ADS1299_4_Sample eeg;
        if (_eegQueue.pop(eeg)) {
            hasData = true;
            uint32_t factor = g_runtimeState.getDownsamplingFactor();
            if (factor < 1) factor = 1;
            if ((_sampleCount++ % factor) == 0) {
                WireFrame frame = serialiseEeg(eeg);
                distribute(frame);
            }
            #ifdef DEBUG_ENABLE
                dbgEegCount++;
            #endif
            continue;
        }

        // ─────────────────────────────────────────────────────────────────────
        // 4. Priority 3 — ML results (future)
        // ─────────────────────────────────────────────────────────────────────
        MLOutput ml;
        if (_mlQueue.pop(ml)) {
            WireFrame frame = serialiseMl(ml);
            distribute(frame);
            hasData = true;
            #ifdef DEBUG_ENABLE
                dbgMlCount++;
            #endif
            continue;
        }

        // ─────────────────────────────────────────────────────────────────────
        // 5. No data — block until a queue notifies via INotifiable
        // ─────────────────────────────────────────────────────────────────────
        if (!hasData) {
            sleepUntilNotified(1);
        }

        #ifdef DEBUG_ENABLE
        {
            uint32_t loopDuration = millis() - dbgLoopStart;
            if (loopDuration > dbgMaxLoopMs) dbgMaxLoopMs = loopDuration;
            dbgLoopCount++;

            uint32_t debugNow = millis();
            if (debugNow - dbgLastReport >= DBG_INTERVAL_MS) {
                uint32_t elapsed = debugNow - dbgLastReport;
                char dbgBuf[384];
                int  pos = 0;
                pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                    "[PKT] LoopRate: %lu iter/s | MaxLoopMs: %lu\n"
                    "[PKT] StatBegin:\n"
                    "  Frames out: %lu total | EEG=%lu Resp=%lu ML=%lu TimeSync=%lu\n"
                    "  EEG queue: %u/%u | Drops: %lu\n"
                    "  Response queue: %u/%u | Drops: %lu\n"
                    "  ML queue: %u/%u | Drops: %lu\n"
                    "[PKT] StatEnd.\n",
                    (dbgLoopCount * 1000UL) / elapsed,
                    (unsigned long)dbgMaxLoopMs,
                    (unsigned long)(dbgEegCount + dbgRespCount + dbgMlCount + dbgSyncCount),
                    (unsigned long)dbgEegCount,
                    (unsigned long)dbgRespCount,
                    (unsigned long)dbgMlCount,
                    (unsigned long)dbgSyncCount,
                    (unsigned)_eegQueue.size(),      (unsigned)_eegQueue.capacity(),
                    (unsigned long)_eegQueue.droppedCount(),
                    (unsigned)_responseQueue.size(), (unsigned)_responseQueue.capacity(),
                    (unsigned long)_responseQueue.droppedCount(),
                    (unsigned)_mlQueue.size(),       (unsigned)_mlQueue.capacity(),
                    (unsigned long)_mlQueue.droppedCount());

                debugTryPrint(dbgBuf);

                dbgEegCount   = 0;
                dbgRespCount  = 0;
                dbgMlCount    = 0;
                dbgSyncCount  = 0;
                dbgLoopCount  = 0;
                dbgMaxLoopMs  = 0;
                dbgLastReport = debugNow;
            }
        }
        #endif
    }
}


// =============================================================================
// IES serialisation helpers
// =============================================================================

// EEG frame: [A0][cnt][type:EEG | num_selected_ch][ch0 3B]...[chN 3B][C0]
// Channel selection and µV conversion are driven by RuntimeState at call time.
// In iES-compatible mode (OutputMode::IES, mask=0b00001100): 10-byte frame, byte2=0x02.
WireFrame PacketiserTask::serialiseEeg(const ADS1299_4_Sample& s) {
    // Determine which channels to stream from the runtime mask
    channel_select_config_t chCfg;
    ies_channel_select_init_from_mask(&chCfg, g_runtimeState.getChannelEnableMask());

    // Clamp to the number of physical channels on ADS1299-4
    uint8_t numCh = chCfg.num_channels_to_stream;
    if (numCh == 0) numCh = 1;  // always emit at least one channel
    if (numCh > RuntimeState::NUM_CHANNELS) numCh = RuntimeState::NUM_CHANNELS;

    OutputMode mode = g_runtimeState.getOutputMode();

    WireFrame f;
    uint8_t* p = f.bytes;
    *p++ = IES_FRAME_START;
    *p++ = _frameCnt++;
    *p++ = (uint8_t)((IES_SAMPLE_EEG << 4) | (numCh & 0x0F));

    for (uint8_t slot = 0; slot < numCh; slot++) {
        uint8_t chIdx = chCfg.channel_indices[slot];  // 0-based hardware channel index
        if (chIdx >= RuntimeState::NUM_CHANNELS) chIdx = 0;

        int32_t value;
        if (mode == OutputMode::UV) {
            // Convert raw ADC to integer µV using per-channel gain.
            // ADS1299 gain register codes: bits[6:4] encode gain index 0–6.
            //   0x00=×1  0x10=×2  0x20=×4  0x30=×6  0x40=×8  0x50=×12  0x60=×24
            static const float gainMultiplier[] = {1.f, 2.f, 4.f, 6.f, 8.f, 12.f, 24.f};
            uint8_t gainCode  = g_runtimeState.getChannelGain(chIdx + 1);
            uint8_t gainIndex = (gainCode >> 4) & 0x07;
            float   gainMult  = (gainIndex < 7) ? gainMultiplier[gainIndex] : 1.f;
            float   uV        = (float)s.channel[chIdx] * (EEG_SCALE_UV / gainMult);
            int32_t rounded  = (int32_t)roundf(uV);
            // Clamp to 24-bit signed range
            if (rounded >  8388607) rounded =  8388607;
            if (rounded < -8388608) rounded = -8388608;
            value = rounded;
        } else {
            value = s.channel[chIdx];  // raw ADC pass-through
        }

        // Big-endian 24-bit
        *p++ = (uint8_t)((value >> 16) & 0xFF);
        *p++ = (uint8_t)((value >>  8) & 0xFF);
        *p++ = (uint8_t)((value)       & 0xFF);
    }

    *p++ = IES_FRAME_STOP;
    f.len = (uint8_t)(p - f.bytes);
    f.dest = activeStreamDest();
    return f;
}

// Response frame: [A0][cnt][0x6N][cmd_id][status][payload_len][payload...][C0]
// type nibble = 6 (Nicla RESPONSE — outside iES range so host ignores it);
// ch nibble carries payload_len.
WireFrame PacketiserTask::serialiseResponse(const Response& r) {
    WireFrame f;
    uint8_t* p = f.bytes;
    *p++ = IES_FRAME_START;
    *p++ = _frameCnt++;
    *p++ = (0x6 << 4) | (r.payload_len & 0x0F);
    *p++ = r.cmd_id;
    *p++ = (uint8_t)r.status;
    *p++ = r.payload_len;
    for (uint8_t i = 0; i < r.payload_len && i < IES_CMD_PAYLOAD_MAX; i++) {
        *p++ = r.payload[i];
    }
    *p++ = IES_FRAME_STOP;
    f.len = (uint8_t)(p - f.bytes);
    f.dest = cmdSourceToFrameDest(r.dest);
    return f;
}

// TIME_SYNC frame: [A0][cnt][0x71][ts_us 4B BE][sample_cnt 4B BE][C0] = 12 B
// type nibble = 7 (Nicla TIME_SYNC — outside iES range so host ignores it).
// Both 32-bit fields are big-endian to match iES byte-order convention.
WireFrame PacketiserTask::serialiseTimeSync(uint32_t ts_us, uint32_t sample_cnt) {
    WireFrame f;
    uint8_t* p = f.bytes;
    *p++ = IES_FRAME_START;
    *p++ = _frameCnt++;
    *p++ = (0x7 << 4) | 1;  // type=TIME_SYNC(7), payload_count=1
    *p++ = (ts_us >> 24) & 0xFF;
    *p++ = (ts_us >> 16) & 0xFF;
    *p++ = (ts_us >>  8) & 0xFF;
    *p++ = (ts_us)       & 0xFF;
    *p++ = (sample_cnt >> 24) & 0xFF;
    *p++ = (sample_cnt >> 16) & 0xFF;
    *p++ = (sample_cnt >>  8) & 0xFF;
    *p++ = (sample_cnt)       & 0xFF;
    *p++ = IES_FRAME_STOP;
    f.len = (uint8_t)(p - f.bytes);  // 12
    f.dest = activeStreamDest();
    return f;
}

// ML frame: [A0][cnt][0x81][label][confidence 4B LE][C0] = 9 B  (future)
// type nibble = 8 (Nicla ML_OUTPUT — outside iES range so host ignores it).
WireFrame PacketiserTask::serialiseMl(const MLOutput& m) {
    WireFrame f;
    uint8_t* p = f.bytes;
    *p++ = IES_FRAME_START;
    *p++ = _frameCnt++;
    *p++ = (0x8 << 4) | 1;  // type=ML_OUTPUT(8), payload_count=1
    *p++ = m.class_label;
    uint32_t conf;
    memcpy(&conf, &m.confidence, 4);
    *p++ = (conf)       & 0xFF;
    *p++ = (conf >>  8) & 0xFF;
    *p++ = (conf >> 16) & 0xFF;
    *p++ = (conf >> 24) & 0xFF;
    *p++ = IES_FRAME_STOP;
    f.len = (uint8_t)(p - f.bytes);  // 9
    f.dest = activeStreamDest();
    return f;
}
