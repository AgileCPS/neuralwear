/**
 * @file    eeg.cpp
 * @brief   EEG data acquisition task implementation for Arduino Nicla Voice.
 */

#include "config.h"
#include "eeg.h"
#include "ADS1299_Library.h"
#include <Arduino.h>


// ═════════════════════════════════════════════════════════════════════════════
// Global Instances
// ═════════════════════════════════════════════════════════════════════════════

// Instantiation of the SPI EEG data buffer (depth = FIFO_DEPTH_EEG_BLE from config.h).
EegFifo<FIFO_DEPTH_EEG_BLE> spiEegBuffer;

// Global EEG acquisition task instance
EegAcquisitionTask eegAcquisitionTask;


// ═════════════════════════════════════════════════════════════════════════════
// EegAcquisitionTask Implementation
// ═════════════════════════════════════════════════════════════════════════════

EegAcquisitionTask::EegAcquisitionTask()
    : ProducerTask<ADS1299_4_Sample>(TASK_PRIORITY_ACQUISITION, STACK_SIZE_ACQUISITION),
      _ads(ads1299),         // Bind to global ads1299 instance
      _drdySemaphore(0, 1),  // Initial count = 0, max count = 1 (binary semaphore)
      _sampleCounter(0)
{
    // No additional initialization needed
}


void EegAcquisitionTask::signalDataReady() {
    // Release the semaphore to wake the acquisition thread
    // This is called from ISR context, so use the ISR-safe release() method
    _drdySemaphore.release();
}


void EegAcquisitionTask::run() {
    // Main acquisition loop — runs at osPriorityRealtime
    #ifdef DEBUG_ENABLE
        static uint32_t dbgLoopCount         = 0;
        static uint32_t dbgLastHealthTime    = millis();
        static uint32_t dbgMaxLoopDurationMs = 0;
        static uint32_t dbgLastDropCount     = 0;   // tracks EEG→PacketiserTask queue drops
        static const uint32_t DBG_HEALTH_INTERVAL_MS = 1000;
    #endif

    while (!_stopRequested) {
        #ifdef DEBUG_ENABLE
            uint32_t dbgLoopStart = millis();
        #endif

        // 1. Wait for DRDY ISR to signal new data
        //    This blocks until the semaphore is released by signalDataReady()
        _drdySemaphore.acquire();

        // Check if stop was requested while we were blocked
        if (_stopRequested) {
            break;
        }

        // 2. Read sample from ADS1299 via SPI
        //    updateChannelData() performs the SPI transaction and updates
        //    _ads.boardChannelDataInt[0-3] with the 4-channel data
        _ads.updateChannelData();

        // 3. Package the sample into ADS1299_4_Sample struct
        ADS1299_4_Sample sample;
        sample.channel[0]     = _ads.boardChannelDataInt[0];  // CH1
        sample.channel[1]     = _ads.boardChannelDataInt[1];  // CH2
        sample.channel[2]     = _ads.boardChannelDataInt[2];  // CH3
        sample.channel[3]     = _ads.boardChannelDataInt[3];  // CH4
        sample.sample_number  = _sampleCounter++;

        // 4. Distribute to all subscribed consumers (fan-out via ProducerTask)
        //    This calls IQueue::push() for each subscriber queue.
        //    push() is mutex-guarded; never waits for space (drop-oldest on full).
        distribute(sample);

        // ─────────────────────────────────────────────────────────────────────
        // 5. Loop metrics (DEBUG_ENABLE, every 5 s)
        //    LoopRate ≈ effective sample rate; MaxLoopMs = worst-case
        //    sample-to-sample gap (includes DRDY wait + SPI + distribute).
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
                // Dropped samples = new evictions on the EEG→PacketiserTask queue
                uint32_t currentDrops = _subscribers.empty()
                                            ? 0 : _subscribers[0]->droppedCount();
                uint32_t droppedSince  = currentDrops - dbgLastDropCount;
                dbgLastDropCount       = currentDrops;

                char dbgBuf[256];
                int  pos = 0;
                pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                    "[EEG] LoopRate: %lu iter/s | MaxLoopMs: %lu\n"
                    "[EEG] StatBegin:\n",
                    (dbgLoopCount * 1000UL) / (now - dbgLastHealthTime),
                    (unsigned long)dbgMaxLoopDurationMs);

                // EEG → PacketiserTask queue (subscribers[0])
                if (!_subscribers.empty() && pos < (int)sizeof(dbgBuf)) {
                    pos += snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                        "  EEG→PacketiserTask queue: %u/%u | Drops: %lu\n",
                        (unsigned)_subscribers[0]->size(),
                        (unsigned)_subscribers[0]->capacity(),
                        (unsigned long)droppedSince);
                }

                if (pos < (int)sizeof(dbgBuf)) {
                    snprintf(dbgBuf + pos, sizeof(dbgBuf) - pos,
                        "[EEG] StatEnd.\n");
                }
                debugTryPrint(dbgBuf);
                dbgLoopCount         = 0;
                dbgLastHealthTime    = now;
                dbgMaxLoopDurationMs = 0;
            }
        }
        #endif
    }

    // Thread exits cleanly when _stopRequested is set by stop()
}


void EegAcquisitionTask::setSampleRate(ADS1299_Library::SAMPLE_RATE rate) {
    _ads.setSampleRate((uint8_t)rate);
}


const char* EegAcquisitionTask::getSampleRate() const {
    return _ads.getSampleRate();
}
