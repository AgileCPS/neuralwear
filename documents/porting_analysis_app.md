# iES → Nicla Voice Porting Analysis

**Source project**: `iES_v0.3-master` (MSP432P4111 + TI-RTOS / SYS/BIOS)  
**Target platform**: Arduino Nicla Voice (nRF52832 + Mbed OS 6 / CMSIS-RTOS v2 RTX5)  
**Date**: March 2026

---

## Table of Contents

1. [Application Layer System Architecture](#1-application-layer-system-architecture)
2. [Reusable Verbatim (zero or minimal changes)](#2-reusable-verbatim)
3. [Must-Port Components (RTOS/Platform substitutions)](#3-must-port-components)
4. [Staged Porting Plan with Validation](#4-staged-porting-plan-with-validation)
5. [FIFO Queue Buffer Design for Mbed OS 6](#5-fifo-queue-buffer-design-for-mbed-os-6)

---

## 1. Application Layer System Architecture

### 1.1 iES Architecture (MSP432 / TI-RTOS)

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Application Layer                            │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │ btspp_recv_  │  │ btspp_send_  │  │      ies_task_fxn        │  │
│  │  task_fxn    │  │  task_fxn    │  │  (coordinator / state    │  │
│  │  priority 2  │  │  priority 2  │  │   machine,  priority 3)  │  │
│  │             ─┼──►(ies2btspp_  │  │                          │  │
│  │  command     │  │  mqueue +    │  │  Event_pend(            │  │
│  │  parser      │  │  btspp_send_ │  │    ADS_NEW_DATA         │  │
│  │  ──Event_post►  │  sem)        │  │    IMU_NEW_DATA         │  │
│  └──────────────┘  └────┬─────────┘  │    EDA_NEW_DATA         │  │
│                         │ BPLib.send │    BATT_NEW_DATA        │  │
│  ┌──────────────┐        └───────────►    START/STOP_STREAMING) │  │
│  │ imu_task_fxn │  periodic 50 Hz  │                          │  │
│  │  priority 2  ├─(imu2ies_mqueue)─►  EEG pipeline:           │  │
│  │ sem_wait(    │                  │    updateChannelData()    │  │
│  │  imu_sem)    │                  │    ADS_processChannelData │  │
│  └──────────────┘                  │    downsampling          │  │
│                                    │    mq_send(ies2btspp)    │  │
│  ┌──────────────┐                  │    sem_post(btspp_send)  │  │
│  │ eda_task_fxn │  periodic 50 Hz  │                          │  │
│  │  priority 2  ├─(eda2ies_mqueue)─►  SD card write           │  │
│  │ sem_wait(    │                  └──────────────────────────┘  │
│  │  eda_sem)    │                                                 │
│  └──────────────┘                                                 │
│                                                                     │
│  ┌─────────────────────────────────┐                               │
│  │  ads_interrupt_buffering_fxn    │   (hidden infrastructure)     │
│  │  (priority high)                │                               │
│  │  mq_receive(ads_interrupt_queue)│                               │
│  │  → Event_post(ADS_NEW_DATA)     │                               │
│  └────────────▲────────────────────┘                               │
│               │ mq_send (ISR-safe, O_NONBLOCK)                     │
│  ─────────────┼──────────────────────────────────────────────────  │
│  ADS_DRDY_Service()  ← GPIO ISR (hardware interrupt)               │
└─────────────────────────────────────────────────────────────────────┘

Inter-task primitives (TI-RTOS / POSIX):
  Event_Handle   ies_event_handle   — bitmask events (15 bits used)
  mqd_t          ies2btspp_mqd      — 512 × data_sample_rec
  mqd_t          imu2ies_mqd        — 128 × data_sample_rec
  mqd_t          eda2ies_mqd        — 128 × data_sample_rec
  mqd_t          batt2ies_mqd       — 8   × data_sample_rec
  mqd_t          ads_interrupt_queue — ADS_DRDY timestamp records
  sem_t          btspp_send_sem     — gate between ies_task and btspp_send
  sem_t          imu_sem            — gate from 20 ms timer to imu_task
  sem_t          eda_sem            — gate from 20 ms timer to eda_task

Sampling timer:
  Timer_Handle   20 ms periodic → sampling_timer_handler() ISR
  ISR posts imu_sem and eda_sem (sem_post is POSIX async-signal-safe)
```

### 1.2 Target Architecture (Nicla Voice / Mbed OS 6)

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Application Layer                            │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │
│  │  ble_recv_   │  │  ble_send_   │  │   coordinator_task       │  │
│  │  thread      │  │  thread      │  │   (loop() / main thread) │  │
│  │  Normal prio │  │  Normal prio │  │                          │  │
│  │             ─┼──►(ads_mailbox  │  │  EventFlags::wait_any(   │  │
│  │  command     │  │  ims_mailbox │  │    ADS_DATA_READY        │  │
│  │  parser      │  │  eda_mailbox │  │    IMU_DATA_READY        │  │
│  │  ──flags_set─►  │  etc.)       │  │    EDA_DATA_READY        │  │
│  └──────────────┘  └──────────────┘  │    CMD_START / CMD_STOP) │  │
│                                      │                          │  │
│  ┌──────────────┐                    │  EEG pipeline:           │  │
│  │ acquisition_ │                    │    (pulls from           │  │
│  │ thread       │                    │     ads_mailbox)         │  │
│  │  High prio   │ ─(ads_mailbox)────►│    processChannelData()  │  │
│  │ EventFlags:: │                    │    downsampling          │  │
│  │ wait_any(    │                    │    push to ble_mailbox   │  │
│  │  DRDY_FLAG)  │                    └──────────────────────────┘  │
│  └──────▲───────┘                                                   │
│         │ EventFlags::set (ISR-safe)                                │
│  ───────┼───────────────────────────────────────────────────────── │
│  DRDY_ISR()  ← attachInterrupt(pin 11, FALLING)                    │
└─────────────────────────────────────────────────────────────────────┘

Inter-task primitives (Mbed OS 6 / CMSIS-RTOS v2):
  rtos::EventFlags  ies_flags         — replaces Event_Handle
  rtos::Mail<ADS_Frame, 64>           — replaces ads_interrupt_queue
  rtos::Mail<data_sample_rec, 64>     — replaces ies2btspp_mqd
  rtos::Mail<data_sample_rec, 32>     — replaces imu2ies_mqd / eda2ies_mqd
  rtos::Semaphore   imu_sem(0, 1)     — replaces POSIX sem_t imu_sem
  rtos::Semaphore   eda_sem(0, 1)     — replaces POSIX sem_t eda_sem
  mbed::Ticker      sampl_ticker      — replaces TI Timer_Handle (20 ms)
```

### 1.3 RTOS Primitive Mapping Table

| iES (TI-RTOS / POSIX)                     | Nicla Voice (Mbed OS 6)                            | ISR-safe? |
|--------------------------------------------|----------------------------------------------------|-----------|
| `Event_Handle` + `Event_post()`            | `rtos::EventFlags::set(flags)`                     | ✅ Yes    |
| `Event_pend(handle, OR_mask)`              | `EventFlags::wait_any(flags)`                      | ❌ No     |
| `mqd_t` + `mq_send()` (typed message)      | `rtos::Mail<T, N>::try_alloc()` + `put()`          | `try_alloc` ✅ |
| `mqd_t` + `mq_receive()` (typed message)  | `Mail<T, N>::try_get()` + `free()`                 | ❌ No     |
| `sem_t` + `sem_post()` (binary)            | `rtos::Semaphore::release()`                       | ✅ Yes    |
| `sem_t` + `sem_wait()` (blocking)          | `rtos::Semaphore::acquire()`                       | ❌ No     |
| `pthread_create()` with `sched_priority`   | `rtos::Thread(osPriority, stack_size)`             | N/A       |
| `Timer_Handle` periodic callback           | `mbed::Ticker::attach(fn, period)`                 | Runs in IRQ |
| `GPIO_setCallback()` + `GPIO_enableInt()`  | `attachInterrupt(pin, fn, FALLING)`                | N/A       |
| `SPI_open()` / `SPI_transfer()`            | `SPI.begin()` / `SPI.transfer()` (Arduino)         | N/A       |
| `I2C_open()` / `I2C_transfer()`            | `Wire.begin()` / `Wire.requestFrom()` (Arduino)    | N/A       |
| `ADC_open()` / `ADC_convert()`             | `analogRead()` (Arduino)                           | N/A       |
| `clock_settime()` / `time()`               | `set_time()` / `time()` (Mbed OS POSIX time)       | N/A       |

---

## 2. Reusable Verbatim

These files require **zero or trivial changes** (include path fixes only) and can be copied as-is to the Nicla project.

### 2.1 ADS1299 Driver — Already Ported ✅

The SPI_Test folder already contains a fully ported driver:

| File | Location | Notes |
|------|----------|-------|
| `ADS1299_Library.h/.cpp` | `test/SPI_Test/` | TI-RTOS headers removed; `boardBeginADSInterrupt()` removed; `printAll()` → `Serial.print()`; `csLow/High()` wrap `SPI.beginTransaction/endTransaction()` |
| `ADS1299_Definitions.h` | `test/SPI_Test/` | TI GPIO enums replaced by Arduino pin numbers from `pinDef.h` |
| `DSPI.h/.cpp` | `test/SPI_Test/` | TI `SPI_open/transfer/close` → Arduino `SPI.begin/transfer` thin wrapper |
| `pinDef.h` | `test/SPI_Test/` | Pin number definitions for Nicla Voice header |

**All ADS1299 register and SPI protocol logic is byte-for-byte identical to iES.**

### 2.2 Binary Encoding Utilities — Verbatim ✅

| File | What it does | Dependency |
|------|-------------|------------|
| `ies_misc.h` / `ies_misc.cpp` | LSByte-first buffer ↔ integer conversions (`uint16_to_buffer`, `uint24_to_buffer`, `buffer_to_float`, etc.) | Pure C; `<stdint.h>` only |
| `checksum.h` / `crc8.cpp` | CRC-8/SHT75 (Sensirion lookup table) — used for time-sync command integrity | Pure C; `<stdint.h>`, `<stddef.h>` only |
| `checksum.h` / `crc16.cpp` | CRC-16/IBM — available but not actively used in iES stream | Pure C |

Copy these four files verbatim. No platform headers. MIT licensed (`crc*.cpp`) / LGPL (`ies_misc.cpp`).

### 2.3 Protocol Constants and Data Structures — Verbatim ✅

Lift the following directly from `ies_task.h` / `ies_task.cpp`, removing the TI-RTOS include lines:

```cpp
// Command bytes (identical on Nicla)
#define IES_STREAM_START         'b'
#define IES_STREAM_STOP          's'
#define IES_TIME_SYNC            't'
#define IES_BTSPP_HEART_BEAT     '.'
#define IES_BTSPP_UART_PRINT_SEL 'p'
#define IES_BTSPP_DOWNSAMPLING   'd'
#define IES_IMPEDANCE_CHECK_ON   'Z'
#define IES_IMPEDANCE_CHECK_OFF  'z'
#define OPEN_BCI_SOFT_RESET      'v'

// Sample record (identical — used as the Mail payload)
#define SAMPLE_REC_MAX_CHANNELS 6
typedef struct {
    uint8_t sample_type;
    uint8_t num_of_channels;
    int32_t channel_data[SAMPLE_REC_MAX_CHANNELS];
} data_sample_rec;

enum IES_SAMPLE_TYPE { EEG=0, IMPEDANCE=1, NECK_IMU=2, EAR_IMU=3, EDA=4, BATT_INFO=5 };
```

### 2.4 Packet Framing and Serialisation Logic — Verbatim ✅

The entire `btspp_send_task_fxn` packet-building block (OpenBCI and iES format) is pure data manipulation with no RTOS calls. It can be lifted verbatim and placed in a `ble_send_task` function, substituting only the `btspp.sendBuffer()` call with the BLE UART equivalent.

### 2.5 Command Parser Logic — Verbatim ✅

The `btspp_recv_task_fxn` command `switch` is pure application logic. Substitute:
- `btspp.readRaw(&cmd, 1)` → BLE UART read
- `Event_post(ies_event_handle, FLAG)` → `ies_flags.set(FLAG)`

### 2.6 Scale Factor and Signal Processing Constants — Verbatim ✅

```cpp
#define SCALE_FACTOR_UV  0.5364418669f   // 4.5e6 / (2^23 - 1)
// Used in ADS_processChannelData() — already in the ported library
```

---

## 3. Must-Port Components

These require a direct RTOS-API substitution. The *logic* does not change; only the synchronisation primitives are swapped.

### 3.1 Task System

| iES pattern | Nicla pattern | Change |
|---|---|---|
| `pthread_create(&t, &attr, fn, NULL)` with `sched_priority` | `rtos::Thread t(osPriority, stack_size);` then `t.start(fn);` | Constructor priority enum differs |
| `pthread_attr_setstacksize(&attr, N)` | `Thread t(prio, N, nullptr, "name")` | Passed to constructor |
| Tasks run forever in `while(1)` | Same — no change to task body structure | — |

**Priority mapping:**

| iES `sched_priority` | Mbed OS `osPriority` |
|---|---|
| 1 (int_temp) | `osPriorityLow` |
| 2 (imu, eda, btspp) | `osPriorityNormal` |
| 3 (ies_task) | `osPriorityAboveNormal` |
| ads_interrupt_buffering | `osPriorityHigh` (acquisition thread) |

### 3.2 Event Signaling (TI Events → EventFlags)

```cpp
// iES (TI-RTOS)
Event_Handle ies_event_handle;
ies_event_handle = Event_create(NULL, NULL);
Event_post(ies_event_handle, ADS_NEW_DATA);          // any context
uint32_t ev = Event_pend(ies_event_handle,
    Event_Id_NONE,
    ADS_NEW_DATA | IMU_NEW_DATA | START_STREAMING,
    BIOS_WAIT_FOREVER);

// Nicla (Mbed OS 6)
rtos::EventFlags ies_flags;
#define FLAG_ADS_DATA  (1u << 2)
#define FLAG_IMU_DATA  (1u << 3)
#define FLAG_START     (1u << 0)

ies_flags.set(FLAG_ADS_DATA);                        // any context (ISR-safe)
uint32_t ev = ies_flags.wait_any(
    FLAG_ADS_DATA | FLAG_IMU_DATA | FLAG_START);     // blocks in thread only
```

**Note:** `EventFlags::wait_any()` clears the matched flags atomically — same semantics as `Event_pend` with `OR_mode`.

### 3.3 Message Queues (POSIX mqueue → rtos::Mail)

```cpp
// iES (POSIX mqueue)
struct mq_attr attr = { .mq_maxmsg = 512, .mq_msgsize = sizeof(data_sample_rec) };
mqd_t q = mq_open("iES2BTPP", O_RDWR | O_CREAT | O_NONBLOCK, 0664, &attr);
mq_send(q, (char*)&rec, sizeof(rec), 0);
mq_receive(q, (char*)&rec, sizeof(rec), NULL);

// Nicla (Mbed OS 6)
rtos::Mail<data_sample_rec, 64> ies2ble_mail;

// Producer (thread context — non-ISR):
data_sample_rec *m = ies2ble_mail.alloc();      // blocks if pool full (OK in thread)
if (m) { *m = rec; ies2ble_mail.put(m); }

// Producer (ISR context — DRDY ISR must NOT send samples, only signal):
data_sample_rec *m = ies2ble_mail.try_alloc();  // non-blocking, ISR-safe
if (m) { *m = rec; ies2ble_mail.put(m); }

// Consumer (thread context):
data_sample_rec *m;
while (ies2ble_mail.try_get(&m)) {
    process(*m);
    ies2ble_mail.free(m);
}
```

### 3.4 Semaphores (POSIX sem_t → rtos::Semaphore)

```cpp
// iES
sem_t imu_sem;
sem_init(&imu_sem, 1, 0);
sem_post(&imu_sem);   // in timer ISR
sem_wait(&imu_sem);   // in imu_task

// Nicla
rtos::Semaphore imu_sem(0, 1);
imu_sem.release();    // in Ticker callback (ISR context — release() is ISR-safe)
imu_sem.acquire();    // in acquisition thread
```

### 3.5 Periodic Sampling Timer

```cpp
// iES
Timer_Handle timer = Timer_open(Board_TIMER0, &params);
Timer_start(timer);
// ISR: sampling_timer_handler() → sem_post(&imu_sem); sem_post(&eda_sem);

// Nicla — option 1 (Ticker, simpler)
mbed::Ticker sampl_ticker;
void sampling_timer_handler() {
    imu_sem.release();    // Ticker callback runs in IRQ context
    if (use_eda) eda_sem.release();
}
sampl_ticker.attach(&sampling_timer_handler, 20ms);

// Nicla — option 2 (Thread with sleep, more deterministic at low priorities)
void imu_periodic_thread() {
    while (true) {
        ThisThread::sleep_for(20ms);
        // do IMU read directly here — avoids extra semaphore
    }
}
```

### 3.6 Bluetooth Transport (BPLib / RN-42 → BLE UART)

This is the largest functional substitution.

| iES | Nicla |
|-----|-------|
| RN-42 Bluetooth Classic (SPP) via UART1 | nRF52832 native BLE (NUS — Nordic UART Service) |
| `BPLib::readRaw()` / `sendBuffer()` | `BLECharacteristic::writeValue()` / `onWrite()` callback (ArduinoBLE) |
| 115200 baud UART, binary | BLE packet MTU ~20–244 bytes (negotiated) |
| No connection state (BPLib `connected()` is stub returning true) | Real BLE connection events via `BLE.central()` |

**Application-layer protocol (0xA0 framing, command bytes) is unchanged.** Only the physical transport function calls change.

### 3.7 Peripheral Drivers

| iES driver | Mbed OS / Arduino equivalent | Notes |
|---|---|---|
| `I2C_open()` + TI I2C transfer | Arduino `Wire` library | MPU9250 and MAX17043 have Wire-based Arduino ports |
| `ADC_open()` + `ADC_convert()` | `analogRead(pin)` | nRF52832 ADC is 12-bit; scale accordingly |
| `SDFatFS_open()` + FatFs | Arduino `SD.h` or `SdFat` library | Same FAT32; different API |
| `clock_settime()` (TI epoch 1900) | `set_time()` (Mbed OS, Unix epoch 1970) | **Epoch offset: 70 years = 2208988800 s** — must adjust time-sync conversion |

---

## 4. Staged Porting Plan with Validation

### Stage 1 — ADS1299 SPI Driver ✅ COMPLETE

**Files**: `ADS1299_Library.h/.cpp`, `ADS1299_Definitions.h`, `DSPI.h/.cpp`, `pinDef.h`  
**Location**: `test/SPI_Test/`  
**Work done**:
- Removed all TI-RTOS / SimpleLink SDK headers
- Removed `boardBeginADSInterrupt()` RTOS task
- `printAll()` → `Serial.print()`
- `csLow/High()` wrap `SPI.beginTransaction/endTransaction()`
- `pinMode()` signature fixed (2 args)

**Validation criteria**:
- [x] `ADS_getDeviceID()` returns `0x3C` (ADS1299-4 product ID)
- [x] `printAllRegisters()` dumps all 24 registers with correct addresses
- [x] DRDY interrupt fires at configured ODR (250 SPS → 1 interrupt per 4 ms)
- [x] `updateChannelData()` returns 4 unique, non-zero signed 24-bit values
- [x] Channel data printed continuously with no lockups

---

### Stage 2 — FIFO Acquisition Buffer + Acquisition Thread

**Goal**: Replace the `volatile bool channelDataAvailable` poll with a proper double-stage pipeline (EventFlags → acquisition thread → Mail queue) matching the iES `ads_interrupt_buffering` architecture.

**Files to create**:
- `src/ads_buffer.h` — `ADS_Frame` struct + `Mail` object declaration
- `src/acquisition_task.h/.cpp` — acquisition thread function

**Key work**:
1. Define `ADS_Frame` struct (see Section 5 for full design)
2. Declare `rtos::EventFlags ies_flags` globally
3. In `DRDY_ISR()`: replace `ads1299.channelDataAvailable = true` with `ies_flags.set(FLAG_ADS_DATA_READY)`
4. Create `acquisition_thread` at `osPriorityHigh`:
   - `wait_any(FLAG_ADS_DATA_READY)`
   - `ads1299.updateChannelData()`
   - `alloc` + `put` to `ads_mail`
5. Move prints/processing out of `loop()` into a consumer thread or tail of `loop()`

**Validation criteria**:
- [ ] At 250 SPS: zero frame drops over 60 s (verify with rolling counter)
- [ ] At 1 kSPS: drop count < 1% over 60 s
- [ ] `ads_mail.full()` never returns true under normal operation
- [ ] Timestamp monotonically increases with step ≤ 1.5× expected period
- [ ] Consumer thread can process and print at ≥ 250 SPS without blocking acquisition

---

### Stage 3 — Protocol Layer (ies_misc + Packet Framing)

**Goal**: Port the binary encoding utilities and the complete iES/OpenBCI packet serialiser and command parser.

**Files to copy verbatim**:
- `ies_misc.h` / `ies_misc.cpp`
- `checksum.h` / `crc8.cpp` / `crc16.cpp`

**Files to create**:
- `src/protocol.h` — `data_sample_rec`, command byte defines, `IES_SAMPLE_TYPE` enum
- `src/packet_builder.h/.cpp` — `build_ies_packet()` and `build_openbci_packet()` extracted from `btspp_send_task_fxn`
- `src/command_parser.h/.cpp` — `parse_command()` switch extracted from `btspp_recv_task_fxn`

**Key work**:
1. Copy `ies_misc.cpp` and `crc8.cpp` verbatim
2. Extract packet builder from `btspp_send_task_fxn` — no logic changes, only the `sendBuffer()` call is stubbed for now
3. Extract command parser from `btspp_recv_task_fxn` — replace `Event_post()` with `ies_flags.set()`
4. Adjust time-sync epoch: incoming 32-bit seconds are iES epoch (1900); convert: `unix_time = ies_time - 2208988800UL` before calling `set_time()`

**Validation criteria**:
- [ ] `uint24_to_buffer(0xABCDEF, buf)` → `buf = {0xEF, 0xCD, 0xAB}` (LSByte first)
- [ ] `buffer_to_uint24(buf)` round-trips correctly for boundary values (0, 0x7FFFFF, 0x800000, 0xFFFFFF)
- [ ] `crc_8([0x01,0x02,0x03,0x04], 4)` matches Python reference: `from crc import Calculator, Crc8.SENSIRION`
- [ ] Build an EEG packet from known channel values; decode with Python script from `ies_message_protocol.md`; verify bytes match exactly
- [ ] Send `IES_STREAM_START` ('b') byte; verify `ies_flags.get() & FLAG_START_STREAMING` is set
- [ ] Send `IES_TIME_SYNC` + 5-byte payload with correct CRC; verify `get_time()` returns expected epoch

---

### Stage 4 — BLE Transport Layer

**Goal**: Replace `BPLib` (RN-42 Bluetooth Classic SPP) with the nRF52832 native BLE stack exposing a Nordic UART Service (NUS) characteristic, preserving the binary application protocol bit-for-bit.

**Library**: `ArduinoBLE` (available for Nicla Voice via Arduino IDE)

**Files to create**:
- `src/ble_transport.h/.cpp` — `BleTransport` class with `begin()`, `sendBuffer()`, `readRaw()`, `available()`, `connected()` — same signature as `BPLib` for drop-in compatibility
- `src/ble_recv_task.h/.cpp` — consumer of RX characteristic, posts commands to parser
- `src/ble_send_task.h/.cpp` — consumer of `ies2ble_mail`, calls `sendBuffer()`

**Key work**:
1. Define NUS service UUID and RX/TX characteristic UUIDs
2. Implement `BleTransport::sendBuffer()` splitting payloads > MTU into multiple characteristic writes
3. Implement `BleTransport::readRaw()` reading from RX FIFO populated by `onWrite()` callback
4. Instantiate `ble_recv_thread` (Normal priority) and `ble_send_thread` (Normal priority)

**Validation criteria**:
- [ ] Nicla is discoverable; can connect from nRF Connect app on phone
- [ ] Send 'b' byte from nRF Connect → Nicla flags `FLAG_START_STREAMING`
- [ ] Receive iES-format packets in nRF Connect; verify 0xA0 start and 0xC0 end bytes
- [ ] Packet counter increments monotonically with no skips at 250 SPS
- [ ] Send time-sync payload, verify device clock updates correctly
- [ ] 30-second streaming session: no BLE disconnection, no buffer overflow

---

### Stage 5 — Auxiliary Sensors (IMU + EDA + Battery)

**Goal**: Port the `imu_task_fxn` and `eda_task_fxn` to use Arduino `Wire` and `analogRead`, controlled by a `Ticker`-based 50 Hz periodic signal.

**Files to create**:
- `src/imu_task.h/.cpp` — Wire-based MPU9250 read at 50 Hz
- `src/eda_task.h/.cpp` — `analogRead()` at 50 Hz + nS conversion formula (verbatim from iES)

**Library requirements**:
- MPU9250 Wire-based Arduino library (e.g., `hideakitai/MPU9250`)
- MAX17043 Wire-based Arduino library
- No changes to conversion formulae

**Validation criteria**:
- [ ] IMU accel magnitude at rest ≈ 1.0 g ± 0.05 g on all three axes
- [ ] IMU data arrives in `imu_mail` at exactly 50 Hz (measured over 1000 samples)
- [ ] EDA reading with known resistor (e.g., 100 kΩ) returns value within 5% of expected nS
- [ ] Battery SoC reading is non-zero and reasonable (> 0%, < 105%)
- [ ] All three sensor streams transmit concurrently with EEG without frame drops in any queue

---

### Stage 6 (Optional) — SD Card Logging

**Goal**: Port the FatFs-based SD card logging to Arduino `SD.h` or `SdFat`.

**Notes**:
- Nicla Voice has no built-in SD slot; an SPI SD card breakout on a second SPI CS pin would be required
- `SD.h` exposes the same `File.write()` / `File.println()` API pattern used in iES CSV writing
- The auto-save timer (1-minute periodic, iES uses a POSIX `sleep` with `struct timespec`) is replaced by a `Ticker` or a dedicated low-priority thread with `ThisThread::sleep_for(60s)`

**Validation criteria**:
- [ ] Create `eeg.csv`, write 10 s of data, close, re-read, verify row count = sample_rate × 10
- [ ] No corruption on power-cycle mid-write (verify partial file is still parseable up to last complete line)
- [ ] Auto-save flushes file handle every 60 s (verify with modified 5-second interval during test)

---

## 5. FIFO Queue Buffer Design for Mbed OS 6

### 5.1 Design Goals

1. **ISR-safe producer path** — DRDY ISR must never block
2. **Zero dynamic allocation** — no `new`/`malloc` after startup; all pool memory is static
3. **Typed messages** — carry a complete `ADS_Frame` (channel data + timestamp + counter)
4. **Bounded latency** — consumer can drain the queue without missing samples at 1 kSPS
5. **Minimal overhead** — single copy from SPI register file to pool slot; no intermediate buffer

### 5.2 The Two-Stage Pipeline

```
Hardware                    Acquisition Thread          Consumer Thread
────────                    ──────────────────          ───────────────
                            (osPriorityHigh)            (loop() or Thread)

DRDY pin FALLING edge
    │
    ▼
DRDY_ISR()
  ies_flags.set(             ◄── wakeup signal
    FLAG_ADS_DATA_READY)
                              ies_flags.wait_any(
                                FLAG_ADS_DATA_READY)
                                  │
                                  ▼ (wakes immediately)
                              ads1299.updateChannelData()
                                  │ SPI read — safe, in thread
                                  ▼
                              ADS_Frame *f = ads_mail.try_alloc()
                                  │
                                  ├─ f == nullptr: pool full → drop_count++
                                  │
                                  └─ fill f:
                                       f->ch[0..3] = boardChannelDataInt[0..3]
                                       f->timestamp_ms = Kernel::Clock::now()
                                       f->frame_counter = counter++
                                     ads_mail.put(f)
                                                              ADS_Frame *f;
                                                              if (ads_mail.try_get(&f)) {
                                                                  process(*f);
                                                                  ads_mail.free(f);
                                                              }
```

### 5.3 ADS_Frame Struct

```cpp
/**
 * @brief  One complete ADS1299 sample frame, used as the Mail payload.
 *
 * Size: 4×4 + 4 + 1 + 3(pad) = 24 bytes per frame.
 * At 1 kSPS a pool of 64 frames = 64 ms of burst capacity.
 */
struct ADS_Frame {
    int32_t  ch[4];            // ADS1299 signed 24-bit values,
                               // zero-extended to int32 (MSB sign-extended)
    uint32_t timestamp_ms;     // Kernel::Clock::now().time_since_epoch() / 1ms
                               // captured in acquisition thread immediately
                               // after updateChannelData() returns
    uint8_t  frame_counter;    // Rolling 0–255; consumer detects drops
                               // when (current - previous) != 1 mod 256
    uint8_t  _pad[3];          // Explicit padding for 4-byte alignment
};
```

### 5.4 EventFlags Bit Assignments

```cpp
// In ies_flags.h (shared header)
#define FLAG_ADS_DATA_READY   (1u << 0)   // DRDY ISR → acquisition thread
#define FLAG_IMU_DATA_READY   (1u << 1)   // Ticker ISR → imu_task
#define FLAG_EDA_DATA_READY   (1u << 2)   // Ticker ISR → eda_task
#define FLAG_START_STREAMING  (1u << 3)   // command parser → coordinator
#define FLAG_STOP_STREAMING   (1u << 4)   // command parser → coordinator
#define FLAG_PRINT_EEG        (1u << 5)   // command parser → coordinator
#define FLAG_OPEN_BCI_MODE    (1u << 6)   // command parser → coordinator
#define FLAG_IES_MODE         (1u << 7)   // command parser → coordinator
// ... up to bit 30 (bit 31 reserved by Mbed OS)

extern rtos::EventFlags ies_flags;        // defined in main .ino
```

### 5.5 Mail Object Declaration

```cpp
// In ads_buffer.h
#include "mbed.h"
#include "ADS1299_Library.h"

// 64-frame pool: 64 × 24 bytes = 1536 bytes static RAM
// 64 ms of burst capacity at 1 kSPS — covers worst-case BLE TX delay
#define ADS_MAIL_DEPTH 64

extern rtos::Mail<ADS_Frame, ADS_MAIL_DEPTH> ads_mail;
```

### 5.6 Full Acquisition Thread Implementation Sketch

```cpp
// acquisition_task.cpp
#include "mbed.h"
#include "ads_buffer.h"
#include "ADS1299_Library.h"

rtos::Mail<ADS_Frame, ADS_MAIL_DEPTH> ads_mail;
static uint32_t drop_count = 0;
static uint8_t  frame_ctr  = 0;

void acquisition_task_fn(void) {
    while (true) {
        // Block until DRDY ISR fires
        ies_flags.wait_any(FLAG_ADS_DATA_READY);

        // SPI read — safe in thread context
        ads1299.updateChannelData();

        // Allocate from pool — non-blocking (try_alloc)
        ADS_Frame *f = ads_mail.try_alloc();

        if (f == nullptr) {
            // Pool exhausted: consumer is too slow
            drop_count++;
            continue;
        }

        // Fill frame
        for (int i = 0; i < 4; i++) {
            f->ch[i] = ads1299.boardChannelDataInt[i];
        }
        f->timestamp_ms  = rtos::Kernel::Clock::now().time_since_epoch().count();
        f->frame_counter = frame_ctr++;
        memset(f->_pad, 0, sizeof(f->_pad));

        ads_mail.put(f);
    }
}

// Declared in .ino:
// rtos::Thread acquisition_thread(osPriorityHigh, 2048, nullptr, "AcqTask");
// acquisition_thread.start(acquisition_task_fn);
```

### 5.7 Consumer Usage Pattern

```cpp
// In loop() or a dedicated consumer thread:
void consume_ads_frames(void) {
    ADS_Frame *f;
    while (ads_mail.try_get(&f)) {      // drain all available frames
        // 1. Scale to µV
        float uV[4];
        for (int i = 0; i < 4; i++) {
            uV[i] = (float)f->ch[i] * SCALE_FACTOR_UV;
        }

        // 2. Build iES or OpenBCI packet (using packet_builder)
        uint8_t pkt[33];
        uint8_t pkt_len = build_ies_packet(f, pkt);

        // 3. Send over BLE UART
        ble_transport.sendBuffer(pkt, pkt_len);

        // 4. Mandatory: return slot to pool
        ads_mail.free(f);
    }
}
```

### 5.8 Queue Depth Sizing Guide

| Sample Rate | Min safe depth | Recommended depth | RAM cost (24 B/frame) |
|---|---|---|---|
| 250 SPS | 8 frames (32 ms) | 32 frames (128 ms) | 768 B |
| 500 SPS | 16 frames (32 ms) | 64 frames (128 ms) | 1536 B |
| 1 kSPS | 32 frames (32 ms) | 128 frames (128 ms) | 3072 B |

**nRF52832 total SRAM is 64 KB.** Queue depth should not exceed ~200 frames to leave room for stacks (acquisition thread: 2 KB, consumer thread: 4 KB, main/loop: 4 KB default).

### 5.9 Drop Detection in Consumer

```cpp
static uint8_t last_ctr = 0;
static uint32_t total_drops = 0;

// When consuming each frame f:
uint8_t expected = last_ctr + 1;      // wraps naturally at uint8 overflow
if (f->frame_counter != expected && last_ctr != 0) {
    total_drops += (uint8_t)(f->frame_counter - expected);
}
last_ctr = f->frame_counter;
```

### 5.10 Thread-Safety Summary

| Operation | Context | Safe? | Reason |
|---|---|---|---|
| `ies_flags.set(FLAG_ADS_DATA_READY)` | DRDY ISR | ✅ | `EventFlags::set()` is IRQ-safe |
| `ads_mail.try_alloc()` | acquisition thread | ✅ | Pool alloc is thread-safe |
| `ads_mail.put(f)` | acquisition thread | ✅ | Queue put is thread-safe |
| `ads_mail.try_get(&f)` | consumer thread | ✅ | Queue get is thread-safe |
| `ads_mail.free(f)` | consumer thread | ✅ | Pool free is thread-safe |
| `ads1299.updateChannelData()` | acquisition thread only | ✅ | Single owner of SPI bus |
| `ads1299.updateChannelData()` | ISR | ❌ | `SPI.transfer()` is NOT IRQ-safe |
| `Mutex::lock()` | ISR | ❌ | Never lock a Mutex from ISR |
| `ads_mail.alloc()` (blocking) | acquisition thread | ✅ | Blocking alloc OK in thread |
| `ads_mail.alloc()` (blocking) | ISR | ❌ | Never block in ISR |

---

*Document end — see `ies_message_protocol.md` for byte-level packet format reference.*
