# NICLA Voice — Technical Notes

> **Format:** Each note is numbered NOTE-XXX, with date, scope, and author metadata.
> New notes are appended at the bottom; the Table of Contents is updated accordingly.

---

## Table of Contents

- [NOTE-001 — SPI Pin Mapping: Full Stack Trace](#note-001--spi-pin-mapping-full-stack-trace)
- [NOTE-002 — ADS1299 Library: Full Comparison — iES Original vs Nicla Voice Port](#note-002--ads1299-library-full-comparison--ies-original-vs-nicla-voice-port)
- [NOTE-003 — Byte Order: ADS1299 (Big-Endian SPI) vs nRF52832 (Little-Endian CPU)](#note-003--byte-order-ads1299-big-endian-spi-vs-nrf52832-little-endian-cpu)
- [NOTE-004 — ADS1299 Analog Supply Voltages and Absolute Limits](#note-004--ads1299-analog-supply-voltages-and-absolute-limits)
- [NOTE-005 — Task Scheduling: Semaphore Wake-up + Cooperative Yield Pattern](#note-005--task-scheduling-semaphore-wake-up--cooperative-yield-pattern)
- [NOTE-006 — UartChannelTask Crash: Stack Overflow from Serial.print(float) in Mbed USB CDC](#note-006--uartchanneltask-crash-stack-overflow-from-serialprintfloat-in-mbed-usb-cdc)
- [NOTE-007 — nRF52832 Has No Hardware EEPROM: Flash-Emulated Persistent Storage](#note-007--nrf52832-has-no-hardware-eeprom-flash-emulated-persistent-storage)
- [NOTE-008 — iES EEG Wire Format: µV Integer vs Raw ADC Codes, Default Gain and Output Mode](#note-008--ies-eeg-wire-format-µv-integer-vs-raw-adc-codes-default-gain-and-output-mode)
- [NOTE-009 — UART Test Suite Status and Known Flakiness (2026-06-19, updated 2026-07-03)](#note-009--uart-test-suite-status-and-known-flakiness-2026-06-19)
- [NOTE-010 — Host Debatching and Timestamp Reconstruction (UART vs BLE)](#note-010--host-debatching-and-timestamp-reconstruction-uart-vs-ble)
- [NOTE-011 — HostProtocolMode: OpenVIBE Legacy Compat Mode and Runtime Baud Switching](#note-011--hostprotocolmode-openvibe-legacy-compat-mode-and-runtime-baud-switching)

---

## NOTE-001 · SPI Pin Assignment — Arduino Mbed Core Stack Trace

**Date:** 2026-03-07  
**Scope:** `test/SPI_Test/` — ADS1299 library ported to Arduino Nicla Voice

### Summary

The SPI bus pins are **not** configured anywhere in the application code.
They are fixed at library construction time by the `variants/NICLA/` board
variant inside ArduinoCore-mbed, and happen to match `pinDef.h` exactly.
CS is always software-controlled by explicit `digitalWrite()` calls.

---

### Layer-by-layer trace

#### Layer 1 — Build system (`boards.txt`)

The Nicla Voice build entry selects the **`NICLA`** variant folder:

```
nicla_voice.build.variant=NICLA
```

This is the same variant shared with the Nicla Sense ME (both use nRF52832).

---

#### Layer 2 — `variants/NICLA/pins_arduino.h` (Arduino pin index → PinName)

```cpp
// Arduino logical pin numbers
#define PIN_SPI_MISO  (7u)
#define PIN_SPI_MOSI  (8u)
#define PIN_SPI_SCK   (9u)
#define PIN_SPI_SS    (6u)

// Mbed PinName values passed directly to the MbedSPI constructor
#define SPI_MISO   (p28)   // nRF52 P0.28
#define SPI_MOSI   (p27)   // nRF52 P0.27
#define SPI_SCK    (p11)   // nRF52 P0.11
```

---

#### Layer 3 — `variants/NICLA/variant.cpp` (pin index → physical nRF52 pad)

```cpp
{ P0_29, NULL, NULL, NULL },    // 6: CS
{ P0_28, NULL, NULL, NULL },    // 7: CIPO  ← MISO
{ P0_27, NULL, NULL, NULL },    // 8: COPI  ← MOSI
{ P0_11, NULL, NULL, NULL },    // 9: SCLK  ← SCK
```

---

#### Layer 4 — `libraries/SPI/SPI.cpp` (global `SPI` object construction)

At the end of `SPI.cpp`, the global singleton is created with the PinNames
from the variant header:

```cpp
// compiled when SPI_HOWMANY > 0
arduino::MbedSPI SPI(SPI_MISO, SPI_MOSI, SPI_SCK);
//                    p28,      p27,      p11
```

`MbedSPI(int miso, int mosi, int sck)` converts these through
`digitalPinToPinName()` and stores the results as private `PinName`
members `_miso`, `_mosi`, `_sck`.

---

#### Layer 5 — `MbedSPI::begin()` (peripheral instantiation)

`SPI.begin()` (called from `DSPIClass::begin()`) creates the underlying
Mbed `SPI` master object:

```cpp
void arduino::MbedSPI::begin() {
    dev->obj = new mbed::SPI(_mosi, _miso, _sck);
    //                        P0.27,  P0.28, P0.11
}
```

This is the point at which the nRF52 SPI peripheral is actually
configured for those pads.

---

#### Layer 6 — `beginTransaction()` / `transfer()` (runtime operation)

```cpp
void arduino::MbedSPI::beginTransaction(SPISettings settings) {
    dev->obj->format(8, settings.getDataMode());   // frame width + CPOL/CPHA
    dev->obj->frequency(settings.getClockFreq());  // bus clock Hz
}

uint8_t arduino::MbedSPI::transfer(uint8_t data) {
    uint8_t ret;
    dev->obj->write((const char*)&data, 1, (char*)&ret, 1);
    return ret;
}
```

`beginTransaction()` is called in `ADS1299_Library::csLow()` with
`ADS_SPI_SETTINGS` (8 MHz, MSBFIRST, SPI_MODE1 / CPOL=0 CPHA=1),
and `endTransaction()` is called in `csHigh()`.

---

### Complete pin mapping

| Arduino pin | nRF52832 pad | SPI role             | `pinDef.h` macro |
|:-----------:|:------------:|----------------------|-----------------|
| **6**       | P0.29        | CS (SW `digitalWrite`) | `SPI_CS`       |
| **7**       | P0.28        | MISO (CIPO)          | `SPI_MISO`      |
| **8**       | P0.27        | MOSI (COPI)          | `SPI_MOSI`      |
| **9**       | P0.11        | SCK (SCLK)           | `SPI_SCK`       |
| **10**      | —            | /RESET (ADS1299)     | `ADS_RST_PIN`   |
| **11**      | —            | /DRDY  (ADS1299)     | `ADS_DRDY_PIN`  |

### Key observations

- `arduino::MbedSPI` does **not** expose `setMISO()` / `setMOSI()` / `setSCK()`.
  Pin assignment is locked at object construction and cannot be changed at runtime.
- The `SPI` singleton is constructed **before** `setup()` runs (static initialisation),
  so `SPI.begin()` only instantiates the Mbed `SPI` object — it does not reconfigure
  which pads are used.
- CS is **never** managed by the SPI library; it must be driven manually with
  `digitalWrite()`, wrapped in `SPI.beginTransaction()` / `SPI.endTransaction()`.
- The SPI settings used for every transaction:
  - Clock: **8 MHz** (`DSPI_CLOCK_HZ`)
  - Bit order: **MSBFIRST**
  - Mode: **SPI_MODE1** (CPOL=0, CPHA=1) — required by ADS1299 datasheet

---

*End of NOTE-001*

---

## NOTE-002 — ADS1299 Library: Full Comparison — iES Original vs Nicla Voice Port

| | |
|---|---|
| **Date** | 2026-03-07 |
| **Scope** | `firmware/src/ADS1299_Library.cpp` |
| **Author** | Review via GitHub Copilot |

---

### 1. Register Field Values

All register field values are **identical** between both versions. The following
constants are bit-for-bit preserved:

| Register | Field | Value | Meaning |
|----------|-------|-------|---------|
| `CONFIG1` | `ADS1299_REG_CONFIG1` | `0x96` | HR mode, 250 SPS, CLK output off |
| `CONFIG2` | `ADS1299_REG_CONFIG2` | `0xC0` | INT test, amp=±2.42mV, DC |
| `CONFIG3` | `ADS1299_REG_CONFIG3` | `0xE0` | BIAS on, internal BIASREF, buffer on |
| `CHnSET` default | `ACTIVECHANNEL` | `0x60` | Gain=24, SRB2 off, normal electrode |
| `CHnSET` off | `DEACTIVATEDCHANNEL` | `0x81` | Power-down, shorted input |
| `BIAS_SENSP/N` | all channels | `0xFF` | All channels contribute to BIAS |
| `LOFF_SENSP/N` | default | `0x00` | Lead-off detection disabled |
| `MISC1` | `SRB1` | `0x20` | SRB1 connected |

> ✅ **No register value differences found.**

---

### 2. SPI Protocol Logic

#### 2.1 Command Bytes
All ADS1299 SPI command bytes are **unchanged**:

| Command | Byte | Preserved? |
|---------|------|-----------|
| `WAKEUP` | `0x02` | ✅ |
| `STANDBY` | `0x04` | ✅ |
| `RESET` | `0x06` | ✅ |
| `START` | `0x08` | ✅ |
| `STOP` | `0x0A` | ✅ |
| `RDATAC` | `0x10` | ✅ |
| `SDATAC` | `0x11` | ✅ |
| `RDATA` | `0x12` | ✅ |
| `RREG` | `0x20 \| addr` | ✅ |
| `WREG` | `0x40 \| addr` | ✅ |

#### 2.2 RREG / WREG Protocol
Both versions follow the exact ADS1299 datasheet sequence:

```
RREG:  [ 0x20|addr ] [ 0x00|(n-1) ] [ dummy clocks × n ]
WREG:  [ 0x40|addr ] [ 0x00|(n-1) ] [ data byte × n   ]
```
> ✅ **No protocol logic differences found.**

#### 2.3 CS Control

| Aspect | iES Original | Nicla Voice Port |
|--------|-------------|-----------------|
| CS assert | `GPIO_write(SS, LOW)` | `digitalWrite(BOARD_ADS, LOW)` |
| CS release | `GPIO_write(SS, HIGH)` | `digitalWrite(BOARD_ADS, HIGH)` |
| SPI transaction guard | ❌ None | ✅ `SPI.beginTransaction()` / `SPI.endTransaction()` |

> ✅ Logically equivalent. The port **adds** transaction safety not present in the original.

---

### 3. Initialisation Sequence Flow

Both versions follow the same documented ADS1299 power-up sequence.
The flow is preserved step-for-step:

```
1.  Assert RESET low
2.  delay (power-on stabilisation)
3.  Release RESET high
4.  delay (t_reset_release)
5.  Send SDATAC           ← stop any continuous read left from power-up
6.  delay (t_sdecode)
7.  Write CONFIG3         ← enable internal bias buffer
8.  delay
9.  Write CONFIG1         ← data rate + HR mode
10. Write CONFIG2         ← test signal config
11. Write all CHnSET      ← channel gain / mux
12. Write BIAS_SENSP/N    ← bias drive
13. Write LOFF_SENSP/N    ← lead-off (disabled by default)
14. Write MISC1           ← SRB1
15. Send START            ← begin conversions
16. Send RDATAC           ← stream mode on
```

> ✅ **Sequence order is identical.**

---

### 4. Timing

#### 4.1 Delay Values — Direct Comparison

| Location in sequence | iES Original | Nicla Voice Port | Datasheet Minimum | Status |
|----------------------|-------------|-----------------|-------------------|--------|
| Power-on reset wait | `Task_sleep(1000)` → 1000 ms | `delay(1000)` → 1000 ms | ~500 ms (2^18 CLK @ 2.048 MHz) | ✅ |
| RESET pulse low | `usleep(9)` → 9 µs | `delayMicroseconds(9)` → 9 µs | ~1 µs (2 CLK) | ✅ |
| After RESET release | `usleep(100)` → 100 µs | `delayMicroseconds(100)` → 100 µs | ~9 µs (18 CLK) | ✅ |
| After SDATAC | `usleep(3)` → 3 µs | `delayMicroseconds(3)` → 3 µs | ~2 µs (4 CLK @ 2.048 MHz) | ⚠️ Marginal |
| Between register writes | `usleep(10)` → 10 µs | `delayMicroseconds(10)` → 10 µs | Not specified | ✅ |
| After CONFIG3 write | `Task_sleep(150)` → 150 ms | `delay(150)` → 150 ms | Not specified | ✅ |

> ⚠️ **After SDATAC:** 3 µs margin is thin. At 2.048 MHz CLK, 4 CLK cycles = 1.95 µs minimum.
> 3 µs gives only ~1 µs headroom. Consider increasing to `delayMicroseconds(10)` for safety.

#### 4.2 Timing Implementation Differences

| Mechanism | iES Original | Nicla Voice Port | Risk |
|-----------|-------------|-----------------|------|
| Millisecond sleep | `Task_sleep(ms)` — RTOS scheduler yields | `delay(ms)` — calls `mbed::ThisThread::sleep_for()` | ✅ Equivalent |
| Microsecond sleep | `usleep(us)` — POSIX, RTOS-aware | `delayMicroseconds(us)` — busy-wait DWT counter | ⚠️ Blocks CPU; interrupts still fire |
| SPI clock | 8 MHz (hardware SPI peripheral) | `SPISettings(8000000, MSBFIRST, SPI_MODE1)` | ✅ Identical |
| SPI mode | CPOL=0, CPHA=1 (MODE1) | `SPI_MODE1` | ✅ Identical |
| SPI bit order | MSB first | `MSBFIRST` | ✅ Identical |

#### 4.3 DRDY Timing — Most Critical Difference

This is the **most significant timing difference** between the two versions:

| Aspect | iES Original | Nicla Voice Port |
|--------|-------------|-----------------|
| DRDY handling | Hardware ISR → posts RTOS binary semaphore | **Removed** — must be handled in sketch |
| Task wake mechanism | `Semaphore_pend(BIOS_WAIT_FOREVER)` — zero busy-wait | Polling `digitalRead()` OR `attachInterrupt()` in sketch |
| Worst-case latency | Deterministic (RTOS scheduler) | Non-deterministic if polling |
| Sample period @ 250 SPS | 4 ms between DRDY pulses | 4 ms between DRDY pulses |
| Dropped sample risk | Very low (RTOS semaphore) | **High if polling**; low if `attachInterrupt()` used |

> ⚠️ **Action required:** Confirm that the sketch attaches an interrupt to `ADS_DRDY_PIN`
> (pin 11) via `attachInterrupt()`. Polling-based DRDY detection will cause sample
> drops at 250 SPS, especially with Mbed OS background tasks running.

---

### 5. Data Retrieval — `updateChannelData()`

| Aspect | iES Original | Nicla Voice Port |
|--------|-------------|-----------------|
| Buffered path | ✅ Present (mqueue-based, RTOS) | ❌ Removed |
| Non-buffered path | ✅ Present | ✅ Preserved (only path) |
| Status bytes read | 3 bytes (24-bit status word) | 3 bytes — identical |
| Channel bytes read | 3 bytes × 8 channels = 24 bytes | 3 bytes × 8 channels — identical |
| 24-bit sign extension | `>> 8` arithmetic shift | Identical |
| Scale factor | `4.5 / 24 / (2^23 - 1)` µV/LSB | Identical |

> ✅ The raw data assembly logic is **bit-for-bit identical**.

---

### 6. Removed Features Summary

| Feature | iES Original | Nicla Voice Port | Impact |
|---------|-------------|-----------------|--------|
| TI-RTOS kernel | ✅ Full RTOS | ❌ Removed | Replaced by Mbed OS |
| POSIX threads/mqueue | ✅ Present | ❌ Removed | Not needed without buffered path |
| `boardBeginADSInterrupt()` | ✅ Sets up DRDY ISR + semaphore | ❌ Removed | ⚠️ Must be done in sketch |
| LIS3DH accelerometer | ✅ Present | ❌ Removed | Hardware not on Nicla Voice |
| AGC / `cir_queue` | ✅ Present | ❌ Removed | Application-level feature |
| Buffered sample queue | ✅ mqueue | ❌ Removed | Sketch must handle sample buffering |
| IES debug macros | ✅ `IES_PRINTF` | ❌ → `Serial.print()` | ✅ Functionally equivalent |

---

### 7. Overall Verdict

| Category | Verdict |
|----------|---------|
| Register values | ✅ Identical |
| SPI command bytes | ✅ Identical |
| Protocol logic (RREG/WREG) | ✅ Identical |
| Initialisation sequence order | ✅ Identical |
| Timing values | ✅ Identical (⚠️ SDATAC marginal) |
| Timing implementation | ✅ Functionally equivalent |
| Data assembly (24-bit samples) | ✅ Bit-for-bit identical |
| DRDY handling | ⚠️ Moved to sketch — must use `attachInterrupt()` |
| Removed features | ℹ️ Intentional — platform incompatible |

> **Conclusion:** The port is faithful to the original ADS1299 protocol and timing.
> The only actionable item is confirming that `attachInterrupt(ADS_DRDY_PIN, ...)` is
> used in the sketch rather than polling, to avoid sample drops at 250 SPS.

---

## NOTE-003 — Byte Order: ADS1299 (Big-Endian SPI) vs nRF52832 (Little-Endian CPU)

| | |
|---|---|
| **Date** | 2026-03-24 |
| **Scope** | System-level — ADS1299 ↔ nRF52832 data path |

---

### Facts

| Component | Byte order | Source |
|-----------|-----------|--------|
| **ADS1299** — SPI output (status word + channel samples) | **Big-endian (MSB first)** | ADS1299 datasheet §8.5: "Data are shifted out MSB first" |
| **nRF52832** (Nicla Voice MCU) | **Little-endian** | ARM Cortex-M4 architecture; all nRF52-series devices are LE |

### Implication for the ADS1299 Library

The mismatch is fully handled in software by `updateChannelData()` using
explicit left-shift accumulation:

```cpp
// Byte 0 → MSB, Byte 2 → LSB  (big-endian as sent by ADS1299)
boardChannelDataInt[i] = (boardChannelDataInt[i] << 8) | inByte;
```

This byte-by-byte assembly is **architecture-agnostic** — it does not rely on
the CPU's native byte order — so the library is correct on the little-endian
nRF52832 without any byte-swapping.

> ✅ No `bswap`, `ntohs`, or other explicit endianness conversion is needed or
> present. The code is portable as written.

---

## NOTE-004 — ADS1299 Analog Supply Voltages and Absolute Limits

| | |
|---|---|
| **Date** | 2026-03-24 |
| **Scope** | ADS1299 hardware — power supply requirements |

---

### Nominal Analog Supply

| Rail | Voltage |
|------|---------|
| **AVDD** (positive analog supply) | **+2.5 V** |
| **AVSS** (negative analog supply) | **−2.5 V** |
| Total analog supply span (AVDD − AVSS) | **5.0 V** |

The ADS1299 requires a dual (split) analog supply centred around a mid-supply reference.
The digital supply (DVDD) is separate and typically +3.3 V.

---

### Absolute Maximum Ratings

Inputs and supply pins must never exceed the absolute maximum limits below, or
permanent device damage may result.

| Parameter | Absolute Minimum | Absolute Maximum |
|-----------|-----------------|------------------|
| Any analog input pin | AVSS − 0.3 V | AVDD + 0.3 V |
| AVSS supply pin | − | AVDD + 0.3 V |
| AVDD supply pin | AVSS − 0.3 V | − |

With nominal supplies (AVDD = +2.5 V, AVSS = −2.5 V):

| Limit | Value |
|-------|-------|
| Absolute minimum voltage on any analog pin | −2.5 − 0.3 = **−2.8 V** |
| Absolute maximum voltage on any analog pin | +2.5 + 0.3 = **+2.8 V** |


---

## NOTE-005 — Task Scheduling: Semaphore Wake-up + Cooperative Yield Pattern

| | |
|---|---|
| **Date** | 2026-04-04 |
| **Scope** | `test/SPI_Test/` — all RTOS task loop implementations |

---

### Background: Observed Problem

During streaming at 1001 SPS (ADS1299 at 1 kHz ODR), the following behaviour
was observed on the serial console:

```
[StreamMux]  Sample rate: 1001.0 SPS  |  EEG queue: 6/64  | Drops: 0
[UART TX]    Packet rate:  960.0 pkt/s | Queue: 63/64 items | Drops: 1557
```

StreamMux produced packets at 1001/s with no queue drops. The UART TX task
consumed at only 960/s — a 41 pkt/s deficit — causing the TX queue to fill
to 63/64 and 1557 cumulative drops.

---

### Root Cause Analysis

Three compounding causes were identified:

#### Cause A — Single pop + unconditional 1 ms sleep = hard throughput ceiling

Every task loop used `rtos::ThisThread::sleep_for(1ms)` unconditionally:

```cpp
// Before fix — uart_channel.cpp
while (!_stopRequested) {
    processTx();   // pops ONE packet
    processRx();
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(1));  // always sleeps
}
```

This hard-capped throughput at ~1000 pkt/s. With OS timer jitter the actual
rate fell to ~960 pkt/s — a 4% slip sufficient to cause continuous queue
overflow at 1001 SPS input.

#### Cause B — OS timer jitter

On Mbed OS the 1 ms SysTick can slip to 1–2 ms under load, reducing effective
throughput below the theoretical 1000 pkt/s ceiling.

#### Cause C — Synchronous Serial.print() inside the TX loop

Every ~1 second the debug stats block (5× `Serial.print()`) fired inside
`processTx()`. These synchronous USB CDC writes stalled the loop for hundreds
of microseconds, causing a burst of additional drops at the exact moment of
each stats printout.

---

### Fix #1 — Conditional sleep: yield when busy, block when idle

Replace the unconditional `sleep_for(1ms)` with a two-branch pattern:

```cpp
// After fix — uart_channel.cpp
if (_txQueue.isEmpty()) {
    // Queue empty: block on semaphore; woken immediately on next push()
    _txReady.try_acquire_for(std::chrono::milliseconds(1));
} else {
    // Queue non-empty: yield the time-slice cooperatively; do NOT spin
    rtos::ThisThread::yield();
}
```

`yield()` is mandatory in the non-empty branch. Without it, the loop spins
at MCU-maximum speed polling `Serial.available()` (thousands of times/sec),
which corrupts the nRF52832 USB CDC driver state after a random interval of
a few seconds (observed as a complete MCU hang with LED blinking stopped).

---

### Fix #2 — INotifiable / setOwner() wake-up at the IQueue abstraction layer

`FifoQueue<T,N>` holds a non-owning `INotifiable*` back-pointer to its owner task.
After releasing the mutex in `push()`, if `_owner` is set and the fill level has
reached `TASK_WAKE_THRESHOLD_PCT` (default 0 = every push), it calls:

```cpp
// fifo_queue.h — FifoQueue::push() (after _mutex.unlock())
if (_owner) {
    _owner->notify();   // INotifiable interface — implemented by BaseTask
}
```

`BaseTask` implements `INotifiable` by releasing a binary semaphore:

```cpp
// task.cpp — BaseTask::notify()
void BaseTask::notify() {
    _wakeSem.release();   // rtos::Semaphore — count capped at 1 (binary)
}
```

Each consumer task calls `queue.setOwner(this)` in its constructor to register itself:

```cpp
// PacketiserTask constructor — all three input queues wake the same task
_eegQueue.setOwner(this);
_responseQueue.setOwner(this);
_mlQueue.setOwner(this);

// GatewayTask constructor
_dataQueue.setOwner(this);
_cmdFromUartQueue.setOwner(this);
_cmdFromBleQueue.setOwner(this);

// UartChannelTask constructor
_txQueue.setOwner(this);

// CommandHandlerTask constructor
_cmdQueue.setOwner(this);
```

Consumer tasks idle via `sleepUntilNotified()` (defined in `BaseTask`):

```cpp
// task.h — BaseTask::sleepUntilNotified()
void BaseTask::sleepUntilNotified(uint32_t timeout_ms) {
    _wakeSem.try_acquire_for(std::chrono::milliseconds(timeout_ms));
}
```

The `notify()` release is performed **after** `_mutex.unlock()`, not inside the
locked section, to avoid holding the queue mutex while entering the OS semaphore layer.

---

### Resulting Wake-up Chain

```
ADS1299 DRDY ──► ISR ──► _drdySemaphore.release()
                               │
                    EegAcquisitionTask wakes (osPriorityRealtime)
                    distribute() ──► _eegQueue.push() ──► _owner->notify()
                                                               │
                                                PacketiserTask wakes (osPriorityAboveNormal)
                                                distribute() ──► _dataQueue.push() ──► _owner->notify()
                                                                                            │
                                                                               GatewayTask wakes (osPriorityNormal)
                                                                               _txQueue.push() ──► _owner->notify()
                                                                                                        │
                                                                                          UartChannelTask wakes (osPriorityNormal)
```

Every task wakes within one OS scheduler tick of its upstream producer
depositing data. No task spins without yielding to the OS.

---

### Pattern Applied Uniformly

The same two-branch pattern is used in every task loop:

| Task | Idle path | Busy path |
|------|-----------|-----------|
| `UartChannelTask` | `sleepUntilNotified(1)` | loop continues without sleeping |
| `PacketiserTask` | `sleepUntilNotified(1)` | loop continues without sleeping |
| `GatewayTask` | `sleepUntilNotified(1)` | loop continues without sleeping |
| `CommandHandlerTask` | `sleepUntilNotified(1)` | loop continues without sleeping |
| `EegAcquisitionTask` | `_drdySemaphore.acquire()` (no timeout; DRDY-driven) | — |

`EegAcquisitionTask` is purely interrupt-driven and was already correct; no
changes were required.

---

### Key Design Decisions

1. **`sleepUntilNotified(1)` not `sleepUntilNotified(0)`**: the 1 ms timeout is a safety
   fallback so tasks remain responsive to `_stopRequested` even if a `notify()` call is
   somehow missed (e.g. during task tear-down).

2. **`notify()` after mutex unlock**: `FifoQueue::push()` calls `_owner->notify()` after
   releasing `_mutex`, not inside the locked section.  This prevents the woken consumer
   from immediately contending on a mutex still held by the producer.

3. **Multiple queues, one owner task** (PacketiserTask, GatewayTask): tasks that consume
   from more than one queue register themselves (`setOwner(this)`) on all their input
   queues.  Any queue becoming non-empty calls `notify()` on the same `BaseTask` instance,
   waking the consumer once; the run loop then drains whichever queues have data.

4. **`IQueue::setOwner` default no-op**: the virtual method has an empty default body in
   `IQueue<T>`, so queues not wired to any task compile and behave correctly with no owner.

*End of NOTE-005*

---

## NOTE-006 — UartChannelTask Crash: Stack Overflow from Serial.print(float) in Mbed USB CDC

| | |
|---|---|
| **Date** | 2026-04-04 |
| **Scope** | `test/SPI_Test/uart_channel.cpp` — `UartChannelTask` |
| **Author** | Review via GitHub Copilot |

---

### Symptom

The firmware ran correctly for an indeterminate period (seconds to minutes) then
crashed hard — no assertion message, no graceful stop, just a silent hardfault /
watchdog reset. The crash time was non-deterministic, ruling out a simple logic
bug and pointing to a memory-corruption or resource-exhaustion root cause.

---

### Root Cause 1 (primary) — Stack overflow in `UartChannelTask`

**File:** `uart_channel.cpp`, constructor  
**Original stack size:** `1024` bytes  
**Fixed stack size:** `2048` bytes

`Serial.print(float)` on Mbed USB CDC (`BufferedSerial` path) internally routes
through `printf` float formatting, which allocates roughly **300–400 bytes on
the call stack** per invocation. In a single pass of the `run()` loop both
`processTx()` and `processRx()` can each call `Serial.print(float)` (TX packet
rate and RX byte rate), pushing instantaneous stack consumption above 700 B on
a 1024 B stack. Additionally, any ISR or context-switch overhead that happens
to fire during a print pushes the frame even higher. The result is silent stack
overflow and memory corruption of whatever neighbour allocation sits below the
stack canary.

#### Rule derived

> **Any Mbed task that calls `Serial.print(float)` — even once — must budget at
> least 512 B for that single call.  Two concurrent `print(float)` call chains
> (direct + indirect) require a stack of at minimum 2048 B.  Default to 2048 B
> unless the task is provably print-free.**

---

### Temporary Fix Applied (2026-05-03) — Non-blocking debug via `debugTryPrint()`

**Root cause confirmed during 1 kSPS streaming run:**  
All five RTOS tasks contained `#ifdef DEBUG_ENABLE` blocks that called
`gSerialMutex.lock(); Serial.print(); gSerialMutex.unlock()`. Under continuous
EEG streaming the USB CDC TX buffer saturates quickly. When the buffer is full,
`Serial.print()` blocks inside the Mbed `BufferedSerial` layer waiting for
drain. Every task that tries to acquire `gSerialMutex` then stalls, and the
WDT fires before any task checks in.

**Fix:** Replaced every blocking debug-print site with a `debugTryPrint()`
helper (defined in `task.h` / `task.cpp`) that uses `gSerialMutex.try_lock()`.
If the lock is unavailable (i.e. another task is printing or CDC is congested),
`try_lock()` returns `false` immediately and the log line is silently dropped.
The calling task is never blocked, so the WDT is always serviced.

```cpp
// task.cpp
void debugTryPrint(const char* msg) {
    if (gSerialMutex.try_lock()) {
        Serial.print(msg);
        gSerialMutex.unlock();
    }
    // else: log dropped — task continues unblocked
}
```

All `#ifdef DEBUG_ENABLE` guards remain in place; only the inner
`gSerialMutex.lock() / Serial.print() / unlock()` triple is replaced by
`debugTryPrint()`.

**Trade-off:** Debug logs may be dropped under heavy streaming load. The EEG TX
path (`Serial.write()` in `uart_channel.cpp`) does not hold `gSerialMutex` and
is unaffected.


---

### Files changed

| File | Change |
|---|---|
| `firmware/src/uart_channel.cpp` | Stack 1024→2048; all blocking debug prints replaced with `debugTryPrint()` |
| `firmware/src/task.h` | Added `debugTryPrint()` declaration; `gSerialMutex` `extern` declaration — both inside `#ifdef DEBUG_ENABLE` |
| `firmware/src/task.cpp` | `gSerialMutex` definition; `debugTryPrint()` implementation — both inside `#ifdef DEBUG_ENABLE` |
| `firmware/src/eeg.cpp` | Blocking debug prints replaced with `debugTryPrint()` |
| `firmware/src/packetiser.cpp` | Blocking debug prints replaced with `debugTryPrint()` |
| `firmware/src/gateway.cpp` | Blocking debug prints replaced with `debugTryPrint()` |
| `firmware/src/cmd_handler.cpp` | Blocking debug prints replaced with `debugTryPrint()` |
| `firmware/src/src.ino` | Heap-free report in `loop()` replaced with `debugTryPrint()` |
| `firmware/src/config.h` | `DEBUG_ENABLE` set to `1`; added `UART_BACKPRESSURE_SLEEP_MS 5` |

*End of NOTE-006*

---

## NOTE-007 — nRF52832 Has No Hardware EEPROM: Flash-Emulated Persistent Storage

**Date:** 2026-05-09  
**Scope:** `firmware/src/` — persistent configuration design  
**Trigger:** Design of PersistentConfig module for output-mode, gain, ODR, channel mask

### Summary

The nRF52832 SoC on the Arduino Nicla Voice **has no dedicated EEPROM peripheral**.
It contains only:
- 512 KB internal NOR flash (program storage)
- 64 KB SRAM (volatile)

Persistent storage must therefore be emulated in flash.

---

### FlashIAP Direct Implementation

The firmware uses Mbed OS `FlashIAP` (`#include "FlashIAP.h"`) directly — not
the `<EEPROM.h>` Arduino wrapper. The 18-byte config block is stored at the
start of the **last 4 KB sector** of the 512 KB internal NOR flash. The sector
address is computed at first use:

```cpp
// persistent_config.cpp
#include "FlashIAP.h"
static mbed::FlashIAP s_flash;

// Flash I/O helpers (init/deinit per call to release peripheral during streaming):
s_flash.init();
s_flash.read(&layout, configAddr, sizeof(EepromLayout));
s_flash.deinit();

// Write path (read-before-write, then erase + program full 4 KB sector):
s_flash.read(s_sectorBuf, addr, sectorSize);
if (memcmp(s_sectorBuf + offset, src, sizeof(EepromLayout)) != 0) {
    s_flash.erase(addr, sectorSize);
    s_flash.program(s_sectorBuf, addr, sectorSize);
}
```

### Flash Endurance and Wear-Protection Strategy

nRF52832 internal flash is NOR flash rated for **~10,000 erase/write cycles per
page** (128 bytes/page on nRF52). The emulation layer manages page-level erase
internally; the application sees a flat byte array.

To protect against premature wear from frequent setting changes, the
`PersistentConfig` module uses a **read-before-write** strategy:

```
For each byte to persist:
    current = EEPROM.read(offset)
    if current != new_value:
        EEPROM.write(offset, new_value)
        readback = EEPROM.read(offset)
        if readback != new_value → flag EepromWriteError
EEPROM.commit()
```

This means settings that have not changed never touch flash.

### Integrity Protection

A `magic` word (`0xE1E50001`) at offset 0 and a CRC-16 over the entire struct
are stored with every write. On load:
- If magic mismatches → first boot or erased flash → load factory defaults, write
- If CRC mismatches → corruption → load factory defaults, write
- If schema version mismatches → future migration path

### EEPROM Layout (v1)

```
Offset  Size  Field                    Default
──────  ────  ─────────────────────────────────────────────────────────
 0       4    magic                    0xE1E50001
 4       1    schema_version           1
 5       1    output_mode              1 = iES µV (OutputMode::UV)  [see NOTE-008]
 6       1    sample_rate              ADS1299_Library::SAMPLE_RATE_1000
 7       1    downsampling_factor      4  (1 kSPS ÷ 4 = 250 SPS effective)
 8       4    channel_gain[4]          ADS_GAIN01 (×1)  [see NOTE-008]
12       1    channel_enable_mask      0b00001100 (CH3+CH4 only)
13       1    uart_enabled_at_boot     1
14       1    ble_enabled_at_boot      0
15       1    host_protocol_mode       0 = MODERN  [see NOTE-011; was "reserved"]
16       2    crc16 (over bytes 0–15)
──────────────────────────────────────────────────────────────────────
Total: 18 bytes
```

*End of NOTE-007*

---

## NOTE-008 — iES EEG Wire Format: µV Integer vs Raw ADC Codes, Default Gain and Output Mode

**Date:** 2026-05-09  
**Scope:** `firmware/src/packetiser.cpp`, `runtime_state.*`, `persistent_config.*`  
`code_references/iES_v0.3-master/ies_app/ies_task.cpp`

### Summary

The iES firmware sends **integer µV values** over BT-SPP in native mode.
Raw ADC codes are only sent in the separate OpenBCI-compatible mode.
Both use the same `uint24_to_buffer()` (big-endian 24-bit) packing — the
difference is the value stored, not the wire encoding.

---

### iES Conversion Path (verbatim from `ADS_processChannelData`)

```c
// From ADS_1299_Library.cpp:
double uV = boardChannelDataInt[channel_index] * SCALE_FACTOR_UV / getGainInt(N);
int32_t uV_int = round(uV);
sample_rec.channel_data[count] = uV_int;   // stored as int32_t

// SCALE_FACTOR_UV (from ADS1299_Library_Definitions.h):
#define SCALE_FACTOR_UV  0.5364418669   // = 4.5×10⁶ / (2²³ − 1)
```

The result is a **rounded integer number of µV**, stored in `channel_data[count]`
as `int32_t`, then serialized by `btspp_send_task_fxn` as:

```c
uint24_to_buffer(sample_rec.channel_data[count], &buf[index]);
// Packs lower 24 bits, big-endian: buf[0]=bits23:16, buf[1]=bits15:8, buf[2]=bits7:0
```

At gain=1, 1 LSB of wire value = 1 µV.
At gain=24, 1 LSB of wire value = 1 µV (scaling applied before packing).
The receiver always interprets the 24-bit field as integer µV.

### OpenBCI Mode (raw ADC codes)

```c
// openbci_compatible == true:
uint24_to_buffer(sample_rec.channel_data[count], &buf[index]);
// channel_data holds raw boardChannelDataInt (sign-extended 24-bit ADC count)
```

Same wire encoding, different semantic: receiver must apply the scale factor itself.

### Default Settings (iES `initialize_ads()`)

```c
defaultChannelSettings[GAIN_SET] = ADS_GAIN01;  // ×1 — NOT ×24
defaultChannelSettings[INPUT_TYPE_SET] = ADSINPUT_NORMAL;
// ...
writeChannelSettings();   // immediately programs ×1 to all channels over SPI
```

#### Hardware vs. software default — critical distinction

The **ADS1299 chip** powers on with CHnSET bits[6:4] = `0b110` = **×24** (hardware register
default, ADS1299 datasheet Table 12). The iES software immediately overwrites this in
`initialize_ads()` by calling `writeChannelSettings()`, programming **×1** to every channel.
If firmware crashes before `writeChannelSettings()` executes, the hardware runs at ×24.

#### Adaptive Gain Control (AGC) — disabled

AGC is disabled in two independent places in iES v0.3:

```c
// ADS_1299_Library.cpp (constructor-level):
adaptive_gain = false;

// ies_task.cpp (application startup):
ads1299.adaptive_gain = false;
```

The only ×24 reference in `ies_task.cpp` is fully commented out — a developer experiment:
```c
//  openbci.channelSettings[3 - 1][GAIN_SET] = ADS_GAIN24;  // commented out, not shipped
//  openbci.writeChannelSettings(3);
```

**iES v0.3 ships with gain = ×1 on all channels, AGC disabled, no runtime gain change.**

#### Nicla port — already compatible

`firmware/src/ADS1299_Library.cpp` line 133:
```cpp
defaultChannelSettings[GAIN_SET] = ADS_GAIN01;  // identical to iES
```
`src.ino` never modifies `GAIN_SET`. **Gain is already compatible with iES.**

#### How to confirm at runtime

**Register readback** — add to `setup()` after `ads1299.begin()`:
```cpp
byte ch3set = ads1299.RREG(CH3SET, BOARD_ADS);
Serial.print("CH3SET = 0x"); Serial.println(ch3set, HEX);
// Expected: 0x01 (bits[6:4]=000 = ×1, bit0=1 = SRB2 on)
// If 0x61: hardware at power-on default ×24 — writeChannelSettings() did not run
```

**Known-signal amplitude check** — at 1 mV peak input, gain ×1, VREF = 4.5 V:
$$N = \frac{1\times10^{-3}}{4.5} \times 2^{23} \approx 1863 \text{ counts}$$
If ~44,700 counts observed, the hardware gain is still ×24.

### Default Output Mode (iES)

```c
static bool openbci_compatible = false;  // iES native (µV) is the default
```

iES native mode (µV integer) is the default. OpenBCI mode is opt-in.

### Implications for Nicla Port

- **Current firmware** (as of 2026-05-09): `serialiseEeg()` sends **raw ADC codes**
  (sign-extended 24-bit, no µV conversion). This matches OpenBCI behaviour, not
  iES native behaviour.
- **Required change**: add `OutputMode` to `RuntimeState` + `PersistentConfig`;
  when `OUTPUT_MODE_IES`, apply `round(raw * SCALE_FACTOR_UV / gain)` in
  `serialiseEeg()` before packing.
- **Wire format stays identical** — the 24-bit big-endian packing is unchanged;
  only the value differs.
  are already µV; when mode = OpenBCI, multiply by `0.5364418669 / gain`.

*End of NOTE-008*

---

## NOTE-009 · UART Test Suite Status and Known Flakiness (2026-06-19)

**Date:** 2026-06-19 (updated 2026-07-04)  
**Scope:** `firmware/src/uart_channel.h/.cpp` — UART protocol and baud behaviour  
**Firmware:** `ADS1299NiclaFW` (`src.ino`) v0.2.0 @ **460800 baud** USB CDC

### Summary

**UART Phase 4 validation complete (2026-07-04).** All four host test scripts pass @ **460800 baud**. BLE implementation has **not started** — next milestone is BLE Stage 1 per `ble_channel_design.md` §9.2.

### Streaming command gate (firmware, 2026-06-19)

While streaming, **only** these commands are accepted:

| Command | Byte |
|---|---|
| `STOP_STREAMING` | `'s'` |
| `HEARTBEAT` | `'.'` |
| `TIME_SYNC` | `0x74` |

`QUERY_STATUS`, `GET_VERSION`, config setters, `START`, etc. → `ERR_NOT_ALLOWED`. Query config before `START` or after `STOP`.

### Baud rate

| Baud | Result |
|---|---|
| 1 000 000 | ❌ Corrupts bytes (e.g. ×17 gain) |
| 921 600 | ⚠️ Intermittent on this Windows CDC link |
| 460 800 | ✅ **Production choice** — all four scripts pass (2026-07-04); streaming stress DS×1 @ 1000 wire fps |
| 115 200 | ✅ Stable for commands; ❌ insufficient for DS×2 stress (500 wire fps) |

`config.h` (as of 2026-07-09): `#define SERIAL_BAUD_MODERN 460800` /
`#define SERIAL_BAUD_OPENVIBE 115200` — the latter is now runtime-selectable
via `CMD_SET_HOST_MODE` without reflashing; see NOTE-011.

*End of NOTE-009*

---

## NOTE-010 · Host Debatching and Timestamp Reconstruction (UART vs BLE)

**Date:** 2026-07-04  
**Scope:** Host software development — UART (`UartChannelTask`), BLE (`BleChannelTask`), all Windows receivers  
**Author:** Session handover / streaming test investigation

### Summary

**UART firmware does not batch frames.** `UartChannelTask::processTx()` emits one
IES wireframe per `Serial.write()` call. There is no firmware-side coalescing on
the UART path.

**Windows USB CDC does batch on the host.** The virtual COM driver delivers bytes
to the application in bursts whose timing reflects OS/driver scheduling, not the
device’s 250 Hz EEG wire rate. A 500 ms gap between `read()` calls does **not**
imply 500 ms of missing EEG data.

**BLE (future) batches on the device** — multiple frames per notification — but
the host still must parse frame-by-frame inside each payload.

### Mandatory host behaviour

Any Windows receiver — **UART or BLE** — must implement:

| Responsibility | Method | Do **not** use |
|----------------|--------|----------------|
| Frame extraction | Stateful parser over accumulated bytes | One read = one frame |
| Drop / gap detection | IES **frame counter** (byte 1), adjusted for non-EEG frames between EEG frames | Host wall-clock or inter-read interval |
| Sample timestamps | **TIME_SYNC** (type 7): `device_ts_us` + wire-sample index × period | USB/BLE arrival time |

### Reference code


### Documentation cross-refs

- `ies_message_protocol.md` §11 — Host Receiver Requirements (canonical host-dev guide)
- `ble_channel_design.md` §7 — Central Application Requirements
- `firmware_architecture.md` §8.1 — TIME_SYNC design rationale

### Incident that motivated this note

`test_streaming.py` continuity test failed with “511 ms max gap” while frame rate
was healthy (~254 fps). Root cause: test stamped all frames in a USB read chunk
with the same host `perf_counter()` time. Fixed by switching to frame-counter
continuity and plotting against reconstructed TIME_SYNC timestamps.

*End of NOTE-010*

---

## NOTE-011 · HostProtocolMode: OpenVIBE Legacy Compat Mode and Runtime Baud Switching

**Date:** 2026-07-09  
**Scope:** `runtime_state.h/.cpp`, `cmd_handler.h/.cpp`, `uart_channel.h/.cpp`,
`packetiser.cpp`, `persistent_config.h/.cpp`, `config.h`, `src.ino`

### Summary

Added `HostProtocolMode` (`MODERN` default, `LEGACY_IES` opt-in) so
ADS1299NiclaFW can be driven **unmodified** by the original OpenVIBE
`CDriveriES` driver. Full protocol-level rationale and the wire-format
differences live in `ies_message_protocol.md` §12 — this note covers the
firmware/implementation details that doc doesn't.

### Why not just "always emit iES-pure frames"?

`MODERN` mode preserves `RESPONSE` acks and `TIME_SYNC` frames that host software depends on for correctness and timestamp reconstruction (NOTE-010). `LEGACY_IES` suppresses them to match the original iES contract OpenVIBE expects. Hence a runtime-switchable flag rather than a permanent behavior change.

### Where each piece of the OpenVIBE incompatibility was found

All four issues in `ies_message_protocol.md` §12.1 were confirmed by reading
the driver source directly:
- Time-sync framing bug: `resetBoard()` lines ~640–650 (`strlen()` on a
  6-byte buffer ending in `'\0'`).
- Fixed 2-channel, type-nibble-blind parser: `parseByte()`'s
  `ParserAutomaton_SampleNumberReceived` case discards byte 2 entirely.
- Fire-and-forget commands: `sendCommand(..., bWaitForResponse=false)` call
  sites for `.` (heartbeat) and `t` (time sync) in the connect/loop paths.
- Baud: `CBR_115200` in the port-open `SetCommState()` call.

### Implementation notes not obvious from the code alone

1. **The `CMD_SET_HOST_MODE` ack is the one exception to "suppress all
   RESPONSE frames in legacy mode."** Without this exception, a host
   switching *into* legacy mode would never get positive confirmation — by
   the time `PacketiserTask` serialises the response, `RuntimeState` already
   reports `LEGACY_IES`, so the generic suppression check would silently eat
   the very ack the switch depends on. `packetiser.cpp` special-cases
   `resp.cmd_id == CMD_SET_HOST_MODE`.
2. **Baud switching must happen after the ack is flushed, not before.**
   `CommandHandlerTask::cmdSetHostMode()` only *arms* a pending baud change
   (`UartChannelTask::requestBaudChange()`); `UartChannelTask::run()` applies
   it (`Serial.end()` / `Serial.begin()`) only once `_txQueue.isEmpty()` —
   i.e. after the ack WireFrame has actually been written to `Serial`.
   Applying it any earlier would flip the CDC line coding while the ack is
   still in flight at the old baud.
3. **Boot-time ordering matters.** `src.ino::setup()` used to call
   `Serial.begin(SERIAL_BAUD_MODERN)` before `PersistentConfig::load()`. That
   order had to flip — `PersistentConfig::load()` is flash-only (no `Serial`
   dependency) and now runs first so the persisted `host_protocol_mode` can
   pick the correct boot baud. The setup-log print statements that used to
   run inline with `PersistentConfig::load()` are now deferred (their result
   captured in a local `bool`) until after `Serial.begin()`.
4. **`CMD_DEMO`'s full-state reset would otherwise silently exit legacy
   mode.** `cmdDemo()` calls `RuntimeState::initialize()` to restore factory
   defaults, which resets `_hostProtocolMode` to `MODERN` — but without also
   triggering a baud switch, leaving `RuntimeState` and the physical `Serial`
   baud out of sync. Fixed by snapshotting and restoring
   `getHostProtocolMode()` around the `initialize()` call in `cmdDemo()`.
5. **Native USB-CDC, not a bridged UART.** `uart_channel.h`/`.cpp` and
   `config.h` §4 already documented this (`"USB FS ceiling ≈ 125 KB/s
   regardless of baud rate"`), which is why `Serial.end()`/`Serial.begin()`
   at runtime carries no physical bit-timing risk — the risk is purely
   protocol-level (a host that hasn't reconfigured its own port yet won't
   understand bytes sent at the new line coding).
6. **115200 baud caps the usable wire rate at ~250 SPS × 2ch, so legacy mode
   forces a true 250 SPS ADC rate with no decimation** (`ies_message_protocol.md`
   §12.3a), rather than the `MODERN` factory default of 1000 SPS ÷
   `downsampling_factor=4` (same *effective* 250 SPS, but via 4x oversampling
   whose digital-filter response doesn't match OpenVIBE's fixed 250 Hz
   declaration). `CommandHandlerTask::enforceLegacyDefaults()` centralises
   this — plus the pre-existing channel-mask/output-mode normalization — and
   is called both from `cmdSetHostMode()` and (again) from `cmdDemo()`, since
   `RuntimeState::initialize()` in point 4 above also resets
   `sample_rate`/`downsampling_factor` to their factory defaults.
   `CMD_SET_ODR`/`CMD_DOWNSAMPLING` reject any other value with
   `ERR_NOT_ALLOWED` while `LEGACY_IES` is active.

*End of NOTE-011*
