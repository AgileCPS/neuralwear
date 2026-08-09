/**
 * @file pinDef.h
 * @brief Arduino pin assignments for the ADS1299 shield on Arduino Nicla Voice.
 *
 * Physical connections (Nicla Voice header ↔ ADS1299 shield):
 *
 *  Nicla pin 6  → ADS1299 /CS    (SPI chip select, active-low)
 *  Nicla pin 7  → ADS1299 DOUT   (SPI MISO)
 *  Nicla pin 8  → ADS1299 DIN    (SPI MOSI)
 *  Nicla pin 9  → ADS1299 SCLK   (SPI clock)
 *  Nicla pin 10 → ADS1299 /RESET (hardware reset, active-low)
 *  Nicla pin 11 → ADS1299 /DRDY  (data-ready interrupt, active-low)
 */

#ifndef PIN_DEF_H
#define PIN_DEF_H

/* SPI bus */
#define SPI_CS      6
#define SPI_MISO    7
#define SPI_MOSI    8
#define SPI_SCK     9

/* ADS1299 control lines */
#define ADS_RST_PIN   10   // Active-low hardware reset
#define ADS_DRDY_PIN  11   // Active-low data-ready interrupt

#endif // PIN_DEF_H
