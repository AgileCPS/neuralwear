/**
 * @file DSPI.h
 * @brief Arduino SPI wrapper — replacement for the iES TI-SDK DSPI layer.
 *
 * PORTING NOTE (iES_v0.3-master → Arduino Nicla Voice)
 * -------------------------------------------------------
 * In the original iES firmware the DSPI class was a thin adapter that exposed
 * an Arduino-compatible interface (begin / transfer / setSelect / end) but was
 * backed internally by the TI SimpleLink SDK calls:
 *
 *     SPI_open()      → DSPI::begin()
 *     SPI_transfer()  → DSPI::transfer()
 *     SPI_close()     → DSPI::end()
 *
 * On the Nicla Voice (nRF52832 / Arduino Mbed OS core) the native Arduino
 * <SPI.h> already implements this interface directly.  This file therefore
 * re-implements DSPI as a minimal wrapper around the built-in SPI class.
 *
 * NOTE: arduino::MbedSPI does NOT support setMISO() / setMOSI() / setSCK().
 * Pin assignment is fixed by the board variant constructor.  The Nicla Voice
 * variant maps the default SPI object to MISO=7, MOSI=8, SCK=9, which
 * matches pinDef.h — so no pin re-configuration is needed.
 *
 * SPI settings used:
 *   Clock : 8 MHz  (same as original: DSPI_SPD_DEFAULT 8 000 000)
 *   Bit order : MSBFIRST
 *   Mode  : SPI_MODE1  (CPOL=0, CPHA=1) — required by ADS1299 datasheet
 *           (Same as original: DSPI_FRAME_FORMAT_DEFAULT = SPI_POL0_PHA1)
 *
 * SPI transaction management:
 *   SPI.beginTransaction() / SPI.endTransaction() are called in
 *   ADS1299_Library::csLow() / csHigh() so that every CS-assert/deassert
 *   cycle is protected as a single atomic SPI transaction.
 *   DSPI::transfer() therefore just calls SPI.transfer() directly.
 *
 * CS pin management:
 *   The ADS1299 CS line is driven by explicit digitalWrite() calls inside
 *   ADS1299_Library::csLow() / csHigh() — exactly as in the original code.
 *   DSPI does not manage CS.
 */

#ifndef DSPI_H
#define DSPI_H

#include <Arduino.h>
#include <SPI.h>
#include "pinDef.h"

/* ADS1299 SPI parameters */
#define DSPI_CLOCK_HZ     8000000UL   // 8 MHz — ADS1299 max is 20 MHz
#define DSPI_BIT_ORDER    MSBFIRST
#define DSPI_MODE         SPI_MODE1   // CPOL=0, CPHA=1 per ADS1299 datasheet

/* SPISettings object used by ADS1299_Library::csLow() / csHigh() */
extern const SPISettings ADS_SPI_SETTINGS;

class DSPIClass {
public:
    /**
     * Initialise the SPI peripheral and configure the bus pins.
     * Maps one-to-one to the original SPI_open() call.
     */
    void begin();

    /**
     * Release the SPI peripheral.
     * Maps one-to-one to the original SPI_close() call.
     */
    void end();

    /**
     * Transfer one byte on the SPI bus and return the received byte.
     * Maps one-to-one to the original SPI_transfer() call.
     *
     * @note  SPI.beginTransaction() must already be in effect when this
     *        is called (asserted in csLow(), released in csHigh()).
     */
    uint8_t transfer(uint8_t data);
};

extern DSPIClass spi;   // Global SPI object, same name as in iES original

#endif // DSPI_H
