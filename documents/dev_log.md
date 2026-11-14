# NICLA EEG Firmware — Development Log & Plan

**Project:** ADS1299 EEG acquisition on Arduino Nicla Voice (nRF52832 / Mbed OS)  
**Hardware:** Nicla Voice + custom ADS1299-4 shield  
**Protocol:** iES WireFrame over USB-CDC UART; BLE (future)


---

## Table of Contents

- [Completed Work](#completed-work)
- [Phase 3 — Analog Input Validation](#phase-3--analog-input-validation)
- [Phase 4 — Full iES Application Layer Protocol](#phase-4--full-ies-application-layer-protocol)
- [Phase 5 — BLE Pairing & Streaming](#phase-5--ble-pairing--streaming)
- [Decision Log](#decision-log)
- [Known Issues & Notes](#known-issues--notes)

---

---

## Completed Work

### Phase 1 — Task Framework ✅ (2026-03 to 2026-04)

All infrastructure tasks implemented and compiling:

| Component | File(s) | Status |
|---|---|---|
| BaseTask / ProducerTask / ConsumerTask | `task.h`, `task.cpp` | ✅ Done |
| FifoQueue (thread-safe, drop-oldest) | `fifo_queue.h` | ✅ Done |
| EegAcquisitionTask (DRDY ISR + semaphore) | `eeg.h`, `eeg.cpp` | ✅ Done |
| ADS1299 driver port (iES → Arduino Mbed) | `ADS1299_Library.*`, `ADS1299_Definitions.h` | ✅ Done |
| `config.h` (all tunable parameters) | `config.h` | ✅ Done |
| `pinDef.h` (pin assignments) | `pinDef.h` | ✅ Done |

### Phase 2 — Streaming Pipeline ✅ (2026-04)

Full task pipeline implemented:

| Component | File(s) | Status |
|---|---|---|
| PacketiserTask (EEG → IES WireFrame) | `packetiser.h`, `packetiser.cpp` | ✅ Done |
| GatewayTask (route to channels) | `gateway.h`, `gateway.cpp` | ✅ Done |
| UartChannelTask (TX drain + RX framing) | `uart_channel.h`, `uart_channel.cpp` | ✅ Done |
| CommandHandlerTask (cmd execute → response) | `cmd_handler.h`, `cmd_handler.cpp` | ✅ Done |
| RuntimeState (thread-safe config) | `runtime_state.h`, `runtime_state.cpp` | ✅ Done |
| Main sketch wiring + task start sequence | `src.ino` | ✅ Done |

### Milestone: Phase 4 Complete — UART Command Pipeline ✅ (2026-06-17, re-validated 2026-06-19)

- Full iES application-layer command set implemented on UART.
- Key additions: streaming command gate (STOP / HEARTBEAT / TIME_SYNC only), lazy ODR application, persistent config via FlashIAP, µV output mode, DEMO/RESET, `CMD_GET_VERSION`.

### Milestone: OpenVIBE Legacy Compatibility Mode — Complete ✅ (2026-07-09)

Added `HostProtocolMode` (`MODERN` default / `LEGACY_IES` opt-in) so
ADS1299NiclaFW can be driven **unmodified** by the original OpenVIBE
`CDriveriES` driver, with all current (Modern) behavior unchanged by default.

- New `CMD_SET_HOST_MODE = 0x14`; auto-normalizes channel mask (→2ch) and
  output mode (→iES µV) once when entering legacy.
- Fixed the OpenVIBE `'t'` time-sync framing bug (its `sendCommand()` never
  actually puts the CRC byte on the wire — `payloadLenForCmd()` now branches
  4 vs 5 bytes by mode).
- `PacketiserTask` suppresses `RESPONSE`/`TIME_SYNC` frames in legacy mode
  (with a forced-through exception for the `CMD_SET_HOST_MODE` ack itself),
  matching the original iES "no acknowledgement" contract OpenVIBE expects.
- Guard rails: `CMD_SET_CHANNEL_MASK` (≠2ch), `CMD_SET_OUTPUT_MODE(RAW)`,
  `CMD_SET_ODR` (≠250 SPS), and `CMD_DOWNSAMPLING` (≠1) all rejected while
  legacy mode is active.
- Sample rate locked to a true 250 SPS with no downsampling while legacy
  mode is active (`CommandHandlerTask::enforceLegacyDefaults()`) — 115200
  baud can't sustain more than that anyway, and running the ADC directly at
  250 SPS avoids the digital-filter mismatch of the `MODERN` default's
  1000 SPS ÷ DS×4. Re-applied after `CMD_DEMO`'s state reset too.
- Baud now runtime-switchable: 460800 (Modern) vs 115200 (Legacy, matching
  OpenVIBE's hardcoded `CBR_115200`) — boot-time selection from persisted
  config (loaded before `Serial.begin()`) plus a live `Serial.end()/begin()`
  switch in `UartChannelTask`, sequenced so the mode-change ack always
  flushes at the *old* baud first.
- Persisted via the repurposed `EepromLayout.reserved` byte → `host_protocol_mode`
  (no schema bump; pre-existing units default to `MODERN`).
- `firmware/uartLogger/ies_protocol.py`: `IesUartClient.switch_host_mode()`,
  `build_time_sync_legacy()`, `build_set_host_mode()`.
- New `firmware/uartLogger/test_openvibe_compat.py` replays OpenVIBE's exact
  byte sequence at 115200 and asserts every frame on the wire is pure
  10-byte 2-channel EEG.
- Full rationale: `ies_message_protocol.md` §12; implementation notes:
  NOTE-011 in `technical_notes.md`.

### Milestone: Phase 3 Analog Input Validation — Complete ✅ (2026-07-04)

Function generator bench test on real ADS1299 channels; production mode (CH3+CH4, no internal test signal) field-tested. See §3.3.

---

## Phase 3 — Analog Input Validation ✅ (2026-07-04)

**Goal:** Verify that the ADS1299-4 correctly digitises a known waveform injected from a function generator into the analog frontend.

**Status:** ✅ **Complete** (2026-07-04)

### 3.1 — ADS1299 Channel Routing Macros ✅ (2026-05-09)

**Problem:** When `DEBUG_ENABLE` is defined, all channels are routed to the internal test signal. There is no production-mode channel routing and no way to enable a subset of channels for intermediate analog testing.

**Solution implemented:**

Three compile-time modes controlled by macros in `config.h`:

| Macro state | CH1 | CH2 | CH3 | CH4 | Test signal |
|---|---|---|---|---|---|
| Neither defined | Powered down | Powered down | Analog | Analog | Off |
| `ANALOG_TEST_ENABLE` only | Analog | Analog | Analog | Analog | Off |
| `DEBUG_ENABLE` only | Test sig | Test sig | Test sig | Test sig | On |
| Both defined | Test sig | Test sig | Test sig | Test sig | On (DEBUG takes precedence) |

**Files changed:**
- `config.h` — added `ANALOG_TEST_ENABLE` macro (commented out by default)
- `src.ino` — replaced single `#ifdef DEBUG_ENABLE` block with three-way `#ifdef`/`#else`/`#ifndef` logic + `Serial.println` confirmation of active mode

**How to use:**
1. Comment out `DEBUG_ENABLE` and `ANALOG_TEST_ENABLE` → production mode (CH3+CH4 only)
2. Uncomment `ANALOG_TEST_ENABLE` only → all 4 channels active for bench testing
3. Uncomment `DEBUG_ENABLE` → internal test signal (streaming pipeline verification)

### 3.4 — PersistentConfig + OutputMode (µV vs Raw ADC) ✅ (2026-06-17)

**Background:** As verified from iES v0.3 source (see NOTE-007, NOTE-008 in `technical_notes.md`):
- The iES protocol sends **integer µV** values in native mode (default).
- Current firmware sends **raw ADC codes** — this is the OpenBCI-compatible behaviour.
- iES default gain is **×1** (`ADS_GAIN01`); AGC can raise it at runtime.
- nRF52832 has **no hardware EEPROM** — persistent config must use `EEPROM.h` (FlashIAP-backed).

**Plan:**

**New files to create:**
- `persistent_config.h` — `PersistentConfig` class, EEPROM layout constants, `EepromLayout` struct
- `persistent_config.cpp` — `load()`, `save()`, `reset()` implementation using `EEPROM.h`

**Files to modify:**
| File | Change |
|---|---|
| `runtime_state.h` | Add `OutputMode` enum + `_outputMode` field + setter/getter; enable `MBED_ENABLED` |
| `runtime_state.cpp` | Implement all TODO stubs |
| `src.ino` `setup()` | Insert `PersistentConfig::load()` + apply-to-hardware |
| `packetiser.cpp` `serialiseEeg()` | Branch on `g_runtimeState.getOutputMode()`: iES → µV conversion; OpenBCI → raw |
| `cmd_handler.cpp` | `CMD_SET_GAIN`, `CMD_SET_ODR`, `CMD_SET_OUTPUT_MODE` → update runtime state + persist |

**µV conversion to apply in `serialiseEeg()` when mode = iES:**
```cpp
// For each channel i:
double uV = (double)s.channel[i] * EEG_SCALE_UV / (double)gain_for_channel_i;
int32_t uV_int = (int32_t)round(uV);
// Clamp to int24 range: [-8388608, 8388607]
uV_int = max(-8388608, min(8388607, uV_int));
// Serialize big-endian 24 bits:
out[0] = (uV_int >> 16) & 0xFF;
out[1] = (uV_int >>  8) & 0xFF;
out[2] =  uV_int        & 0xFF;
```

**`EEG_SCALE_UV`** is already defined in `eeg.h` as `4500000.0f / 8388607.0f ≈ 0.5364418669`.

### 3.3 — Analog Validation Test Procedure ✅ (2026-07-04)

Bench validation with a function generator on the ADS1299 analog frontend:

1. Flash firmware with `DEBUG_ENABLE` commented out (`ANALOG_TEST_ENABLE` as needed for multi-channel bench tests).
2. Connect function generator to channel inputs on the ADS1299-4 shield.
3. Stream and observe via host tool (`FirmwareTestApp.py` / plot pipeline).
4. Confirm digitised waveform frequency and amplitude match generator output on active channels.
5. **Production mode** (`DEBUG_ENABLE` and `ANALOG_TEST_ENABLE` off): CH3+CH4 only — CH1/CH2 powered down; injected waveform visible on CH3/CH4.

**Result:** Analog path validated on real hardware. Phase 3 complete.

---

## Phase 4 — Full iES Application Layer Protocol ✅ (2026-06-17)

**Goal:** Implement the complete iES application-layer command/response protocol, compatible with the original iES firmware and host-side software.

**Status:** ✅ UART implementation complete — BLE is next sprint

**Reference documents:**
- `ies_application_layer_protocol.md`
- `ies_message_protocol.md`

---

## Phase 5 — BLE Pairing & Streaming

**Goal:** Implement BLE connectivity so the device can stream EEG data wirelessly to a BLE-capable host.

**Status:** ⏳ **Next major milestone** — UART path fully validated (2026-07-04); BLE not started

**Prerequisites met (2026-07-04):**
- UART command pipeline, RuntimeState, PersistentConfig/FlashIAP, PacketiserTask wireframes
- Streaming command gate, lazy ODR, `CMD_GET_VERSION` (v0.2.0)
- Host test suite: **all four scripts pass @ 460800 baud** (logic, timing, sequences, streaming + stress ramp)

**Prerequisites still open before BLE coding:**
- ~~Enable `MBED_ENABLED` in `runtime_state.h` for multi-writer mutex~~ — **KI-001 resolved**: `MBED_ENABLED` is already defined unconditionally; mutex is active.

**Notes:**
- Nicla Voice uses nRF52832 with SoftDevice S132; BLE via Mbed BLE API (see `ble_channel_design.md`).
- `ble_channel.h` / `ble_channel.cpp` — Stage 2 GATT service implemented (disabled by `FEATURE_BLE_ENABLE = 0`); TX aggregation loop is a stub sleeping indefinitely.
- `CMD_ENABLE_BLE` (0x21) is implemented; toggles GatewayTask BLE routing flag.
- Full staged plan: `ble_channel_design.md` §9 (Stages 1–5).

### 5.1 — BLE Service Design

- **Service UUID:** `A9E07020-0001-4A58-B8C9-3F0DAB7E5C1D`
- **Characteristics:**
  - TX (Notify): `A9E07020-0002-4A58-B8C9-3F0DAB7E5C1D` — streams WireFrames; client subscribes to notifications.
  - RX (Write no response): `A9E07020-0003-4A58-B8C9-3F0DAB7E5C1D` — host writes command bytes; `BleChannelTask` forwards to Gateway.
- **Library:** Mbed OS BLE API (`ble/BLE.h`) — ArduinoBLE was rejected (see `ble_channel_design.md` §4).
- **Pairing:** Just Works (Phase 1); static passkey (Phase 2). See `ble_channel_design.md` §3.4.

### 5.2 — Implementation Steps

1. Enable `FEATURE_BLE_ENABLE = 1` in `config.h` (Stage 2 GATT code is already written in `ble_channel.cpp`).
2. Implement TX aggregation loop in `BleChannelTask::run()` per `ble_channel_design.md` §5.4.
3. Implement RX `feedRxByte()` IES frame parser per `ble_channel_design.md` §5.6.
4. ~~Enable `MBED_ENABLED` in `runtime_state.h`~~ — already active (KI-001 resolved); mutex guards are in place.
5. Test with nRF Connect app on Android/iOS before integrating with host software.

---

## Decision Log

| Date | Decision | Rationale |
|---|---|---|
| 2026-03 | Port ADS1299 driver from iES_v0.3 (not rewrite) | Preserves verified register sequences; minimal diff for future iES compatibility |
| 2026-03 | Use Mbed OS threads directly (not Arduino loop) | Required for real-time DRDY ISR guarantee; `loop()` is unreliable at 1 kSPS |
| 2026-04 | Publisher/Subscriber with FifoQueue (drop-oldest) | Prevents ISR stalls; EEG data is losable; commands are not (small queue, no overflow in practice) |
| 2026-04 | PacketiserTask produces IES WireFrame directly | Channels become dumb pumps — no protocol knowledge needed in UartChannelTask |
| 2026-04 | TIME_SYNC frame every 1 s instead of per-sample timestamps | 99.98% bandwidth reduction; host reconstructs timestamps from sample counter |
| 2026-04 | Batch frame drain in GatewayTask | Eliminates ~8% throughput deficit from one-frame-per-reschedule overhead |
| 2026-05 | CH1+CH2 powered down in production mode | Only CH3+CH4 connected to analog frontend; floating inputs add noise |
| 2026-05 | Separate `ANALOG_TEST_ENABLE` macro from `DEBUG_ENABLE` | Allows bench-testing all 4 analog channels without enabling test-signal path |
| 2026-05 | Add `OutputMode` runtime parameter (OpenBCI raw / iES µV) | Current firmware sends raw ADC codes; iES native protocol requires integer µV. Need both for interoperability. See NOTE-008. |
| 2026-05 | Default output mode = iES (µV) | Mirrors iES v0.3 default (`openbci_compatible = false`). OpenBCI mode is opt-in. |
| 2026-05 | Default gain = ×1 (`ADS_GAIN01`) | Mirrors iES v0.3 `initialize_ads()` default. AGC can raise it at runtime. |
| 2026-05 | Persistent config via `EEPROM.h` flash emulation | nRF52832 has no hardware EEPROM; Arduino Mbed core provides FlashIAP-backed emulation (~1 KB). 18-byte struct with magic word + CRC-16. Read-before-write to protect flash endurance. See NOTE-007. |
| 2026-06 | Reassign Nicla-only type nibbles to 6–15 (away from iES 0–5) | Early implementation collided: RESPONSE=type 4 (iES EDA) and TIME_SYNC=type 5 (iES BATT_INFO). Corrected to RESPONSE=type 6, TIME_SYNC=type 7, ML_OUTPUT=type 8. iES host will silently discard unknown types ≥6. See `ies_message_protocol.md` §10. |
| 2026-06 | TIME_SYNC is dual-direction and both work without conflict | iES `'t'` command (host→device, sets RTC) and Nicla proactive frame (device→host, type 7) coexist. iES host sends `'t'` and ignores type-7 responses. New BLE host uses both. `cmdTimeSync()` and `serialiseTimeSync()` are independent code paths. |
| 2026-06 | `ads1299.startADS()` removed from `DEBUG_ENABLE` setup block | Auto-starting ADS1299 flooded the USB CDC TX buffer with EEG frames, saturating `availableForWrite()` so `debugTryPrint()` silently dropped all debug messages and delayed command responses by ~1.5 s. Streaming now always starts via explicit `'b'` command only. |
| 2026-06-19 | Strategic test suite: four scripts (`test_logic`, `test_timing`, `test_sequences`, `test_streaming`) | Separates logic validation, timing characterisation, command interaction, and streaming performance. Run one at a time; 1 s settle between commands. |
| 2026-07-04 | BLE Stage 1: `FrameDest` unicast routing | `WireFrame.dest` UART/BLE only; Gateway typed queues; Packetiser `activeStreamDest()`; ENABLE_UART/BLE mutual exclusivity. |
| 2026-07-04 | Phase 3 analog validation complete | Function generator on real channels; production mode CH3+CH4 confirmed before BLE Stage 1. |
| 2026-07-04 | Production UART baud **460800** (4×115200) | All four test scripts pass: logic 11/11, sequences 8/8, timing 0 timeouts, streaming + stress DS×4→×2→×1. DS×2 failed @ 115200 (bandwidth); stable @ 460800. UART Phase 4 complete → BLE Stage 1. |
| 2026-07-04 | BLE Stage 2: stack init + GATT service | Mbed BLE API, EventQueue thread, TX notify / RX write, advertising as NICLA_EEG. |
| 2026-06 | Add `CMD_DEMO = 0x40` and `CMD_RESET = 0x41` as binary protocol commands | Initial design proposed text-based triggers (`"demo"`, `"reset"`); rejected due to collision risk with bare-byte parser. Binary commands integrated cleanly into existing `payloadLenForCmd()` table. |
| 2026-06 | Streaming command gate: whitelist-only during active stream | Prevents SPI race and keeps the command path minimal while EEG is streaming. Allowed during streaming: **STOP**, **HEARTBEAT**, **TIME_SYNC** only. `QUERY_STATUS` and `GET_VERSION` blocked — query before START or after STOP. |
| 2026-06 | Lazy hardware ODR application — defer CONFIG1 write to stream-start | Writing ADS1299 CONFIG1 from `cmdSetOdr` (outside streaming) triggered spurious DRDY pulses, waking EegAcquisitionTask and causing SPI contention. ODR now stored in RuntimeState only; `cmdStartStreaming` applies full hardware state (`setSampleRate` + `applyToHardware`) before `startADS()`. |
| 2026-06 | Rename `crc_8` → `ies_crc8_sht75` in `ies_checksum.h` | `static inline` functions can still emit weak external symbols; a same-named `crc_8` in ADS1299_Library could silently shadow our SHT75 lookup table, always returning ERR_BAD_CRC. Unique name eliminates the risk. |
| 2026-06 | `IES_CMD_PAYLOAD_MAX` raised 8 → 12 | `QUERY_STATUS` response carries 10 bytes. Buffer was too small; firmware truncated the payload and the host-side parser rejected every `0x30` response frame. |
| 2026-07-09 | Add `HostProtocolMode` runtime flag instead of a permanent iES-pure mode | Modern tooling (`ies_protocol.py`, all four `test_*.py` scripts) depends on RESPONSE acks and TIME_SYNC frames; making the firmware always iES-pure would break every existing test. Runtime-switchable flag (default MODERN) preserves current behavior while enabling OpenVIBE compatibility on demand. See NOTE-011. |
| 2026-07-09 | Baud runtime-switchable (460800 Modern / 115200 Legacy) instead of a `config.h` reflash | OpenVIBE hardcodes `CBR_115200`; the device is native USB-CDC so switching baud at runtime carries no physical bit-timing risk. Boot-time selection reads persisted config before `Serial.begin()`; live switching sequenced so the mode-change ack flushes at the old baud first. |

---

## Known Issues & Notes

| ID | Severity | Description | Resolution |
|---|---|---|---|
| NOTE-007 | Fixed | nRF52832 has no hardware EEPROM — persistent config uses flash-emulated EEPROM | Implemented with Mbed OS `FlashIAP` directly (not `EEPROM.h`). 18-byte struct with magic `0xE1E50001` + CRC-16; read-before-write strategy. |
| NOTE-008 | Fixed | iES native protocol sends integer µV, not raw ADC codes | `OutputMode` enum + `serialiseEeg()` µV conversion implemented. Default = iES µV. OpenBCI raw mode available via `CMD_SET_OUTPUT_MODE` or `p`+`i` command. |
| KI-001 | Fixed | `MBED_ENABLED` guard in `runtime_state.h` was thought to be commented out | `MBED_ENABLED` is `#define`'d unconditionally in `runtime_state.h`; mutex guards are always active. |
| KI-002 | Low | `BleChannelTask` TX aggregation loop not yet implemented; GATT service (Stage 2) written but `FEATURE_BLE_ENABLE = 0` | Address in Phase 5 — enable flag and implement TX loop |
| KI-004 | Low | Impedance `cmdImpedanceOn/Off` only sets runtime flag; no ADS1299 LOFF register writes | Phase 5 or later: implement `ads1299.streamSafeLeadOffSetForChannel()` calls |
