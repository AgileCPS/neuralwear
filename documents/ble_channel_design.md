# BLE Channel Implementation Design

**Platform:** Arduino Nicla Voice (nRF52832), Mbed OS 6, ADS1299-4 EEG Frontend  
**Target Throughput:** 1000 Samples Per Second (1 kSPS)  
**Document Status:** Design blueprint — **BLE not yet implemented** (`ble_channel.cpp` contains placeholder stubs)  
**Prerequisites (2026-07-04):** UART path fully validated @ 460800 baud (all four host test scripts pass). **Stage 1** (§9.2) is the next firmware milestone.

---

## 1. System Context

### 1.1 Platform

See `firmware_architecture.md` Section 1.2 for the full platform summary.

BLE-specific hardware constraint: the nRF52832 SoftDevice S132 v7.x supports BLE 5.0 but **1M PHY only** (no 2M PHY). This caps the practical BLE effective throughput at approximately **125 kB/s** — a critical limit for high-rate streaming (see Section 8).

### 1.2 Data Pipeline

See `firmware_architecture.md` Section 8.2 for the full pipeline data flow diagram.

BLE-specific notes:
- `BleChannelTask` is a **pure transport pump** — it receives pre-serialised `WireFrame` objects from `GatewayTask` and aggregates their raw bytes into BLE notifications. It has no knowledge of IES framing.
- `BLE_TX_QUEUE_SIZE = 10` in `config.h` is a **placeholder** that must be updated to **128** before streaming is enabled (see Section 5.1 for the derivation).
- `BleChannelTask` priority is `osPriorityNormal (0)` — must match `GatewayTask` (producer) per the consumer ≥ producer priority rule (see Section 5.2).

### 1.3 BLE-Relevant Data Sizes

For full struct definitions (`ADS1299_4_Sample`, `WireFrame`, IES wire format) see `firmware_architecture.md` Sections 8.1 and 8.2.

**WireFrame: `sizeof = 21 B` always, but only `frame.len` bytes are transmitted OTA.** The struct carries a fixed-size buffer (`bytes[IES_MAX_FRAME_SIZE=20]`) for zero-heap-allocation queue slots; `len` tags how many bytes are actually valid:

| Frame type | OTA bytes (`frame.len`) | bytes[] unused per slot |
|---|---|---|
| EEG 4-ch   | 16 B | 4 B |
| TIME_SYNC  | 12 B | 8 B |
| RESPONSE   | ≤15 B | varies |

This distinction matters for BLE TX queue RAM sizing — the queue is allocated on struct size, not OTA size:

```
BLE_TX_QUEUE_SIZE × sizeof(WireFrame) = N × 21 bytes
  At N=10  (current placeholder): 210 B    ← overflows on first 30 ms CI gap
  At N=64  (minimum safe):        1,344 B
  At N=128 (recommended):         2,688 B  ← 128 ms headroom at 1 kSPS
```

At 1 kSPS with 1 Hz TIME_SYNC: average OTA throughput ≈ **16 kB/s (128 kbps)**.

### 1.4 Current Implementation State (updated 2026-06-19)

| Component | Status |
|---|---|
| UART command TX/RX, PacketiserTask, GatewayTask | ✅ Implemented and tested |
| `CMD_ENABLE_BLE` (0x21) | ⚠️ Stub — toggles flag only |
| `ble_channel.h` / `ble_channel.cpp` | ❌ Stub — `run()` sleeps indefinitely |
| BLE stack init, GATT service | ❌ Not started |
| `BLE_TX_QUEUE_SIZE` in `config.h` | ⚠️ Placeholder **10** — must be **128** before streaming (§5.1) |
| `FrameDest::BLE` routing in GatewayTask | ❌ Not wired |
| Bonding / passkey / whitelist | ❌ Designed only (§3.4, Stage 5+) |

**Next coding work:** `ble_channel_design.md` §9.2 Stage 1 — shared `WireFrame` routing fixes and `BLE_TX_QUEUE_SIZE` resize, then Stage 2 BLE stack init.

---

## 2. Problem Analysis

### 2.1 Why Naive 1-Notification-Per-Frame Fails

Sending one BLE notification per `WireFrame` (1000 notifications/s) appears viable given the small 16-byte payload but fails at three compounding levels.

#### 2.1.1 BLE Transport Overhead per Notification

The nRF52832 SoftDevice does not send raw application bytes. Every `gattServer().write()` call triggers the full BLE protocol stack:

```
Application bytes (16 B)
  + ATT header            (3 B)      → ATT layer
  + L2CAP header          (4 B)      → L2CAP layer
  + Link Layer header     (2 B)      → LL PDU = 25 B
  + Link Layer MIC        (4 B, enc) → encrypted PDU
```

This is an **18.75% payload efficiency** at 16-byte payloads with the default 20-byte ATT MTU. At 1000 notifications/s:

- The SoftDevice's internal TX queue (typically 3–6 slots) saturates immediately.
- The radio cannot process 1000 Link Layer PDUs per second on a single 1M PHY connection — the radio's maximum burst is bounded by the Connection Interval (CI) and PDU size.
- Interrupt load from 1000 SoftDevice TX-complete events per second adds ~10–15% CPU overhead.

**Result:** dropped notifications, stack instability, and unsustainable power draw.

#### 2.1.2 Connection Interval and Queue Depth Mismatch

The BLE radio does not transmit continuously. It sleeps between **Connection Events** and only wakes during the negotiated **Connection Interval (CI)**. During each CI, the Central and Peripheral exchange one or more PDUs.

At a typical CI of **7.5 ms** (minimum), up to 7 frames accumulate before the radio wakes. At **30 ms** (common on mobile operating systems), 30 frames accumulate per CI. With a queue depth of `BLE_TX_QUEUE_SIZE = 10`, the queue overflows before the first CI:

```
Overflow condition:
  frames_accumulated = CI_ms × SPS / 1000
  30 ms × 1000 SPS / 1000 = 30 frames → overflows a depth-10 queue on the first CI
```

The `FifoQueue` drop-oldest policy means the **oldest data is silently discarded** — the EEG record has gaps that cannot be reconstructed.

RF interference compounds this further. In the 2.4 GHz ISM band, Wi-Fi (802.11n/ac channels 1, 6, 11) overlaps BLE advertising and connection channels. The SoftDevice may miss 3–4 consecutive connection events, leaving the queue draining for **90–120 ms** before the next successful exchange.

```
Worst-case buffering requirement:
  Max CI miss span = 4 missed events × 30 ms = 120 ms
  At 1 kSPS: 120 frames minimum
  Recommended queue depth: 128 items (128 ms headroom, power-of-2 friendly)
```

#### 2.1.3 Aggregation, Not Fragmentation

The IES EEG frame (16 bytes OTA) is **smaller** than the default 20-byte ATT MTU. The correct problem framing is **aggregation** — packing multiple frames into one notification — not fragmentation (splitting one large payload).

With Data Length Extension (DLE) and ATT MTU negotiation, the maximum ATT payload rises to:

```
BLE 5.0 LL PDU max:     251 bytes
  − LL header:            2 bytes
  − L2CAP header:         4 bytes
  − ATT header:           3 bytes
  = ATT data payload:   244 bytes  (= IES_MAX_FRAME_SIZE × 12.2 frames)
```

At 244 bytes per notification, a single notification can carry **15 EEG frames** (15 × 16 = 240 bytes, with 4 bytes spare). This reduces notification rate from 1000/s to approximately **67/s** — a 15× reduction in SoftDevice interrupt load.

Payload efficiency rises from 18.75% (single 16-byte frame) to **98.4%** (240 useful bytes in a 244-byte ATT payload).

### 2.2 Quantified Summary

| Metric | Naive (1 notify/frame) | Aggregated (DLE + MTU=244) |
|---|---|---|
| Notifications/s | 1,000 | ~67 |
| Payload efficiency | 18.75% | 98.4% |
| Required queue depth | 1 | 128 |
| RAM for queue (WireFrame, 21 B) | 21 B | 2,688 B |
| Viable for 1 kSPS? | No | Yes |

---

## 3. Requirements and Constraints

### 3.1 Hard Constraints (Hardware and BLE Specification)

| Constraint | Value | Source |
|---|---|---|
| nRF52832 RAM | 64 KB total | Nordic datasheet |
| nRF52832 BLE PHY | 1M only (no 2M PHY) | nRF52832 Product Spec |
| SoftDevice S132 internal TX buffers | 3–6 slots (stack-version dependent) | Nordic S132 spec |
| Maximum ATT data payload (DLE) | 244 bytes | BLE 5.0 specification |
| Connection Interval minimum | 7.5 ms | BLE 5.0 specification |
| Connection Interval maximum | 4,000 ms | BLE 5.0 specification |
| nRF52 SoftDevice stack requirement | ~1.5 KB task stack | Nordic S132 migration guide |

### 3.2 Firmware Architecture Constraints

These are constraints imposed by the existing codebase design that must be honoured.

1. **FifoQueue::pop() is always non-blocking.** The `IQueue<T>::pop(item)` API returns `false` immediately on an empty queue — there is no blocking overload. Blocking-wait is implemented via `BaseTask::sleepUntilNotified(timeout_ms)` plus the `INotifiable::notify()` semaphore signal fired by `FifoQueue::push()`. The `_txQueue.setOwner(this)` call in the task constructor is mandatory to wire this up.

2. **BLE callbacks execute in EventQueue context, not task context.** The Mbed OS BLE API (`ble/BLE.h`) is event-driven; BLE events fire on an `events::EventQueue` dispatch loop. `BleChannelTask` runs as an RTOS thread. Cross-context communication (callback → task) must go through a thread-safe `FifoQueue` or an RTOS semaphore — never a raw shared variable.

3. **`BleChannelTask` receives pre-formatted `WireFrame` objects.** The task must not re-interpret or re-serialise IES frames. It only aggregates `frame.bytes[0..frame.len-1]` into a TX buffer and dispatches the buffer as a raw BLE notification.

4. **`GatewayTask` fans out to all registered channel subscribers.** The `BleChannelTask::getTxQueue()` pointer must be registered with `GatewayTask` via `addSubscriber()` at startup — same pattern as `UartChannelTask`.

5. **Priority rule from `config.h`:** "The consumer must be >= producer priority to keep queues drained." `GatewayTask` is `osPriorityNormal (0)`, so `BleChannelTask` must be `osPriorityNormal (0)` or higher. The current `ble_channel.cpp` placeholder uses `osPriorityBelowNormal (-1)` and `config.h` defines `TASK_PRIORITY_BLE = osPriorityNormal (0)`. **The task must use `TASK_PRIORITY_BLE` and `STACK_SIZE_BLE` from `config.h`**, not hardcoded values — see Section 6 for required fixes.

### 3.3 Derived Requirements

| ID | Requirement |
|---|---|
| R1 | `BLE_TX_QUEUE_SIZE` must be ≥ 128 to cover worst-case CI + 4-miss RF dropout (128 ms at 1 kSPS). |
| R2 | `_txQueue.setOwner(this)` must be called in the `BleChannelTask` constructor to enable `sleepUntilNotified()` wakeup. |
| R3 | MTU and DLE must be negotiated on connection to achieve 244-byte ATT payload. |
| R4 | The TX loop must aggregate multiple `WireFrame` payloads into a single `tx_buffer[244]` before calling `gattServer().write()`. |
| R5 | When `gattServer().write()` returns `BLE_ERROR_NO_MEM`, the TX loop must not drop the buffer — it must wait for the SoftDevice TX queue to drain and retry. |
| R6 | BLE callbacks (`onDataSent`, `onDataWritten`, connection/disconnection) must communicate with `BleChannelTask` exclusively through RTOS-safe primitives. |
| R7 | The BleChannelTask constructor must use `TASK_PRIORITY_BLE` and `STACK_SIZE_BLE` from `config.h`. |
| R8 | A BLE EventQueue dispatch thread must be started alongside `BleChannelTask` to process SoftDevice events. |

---

### 3.4 Security and Pairing Model

#### 3.4.1 Threat Model and Security Level

The device is a research-lab EEG acquisition tool. The operator controls both the firmware and the Central application. The operating environment is a controlled lab, not a public space. Given these constraints, **Just Works pairing with a single bonded device** is the baseline security model. Just Works encrypts the BLE link (protecting against passive eavesdropping) but provides no MITM protection. This is acceptable for v1.

A **UART-configured static passkey** (6-digit PIN) will be added in a later phase after streaming is fully validated. The two-phase approach is deliberate: passkey pairing introduces additional connection setup steps that would slow down early development and testing.

#### 3.4.2 Pairing State Machine

The device holds at most **one trusted device** at any time. The bonding state is tracked by a `_hasBond` flag stored in emulated EEPROM (flash). The state machine:

```
┌─────────────────────────────────────────────────────────────┐
│  BOOT                                                       │
│  Read _hasBond from EEPROM                                  │
└──────────────┬──────────────────────────────────────────────┘
               │
       ┌───────┴────────┐
       │                │
  _hasBond = false  _hasBond = true
       │                │
       ▼                ▼
  Advertise        Advertise with
  (open, any       whitelist filter
   Central can     (bonded peer only)
   connect)              │
       │                 │
       ▼                 ▼
  Pairing          Encrypted
  accepted         reconnect
  (Just Works)     (no re-pairing)
       │
       ▼
  Write _hasBond=true to EEPROM
  Restart advertising with whitelist
```

**Whitelist and privacy:** After bonding, `enablePrivacy(true)` must be enabled. Modern OSes use random private resolvable addresses (RPA) — the peer's BLE address changes periodically. The IRK (Identity Resolving Key) stored during bonding allows the device to resolve the RPA and confirm it is the bonded peer. Without privacy enabled, the whitelist cannot match a peer using RPA.

#### 3.4.3 Single Bond Policy

- Only one bond is stored at any time.
- If the bonded peer tries to re-pair (e.g., after forgetting the device on the host OS), the existing bond must be explicitly cleared first via a UART admin command before re-pairing is accepted. The device does not auto-accept re-pairing from an already-bonded peer address.
- A second unknown Central connecting while a bond exists is rejected by the whitelist filter — it cannot pair without a UART clear command.

#### 3.4.4 Bond Data Storage

Two separate storage mechanisms are used:

| Data | Storage | Size |
|---|---|---|
| `_hasBond` flag | `PersistentConfig` / `EepromLayout` (FlashIAP) | 1 byte |
| Passkey (Phase 2) | `PersistentConfig` / `EepromLayout` (FlashIAP) | 6 bytes |
| Cryptographic bond keys (LTK, IRK) | Mbed OS SecurityManager via KVStore | ~80 bytes |

**Storage implementation:** Phase 4 replaced the originally planned `EEPROM.h` approach with direct Mbed OS `FlashIAP` accessed via `PersistentConfig` / `EepromLayout` (`persistent_config.h`). The current `EepromLayout` is **schema v1** (18 bytes, fixed fields for EEG config). To store BLE bond data, schema v2 must be added:

```
Proposed EepromLayout schema v2 additions (at reserved byte 15, or new trailing bytes):
  Offset  Size  Field          Default
  15       1    ble_has_bond   0x00
  16*      6    ble_passkey    "000000" (ASCII)  — Phase 2 only
  + bump crc16 range and schema_version
```

Do **not** alter schema v1 byte offsets — old firmware would interpret new fields as garbage. The cleanest approach is: bump `schema_version` from 1 to 2, extend the struct, and add a migration path in `PersistentConfig::load()` (v1 layout → v2 defaults for new fields).

**KVStore dependency (must verify early):** The Mbed OS `SecurityManager` persists LTK/IRK via KVStore — a separate flash-backed key-value store compiled into `libmbed.a`. Whether KVStore is available in the Nicla core's precompiled `libmbed.a` must be confirmed with a test sketch (init bonding, power-cycle, confirm bond survives). If KVStore is absent, the fallback is to read bond keys from SecurityManager callbacks and store them manually via `PersistentConfig` (~80 bytes for a single bond — fits comfortably alongside the existing 18-byte config block).

Bond data does **not** need to survive firmware reflash. A reflash always requires a `BLE_CLEAR_BOND` command followed by re-pairing, which is expected behaviour during development.

#### 3.4.5 UART Admin Commands for BLE Management

Since the device has no button or display, all BLE configuration is performed via the UART command channel. The following commands must be implemented as part of the command handler:

| Command | Action |
|---|---|  
| `BLE_CLEAR_BOND` | Wipe bond store, set `_hasBond = false` in EEPROM, restart advertising open |
| `BLE_STATUS` | Report current state: bond exists / connected / advertising, peer address |
| `BLE_FORCE_PAIR` | Temporarily suspend whitelist filter to allow re-pairing without full clear |
| `BLE_SET_PASSKEY` *(Phase 2)* | Set 6-digit static passkey, store in EEPROM, takes effect after next `BLE_CLEAR_BOND` + re-pair |

The Central desktop application must expose these commands in a configuration panel (similar to HC-06 Bluetooth module configuration tools).

#### 3.4.6 Phase 2 — Static Passkey

After streaming is fully validated, the security model upgrades to a UART-configured static passkey:

- Passkey is a 6-digit integer (000000–999999) set via `BLE_SET_PASSKEY` UART command and stored in EEPROM.
- On the next pairing, `SecurityManager::setStaticPasskey()` is called with the stored value, and `IO_CAPS_DISPLAY_ONLY` is declared — the Central must enter the passkey.
- The Central application displays the passkey entry UI (textbox + confirm button).
- Default passkey before any `BLE_SET_PASSKEY` command is issued: device operates as Just Works (no passkey required). Passkey enforcement is opt-in.
- **Transition from Just Works to passkey:** Existing Just Works bonds must be cleared (`BLE_CLEAR_BOND`) before switching, because the existing LTK was negotiated without passkey authentication. Attempting to reconnect after switching security mode without clearing the bond will fail on both sides.

---

## 4. Library Selection

### 4.1 Options Considered

Three options exist on this platform:

**Option A — ArduinoBLE**

ArduinoBLE is a high-level abstraction library shipping with the Arduino ecosystem. On the Nicla Voice (Arduino Mbed core), ArduinoBLE is internally implemented **on top of** the Mbed OS BLE API (`ble/BLE.h`). It provides a simplified API for basic BLE applications.

Critical limitations for this use case:
- `notify()` / `writeValue()` return `void` — no error return, no backpressure detection.
- `BLE_ERROR_NO_MEM` (SoftDevice TX buffer full) is silently swallowed — data is dropped with no indication.
- No `onDataSent()` callback exposed to application code.
- No explicit ATT MTU exchange API.
- No Data Length Extension (DLE) control.

**Verdict: not suitable.** ArduinoBLE cannot implement requirements R3, R4, R5.

**Option B — Raw Nordic SoftDevice API (nRF5 SDK)**

The Nordic nRF5 SDK exposes the SoftDevice directly via `sd_ble_gatts_hvx()`, `sd_ble_gap_data_length_update()`, and `BLE_EVT_TX_COMPLETE` — giving complete control. This is the deepest possible layer.

Critical limitations:
- The Arduino Mbed Nicla core compiles against `libmbed.a`, which drives the SoftDevice through the Mbed OS HCI transport layer (Cordio). Using raw nRF5 SDK calls alongside Mbed OS would conflict with the SoftDevice resource ownership.
- Requires abandoning the Arduino Mbed framework entirely, losing access to all existing Mbed OS RTOS primitives (`rtos::Thread`, `rtos::Mutex`, `FifoQueue`, etc.).

**Verdict: not suitable.** Integration cost is prohibitive on this platform.

**Option C — Mbed OS BLE API (`#include "ble/BLE.h"`)**

The Mbed OS BLE API (also called `BLE_API`) is the correct abstraction layer for this platform. It is:
- Already compiled into `libmbed.a` — no additional dependency.
- The layer that ArduinoBLE wraps — using it directly gives the full API without the ArduinoBLE restrictions.
- Backed by the Cordio stack, which drives the nRF52832 SoftDevice S132 via HCI.

It provides exactly what is required:

| Required capability | Mbed BLE API |
|---|---|
| `onDataSent()` notification (flow control signal) | `GattServer::EventHandler::onDataSent()` |
| `ble_error_t` return from `write()` | `ble_error_t GattServer::write(...)` |
| `BLE_ERROR_NO_MEM` backpressure detection | returned by `write()` when SoftDevice TX buffer full |
| ATT MTU change callback | `GattServer::EventHandler::onAttMtuChange()` |
| Explicit DLE request | `ble.gap().setPreferredConnectionParams()` + link layer negotiation on connect |
| Connection parameter update | `ble.gap().requestConnectionParametersUpdate()` |
| Clean event handler pattern | `GattServer::setEventHandler(handler)` |

### 4.2 Decision: Mbed OS BLE API (`ble/BLE.h`)

Use `#include "ble/BLE.h"` with the `GattServer::EventHandler` interface. This is confirmed as the standard approach in the official Mbed OS 6 documentation and examples.

### 4.3 BLE EventQueue / RTOS Integration Architecture

The Mbed BLE API is **event-driven**, not thread-driven. BLE stack events (connection, disconnection, data sent, data written) are dispatched on an `events::EventQueue`. This creates a context boundary:

```
[BLE SoftDevice IRQ]
        │
        ▼
BLE EventQueue thread      ← BLE callbacks fire here (EventHandler methods)
  e.g. onDataSent()        ← NOT in BleChannelTask thread context
        │
        │  Must use RTOS-safe primitive to cross context boundary:
        │  Option A: rtos::Semaphore (for backpressure signalling)
        │  Option B: FifoQueue<Command> (for RX commands)
        ▼
BleChannelTask thread      ← sleepUntilNotified() / queue pop loop
```

**EventQueue thread setup** (in `ADS1299NiclaFW.ino` or `BleChannelTask::initBle()`):

```cpp
events::EventQueue ble_event_queue;

void schedule_ble_events(BLE::OnEventsToProcessCallbackContext* ctx) {
    ble_event_queue.call(mbed::callback(&ctx->ble, &BLE::processEvents));
}

// In setup / initBle():
BLE& ble = BLE::Instance();
ble.onEventsToProcess(schedule_ble_events);
// Start a dedicated thread to run the EventQueue dispatch loop:
rtos::Thread ble_event_thread(osPriorityNormal, STACK_SIZE_BLE);
ble_event_thread.start(mbed::callback(&ble_event_queue,
                                       &events::EventQueue::dispatch_forever));
```

The `BleChannelTask` thread and the BLE EventQueue thread are **two separate RTOS threads**. All communication between them goes through thread-safe primitives.

---

## 5. Solution Design

### 5.1 Queue Dimensioning

Update `config.h`:

```cpp
#define BLE_TX_QUEUE_SIZE   128   // 128 ms headroom at 1 kSPS; covers 4× 30 ms CI miss
```

**Derivation:**

```
Standard mobile CI:          30 ms
Worst-case missed CI events: 4 (common in 2.4 GHz ISM with Wi-Fi)
Required buffering:          4 × 30 ms = 120 ms → round up to 128 (power-of-2)
RAM cost:                    128 × 21 B = 2,688 B  (fits comfortably in 64 KB)
```

If the Central can reliably use a 7.5 ms CI (e.g., a dedicated USB BLE dongle), the minimum safe depth drops to 32 items (32 ms). 128 is the conservative, mobile-friendly target.

### 5.2 Task Priority and Stack

`ble_channel.cpp` constructor must be changed to:

```cpp
BleChannelTask::BleChannelTask()
    : BaseTask(TASK_PRIORITY_BLE, STACK_SIZE_BLE),
      _cmdOutputQueue(nullptr),
      _connected(false)
{
    _txQueue.setOwner(this);   // REQUIRED: enables sleepUntilNotified() wakeup
}
```

`TASK_PRIORITY_BLE = osPriorityNormal (0)` and `STACK_SIZE_BLE = 4096` from `config.h`. The 4096-byte stack is necessary because the Cordio BLE stack maintains approximately 1.5 KB of internal context on the calling thread's stack during `gattServer().write()`.

> **Note — priority rule conflict:** `GatewayTask` (producer) is `osPriorityNormal (0)`. The general rule in `config.h` states the consumer must be ≥ the producer priority to avoid starvation. `osPriorityBelowNormal (-1)` used in the current placeholder violates this — the BLE queue would fall further behind with every context switch won by `GatewayTask`. `osPriorityNormal (0)` is the minimum safe priority.

### 5.3 MTU and DLE Negotiation

On connection, request maximum ATT MTU and DLE immediately via the BLE EventHandler:

```cpp
void BleEventHandler::onConnectionComplete(const ble::ConnectionCompleteEvent& event) {
    if (event.getStatus() != BLE_ERROR_NONE) return;

    _connHandle = event.getConnectionHandle();
    bleChannelTask.onConnect();

    // Request DLE: ask peer to extend LL PDU to 251 bytes
    BLE::Instance().gap().setPreferredConnectionParams(
        ble::ConnectionParameters()
            .setConnectionInterval(
                ble::conn_interval_t(ble::millisecond_t(15)),   // 15 ms min
                ble::conn_interval_t(ble::millisecond_t(30)))   // 30 ms max
    );

    // Initiate ATT MTU exchange: request 247 (→ 244 data bytes after ATT header)
    // Cordio initiates MTU exchange automatically if mtu > default (23).
    // Set via: ble.gattServer() ATT_MTU configuration at init time.
}

void BleEventHandler::onDataSent(const GattDataSentCallbackParams& params) {
    // Signal BleChannelTask that TX buffer is draining — safe to retry
    _txDrainSemaphore.release();
}
```

The maximum ATT data payload with DLE is **244 bytes**:
```
LL PDU = 251 B → L2CAP = 247 B → ATT = 244 B data payload
```

The `tx_buffer` in `BleChannelTask` must be sized accordingly:
```cpp
static uint8_t tx_buffer[244];
```

### 5.4 Aggregation Loop (TX Path)

The `processTx()` method implements the aggregation state machine. This is the core of the BLE channel design.

**Pattern (modelled on `UartChannelTask::run()`):**

```cpp
void BleChannelTask::run() {
    while (!_stopRequested) {

        // 1. Idle wait — sleepUntilNotified() blocks on binary semaphore.
        //    GatewayTask::push() calls _txQueue.notify() → unblocks here.
        if (_txQueue.isEmpty()) {
            sleepUntilNotified(1);
            continue;
        }

        if (!_connected) {
            // Not connected: drain queue to avoid overflow, discard frames.
            WireFrame discarded;
            while (_txQueue.pop(discarded)) {}
            sleepUntilNotified(10);
            continue;
        }

        // 2. Aggregate: pack consecutive WireFrames into tx_buffer.
        uint8_t  tx_buffer[244];
        uint16_t tx_len = 0;
        WireFrame frame;

        while (_txQueue.pop(frame)) {

            // Check if appending this frame would exceed negotiated MTU.
            if (tx_len + frame.len > _currentMtu) {
                // Dispatch what we have, then restart accumulation.
                dispatchBuffer(tx_buffer, tx_len);
                tx_len = 0;
            }

            // Append frame payload bytes to tx_buffer.
            memcpy(tx_buffer + tx_len, frame.bytes, frame.len);
            tx_len += frame.len;
        }

        // 3. Dispatch remaining bytes in buffer (queue was empty → send now,
        //    don't hold partial data indefinitely).
        if (tx_len > 0) {
            dispatchBuffer(tx_buffer, tx_len);
        }

        // 4. Cooperative yield: let GatewayTask refill the queue.
        rtos::ThisThread::yield();
    }
}
```

**Dispatch conditions** — a notification is sent when **either**:
- Appending the next frame would exceed `_currentMtu` (MTU limit reached), **or**
- `_txQueue.pop()` returns `false` (queue emptied — flush to minimise latency).

The second condition is critical: it prevents frames from sitting in `tx_buffer` indefinitely waiting for enough data to fill the MTU. At 1 kSPS, a 16-frame batch forms within 16 ms — well within any CI, so latency is bounded without special timers.

### 5.5 Backpressure Handling

`GattServer::write()` returns `BLE_ERROR_NO_MEM` when the SoftDevice internal TX queue is full (typically 3–6 PDU slots). This is normal under high throughput.

**Do not drop the buffer.** The `dispatchBuffer()` helper must retry until the SoftDevice accepts the data:

```cpp
void BleChannelTask::dispatchBuffer(const uint8_t* data, uint16_t len) {
    ble_error_t err;
    do {
        err = BLE::Instance().gattServer().write(
            _dataCharHandle, data, len, false);

        if (err == BLE_ERROR_NO_MEM) {
            // SoftDevice TX queue is full.
            // The BLE EventQueue thread will call onDataSent() when a PDU is
            // transmitted and a slot frees up. Wait on the semaphore it signals.
            _txDrainSemaphore.try_acquire_for(rtos::Kernel::Clock::duration_u32(2));
            // Alternatively: rtos::ThisThread::yield() if semaphore is not wired yet.
        }
    } while (err == BLE_ERROR_NO_MEM && !_stopRequested);
}
```

The `_txDrainSemaphore` is released by the `onDataSent()` callback in the BLE EventHandler, which runs on the BLE EventQueue thread. This is the **preferred** mechanism: the BleChannelTask blocks precisely until the SoftDevice has a free slot, rather than spinning with `yield()`.

If the `onDataSent` → semaphore wiring is not yet in place during initial development, `rtos::ThisThread::yield()` is an acceptable temporary substitute:

```cpp
if (err == BLE_ERROR_NO_MEM) {
    rtos::ThisThread::yield();  // temporary; replace with semaphore later
}
```

### 5.6 RX Path (Command Channel)

Commands from the Central arrive as BLE GATT writes on a dedicated **control characteristic**. These fire the `onDataWritten()` callback on the BLE EventQueue thread. The callback must parse the IES frame and push the command to the `GatewayTask._cmdFromBleQueue` via the `_cmdOutputQueue` pointer.

```cpp
void BleEventHandler::onDataWritten(const GattWriteCallbackParams& params) {
    if (params.handle != _ctrlCharHandle) return;
    if (!bleChannelTask._cmdOutputQueue) return;

    // Feed raw bytes into the IES frame boundary detector (same state machine
    // as UartChannelTask::processRx()).
    for (uint16_t i = 0; i < params.len; i++) {
        bleChannelTask.feedRxByte(params.data[i]);
    }
}
```

The `feedRxByte()` state machine mirrors `UartChannelTask::processRx()`: it accumulates bytes between `[0xA0]` start and `[0xC0]` stop delimiters, constructs a `Command` struct, and pushes it to `_cmdOutputQueue`. Since this executes on the BLE EventQueue thread, using `FifoQueue::push()` (mutex-protected, non-blocking) is safe.

---

## 6. Required Configuration and Code Changes

The following changes must be made before BLE streaming is enabled:

### 6.1 `config.h`

```cpp
// CHANGE:
#define BLE_TX_QUEUE_SIZE         10    // ← placeholder

// TO:
#define BLE_TX_QUEUE_SIZE        128    // 128 ms headroom at 1 kSPS

// VERIFY (already correct, do not change):
#define TASK_PRIORITY_BLE     osPriorityNormal   // must be ≥ GatewayTask priority
#define STACK_SIZE_BLE        4096               // Cordio needs ~1.5 KB internal stack
```

### 6.2 `ble_channel.cpp` constructor

```cpp
// CHANGE:
BleChannelTask::BleChannelTask()
    : BaseTask(osPriorityBelowNormal, 2048),   // ← wrong priority, hardcoded stack

// TO:
BleChannelTask::BleChannelTask()
    : BaseTask(TASK_PRIORITY_BLE, STACK_SIZE_BLE),   // ← from config.h
```

Add `_txQueue.setOwner(this);` in the constructor body.

### 6.3 `ADS1299NiclaFW.ino` — startup registration

```cpp
// Register BleChannelTask with GatewayTask (same pattern as UART):
gatewayTask.addSubscriber(bleChannelTask.getTxQueue());

// Start the BLE EventQueue dispatch thread (separate from BleChannelTask):
static events::EventQueue ble_event_queue;
BLE::Instance().onEventsToProcess(schedule_ble_events);
static rtos::Thread ble_event_thread(osPriorityNormal, STACK_SIZE_BLE);
ble_event_thread.start(
    mbed::callback(&ble_event_queue, &events::EventQueue::dispatch_forever));

// Start BleChannelTask after wiring is complete:
bleChannelTask.start();
```

---

## 7. Central Application Requirements

> **Applies to UART and BLE.** UART firmware sends one frame per write (no device
> batching), but Windows USB CDC reads arrive in bursts — the same debatching,
> frame-counter continuity, and TIME_SYNC timestamp rules apply on every transport.
> See `ies_message_protocol.md` §11 and `technical_notes.md` NOTE-010.

### 7.1 Connection Setup

The Central (PC, iOS, Android) must actively negotiate the following immediately on connection:

1. **Connection Interval (CI):** Request 15–30 ms. If the host OS imposes 100 ms (common on iOS in background), the queue will overflow at 1 kSPS within approximately 50 ms of the first missed CI. A 128-item queue provides 128 ms — it survives a single 100 ms OS-imposed CI but not sustained 100 ms intervals.

2. **Data Length Extension (DLE):** Request `TxOctets = 251`, `TxTime = 2120` µs. This extends the LL PDU from 27 to 251 bytes, enabling the 244-byte ATT data payload.

3. **ATT MTU Exchange:** Request `ATT_MTU = 247` (yielding 244 bytes of data after ATT header overhead). This must be initiated by the Central after the connection is established.

### 7.2 Frame Parsing

**UART (USB CDC):** The host `read()` call may return many bytes spanning multiple
complete IES wireframes. Use the same sequential parser as for BLE — do not
assume read boundaries align with frame boundaries.

**BLE:** A single notification will contain **multiple concatenated IES frames**. The Central parser must:

1. Iterate over the received byte array sequentially.
2. Locate the `[0xA0]` start byte.
3. Read `type_ch` byte (offset +2) to determine frame type and length.
4. Locate the `[0xC0]` stop byte at the expected end offset.
5. Validate and extract the frame payload.
6. Repeat from step 2 with the remaining bytes.

**Do not assume frames are aligned to notification boundaries** — a notification may end mid-frame if the MTU is reached mid-aggregation. The parser must maintain carry-over state across notifications.

### 7.3 Timestamp Reconstruction

The Central must not use BLE arrival times for EEG sample timestamps (BLE arrival time jitter = ±CI/2 = ±7.5–15 ms, which is unusable for EEG analysis).

**Protocol:** The `PacketiserTask` emits a `TIME_SYNC` frame at 1 Hz:
```
[0xA0][cnt][0x71][ts_us 4B BE][sample_cnt 4B BE][0xC0]  = 12 B
```
Type nibble = **7** (not 5 — reassigned in Phase 4 to avoid collision with iES EDA/BATT_INFO types; see `dev_log.md` decision log 2026-06). Both `ts_us` and `sample_cnt` are **big-endian** as of Phase 4.

The Central maintains a running linear interpolator:
```
sample_timestamp_us = sync_ts_us + (sample_number - sync_sample_cnt) × (1_000_000 / SPS)
```

Gap detection: if the frame counter between consecutive EEG frames skips more
than the non-EEG frames in between, a firmware drop occurred (queue overflow).
The Central must mark the gap in the dataset. **Do not use host read arrival
time or BLE notification arrival time for gap detection.**

---

## 8. Future Considerations: 16 kSPS Target

### 8.1 Throughput Limit of nRF52832 BLE 1M PHY

At 16 kSPS, 4-ch EEG throughput = 16,000 × 16 bytes = **256 kB/s (2.048 Mbps)**. The nRF52832 with 1M PHY and DLE has a practical effective throughput ceiling of approximately **125 kB/s** — 16 kSPS exceeds this by 2×.

The nRF52832 does **not** support BLE 5.0 2M PHY. 2M PHY (which doubles throughput to ~250 kB/s) requires a different chip such as the **nRF52840**.

### 8.2 Options at 16 kSPS

| Option | Notes |
|---|---|
| Upgrade to nRF52840 + 2M PHY | Board redesign required; 2M PHY gives ~250 kB/s effective |
| Raw ADC packing on BLE channel | Strip IES framing overhead; pack 3-byte raw ADC values → 192 kB/s (marginal) |
| Wired USB CDC (current UART path) | Already handles 16 kSPS at USB FS ceiling (~125 KB/s); wireless not viable |
| Compression / delta encoding | 24-bit delta typically achieves 40–60% compression on EEG; could halve BLE load |

### 8.3 Architectural Provision

The current `GatewayTask` fan-out design supports adding a `BleFormatterTask` between `GatewayTask` and `BleChannelTask`. This formatter would intercept the `WireFrame` stream and re-encode it as raw packed ADC values for the BLE path only, while the UART channel continues to receive standard IES frames. No changes to `EegAcquisitionTask`, `PacketiserTask`, or `GatewayTask` would be required.

---

## 9. Implementation Plan

### 9.1 Pre-Implementation Design Gaps (Must Be Resolved First)

Two design decisions were identified as missing before implementation can begin.

---

#### 9.1.1 Q10 — GATT Service and Characteristic UUIDs (RESOLVED HERE)

The firmware requires a custom BLE GATT service with two characteristics:

| Role | Characteristic | Properties |
|---|---|---|
| **TX** | Device → Host data (EEG frames, TIME_SYNC, ML, responses) | Notify |
| **RX** | Host → Device commands | Write (no response) |

**Assigned UUIDs:**

| Entity | UUID |
|---|---|
| EEG Streaming Service | `A9E07020-0001-4A58-B8C9-3F0DAB7E5C1D` |
| TX Characteristic (notify) | `A9E07020-0002-4A58-B8C9-3F0DAB7E5C1D` |
| RX Characteristic (write)  | `A9E07020-0003-4A58-B8C9-3F0DAB7E5C1D` |

These are custom 128-bit UUIDs with a shared base (`A9E07020-xxxx-4A58-B8C9-3F0DAB7E5C1D`) and distinct 16-bit handles in the second segment. The base should be generated once and treated as fixed — do not regenerate.

TX characteristic: `BLE_GATT_CHAR_PROPERTIES_NOTIFY`, max value length = 244 bytes (full DLE ATT payload).  
RX characteristic: `BLE_GATT_CHAR_PROPERTIES_WRITE_WITHOUT_RESPONSE`, max value length = 20 bytes (a single IES command fits).

---

#### 9.1.2 WireFrame Routing Metadata (`FrameDest`) — RESOLVED HERE

**Problem:** `GatewayTask` fans out all `WireFrame` objects to all channel subscribers unconditionally. This causes two issues:
1. Command responses are broadcast to both UART and BLE, even when the command came from only one of them.
2. The `_uartEnabled`/`_bleEnabled` flags exist but are never applied (TODO in `gateway.cpp`).

**Root cause:** `WireFrame` carries no routing metadata — destination is lost when `PacketiserTask` serialises a `Response` into a `WireFrame`.

**Decision:** Add a `dest` field to `WireFrame` and a `FrameDest` enum to encode **unicast** routing intent. UART and BLE are **never used simultaneously** — exactly one transport is active at a time. There is no broadcast/`ALL` destination. This field is firmware-internal only — it is never transmitted OTA.

**Changes to `packetiser.h`:**

```cpp
/// Unicast routing target for GatewayTask — exactly one channel per frame.
/// Never transmitted OTA; firmware-internal only.
enum class FrameDest : uint8_t {
    UART = 0,  ///< Route to UartChannelTask TX queue only
    BLE  = 1,  ///< Route to BleChannelTask TX queue only
};

struct WireFrame {
    uint8_t   len;                        //  1 B — valid bytes in bytes[]
    FrameDest dest;                       //  1 B — unicast routing target
    uint8_t   bytes[IES_MAX_FRAME_SIZE];  // 20 B — IES wire bytes
};
// sizeof = 22 B (was 21 B — +1 B for dest field)
```

**Active transport rule:** The device operates in one of three states: **UART-only**, **BLE-only**, or **idle**. `_uartEnabled` and `_bleEnabled` are **mutually exclusive** at runtime (`CMD_ENABLE_UART` / `CMD_ENABLE_BLE` or equivalent). Packetiser never emits a frame without a single explicit `dest`.

**Changes to `PacketiserTask`:**

- `serialiseEeg()`, `serialiseTimeSync()`, `serialiseMl()`: set `f.dest` from the **currently active transport** (e.g. helper `activeStreamDest()` reading RuntimeState — returns `FrameDest::UART` or `FrameDest::BLE`, never both).
- `serialiseResponse()`: set `f.dest` from command origin — `CmdSource::UART` → `FrameDest::UART`, `CmdSource::BLE` → `FrameDest::BLE`.

**Changes to `GatewayTask`:**

Replace the generic `_channelSubscribers` vector with typed channel pointers:

```cpp
// In gateway.h — replace:
std::vector<IQueue<WireFrame>*>  _channelSubscribers;

// With:
IQueue<WireFrame>*  _uartTxQueue;  ///< set by setUartChannel(); nullptr if not wired
IQueue<WireFrame>*  _bleTxQueue;   ///< set by setBleChannel();  nullptr if not wired
```

Routing loop in `gateway.cpp` — **unicast only** (no fan-out, no `ALL` case):

```cpp
WireFrame pkt;
while (_dataQueue.pop(pkt)) {
    switch (pkt.dest) {
        case FrameDest::UART:
            if (_uartTxQueue) _uartTxQueue->push(pkt);
            break;
        case FrameDest::BLE:
            if (_bleTxQueue)  _bleTxQueue->push(pkt);
            break;
    }
    hasWork = true;
}
```

The gateway does not re-check enable flags for data frames — `dest` is authoritative. Enable flags remain for command-path / transport selection when the active channel is changed.

**Impact on queue RAM:**

```
BLE_TX_QUEUE_SIZE × sizeof(WireFrame) = 128 × 22 B = 2,816 B  (was 2,688 B; +128 B)
```

Wiring in `ADS1299NiclaFW.ino`:
```cpp
// Replace:
gatewayTask.setUartChannel(uartChannelTask.getTxQueue());
gatewayTask.setBleChannel(bleChannelTask.getTxQueue());
// With:
gatewayTask.setUartChannel(uartChannelTask.getTxQueue());
gatewayTask.setBleChannel(bleChannelTask.getTxQueue());   // add when BLE is enabled
```

---

### 9.2 Implementation Steps

The implementation is divided into five sequential stages. Each stage is independently testable before proceeding to the next.

---

#### Stage 1 — Shared Data Structure and Gateway Fixes

**Why first:** This stage modifies existing working code — the `WireFrame` struct, `PacketiserTask`, and `GatewayTask` — which are all active in the current UART path. If these changes are made correctly the UART path must continue streaming without regression. Doing this before any BLE code exists means the changes can be validated in isolation on a working system. If instead BLE code were added first and then the struct changed, it would be impossible to distinguish a struct bug from a BLE initialisation bug.

**Intended routing behaviour — unicast, one transport at a time:**

1. **Command responses** go back to the **originating channel only** — UART command → `FrameDest::UART`; BLE command → `FrameDest::BLE`.

2. **Data streaming** (EEG, ML, TIME_SYNC) goes to the **single active transport only**. UART and BLE are never active simultaneously. Packetiser sets `dest` from the active transport (RuntimeState); Gateway pushes to exactly one queue.

**Why `FrameDest` must be added before BLE:** `GatewayTask` currently fans out every frame to all subscribers unconditionally, and `serialiseResponse()` discards `Response.dest`. Without unicast routing, BLE would receive UART traffic (and vice versa).

**Why replace `_channelSubscribers` vector with typed pointers:** The gateway must push to `_uartTxQueue` or `_bleTxQueue` based on `pkt.dest` — no fan-out vector.

**Files:** `packetiser.h`, `packetiser.cpp`, `gateway.h`, `gateway.cpp`, `config.h`, `ADS1299NiclaFW.ino`

1. Add `FrameDest` enum (`UART`, `BLE` only — no `ALL`) and `dest` field to `WireFrame` in `packetiser.h` (see §9.1.2).
2. Add `activeStreamDest()` helper; update all four `serialise*()` methods to set explicit `f.dest`.
3. Refactor `GatewayTask` to typed `_uartTxQueue`/`_bleTxQueue` pointers and unicast `switch (pkt.dest)` routing. Remove `_channelSubscribers` vector.
4. Enforce mutual exclusivity of `_uartEnabled`/`_bleEnabled` when switching transport.
5. Update `ADS1299NiclaFW.ino` wiring to use `setUartChannel()` / `setBleChannel()`.
6. Update `config.h`: set `BLE_TX_QUEUE_SIZE = 128`.

**Verification:** Upload and confirm UART streaming unaffected. Confirm enabling BLE transport stops UART data output (and vice versa) — never both at once.

---

#### Stage 2 — BLE Stack Init and GATT Service

**Why second:** Stage 1 established the correct data structures. Stage 2 is the minimum increment to prove that BLE hardware works on this specific board — that the Mbed OS BLE API initialises correctly, that the SoftDevice S132 comes up, and that a Central can discover the custom service. No data flows yet. Isolating this step from the TX loop (Stage 3) prevents a situation where a silent BLE init failure is mistaken for an aggregation bug.

**Key architectural fact — two RTOS threads:** The Mbed BLE API is event-driven, not thread-driven. BLE callbacks (`onDataSent`, `onDataWritten`, connect, disconnect) fire on an `events::EventQueue` dispatch loop running in a dedicated RTOS thread — they do **not** fire in the `BleChannelTask` thread context. This means `BleChannelTask` and the BLE EventQueue thread are two separate `rtos::Thread` instances. Cross-context communication (callback → task) must use thread-safe primitives (`FifoQueue` for commands, `rtos::Semaphore` for backpressure signals). Raw shared variables are not safe.

**Why the constructor must be fixed before any BLE init:** The current `ble_channel.cpp` placeholder hardcodes `osPriorityBelowNormal` and `2048` B stack. `GatewayTask` (the producer) runs at `osPriorityNormal`. If the consumer (`BleChannelTask`) has lower priority, the RTOS scheduler will always prefer `GatewayTask` during contention, meaning the BLE TX queue will accumulate indefinitely during busy periods rather than draining. Additionally, `STACK_SIZE_BLE = 4096` is necessary because the Cordio BLE stack allocates approximately 1.5 KB of internal context on the calling thread's stack during `gattServer().write()`. A 2048 B stack will overflow. The fix is to use `TASK_PRIORITY_BLE` and `STACK_SIZE_BLE` from `config.h`. `_txQueue.setOwner(this)` in the constructor is required to wire the `FifoQueue::push()` → `INotifiable::notify()` → `sleepUntilNotified()` wakeup chain that the TX loop depends on.

**Files:** `ble_channel.h`, `ble_channel.cpp`

1. Fix `BleChannelTask` constructor: use `TASK_PRIORITY_BLE`, `STACK_SIZE_BLE`, add `_txQueue.setOwner(this)`.
2. Define the three UUIDs (§9.1.1) as constants in `ble_channel.h`.
3. Implement `initBle()`:
   - Instantiate `BLE::Instance()`
   - Register `BleEventHandler` (connect, disconnect, `onDataSent`, `onDataWritten`, `onAttMtuChange`)
   - Create the GATT service with TX (notify, 244 B max) and RX (write, 20 B max) characteristics
   - Set device name and appearance
   - Start advertising
4. Add the BLE EventQueue thread setup to `ADS1299NiclaFW.ino` (see §6.3).

**Verification:** Connect with nRF Connect app. Confirm service appears, TX characteristic is notifiable, RX characteristic is writable. No data streaming yet — just confirm GATT structure is correct.

---

#### Stage 3 — TX Path (Aggregation Loop)

**Why third:** GATT service is confirmed working (Stage 2). This stage wires live data into it. The aggregation loop is the most complex part of the BLE channel implementation and the most likely source of latent bugs (MTU boundary handling, backpressure, queue timing). Testing it with a known-good GATT structure makes failures attributable to the loop logic rather than service setup.

**Why aggregation instead of one frame per notification:** A naive 1-frame-per-notification approach at 1 kSPS would require 1000 `gattServer().write()` calls per second. The nRF52832 SoftDevice S132 internal TX queue holds only 3–6 PDU slots — 1000 write attempts per second would hit `BLE_ERROR_NO_MEM` almost immediately. Aggregating up to 15 EEG frames (15 × 16 B = 240 B) into a single 244-byte ATT payload reduces the notification rate to ~67/s — a 15× reduction, well within the SoftDevice's capacity.

**Why `yield()` first, semaphore later:** The `_txDrainSemaphore` (released by `onDataSent()`) is the correct backpressure mechanism — it causes `BleChannelTask` to block precisely until the SoftDevice has a free PDU slot. However, `onDataSent()` is only callable after the GATT TX characteristic exists (Stage 2 must be complete). Using `rtos::ThisThread::yield()` as a placeholder allows Stage 3 to be tested independently of whether `onDataSent()` is correctly wired. The semaphore upgrade happens in Stage 4 when `onDataSent()` is wired as part of the RX path stage — both are BLE EventHandler methods and it is natural to wire them together.

**Why `_currentMtu` defaults to 20:** Before MTU negotiation completes, the ATT data payload is limited to the default 20 bytes (BLE 4.2 baseline). If the aggregation loop uses 244 unconditionally and MTU negotiation is slow or fails, `gattServer().write()` will return an error for oversized writes. Defaulting `_currentMtu = 20` and updating it in `onAttMtuChange()` makes the loop safe from the first connection.

**Files:** `ble_channel.cpp`

1. Implement `dispatchBuffer()` with `BLE_ERROR_NO_MEM` retry loop (§5.5). Use `rtos::ThisThread::yield()` initially as the backpressure primitive; upgrade to `_txDrainSemaphore` once `onDataSent()` is wired.
2. Implement `run()` aggregation loop (§5.4): `sleepUntilNotified()` idle, drain `_txQueue`, aggregate into `tx_buffer[244]`, call `dispatchBuffer()`.
3. Track negotiated MTU via `onAttMtuChange()` callback; store in `_currentMtu` (default = 20 until negotiated).
4. Wire `setBleChannel()` in `ADS1299NiclaFW.ino` and start `bleChannelTask`.

**Verification:** Connect, start streaming. Confirm notifications arrive at ~67/s (not 1000/s). Use nRF Connect or a Python script to count notifications per second and measure payload size. Confirm no drops in `droppedCount()` on `_txQueue`.

---

#### Stage 4 — RX Path (Command Channel)

**Why fourth:** TX streaming is confirmed working (Stage 3). Stage 4 is the correct point to add bidirectional communication for two reasons. First, the `feedRxByte()` IES framing state machine and command dispatch logic are the same code pattern as `UartChannelTask::processRx()` — they can be implemented and tested in isolation without affecting TX. Second, completing the RX path also provides the natural point to upgrade from `yield()` to `_txDrainSemaphore` backpressure, because both `onDataWritten()` (RX path) and `onDataSent()` (semaphore backpressure) are methods of the same `BleEventHandler` class and are wired together.

**Why `CmdSource::BLE` matters:** The `FrameDest` routing fix in Stage 1 makes `GatewayTask` route response frames back to the originating channel only. For this to work, the `Command` struct's `source` field must be set to `CmdSource::BLE` when a command arrives over BLE — that is what causes `serialiseResponse()` to set `dest = FrameDest::BLE` on the response `WireFrame`, which in turn causes `GatewayTask` to send it to `_bleTxQueue` only. If `source` is not set (or left at `CmdSource::UART`), command responses will be misrouted.

**Why the semaphore upgrade belongs here:** During Stage 3, `dispatchBuffer()` uses `rtos::ThisThread::yield()` as a placeholder when `BLE_ERROR_NO_MEM` is returned. This is safe for testing but causes unnecessary spin cycles. The correct mechanism is: `onDataSent()` callback (fired by the BLE EventQueue thread when the SoftDevice transmits a PDU and frees a slot) calls `_txDrainSemaphore.release()`, and `dispatchBuffer()` calls `_txDrainSemaphore.try_acquire_for(2ms)` instead of `yield()`. This is the correct producer-consumer rendezvous across the RTOS / BLE EventQueue thread boundary.

**Files:** `ble_channel.cpp`

1. Implement `feedRxByte()` IES frame boundary state machine (mirror `UartChannelTask::processRx()`): detect `[0xA0]` start, accumulate bytes, detect `[0xC0]` stop, construct `Command` with `source = CmdSource::BLE`, push to `_cmdOutputQueue`.
2. Wire `bleChannelTask.setCmdOutputQueue(gatewayTask.getBleCommandQueue())` in `ADS1299NiclaFW.ino`.
3. Wire `onDataSent()` → `_txDrainSemaphore.release()` (upgrade from `yield()` in Stage 3).

**Verification:** Send a START command from nRF Connect RX characteristic. Confirm acquisition starts. Send STOP. Confirm response frame arrives back on the BLE TX characteristic only (not on UART). Confirm UART path is unaffected.

---

#### Stage 5 — Connection Parameter Negotiation

**Why last:** Stages 1–4 give a fully functional BLE channel that streams and accepts commands. Stage 5 improves throughput and latency by negotiating better link-layer parameters — but it requires a working connection to test, and the parameters only affect performance, not correctness. Doing this last means that if negotiation fails on a particular Central (host OS may reject parameter requests), the channel still functions at reduced efficiency rather than failing to stream at all.

**Why CI, DLE, and MTU negotiation matter:**
- **Connection Interval (CI):** The default CI after connection is typically 100 ms on mobile devices (iOS enforces this in the background). At 1 kSPS, the device accumulates 100 EEG frames per CI event — the TX queue must hold at least 100 items (hence `BLE_TX_QUEUE_SIZE = 128`). Requesting CI = 15–30 ms reduces queue depth requirements and latency. Note: the Central may reject this request; 128-item queue is sized to survive 100 ms CI.
- **Data Length Extension (DLE):** Without DLE, the BLE link layer PDU is capped at 27 bytes. With DLE, it extends to 251 bytes. DLE must be negotiated to achieve the 244-byte ATT payload that aggregation depends on. Without DLE, the effective ATT payload is only 20 bytes — aggregation is impossible and the channel reverts to ~1 frame per notification.
- **ATT MTU Exchange:** The ATT MTU exchange (on top of DLE) sets the application-visible payload limit. Without MTU exchange the limit is 23 bytes (20 data). Requesting ATT MTU = 247 yields 244 data bytes. Cordio may initiate this automatically, but it must be confirmed via `onAttMtuChange()`.

**Files:** `ble_channel.cpp`

1. In `onConnectionComplete()`: call `requestConnectionParametersUpdate()` to request CI = 15–30 ms.
2. Confirm MTU exchange is triggered automatically by Cordio (or trigger manually if needed).
3. Confirm DLE request is sent (`setPreferredConnectionParams()` or `setDataLength()`).

**Verification:** Use nRF Sniffer or nRF Connect log to confirm:
- CI negotiated to ≤ 30 ms
- DLE active (PDUs of 251 bytes visible)
- ATT MTU = 247 (244 data bytes) confirmed in `onAttMtuChange()` log

---

### 9.3 Phase 2 — Passkey Security (Post-Streaming)

**Prerequisite:** Stages 1–5 complete and streaming validated end-to-end.

**Why deferred:** Just Works is sufficient for a lab research device and removes a class of connection-reliability problems during development. Static passkey adds MITM protection but complicates the pairing flow — this complexity is not justified until the streaming path is stable.

**Implementation steps:**

1. Verify KVStore bond persistence (see §3.4.4). If KVStore is unavailable in `libmbed.a`, implement manual LTK/IRK storage in EEPROM as the fallback.
2. Add `BLE_SET_PASSKEY` handling to `cmd_handler.cpp`: parse 6-digit integer from command payload, write to EEPROM, send acknowledgement response.
3. On `BleChannelTask` init: read passkey from EEPROM. If a passkey has been configured, call `SecurityManager::setStaticPasskey()` and declare `IO_CAPS_DISPLAY_ONLY`. If no passkey is configured, declare `IO_CAPS_NONE` (Just Works).
4. Implement the `_hasBond` flag in EEPROM: set after bonding completes (`onBondingComplete()` callback), clear in `BLE_CLEAR_BOND` handler.
5. Implement `enablePrivacy(true)` after bonding to handle random private resolvable addresses (RPA) — required for whitelist to work with modern OS Bluetooth stacks.
6. Implement whitelist filter: after bonding, restart advertising with `FilterPolicy::FILTER_CONNECTION_REQUEST` so only the bonded peer can connect.
7. Add `BLE_CLEAR_BOND`, `BLE_STATUS`, and `BLE_FORCE_PAIR` UART commands (see §3.4.5).
8. Add passkey configuration UI to the desktop Central application: textbox for 6-digit PIN + "Set Passkey" button + "Clear Bond" button.

**Verification:**
- Send `BLE_SET_PASSKEY 123456` via UART. Power-cycle. Confirm Central is prompted for PIN on connect.
- Confirm wrong PIN is rejected.
- Send `BLE_CLEAR_BOND` via UART. Confirm device re-enters open advertising. Confirm re-pairing with new PIN succeeds.
- Power-cycle. Confirm bonded Central reconnects automatically without PIN entry.
- On Central, "forget device". Confirm device rejects reconnect (whitelist active). Confirm `BLE_CLEAR_BOND` + re-pair restores connection.
