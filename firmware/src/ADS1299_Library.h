/**
 * @file ADS1299_Library.h
 * @brief ADS1299 driver — ported to Arduino Nicla Voice (nRF52832 / Mbed OS).
 *
 * PORTING SUMMARY (iES_v0.3-master → Arduino Nicla Voice)
 * ---------------------------------------------------------
 * The original iES firmware was layered as:
 *
 *   [ADS1299_Library]  ← Arduino-API calls (pinMode, digitalWrite, SPI)
 *        ↓
 *   [Arduino.cpp / DSPI.cpp]  ← hand-built Arduino shim on TI SimpleLink SDK
 *        ↓
 *   [TI SimpleLink SDK / TI-RTOS]  ← MSP432 hardware
 *
 * On the Nicla Voice the native Arduino Mbed OS core provides the Arduino API
 * directly, so the shim layer (Arduino.cpp / DSPI.cpp) is replaced by thin
 * wrappers (see DSPI.h) or deleted entirely.  The ADS1299_Library source code
 * needed only the following edits:
 *
 *  1. Removed all #include <ti/…> and TI-RTOS headers.
 *  2. Removed POSIX mqueue and pthread headers (interrupt buffering task).
 *  3. Removed LIS3DH / accel methods (not present on Nicla Voice).
 *  4. Removed BLE / WiFi streaming helpers (not needed for the SPI test).
 *  5. Removed adaptive gain control (AGC) and cir_queue dependency.
 *  6. Removed the TI-RTOS interrupt-buffering task (boardBeginADSInterrupt).
 *  7. boardBegin() simplified — no TI event/semaphore/mqueue setup.
 *  8. printAll() / printlnAll() now call Serial.print() (USB CDC).
 *  9. IES_DEBUG_STATE() replaced by Serial.print().
 * 10. pinMode(pin, mode, NULL) → pinMode(pin, mode)  (Arduino API, 2 args).
 * 11. csLow() calls SPI.beginTransaction(ADS_SPI_SETTINGS) then drives CS
 *     low; csHigh() drives CS high then calls SPI.endTransaction(). This
 *     protects every CS-assert/deassert cycle as a single atomic SPI
 *     transaction and guarantees correct clock rate and mode on every access.
 *
 * Everything else (SPI protocol, register sequences, channel management) is
 * identical to the original.
 */

#ifndef ADS1299_LIBRARY_H
#define ADS1299_LIBRARY_H

#include <Arduino.h>
#include "ADS1299_Definitions.h"
#include "DSPI.h"

/* ------------------------------------------------------------ */
/*  Forward declaration                                         */
/* ------------------------------------------------------------ */
class ADS1299_Library;
extern ADS1299_Library ads1299;   // Global singleton (same as original iES)

/* ------------------------------------------------------------ */
/*  ADS1299_Library class                                       */
/* ------------------------------------------------------------ */
class ADS1299_Library {

public:
    /* ---- Enumerations ---- */

    typedef enum SAMPLE_RATE {
        SAMPLE_RATE_16000 = 0,
        SAMPLE_RATE_8000,
        SAMPLE_RATE_4000,
        SAMPLE_RATE_2000,
        SAMPLE_RATE_1000,
        SAMPLE_RATE_500,
        SAMPLE_RATE_250
    } sample_rate_e;

    typedef enum BOARD_MODE {
        BOARD_MODE_DEFAULT = 0,
        BOARD_MODE_DEBUG,
        BOARD_MODE_ANALOG,
        BOARD_MODE_DIGITAL,
        BOARD_MODE_MARKER,
        BOARD_MODE_BLE,
        BOARD_MODE_END_OF_MODES
    } board_mode_e;

    typedef enum DEBUG_MODE {
        DEBUG_MODE_ON = 0,
        DEBUG_MODE_OFF
    } debug_mode_e;

    typedef enum PACKET_TYPE {
        PACKET_TYPE_ACCEL = 0,
        PACKET_TYPE_RAW_AUX,
        PACKET_TYPE_USER_DEFINED,
        PACKET_TYPE_ACCEL_TIME_SET,
        PACKET_TYPE_ACCEL_TIME_SYNC,
        PACKET_TYPE_RAW_AUX_TIME_SET,
        PACKET_TYPE_RAW_AUX_TIME_SYNC
    } packet_type_e;

    /* ---- Constructor ---- */
    ADS1299_Library();

    /* ---- Board-level initialisation ---- */
    void     begin(void);              // Calls boardBegin()
    boolean  boardBegin(void);         // Simplified: no RTOS task setup
    void     boardReset(void);         // Soft reset: initialize + lead-off config

    void     initialize(void);         // Configure CS & RST pins, start SPI, call initialize_ads()
    void     initialize_ads(void);     // ADS1299 hardware reset + register defaults

    /* ---- Channel management ---- */
    void  resetADS(int targetSS);
    void  deactivateChannel(byte N);
    void  activateChannel(byte N);
    void  writeChannelSettings(void);          // Write settings for all channels
    void  writeChannelSettings(byte N);        // Write settings for channel N
    void  setChannelsToDefault(void);
    void  changeChannelLeadOffDetect(void);
    void  changeChannelLeadOffDetect(byte N);
    void  configureLeadOffDetection(byte amplitudeCode, byte freqCode);
    void  configureInternalTestSignal(byte amplitudeCode, byte freqCode);

    /* ---- Streaming ---- */
    void  startADS(void);
    void  stopADS(void);
    void  startADSMiddleStream(void);
    void  updateChannelData(void);             // Read one sample from SPI (non-buffered)

    /* ---- Device identification ---- */
    byte  ADS_getDeviceID(int targetSS);       // Reads ID register, returns 0x3C for ADS1299-4

    /* ---- Low-level SPI ---- */
    void  csLow(int SS);                       // Assert CS; begins SPI transaction
    void  csHigh(int SS);                      // Deassert CS; ends SPI transaction
    byte  xfer(byte _data);                    // Transfer one byte via SPI

    /* ---- ADS1299 SPI commands ---- */
    void  WAKEUP(int targetSS);
    void  STANDBY(int targetSS);
    void  RESET(int targetSS);
    void  START(int targetSS);
    void  STOP(int targetSS);
    void  RDATAC(int targetSS);
    void  SDATAC(int targetSS);
    void  RDATA(int targetSS);

    /* ---- Register read / write ---- */
    byte  RREG(byte _address, int targetSS);
    void  RREGS(byte _address, byte _numRegistersMinusOne, int targetSS);
    void  WREG(byte _address, byte _value, int targetSS);
    void  WREGS(byte _address, byte _numRegistersMinusOne, int targetSS);

    /* ---- Register dump (requires verbosity = true) ---- */
    void  printAllRegisters(void);
    void  printADSregisters(int targetSS);

    /* ---- Output helpers (back-end: Serial) ---- */
    void  printAll(char c);
    void  printAll(const char *arr);
    void  printlnAll(void);
    void  printlnAll(const char *arr);
    void  printSerial(int c);
    void  printSerial(char c);
    void  printSerial(const char *arr);
    void  printlnSerial(void);
    void  printlnSerial(char c);
    void  printlnSerial(const char *arr);
    void  printHex(byte _data);
    void  printHex(int _data);
    void  printDec(int _data);
    void  printlnHex(byte _data);
    void  printRegisterName(byte _address);
    void  printSuccess(void);
    void  printFailure(void);
    void  sendEOT(void);
    void  writeSerial(uint8_t c);
    void  writeSerial(int c);

    /* ---- Sample rate ---- */
    void        setSampleRate(uint8_t newSampleRateCode);
    const char* getSampleRate(void);

    /* ---- Gain helpers ---- */
    uint8_t  getGainInt(uint8_t N);
    byte     getDefaultChannelSettingForSetting(byte setting);
    char     getDefaultChannelSettingForSettingAscii(byte setting);

    /* ---- Channel number helpers ---- */
    char  getConstrainedChannelNumber(byte channelNumber);
    char  getTargetSSForConstrainedChannelNumber(byte channelNumber);

    /* ---- Stream-safe wrappers ---- */
    void  streamSafeChannelActivate(byte channelNumber);
    void  streamSafeChannelDeactivate(byte channelNumber);
    void  streamSafeLeadOffSetForChannel(byte channelNumber, byte pInput, byte nInput);
    void  streamSafeChannelSettingsForChannel(byte channelNumber,
              byte powerDown, byte gain, byte inputType,
              byte bias, byte srb2, byte srb1);
    void  streamSafeSetAllChannelsToDefault(void);
    void  streamSafeSetSampleRate(SAMPLE_RATE newSampleRate);
    bool  streamSafeIncreaseGain(uint8_t N);
    bool  streamSafeDecreaseGain(uint8_t N);
    void  streamStart(void);
    void  streamStop(void);

    /* ---- Public variables ---- */
    boolean  verbosity;          // true → print register read/write details to Serial
    boolean  streaming;          // true when data acquisition is active
    boolean  daisyPresent;       // true if a second ADS1299 (daisy) is present
    boolean  boardUseSRB1;
    boolean  daisyUseSRB1;
    volatile boolean channelDataAvailable;  // Set true by DRDY ISR

    int   numChannels;           // Number of active EEG channels (4 for ADS1299-4)
    byte  channelSettings[OPENBCI_NUMBER_OF_CHANNELS_DAISY][OPENBCI_NUMBER_OF_CHANNEL_SETTINGS];
    byte  defaultChannelSettings[OPENBCI_NUMBER_OF_CHANNEL_SETTINGS];
    byte  leadOffSettings[OPENBCI_NUMBER_OF_CHANNELS_DAISY][OPENBCI_NUMBER_OF_LEAD_OFF_SETTINGS];
    boolean  useInBias[OPENBCI_NUMBER_OF_CHANNELS_DAISY];
    boolean  useSRB2[OPENBCI_NUMBER_OF_CHANNELS_DAISY];

    byte  boardChannelDataRaw[OPENBCI_MAX_NUMBER_BYTES_PER_ADS_SAMPLE];
    int   boardChannelDataInt[OPENBCI_MAX_NUMBER_CHANNELS_PER_ADS_SAMPLE];

    SAMPLE_RATE  curSampleRate;
    BOARD_MODE   curBoardMode;
    DEBUG_MODE   curDebugMode;
    PACKET_TYPE  curPacketType;

private:
    void  initializeVariables(void);

    boolean  commandFromSPI;
    boolean  firstDataPacket;
    boolean  isRunning;
    byte     regData[24];    // Mirror of ADS1299 register values
    int      boardStat;      // Last-read status register (3 bytes packed into int)
};

#endif // ADS1299_LIBRARY_H
