/**
 * @file DSPI.cpp
 * @brief Arduino SPI wrapper implementation — replaces the TI-SDK DSPI layer.
 *
 * See DSPI.h for full porting notes.
 */

#include "DSPI.h"

/* Global SPISettings shared with ADS1299_Library::csLow() / csHigh() */
const SPISettings ADS_SPI_SETTINGS(DSPI_CLOCK_HZ, DSPI_BIT_ORDER, DSPI_MODE);

/* ------------------------------------------------------------ */

void DSPIClass::begin() {
    /*
     * On the Arduino Mbed OS / nRF52832 core (arduino::MbedSPI) the
     * setMISO() / setMOSI() / setSCK() setter methods do not exist.
     * Pin assignment is fixed at construction time via the SPIClass
     * constructor in the board variant.
     *
     * The Nicla Voice board variant already maps the default SPI object to
     * the same physical pins defined in pinDef.h:
     *   MISO → pin 7,  MOSI → pin 8,  SCK → pin 9
     * so a plain SPI.begin() is sufficient.
     */
    SPI.begin();
}

void DSPIClass::end() {
    SPI.end();
}

uint8_t DSPIClass::transfer(uint8_t data) {
    /*
     * SPI.beginTransaction() / endTransaction() are called in
     * ADS1299_Library::csLow() / csHigh() so we do not call them here.
     * This matches the original iES behaviour where the TI SPI_transfer()
     * was called with CS already asserted.
     */
    return SPI.transfer(data);
}

/* Singleton instance — same name as in the original iES source */
DSPIClass spi;
