# EEG Firmware Architecture — Arduino NICLA Voice + ADS1299 Shield

**Document status:** Draft v0.5 — Section 12 updated: corrected gain default, RuntimeState file names, added OutputMode and PersistentConfig (Section 12.4)  
**Last updated:** 2026-05-09  
**Note:** Task-based architecture with Gateway communication hub and Publisher/Subscriber pattern

---

## Table of Contents

### PART I: OVERVIEW & REQUIREMENTS
1. [Introduction](#1-introduction)
   - 1.1 [Purpose and Scope](#11-purpose-and-scope)
   - 1.2 [Target Platform](#12-target-platform)
   - 1.3 [Document Organization](#13-document-organization)
2. [System Requirements](#2-system-requirements)
   - 2.1 [Functional Requirements](#21-functional-requirements)
   - 2.2 [Throughput Requirements](#22-throughput-requirements)
   - 2.3 [Software Constraints](#23-software-constraints)
3. [Hardware Configuration](#3-hardware-configuration)
   - 3.1 [Platform Components](#31-platform-components)
   - 3.2 [Pin Assignments](#32-pin-assignments)
   - 3.3 [Active Channels](#33-active-channels)
4. [Design Decisions & Open Questions](#4-design-decisions--open-questions)
   - 4.1 [Resolved Decisions](#41-resolved-decisions)
   - 4.2 [Open Questions](#42-open-questions)

### PART II: ARCHITECTURE
5. [Architectural Overview](#5-architectural-overview)
   - 5.1 [Layered Architecture](#51-layered-architecture)
   - 5.2 [Architectural Principles](#52-architectural-principles)
   - 5.3 [High-Level Data Flow](#53-high-level-data-flow)
6. [Task Architecture](#6-task-architecture)
   - 6.1 [Task Overview and Priorities](#61-task-overview-and-priorities)
   - 6.2 [EEG Acquisition Task](#62-eeg-acquisition-task)
   - 6.3 [Packetiser Task](#63-packetiser-task)
   - 6.4 [Gateway Task](#64-gateway-task)
   - 6.5 [Channel Tasks (UART & BLE)](#65-channel-tasks-uart--ble)
   - 6.6 [Command Handler Task](#66-command-handler-task)
   - 6.7 [ML Processor Task (Future)](#67-ml-processor-task-future)
7. [Publisher/Subscriber Pattern](#7-publishersubscriber-pattern)
   - 7.1 [Pattern Overview](#71-pattern-overview)
   - 7.2 [FIFO Queue Specification](#72-fifo-queue-specification)
   - 7.3 [Queue Ownership and Consumer Wake-up](#73-queue-ownership-and-consumer-wake-up)
   - 7.4 [Subscription Relationships](#74-subscription-relationships)
   - 7.5 [Overflow Policy](#75-overflow-policy)
8. [Data Flow & Data Model](#8-data-flow--data-model)
   - 8.1 [Path A — EEG Acquisition](#81-path-a--eeg-acquisition-sensor--packetisertask)
   - 8.2 [Path B — Data Output](#82-path-b--data-output-packetisertask--channels--wire)
   - 8.3 [Path C — Command Input](#83-path-c--command-input-wire--cmdhandler--packetisertask)
   - 8.4 [Queue Summary](#84-queue-summary)
9. [Communication Channels](#9-communication-channels)
   - 9.1 [Channel Architecture](#91-channel-architecture)
   - 9.2 [UART Channel](#92-uart-channel)
   - 9.3 [BLE Channel](#93-ble-channel)
   - 9.4 [Command Processing](#94-command-processing)

### PART III: IMPLEMENTATION
10. [Execution Flow](#10-execution-flow)
    - 10.1 [Startup Sequence](#101-startup-sequence)
    - 10.2 [Interrupt Handling (DRDY)](#102-interrupt-handling-drdy)
    - 10.3 [Task Scheduling](#103-task-scheduling)
11. [Programming Interfaces](#11-programming-interfaces)
    - 11.1 [IProducer<T>](#111-iproducert)
    - 11.2 [IConsumer<T>](#112-iconsumert)
    - 11.3 [BaseTask](#113-basetask)
    - 11.4 [Template Task Classes](#114-template-task-classes)
    - 11.5 [Concrete Task Examples](#115-concrete-task-examples)
12. [Configuration Management](#12-configuration-management)
    - 12.1 [config.h Structure](#121-configh-structure)
    - 12.2 [Configuration Sections](#122-configuration-sections)
    - 12.3 [Runtime Configuration — RuntimeState](#123-runtime-configuration--runtimestate)
    - 12.4 [Persistent Configuration — PersistentConfig](#124-persistent-configuration--persistentconfig)
13. [Source Tree Organization](#13-source-tree-organization)
    - 13.1 [Current Structure](#131-current-structure)
    - 13.2 [Module Reference](#132-module-reference)

---

# PART I: OVERVIEW & REQUIREMENTS

## 1. Introduction

### 1.1 Purpose and Scope

This document specifies the firmware architecture for a real-time EEG acquisition and processing system built on the Arduino NICLA Voice platform with ADS1299 EEG frontend. The architecture supports:

- **Real-time EEG data acquisition** from ADS1299 via DRDY-driven SPI
- **Concurrent ML inference** using on-board NDP120 neural processor
- **Multi-channel communication** over UART and BLE
- **Publisher/Subscriber pattern** for decoupled, queue-mediated task communication (producers never wait for queue space; brief mutex contention is the only possible wait)
- **Configurable, extensible design** for future enhancements

### 1.2 Target Platform

| Component | Description |
|-----------|-------------|
| **Board** | Arduino NICLA Voice |
| **MCU** | nRF52832 (ARM Cortex-M4F, 64KB RAM, 512KB Flash) |
| **RTOS** | Arduino Mbed OS (RTOS primitives) |
| **EEG Frontend** | ADS1299 (4-channel, 24-bit ADC) on custom shield |
| **ML Processor** | NDP120 (on-board Neural Decision Processor) |
| **Connectivity** | USB Serial, BLE 5.0 (built-in nRF radio) |

### 1.3 Document Organization

- **Part I** (Sections 1-4): System overview, requirements, hardware, and decision tracking
- **Part II** (Sections 5-9): Architecture patterns, tasks, data model, and communication
- **Part III** (Sections 10-14): Implementation details, flows, interfaces, and source organization

---

## 2. System Requirements

### 2.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | Continuously sample ADS1299 at configured ODR via DRDY interrupt | MUST | Defined |
| FR-02 | Send periodic time sync packets to enable host-side timestamp reconstruction | MUST | Q2 open |
| FR-03 | Distribute samples to all subscribers via `distribute()` — never waits for queue space; drop-oldest on overflow | MUST | Defined |
| FR-04 | Drop oldest sample on FIFO overflow; track dropped count | MUST | Defined |
| FR-05 | Stream raw EEG data over UART (Phase 2) and BLE (future) | MUST | In progress |
| FR-06 | Run ML inference on NDP120 concurrently with acquisition | MUST | Q6 open |
| FR-07 | Accept commands over USB Serial (UART) | MUST | Q13 open |
| FR-08 | Accept commands over BLE (control channel) | MUST | Q10-Q11 open |
| FR-09 | All system parameters configurable in single `config.h` | MUST | Defined |
| FR-10 | Task priorities configurable via `config.h` | MUST | Resolved |
| FR-11 | Enable/disable UART and BLE channels independently via flags | MUST | Defined |
| FR-12 | Gateway validates incoming commands (lightweight check) | MUST | Defined |
| FR-13 | Gateway prioritizes command responses over streaming data | MUST | Defined |
| FR-14 | PacketiserTask serialises each input item (EEG, Response, ML) to IES wire format individually; priority order: Response > EEG > ML | MUST | Resolved |
| FR-15 | UART and BLE share identical command vocabulary | SHOULD | Q13 open |
| FR-16 | Channel tasks handle protocol-specific framing independently | MUST | Defined |
| FR-17 | FIFO overflow count and dropped count queryable at runtime | SHOULD | Defined |
| FR-18 | Publisher/subscriber relationships configurable at startup | MUST | Defined |

### 2.2 Throughput Requirements

| Requirement | Details | Status |
|-------------|---------|--------|
| **BLE throughput** | Must accommodate highest ADS1299 ODR (16 kSPS) for raw data per channel | ✅ Q5 RESOLVED: Design for max, test at 1000 SPS |
| **ADS1299 ODR** | Design capacity: 16 kSPS (max); Default: 1000 SPS | ✅ **Q5 RESOLVED** |
| **Channel coexistence** | BLE data and control channels must not starve each other | Resolved (Q3) |
| **Packet format** | Frame format for EEG/ML/command packets | **Q9 OPEN** |

**Throughput calculation dependencies:**
- Q5 (ADS1299 ODR) ✅ RESOLVED: Designed for 16 kSPS max (320 kB/s struct); default 1000 SPS (20 kB/s)
- Q9 (packet format) determines framing overhead and packing efficiency
- Q11 (BLE library) affects achievable throughput
- **Note:** 16 kSPS requires BLE packet optimization to fit within 2 Mbps PHY (see Q5)

### 2.3 Software Constraints

| Constraint | Implementation |
|------------|----------------|
| **RTOS** | Arduino Mbed OS with RTOS primitives (Thread, Mutex, Semaphore, Queue) |
| **Concurrency** | All inter-task communication via FIFO queues; no indefinite blocking (only brief, bounded mutex contention) |
| **ISR safety** | No SPI access or blocking calls in DRDY ISR; use semaphore signaling only |
| **Memory** | 64KB RAM total (nRF52832); Estimated usage: ~32KB Phase 2 (50%), ~38KB with BLE+ML (59%) |
| **Configuration** | Single source of truth: `config.h` for all tuning parameters |

---

## 3. Hardware Configuration

### 3.1 Platform Components

| Component | Part | Interface | Notes |
|-----------|------|-----------|-------|
| **EEG Frontend** | ADS1299 (4-channel, 24-bit) | Hardware SPI | Custom shield for NICLA Voice |
| **ML Processor** | NDP120 | On-board | Neural Decision Processor |
| **BLE Radio** | nRF52832 (built-in) | BLE 5.0 | Shared with MCU |
| **Debug/Control** | USB Serial (CDC) | UART | 921600 baud (Q12 resolved; 1 Mbaud caused Windows CDC corruption) |
| **Reference Code (Primary)** | iES_v0.3-master | TI-RTOS | `code_references/iES_v0.3-master/` (ADS1299 driver, FIFO queue) |
| **Reference Code (Secondary)** | OpenBCI_8 | Arduino | `code_references/OpenBCI_8/` (ADS1299 register definitions) |

### 3.2 Pin Assignments

All pin assignments are defined in `pinDef.h`:

| Signal | Arduino Pin | Function | Notes |
|--------|-------------|----------|-------|
| `ADS_RST_PIN` | 10 | ADS1299 Reset | Active-low |
| `ADS_DRDY_PIN` | 11 | Data Ready Interrupt | Active-low, FALLING edge trigger |
| `SPI_CS` | 6 | SPI Chip Select | Active-low |
| `SPI_MISO` | 7 | SPI Master In Slave Out | Hardware SPI |
| `SPI_MOSI` | 8 | SPI Master Out Slave In | Hardware SPI |
| `SPI_SCK` | 9 | SPI Clock | Hardware SPI |

**Status:** ✅ All pin assignments resolved (Q16).

### 3.3 Active Channels

| Channel | Status | Configuration |
|---------|--------|---------------|
| **CH1** | Reference/test | Both IN1P and IN1N tied to AVDD (+2.5V); reads ~0V differential |
| **CH2** | Not used | EEG-capable in schematic; powered down (not intended for use) |
| **CH3** | Active | EEG input (primary channel) |
| **CH4** | Active | EEG input (primary channel) |

---

## 4. Design Decisions & Open Questions

### 4.1 Resolved Decisions

| ID | Decision | Resolution |
|----|----------|------------|
| **Q3** | PacketiserTask dispatch order | ✅ Each input item serialised and dispatched individually. Priority: Response first (immediate delivery), then EEG, then ML. No combining. |
| **Q4** | Debug task priority | ✅ No dedicated debug task; use USB Serial in main loop |
| **Q1** | CH2 channel configuration | ✅ CH2 is EEG-capable but not used; powered down to reduce noise |
| **Q5** | ADS1299 output data rate (ODR) | ✅ Design for 16 kSPS max capacity; default 1000 SPS for testing |
| **Q7** | FIFO queue depths | ✅ Defined in `config.h`: STREAMING=64, EEG_BLE=64, EEG_ML=32 |
| **Q12** | Debug output transport | ✅ USB CDC Serial at 921600 baud (1 Mbaud caused data corruption on Windows) |
| **Q14** | Thread stack sizes | ✅ Defined in `config.h` for each task |
| **Q16** | SPI pin assignments | ✅ CS=6, MISO=7, MOSI=8, SCK=9 (see Section 3.2) |
| **Q17** | Role of loop() | ✅ Empty or minimal background work; all real work in RTOS tasks |

### 4.2 Open Questions

Each item below blocks specific implementation modules. Questions are annotated with blocking scope and recommendations.

---

#### Q1 — CH2 Configuration
**Status:** ✅ **RESOLVED**  
**Decision:** CH2 is a normal EEG input in the schematic (same as CH3/CH4) but is **not intended to be used**. It will be **powered down** to reduce noise and power consumption.

**Rationale:** 
- Hardware supports 4-channel ADS1299 with all channels EEG-capable
- Only CH3, CH4 are active EEG channels (CH1 is reference/test, CH2 is unused)
- Powering down unused channels reduces analog noise and current draw
- Can be enabled later if needed via configuration change

---

#### Q2 — Time Sync Packet Interval
**Status:** 🔴 OPEN  
**Blocks:** `PacketiserTask`, TIME_SYNC frame generation, packet format (Q9)  
**Question:** How often should the system send time sync packets? Options:
- Every N samples (e.g., every 250 samples = 1 second @ 250 SPS)
- Every T milliseconds (e.g., every 1000ms)
- On-demand only (host requests via command)

**Design rationale:** Following iES firmware pattern (`IES_TIME_SYNC 't'` command), use periodic time sync packets instead of per-sample timestamps. This significantly reduces data overhead:
- **Per-sample timestamps:** 4 bytes × 16,000 samples/s = 64 KB/s overhead @ max rate
- **Periodic sync packets:** ~10 bytes/s @ 1 Hz sync rate
- Host reconstructs timestamps: `timestamp = last_sync_time + ((sample_number - sync_sample_number) × sample_period)`

**Impact:** Time sync packets contain `uint32_t timestamp_us` (microseconds since boot) and `uint32_t sample_counter` (current value of the global sample counter).  
**Recommendation:** Send time sync packet every 1 second (configurable in `config.h`). Host can reconstruct sample timestamps with <1µs error accumulation between syncs.

---

#### Q5 — ADS1299 Output Data Rate (ODR)
**Status:** ✅ **RESOLVED**  
**Decision:** System designed for **maximum capacity of 16 kSPS**, but will use **1000 SPS as default** for testing and validation.

**Design Impact (16 kSPS maximum):**
- Raw ADC data: 4 channels × 3 bytes × 16,000 SPS = 192 kB/s (from ADS1299 SPI)
- Struct data: 20 bytes × 16,000 SPS = **320 kB/s** (`ADS1299_4_Sample` including sample_number)
- FIFO sizing: 64-sample buffer = **4ms** latency at max rate
- BLE throughput: Requires ~2.56 Mbps (320 kB/s × 8 bits/byte) — **exceeds BLE 5.0 2 Mbps PHY** ⚠️
  - **nRF52832 practical ceiling at 1M PHY + DLE:** ~125 kB/s effective. At 1 kSPS (16 kB/s OTA), BLE is fully viable. At 16 kSPS (256 kB/s OTA), it exceeds the PHY ceiling by 2× — wireless streaming is infeasible without PHY upgrade or compression.
  - **Mitigations for 16 kSPS:** (a) Upgrade to nRF52840 (2M PHY, ~250 kB/s effective); (b) strip IES framing and pack raw 3-byte ADC values (→ 192 kB/s, still marginal); (c) apply 24-bit delta encoding (~40–60% compression on EEG signals).
  - **Architectural provision:** `GatewayTask` fan-out supports adding a `BleFormatterTask` between `GatewayTask` and `BleChannelTask` for BLE-specific raw/compressed packing without modifying any upstream task. Full analysis in `ble_channel_design.md` Section 8.
- Time synchronization: Periodic time sync packets (Q2) allow host to reconstruct timestamps without per-sample overhead

**Default Operation (1000 SPS):**
- Raw ADC data: 4 channels × 3 bytes × 1000 SPS = 12 kB/s
- Struct data: 20 bytes × 1000 SPS = **20 kB/s** (`ADS1299_4_Sample`)
- FIFO sizing: 64-sample buffer = **64ms** latency
- BLE throughput: Only ~160 kbps, well within BLE capacity
- Conservative starting point for validation; proven feasible in literature

**Configuration:** ODR is configurable in `config.h`; runtime switching via command interface (Q13).

---

#### Q6 — How ADS1299 Data Is Fed Into the NDP120
**Status:** 🔴 OPEN — Most technically uncertain  
**Blocks:** `ndp120_driver`, `ml_processor` placeholder  
**Question:** The NDP120 is natively designed for audio and IMU inputs. The mechanism by which raw ADS1299 time-series data is presented to the NDP120 inference engine is not defined.  
**Options:**
1. Memory-mapped buffer sharing
2. Re-encode EEG as "pseudo-audio" format
3. NDP120 SDK custom sensor interface

**Recommendation:** Consult NDP120 SDK documentation; may require vendor support or custom interface layer.

---

#### Q8 — ML Output Data Structure (`MLOutput` fields)
**Status:** 🔴 OPEN  
**Blocks:** `IMLOutput` interface, `packetiser`, `WireFrame` IES ML frame format  
**Question:** What does the ML inference produce? Options:
- Class label only (1-2 bytes)
- Class label + confidence score
- Feature vector
- All of the above

**Impact:** Determines `MLOutput` struct size and PacketiserTask IES serialization logic.  
**Recommendation:** Define minimal output (label + confidence) for Phase 1; extend later if needed.

---

#### Q9 — BLE / UART Packet Frame Format
**Status:** ⚠️ PARTIALLY RESOLVED  
**Blocks:** `packetiser.h/.cpp` (Response and ML frames), `ble_channel.h/.cpp`

**Resolved — EEG and TIME_SYNC frames:** IES native format adopted (see `ies_message_protocol.md` Section 5.1).
```
[0xA0 start][frame_count 1B][type_ch 1B: (type<<4)|num_ch][ch_data N×3B][0xC0 stop]
     type nibble:  0=EEG  5=TIME_SYNC
     ch nibble:    number of 3-byte channel data fields

EEG (4-channel, 250 SPS):   [A0][cnt][0x04][ch0 3B][ch1 3B][ch2 3B][ch3 3B][C0] = 16 B
TIME_SYNC:                  [A0][cnt][0x51][ts_us 4B][sample_cnt 4B][C0]          = 12 B
```

**Still open — Response and ML frames:** IES-extension frame formats for `Response` and `MLOutput` are TBD. Proposed type nibble values:
- `4` — RESPONSE: `cmd_id (1B)` + `status (1B)` + `payload_len (1B)` + `payload (≤8B)` → frame ≤ 15 B
- `6` — ML_OUTPUT: `class_label (1B)` + `confidence (4B)` → frame = 9 B (future, Q8)

**Wire object:** `WireFrame { uint8_t len; FrameDest dest; uint8_t bytes[IES_MAX_FRAME_SIZE]; }` (22 B) — produced by PacketiserTask; `dest` selects UART or BLE (unicast, internal only).

---

#### Q10 — BLE Service and Characteristic UUIDs
**Status:** 🔴 OPEN  
**Blocks:** `ble_app` placeholder  
**Question:** Custom 128-bit UUIDs needed for:
- EEG streaming service
- EEG data characteristic (notify)
- ML output characteristic (notify, future)
- Control/status characteristic (write/read)

**Recommendation:** Generate UUIDs using standard UUID generator; document in separate BLE protocol spec.

---

#### Q11 — BLE Library Selection
**Status:** ✅ **RESOLVED**  
**Decision:** Use **Mbed OS BLE API** (`#include "ble/BLE.h"`) with `GattServer::EventHandler` interface.

**Rationale (full analysis in `ble_channel_design.md` Section 4):**
- **ArduinoBLE** — wraps `ble/BLE.h` on this platform. Silently swallows `BLE_ERROR_NO_MEM`; no `onDataSent()` callback; no MTU/DLE control. Not viable for high-throughput streaming.
- **Raw nRF5 SDK SoftDevice** — conflicts with Mbed OS's ownership of the SoftDevice via Cordio HCI transport; abandoning the Arduino Mbed framework is prohibitive.
- **Mbed BLE API (`ble/BLE.h`)** — already in `libmbed.a` (no extra dependency); provides `onDataSent()` for TX flow control, `ble_error_t` returns from `write()`, ATT MTU exchange callbacks, DLE control, and connection parameter updates.

**Integration note:** Mbed BLE is event-driven and requires a dedicated BLE EventQueue dispatch thread (separate from `BleChannelTask`). All cross-context communication (BLE EventQueue thread ↔ `BleChannelTask` thread) uses RTOS-safe primitives (`FifoQueue`, `rtos::Semaphore`).

---

#### Q13 — UART Command Vocabulary and Message Format
**Status:** 🔴 OPEN  
**Blocks:** Command Handler implementation  
**Question:** Define specific command identifiers, argument formats, and response formats.  
**Proposed minimal command set for Phase 2:**
- `START` / `STOP` — begin/halt EEG streaming
- `SET_GAIN <gain>` — configure ADS1299 gain (1, 2, 4, 6, 8, 12, 24)
- `SET_ODR <odr>` — configure output data rate (250, 500, 1000, 2000)
- `ENABLE_UART` / `DISABLE_UART` — set enUART flag at Gateway
- `ENABLE_BLE` / `DISABLE_BLE` — set enBLE flag at Gateway
- `STATUS` — query current configuration and channel states

**Recommendation:** ASCII format for Phase 2 (easy debugging); binary for production. Define formal command grammar.

---

#### Q15 — Error Handling and Recovery Strategy
**Status:** 🔴 OPEN  
**Blocks:** All modules  
**Question:** What should happen when:
- ADS1299 is unresponsive
- NDP120 model fails to load
- BLE disconnects unexpectedly
- FIFO overflows persistently

**Recommendation:** Define error states and recovery actions:
- **ADS1299 failure:** Halt acquisition, set error flag, notify via status
- **BLE disconnect:** Disable `enBLE`, buffer data in FIFO, resume on reconnect
- **FIFO overflow:** Log in diagnostics, continue operation (drop-oldest policy)

---

#### Q18 — FifoQueue Thread-Safety Mechanism
**Status:** ✅ **RESOLVED**  
**Decision:** `rtos::Mutex` (priority-inheritance mutex from Mbed OS).

`fifo_queue.h` uses `mutable rtos::Mutex _mutex`; every `push()`, `pop()`, `peek()`, `size()`, and `status()` acquires it. This gives bounded worst-case latency (brief critical section only). **Not ISR-safe by design** — the DRDY ISR uses a semaphore signal only, never calls `FifoQueue::push()` directly. Lock-free migration can be deferred until profiling identifies contention as a bottleneck.

---

# PART II: ARCHITECTURE

## 5. Architectural Overview

### 5.1 Layered Architecture

The firmware is organized into five layers, from hardware to application:

```
┌─────────────────────────────────────────────────────────────────────┐
│                       COMMUNICATION LAYER                           │
│   UART Channel Task  │  BLE Channel Task  │  Gateway Task           │
│   (framing + I/O)    │  (framing + I/O)   │  (routing + control)    │
├─────────────────────────────────────────────────────────────────────┤
│                       PROCESSING LAYER                              │
│   PacketiserTask    │  Command Handler   │  ML Processor [future]  │
│   (IES serialiser)  │  (cmd execution)   │  (NDP120 inference)     │
├─────────────────────────────────────────────────────────────────────┤
│                       DATA UTILITY LAYER                            │
│   FifoQueue<T> │ Task base classes │ Publisher/Subscriber pattern   │
├─────────────────────────────────────────────────────────────────────┤
│                       ACQUISITION LAYER                             │
│   EEG Acquisition Task (highest priority, DRDY-driven SPI read)     │
├─────────────────────────────────────────────────────────────────────┤
│                       HARDWARE LAYER                                │
│   ADS1299 (SPI) │ NDP120 (on-board) │ BLE (nRF) │ USB Serial        │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 Architectural Principles

| Principle | Implementation |
|-----------|----------------|
| **No wait on queue space** | Producers never wait for a consumer to free a slot — drop-oldest guarantees forward progress; brief mutex contention between concurrent `push()`/`pop()` is possible but bounded |
| **Publisher/Subscriber** | Tasks publish data to subscriber queues; producers don't know consumers |
| **Thread-safe** | All queue operations are mutex-guarded (`rtos::Mutex` priority-inheritance, Q18 resolved) |
| **Drop-oldest** | On FIFO overflow, drop oldest entry; producer never waits for queue space (brief mutex contention with a concurrent `pop()` is the only possible wait) |
| **Priority-based** | RTOS scheduler runs highest-priority runnable task |
| **Configurable** | All parameters in single `config.h`; no hardcoded magic numbers |

### 5.3 High-Level Data Flow

```
DRDY ISR → EEG Acquisition Task → [PacketiserTask, ML Processor]
                                        ↓
                                   PacketiserTask ← [EEG, ML, Responses]
                                   (serialises each item to IES WireFrame)
                                        ↓
                                   Gateway Task
                                   ↙         ↘
                         UART Channel    BLE Channel
                              ↓                ↓
                        Physical I/O     Physical I/O

[Commands flow reverse: UART/BLE RX → Gateway → Command Handler → Response]
```

---

## 6. Task Architecture

### 6.1 Task Overview and Priorities

All tasks are RTOS threads managed by Arduino Mbed OS. Task priorities follow CMSIS-RTOS v2 convention.

| Task | Priority | Stack (bytes) | Trigger | Role |
|------|----------|---------------|---------|------|
| **EegAcquisitionTask** | `osPriorityRealtime` (+3) | 4096 | DRDY ISR semaphore | SPI read → distribute to subscribers |
| **PacketiserTask** | `osPriorityAboveNormal` (+1) | 2048 | Queue non-empty | Serialise EEG/ML/responses to IES WireFrames |
| **GatewayTask** | `osPriorityNormal` (0) | 2048 | Queue non-empty | Route data/commands between channels |
| **UartChannelTask** | `osPriorityNormal` (0) | 2048 | RX/TX events | UART I/O + framing |
| **BleChannelTask** (future) | `osPriorityNormal` (0) | 4096 | BLE events | BLE I/O + aggregation |
| **CommandHandlerTask** | `osPriorityNormal` (0) | 2048 | Queue non-empty | Execute commands |
| **MlProcessorTask** (future) | `osPriorityBelowNormal` (-1) | 2048 | Queue non-empty | NDP120 inference |

**Notes:**
- All priorities and stack sizes are configurable in `config.h`
- Distribution happens inline within producer's thread (no separate distribution task)

### 6.2 EEG Acquisition Task

**Type:** `ProducerTask<ADS1299_4_Sample>`

**Implementation:** `eeg.h/.cpp`

**Responsibilities:**
- Wait on DRDY semaphore (signaled by ISR)
- Read ADS1299 via SPI (`updateChannelData()`)
- Increment sample counter
- Distribute sample to all subscribers

**Priority:** Highest (`osPriorityRealtime`) to ensure real-time response to DRDY

**Pseudocode:**
```cpp
void EegAcquisitionTask::run() {
    while (!_stopRequested) {
        _drdySemaphore.acquire();      // Block until DRDY ISR signals
        
        ADS1299_4_Sample sample;
        sample.sample_number = _sampleCounter++;  // Global counter for time sync
        
        ads1299.updateChannelData();
        for (int i = 0; i < 4; i++) {
            sample.channel[i] = ads1299.getChannelData(i+1);
        }
        
        distribute(sample);  // Mutex-guarded per queue; never waits for space (drop-oldest)
    }
}
```

### 6.3 Packetiser Task

**Type:** Multi-consumer, single-producer (consumes three typed queues; produces `WireFrame`)

**Implementation:** `packetiser.h/.cpp`

**Responsibilities:**
- Pop from three typed input queues with fixed priority: Response > EEG > ML
- Serialize each item to IES native wire format (see Q9 / `ies_message_protocol.md` Section 5.1)
- Generate periodic TIME_SYNC frames (Q2: interval configurable in `config.h`)
- Maintain per-stream frame counter (IES byte 1)
- Produce ready-to-transmit `WireFrame` objects; push to Gateway

**Priority:** `osPriorityAboveNormal` (+1) — minimises latency between acquisition and wire

**Dispatch logic:**
```cpp
void PacketiserTask::run() {
    while (!_stopRequested) {
        WireFrame frame;

        // TIME_SYNC: periodic, inserted ahead of any data
        if (millis() - _lastSyncMs >= PACKETISER_SYNC_INTERVAL_MS) {
            frame = serializeTimeSync(micros(), _eegSampleCounter);
            distribute(frame);
            _lastSyncMs = millis();
        }

        // Priority 1: command responses
        Response resp;
        if (_responseQueue.pop(resp)) {
            frame = serializeResponse(resp, _frameCnt++);
            distribute(frame);
            continue;
        }

        // Priority 2: EEG samples
        ADS1299_4_Sample eeg;
        if (_eegQueue.pop(eeg)) {
            frame = serializeEeg(eeg, _frameCnt++);
            distribute(frame);
            continue;
        }

        // Priority 3: ML results (future)
        MLOutput ml;
        if (_mlQueue.pop(ml)) {
            frame = serializeMl(ml, _frameCnt++);
            distribute(frame);
            continue;
        }

        sleepUntilNotified(PACKETISER_SLEEP_MS);
    }
}
```

**IES serialisation helpers:**
```cpp
// EEG frame: [A0][cnt][0x04][ch0 3B][ch1 3B][ch2 3B][ch3 3B][C0] = 16 B
WireFrame serializeEeg(const ADS1299_4_Sample& s, uint8_t cnt);

// Response frame: [A0][cnt][0x4N][cmd_id][status][len][payload...][C0] <= 15 B  (Q9)
WireFrame serializeResponse(const Response& r, uint8_t cnt);

// TIME_SYNC frame: [A0][cnt][0x51][ts_us 4B][sample_cnt 4B][C0] = 12 B
WireFrame serializeTimeSync(uint32_t ts_us, uint32_t sample_cnt);

// ML frame: [A0][cnt][0x61][label][confidence 4B][C0] = 9 B  (future, Q8/Q9)
WireFrame serializeMl(const MLOutput& m, uint8_t cnt);
```


### 6.4 Gateway Task

**Type:** `ConsumerProducerTask<GatewayInput, GatewayOutput>`

**Implementation:** `gateway.h/.cpp`

**Responsibilities:**
- Route `WireFrame` objects from PacketiserTask to **one** channel per frame (UART **or** BLE — never both)
- Route commands from channels to Command Handler
- Maintain mutually exclusive transport enable flags: `enUART`, `enBLE`
- Prioritize responses over streaming data
- Lightweight command validation (discard malformed)

**Priority:** `osPriorityNormal` (0)

**Routing Logic:**
```cpp
void GatewayTask::run() {
    while (!_stopRequested) {
        GatewayInput input;
        if (_incomingQueue.pop(input)) {
            
            if (input.isCommand()) {
                // Validate and forward to Command Handler
                if (validateCommand(input.command)) {
                    distributeToHandler(input.command);
                }
            }
            else if (input.isData()) {
                // Unicast — WireFrame.dest is authoritative (UART or BLE, never both)
                switch (input.data.dest) {
                    case FrameDest::UART: if (_uartTxQueue) _uartTxQueue->push(input.data); break;
                    case FrameDest::BLE:  if (_bleTxQueue)  _bleTxQueue->push(input.data);  break;
                }
            }
        }
        else {
            thisThread::sleep_for(1ms);
        }
    }
}
```

### 6.5 Channel Tasks (UART & BLE)

**Type:** `ConsumerProducerTask<WireFrame, Command>`

**Implementation:** `uart_channel.h/.cpp`, `ble_channel.h/.cpp`

**Responsibilities:**
- **RX path:** Parse incoming bytes → validate → construct `Command` → send to Gateway
- **TX path:** Pop `WireFrame` from input queue → `Serial.write(frame.bytes, frame.len)` (no format knowledge)

**Framing:** None — IES serialization is done by PacketiserTask. Channel task is a pure transport pump.

**Priority:** `osPriorityNormal` (0) for both UART and BLE — consumer must be ≥ producer (`GatewayTask`) priority

### 6.6 Command Handler Task

**Type:** `ConsumerProducerTask<Command, Response>`

**Implementation:** `cmd_handler.h/.cpp`

**Responsibilities:**
- Parse and execute commands: START, STOP, SET_GAIN, SET_ODR, ENABLE_UART, etc.
- Generate `Response` packets (type nibble = **6**, NOT 4)
- Publish responses to PacketiserTask (for priority dispatch before EEG/ML)
- Persist modified settings via `PersistentConfig::save()`

**Full Command Registry:**

| Byte | `CommandId` constant | Origin | Payload | Notes |
|------|----------------------|--------|---------|-------|
| `'b'` | `CMD_START_STREAMING` | iES | none | Must call `ads1299.startADS()`; not just set a flag |
| `'s'` | `CMD_STOP_STREAMING` | iES | none | Must call `ads1299.stopADS()` |
| `'t'` | `CMD_TIME_SYNC` | iES | 4B epoch + 1B CRC | Verify CRC-8; store time offset |
| `'v'` | `CMD_SOFT_RESET` | iES | none | Send OpenBCI banner via raw UART write |
| `'.'` | *(missing)* | iES | none | Add: no-op, return OK |
| `'d'` | *(missing)* | iES | 1B factor | Add: `g_runtimeState.setDownsamplingFactor()`; persist |
| `'Z'` | *(missing)* | iES | none | Add: impedance ON on CH3+CH4 |
| `'z'` | *(missing)* | iES | none | Add: impedance OFF |
| `'p'` | *(missing)* | iES | 1B sub-cmd | Add: `'o'`→OpenBCI raw; `'i'`→iES µV; debug sub-cmds |
| `0x10` | `CMD_SET_ODR` | Nicla | 1B ODR code | Stub; implement + persist |
| `0x11` | `CMD_SET_GAIN` | Nicla | 2B ch+gain | Stub; implement + persist |
| `0x12` | `CMD_SET_OUTPUT_MODE` | Nicla | 1B mode | **Missing** — add to enum; implement + persist |
| `0x13` | `CMD_SET_CHANNEL_MASK` | Nicla | 1B bitmask | **Missing** — add to enum; implement + persist |
| `0x20` | `CMD_ENABLE_UART` | Nicla | 1B | Stub |
| `0x21` | `CMD_ENABLE_BLE` | Nicla | 1B | Stub |
| `0x30` | `CMD_QUERY_STATUS` | Nicla | none | Stub; return 10-byte status payload |
| `0x31` | `CMD_SAVE_CONFIG` | Nicla | none | **Missing** — add to enum; call `PersistentConfig::save()` |

**Streaming command gate** *(2026-06-19)*

While `_isStreaming` is true, `executeCommand()` accepts only:

| Command | `CommandId` | Rationale |
|---|---|---|
| `STOP_STREAMING` | `'s'` | Required to end recording |
| `HEARTBEAT` | `'.'` | Link keepalive; no state change |
| `TIME_SYNC` | `0x74` | Epoch sync during recording |

All other commands — including `QUERY_STATUS`, `GET_VERSION`, `START_STREAMING`, and every config setter — return `ERR_NOT_ALLOWED` (0x04). Status and config queries must be issued **before** `START` or **after** `STOP`. Host apps should track settings locally during a stream.

**Command Response-Time Characterisation** *(measured 2026-06-19, 921600 baud, 30 runs per command, 1 s settle)*

Commands fall into three latency classes based on what the handler must do:

| Command | `CommandId` | Class | Mean RTT | Max observed | Recommended timeout | Notes |
|---|---|---|---|---|---|---|
| `GET_VERSION` | `0x32` | Fast | 12.6 ms | 15 ms | **100 ms** | Pure state read |
| `QUERY_STATUS` | `0x30` | Fast | 12.5 ms | 13 ms | **100 ms** | Returns 10-byte payload |
| `SET_ODR` | `0x10` | Fast | 12.2 ms | 17 ms | **100 ms** | RuntimeState only; no SPI |
| `SET_OUTPUT_MODE` | `0x12` | Fast | 12.1 ms | 17 ms | **100 ms** | RuntimeState only; no SPI |
| `IMPEDANCE_ON/OFF` | `'Z'`/`'z'` | Fast | 12.1 ms | 14 ms | **100 ms** | RuntimeState only |
| `DOWNSAMPLING` | `0x64` | Fast | 12.3 ms | 14 ms | **100 ms** | RuntimeState only |
| `TIME_SYNC` | `0x74` | Fast | 12.5 ms | 14 ms | **100 ms** | CRC verify + state update |
| `START_STREAMING` | `'b'` | Fast | ~12 ms | ~15 ms | **100 ms** | Starts ADS1299; no SPI register writes |
| `STOP_STREAMING` | `'s'` | Fast | ~12 ms | ~15 ms | **100 ms** | SDATAC command only |
| `SET_CHANNEL_MASK` | `0x13` | SPI write | 20.8 ms | 22 ms | **150 ms** | `writeChannelSettings()` — SDATAC + 4× WREG/RREG |
| `SET_GAIN` | `0x11` | SPI write | 21.5 ms | 30 ms | **150 ms** | `writeChannelSettings()` — SPI jitter up to 30 ms |
| `SAVE_CONFIG` | `0x31` | Flash write | ~13 ms† | 103 ms† | **500 ms** | FlashIAP erase+write; nRF52832 spec worst case ~500 ms |

**Latency class definitions:**
- **Fast** (~12 ms): UART receive → RTOS queue → CommandHandlerTask wakes → state update → response enqueue → PacketiserTask → UartChannelTask → USB CDC transmit. No blocking operations.
- **SPI write** (~20–30 ms): same path plus `writeChannelSettings()` — one `SDATAC` + 4 channels × (3× SPI register write/read + 2 ms `delay()`). SPI bus must be idle (streaming stopped).
- **Flash write** (~13–103 ms): same path plus `FlashIAP::erase()` + `FlashIAP::program()`. First write after boot or after settings change triggers a 4 KB page erase (~85 ms per nRF52832 datasheet); subsequent writes to unchanged data skip erase and complete in ~13 ms.

†SAVE_CONFIG first-call latency varies with page state. In the worst case (full dirty page) the nRF52832 specification allows up to ~500 ms. The 103 ms observed is a typical dirty-write cycle.

**Occasional timeouts (baud-rate drops):** At 921600 baud, approximately 3–7% of SPI-write commands experience a single corrupted byte, causing the response frame to be rejected by the host parser. This is a transport artefact, not a firmware fault. Host software should implement a **retry-up-to-3** policy with the same per-attempt timeout.

**Priority:** `osPriorityNormal` (0)

**Command Execution:**
```cpp
void CommandHandlerTask::run() {
    while (!_stopRequested) {
        Command cmd;
        if (_incomingQueue.pop(cmd)) {
            Response resp;
            resp.cmd_id = cmd.cmd_id;
            
            switch (cmd.cmd_id) {
                case CMD_START:
                    eegTask->start();
                    resp.status = STATUS_OK;
                    break;
                case CMD_STOP:
                    eegTask->stop();
                    resp.status = STATUS_OK;
                    break;
                // ... other commands
            }
            
            distribute(resp);  // Send to PacketiserTask._responseQueue
        }
        else {
            thisThread::sleep_for(5ms);
        }
    }
}
```

### 6.7 ML Processor Task (Future)

**Type:** `ConsumerProducerTask<ADS1299_4_Sample, MLOutput>`

**Implementation:** `ml_processor.h/.cpp`

**Responsibilities:**
- Consume EEG samples from EEG Acquisition Task
- Run inference on NDP120 (Q6 mechanism TBD)
- Produce `MLOutput` (Q8 structure TBD)
- Publish to PacketiserTask

**Priority:** `osPriorityBelowNormal` (-1) — can tolerate latency

---

## 7. Publisher/Subscriber Pattern

### 7.1 Pattern Overview

**Core Concepts:**
- **Producers** (`IProducer<T>`) generate data and fan-out to all subscribed queues via `distribute()`
- **Consumers** (`IConsumer<T>`) **own** their incoming `FifoQueue<T>` and expose it via `getQueue()`
- **Tasks** can be producer-only, consumer-only, or hybrid (both)
- **Ownership:** A consumer task owns the queue; a producer holds a **non-owning pointer** to it
- **No wait on queue space:** `distribute()` never waits for a consumer to free a slot — drop-oldest guarantees immediate insertion; brief mutex contention (with a concurrent `pop()`) is possible but bounded by the critical section
- **Wake-up:** A queue holds a non-owning back-reference to its owner (`INotifiable*`) so it can unblock the consumer the moment data arrives

**Base Interfaces (`task.h`):**
```cpp
// Producer: distributes data to N subscriber queues (non-owning pointers)
template<typename T>
class IProducer {
    virtual void subscribe(IQueue<T>* queue) = 0;  // Called at setup; adds to _subscribers[]
};

// Consumer: owns the incoming queue, exposes a pointer for producers to subscribe to
template<typename T>
class IConsumer {
    virtual IQueue<T>* getQueue() = 0;  // Returns a non-owning pointer to the owned queue
};
```

**Concurrency roles:**
- `ProducerTask<T>::distribute()` → calls `queue->push()` on each subscriber (producer thread context)
- `ConsumerTask<T>::run()` → calls `queue->pop()` (consumer thread context)
- `FifoQueue` → `rtos::Mutex` ensures `push()` and `pop()` are mutually exclusive

### 7.2 FIFO Queue Specification

**Template:** `FifoQueue<T, CAPACITY>` (defined in `fifo_queue.h`)

**Key API:**
```cpp
template<typename T, size_t CAPACITY>
class FifoQueue : public IQueue<T> {
    bool        push(const T& item);         // Mutex-guarded; drop-oldest on full; notifies owner
    bool        pop(T& item);                // Mutex-guarded; returns false immediately if empty
    bool        peek(T& item) const;         // Mutex-guarded; non-destructive; false if empty
    void        clear();                     // Resets ring buffer and all diagnostic counters
    size_t      size() const;                // Thread-safe current item count
    size_t      capacity() const;            // Compile-time constant; no mutex needed
    uint32_t    droppedCount() const;        // Cumulative eviction count since last clear()
    QueueStatus status() const;              // Fill-level / health enum (OVERFLOWED is sticky)
    void        setOwner(INotifiable* owner);// Bind the consuming task; enables push() wake-up
};
```

**Thread-safety:** Every `push()`, `pop()`, `peek()`, `size()`, and `status()` acquires `mutable rtos::Mutex _mutex` (priority-inheritance mutex from Mbed OS). **Not ISR-safe.**

**`QueueStatus` enum** (ordered by severity; highest severity wins):

| Value | Meaning |
|-------|---------|
| `EMPTY` | Queue holds no items |
| `NORMAL` | Items present; fill below `FIFO_NEAR_FULL_PCT`% |
| `NEAR_FULL` | Fill ≥ `FIFO_NEAR_FULL_PCT`% of capacity |
| `OVERFLOWED` | ≥ 1 item evicted since last `clear()` — **sticky until `clear()` is called** |

**Configurable thresholds (both defined in `config.h`):**

| Parameter | Config key | Default | Role |
|-----------|------------|---------|------|
| Near-full status trigger | `FIFO_NEAR_FULL_PCT` | `75` | `status()` → `NEAR_FULL` when fill ≥ 75% of capacity |
| Consumer wake threshold | `TASK_WAKE_THRESHOLD_PCT` | `0` | `push()` calls `owner->notify()` on every push (wake on every item) |

### 7.3 Queue Ownership and Consumer Wake-up

**Ownership model:** Each consumer task owns its input queue(s) as member variables and registers itself as the owner by calling `setOwner(this)` in its constructor. The queue holds a non-owning `INotifiable*` back-pointer. Producers only ever hold non-owning `IQueue<T>*` pointers acquired via `getQueue()`.

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Consumer Task  (concrete: PacketiserTask, GatewayTask, UartChannelTask…) │
│                                                                          │
│   Owns ──────────────────────────────────────────────────────────────┐   │
│                                                                      ▼   │
│   ┌───────────────────────────────────────────────────────────────────┐  │
│   │  FifoQueue<T, N>                                                  │  │
│   │                                                                   │  │
│   │  _buf[N]             static ring buffer (zero heap after ctor)    │  │
│   │  _mutex              rtos::Mutex (priority-inheritance)           │  │
│   │  _owner ──────────────────────────────────────────────────────────┼──┼──► INotifiable*
│   │  _count / _head / _tail                                           │  │    (back-ref to
│   │  _dropped_count                                                   │  │     owning task)
│   │  _status                                                          │  │
│   └───────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│   getQueue() ──► returns &_member_queue   ← called by producer at setup  │
│                                                                          │
│   notify() ◄── called by FifoQueue::push() when fill ≥ threshold         │
│     └─ INotifiable impl: _wakeSem.release() (binary semaphore)           │
│                                                                          │
│   sleepUntilNotified() ← run() loop idles here between data bursts       │
│     └─ _wakeSem.try_acquire_for(timeout_ms)  — woken by notify()         │
└──────────────────────────────────────────────────────────────────────────┘
             ▲
             │  queue->push(item)  [mutex-guarded]
             │  (non-owning IQueue<T>* obtained from getQueue())
┌────────────┴───────────────────┐
│  ProducerTask<T>               │
│                                │
│  _subscribers[]   ◄── non-owning IQueue<T>* list (set up at setup())
│                                │
│  distribute(item):             │
│    for each q in _subscribers: │
│        q->push(item)           │
└────────────────────────────────┘
```

**Consumer wake flow** (triggered by a producer push):

```
[Producer Thread]                          [Consumer Thread]
      │                                          │
      │  queue->push(item)                       │  sleepUntilNotified(timeout_ms)
      │   ├─ _mutex.lock()                       │   └─ _wakeSem.try_acquire_for(...)
      │   ├─ write item to ring buffer           │         BLOCKING ◄──────────────────┐
      │   ├─ _updateStatus()                     │                                     │
      │   ├─ captureSnapshot = _count            │                                     │
      │   ├─ _mutex.unlock()                     │                                     │
      │   └─ if owner && snapshot >= threshold ──┼──► owner->notify()                  │
      │                                          │      └─ _wakeSem.release() ─────────┘
      │                                          │           (unblocks consumer)
      │                                          │   sleepUntilNotified() returns
      │                                          │   run() drains queue via pop()
```

**`setOwner()` is called in each task constructor**, linking the queue to its owning task:

```cpp
// PacketiserTask owns three input queues — all three wake the same task
PacketiserTask::PacketiserTask() : ProducerTask(osPriorityAboveNormal, ...) {
    _eegQueue.setOwner(this);       // EegAcquisitionTask pushes here
    _responseQueue.setOwner(this);  // CommandHandlerTask pushes here
    _mlQueue.setOwner(this);        // ML task pushes here (future)
}

// GatewayTask owns three input queues
GatewayTask::GatewayTask() : BaseTask(osPriorityNormal, ...) {
    _dataQueue.setOwner(this);          // PacketiserTask pushes here
    _cmdFromUartQueue.setOwner(this);   // UartChannelTask pushes here
    _cmdFromBleQueue.setOwner(this);    // BleChannelTask pushes here (future)
}
```

### 7.4 Subscription Relationships

Wired during `setup()` in `ADS1299NiclaFW.ino` **before** any task is started. Producers subscribe to consumer queues by calling `getQueue()`; the returned pointer is stored in `_subscribers[]`:

```cpp
// ── EEG data path ──────────────────────────────────────────────────────────

// EegAcquisitionTask (producer) → PacketiserTask._eegQueue (consumer, depth 64)
eegAcquisitionTask.subscribe(packetiserTask.getEegQueue());

// PacketiserTask (producer) → GatewayTask._dataQueue (consumer, depth 128)
packetiserTask.subscribe(gatewayTask.getDataQueue());

// GatewayTask (producer) → UartChannelTask._txQueue (consumer, depth 64)
gatewayTask.setUartChannel(uartChannelTask.getTxQueue());
gatewayTask.setBleChannel(bleChannelTask.getTxQueue());

// ── Command / response path ────────────────────────────────────────────────

// UartChannelTask RX (producer) → GatewayTask._cmdFromUartQueue (consumer, depth 8)
uartChannelTask.setCmdOutputQueue(gatewayTask.getUartCommandQueue());

// GatewayTask (producer) → CommandHandlerTask._cmdQueue (consumer, depth 8)
gatewayTask.setCmdHandlerQueue(cmdHandlerTask.getCommandQueue());

// CommandHandlerTask (producer) → PacketiserTask._responseQueue (consumer, depth 8)
cmdHandlerTask.setResponseQueue(packetiserTask.getResponseQueue());
```

**Full subscription topology:**

```mermaid
flowchart TD
    ISR([DRDY ISR]) -->|signalDataReady| EEG

    subgraph EEG["EegAcquisitionTask — ProducerTask&lt;ADS1299_4_Sample&gt;"]
        EEG_sub["_subscribers[]  (non-owning ptrs)"]
    end

    subgraph MUX["PacketiserTask — ProducerTask&lt;WireFrame&gt;"]
        direction TB
        MUX_eeg["_eegQueue\nFifoQueue&lt;ADS1299_4_Sample, 64&gt;\nowner = this"]
        MUX_resp["_responseQueue\nFifoQueue&lt;Response, 8&gt;\nowner = this"]
        MUX_ml["_mlQueue\nFifoQueue&lt;MLOutput, 32&gt;\nowner = this (future)"]
        MUX_sub["_subscribers[]  (non-owning ptrs)"]
    end

    subgraph GW["GatewayTask — BaseTask"]
        direction TB
        GW_data["_dataQueue\nFifoQueue&lt;WireFrame, 20&gt;\nowner = this"]
        GW_uart["_cmdFromUartQueue\nFifoQueue&lt;Command, 8&gt;\nowner = this"]
        GW_ble["_cmdFromBleQueue\nFifoQueue&lt;Command, 8&gt;\nowner = this (future)"]
        GW_chsub["_channelSubscribers[]"]
        GW_cmdsub["_cmdHandlerQueue*"]
    end

    subgraph UART["UartChannelTask — BaseTask"]
        UART_tx["_txQueue\nFifoQueue&lt;WireFrame, 64&gt;\nowner = this"]
        UART_cmd["_cmdOutputQueue*\n(non-owning)"]
    end

    subgraph CMD["CommandHandlerTask — BaseTask"]
        CMD_q["_cmdQueue\nFifoQueue&lt;Command, 8&gt;\nowner = this"]
        CMD_resp["_responseQueue*\n(non-owning)"]
    end

    %% Data path
    EEG_sub -->|push + notify| MUX_eeg
    MUX_sub -->|push + notify| GW_data
    GW_chsub -->|push + notify| UART_tx

    %% Command path
    UART_cmd -->|push + notify| GW_uart
    GW_cmdsub -->|push + notify| CMD_q
    CMD_resp -->|push + notify| MUX_resp
```

### 7.5 Overflow Policy

**Drop-Oldest Strategy** (implemented in `FifoQueue::push()`):

1. On `push()` when `_count == CAPACITY`:
   - Advance `_head` by 1 (evict oldest item — no copy needed)
   - Write new item at `_tail`, advance `_tail`, keep `_count` unchanged
   - Increment `_dropped_count`
   - `_updateStatus()` sets `QueueStatus::OVERFLOWED` (sticky)

2. Rationale:
   - **Producer never waits for queue space** — critical for `EegAcquisitionTask` at `osPriorityRealtime`; brief mutex contention with a concurrent `pop()` is the only possible wait, and it is bounded to the duration of the critical section
   - **Newest data preferred** — most recent EEG samples are most useful for real-time streaming
   - **Sticky `OVERFLOWED`** — persists until `clear()`, preventing silent loss

**`OVERFLOWED` is sticky:** `status()` remains `QueueStatus::OVERFLOWED` even after the queue drains. Use `droppedCount()` for precise loss accounting; call `clear()` to reset.

---

## 8. Data Flow & Data Model

Each path below shows the task chain end-to-end. Data structure definitions
appear **at the first queue boundary where that type crosses** — so when you
trace a path you immediately see what is being passed, without jumping elsewhere.

---

### 8.1 Path A — EEG Acquisition (sensor → PacketiserTask)

```
[ADS1299 hardware]
      │  DRDY pin falls (pin 11, active-low)
      ▼
[DRDY ISR]
      │  eegAcquisitionTask.signalDataReady()  →  _drdySemaphore.release()
      │  (no SPI in ISR — Mbed OS constraint)
      ▼
[EegAcquisitionTask]  osPriorityRealtime
      │  _drdySemaphore.acquire()
      │  ads1299.updateChannelData()  →  SPI read, fills channel[0..3]
      │  construct ADS1299_4_Sample { channel[4], sample_number++ }
      │
      │  distribute(sample)  — push to every _subscribers queue
      │
      │  ┌─────────────────────────────────────────────────────┐
      │  │  Queue: PacketiserTask._eegQueue                     │
      │  │  Type:  ADS1299_4_Sample  (20 B)                    │
      │  │  Depth: FIFO_DEPTH_STREAMING = 64  →  1,280 B       │
      │  │  Drop policy: drop-oldest (never blocks producer)    │
      │  └─────────────────────────────────────────────────────┘
      │  ┌─────────────────────────────────────────────────────┐
      │  │  Queue: PacketiserTask._mlQueue  (future, ML Processor)│
      │  │  Type:  ADS1299_4_Sample  (20 B)                    │
      │  │  Depth: FIFO_DEPTH_EEG_ML = 32  →  640 B            │
      │  └─────────────────────────────────────────────────────┘
      ▼
[task sleeps — awaits next DRDY semaphore]
```

**`ADS1299_4_Sample`** — EEG sample unit (`eeg.h`, 20 B, no padding):

```cpp
struct ADS1299_4_Sample {
    int32_t   channel[4];    // CH1–CH4: 24-bit ADC, sign-extended to int32
    uint32_t  sample_number; // monotonic counter — gap detection & timestamp reconstruction
};
```

No per-sample timestamp. Host reconstructs time:
`timestamp_us = sync_ts + (sample_number − sync_sample_number) × period_us`
Scale: **≈ 0.5364 µV/LSB** (Vref = 4.5 V, gain = 1, 24-bit two's complement).

---

### 8.2 Path B — Data Output (PacketiserTask → channels → wire)

```
[PacketiserTask]  osPriorityAboveNormal
      │
      │  Pop with priority order:
      │    1. Response          from  _responseQueue  (highest — immediate delivery)
      │    2. ADS1299_4_Sample  from  _eegQueue
      │    3. MLOutput          from  _mlQueue        (future)
      │
      │  Serialize each item to IES native wire format:
      │    ADS1299_4_Sample  →  [A0][cnt][0x04][ch0 3B][ch1 3B][ch2 3B][ch3 3B][C0]  (16 B)
      │    Response          →  [A0][cnt][0x4N][cmd_id][status][len][payload…][C0]   (≤15 B) (Q9)
      │    MLOutput          →  [A0][cnt][0x61][label][confidence 4B][C0]            ( 9 B)  (future, Q9)
      │    TIME_SYNC         →  [A0][cnt][0x51][ts_us 4B][sample_cnt 4B][C0]         (12 B)
      │
      │  Increments per-stream frame counter (IES byte 1)
      │  One input item → one WireFrame; no combining
      │
      │  construct WireFrame { len, bytes[IES_MAX_FRAME_SIZE] }
      │
      │  distribute(frame)  — push to every _subscribers queue
      │
      │  ┌─────────────────────────────────────────────────────┐
      │  │  Queue: GatewayTask._dataQueue                      │
      │  │  Type:  WireFrame  (21 B)                           │
      │  │  Depth: FIFO_DEPTH_GATEWAY_DATA = 128  →  2,688 B   │
      │  └─────────────────────────────────────────────────────┘
      ▼
[GatewayTask]  osPriorityNormal
      │
      │  pop WireFrame from _dataQueue
      │  switch (frame.dest) — unicast to UART or BLE (never both)
      │
      │  ┌────────────────────────────────────────────────────────┐
      │  │  Queue: UartChannelTask._txQueue                       │
      │  │  Type:  WireFrame  (21 B)                              │
      │  │  Depth: UART_TX_QUEUE_SIZE = 64  →  1,344 B            │
      │  └────────────────────────────────────────────────────────┘
      │  ┌────────────────────────────────────────────────────────┐
      │  │  Queue: BleChannelTask._txQueue  (future)              │
      │  │  Type:  WireFrame  (21 B)                              │
      │  │  Depth: BLE_TX_QUEUE_SIZE = 10  →  210 B               │
      │  └────────────────────────────────────────────────────────┘
      ▼
[UartChannelTask]  osPriorityNormal        [BleChannelTask]  (future)
      │                                           │
      │  pop WireFrame from _txQueue              │  pop WireFrame
      │  Serial.write(frame.bytes, frame.len)     │  fragment to BLE MTU
      │  — no format knowledge required —         │  characteristic.notify()
      ▼                                           ▼
[Physical UART TX]                        [Physical BLE notify]
```

**`WireFrame`** — pre-serialized IES wire frame (`packetiser.h`, 22 B):

```cpp
#define IES_MAX_FRAME_SIZE  20  // 4-ch EEG frame = 16 B; headroom for future types

enum class FrameDest : uint8_t { UART = 0, BLE = 1 };  // unicast only — no ALL

struct WireFrame {
    uint8_t    len;                       // valid bytes in bytes[]
    FrameDest  dest;                      // unicast routing target (internal only)
    uint8_t    bytes[IES_MAX_FRAME_SIZE]; // ready-to-transmit IES-format frame
};
// sizeof = 22 B
```

IES native frame structure (see `ies_message_protocol.md` Section 5.1):
```
[0xA0 start][frame_count 1B][type_ch 1B: (type<<4)|num_ch][ch0 3B]...[chN 3B][0xC0 stop]
     type nibble: 0=EEG  4=RESPONSE  5=TIME_SYNC  6=ML_OUTPUT
     ch nibble:   number of 3-byte channel data fields
```

**`Response`** — reply from CmdHandler, consumed from `_responseQueue` (`cmd.h`, 12 B):

```cpp
struct Response {
    uint8_t   cmd_id;                        // echoes Command::cmd_id
    CmdStatus status;                        // OK | ERR_UNKNOWN | ERR_BAD_PAYLOAD | …
    uint8_t   payload[IES_CMD_PAYLOAD_MAX];  // response data (command-specific, ≤ 8 B)
    uint8_t   payload_len;
    CmdSource dest;                          // transport for the reply
};  // sizeof == 12 B  (IES_CMD_PAYLOAD_MAX = 8, from config.h)
```

**`MLOutput`** — ML result, future (`packetiser.h`, ≈ 8 B):

```cpp
struct MLOutput {
    uint8_t  class_label;  // classification result
    float    confidence;   // [0.0, 1.0]
};  // sizeof ≈ 8 B (1 B + 3 B padding + 4 B float)
```

---

### 8.3 Path C — Command Input (wire → CmdHandler → PacketiserTask)

```
[UART RX bytes]                         [BLE Write event]
      │                                        │
[UartChannelTask]  osPriorityNormal     [BleChannelTask]  (future)
      │                                        │
      │  accumulate bytes, detect frame        │  parse BLE payload
      │  unescape, validate CRC                │
      │  construct Command { cmd_id,           │  construct Command
      │      payload[], source=UART }          │      source=BLE }
      │                                        │
      │  ┌──────────────────────────────────────────────────────┐
      │  │  Queue: GatewayTask._cmdFromUartQueue                │
      │  │  Type:  Command  (11 B)                              │
      │  │  Depth: FIFO_DEPTH_CMD = 8  →  88 B                  │
      │  └──────────────────────────────────────────────────────┘
      │  ┌──────────────────────────────────────────────────────┐
      │  │  Queue: GatewayTask._cmdFromBleQueue  (future)       │
      │  │  Type:  Command  (11 B)                              │
      │  │  Depth: FIFO_DEPTH_CMD = 8  →  88 B                  │
      │  └──────────────────────────────────────────────────────┘
      ▼
[GatewayTask]  osPriorityNormal
      │
      │  pop Command from either cmd queue
      │  lightweight validation (ID range, payload_len)
      │  discard malformed; forward valid
      │
      │  ┌──────────────────────────────────────────────────────┐
      │  │  Queue: CommandHandlerTask._cmdQueue                 │
      │  │  Type:  Command  (11 B)                              │
      │  │  Depth: FIFO_DEPTH_CMD = 8  →  88 B                  │
      │  └──────────────────────────────────────────────────────┘
      ▼
[CommandHandlerTask]  osPriorityNormal
      │
      │  pop Command; execute action:
      │    START / STOP acquisition
      │    SET_GAIN / SET_ODR  →  ADS1299 SPI register writes
      │    ENABLE_UART / ENABLE_BLE  →  Gateway flag update
      │    STATUS query
      │  construct Response { cmd_id, status, payload[], dest }
      │
      │  ┌──────────────────────────────────────────────────────┐
      │  │  Queue: PacketiserTask._responseQueue                │
      │  │  Type:  Response  (12 B)  ← defined in Path B above  │
      │  │  Depth: FIFO_DEPTH_RESPONSE = 8  →  96 B             │
      │  └──────────────────────────────────────────────────────┘
      ▼
[PacketiserTask]  — Response dispatched at highest priority (before EEG/ML)
      │
      └──► serialized to WireFrame → GatewayTask → channel → wire
```

**`Command`** — control-plane message (`cmd.h`, 11 B):

```cpp
struct Command {
    uint8_t   cmd_id;                        // IES_CMD_* ('b', 's', 't', …)
    uint8_t   payload[IES_CMD_PAYLOAD_MAX];  // command arguments (≤ 8 B)
    uint8_t   payload_len;
    CmdSource source;                        // UART | BLE
};  // sizeof == 11 B  (IES_CMD_PAYLOAD_MAX = 8, from config.h)
```

---

### 8.4 Queue Summary

**Rule: every queue is owned (declared as a member variable) by its consumer task.**
Producers hold only a non-owning `IQueue<T>*` pointer. The consumer's constructor calls `setOwner(this)`.
Every `push()` / `pop()` acquires a per-queue `rtos::Mutex`.

| Consumer task | Queue member | Item type | Item size | Depth | Config key | BSS cost |
|---|---|---|---|---|---|---|
| `PacketiserTask` | `_eegQueue` | `ADS1299_4_Sample` | 20 B | 64 | `FIFO_DEPTH_STREAMING` | 1,280 B |
| `PacketiserTask` | `_mlQueue` | `MLOutput` | 8 B | 32 | `FIFO_DEPTH_EEG_ML` | 256 B |
| `PacketiserTask` | `_responseQueue` | `Response` | 12 B | 8 | `FIFO_DEPTH_RESPONSE` | 96 B |
| `GatewayTask` | `_dataQueue` | `WireFrame` | 22 B | 128 | `FIFO_DEPTH_GATEWAY_DATA` | 2,816 B |
| `GatewayTask` | `_cmdFromUartQueue` | `Command` | 11 B | 8 | `FIFO_DEPTH_CMD` | 88 B |
| `GatewayTask` | `_cmdFromBleQueue` | `Command` | 11 B | 8 | `FIFO_DEPTH_CMD` | 88 B |
| `UartChannelTask` | `_txQueue` | `WireFrame` | 22 B | 64 | `UART_TX_QUEUE_SIZE` | 1,408 B |
| `BleChannelTask` | `_txQueue` | `WireFrame` | 22 B | 128 | `BLE_TX_QUEUE_SIZE` | 2,816 B |
| `CommandHandlerTask` | `_cmdQueue` | `Command` | 11 B | 8 | `FIFO_DEPTH_CMD` | 88 B |

**Active BSS (Phase 2, no BLE/ML):** ~5,584 B (~5.5 KB)  
**With BLE + ML (future):** ~6,138 B (~6.0 KB) — pending `BLE_TX_QUEUE_SIZE` update to 128 (→ 2,688 B), actual BLE queue cost will be ~8,610 B (~8.4 KB total)

> ⚠️ `BLE_TX_QUEUE_SIZE = 10` is a placeholder. It must be changed to **128** (`config.h`) before BLE streaming is enabled. At depth 10, the queue overflows on the first 30 ms BLE Connection Interval gap. See `ble_channel_design.md` Section 5.1.

---

### 9.1 Channel Architecture

**Concept:** UART and BLE are **independent**, **bidirectional** communication channels that:
- Run as separate RTOS tasks
- Implement identical `ConsumerProducerTask` pattern
- Share common command vocabulary (Q13)
- Add protocol-specific framing independently

**Gateway Control:**
- `enUART` flag (bool) — enable/disable UART output
- `enBLE` flag (bool) — enable/disable BLE output
- Controlled via commands: `ENABLE_UART`, `DISABLE_UART`, etc.

### 9.2 UART Channel

**Task:** `UartChannelTask`  
**Interface:** USB Serial (CDC), 921600 baud  
**Priority:** `osPriorityNormal` (0)

**RX Path:**
1. Poll `Serial.available()` in task loop
2. Read bytes into buffer
3. Parse frame (detect delimiters, unescape, validate CRC)
4. Construct `Command`
5. Push to Gateway's input queue

**TX Path:**
1. Pop `WireFrame` from input queue
2. `Serial.write(frame.bytes, frame.len)` — IES-format bytes transmitted directly
3. No additional framing — PacketiserTask has already serialized to IES native format

**Framing:** None at this layer. IES wire format ([0xA0]…[0xC0]) is produced by PacketiserTask.

### 9.3 BLE Channel

**Task:** `BleChannelTask`  
**Interface:** BLE 5.0 (nRF52832 built-in, 1M PHY only)  
**Priority:** `osPriorityNormal` (0) — must be ≥ `GatewayTask` (producer) priority to avoid starvation  
**BLE Library:** Mbed OS BLE API (`ble/BLE.h`) — Q11 resolved; see `ble_channel_design.md` Section 4  
**Status:** FUTURE — placeholder stubs only; `BLE_TX_QUEUE_SIZE = 10` in `config.h` must be updated to 128  
**Full design:** `ble_channel_design.md`

**RX Path:**
1. BLE GATT write callback fires on BLE EventQueue thread (not task thread)
2. `feedRxByte()` state machine detects IES boundaries (`[0xA0]`…`[0xC0]`)
3. Constructs `Command`; pushes to `GatewayTask._cmdFromBleQueue` via `_cmdOutputQueue`

**TX Path (Aggregation — not fragmentation):**
1. `sleepUntilNotified()` idle-wait; woken by `GatewayTask::push()` via `INotifiable`
2. Pop `WireFrame` objects from `_txQueue` (non-blocking `pop()` loop)
3. **Aggregate** multiple frame payloads into `tx_buffer[244]` until MTU reached or queue empty (IES EEG frames are 16 B OTA — smaller than the default 20 B ATT MTU; aggregation is required, not fragmentation)
4. `BLE::Instance().gattServer().write()` to notify Central
5. On `BLE_ERROR_NO_MEM`: block on `_txDrainSemaphore` (released by `onDataSent()` callback on BLE EventQueue thread); retry

**BLE EventQueue thread:** A dedicated thread running `ble_event_queue.dispatch_forever()` is required alongside `BleChannelTask` to process SoftDevice events. All communication between this thread and `BleChannelTask` uses `rtos::Semaphore` or `FifoQueue`.

**Framing:** None at this layer. IES wire format already in `WireFrame`.  
**UUIDs:** Q10 open

### 9.4 Command Processing

**Flow:**
```
UART RX / BLE Write → Channel Task → Gateway → Command Handler → Response → PacketiserTask → Gateway → Channel TX
```

**Gateway Role:**
- **Lightweight validation:** Check packet structure, discard malformed
- **Routing:** Forward valid commands to Command Handler
- **Priority:** Responses before streaming data in output queue

**Command Handler Role:**
- **Full parsing:** Decode command ID and arguments
- **Execution:** Perform system action (start/stop, configure, query)
- **Response generation:** Construct `Response` with status code and payload

**Command Vocabulary:** Q13 open — proposed commands in Section 4.2 Q13

---

# PART III: IMPLEMENTATION

## 10. Execution Flow

### 10.1 Startup Sequence

**Detailed Initialization:**

```cpp
void setup() {
    // ===== 1. Board Initialization =====
    nicla::begin();  // NICLA Voice board-specific init (enables VDDIO_EXT 3.3V)

    Serial.begin(SERIAL_BAUD_RATE);  // 921600 baud (1 Mbaud caused Windows CDC corruption)
    // Wait for USB CDC up to SERIAL_CONNECT_TIMEOUT_MS
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < SERIAL_CONNECT_TIMEOUT_MS) {
        delay(SERIAL_CONNECT_POLL_MS);
    }

    // ===== 2. ADS1299 Initialization =====
    ads1299.verbosity = true;
    ads1299.initialize();          // SPI init, reset, register config
    ads1299.verbosity = false;

    // Verify device ID
    byte id = ads1299.ADS_getDeviceID(BOARD_ADS);  // expected: ADS_ID (0x3C)

    // Configure default sample rate
    eegAcquisitionTask.setSampleRate(ADS1299_Library::SAMPLE_RATE_1000);

    // ===== (DEBUG) Test signal routing =====
    #ifdef DEBUG_ENABLE
    ads1299.configureInternalTestSignal(ADSTESTSIG_AMP_2X, ADSTESTSIG_PULSE_FAST);
    for (int i = 0; i < ads1299.numChannels; i++) {
        ads1299.channelSettings[i][INPUT_TYPE_SET] = ADSINPUT_TESTSIG;
    }
    ads1299.writeChannelSettings();
    #endif

    // ===== 3. Wire Publisher/Subscriber Relationships =====
    eegAcquisitionTask.subscribe(packetiserTask.getEegQueue());
    packetiserTask.subscribe(gatewayTask.getDataQueue());
    gatewayTask.setUartChannel(uartChannelTask.getTxQueue());
gatewayTask.setBleChannel(bleChannelTask.getTxQueue());
    uartChannelTask.setCmdOutputQueue(gatewayTask.getUartCommandQueue());
    gatewayTask.setCmdHandlerQueue(cmdHandlerTask.getCommandQueue());
    cmdHandlerTask.setResponseQueue(packetiserTask.getResponseQueue());

    // ===== 4. Attach DRDY Interrupt =====
    pinMode(ADS_DRDY_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(ADS_DRDY_PIN), DRDY_ISR, FALLING);

    // ===== 5. Start All Tasks (lowest to highest priority) =====
    cmdHandlerTask.start();
    gatewayTask.start();
    uartChannelTask.start();
    packetiserTask.start();
    eegAcquisitionTask.start();
    // bleChannelTask.start();  // Future

    // ===== 6. Start ADS1299 Acquisition =====
    // In production firmware, streaming starts on 'b' command only.
    // Unconditional start is only valid in debug mode (bench testing).
#ifdef DEBUG_ENABLE
    ads1299.startADS();  // sends RDATAC + START commands
#endif
}

void loop() {
    // Heartbeat LED (1 Hz blink via HEARTBEAT_LED_INTERVAL_MS)
    // Memory health report every 5 s (DEBUG_ENABLE only)
    // All real EEG work is done in RTOS task threads
}
```

### 10.2 Interrupt Handling (DRDY)

**DRDY ISR Flow:**

```
[nRF52832 GPIO Interrupt]
      │
      │  FALLING edge detected on pin 11 (ADS_DRDY_PIN)
      ▼
[NVIC Interrupt Vector]
      │
      │  Preempt current task
      │  Enter ISR context (elevated priority)
      ▼
void onDRDY_ISR() {
    // CRITICAL: Keep ISR minimal
    // NO SPI access (Mbed OS constraint)
    // NO blocking calls (printf, mutex, etc.)
    
    eegAcquisitionTask.signalDataReady();  // Inline function:
                                           // _drdySemaphore.release()
}
      │
      │  Return from ISR
      ▼
[Mbed RTOS Scheduler]
      │
      │  Check runnable tasks
      │  EEG Acquisition Task now unblocked (semaphore released)
      │  osPriorityRealtime (+3) → highest priority
      │  Preempt current task (if lower priority)
      ▼
[EEG Acquisition Task Resumes]
      │
      │  _drdySemaphore.acquire() returns immediately
      │  Begin SPI read...
```

**Why ISR Doesn't Read SPI:**
- Mbed OS SPI transactions are protected by RTOS mutex
- ISR context cannot call blocking primitives (undefined behavior)
- Semaphore signal is ISR-safe and minimal latency
- Keeps ISR short to avoid interference with BLE SoftDevice timing

**Time Synchronization (Q2):** Separate time sync packets sent periodically (e.g., every 1 second) contain microsecond timestamp and current sample counter. Host reconstructs sample timestamps via interpolation.

### 10.3 Task Scheduling

**Mbed RTOS Scheduling Policy:**

1. **Priority-based preemptive scheduler**
   - Highest-priority runnable task always runs
   - Same-priority tasks use round-robin

2. **Task States:**
   - **Running:** Currently executing on CPU
   - **Ready:** Runnable, waiting for CPU
   - **Blocked:** Waiting on semaphore/queue/sleep
   - **Suspended:** Stopped by `task.stop()`

3. **Scheduling Example:**

```
Timeline (simplified):

T=0ms:    [All tasks blocked on queues/semaphores]
          → Main loop (osPriorityNormal) runs

T=4ms:    [DRDY ISR]
          → EEG Acquisition Task unblocked (osPriorityRealtime)
          → Preempts main loop
          → Reads SPI, distributes sample (1ms total)
          → Blocks on semaphore again

T=5ms:    [PacketiserTask wakes: queue non-empty]
          → osPriorityAboveNormal (+1)
          → Preempts main loop (osPriorityNormal)
          → Pops sample, serializes to IES WireFrame, distributes (0.5ms)
          → Blocks on queue again

T=5.5ms:  [Gateway wakes: queue non-empty]
          → osPriorityNormal (0)
          → Main loop also ready (same priority)
          → Gateway runs (queued first)
          → Routes packet to UART Channel (0.2ms)
          → Blocks on queue again

T=5.7ms:  [UART Channel wakes: queue non-empty]
          → osPriorityNormal (0)
          → Adds framing, writes to Serial (0.5ms)
          → Blocks on queue again

T=6.2ms:  [All tasks blocked again]
          → Main loop resumes (heartbeat LED check)

T=8ms:    [Next DRDY ISR]
          → Cycle repeats...
```

**Key Insights:**
- EEG Acquisition always preempts (highest priority)
- PacketiserTask runs before Gateway/Channels (above-normal priority)
- Gateway, UART, Command Handler share same priority (round-robin)
- Main loop only runs when all tasks blocked

---

## 11. Programming Interfaces

### 11.1 IProducer<T>

**Purpose:** Interface for tasks that produce data of type `T` and distribute to subscribers.

**Definition:** (`task.h`)

```cpp
template<typename T>
class IProducer {
public:
    virtual ~IProducer() = default;
    
    // Subscribe a consumer queue to receive data from this producer.
    // NOT thread-safe — call during setup() only, before start().
    virtual void subscribe(IQueue<T>* queue) = 0;
};
```

**Usage:**
```cpp
ProducerTask<ADS1299_4_Sample> eegTask;
ConsumerTask<ADS1299_4_Sample> muxTask;

eegTask.subscribe(muxTask.getQueue());  // Wire subscription in setup()
```

### 11.2 IConsumer<T>

**Purpose:** Interface for tasks that consume data of type `T` from an input queue.

**Definition:** (`task.h`)

```cpp
template<typename T>
class IConsumer {
public:
    virtual ~IConsumer() = default;
    
    // Get a non-owning pointer to this consumer's incoming queue.
    // Producers call this to subscribe.
    virtual IQueue<T>* getQueue() = 0;
};
```

**Usage:**
```cpp
ConsumerTask<ADS1299_4_Sample> muxTask;

IQueue<ADS1299_4_Sample>* queue = muxTask.getQueue();
// Producer subscribes to this queue
```

### 11.3 BaseTask

**Purpose:** Abstract base class for all RTOS tasks.

**Definition:** (`task.h`)

```cpp
class BaseTask {
protected:
    rtos::Thread*  _thread;
    bool           _stopRequested;
    bool           _isRunning;
    osPriority     _priority;
    uint32_t       _stackSize;
    
    // Pure virtual — concrete tasks implement task loop
    virtual void run() = 0;
    
    // Static thread entry point (required by Mbed OS)
    static void threadEntry(void* arg);
    
public:
    BaseTask(osPriority priority, uint32_t stackSize);
    virtual ~BaseTask();
    
    // Lifecycle management
    void start();           // Spawn thread, begin run()
    void stop();            // Signal stop, join thread
    bool isRunning() const;
};
```

**Usage:**
```cpp
class MyTask : public BaseTask {
protected:
    void run() override {
        while (!_stopRequested) {
            // Task work...
            thisThread::sleep_for(10ms);
        }
    }
    
public:
    MyTask() : BaseTask(osPriorityNormal, 1024) {}
};
```

### 11.4 Template Task Classes

**ProducerTask<T>:**

```cpp
template<typename T>
class ProducerTask : public BaseTask, public IProducer<T> {
protected:
    std::vector<IQueue<T>*>  _subscribers;
    
    // Distribute item to all subscriber queues.
    // push() is mutex-guarded and never waits for space (drop-oldest on full).
    void distribute(const T& item) {
        for (auto* queue : _subscribers) {
            queue->push(item);  // Mutex-guarded; never waits for space (drop-oldest on full)
        }
    }
    
public:
    void subscribe(IQueue<T>* queue) override {
        _subscribers.push_back(queue);
    }
};
```

**ConsumerTask<T>:**

```cpp
template<typename T>
class ConsumerTask : public BaseTask, public IConsumer<T> {
protected:
    FifoQueue<T, FIFO_DEPTH_STREAMING>*  _incomingQueue;
    
public:
    ConsumerTask(size_t queueDepth) 
        : _incomingQueue(new FifoQueue<T, queueDepth>()) {}
    
    IQueue<T>* getQueue() override {
        return _incomingQueue;
    }
};
```

**ConsumerProducerTask<TIn, TOut>:**

```cpp
template<typename TIn, typename TOut>
class ConsumerProducerTask : public BaseTask, 
                              public IConsumer<TIn>, 
                              public IProducer<TOut> {
protected:
    FifoQueue<TIn, FIFO_DEPTH_STREAMING>*  _incomingQueue;
    std::vector<IQueue<TOut>*>              _subscribers;
    
    void distribute(const TOut& item) {
        for (auto* queue : _subscribers) {
            queue->push(item);
        }
    }
    
public:
    IQueue<TIn>* getQueue() override {
        return _incomingQueue;
    }
    
    void subscribe(IQueue<TOut>* queue) override {
        _subscribers.push_back(queue);
    }
};
```

### 11.5 Concrete Task Examples

**EegAcquisitionTask:**

```cpp
class EegAcquisitionTask : public ProducerTask<ADS1299_4_Sample> {
private:
    rtos::Semaphore  _drdySemaphore;
    uint32_t         _sampleCounter;  // Global counter for time sync
    ADS1299          _ads1299;
    
protected:
    void run() override {
        while (!_stopRequested) {
            _drdySemaphore.acquire();  // Block until DRDY ISR signals
            
            ADS1299_4_Sample sample;
            sample.sample_number = _sampleCounter++;  // No per-sample timestamp (Q2 resolved)
            
            _ads1299.updateChannelData();
            for (int i = 0; i < 4; i++) {
                sample.channel[i] = _ads1299.getChannelData(i+1);
            }
            
            distribute(sample);  // Mutex-guarded per queue; never waits for space (drop-oldest)
        }
    }
    
public:
    EegAcquisitionTask() 
        : ProducerTask(osPriorityRealtime, 2048),
          _drdySemaphore(0, 1),
          _sampleCounter(0) {}
    
    // Called from DRDY ISR
    void signalDataReady() {
        _drdySemaphore.release();
    }
};
```

**GatewayTask:**

```cpp
class GatewayTask : public ConsumerProducerTask<GatewayInput, GatewayOutput> {
private:
    bool  _enUART;
    bool  _enBLE;
    
protected:
    void run() override {
        while (!_stopRequested) {
            GatewayInput input;
            if (_incomingQueue->pop(input)) {
                
                if (input.isCommand()) {
                    if (validateCommand(input.command)) {
                        // Route to Command Handler
                        distribute(input.command);
                    }
                }
                else if (input.isData()) {
                    // Route to enabled channels
                    if (_enUART) distributeToUART(input.data);
                    if (_enBLE) distributeToBLE(input.data);
                }
            }
            else {
                thisThread::sleep_for(1ms);
            }
        }
    }
    
public:
    GatewayTask() 
        : ConsumerProducerTask(osPriorityNormal, 1536),
          _enUART(true),
          _enBLE(false) {}
    
    void enableChannel(ChannelType channel, bool enable) {
        if (channel == CHANNEL_UART) _enUART = enable;
        if (channel == CHANNEL_BLE) _enBLE = enable;
    }
};
```

---

## 12. Configuration Management

### 12.1 config.h Structure

**Purpose:** Single source of truth for all system parameters.

**Principles:**
- No magic numbers in code
- All tuning parameters in one file
- Grouped by functional category
- Includes calculated memory budget validation

**File:** `firmware/ADS1299NiclaFW/config.h`

### 12.2 Configuration Sections

#### Section 1: FIFO Queue Depths

```cpp
// EEG sample buffer (PacketiserTask _eegQueue input)
#define FIFO_DEPTH_STREAMING       64    // 64 × 20 bytes = 1280 bytes
                                         // @ 1000 SPS → 64ms buffer
                                         // @ 16 kSPS (max) → 4ms buffer

// EEG to BLE channel buffer
#define FIFO_DEPTH_EEG_BLE         64    // 1280 bytes

// EEG to ML processor buffer
#define FIFO_DEPTH_EEG_ML          32    // 640 bytes

// Gateway input buffer
#define FIFO_DEPTH_GATEWAY         20

// UART Channel output buffer
#define FIFO_DEPTH_UART            10

// BLE Channel output buffer (future)
#define FIFO_DEPTH_BLE             10

// Command Handler input buffer
#define FIFO_DEPTH_CMD             5

// Near-full threshold (percentage)
#define FIFO_NEAR_FULL_PCT         75
```

#### Section 2: RTOS Task Configuration

```cpp
// Task Priorities (CMSIS-RTOS v2)
// Priorities are hardcoded in each task constructor using these config.h constants.
#define TASK_PRIORITY_ACQUISITION    osPriorityRealtime     // +3 (EEG, must be highest)
#define TASK_PRIORITY_BLE            osPriorityNormal       // future
#define TASK_PRIORITY_ML             osPriorityAboveNormal  // future
#define TASK_PRIORITY_LOG            osPriorityLow          // future
#define TASK_PRIORITY_COMMAND        osPriorityNormal       // future
// Note: PacketiserTask, GatewayTask, UartChannelTask, CommandHandlerTask use
// priorities hardcoded in their constructors (osPriorityAboveNormal / osPriorityNormal)

// Task Stack Sizes (bytes)
// Oversized vs. Phase 1 estimates to accommodate CDC USB + debug float paths.
#define STACK_SIZE_ACQUISITION   4096  // SPI call chain + ISR nesting headroom
#define STACK_SIZE_PACKETISER    2048  // snprintf(float) path dominates
#define STACK_SIZE_GATEWAY       2048
#define STACK_SIZE_UART          2048  // processTx/Rx + snprintf(float)
#define STACK_SIZE_CMD_HANDLER   2048  // may invoke ADS1299 SPI writes
#define STACK_SIZE_BLE           4096  // nRF52 SoftDevice internal stack ~1.5 KB
#define STACK_SIZE_ML            2048  // future
#define STACK_SIZE_LOG           1024  // printf-only, no deep calls
```

#### Section 2.5: ADS1299 Hardware Configuration

```cpp
// ADS1299 Sampling Rate (Q5 resolved)
#define ADS1299_DEFAULT_ODR          SPS_250    // Default: 250 SPS for testing
#define ADS1299_MAX_ODR              SPS_16000  // Maximum capacity: 16 kSPS

// ADS1299 Channel Configuration
#define ADS1299_GAIN_DEFAULT         ADS_GAIN01  // Default gain: ×1 (matches iES v0.3 initialize_ads() default)
#define ADS1299_ACTIVE_CHANNELS      4          // 4-channel ADS1299

// Notes:
// - System designed for 16 kSPS maximum throughput:
//   * Raw ADC data: 192 kB/s (4 ch × 3 bytes × 16 kSPS)
//   * ADS1299_4_Sample struct: 320 kB/s (20 bytes × 16 kSPS)
//   * BLE: Requires packet optimization to fit 2 Mbps PHY
// - Default 1000 SPS for validation (20 kB/s struct, BLE-proven)
// - ODR can be changed at runtime via command interface (Q13)
```

#### Section 3: Serial/UART Configuration

```cpp
#define SERIAL_BAUD_RATE             921600     // USB CDC baud rate (1 Mbaud caused data corruption on Windows)
#define SERIAL_CONNECT_TIMEOUT_MS    5000       // max wait for host to open port in setup()
#define SERIAL_CONNECT_POLL_MS       10         // polling interval during that wait
#define SERIAL_WRITE_TIMEOUT_MS      100        // blocking-write deadline
#define UART_BACKPRESSURE_SLEEP_MS   5          // back-off when USB CDC TX is full
```

#### Section 4: Data Stream Format

```cpp
#define STREAM_FORMAT_CSV     0   // human-readable, easy to plot
#define STREAM_FORMAT_BINARY  1   // compact, needed above ~500 SPS
#define STREAM_FORMAT         STREAM_FORMAT_CSV   // active format

// CSV options
#define CSV_INCLUDE_HEADER        1   // emit column header once on connect
#define CSV_FIELD_SEPARATOR       ','
#define CSV_INCLUDE_FRAME_COUNTER 1   // monotonic counter column for drop detection

// Binary options
#define BINARY_PACKET_SYNC_BYTE   0xA5  // framing byte at start of each packet
#define BINARY_INCLUDE_CRC        1     // append CRC-8 for integrity
```

#### Section 5: Debug Logging

```cpp
#define DEBUG_ENABLE  1   // Best-effort debug logging; may drop logs under CDC backpressure

// Per-subsystem enable bits — OR into DEBUG_DEFAULT_MASK
#define DEBUG_ADS1299_INIT    (1 << 0)  // register init sequence
#define DEBUG_ADS1299_SPI     (1 << 1)  // per-transaction SPI trace (very verbose)
#define DEBUG_FIFO_OVERFLOW   (1 << 2)  // log every drop event
#define DEBUG_TASK_TIMING     (1 << 3)  // loop rate / max loop ms per task
#define DEBUG_BLE             (1 << 4)  // future
#define DEBUG_ML              (1 << 5)  // future
#define DEBUG_COMMAND         (1 << 6)  // future
#define DEBUG_UART_CHANNEL    (1 << 7)  // TX/RX stats and fault events
#define DEBUG_STACK_HEALTH    (1 << 8)  // heap free + stack watermark

#define DEBUG_DEFAULT_MASK    (DEBUG_ADS1299_INIT | DEBUG_FIFO_OVERFLOW | \
                               DEBUG_UART_CHANNEL | DEBUG_STACK_HEALTH)
```

#### Section 6: Timing

```cpp
#define HEARTBEAT_LED_INTERVAL_MS    500   // loop() LED blink half-period
```

#### Section 7: System Limits

```cpp
#define IES_MAX_FRAME_SIZE    20   // max IES frame body bytes (4-ch EEG = 16 B)
#define IES_CMD_PAYLOAD_MAX    8   // max bytes in a Command payload field
```

#### Section 8: BLE Configuration (Future)

```cpp
// Q10 open — UUIDs TBD
#define BLE_SERVICE_UUID             "12345678-1234-1234-1234-123456789abc"
#define BLE_DATA_CHAR_UUID           "..."
#define BLE_CONTROL_CHAR_UUID        "..."

#define BLE_MTU                      244  // Maximum (BLE 5.0)
#define BLE_CONN_INTERVAL_MS         7.5  // 7.5ms (minimum for high throughput)
```

#### Section 9: ML/NDP120 Configuration (Future)

```cpp
#define ML_MODEL_PATH                "/fs/model.synpkg"
#define ML_INFERENCE_INTERVAL_MS     100
#define ML_INPUT_WINDOW_SAMPLES      128
```

#### Section 10: Memory Budget Validation

```cpp
// ===== FIFO Queue Memory =====
// ADS1299_4_Sample queues (20 bytes each - no per-sample timestamp)
#define RAM_FIFO_STREAMING     (FIFO_DEPTH_STREAMING * 20)      // 64 × 20 = 1,280 bytes
#define RAM_FIFO_EEG_ML        (FIFO_DEPTH_EEG_ML * 20)         // 32 × 20 = 640 bytes

// WireFrame queues (1 + IES_MAX_FRAME_SIZE = 21 bytes each)
#define RAM_FIFO_GATEWAY       (FIFO_DEPTH_GATEWAY_DATA * 21)   // 128 × 21 = 2,688 bytes
#define RAM_FIFO_UART          (UART_TX_QUEUE_SIZE * 21)        // 64 × 21 = 1,344 bytes
#define RAM_FIFO_BLE           (BLE_TX_QUEUE_SIZE * 21)         // 10 × 21 = 210 bytes (future)

// Command / Response queues (11 / 12 bytes each)
#define RAM_FIFO_CMD           (FIFO_DEPTH_CMD * 11)            // 8 × 11 = 88 bytes (each)
#define RAM_FIFO_RESP          (FIFO_DEPTH_RESPONSE * 12)       // 8 × 12 = 96 bytes

// Total FIFO memory (Phase 2, no BLE/ML)
#define RAM_FIFO_PHASE2        (RAM_FIFO_STREAMING + RAM_FIFO_EEG_ML + \
                                RAM_FIFO_GATEWAY + RAM_FIFO_UART + \
                                RAM_FIFO_CMD * 3 + RAM_FIFO_RESP)
                                // = 1280 + 640 + 420 + 1344 + 264 + 96 = 4,044 bytes (3.9 KB)

#define RAM_FIFO_WITH_BLE      (RAM_FIFO_PHASE2 + RAM_FIFO_BLE)
                                // = 4,044 + 210 = 4,254 bytes (4.2 KB)

// ===== Task Stack Memory =====
#define RAM_STACKS_PHASE2      (STACK_SIZE_ACQUISITION + STACK_SIZE_PACKETISER + \
                                STACK_SIZE_GATEWAY + STACK_SIZE_UART + STACK_SIZE_CMD_HANDLER)
                                // = 4096 + 2048 + 2048 + 2048 + 2048 = 12,288 bytes (12.0 KB)

#define RAM_STACKS_WITH_BLE    (RAM_STACKS_PHASE2 + STACK_SIZE_BLE)
                                // = 12,288 + 4096 = 16,384 bytes (16.0 KB)

#define RAM_STACKS_WITH_ML     (RAM_STACKS_WITH_BLE + STACK_SIZE_ML)
                                // = 16,384 + 2048 = 18,432 bytes (18.0 KB)

// ===== Other Memory Allocations =====
// Estimated global variables and driver state
#define RAM_GLOBALS_EST        1024   // ADS1299 driver, task objects, etc.
#define RAM_MBED_KERNEL_EST    8192   // Mbed OS RTOS kernel overhead (estimated)
#define RAM_HEAP_RESERVE       5120   // Reserved for dynamic allocations

// ===== Total RAM Estimates =====
#define RAM_TOTAL_PHASE2       (RAM_FIFO_PHASE2 + RAM_STACKS_PHASE2 + \
                                RAM_GLOBALS_EST + RAM_MBED_KERNEL_EST + RAM_HEAP_RESERVE)
                                // = 11,300 + 7,168 + 1,024 + 8,192 + 5,120 = 32,804 bytes (32.0 KB)

#define RAM_TOTAL_WITH_BLE     (RAM_FIFO_WITH_BLE + RAM_STACKS_WITH_BLE + \
                                RAM_GLOBALS_EST + RAM_MBED_KERNEL_EST + RAM_HEAP_RESERVE)
                                // = 13,890 + 8,704 + 1,024 + 8,192 + 5,120 = 36,930 bytes (36.1 KB)

#define RAM_TOTAL_WITH_ML      (RAM_FIFO_WITH_BLE + RAM_STACKS_WITH_ML + \
                                RAM_GLOBALS_EST + RAM_MBED_KERNEL_EST + RAM_HEAP_RESERVE)
                                // = 13,890 + 10,752 + 1,024 + 8,192 + 5,120 = 38,978 bytes (38.1 KB)

// ===== Compile-Time Safety Check =====
// nRF52832 has 64KB (65,536 bytes) total RAM
// Conservative limit: 48KB (49,152 bytes) to leave margin for unexpected allocations
#if RAM_TOTAL_PHASE2 > 49152
#error "RAM budget exceeded for Phase 2! Reduce FIFO depths or stack sizes."
#endif

#if RAM_TOTAL_WITH_ML > 49152
#warning "RAM usage for full system (BLE + ML) exceeds safe limit. Profiling required."
#endif

/*
 * RAM Budget Summary:
 * 
 * Phase 2 (UART only):          32.0 KB / 64 KB (50% utilization)
 * With BLE:                      36.1 KB / 64 KB (56% utilization)
 * With BLE + ML:                 38.1 KB / 64 KB (59% utilization)
 * 
 * Remaining for user code/heap:
 * - Phase 2:      ~32 KB free
 * - With BLE:     ~28 KB free
 * - With ML:      ~26 KB free
 * 
 * Memory savings from time sync design (Q2 resolved):
 *   Removed 4-byte timestamp per sample = 640 bytes saved in FIFOs
 * 
 * NOTE: Mbed OS kernel overhead is estimated. Actual usage should be 
 *       measured with runtime profiling tools.
 */
```

#### Section 11: Feature Enable/Disable Flags

```cpp
#define ENABLE_ML                    0  // Future
#define ENABLE_BLE                   0  // Future
#define ENABLE_UART                  1  // Phase 2
#define ENABLE_STATUS_LED            1
#define ENABLE_FIFO_DIAGNOSTICS      1
```

---

### 12.3 Runtime Configuration — RuntimeState

**Purpose:** Thread-safe in-RAM holder for all runtime-modifiable system state.

**Files:** `firmware/ADS1299NiclaFW/runtime_state.h`, `runtime_state.cpp`

#### Managed Parameters

| Category | Parameters |
|----------|-----------|
| **ADS1299 Channels** | Active/inactive state, gain per channel (CH1-CH4) |
| **Streaming** | Enable/disable, sample rate, downsampling factor |
| **Communication** | UART enable, BLE enable |
| **Output Mode** | `OutputMode` — iES native (µV) or OpenBCI raw ADC |

**Not managed:** Compile-time constants (`config.h`), pin assignments (`pinDef.h`).

#### OutputMode Enum

```cpp
enum OutputMode {
    OUTPUT_MODE_IES    = 1,  // iES native: integer µV (default, matches iES v0.3)
    OUTPUT_MODE_OPENBCI = 0  // OpenBCI-compatible: raw 24-bit ADC codes
};
```

When `OUTPUT_MODE_IES`, `PacketiserTask::serialiseEeg()` applies:
```cpp
double uV = (double)raw_count * EEG_SCALE_UV / (double)gain;
int32_t uV_int = (int32_t)round(uV);  // clamp to [-8388608, 8388607]
```
where `EEG_SCALE_UV = 4500000.0f / 8388607.0f ≈ 0.5364418669` (defined in `eeg.h`).
Wire encoding (uint24 big-endian) is **identical** in both modes — only the value differs.

See NOTE-008 in `technical_notes.md` for the full derivation.

#### Single Global Instance

```cpp
extern RuntimeState g_runtimeState;
```

#### Design Features

- **Mutex-guarded** when `MBED_ENABLED` is defined in `runtime_state.h`
- **Validated setters** return `bool` (false on out-of-range input)
- **Dirty flag** for batched hardware updates: `isDirty()` / `applyToHardware(&ads1299)`

#### Integration

- **Command Handler:** Updates state based on received commands; calls `PersistentConfig::save()`
- **EEG Task:** Queries active channels
- **PacketiserTask:** Queries `OutputMode`, gain, downsampling
- **Channel Tasks:** Query output routing flags

---

### 12.4 Persistent Configuration — PersistentConfig

**Purpose:** Persist `RuntimeState` settings across power cycles using flash-emulated EEPROM.

**Files:** `firmware/ADS1299NiclaFW/persistent_config.h`, `persistent_config.cpp`

> **Platform constraint:** The nRF52832 has **no hardware EEPROM**. The Arduino Mbed core
> provides `<EEPROM.h>` backed by `FlashIAP`, reserving ~1 KB of internal NOR flash.
> See NOTE-007 in `technical_notes.md` for wear-protection strategy.

#### EEPROM Layout (schema v1, 18 bytes)

```
Offset  Size  Field                    Default
──────  ────  ───────────────────────  ───────────────────────
 0       4    magic                    0xE1E50001
 4       1    schema_version           1
 5       1    output_mode              1 = iES (µV)
 6       1    sample_rate              ADS1299 ODR code
 7       1    downsampling_factor      1
 8       4    channel_gain[4]          ADS_GAIN01 (×1)
12       1    channel_enable_mask      0b00001111 (CH1–CH4)
13       1    uart_enabled_at_boot     1
14       1    ble_enabled_at_boot      0
15       1    reserved                 0x00
16       2    crc16 (over bytes 0–15)
```

#### API

```cpp
class PersistentConfig {
public:
    static bool load(RuntimeState& state);  // load from EEPROM → state; reset to defaults on CRC fail
    static bool save(const RuntimeState& state);  // write state → EEPROM (read-before-write)
    static void reset();                          // write factory defaults + commit
};
```

#### Boot Sequence Integration

```cpp
// In ADS1299NiclaFW.ino setup():
if (!PersistentConfig::load(g_runtimeState)) {
    Serial.println("EEPROM invalid — defaults loaded and written.");
}
g_runtimeState.applyToHardware(&ads1299);  // apply gain, channel mask, ODR
```

#### Wear Protection

- Read-before-write: each byte is only written if changed
- CRC-16 verification on every load
- Magic word detects first-boot or erased flash

---

## 13. Source Tree Organization

### 13.1 Current Structure

```
NICLA_Voice/
├── pinDef.h                          ← Pin assignments (SPI, DRDY, RST)
│
├── test/SPI_Test/                    ← Development project (Phase 2)
│   ├── SPI_Test.ino                  ← Arduino entry point: setup(), loop()
│   │
│   ├── config.h                      ← SINGLE centralized configuration (12 sections)
│   ├── system_config.h/.cpp          ← Runtime configuration manager (Section 12.3)
│   │
│   ├── task.h, task.cpp              ← Base classes: BaseTask, ProducerTask, ConsumerTask
│   ├── fifo_queue.h                  ← Thread-safe FifoQueue<T,N> template
│   ├── eeg.h, eeg.cpp                ← ADS1299_4_Sample, EegAcquisitionTask
│   │
│   ├── ADS1299_Library.h/.cpp        ← Ported ADS1299 SPI driver
│   ├── ADS1299_Definitions.h         ← Register definitions
│   ├── DSPI.h, DSPI.cpp              ← SPI wrapper (compatibility layer)
│   │
│   ├── cmd.h, cmd.cpp                ← Command/Response structures (Q13 TBD)
│   │
│   └── (future additions):
│       ├── packetiser.h/.cpp         ← PacketiserTask (IES serialiser)
│       ├── gateway.h/.cpp            ← Gateway task
│       ├── uart_channel.h/.cpp       ← UART Channel task
│       ├── ble_channel.h/.cpp        ← BLE Channel task (Q10-Q11)
│       ├── cmd_handler.h/.cpp        ← Command Handler task (Q13)
│       └── ml_processor.h/.cpp       ← ML Processor task (Q6, Q8)
│
├── code_references/                  ← Read-only reference (do not edit)
│   ├── iES_v0.3-master/              ← PRIMARY reference: Original iES firmware (TI-RTOS)
│   │   ├── Board.h, main_tirtos.c, platform.h, ...
│   │   └── ies_app/                  ← ADS1299 driver, circular queue, porting source
│   └── OpenBCI_8/                    ← SECONDARY reference: OpenBCI ADS1299 (Arduino)
│       ├── ADS1299.cpp/.h, Definitions.h, OpenBCI_8.cpp/.h, ...
│
├── NICLA_docs/                       ← Board documentation and scripts
│   ├── general.md, example1-4.md
│   ├── check_consistency.py, convert.py, format_html.py, format_md.py
│   └── deep_code_check.py
│
└── Documentation files:
    ├── firmware_architecture.md              ← This file (architecture spec)
    ├── dev_log.md                            ← Development log and phase plan
    ├── SessionHandOver.md                    ← Living session handover
    ├── ble_channel_design.md                 ← BLE implementation design
    ├── porting_analysis_app.md
    ├── porting_analysis_driver.md
    ├── ies_message_protocol.md
    ├── ies_application_layer_protocol.md
    └── technical_notes.md
```

### 13.2 Module Reference

| Module | Files | Purpose | Status |
|--------|-------|---------|--------|
| **Entry Point** | `SPI_Test.ino` | Arduino setup(), loop(), task wiring | Implemented |
| **Configuration** | `config.h` | Central configuration (12 sections) | Defined |
| **Runtime Config** | `runtime_state.h/.cpp` | Thread-safe runtime state (`OutputMode`, gain, ODR, channel mask) | Stub — implementation pending |
| **Persistent Config** | `persistent_config.h/.cpp` | EEPROM-backed persistence (FlashIAP, 18-byte struct, CRC-16) | Planned (Phase 3.4) |
| **Pin Definitions** | `pinDef.h` | Hardware pin mapping | Defined |
| **Task Framework** | `task.h/.cpp` | BaseTask, Producer/Consumer templates | Implemented |
| **FIFO Queue** | `fifo_queue.h` | Thread-safe ring buffer | Implemented (Q18 open) |
| **EEG Acquisition** | `eeg.h/.cpp` | ADS1299_4_Sample, EegAcquisitionTask | Implemented |
| **ADS1299 Driver** | `ADS1299_Library.h/.cpp`, `DSPI.h/.cpp` | SPI driver for ADS1299 | Ported from iES_v0.3-master |
| **Packetiser** | `packetiser.h/.cpp` | IES serialiser task; WireFrame producer | Current |
| **Gateway** | `gateway.h/.cpp` | Command/data router | Future |
| **UART Channel** | `uart_channel.h/.cpp` | UART I/O + framing | Future (Q9, Q13) |
| **BLE Channel** | `ble_channel.h/.cpp` | BLE I/O + framing | Future (Q10, Q11) |
| **Command Handler** | `cmd_handler.h/.cpp` | Command parser + executor | Future (Q13) |
| **ML Processor** | `ml_processor.h/.cpp` | NDP120 inference task | Future (Q6, Q8) |

---

*End of Document — firmware_architecture_restructured.md*
