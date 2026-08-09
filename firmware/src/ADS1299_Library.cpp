/**
 * @file ADS1299_Library.cpp
 * @brief ADS1299 driver — ported to Arduino Nicla Voice (nRF52832 / Mbed OS).
 *
 * PORTING CHANGES from iES_v0.3-master / ADS_1299_Library.cpp
 * ------------------------------------------------------------
 *  - Removed all <ti/sysbios/…> TI-RTOS kernel headers.
 *  - Removed <mqueue.h>, <pthread.h> POSIX headers.
 *  - Removed "ies_task.h", "ies_misc.h", "ies_debug.h" (TI-specific).
 *  - Removed boardBeginADSInterrupt() and its RTOS task / semaphore / mqueue.
 *  - boardBegin() simplified to just call boardReset().
 *  - Removed LIS3DH accelerometer methods.
 *  - Removed AGC (adaptive gain control) and cir_queue dependency.
 *  - IES_DEBUG_STATE / IES_PRINTF replaced by Serial.print().
 *  - printAll() / printlnAll() now call Serial.print().
 *  - pinMode(pin, mode, NULL) → pinMode(pin, mode)  (Arduino, 2 args).
 *  - csLow()  now calls SPI.beginTransaction(ADS_SPI_SETTINGS) first.
 *  - csHigh() now calls SPI.endTransaction() after releasing CS.
 *  - updateChannelData() retains only the non-buffered (interrupt-less) path.
 *  - All ADS1299 SPI protocol logic is unchanged.
 */

#include "ADS1299_Library.h"
#include <SPI.h>

/* ============================================================ */
/*  Constructor                                                 */
/* ============================================================ */

ADS1299_Library::ADS1299_Library() {
    initializeVariables();
}

/* ============================================================ */
/*  Private: initialise all member variables to safe defaults   */
/* ============================================================ */

void ADS1299_Library::initializeVariables(void) {
    channelDataAvailable = false;
    commandFromSPI       = false;
    daisyPresent         = false;
    streaming            = false;
    verbosity            = false;
    boardUseSRB1         = false;
    daisyUseSRB1         = false;
    isRunning            = false;
    firstDataPacket      = false;
    boardStat            = 0;
    numChannels          = 0;

    curSampleRate = SAMPLE_RATE_1000;
    curBoardMode  = BOARD_MODE_DEFAULT;
    curDebugMode  = DEBUG_MODE_OFF;
    curPacketType = PACKET_TYPE_ACCEL;

    memset(regData,              0, sizeof(regData));
    memset(boardChannelDataRaw,  0, sizeof(boardChannelDataRaw));
    memset(boardChannelDataInt,  0, sizeof(boardChannelDataInt));
    memset(channelSettings,      0, sizeof(channelSettings));
    memset(defaultChannelSettings, 0, sizeof(defaultChannelSettings));
    memset(leadOffSettings,      0, sizeof(leadOffSettings));

    for (int i = 0; i < OPENBCI_NUMBER_OF_CHANNELS_DAISY; i++) {
        useInBias[i] = false;
        useSRB2[i]   = false;
    }
}

/* ============================================================ */
/*  Board-level initialisation                                  */
/* ============================================================ */

void ADS1299_Library::begin(void) {
    boardBegin();
}

/**
 * Simplified boardBegin().
 * Original also called boardBeginADSInterrupt() which set up a TI-RTOS task,
 * binary semaphore, and POSIX message queue.  On the Nicla Voice the DRDY
 * interrupt is attached separately in the sketch when needed.
 */
boolean ADS1299_Library::boardBegin(void) {
    boardReset();
    return true;
}

void ADS1299_Library::boardReset(void) {
    initialize();
    delay(500);
    configureLeadOffDetection(LOFF_MAG_6NA, LOFF_FREQ_31p2HZ);
    ADS_getDeviceID(BOARD_ADS);
    delay(5);
}

/**
 * Set up the CS and RST GPIO pins, start the SPI peripheral, then
 * run the full ADS1299 register initialisation sequence.
 */
void ADS1299_Library::initialize(void) {
    pinMode(BOARD_ADS, OUTPUT);
    digitalWrite(BOARD_ADS, HIGH);   // CS deasserted

    spi.begin();
    initialize_ads();
}

/**
 * ADS1299 recommended power-up and register initialisation sequence.
 *
 * Porting change: pinMode() calls remove the TI GPIO callback (NULL) third
 * argument; Arduino's pinMode() takes only two parameters.
 */
void ADS1299_Library::initialize_ads(void) {
    // Recommended power-up sequence requires > Tpor (~32 ms)
    delay(50);
    pinMode(ADS_RST, OUTPUT);       // was: pinMode(ADS_RST, OUTPUT, NULL)
    digitalWrite(ADS_RST, LOW);     // assert reset
    delayMicroseconds(4);
    digitalWrite(ADS_RST, HIGH);    // deassert reset
    delayMicroseconds(20);          // wait ≥ 18 tCLK (~8 µs at default fCLK)
    delay(40);

    resetADS(BOARD_ADS);            // send RESET opcode, exit RDATAC, power down all channels
    delay(40);

    // CONFIG1: no CLK output (no daisy), set output data rate
    WREG(CONFIG1, (ADS1299_CONFIG1_DAISY_NOT | curSampleRate), BOARD_ADS);
    numChannels = 4;                // ADS1299-4 has 4 channels

    // Default channel settings
    defaultChannelSettings[POWER_DOWN]     = NO;
    defaultChannelSettings[GAIN_SET]       = ADS_GAIN01;
    defaultChannelSettings[INPUT_TYPE_SET] = ADSINPUT_NORMAL;
    defaultChannelSettings[BIAS_SET]       = YES;
    defaultChannelSettings[SRB2_SET]       = NO;
    defaultChannelSettings[SRB1_SET]       = NO;

    for (int i = 0; i < numChannels; i++) {
        for (int j = 0; j < OPENBCI_NUMBER_OF_CHANNEL_SETTINGS; j++) {
            channelSettings[i][j] = defaultChannelSettings[j];
        }
        useInBias[i] = true;
        useSRB2[i]   = true;
    }
    boardUseSRB1 = daisyUseSRB1 = false;

    writeChannelSettings();

    WREG(CONFIG3, 0b11101100, BOARD_ADS);  // Enable internal reference buffer
    delay(1);

    for (int i = 0; i < numChannels; i++) {
        leadOffSettings[i][PCHAN] = OFF;
        leadOffSettings[i][NCHAN] = OFF;
    }

    verbosity       = false;
    firstDataPacket = true;
    streaming       = false;
}

/* ============================================================ */
/*  Channel management                                          */
/* ============================================================ */

void ADS1299_Library::resetADS(int targetSS) {
    int startChan, stopChan;
    if (targetSS == BOARD_ADS) {
        startChan = 1;
        stopChan  = 4;
    } else {
        return;
    }
    RESET(targetSS);    // send RESET opcode → all registers reload defaults
    SDATAC(targetSS);   // exit Read Data Continuous mode
    delay(100);

    for (int chan = startChan; chan <= stopChan; chan++) {
        deactivateChannel(chan);
    }
}

void ADS1299_Library::deactivateChannel(byte N) {
    byte setting, startChan, endChan, targetSS;
    if (N < 5) {
        targetSS  = BOARD_ADS;
        startChan = 0;
        endChan   = 4;
    } else {
        return;
    }

    SDATAC(targetSS);
    delay(1);

    N = constrain(N - 1, startChan, endChan - 1);  // convert to 0-based index

    setting = RREG(CH1SET + (N - startChan), targetSS);
    delay(1);
    bitSet(setting, 7);     // bit 7 = 1 → power down channel
    bitClear(setting, 3);   // bit 3 = 0 → exclude from SRB2
    WREG(CH1SET + (N - startChan), setting, targetSS);
    delay(1);

    // Remove from BIAS generation
    setting = RREG(BIAS_SENSP, targetSS);
    delay(1);
    bitClear(setting, N - startChan);
    WREG(BIAS_SENSP, setting, targetSS);
    delay(1);

    setting = RREG(BIAS_SENSN, targetSS);
    delay(1);
    bitClear(setting, N - startChan);
    WREG(BIAS_SENSN, setting, targetSS);
    delay(1);

    leadOffSettings[N][0] = leadOffSettings[N][1] = NO;
    changeChannelLeadOffDetect(N + 1);
}

void ADS1299_Library::activateChannel(byte N) {
    byte setting, startChan, endChan, targetSS;
    if (N <= numChannels) {
        targetSS  = BOARD_ADS;
        startChan = 0;
        endChan   = numChannels;
    } else {
        if (!daisyPresent) return;
        targetSS  = DAISY_ADS;
        startChan = 8;
        endChan   = 16;
    }

    N = constrain(N - 1, startChan, endChan - 1);

    SDATAC(targetSS);
    setting = 0x00;
    setting |= channelSettings[N][GAIN_SET];
    setting |= channelSettings[N][INPUT_TYPE_SET];
    if (useSRB2[N]) {
        channelSettings[N][SRB2_SET] = YES;
    } else {
        channelSettings[N][SRB2_SET] = NO;
    }
    if (channelSettings[N][SRB2_SET] == YES) {
        bitSet(setting, 3);
    }
    WREG(CH1SET + (N - startChan), setting, targetSS);

    if (useInBias[N]) {
        channelSettings[N][BIAS_SET] = YES;
    } else {
        channelSettings[N][BIAS_SET] = NO;
    }

    setting = RREG(BIAS_SENSP, targetSS);
    if (channelSettings[N][BIAS_SET] == YES) {
        bitSet(setting, N - startChan);
        useInBias[N] = true;
    } else {
        bitClear(setting, N - startChan);
        useInBias[N] = false;
    }
    WREG(BIAS_SENSP, setting, targetSS);
    delay(1);

    setting = RREG(BIAS_SENSN, targetSS);
    if (channelSettings[N][BIAS_SET] == YES) {
        bitSet(setting, N - startChan);
    } else {
        bitClear(setting, N - startChan);
    }
    WREG(BIAS_SENSN, setting, targetSS);
    delay(1);

    setting = 0x00;
    if (targetSS == BOARD_ADS && boardUseSRB1) setting = 0x20;
    if (targetSS == DAISY_ADS && daisyUseSRB1) setting = 0x20;
    WREG(MISC1, setting, targetSS);
}

/** Write channel settings for all channels of one ADS (BOARD or DAISY). */
void ADS1299_Library::writeChannelSettings(void) {
    boolean use_SRB1 = false;
    byte    setting, startChan, endChan, targetSS;

    for (int b = 0; b < 2; b++) {
        if (b == 0) {
            targetSS  = BOARD_ADS;
            startChan = 0;
            endChan   = numChannels;
        }
        if (b == 1) {
            if (!daisyPresent) return;
            targetSS  = DAISY_ADS;
            startChan = 8;
            endChan   = 16;
        }

        SDATAC(targetSS);
        delay(1);

        for (byte i = startChan; i < endChan; i++) {
            setting = 0x00;
            if (channelSettings[i][POWER_DOWN] == YES) {
                setting |= 0x80;
            }
            setting |= channelSettings[i][GAIN_SET];
            setting |= channelSettings[i][INPUT_TYPE_SET];
            if (channelSettings[i][SRB2_SET] == YES) {
                setting |= 0x08;     // close SRB2 switch
                useSRB2[i] = true;
            } else {
                useSRB2[i] = false;
            }
            WREG(CH1SET + (i - startChan), setting, targetSS);

            // add or remove from BIAS generation
            setting = RREG(BIAS_SENSP, targetSS);
            if (channelSettings[i][BIAS_SET] == YES) {
                bitSet(setting, i - startChan);
                useInBias[i] = true;
            } else {
                bitClear(setting, i - startChan);
                useInBias[i] = false;
            }
            WREG(BIAS_SENSP, setting, targetSS);
            delay(1);

            setting = RREG(BIAS_SENSN, targetSS);
            if (channelSettings[i][BIAS_SET] == YES) {
                bitSet(setting, i - startChan);
            } else {
                bitClear(setting, i - startChan);
            }
            WREG(BIAS_SENSN, setting, targetSS);
            delay(1);

            if (channelSettings[i][SRB1_SET] == YES) {
                use_SRB1 = true;
            }
        }

        // SRB1 switch — evaluated from channelSettings, not previous state
        if (use_SRB1) {
            for (int i = startChan; i < endChan; i++) {
                channelSettings[i][SRB1_SET] = YES;
            }
            WREG(MISC1, 0x20, targetSS);
            if (targetSS == BOARD_ADS) boardUseSRB1 = true;
            if (targetSS == DAISY_ADS) daisyUseSRB1 = true;
        } else {
            for (int i = startChan; i < endChan; i++) {
                channelSettings[i][SRB1_SET] = NO;
            }
            WREG(MISC1, 0x00, targetSS);
            if (targetSS == BOARD_ADS) boardUseSRB1 = false;
            if (targetSS == DAISY_ADS) daisyUseSRB1 = false;
        }
    }
}

/** Write settings for a single channel N (1-based). */
void ADS1299_Library::writeChannelSettings(byte N) {
    byte setting, startChan, endChan, targetSS;
    if (N <= numChannels) {
        targetSS  = BOARD_ADS;
        startChan = 0;
        endChan   = numChannels;
    } else {
        if (!daisyPresent) return;
        targetSS  = DAISY_ADS;
        startChan = 8;
        endChan   = 16;
    }

    N = constrain(N - 1, startChan, endChan - 1);

    SDATAC(targetSS);
    delay(1);

    setting = 0x00;
    if (channelSettings[N][POWER_DOWN] == YES) {
        setting |= 0x80;
    }
    setting |= channelSettings[N][GAIN_SET];
    setting |= channelSettings[N][INPUT_TYPE_SET];
    if (channelSettings[N][SRB2_SET] == YES) {
        setting |= 0x08;     // close SRB2 switch
        useSRB2[N] = true;
    } else {
        useSRB2[N] = false;
    }
    WREG(CH1SET + (N - startChan), setting, targetSS);

    // add or remove from BIAS generation
    setting = RREG(BIAS_SENSP, targetSS);
    if (channelSettings[N][BIAS_SET] == YES) {
        useInBias[N] = true;
        bitSet(setting, N - startChan);
    } else {
        useInBias[N] = false;
        bitClear(setting, N - startChan);
    }
    WREG(BIAS_SENSP, setting, targetSS);
    delay(1);

    setting = RREG(BIAS_SENSN, targetSS);
    if (channelSettings[N][BIAS_SET] == YES) {
        bitSet(setting, N - startChan);
    } else {
        bitClear(setting, N - startChan);
    }
    WREG(BIAS_SENSN, setting, targetSS);
    delay(1);

    // SRB1 applies to all channels if set on this one
    if (channelSettings[N][SRB1_SET] == YES) {
        for (int i = startChan; i < endChan; i++) channelSettings[i][SRB1_SET] = YES;
        if (targetSS == BOARD_ADS) boardUseSRB1 = true;
        if (targetSS == DAISY_ADS) daisyUseSRB1 = true;
        setting = 0x20;
    }
    if (channelSettings[N][SRB1_SET] == NO) {
        for (int i = startChan; i < endChan; i++) channelSettings[i][SRB1_SET] = NO;
        if (targetSS == BOARD_ADS) boardUseSRB1 = false;
        if (targetSS == DAISY_ADS) daisyUseSRB1 = false;
        setting = 0x00;
    }
    WREG(MISC1, setting, targetSS);
}

void ADS1299_Library::setChannelsToDefault(void) {
    for (int i = 0; i < numChannels; i++) {
        for (int j = 0; j < OPENBCI_NUMBER_OF_CHANNEL_SETTINGS; j++) {
            channelSettings[i][j] = defaultChannelSettings[j];
        }
        useInBias[i] = true;
        useSRB2[i]   = true;
    }
    boardUseSRB1 = daisyUseSRB1 = false;
    writeChannelSettings();

    for (int i = 0; i < numChannels; i++) {
        leadOffSettings[i][PCHAN] = OFF;
        leadOffSettings[i][NCHAN] = OFF;
    }
    changeChannelLeadOffDetect();
    WREG(MISC1, 0x00, BOARD_ADS);
}

void ADS1299_Library::changeChannelLeadOffDetect(void) {
    byte startChan, endChan, targetSS;
    for (int b = 0; b < 2; b++) {
        if (b == 0) {
            targetSS  = BOARD_ADS;
            startChan = 0;
            endChan   = 4;
        }
        if (b == 1) return;  // No daisy

        SDATAC(targetSS);
        delay(1);
        byte P_setting = RREG(LOFF_SENSP, targetSS);
        byte N_setting = RREG(LOFF_SENSN, targetSS);

        for (int i = startChan; i < endChan; i++) {
            if (leadOffSettings[i][PCHAN] == ON) {
                bitSet(P_setting, i - startChan);
            } else {
                bitClear(P_setting, i - startChan);
            }
            if (leadOffSettings[i][NCHAN] == ON) {
                bitSet(N_setting, i - startChan);
            } else {
                bitClear(N_setting, i - startChan);
            }
            WREG(LOFF_SENSP, P_setting, targetSS);
            WREG(LOFF_SENSN, N_setting, targetSS);
        }
    }
}

void ADS1299_Library::changeChannelLeadOffDetect(byte N) {
    byte targetSS, startChan, endChan;
    if (N < 5) {
        targetSS  = BOARD_ADS;
        startChan = 0;
        endChan   = 4;
    } else {
        return;
    }

    N = constrain(N - 1, startChan, endChan - 1);
    SDATAC(targetSS);
    delay(1);

    byte P_setting = RREG(LOFF_SENSP, targetSS);
    byte N_setting = RREG(LOFF_SENSN, targetSS);

    if (leadOffSettings[N][PCHAN] == ON) {
        bitSet(P_setting, N - startChan);
    } else {
        bitClear(P_setting, N - startChan);
    }
    if (leadOffSettings[N][NCHAN] == ON) {
        bitSet(N_setting, N - startChan);
    } else {
        bitClear(N_setting, N - startChan);
    }
    WREG(LOFF_SENSP, P_setting, targetSS);
    WREG(LOFF_SENSN, N_setting, targetSS);
}

void ADS1299_Library::configureLeadOffDetection(byte amplitudeCode, byte freqCode) {
    amplitudeCode &= 0b00001100;
    freqCode      &= 0b00000011;

    byte setting, targetSS;
    for (int i = 0; i < 2; i++) {
        if (i == 0) {
            targetSS = BOARD_ADS;
        }
        if (i == 1) return;  // No daisy

        setting  = RREG(LOFF, targetSS);
        setting &= 0b11110000;
        setting |= amplitudeCode;
        setting |= freqCode;
        WREG(LOFF, setting, targetSS);
        delay(1);
    }
}

void ADS1299_Library::configureInternalTestSignal(byte amplitudeCode, byte freqCode) {
    byte setting, targetSS;
    for (int i = 0; i < 2; i++) {
        if (i == 0) {
            targetSS = BOARD_ADS;
        }
        if (i == 1) {
            if (!daisyPresent) return;
            targetSS = DAISY_ADS;
        }
        if (amplitudeCode == ADSTESTSIG_NOCHANGE)
            amplitudeCode = (RREG(CONFIG2, targetSS) & (0b00000100));
        if (freqCode == ADSTESTSIG_NOCHANGE)
            freqCode = (RREG(CONFIG2, targetSS) & (0b00000011));
        freqCode      &= 0b00000011;
        amplitudeCode &= 0b00000100;
        setting = 0b11010000 | freqCode | amplitudeCode;
        WREG(CONFIG2, setting, targetSS);
        delay(1);
    }
}

/* ============================================================ */
/*  Streaming                                                   */
/* ============================================================ */

void ADS1299_Library::startADS(void) {
    firstDataPacket = true;
    RDATAC(BOTH_ADS);
    delay(1);
    START(BOTH_ADS);
    delay(1);
    isRunning = true;
}

void ADS1299_Library::startADSMiddleStream(void) {
    streaming = true;
    RDATAC(BOTH_ADS);
    delay(1);
    START(BOTH_ADS);
    delay(1);
    isRunning = true;
}

void ADS1299_Library::stopADS(void) {
    STOP(BOTH_ADS);
    delay(1);
    SDATAC(BOTH_ADS);
    delay(1);
    isRunning = false;
}

void ADS1299_Library::streamStart(void) {
    streaming = true;
    startADS();
}

void ADS1299_Library::streamStop(void) {
    stopADS();
    streaming = false;
}

/** Read one 24-bit sample per channel from the SPI bus (non-buffered path). */
void ADS1299_Library::updateChannelData(void) {
    byte inByte;
    int  byteCounter = 0;

    // This flag is set true by the DRDY ISR; clear it before reading
    channelDataAvailable = false;

    csLow(BOARD_ADS);
    // Read 3-byte status word
    for (int i = 0; i < 3; i++) {
        inByte    = xfer(0x00);
        boardStat = (boardStat << 8) | inByte;
    }
    // Read 3 bytes × 4 channels
    for (int i = 0; i < OPENBCI_ADS_CHANS_PER_BOARD; i++) {
        boardChannelDataInt[i] = 0;
        for (int j = 0; j < OPENBCI_ADS_BYTES_PER_CHAN; j++) {
            inByte = xfer(0x00);
            boardChannelDataRaw[byteCounter] = inByte;
            byteCounter++;
            boardChannelDataInt[i] = (boardChannelDataInt[i] << 8) | inByte;    // Operated on register, independent from memory endianness
        }
    }
    csHigh(BOARD_ADS);

    // Sign-extend 24-bit two's-complement to 32-bit
    for (int i = 0; i < OPENBCI_ADS_CHANS_PER_BOARD; i++) {
        if (bitRead(boardChannelDataInt[i], 23) == 1) {
            boardChannelDataInt[i] |= 0xFF000000;
        } else {
            boardChannelDataInt[i] &= 0x00FFFFFF;
        }
    }

    if (firstDataPacket) firstDataPacket = false;
}

/* ============================================================ */
/*  Device identification                                       */
/* ============================================================ */

/** Read ID register. Expected value: 0x3C for ADS1299-4. */
byte ADS1299_Library::ADS_getDeviceID(int targetSS) {
    byte data = RREG(ID_REG, targetSS);
    if (verbosity) {
        printAll("[ADS_getDeviceID] On Board ADS ID ");
        printHex(data);
        printlnAll();
        sendEOT();
    }
    return data;
}

/* ============================================================ */
/*  Register dump                                               */
/* ============================================================ */

void ADS1299_Library::printAllRegisters(void) {
    printADSregisters(BOARD_ADS);
}

void ADS1299_Library::printADSregisters(int targetSS) {
    boolean prev = verbosity;
    verbosity = true;
    RREGS(0x00, 0x0C, targetSS);
    delay(10);
    RREGS(0x0D, 0x17 - 0x0D, targetSS);
    verbosity = prev;
}

/* ============================================================ */
/*  Low-level SPI                                               */
/* ============================================================ */

/**
 * Assert chip select and begin a SPI transaction.
 * SPI.beginTransaction() must wrap every CS-low → CS-high cycle so that
 * the bus clock and mode are guaranteed correct and no other SPI device
 * can interleave.
 */
void ADS1299_Library::csLow(int SS) {
    SPI.beginTransaction(ADS_SPI_SETTINGS);
    switch (SS) {
    case BOARD_ADS:
        digitalWrite(BOARD_ADS, LOW);
        break;
    default:
        break;
    }
}

/**
 * Deassert chip select and end the SPI transaction.
 */
void ADS1299_Library::csHigh(int SS) {
    switch (SS) {
    case BOARD_ADS:
        digitalWrite(BOARD_ADS, HIGH);
        break;
    default:
        break;
    }
    SPI.endTransaction();
}

/** Transfer one byte on the SPI bus; CS must already be asserted. */
byte ADS1299_Library::xfer(byte _data) {
    return spi.transfer(_data);
}

/* ============================================================ */
/*  ADS1299 SPI system commands                                 */
/* ============================================================ */

void ADS1299_Library::WAKEUP(int targetSS) {
    csLow(targetSS);
    xfer(_WAKEUP);
    csHigh(targetSS);
    delayMicroseconds(3);  // must wait 4 tCLK before next command
}

void ADS1299_Library::STANDBY(int targetSS) {
    csLow(targetSS);
    xfer(_STANDBY);
    csHigh(targetSS);
}

void ADS1299_Library::RESET(int targetSS) {
    csLow(targetSS);
    xfer(_RESET);
    delayMicroseconds(12);  // must wait 18 tCLK to execute
    csHigh(targetSS);
}

void ADS1299_Library::START(int targetSS) {
    csLow(targetSS);
    xfer(_START);
    csHigh(targetSS);
}

void ADS1299_Library::STOP(int targetSS) {
    csLow(targetSS);
    xfer(_STOP);
    csHigh(targetSS);
}

void ADS1299_Library::RDATAC(int targetSS) {
    csLow(targetSS);
    xfer(_RDATAC);
    csHigh(targetSS);
    delayMicroseconds(3);
}

void ADS1299_Library::SDATAC(int targetSS) {
    csLow(targetSS);
    xfer(_SDATAC);
    csHigh(targetSS);
    delayMicroseconds(10);  // must wait ≥ 4 tCLK after this command
}

void ADS1299_Library::RDATA(int targetSS) {
    byte inByte;
    csLow(targetSS);
    xfer(_RDATA);
    for (int i = 0; i < 3; i++) {
        inByte    = xfer(0x00);
        boardStat = (boardStat << 8) | inByte;
    }
    if (targetSS == BOARD_ADS) {
        for (int i = 0; i < OPENBCI_ADS_CHANS_PER_BOARD; i++) {
            boardChannelDataInt[i] = 0;
            for (int j = 0; j < OPENBCI_ADS_BYTES_PER_CHAN; j++) {
                inByte = xfer(0x00);
                boardChannelDataInt[i] = (boardChannelDataInt[i] << 8) | inByte;
            }
        }
        for (int i = 0; i < OPENBCI_ADS_CHANS_PER_BOARD; i++) {
            if (bitRead(boardChannelDataInt[i], 23) == 1) {
                boardChannelDataInt[i] |= 0xFF000000;
            } else {
                boardChannelDataInt[i] &= 0x00FFFFFF;
            }
        }
    }
    csHigh(targetSS);
}

/* ============================================================ */
/*  Register read / write                                       */
/* ============================================================ */

/** Read one register at _address and return its value. */
byte ADS1299_Library::RREG(byte _address, int targetSS) {
    byte opcode1 = _address + 0x20;  // RREG: 001rrrrr
    csLow(targetSS);
    xfer(opcode1);              // opcode byte 1: command + address
    xfer(0x00);                 // opcode byte 2: number of registers − 1 (0 = read 1)
    regData[_address] = xfer(0x00);  // clock in the register value
    csHigh(targetSS);

    if (verbosity) {
        printAll("[RREG] ");
        printRegisterName(_address);
        printHex(_address);
        printAll(", ");
        printHex(regData[_address]);
        printlnAll();
    }
    return regData[_address];
}

/** Read _numRegistersMinusOne+1 consecutive registers starting at _address. */
void ADS1299_Library::RREGS(byte _address, byte _numRegistersMinusOne, int targetSS) {
    byte opcode1 = _address + 0x20;
    csLow(targetSS);
    xfer(opcode1);
    xfer(_numRegistersMinusOne);
    for (int i = 0; i <= _numRegistersMinusOne; i++) {
        regData[_address + i] = xfer(0x00);
    }
    csHigh(targetSS);

    if (verbosity) {
        for (int i = 0; i <= _numRegistersMinusOne; i++) {
            printAll("[RREGS] ");
            printRegisterName(_address + i);
            printHex(_address + i);
            printAll(", ");
            printHex(regData[_address + i]);
            printlnAll();
            if (!commandFromSPI)
                delay(30);
        }
    }
}

/** Write one register at _address with value _value. */
void ADS1299_Library::WREG(byte _address, byte _value, int targetSS) {
    byte opcode1 = _address + 0x40;  // WREG: 010rrrrr
    byte readback;
    csLow(targetSS);
    xfer(opcode1);   // opcode byte 1: command + address
    xfer(0x00);      // opcode byte 2: number of registers − 1
    xfer(_value);    // data byte
    csHigh(targetSS);
    regData[_address] = _value;

    if (verbosity) {
        printAll("[WREG] Wrote ");
        printHex(_value);
        printAll(" to register ");
        printHex(_address);
        printlnAll(".");
        // printAll("[WREG->RREG_VERIFY] ");
        sendEOT();
        readback = RREG(_address, targetSS);   // verify by reading back
        if (readback == _value) {
            printAll("[WREG_VERIFY][PASS] Register ");
            printHex(_address);
            printAll(" write confirmed (expected=");
            printHex(_value);
            printAll(", read=");
            printHex(readback);
            printlnAll(").");
        } else {
            printAll("[WREG_VERIFY][FAIL] Register ");
            printHex(_address);
            printAll(" write mismatch (expected=");
            printHex(_value);
            printAll(", read=");
            printHex(readback);
            printlnAll(").");
        }
    }
}

/** Write _numRegistersMinusOne+1 consecutive registers from regData[]. */
void ADS1299_Library::WREGS(byte _address, byte _numRegistersMinusOne, int targetSS) {
    byte opcode1 = _address + 0x40;
    csLow(targetSS);
    xfer(opcode1);
    xfer(_numRegistersMinusOne);
    for (int i = _address; i <= (_address + _numRegistersMinusOne); i++) {
        xfer(regData[i]);
    }
    csHigh(targetSS);
    if (verbosity) {
        printAll("[WREGS] Registers ");
        printHex(_address);
        printAll(" to ");
        printHex(_address + _numRegistersMinusOne);
        printlnAll(" modified.");
        printAll("[WREGS->RREGS_VERIFY] ");
        sendEOT();
        // Read back
        RREGS(_address, _numRegistersMinusOne, targetSS);
    }
}

/* ============================================================ */
/*  Sample rate                                                 */
/* ============================================================ */

void ADS1299_Library::setSampleRate(uint8_t newSampleRateCode) {
    curSampleRate = (SAMPLE_RATE)newSampleRateCode;
    // Only update CONFIG1 (output data rate) — a full initialize_ads() is not
    // needed just to change the sample rate. initialize_ads() performs a
    // hardware RESET + 4-channel deactivation which takes ~260 ms and can cause
    // SPI bus contention with EegAcquisitionTask.
    SDATAC(BOARD_ADS);
    delay(1);
    WREG(CONFIG1, (ADS1299_CONFIG1_DAISY_NOT | (uint8_t)curSampleRate), BOARD_ADS);
    delay(1);
}

const char* ADS1299_Library::getSampleRate(void) {
    switch (curSampleRate) {
    case SAMPLE_RATE_16000: return "16000";
    case SAMPLE_RATE_8000:  return "8000";
    case SAMPLE_RATE_4000:  return "4000";
    case SAMPLE_RATE_2000:  return "2000";
    case SAMPLE_RATE_1000:  return "1000";
    case SAMPLE_RATE_500:   return "500";
    case SAMPLE_RATE_250:
    default:                return "250";
    }
}

/* ============================================================ */
/*  Gain helpers                                                */
/* ============================================================ */

/** Return the integer gain multiplier for channel N (1-based). */
uint8_t ADS1299_Library::getGainInt(uint8_t N) {
    if (N < 1 || N > 4) {
        return 0;
    }
    switch (channelSettings[N - 1][GAIN_SET]) {
    case ADS_GAIN01:  return 1;
    case ADS_GAIN02:  return 2;
    case ADS_GAIN04:  return 4;
    case ADS_GAIN06:  return 6;
    case ADS_GAIN08:  return 8;
    case ADS_GAIN12:  return 12;
    case ADS_GAIN24:  return 24;
    default:          return 0;
    }
}

byte ADS1299_Library::getDefaultChannelSettingForSetting(byte setting) {
    switch (setting) {
    case POWER_DOWN:     return NO;
    case GAIN_SET:       return ADS_GAIN24;
    case INPUT_TYPE_SET: return ADSINPUT_NORMAL;
    case BIAS_SET:       return YES;
    case SRB2_SET:       return YES;
    case SRB1_SET:
    default:             return NO;
    }
}

char ADS1299_Library::getDefaultChannelSettingForSettingAscii(byte setting) {
    switch (setting) {
    case GAIN_SET:
        return (ADS_GAIN24 >> 4) + '0';
    default:
        return getDefaultChannelSettingForSetting(setting) + '0';
    }
}

/* ============================================================ */
/*  Channel number helpers                                      */
/* ============================================================ */

char ADS1299_Library::getConstrainedChannelNumber(byte channelNumber) {
    int maxChannel = (numChannels > 0) ? (numChannels - 1) : 0;
    return constrain(channelNumber - 1, 0, maxChannel);
}

char ADS1299_Library::getTargetSSForConstrainedChannelNumber(byte channelNumber) {
    if (channelNumber < numChannels) {
        return BOARD_ADS;
    } else {
        return 0xFF;
    }
}

/* ============================================================ */
/*  Stream-safe wrappers                                        */
/* ============================================================ */

void ADS1299_Library::streamSafeChannelActivate(byte channelNumber) {
    boolean wasStreaming = streaming;
    if (streaming) streamStop();
    activateChannel(channelNumber);
    if (wasStreaming) streamStart();
}

void ADS1299_Library::streamSafeChannelDeactivate(byte channelNumber) {
    boolean wasStreaming = streaming;
    if (streaming) streamStop();
    deactivateChannel(channelNumber);
    if (wasStreaming) streamStart();
}

void ADS1299_Library::streamSafeLeadOffSetForChannel(byte channelNumber, byte pInput, byte nInput) {
    boolean wasStreaming = streaming;
    if (streaming) streamStop();
    leadOffSettings[channelNumber - 1][PCHAN] = pInput;
    leadOffSettings[channelNumber - 1][NCHAN] = nInput;
    changeChannelLeadOffDetect(channelNumber);
    if (wasStreaming) streamStart();
}

void ADS1299_Library::streamSafeChannelSettingsForChannel(
        byte channelNumber, byte powerDown, byte gain,
        byte inputType, byte bias, byte srb2, byte srb1) {
    boolean wasStreaming = streaming;
    if (streaming) streamStop();
    writeChannelSettings(channelNumber);
    if (wasStreaming) streamStart();
}

void ADS1299_Library::streamSafeSetAllChannelsToDefault(void) {
    boolean wasStreaming = streaming;
    if (streaming) streamStop();
    setChannelsToDefault();
    if (wasStreaming) streamStart();
}

void ADS1299_Library::streamSafeSetSampleRate(SAMPLE_RATE newSampleRate) {
    boolean wasStreaming = streaming;
    if (streaming) streamStop();
    setSampleRate((uint8_t)newSampleRate);
    if (wasStreaming) streamStart();
}

bool ADS1299_Library::streamSafeIncreaseGain(uint8_t N) {
    boolean wasStreaming = streaming;
    if (N < 1 || N > 4) return false;
    if (channelSettings[N - 1][GAIN_SET] == ADS_GAIN24) return false;
    if (streaming) streamStop();
    uint8_t new_gain;
    switch (channelSettings[N - 1][GAIN_SET]) {
    case ADS_GAIN01: new_gain = ADS_GAIN02; break;
    case ADS_GAIN02: new_gain = ADS_GAIN04; break;
    case ADS_GAIN04: new_gain = ADS_GAIN06; break;
    case ADS_GAIN06: new_gain = ADS_GAIN08; break;
    case ADS_GAIN08: new_gain = ADS_GAIN12; break;
    case ADS_GAIN12: new_gain = ADS_GAIN24; break;
    default:         new_gain = channelSettings[N - 1][GAIN_SET]; break;
    }
    channelSettings[N - 1][GAIN_SET] = new_gain;
    writeChannelSettings(N);
    if (wasStreaming) startADSMiddleStream();
    return true;
}

bool ADS1299_Library::streamSafeDecreaseGain(uint8_t N) {
    boolean wasStreaming = streaming;
    if (N < 1 || N > 4) return false;
    if (channelSettings[N - 1][GAIN_SET] == ADS_GAIN01) return false;
    if (streaming) streamStop();
    uint8_t new_gain;
    switch (channelSettings[N - 1][GAIN_SET]) {
    case ADS_GAIN02: new_gain = ADS_GAIN01; break;
    case ADS_GAIN04: new_gain = ADS_GAIN02; break;
    case ADS_GAIN06: new_gain = ADS_GAIN04; break;
    case ADS_GAIN08: new_gain = ADS_GAIN06; break;
    case ADS_GAIN12: new_gain = ADS_GAIN08; break;
    case ADS_GAIN24: new_gain = ADS_GAIN12; break;
    default:         new_gain = channelSettings[N - 1][GAIN_SET]; break;
    }
    channelSettings[N - 1][GAIN_SET] = new_gain;
    writeChannelSettings(N);
    if (wasStreaming) startADSMiddleStream();
    return true;
}

/* ============================================================ */
/*  Output helpers — all routed to Arduino Serial (USB CDC)     */
/* ============================================================ */

void ADS1299_Library::printAll(char c) {
    Serial.print(c);
}

void ADS1299_Library::printAll(const char *arr) {
    Serial.print(arr);
}

void ADS1299_Library::printlnAll(void) {
    Serial.println();
}

void ADS1299_Library::printlnAll(const char *arr) {
    Serial.print(arr);
    Serial.println();
}

void ADS1299_Library::printSerial(int c) {
    Serial.print(c);
}

void ADS1299_Library::printSerial(char c) {
    Serial.print(c);
}

void ADS1299_Library::printSerial(const char *arr) {
    Serial.print(arr);
}

void ADS1299_Library::printlnSerial(void) {
    Serial.println();
}

void ADS1299_Library::printlnSerial(char c) {
    Serial.println(c);
}

void ADS1299_Library::printlnSerial(const char *arr) {
    Serial.println(arr);
}

void ADS1299_Library::printHex(byte _data) {
    char buf[5];
    snprintf(buf, sizeof(buf), "0x%02X", _data);
    printAll(buf);
    if (commandFromSPI)
        delay(1);
}

void ADS1299_Library::printHex(int _data) {
    char buf[11];
    snprintf(buf, sizeof(buf), "0x%X", _data);
    printAll(buf);
    if (commandFromSPI)
        delay(1);
}

void ADS1299_Library::printDec(int _data) {
    char buf[11];
    snprintf(buf, sizeof(buf), "%d", _data);
    printAll(buf);
}

void ADS1299_Library::printlnHex(byte _data) {
    printHex(_data);
    printlnAll();
}

void ADS1299_Library::printFailure(void) {
    printAll("Failure: ");
}

void ADS1299_Library::printSuccess(void) {
    printAll("Success: ");
}

void ADS1299_Library::sendEOT(void) {
    // End-of-transmission marker used by OpenBCI protocol — no-op for now
}

void ADS1299_Library::writeSerial(uint8_t c) {
    printDec((int)c);
}

void ADS1299_Library::writeSerial(int c) {
    printDec(c);
}

/** Print human-readable register name for verbosity output. */
void ADS1299_Library::printRegisterName(byte _address) {
    switch (_address) {
    case ID_REG:      printAll("ADS_ID, ");      break;
    case CONFIG1:     printAll("CONFIG1, ");     break;
    case CONFIG2:     printAll("CONFIG2, ");     break;
    case CONFIG3:     printAll("CONFIG3, ");     break;
    case LOFF:        printAll("LOFF, ");        break;
    case CH1SET:      printAll("CH1SET, ");      break;
    case CH2SET:      printAll("CH2SET, ");      break;
    case CH3SET:      printAll("CH3SET, ");      break;
    case CH4SET:      printAll("CH4SET, ");      break;
    case CH5SET:      printAll("CH5SET, ");      break;
    case CH6SET:      printAll("CH6SET, ");      break;
    case CH7SET:      printAll("CH7SET, ");      break;
    case CH8SET:      printAll("CH8SET, ");      break;
    case BIAS_SENSP:  printAll("BIAS_SENSP, ");  break;
    case BIAS_SENSN:  printAll("BIAS_SENSN, ");  break;
    case LOFF_SENSP:  printAll("LOFF_SENSP, ");  break;
    case LOFF_SENSN:  printAll("LOFF_SENSN, ");  break;
    case LOFF_FLIP:   printAll("LOFF_FLIP, ");   break;
    case LOFF_STATP:  printAll("LOFF_STATP, ");  break;
    case LOFF_STATN:  printAll("LOFF_STATN, ");  break;
    case GPIO:        printAll("GPIO, ");        break;
    case MISC1:       printAll("MISC1, ");       break;
    case MISC2:       printAll("MISC2, ");       break;
    case CONFIG4:     printAll("CONFIG4, ");     break;
    default:                                     break;
    }
}

/* ============================================================ */
/*  Global singleton (same symbol as in the original iES code)  */
/* ============================================================ */
ADS1299_Library ads1299;
