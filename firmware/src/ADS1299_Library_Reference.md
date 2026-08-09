# ADS1299 Library Reference (Nicla Voice Port)

This document explains the relevant ADS1299 library functions in firmware/ADS1299NiclaFW/ADS1299_Library.cpp and how they map to console output.

Scope:

- Register read/write APIs (RREG, WREG, RREGS, WREGS)
- Verbose log syntax and patterns
- Initialization flow and channel configuration behavior
- Streaming commands and data-read behavior
- Practical log interpretation examples

---

## 1. Logging Model

Verbose logs are controlled by:

- ads1299.verbosity = true in the sketch

When verbosity is enabled:

- RREG() prints register name, address, and value
- WREG() prints Register 0xXX modified. and then reads the same register back with RREG()
- This creates repeated read/write/read patterns in the console

### 1.1 Single-register read log format

Printed by RREG():

REGISTER_NAME, 0xAA, 0xVV

Example:

BIAS_SENSN, 0x0E, 0x03

Meaning:

- Register symbol: BIAS_SENSN
- Address: 0x0E
- Current register contents: 0x03

### 1.2 Single-register write log format

Printed by WREG():

Register 0xAA modified.

Then WREG() calls RREG() on that same address to verify readback.

So a typical write sequence is:

1. Read old value (optional; depends on caller flow)
2. Register 0xXX modified.
3. Read back new value

---

## 2. Core Register Access Functions

### 2.1 RREG(byte _address, int targetSS)

Purpose:

- Read one ADS1299 register

SPI sequence:

1. Assert CS (csLow)
2. Send opcode 0x20 + _address
3. Send byte count minus one (0x00 for one register)
4. Clock in one data byte
5. Deassert CS (csHigh)

Behavior:

- Stores value into regData[_address]
- Returns that value
- Prints register line when verbosity == true

### 2.2 `WREG(byte _address, byte _value, int targetSS)`

Purpose:

- Write one ADS1299 register

SPI sequence:

1. Assert CS
2. Send opcode 0x40 + _address
3. Send byte count minus one (0x00)
4. Send _value
5. Deassert CS

Behavior:

- Updates regData[_address]
- If verbose:
  - Prints Register 0xXX modified.
  - Calls RREG(_address, targetSS) for verification

### 2.3 `RREGS(byte _address, byte _numRegistersMinusOne, int targetSS)`

Purpose:

- Read a contiguous register block

Behavior:

- Reads `_numRegistersMinusOne + 1` bytes starting at `_address`
- Stores into regData[]
- Verbose mode prints one line per register

### 2.4 `WREGS(byte _address, byte _numRegistersMinusOne, int targetSS)`

Purpose:

- Write a contiguous register block from regData[]

Behavior:

- Writes block and, in verbose mode, performs readback with RREGS()

---

## 3. Register Name Mapping in Logs

The text prefix in logs (for example CH3SET) comes from printRegisterName().

Important mappings:

- 0x01 -> CONFIG1
- 0x03 -> CONFIG3
- 0x05 -> CH1SET
- 0x06 -> CH2SET
- 0x07 -> CH3SET
- 0x08 -> CH4SET
- 0x0D -> BIAS_SENSP
- 0x0E -> BIAS_SENSN
- 0x0F -> LOFF_SENSP
- 0x10 -> LOFF_SENSN
- 0x15 -> MISC1

---

## 4. Initialization Flow (What Runs and Why)

Main path:

1. initialize()
2. initialize_ads()
3. resetADS(BOARD_ADS)
4. WREG(CONFIG1, ...)
5. Set numChannels = 4 (ADS1299-4)
6. Populate default channel settings arrays
7. writeChannelSettings()
8. WREG(CONFIG3, 0xEC, BOARD_ADS)

### 4.1 resetADS() + deactivateChannel()

resetADS() issues RESET/SDATAC and deactivates board channels 1..4.

Each deactivateChannel() call updates:

- CHnSET (power down bit, SRB2 bit)
- BIAS_SENSP
- BIAS_SENSN
- Lead-off routing via changeChannelLeadOffDetect()

This produces repeated clusters in logs for CH1..CH4.

### 4.2 writeChannelSettings() and channel count behavior

Current behavior in this port:

- Board loop uses endChan = numChannels
- On ADS1299-4 this means only channels 1..4 are configured
- CH5SET..CH8SET are not touched anymore

This is the key fix that removes irrelevant register logs for non-existent channels.

---

## 5. Why Repeated Lines Appear in Console

Repetition is expected due to read-modify-write logic plus verification:

Example pattern:

1. BIAS_SENSN, 0x0E, 0x01 (read old)
2. Register 0x0E modified. (write)
3. BIAS_SENSN, 0x0E, 0x03 (readback verify)

The old and new values differ because one channel bit is being updated.

---

## 6. Streaming Control and Data Path

### 6.1 Start/stop commands

- startADS() sends RDATAC then START
- stopADS() sends STOP then SDATAC

If startADS() is not called, DRDY-driven sample output will not proceed as expected.

### 6.2 Data read function

updateChannelData() does:

1. Clear channelDataAvailable
2. Read 3-byte status word
3. Read 3 bytes x 4 channels
4. Sign-extend each 24-bit sample to 32-bit signed int

Notes:

- This function always reads 4 channels in this ADS1299-4 build

---

## 7. Practical Console Interpretation

### 7.1 Device ID lines

- Device ID read: 0x3C means ADS1299-4 is detected
- 0x3E would indicate ADS1299-8

### 7.2 "Modified" lines

Register 0xXX modified. means a write command was sent, not necessarily that value changed from a different value.

To confirm final value, use the subsequent readback line.

### 7.3 Relevant vs irrelevant channel registers

For ADS1299-4:

- Relevant: CH1SET..CH4SET
- Irrelevant: CH5SET..CH8SET

After the channel-bound fix, seeing only CH1..CH4 in init logs is expected and correct.

---

## 8. Quick Reference: Common Log Fragments

CH2SET, 0x06, 0x61

- Read CH2SET, current value 0x61

Register 0x06 modified.

- Wrote CH2SET

CH2SET, 0x06, 0xE1

- Readback confirms CH2SET now 0xE1

BIAS_SENSP, 0x0D, 0x07

- Current BIAS_SENSP bitfield value is 0x07

MISC1, 0x15, 0x00

- SRB1 global routing register currently 0

---

## 9. Relevant Source Locations

- RREG(), RREGS(), WREG(), WREGS(): register SPI access and logs
- printRegisterName(): symbolic name mapping for log prefix
- initialize_ads(): full startup configuration sequence
- resetADS(), deactivateChannel(): reset-time channel shutdown flow
- writeChannelSettings(): apply per-channel settings (bounded by numChannels)
- startADS(), stopADS(): conversion state control
- updateChannelData(): DRDY-driven sample read path

Use the current file in your workspace:

test/SPI_Test/ADS1299_Library.cpp

---

## 10. Notes for Future Maintenance

- Keep channel loops bounded by runtime channel count (numChannels) for board-specific variants
- If you enable daisy support later, revisit helper functions and channel-index conventions (1-based API vs 0-based arrays)
- Verbose mode is excellent for bring-up, but disable for normal streaming to reduce serial overhead
