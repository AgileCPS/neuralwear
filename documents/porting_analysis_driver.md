# iES_v0.3-master → Arduino Nicla Voice: Porting Analysis

**Purpose:** Identify which layers of `iES_v0.3-master` must be replaced, which need
adaptation, and which can be re-used unchanged when porting to the Arduino Nicla Voice
(nRF52832 / Mbed OS / Arduino core).

**Date:** 2026-03-07

---

## 1. Original Platform Summary

| Item | iES_v0.3-master |
|---|---|
| MCU | Texas Instruments MSP432P4111 (Arm Cortex-M4F, 48 MHz) |
| RTOS | TI-RTOS (SYS/BIOS) |
| Driver framework | TI SimpleLink SDK (`<ti/drivers/…>`) |
| IDE / build | TI Code Composer Studio (CCS); proprietary `.cmd` linker scripts |
| SPI API | `SPI_open()` / `SPI_transfer()` from `<ti/drivers/SPI.h>` |
| GPIO API | `GPIO_write()` / `GPIO_setCallback()` from `<ti/drivers/GPIO.h>` |
| Timing | TI-RTOS `Clock_getTicks()`, `Task_sleep()` |
| Debug output | TI Display driver (`Display_printf`) |
| Bluetooth | External RN41 SPP module via `BPLib` |

---

## 2. Target Platform Summary

| Item | Arduino Nicla Voice |
|---|---|
| MCU | Nordic nRF52832 (Arm Cortex-M4F, 64 MHz) |
| RTOS | Mbed OS (via Arduino Mbed OS Nicla Boards core) |
| SPI API | Arduino `SPI` class (`SPI.begin()`, `SPI.transfer()`, `SPISettings`) |
| GPIO API | Arduino `pinMode()` / `digitalWrite()` / `attachInterrupt()` |
| Timing | Arduino `delay()` / `delayMicroseconds()` / `millis()` |
| Debug output | `Serial.print()` over USB CDC |
| Bluetooth | ArduinoBLE (built-in nRF52 radio) |

---

## 3. Architectural Design Intent

Understanding *why* the codebase is structured the way it is makes every porting
decision self-evident.

### 3.1 The Arduino API as a deliberate portability seam

The iES developer made a conscious architectural choice: write the entire upper layer
(`ADS_1299_Library.cpp`, `ies_task.cpp`) against the **Arduino API** —
`pinMode()`, `digitalWrite()`, `delay()`, `spi.transfer()` — as if the MSP432 were
just another Arduino board.

The reason is portability. The Arduino API is a well-known, stable interface contract.
Any code written to it can in principle run on any platform that provides an
implementation of that contract.

### 3.2 The problem: no Arduino core existed for the MSP432

The MSP432 has no mature, production-quality Arduino core. The developer therefore
**hand-built the missing half of the contract themselves**:

| Shim file | What it emulates | Backed by |
|---|---|---|
| `Arduino.cpp` / `Arduino.h` | `pinMode()`, `digitalWrite()`, `delay()`, `millis()` | TI SimpleLink SDK (`<ti/drivers/GPIO.h>`) + TI-RTOS (`Task_sleep`, `Clock_getTicks`) |
| `DSPI.cpp` / `DSPI.h` | `spi.transfer()`, `spi.begin()` | TI SimpleLink SDK (`SPI_open`, `SPI_transfer`) |

These two files are a manual Arduino core emulation layer, written specifically for
the MSP432+TI-RTOS environment. They exist purely because the platform did not provide
one natively.

### 3.3 Two porting strategies for any future platform

This architecture creates a clean binary choice whenever the firmware is ported:

**Case A — Target platform has a native Arduino core (e.g. Arduino Nicla Voice)**

> Delete `Arduino.cpp` and `DSPI.cpp` entirely. The upper layer
> (`ADS_1299_Library.cpp`) already uses the correct Arduino API; the native core
> provides it for free. Zero changes to the upper layer are required.

**Case B — Target platform has no Arduino core (bare-metal, Zephyr, FreeRTOS, etc.)**

> Write a new `Arduino.cpp` and `DSPI.cpp` (or equivalent) that wrap that
> platform's GPIO and SPI drivers behind the same Arduino-compatible function
> signatures. The upper layer remains untouched.

The Nicla Voice is **Case A**. The native Arduino Mbed OS core (nRF52832) provides
`pinMode`, `digitalWrite`, `delay`, `millis`, `attachInterrupt`, and the `SPI` class
natively. The shim layer is simply deleted.

---

## 4. Layer-by-Layer Analysis

The codebase is split into **ten logical layers**, ordered from lowest (hardware) to
highest (application). The porting decision is given for each.

---

### Layer 1 — MCU / Board Initialisation  ❌ REPLACE ENTIRELY

| File | Role | Action |
|---|---|---|
| `MSP_EXP432P4111.c` | Full MSP432 peripheral init: GPIO pin tables, SPI config structs, ADC, DMA, I2C, UART, timer peripheral maps using TI driverlib | Discard. Arduino core handles all peripheral initialisation automatically. |
| `MSP_EXP432P4111.h` | Enumerations of board-level peripheral indices (`MSP_EXP432P4111_SPIName`, `MSP_EXP432P4111_GPIOName`, etc.) | Discard. Replace with `pinDef.h` pin constants for the Nicla Voice. |
| `Board.h` | Macro aliases mapping `Board_SPI2`, `Board_GPIO_LED0`, `iES_GPIO_ADS_CS`, etc. to the MSP432 enums above | Discard. Replace with simple `#define` pin constants (already started in `pinDef.h`). |
| `MSP_EXP432P4111_TIRTOS.cmd`, `msp432p4011.cmd`, `msp432p401r.cmd` | MSP432 linker command scripts | Discard. nRF52/Mbed linker scripts are provided by the Arduino core and board package. |
| `platform.h` / `platform_msp432p4111.c` | MCU reboot via `SysCtl_rebootDevice()` and CRC32 via MSP432 ROM | Discard. Use nRF52 NVIC reset and software CRC if needed. |

**Key point:** `Board.h` is included by every other file in the application. Its
replacement on the target is just an updated `pinDef.h` that exposes the same logical
names (`ADS_CS_PIN`, `ADS_DRDY_PIN`, `ADS_RST_PIN`) as plain integer pin numbers.

---

### Layer 2 — RTOS Entry Point  ❌ REPLACE ENTIRELY

| File | Role | Action |
|---|---|---|
| `main_tirtos.c` | Calls TI driver init functions (`Power_init()`, `GPIO_init()`, `SPI_init()`, …), creates the `ies_task`, then starts the TI-RTOS scheduler with `BIOS_start()` | Replace with Arduino `setup()` / `loop()`. Driver init is implicit in the Arduino core. Task creation is replaced with Mbed `rtos::Thread`. |

---

### Layer 3 — TI-RTOS Kernel Primitives  ❌ REPLACE THROUGHOUT

TI-RTOS primitives are scattered through `ADS_1299_Library.cpp`, `ies_task.cpp`, and
`Arduino.cpp`. Every occurrence must be replaced.

| TI-RTOS construct | Used for | Mbed OS / Arduino replacement |
|---|---|---|
| `Task_Struct` / `Task_Params` / `Task_construct()` | Spawn the interrupt-buffering task | `rtos::Thread` |
| `Semaphore_Handle` / `Semaphore_pend()` / `Semaphore_post()` | Signal the buffering thread from the DRDY ISR | `rtos::Semaphore` |
| `Event_Handle` / `Event_post()` / `Event_pend()` | Notify the main task of new ADS data | `rtos::EventFlags` |
| `mqueue.h` POSIX message queue (`mq_send` / `mq_receive`) | Pass raw channel bytes from ISR to task | `rtos::Queue<T, N>` or `rtos::Mail<T, N>` |
| `<ti/sysbios/knl/Queue.h>` | Internal queue | `rtos::Queue` |
| `Task_sleep(ms)` | Delay | `rtos::ThisThread::sleep_for(ms)` or `delay(ms)` |
| `Clock_getTicks()` | Millisecond timestamp | `millis()` |
| `Semaphore_Mode_BINARY` | Binary semaphore | `rtos::Semaphore(0, 1)` |
| `pthread_create` | Spawn task | `rtos::Thread` |

---

### Layer 4 — Arduino Shim Layer  ❌ REPLACE (file deleted, native API used)

| File | Role | Action |
|---|---|---|
| `Arduino.h` | Declares `pinMode()`, `digitalWrite()`, `delay()`, etc. mapped on to TI driverlib | **Delete.** On the Nicla Voice these functions are provided natively by the Arduino core. No shim needed. |
| `Arduino.cpp` | Implements `pinMode()` using `GPIO_setConfig()` (TI), `digitalWrite()` using `GPIO_write()` (TI), `delay()` / `delayMicroseconds()` using `Task_sleep()` (TI-RTOS), `millis()` using `Clock_getTicks()` (TI-RTOS) | **Delete.** All these functions (same signatures) are built into the Arduino Mbed OS core for nRF52. |

This shim is the reason `ADS_1299_Library.cpp` uses plain Arduino-style calls
(`pinMode`, `digitalWrite`, `delay`). That code does **not** need to change — it already
uses the correct Arduino API. Only the shim file that backed it is thrown away.

---

### Layer 5 — SPI HAL Wrapper (DSPI)  ❌ REPLACE

| File | Role | Action |
|---|---|---|
| `DSPI.h` | Declares the `DSPI` class with `begin()`, `transfer()`, `setPinSelect()`, `setSelect()` etc. | Replace with a thin adapter class (or remove the class entirely) that wraps the native Arduino `SPI` object. |
| `DSPI.cpp` | Implements `DSPI` using `SPI_open()` / `SPI_transfer()` / `SPI_close()` from `<ti/drivers/SPI.h>`. Configures `SPI_MODE1` (CPOL=0, CPHA=1) at 8 MHz. | Replace `SPI_open/transfer/close` with `SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1))` / `SPI.transfer()` / `SPI.endTransaction()`. |

The `DSPI::transfer(uint8_t)` interface is exactly what `ADS` calls via `spi.transfer()`.
Only the internal implementation of that one function changes. The call sites in
`ADS_1299_Library.cpp` (`xfer()`, see Layer 6) do not need modification.

**SPI mode note:** `DSPI_FRAME_FORMAT_DEFAULT` is set to `SPI_POL0_PHA1` = Mode 1
(CPOL=0, CPHA=1). The ADS1299 datasheet specifies CPOL=0, CPHA=1 (Mode 1). The
Arduino equivalent is `SPI_MODE1`. This is consistent with the existing `firmware_design.md`.

---

### Layer 6 — ADS1299 Driver  ⚠️ PARTIALLY PORTABLE (targeted edits only)

This is the most valuable layer. The SPI protocol logic, all register-map constants, the
channel configuration sequences, and the data-read sequences are **platform-independent**.

| Construct | Platform dependency | Porting action |
|---|---|---|
| `xfer(byte)` → `spi.transfer(byte)` | Calls DSPI (Layer 5) | No change in ADS code; DSPI replacement handles it |
| `csLow()` / `csHigh()` → `digitalWrite()` | Calls `Arduino.h` shim (Layer 4) | No change; native Arduino replaces the shim |
| `initialize_ads()` — `delay()`, `delayMicroseconds()`, `pinMode()`, `digitalWrite()` | Calls `Arduino.h` shim | No change |
| `resetADS()` — same calls | Calls `Arduino.h` shim | No change |
| `WREG()`, `RREG()`, `RREGS()`, `SDATAC()`, `RDATAC()`, `RESET()` | Pure SPI protocol | No change |
| `writeChannelSettings()`, `initialize_ads()` — register sequences | ADS1299 register protocol | Keep entirely |
| `ADS_DRDY_Service()` ISR | Uses TI-RTOS `Semaphore_post()` and `Event_post()` | Replace those two calls with Mbed `semaphore.release()` and `eventFlags.set()` |
| `ads_interrupt_buffering_fxn()` buffering thread | Uses `Task_Struct`, `Semaphore_pend()` | Replace with `rtos::Thread` + `rtos::Semaphore.acquire()` |
| `#include <ti/sysbios/…>`, `#include <mqueue.h>` | TI-RTOS / POSIX headers | Remove; replace with Mbed OS headers |
| `ads_interrupt_queue` (`mqd_t`) | POSIX message queue | Replace with `rtos::Queue<ads_interrupt_rec, 512>` |

**ADS1299 register definitions** (`ADS1299_Library_Definitions.h`): all opcodes,
register addresses, gain codes, input-type codes — **100% platform-independent, keep
without any changes.**

---

### Layer 7 — GPIO Interrupt (DRDY)  ⚠️ SMALL CHANGE

| iES implementation | Nicla Voice replacement |
|---|---|
| `GPIO_setCallback(ADS_DRDY, ADS_DRDY_Service)` followed by `GPIO_enableInt(ADS_DRDY)` — using TI GPIO driver | `attachInterrupt(digitalPinToInterrupt(ADS_DRDY_PIN), ADS_DRDY_Service, FALLING)` — standard Arduino API |
| ISR signature: `void ADS_DRDY_Service(uint_least8_t index)` (TI GPIO callback signature) | ISR signature: `void ADS_DRDY_Service(void)` (Arduino interrupt signature) |

---

### Layer 8 — Debug / Logging  ❌ REPLACE

| File | Role | Action |
|---|---|---|
| `ies_debug.h` | Macros wrapping TI `Display_printf(handle, line, col, fmt)` | Replace `Display_printf(…)` with `Serial.printf(fmt, …)`. The macro structure (`IES_DEBUG_STATE`, `IES_DEBUG_RESULT1`) can be preserved, just changing the back-end. |
| `ies_debug.c` (`ies_printf`, `ies_flush`) | Uses TI Display driver | Replace with `Serial.print()` / `Serial.flush()`. |
| `extern Display_Handle displayOut` dependency | TI Display handle passed everywhere | Remove. No display handle is needed with Arduino Serial. |

---

### Layer 9 — Application Task (`ies_task.cpp`)  ❌ REWRITE

`ies_task.cpp` is 1850 lines of application logic that ties together all layers. It
uses TI-RTOS events, pthreads, POSIX message queues, SD FatFS, BPLib BT SPP, ADCBuf,
and the TI Display driver — all of which have no direct equivalent on the Nicla Voice.

The **logic** (streaming state machine, channel enable/disable, gain control, sample
packaging) is reusable as a design reference, but the file cannot be compiled for the
target without a complete rewrite using:

- `rtos::Thread` instead of `pthread_t`
- `rtos::EventFlags` instead of `Event_Handle`
- `rtos::Queue` instead of POSIX `mqueue`
- `ArduinoBLE` instead of `BPLib`
- Arduino `SD` library or none instead of `SDFatFS`
- Arduino `Serial` instead of TI `Display`

---

### Layer 10 — Utility / Support Libraries  ✅ KEEP AS-IS

These files are pure C/C++ with no platform dependencies:

| File | Role | Status |
|---|---|---|
| `ADS1299_Library_Definitions.h` | All ADS1299 register addresses, SPI command opcodes, gain/mux codes, channel-setting constants | **Keep unchanged** |
| `ies_misc.h` / `ies_misc.cpp` | Endian-safe buffer ↔ integer conversions (`buffer_to_uint24`, `uint32_to_buffer`, etc.) | **Keep unchanged** |
| `cir_queue.h` / `cir_queue.cpp` | Circular (ring) buffer for adaptive gain control | **Keep unchanged** |
| `checksum.h` | CRC/checksum helper | **Keep unchanged** |
| `crc8.cpp` / `crc16.cpp` | CRC-8 and CRC-16 table-based implementations | **Keep unchanged** |

**Not applicable to Nicla Voice (discard):**

| File | Reason |
|---|---|
| `BPLib.h` / `BPLib.cpp` | Bluetooth SPP for external RN41 UART module. Nicla Voice uses built-in BLE (ArduinoBLE). |
| `MPU9250.h` / `MPU9250.cpp` | IMU driver. Not needed for the initial SPI test. Can be ported later if required. |
| `MAX17043.h` / `MAX17043.cpp` | Battery fuel gauge. Not on Nicla Voice. |

---

## 5. Summary Table

| Layer | Files | Porting decision |
|---|---|---|
| 1 · MCU / board init | `MSP_EXP432P4111.c/.h`, `Board.h`, `.cmd` files, `platform.*` | ❌ Replace entirely |
| 2 · RTOS entry point | `main_tirtos.c` | ❌ Replace with `setup()` / `loop()` |
| 3 · RTOS primitives | All `<ti/sysbios/…>` usage throughout | ❌ Replace with Mbed OS equivalents |
| 4 · Arduino shim | `Arduino.h`, `Arduino.cpp` | ❌ Delete (native Arduino provides these) |
| 5 · SPI HAL (DSPI) | `DSPI.h`, `DSPI.cpp` | ❌ Replace with Arduino `SPI` |
| 6 · ADS1299 driver | `ADS_1299_Library.cpp`, `ADS1299_Library.h` | ⚠️ Mostly keep; remove TI-RTOS calls in ISR/task |
| 7 · DRDY interrupt | Inside `ADS_1299_Library.cpp` | ⚠️ Change to `attachInterrupt()` + ISR signature |
| 8 · Debug/logging | `ies_debug.h`, `ies_debug.c` | ❌ Replace `Display_printf` with `Serial.print` |
| 9 · Application task | `ies_task.cpp`, `ies_task.h` | ❌ Rewrite using Mbed OS + ArduinoBLE |
| 10 · Utilities | `ADS1299_Library_Definitions.h`, `ies_misc.*`, `cir_queue.*`, `checksum.h`, `crc*.cpp` | ✅ Keep unchanged |
| 10b · Irrelevant HW | `BPLib.*`, `MPU9250.*`, `MAX17043.*` | 🚫 Discard |

---

## 6. What to Build First — SPI Communication Test

The goal for the first test is to verify SPI communication between the Nicla Voice and
the ADS1299 by reading the device-ID register (`ID_REG = 0x00`, expected value `0x3E`
for ADS1299-4 or `0x3E` for ADS1299).

Only the following elements need to be in place:

1. **`pinDef.h`** (already exists) — defines `SPI_CS`, `SPI_MISO`, `SPI_MOSI`,
   `SPI_SCK`, plus `ADS_RST_PIN` (pin 10) and `ADS_DRDY_PIN` (pin 11).

2. **`DSPI` replacement** — a minimal wrapper (or inline use of `SPI.beginTransaction`
   / `SPI.transfer` / `SPI.endTransaction`) with CS control via `digitalWrite()`.

3. **`ADS1299_Library_Definitions.h`** — keep as-is; provides the command opcodes and
   register addresses needed for the test.

4. **Subset of `ADS_1299_Library.cpp`** — specifically:
   - `initialize_ads()` (hardware reset sequence, `SDATAC`, register init)
   - `ADS_getDeviceID()` (reads `ID_REG` via `RREG`)
   - `xfer()`, `csLow()`, `csHigh()`, `RREG()`, `SDATAC()`, `RESET()`

5. **Arduino `Serial`** for printing the result — replaces the debug layer.

No RTOS, no Bluetooth, no interrupt buffering, no SD card is needed for this first test.
The sequence is:
```
nicla::begin()            // enable 3.3 V rail
spi.begin()               // init Arduino SPI
initialize_ads()          // hardware reset + register defaults
id = ADS_getDeviceID()    // RREG(ID_REG) → should return 0x3E
Serial.println(id, HEX)
```

---

## 7. Endianness: ADS1299 (Big-Endian) vs. nRF52832 (Little-Endian)

### 7.1 The Mismatch

| Component | Byte order |
|-----------|-----------|
| **ADS1299** — SPI output (status word + channel samples) | **Big-endian (MSB first)** — per ADS1299 datasheet §8.5 |
| **nRF52832** (Nicla Voice MCU, ARM Cortex-M4) | **Little-endian** |

### 7.2 How the Library Handles It

All multi-byte data reconstruction (`updateChannelData()`, `RDATA()`) uses explicit
left-shift accumulation — a byte-by-byte approach that is fully CPU-agnostic:

```cpp
// Byte 0 → MSB, Byte 2 → LSB  (big-endian as transmitted by ADS1299)
boardChannelDataInt[i] = (boardChannelDataInt[i] << 8) | inByte;
```

Sign extension from 24-bit to 32-bit is also done on the already-assembled integer,
with no memory-layout dependency:

```cpp
if (bitRead(boardChannelDataInt[i], 23) == 1)
    boardChannelDataInt[i] |= 0xFF000000;
else
    boardChannelDataInt[i] &= 0x00FFFFFF;
```

### 7.3 Audit Results

A full audit of `ADS1299_Library.cpp`, `ADS1299_Library.h`, `ADS1299_Definitions.h`,
`DSPI.cpp`, and `DSPI.h` confirmed:

| Aspect | Status |
|--------|--------|
| Channel sample assembly (3 bytes → 32-bit) | ✅ Safe — explicit shift+OR |
| Status word assembly (3 bytes → 32-bit) | ✅ Safe — explicit shift+OR |
| 24-bit sign extension | ✅ Safe — bit-level operation on assembled value |
| `boardChannelDataRaw[]` storage | ✅ Safe — bytes stored in received (big-endian) order; never cast to wider types |
| Register reads / writes (RREG/WREG/RREGS) | ✅ Safe — single-byte per register, no assembly |
| SPI hardware config (`MSBFIRST`, `SPI_MODE1`) | ✅ Safe — correct for ADS1299 protocol |
| Pointer casts / struct unions over byte buffers | ✅ Safe — none present |

**No endianness bugs exist in the current ported library.**

### 7.4 Latent Risks for Future Development

> **Risk 1 — Transmitting `boardChannelDataRaw[]` to a host**
>
> This array stores bytes in ADS1299 wire order (big-endian, MSB first). If it is ever
> sent over USB/BLE to a PC host and the host interprets it as native 32-bit integers
> without first byte-swapping, sample values will be wrong.
>
> **Mitigation:** When serialising raw bytes to a host, document the byte order
> explicitly, or convert to a known wire format (e.g. always little-endian for BLE
> characteristics).

> **Risk 2 — Transmitting `boardChannelDataInt[]` to a host**
>
> These 32-bit integers are in native ARM little-endian format. If serialised
> byte-by-byte (e.g. BLE notify / USB bulk), the receiving side must be told to
> expect little-endian values.
>
> **Mitigation:** Add a comment at every serialisation call site noting the byte
> order, or use explicit `htole32()` / `le32toh()` at the boundary.
