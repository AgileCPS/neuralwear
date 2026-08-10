# Session Handover — NICLA Voice EEG Firmware

> **Single living handover document.** Update this file at the end of each session.  
> **Last updated:** 2026-07-04  
> **Firmware version:** v0.2.0 (`config.h`: `FW_VERSION_STR`)  
> **Supersedes:** `session_handover_20260617.md` (archived context only)

---

## Quick answer: “Where are we and what to do today?”

### Where we are

| Layer | Status |
|---|---|
| **UART firmware** (commands, streaming, flash config) | ✅ **Feature-complete** for Phase 4 |
| **Host test suite** (4 scripts) | ✅ **All pass @ 460800** (2026-07-04) |
| **BLE channel** | 🔄 **Stage 2 verify** — `BLE_RADIO_INIT_ENABLE=1`, nRF Connect |
| **Analog validation** (function generator on CH3/CH4) | ✅ **Done** (2026-07-04) |
| **Production mode** (CH3+CH4 only, no DEBUG_ENABLE) | ✅ **Field-tested** (2026-07-04) |

UART path is **validated**. BLE Stage 2 coded; **radio init enabled** — verify on hardware.

### What to do today (next session)

1. Reflash (radio init now on). Serial: `[BLE] GATT service registered...` or `Init failed, error=...`
2. **nRF Connect** — `NICLA_EEG`, service + TX/RX characteristics.
3. **UART regression** — `test_logic.py`, `test_streaming.py` @ 460800.
4. **Stage 3** — TX aggregation after Stage 2 passes.

---

## Implementation summary

### Done (firmware)

- Multi-task pipeline: EegAcquisition → Packetiser → Gateway → UartChannel
- Full command set: iES bytes + Nicla `0x10`–`0x32`, `CMD_DEMO` (0x40), `CMD_RESET` (0x41)
- `PersistentConfig` via Mbed `FlashIAP` (not hardware EEPROM)
- `OutputMode` raw vs iES µV; channel mask; downsampling; lazy ODR at stream-start
- Streaming command gate: **only** `'s'`, `'.'`, `0x74` during active stream
- `CMD_GET_VERSION` → v0.2.0
- `DEBUG_ENABLE` no longer auto-starts ADS1299 in `setup()` (prevents CDC flood)
- `SERIAL_BAUD_RATE = 460800` (4×115200 — command + streaming headroom on Windows CDC)
- **Analog input validation** — function generator on CH3/CH4; production mode (`DEBUG_ENABLE` off) confirmed (2026-07-04)

### Not done

- `BleChannelTask` TX aggregation (Stage 3) and RX command path (Stage 4)
- BLE radio init verification (`BLE_RADIO_INIT_ENABLE=1` → nRF Connect)
- Impedance LOFF register writes (runtime flag only)
- `MBED_ENABLED` mutex in RuntimeState (enabled in `runtime_state.h`; verify under BLE load later)

---

## Test status (2026-07-04)

| Script | Purpose | Result @ **460800** |
|---|---|---|
| `test_logic.py` | Per-command correctness (11 tests) | ✅ **11/11 pass** |
| `test_timing.py` | RTT baselines | ✅ **Complete** — 0 timeouts; SPI cmds ~20–22 ms mean |
| `test_sequences.py` | Order, gate, persistence (8 tests) | ✅ **8/8 pass** |
| `test_streaming.py` | Fps, format, continuity, TIME_SYNC, stress DS×4→×2→×1 | ✅ **All pass** (basic + stress ramp) |

**Baud note:** Firmware (`config.h`) and all four `test_*.py` scripts are set to **460800** as of 2026-07-04. Reflash required after any baud change. See `technical_notes.md` NOTE-009.

### Historical issues (resolved)

**`stop_unblocks_config` @ 921600** — gain readback ×17 (`0x11` corruption). Stable @ 115200 and @ 460800.

**`test_streaming.py` continuity @ 115200** — false fail from USB CDC batching; fixed with frame-counter checks (NOTE-010). Stress test failed DS×2 @ 115200 (bandwidth); passes @ 460800.

---

## Key files

| Area | Path |
|---|---|
| Firmware sketch | `firmware/ADS1299NiclaFW/ADS1299NiclaFW.ino` |
| Commands / gate | `firmware/ADS1299NiclaFW/cmd_handler.cpp` |
| Config / baud | `firmware/ADS1299NiclaFW/config.h` |
| BLE stub | `firmware/ADS1299NiclaFW/ble_channel.cpp` |
| Dev log & plan | `docs/dev_log.md` |
| Architecture | `docs/firmware_architecture.md` |
| BLE design | `docs/ble_channel_design.md` |
| Test notes | `docs/technical_notes.md` (NOTE-009) |
| Logic tests | `firmware/uartLogger/test_logic.py` |
| Sequence tests | `firmware/uartLogger/test_sequences.py` |
| Streaming tests | `firmware/uartLogger/test_streaming.py` |
| GUI tool | `firmware/uartLogger/FirmwareTestApp.py` |

---

## BLE implementation roadmap (unchanged design, updated status)

Reference: **`docs/ble_channel_design.md`**

| Stage | Work | Status |
|---|---|---|
| **0** | UART validation (four-script suite) | ✅ **Complete** @ 460800 |
| **1** | `FrameDest` (UART \| BLE unicast), Gateway routing, `BLE_TX_QUEUE_SIZE` → 128 | ✅ **Done** (2026-07-04) |
| **2** | BLE stack init, GATT service, advertising | ❌ Not started |
| **3** | TX aggregation loop (notifications) | ❌ Not started |
| **4** | RX command path, `CmdSource::BLE`, semaphore backpressure | ❌ Not started |
| **5** | Connection parameter negotiation | ❌ Not started |
| **6** | Passkey / bonding (Phase 2 security) | ❌ Designed only |

---

## Environment reminders

- **Baud:** **460800** on device and host (`config.h` + all `test_*.py` scripts). Do not use 1 Mbaud (corruption). 921600 was intermittent on this Windows CDC link.
- **COM port:** auto-detect via “USB Serial Device” on Windows; or `--port COM12`.
- **Debug build:** `DEBUG_ENABLE` in `config.h` routes channels to internal test signal — good for streaming tests without hardware.
- **Production test:** comment out `DEBUG_ENABLE`, use `ANALOG_TEST_ENABLE` or neither (CH3+CH4 only).
- **Settle time:** 1 s between commands in all strategic tests (realistic pacing).

---

## Session changelog

### 2026-07-04 (BLE Stage 2 — task bring-up)

- **Stage 2 coded:** Mbed BLE GATT/advertising in `ble_channel.cpp`; init only from `BleChannelTask::run()`.
- **`BLE_RADIO_INIT_ENABLE=0`:** task bring-up verified on hardware; `ble.init()` from setup/loop hangs device.
- **Next:** set `BLE_RADIO_INIT_ENABLE=1`, verify radio + nRF Connect.

### 2026-07-04 (BLE Stage 2 — initial implementation)

### 2026-07-04 (continued)

- **BLE Stage 1 implemented:** `FrameDest` on `WireFrame` (UART/BLE unicast), Packetiser sets `dest`, Gateway typed queues + `routeWireFrame()`, `BLE_TX_QUEUE_SIZE=128`, `CMD_ENABLE_UART`/`BLE` wired with mutual exclusivity.

### 2026-07-04

- **All four UART test scripts pass @ 460800 baud** (after reflash):
  - `test_logic.py` — 11/11
  - `test_sequences.py` — 8/8
  - `test_timing.py` — 0 timeouts; mean RTT ~12 ms (config), ~21 ms (SPI)
  - `test_streaming.py` — basic DS×4 + stress ramp DS×4→×2→×1 (250/500/1000 wire fps)
- Raised `SERIAL_BAUD_RATE` and host scripts from **115200 → 460800** (4×) for streaming headroom; DS×2 failed @ 115200, passes @ 460800.
- Streaming tester: frame-counter continuity, TIME_SYNC timestamp reconstruction, stress downsampling ramp (see NOTE-010).
- **UART Phase 4 validation complete.** Next: **BLE Stage 1**.
- **Phase 3 analog validation complete** — function generator on CH3/CH4; production mode field-tested (`DEBUG_ENABLE` off).

### 2026-07-03

- Removed deprecated docs: `streaming_implementation_proposal.md`, `app_protocol_implementation_notes.md`, `config_parameters_analysis.md`.
- **`test_logic.py`:** ✅ 11/11 pass @ **115200 baud** (after reflash).
- **`test_logic.py`:** ❌ failed @ **921600** — `set_gain` restore loop: no RESPONSE for `cmd=0x11` within 5 s (CH3×2 and CH4×4 had already passed).
- **`test_sequences.py`:** ✅ **8/8 pass** @ **115200** (includes `stop_unblocks_config`, streaming gate, DEMO, double-START).
- Lowered `SERIAL_BAUD_RATE` and host test scripts to **115200** for Windows CDC stability.
- **Next:** `test_streaming.py`.

### 2026-06-19

- Split monolithic autotest into `test_logic`, `test_timing`, `test_sequences`, `test_streaming`.
- Confirmed **921600 baud** required (1 Mbaud corrupts SET_GAIN / readback).
- Documented command RTT in `firmware_architecture.md` §6.6.
- Narrowed streaming gate to STOP + HEARTBEAT + TIME_SYNC only.
- Added `stop_and_drain()` for post-stream serial cleanup.
- Sequence tests mostly pass; streaming continuity + intermittent gain readback remain open.

### 2026-06-17 (prior)

- Phase 4 UART commands completed; original `firmware_autotest.py` 20/20 at least once.
- See `session_handover_20260617.md` for earlier BLE phase task list (A–F).

---

## How to continue in a new chat

Paste or ask:

> Load `docs/SessionHandOver.md` and tell me where we are with the implementation and what to do today.

The assistant should read this file plus `dev_log.md` / `ble_channel_design.md` as needed.
