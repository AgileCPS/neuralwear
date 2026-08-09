/**
 * @file ADS1299_Definitions.h
 * @brief ADS1299 register map, SPI opcodes, and protocol constants.
 *
 * Ported from iES_v0.3-master / ADS1299_Library_Definitions.h.
 * Changes from original:
 *   - Removed TI SimpleLink / MSP432 GPIO-enum references.
 *   - ADS_DRDY, ADS_RST, BOARD_ADS now resolve to Arduino pin numbers
 *     from pinDef.h.
 *   - Kept all ADS1299 register addresses, SPI opcodes, and channel
 *     settings constants verbatim.
 */

#ifndef ADS1299_DEFINITIONS_H
#define ADS1299_DEFINITIONS_H

#include "pinDef.h"

/* ------------------------------------------------------------ */
/*  Physical pin aliases (Arduino pin numbers)                  */
/* ------------------------------------------------------------ */
#define ADS_DRDY    ADS_DRDY_PIN   // /DRDY – data-ready, active-low
#define ADS_RST     ADS_RST_PIN    // /RESET – hardware reset, active-low
#define BOARD_ADS   SPI_CS         // /CS for the on-board ADS1299
#define DAISY_ADS   0xFF           // No daisy module in this design
#define BOTH_ADS    BOARD_ADS

/* ------------------------------------------------------------ */
/*  ADS1299 SPI Command Bytes                                   */
/* ------------------------------------------------------------ */
#define _WAKEUP   0x02  // Wake-up from standby mode
#define _STANDBY  0x04  // Enter standby mode
#define _RESET    0x06  // Reset device registers to default
#define _START    0x08  // Start / restart (synchronise) conversions
#define _STOP     0x0A  // Stop conversion
#define _RDATAC   0x10  // Enable Read Data Continuous mode (default at power-up)
#define _SDATAC   0x11  // Stop Read Data Continuous mode
#define _RDATA    0x12  // Read data by command (single read)

/* ------------------------------------------------------------ */
/*  Register Addresses                                          */
/* ------------------------------------------------------------ */
#define ADS_ID    0x3C  // Product ID for ADS1299-4 (matches original iES source)
#define ID_REG    0x00
#define CONFIG1   0x01
#define CONFIG2   0x02
#define CONFIG3   0x03
#define LOFF      0x04
#define CH1SET    0x05
#define CH2SET    0x06
#define CH3SET    0x07
#define CH4SET    0x08
#define CH5SET    0x09
#define CH6SET    0x0A
#define CH7SET    0x0B
#define CH8SET    0x0C
#define BIAS_SENSP  0x0D
#define BIAS_SENSN  0x0E
#define LOFF_SENSP  0x0F
#define LOFF_SENSN  0x10
#define LOFF_FLIP   0x11
#define LOFF_STATP  0x12
#define LOFF_STATN  0x13
#define GPIO      0x14
#define MISC1     0x15
#define MISC2     0x16
#define CONFIG4   0x17

/* ------------------------------------------------------------ */
/*  Channel Settings Array Indices                              */
/* ------------------------------------------------------------ */
#define POWER_DOWN      (0)
#define GAIN_SET        (1)
#define INPUT_TYPE_SET  (2)
#define BIAS_SET        (3)
#define SRB2_SET        (4)
#define SRB1_SET        (5)

#define YES   (0x01)
#define NO    (0x00)

/* ------------------------------------------------------------ */
/*  Gain Codes  (CHnSET[6:4])                                   */
/* ------------------------------------------------------------ */
#define ADS_GAIN01  (0b00000000)  // ×1
#define ADS_GAIN02  (0b00010000)  // ×2
#define ADS_GAIN04  (0b00100000)  // ×4
#define ADS_GAIN06  (0b00110000)  // ×6
#define ADS_GAIN08  (0b01000000)  // ×8
#define ADS_GAIN12  (0b01010000)  // ×12
#define ADS_GAIN24  (0b01100000)  // ×24

/* ------------------------------------------------------------ */
/*  Input Multiplexer Codes  (CHnSET[2:0])                     */
/* ------------------------------------------------------------ */
#define ADSINPUT_NORMAL     (0b00000000)
#define ADSINPUT_SHORTED    (0b00000001)
#define ADSINPUT_BIAS_MEAS  (0b00000010)
#define ADSINPUT_MVDD       (0b00000011)
#define ADSINPUT_TEMP       (0b00000100)
#define ADSINPUT_TESTSIG    (0b00000101)
#define ADSINPUT_BIAS_DRP   (0b00000110)
#define ADSINPUT_BIAL_DRN   (0b00000111)

/* ------------------------------------------------------------ */
/*  Test Signal Codes                                           */
/* ------------------------------------------------------------ */
#define ADSTESTSIG_AMP_1X         (0b00000000)
#define ADSTESTSIG_AMP_2X         (0b00000100)
#define ADSTESTSIG_PULSE_SLOW     (0b00000000)
#define ADSTESTSIG_PULSE_FAST     (0b00000001)
#define ADSTESTSIG_DCSIG          (0b00000011)
#define ADSTESTSIG_NOCHANGE       (0b11111111)

/* ------------------------------------------------------------ */
/*  CONFIG1 Preset Values                                       */
/* ------------------------------------------------------------ */
#define ADS1299_CONFIG1_DAISY      (0b10110000)  // CLK output enabled (daisy)
#define ADS1299_CONFIG1_DAISY_NOT  (0b10010000)  // CLK output disabled (no daisy)

/* ------------------------------------------------------------ */
/*  Lead-Off Detection Codes                                    */
/* ------------------------------------------------------------ */
#define LOFF_MAG_6NA    (0b00000000)
#define LOFF_MAG_24NA   (0b00000100)
#define LOFF_MAG_6UA    (0b00001000)
#define LOFF_MAG_24UA   (0b00001100)
#define LOFF_FREQ_DC    (0b00000000)
#define LOFF_FREQ_7p8HZ (0b00000001)
#define LOFF_FREQ_31p2HZ (0b00000010)
#define LOFF_FREQ_FS_4  (0b00000011)
#define PCHAN  (0)
#define NCHAN  (1)
#define OFF    (0)
#define ON     (1)

/* ------------------------------------------------------------ */
/*  Channel Activation                                          */
/* ------------------------------------------------------------ */
#define ACTIVATE_SHORTED  (2)
#define ACTIVATE          (1)
#define DEACTIVATE        (0)

/* ------------------------------------------------------------ */
/*  Packet Framing                                              */
/* ------------------------------------------------------------ */
#define PCKT_START  0xA0
#define PCKT_END    0xC0

/* ------------------------------------------------------------ */
/*  Channel / ADS Counts                                        */
/* ------------------------------------------------------------ */
#define OPENBCI_NUMBER_OF_CHANNELS_DAISY    16
#define OPENBCI_NUMBER_OF_CHANNELS_DEFAULT   8
#define OPENBCI_NUMBER_OF_CHANNEL_SETTINGS   6
#define OPENBCI_NUMBER_OF_LEAD_OFF_SETTINGS  2
#define OPENBCI_ADS_BYTES_PER_CHAN           3
#define OPENBCI_ADS_CHANS_PER_BOARD         4   // ADS1299-4 variant

/* For this design only channels 3 & 4 are streamed */
#define OPENBCI_NUMBER_CHANNELS_TO_STREAM    2
#define OPENBCI_CHANNEL_DATA_INDEX           3   // 0-based: channel 3 is index 2, but stream starts here
#define OPENBCI_MAX_NUMBER_CHANNELS_PER_ADS_SAMPLE  4
#define OPENBCI_MAX_NUMBER_BYTES_PER_ADS_SAMPLE     (3 * OPENBCI_MAX_NUMBER_CHANNELS_PER_ADS_SAMPLE)

/* ------------------------------------------------------------ */
/*  Scale Factor  (Vref = 4.5 V, gain = 1, 24-bit)             */
/* ------------------------------------------------------------ */
#define SCALE_FACTOR_UV  0.5364418669  // 4.5e6 / (2^23 - 1)

/* ------------------------------------------------------------ */
/*  Baud Rate                                                   */
/* ------------------------------------------------------------ */
#define OPENBCI_BAUD_RATE  115200

#endif // ADS1299_DEFINITIONS_H
