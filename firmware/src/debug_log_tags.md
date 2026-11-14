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
| **Event suffix** | Fault/stall conditions append `FAULT` or `STALL` as the subtype: `[UART RX] STALL` |
| **No punctuation** | No hyphens, underscores, or dots inside the brackets |
| **Matches class name** | Tag should be a recognisable abbreviation of the class or subsystem it belongs to |

**Examples:**
```
[SYS] HeapFree: ...                   ← system-wide, main loop
[EEG] LoopRate: ...                   ← EegAcquisitionTask
[UART TX] StatBegin: ...              ← UartChannelTask → TX path
[UART RX] STALL: stuck in IN_FRAME    ← UartChannelTask → RX path, stall event
```

---

## `src.ino` — main loop

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[SYS]` | `[SYS] HeapFree: <N> B (claimed: <N>/<total> B)` | 5 s | `loop()` |

---

## `eeg.cpp` — EegAcquisitionTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[EEG]` | `[EEG] LoopRate: <N> iter/s \| MaxLoopMs: <N>` followed by `[EEG] StatBegin:` / queue stats / `[EEG] StatEnd.` | 1 s | `run()` |

**Multi-line block format:**
```
[EEG] LoopRate: <N> iter/s | MaxLoopMs: <N>
[EEG] StatBegin:
  EEG→PacketiserTask queue: <size>/<cap> | Drops: <N>
[EEG] StatEnd.
```

---

## `packetiser.cpp` — PacketiserTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|---------|
| `[PKT]` | `[PKT] LoopRate: <N> iter/s \| MaxLoopMs: <N>` followed by `[PKT] StatBegin:` / frame stats / `[PKT] StatEnd.` | 1 s | `run()` |

**Multi-line block format:**
```
[PKT] LoopRate: <N> iter/s | MaxLoopMs: <N>
[PKT] StatBegin:
  Frames out: <N> total | EEG=<N> Resp=<N> ML=<N> TimeSync=<N>
  EEG queue: <size>/<cap> | Drops: <N>
  Response queue: <size>/<cap> | Drops: <N>
  ML queue: <size>/<cap> | Drops: <N>
[PKT] StatEnd.
```

---

## `gateway.cpp` — GatewayTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[GATEWAY]` | `[GATEWAY] LoopRate: <N> iter/s \| MaxLoopMs: <N>` followed by `[GATEWAY] StatBegin:` / queue stats / `[GATEWAY] StatEnd.` | 1 s | `run()` |

**Multi-line block format:**
```
[GATEWAY] LoopRate: <N> iter/s | MaxLoopMs: <N>
[GATEWAY] StatBegin:
  Input queue: <size>/<cap> | Drops: <N>
  UART TX queue: <size>/<cap> | Drops: <N>
  BLE TX queue:  <size>/<cap> | Drops: <N>
[GATEWAY] StatEnd.
```

---

## `uart_channel.cpp` — UartChannelTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[UART]` | `[UART] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 5 s | `run()` |
| `[UART]` | `[UART] Switching baud to <N> ...` | on event (baud change) | `applyPendingBaudChange()` (gated by `DBG_UART`) |
| `[UART TX]` | `[UART TX] StatBegin:` / frame rate + queue stats / `[UART TX] StatEnd.` | 1 s | `processTx()` |
| `[UART RX]` | `[UART RX] StatBegin:` / byte+cmd rates + errors / `[UART RX] StatEnd.` | 1 s | `processRx()` |
| `[UART RX]` | `[UART RX] Unknown cmd byte 0xXX — discarded` | on event | `processRx()` |
| `[UART RX] STALL` | `[UART RX] STALL: stuck in IN_FRAME for <N> ms (cmd=0xXX, got <N>/<N>) — reset` | on event (>2 s stall) | `processRx()` |

**[UART TX] block format:**
```
[UART TX] StatBegin:
  Frame rate: <N.N> frames/s
  Queue: <size>/<cap> | Drops: <N>
[UART TX] StatEnd.
```

**[UART RX] block format:**
```
[UART RX] StatBegin:
  Byte rate: <N.N> B/s | Cmd rate: <N.N> cmds/s
  Errors: <N> | state=<N>
[UART RX] StatEnd.
```

---

## `cmd_handler.cpp` — CommandHandlerTask

| Tag | Full message | Interval | Source |
|-----|-------------|----------|--------|
| `[CMDHDLR]` | `[CMDHDLR] LoopRate: <N> iter/s \| MaxLoopMs: <N>` | 5 s | `run()` |
| `[CMD]` | `[CMD] >> 0xXX plen=<N> src=UART\|BLE` | per command (gated by `DBG_CMD`) | `run()` |
| `[CMD]` | `[CMD] << 0xXX status=0xXX plen=<N>` | per command (gated by `DBG_CMD`) | `run()` |
| `[CRC_DBG]` | `[CRC_DBG] payload=<hex> rx_crc=<XX> fw_crc=<XX> OK\|BAD` | per TIME_SYNC (gated by `DBG_CMD`) | `cmdTimeSync()` |

---

## Summary

| Tag | Type | Interval |
|-----|------|----------|
| `[SYS]` | System heap | 5 s |
| `[EEG]` | Loop metrics + queue stats | 1 s |
| `[PKT]` | Loop metrics + frame counters + queue stats | 1 s |
| `[GATEWAY]` | Loop metrics + queue stats | 1 s |
| `[UART]` | Loop metrics | 5 s |
| `[UART]` | Baud switch notification | on event |
| `[UART TX]` | TX throughput | 1 s |
| `[UART RX]` | RX throughput | 1 s |
| `[UART RX]` | Unknown command byte | on event |
| `[UART RX] STALL` | Stall event | on event |
| `[CMDHDLR]` | Loop metrics | 5 s |
| `[CMD]` | Per-command dispatch trace | per command |
| `[CRC_DBG]` | TIME_SYNC CRC debug | per TIME_SYNC |
