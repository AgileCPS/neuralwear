# Debug Log Tags — ADS1299NiclaFW

All `Serial.print` messages guarded by `#ifdef DEBUG_ENABLE` that carry a `[TAG]` prefix.
Sorted by source file.

---

## Tag Convention

| Rule | Detail |
|------|--------|
| **All caps** | Tag identifiers are always uppercase: `[UART]`, `[EEG]`, `[SYS]` |
| **Short** | Maximum ~10 characters |
| **Space-separated subtype** | Optional subtype follows the base tag with a space: `[UART TX]`, `[UART RX]` |
| **Event suffix** | Fault/stall conditions append `FAULT` or `STALL` as the subtype: `[UART RX] FAULT` |
| **No punctuation** | No hyphens, underscores, or dots inside the brackets |
| **Matches class name** | Tag should be a recognisable abbreviation of the class or subsystem it belongs to |

**Examples:**
```
[SYS] HeapFree: ...                   ← system-wide, main loop
[EEG] LoopRate: ...                   ← EegAcquisitionTask
[UART TX] Packet rate: ...            ← UartChannelTask → TX path
[UART RX] FAULT: _rxIndex overflow    ← UartChannelTask → RX path, error event
```

---

## `ADS1299NiclaFW.ino` — main loop

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[SYS]` | `[SYS] HeapFree: <N> B (claimed: <N>/<total> B)` | 5 s | `loop()` |

---

## `eeg.cpp` — EegAcquisitionTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[EEG]` | `[EEG] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 5 s | `run()` |

---

## `packetiser.cpp` — PacketiserTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|---------|
| `[PKT]` | `[PKT] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 1 s | `run()` |
| `[PKT]` | `[PKT] Statistics:` followed by per-queue stats (multi-line) | 1 s | `run()` |

**Multi-line block format:**
```
[PKT] LoopRate: <N> iter/s | MaxLoopMs: <N>
[PKT] Statistics:
  Frames out: <N> total | EEG=<N> Resp=<N> ML=<N> TimeSync=<N>
  EEG queue: <size>/<cap> | Drops: <N>
  Response queue: <size>/<cap> | Drops: <N>
  ML queue: <size>/<cap> | Drops: <N> (future)
```

---

## `gateway.cpp` — GatewayTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[GATEWAY]` | `[GATEWAY] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 5 s | `run()` |

---

## `uart_channel.cpp` — UartChannelTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[UART]` | `[UART] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 5 s | `run()` |
| `[UART TX]` | `[UART TX] Packet rate: <N> pkt/s \| Queue: <size>/<cap> items \| Drops: <N>` | 1 s | `processTx()` |
| `[UART RX]` | `[UART RX] Byte rate: <N> B/s \| Frame rate: <N> frames/s \| Errors: <N> \| state=<N>` | 1 s | `processRx()` |
| `[UART RX] FAULT` | `[UART RX] FAULT: _rxIndex overflow (<N> >= <max>) in state=<N> — resetting state machine` | on event | `processRx()` |
| `[UART RX] STALL` | `[UART RX] STALL: state=<N> stuck for <N> ms, rxIndex=<N> — resetting state machine` | on event (>2 s stall) | `processRx()` |
| `[UART RX] FAULT` | `[UART RX] FAULT: _cmdOutputQueue is null — call setCmdOutputQueue() before start()` | on event | `parseFrame()` |

---

## `cmd_handler.cpp` — CommandHandlerTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[CMDHDLR]` | `[CMDHDLR] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 5 s | `run()` |

---

## Summary

| Tag | Type | Interval |
|-----|------|----------|
| `[SYS]` | System heap | 5 s |
| `[EEG]` | Loop metrics | 5 s |
| `[PKT]` | Loop metrics | 1 s |
| `[PKT]` | Frame-type counters + queue stats | 1 s |
| `[GATEWAY]` | Loop metrics | 5 s |
| `[UART]` | Loop metrics | 5 s |
| `[UART TX]` | TX throughput | 1 s |
| `[UART RX]` | RX throughput | 1 s |
| `[UART RX] FAULT` | Error event | on event |
| `[UART RX] STALL` | Error event | on event |
| `[CMDHDLR]` | Loop metrics | 5 s |
