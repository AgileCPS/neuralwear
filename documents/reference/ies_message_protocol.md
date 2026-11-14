# iES v0.3 — Message Protocol Reference

**Purpose**: Complete byte-level specification of every message exchanged between  
the iES device (MSP432) and a host (PC / mobile) over Bluetooth Classic SPP.  
This document is intended to allow full reuse of the command/data design.

---

## Table of Contents

1. [Transport Layer Parameters](#1-transport-layer-parameters)
2. [Byte Order and Primitive Encodings](#2-byte-order-and-primitive-encodings)
3. [CRC-8 Checksum Algorithm](#3-crc-8-checksum-algorithm)
4. [Host → Device: Command Reference](#4-host--device-command-reference)
   - 4.1 [Single-byte commands](#41-single-byte-commands)
   - 4.2 [CMD: Start Streaming (`b`)](#42-cmd-start-streaming-b)
   - 4.3 [CMD: Stop Streaming (`s`)](#43-cmd-stop-streaming-s)
   - 4.4 [CMD: Time Synchronisation (`t`)](#44-cmd-time-synchronisation-t)
   - 4.5 [CMD: Heartbeat (`.`)](#45-cmd-heartbeat-)
   - 4.6 [CMD: Set Downsampling Factor (`d`)](#46-cmd-set-downsampling-factor-d)
   - 4.7 [CMD: Enable Impedance Check (`Z`)](#47-cmd-enable-impedance-check-z)
   - 4.8 [CMD: Disable Impedance Check (`z`)](#48-cmd-disable-impedance-check-z)
   - 4.9 [CMD: Print / Mode Select (`p`)](#49-cmd-print--mode-select-p)
   - 4.10 [CMD: OpenBCI Soft Reset (`v`)](#410-cmd-openbci-soft-reset-v)
5. [Device → Host: Data Packet Reference](#5-device--host-data-packet-reference)
   - 5.1 [iES Native Packet (default)](#51-ies-native-packet-default)
   - 5.2 [OpenBCI Compatible Packet](#52-openbci-compatible-packet)
   - 5.3 [OpenBCI Startup Banner (text)](#53-openbci-startup-banner-text)
6. [Payload Field Details by Sample Type](#6-payload-field-details-by-sample-type)
   - 6.1 [EEG (type 0)](#61-eeg-type-0)
   - 6.2 [IMPEDANCE (type 1)](#62-impedance-type-1)
   - 6.3 [NECK\_IMU (type 2)](#63-neck_imu-type-2)
   - 6.4 [EAR\_IMU (type 3)](#64-ear_imu-type-3)
   - 6.5 [EDA (type 4)](#65-eda-type-4)
   - 6.6 [BATT\_INFO (type 5)](#66-batt_info-type-5)
7. [Frame Counter Behaviour](#7-frame-counter-behaviour)
8. [Complete Byte-Map Tables](#8-complete-byte-map-tables)
9. [Reuse Checklist and Extension Guidance](#9-reuse-checklist-and-extension-guidance)
10. [Nicla Voice — Packet Type Nibble Registry](#10-nicla-voice--packet-type-nibble-registry)
11. [Nicla Voice — Host Receiver Requirements](#11-nicla-voice--host-receiver-requirements)
12. [Nicla Voice — Host Protocol Mode (Modern vs Legacy/OpenVIBE)](#12-nicla-voice--host-protocol-mode-modern-vs-legacyopenvibe)

---

## 1. Transport Layer Parameters

| Parameter         | Value                        |
|-------------------|------------------------------|
| Module            | RN-42 (Bluetooth Classic)    |
| Profile           | SPP (Serial Port Profile)    |
| UART baud rate    | **115200** bps               |
| Data bits         | 8                            |
| Parity            | None                         |
| Stop bits         | 1                            |
| Flow control      | None                         |
| Write mode        | `UART_DATA_BINARY`           |
| Read mode         | `UART_DATA_BINARY`           |
| Read return mode  | `UART_RETURN_FULL`           |
| Echo              | Off                          |

All data is raw binary. There is no framing or escape-byte mechanism at the
transport level — the application-level start byte `0xA0` and end byte `0xC0`
serve as the only delimiters.

---

## 2. Byte Order and Primitive Encodings

All multi-byte integers in both directions are **big-endian (MSB first)**.

### Integer Serialisation

```
uint16 (2 bytes):  [MSB] [LSB]
uint24 (3 bytes):  [bits 23:16] [bits 15:8] [bits 7:0]
uint32 (4 bytes):  [bits 31:24] [bits 23:16] [bits 15:8] [bits 7:0]
uint64 (8 bytes):  [bits 63:56] ... [bits 7:0]
```

The `uint24_to_buffer` function is used for all channel data in packets:

```c
void uint24_to_buffer(uint32_t data, uint8_t* buffer) {
    buffer[0] = (uint8_t)(data >> 16);   // MSB
    buffer[1] = (uint8_t)(data >> 8);
    buffer[2] = (uint8_t)(data);         // LSB
}
```

Channel data values are signed 32-bit integers in µV (or sensor-specific
units), but only 24 bits are transmitted. The value is cast directly —
callers are responsible for ensuring values fit in 24 bits (i.e., within
the range −8,388,608 to +8,388,607).

### Float Serialisation (4 bytes)

Used internally (SD card metadata), **not** in BT-SPP packets:

```
Bytes 0–1: integer part  (uint16, big-endian)
Bytes 2–3: fraction part (uint16, big-endian, 0–99, representing .00–.99)

Examples:
  3.14  →  [0x00, 0x03, 0x00, 0x0E]   (3 decimal, 14 fractional)
  42.00 →  [0x00, 0x2A, 0x00, 0x00]
```

---

## 3. CRC-8 Checksum Algorithm

Only used in the **Time Synchronisation** command payload.

| Property         | Value                                    |
|------------------|------------------------------------------|
| Algorithm        | CRC-8 / SHT75 (Sensirion humidity sensor)|
| Polynomial       | 0x31 (reflected lookup table)            |
| Initial value    | 0x00 (`CRC_START_8`)                     |
| Input reflection | No                                       |
| Output XOR       | 0x00                                     |
| Implementation   | 256-entry lookup table (`sht75_crc_table`)|

```c
// C reference implementation (from crc8.cpp)
uint8_t crc_8(const unsigned char *input_str, size_t num_bytes) {
    size_t a;
    uint8_t crc = 0x00;           // CRC_START_8
    const unsigned char *ptr = input_str;
    for (a = 0; a < num_bytes; a++) {
        crc = sht75_crc_table[(*ptr++) ^ crc];
    }
    return crc;
}
```

The 256-byte lookup table is the standard SHT75 CRC table. A Python
equivalent:

```python
def crc8_sht75(data: bytes) -> int:
    """CRC-8 / SHT75 as used by iES v0.3 time-sync command."""
    table = [
        0,49,98,83,196,245,166,151,185,136,219,234,125,76,31,46,67,114,33,16,
        135,182,229,212,250,203,152,169,62,15,92,109,134,183,228,213,66,115,32,17,
        63,14,93,108,251,202,153,168,197,244,167,150,1,48,99,82,124,77,30,47,184,
        137,218,235,61,12,95,110,249,200,155,170,132,181,230,215,64,113,34,19,126,
        79,28,45,186,139,216,233,199,246,165,148,3,50,97,80,187,138,217,232,127,78,
        29,44,2,51,96,81,198,247,164,149,248,201,154,171,60,13,94,111,65,112,35,18,
        133,180,231,214,122,75,24,41,190,143,220,237,195,242,161,144,7,54,101,84,57,
        8,91,106,253,204,159,174,128,177,226,211,68,117,38,23,252,205,158,175,56,9,
        90,107,69,116,39,22,129,176,227,210,191,142,221,236,123,74,25,40,6,55,100,
        85,194,243,160,145,71,118,37,20,131,178,225,208,254,207,156,173,58,11,88,
        105,4,53,102,87,192,241,162,147,189,140,223,238,121,72,27,42,193,240,163,
        146,5,52,103,86,120,73,26,43,188,141,222,239,130,179,224,209,70,119,36,21,
        59,10,89,104,255,206,157,172
    ]
    crc = 0x00
    for b in data:
        crc = table[b ^ crc]
    return crc
```

---

## 4. Host → Device: Command Reference

The device's receive task (`btspp_recv_task_fxn`) reads commands in a
**blocking loop**: it reads exactly **1 byte** first, then reads any
additional payload bytes specific to that command. Commands are not
terminated with a newline or any other sentinel byte.

### 4.1 Single-Byte Commands

Summary of all command bytes:

| Hex    | ASCII | Name                       | Payload following | Description                            |
|--------|-------|----------------------------|-------------------|----------------------------------------|
| `0x62` | `b`   | `IES_STREAM_START`         | None              | Start biosignal streaming              |
| `0x73` | `s`   | `IES_STREAM_STOP`          | None              | Stop biosignal streaming               |
| `0x74` | `t`   | `IES_TIME_SYNC`            | **5 bytes**       | Synchronise real-time clock            |
| `0x2E` | `.`   | `IES_BTSPP_HEART_BEAT`     | None              | Keep-alive / ping (no response)        |
| `0x64` | `d`   | `IES_BTSPP_DOWNSAMPLING`   | **1 byte**        | Set EEG downsampling factor            |
| `0x5A` | `Z`   | `IES_IMPEDANCE_CHECK_ON`   | None              | Enable lead-off (impedance) detection  |
| `0x7A` | `z`   | `IES_IMPEDANCE_CHECK_OFF`  | None              | Disable lead-off detection             |
| `0x70` | `p`   | `IES_BTSPP_UART_PRINT_SEL` | **1 byte**        | Select print/mode target               |
| `0x76` | `v`   | `OPEN_BCI_SOFT_RESET`      | None              | Request OpenBCI startup banner string  |

---

### 4.2 CMD: Start Streaming (`b`)

```
┌──────────┐
│  0x62    │   1 byte total
└──────────┘
```

- Triggers `START_STREAMING` event.
- Device begins ADS1299 sampling, starts the 50 Hz IMU/EDA timer, and opens
  SD card files if `save_to_sd_card = true`.
- LED: RED on, GREEN off.
- No acknowledgement byte is sent back to the host.

---

### 4.3 CMD: Stop Streaming (`s`)

```
┌──────────┐
│  0x73    │   1 byte total
└──────────┘
```

- Triggers `STOP_STREAMING` event.
- Device stops ADS1299, stops timer, closes SD card files.
- LED: RED off, GREEN on.
- No acknowledgement byte is sent back to the host.

---

### 4.4 CMD: Time Synchronisation (`t`)

Sets the real-time clock on the device. Total transfer: **6 bytes**.

```
Byte  Offset  Field             Size   Notes
──────────────────────────────────────────────────────────────────
  0     0     Command byte      1 B    0x74 ('t')
  1     1     Epoch seconds     4 B    Seconds since Unix epoch
  2     2     (continued)              Big-endian (MSB first)
  3     3     (continued)
  4     4     (continued)       
  5     5     CRC-8 checksum    1 B    CRC-8/SHT75 of bytes [1..4]
──────────────────────────────────────────────────────────────────
```

**Important note on epoch**: The source comment says  
`"32 bits - seconds_from_epoch (1900 Jan 1 00:00:00)"` but
`buffer_to_uint32` is passed directly to `clock_settime(CLOCK_REALTIME)`.
TI-RTOS `CLOCK_REALTIME` counts from **1900**. Standard Unix epoch is 1970.
Use the appropriate epoch for your target OS — if porting to a system that
uses the Unix epoch (1970), adjust accordingly.

**Checksum validation** (device-side):
```c
// timestamp_buffer[0..3] = epoch bytes (4 bytes)
// timestamp_buffer[4]    = received CRC
if (timestamp_buffer[4] != crc_8(timestamp_buffer, 4)) {
    // ERROR: silently discard, log error
}
```

**Host construction example (Python)**:
```python
import struct, time

def build_time_sync():
    epoch_sec = int(time.time())           # Unix epoch (adjust if needed)
    payload = struct.pack('>I', epoch_sec) # 4 bytes, big-endian
    crc = crc8_sht75(payload)
    return b't' + payload + bytes([crc])   # total 6 bytes
```

**Device response**: None. Clock is set silently.

> **Nicla Voice note:** ADS1299NiclaFW implements this command with a
> mode-dependent payload length — see [§12](#12-nicla-voice--host-protocol-mode-modern-vs-legacyopenvibe).
> In `HostProtocolMode::MODERN` it expects the full 6 bytes above (CRC
> validated). In `LEGACY_IES` it expects only 5 bytes (`'t'` + 4 epoch
> bytes, no CRC) — because the original OpenVIBE `CDriveriES` driver's
> `sendCommand()` never actually puts the CRC byte on the wire (see §12.1).

---

### 4.5 CMD: Heartbeat (`.`)

```
┌──────────┐
│  0x2E    │   1 byte total
└──────────┘
```

- Logged internally: `"BTSPP_Recv_thread: BTSPP HEART BEAT."`
- No action taken, no reply sent.
- Useful for detecting connection liveness without triggering state changes.

---

### 4.6 CMD: Set Downsampling Factor (`d`)

Total transfer: **2 bytes**.

```
Byte  Offset  Field               Size   Notes
──────────────────────────────────────────────────────────────────
  0     0     Command byte        1 B    0x64 ('d')
  1     1     Downsampling N      1 B    uint8, 1–255
──────────────────────────────────────────────────────────────────
```

The device sets `ies2btspp_down_sampling = N`.
Every N-th EEG sample is forwarded to the BT-SPP queue.
(1 = every sample; 4 = default; 10 = 1/10 of samples.)

**Side effect on SD logging**:

| N value | SD card logging |
|---------|-----------------|
| 1, 2, 3 (N < 4) | **Disabled** |
| 4 and above     | **Enabled**  |

**Device response**: None.

**Host example**:
```python
def set_downsampling(n: int) -> bytes:
    return bytes([0x64, n & 0xFF])
```

---

### 4.7 CMD: Enable Impedance Check (`Z`)

```
┌──────────┐
│  0x5A    │   1 byte total
└──────────┘
```

- Calls `streamSafeLeadOffSetForChannel(3, 0, 1)` and
  `streamSafeLeadOffSetForChannel(4, 0, 1)`.
- Enables lead-off (AC impedance) detection signal on the **N-input only**
  of channels 3 and 4.
- The `0` argument for P-input means P-side detection remains off;
  `1` for N-input means N-side detection is turned on.
- If streaming, the device momentarily stops and restarts the ADS1299.
- **Device response**: None.

---

### 4.8 CMD: Disable Impedance Check (`z`)

```
┌──────────┐
│  0x7A    │   1 byte total
└──────────┘
```

- Calls `streamSafeLeadOffSetForChannel(3, 0, 0)` and
  `streamSafeLeadOffSetForChannel(4, 0, 0)`.
- Disables lead-off detection on channels 3 and 4.
- **Device response**: None.

---

### 4.9 CMD: Print / Mode Select (`p`)

Total transfer: **2 bytes**. This is the most complex command — the second
byte selects a sub-function.

```
Byte  Offset  Field               Size   Notes
──────────────────────────────────────────────────────────────────
  0     0     Command byte        1 B    0x70 ('p')
  1     1     Sub-command byte    1 B    One of the values below
──────────────────────────────────────────────────────────────────
```

**Sub-command table**:

| Byte | ASCII | Effect on device                                           | BT-SPP TX  | SD card    |
|------|-------|------------------------------------------------------------|------------|------------|
| `0x65` | `e` | Enable EEG UART debug print                               | **Off**    | Unchanged  |
| `0x61` | `a` | Enable neck accelerometer UART debug print (X, Y, Z)     | **Off**    | Unchanged  |
| `0x62` | `b` | Enable ear accelerometer UART debug print (X, Y, Z)      | **Off**    | Unchanged  |
| `0x67` | `g` | Enable neck gyroscope UART debug print (X, Y, Z)         | **Off**    | Unchanged  |
| `0x68` | `h` | Enable ear gyroscope UART debug print (X, Y, Z)          | **Off**    | Unchanged  |
| `0x64` | `d` | Enable EDA UART debug print                               | **Off**    | Unchanged  |
| `0x6F` | `o` | **Switch to OpenBCI compatible mode**                     | On (OpenBCI)| **Off**   |
| `0x69` | `i` | **Switch to iES native mode**                             | On (iES)   | **On**     |

Notes on mode-switch sub-commands:
- `o` sets `openbci_compatible = true`, `save_to_sd_card = false`, `send_to_btspp = true`, all UART prints off.
- `i` sets `openbci_compatible = false`, `save_to_sd_card = true`, `send_to_btspp = true`, all UART prints off.
- All 6 UART print sub-commands (`e`, `a`, `b`, `g`, `h`, `d`) are mutually exclusive — selecting one disables all others.

**Device response**: None.

**Host examples**:
```python
CMD_STREAM_START           = b'b'
CMD_STREAM_STOP            = b's'
CMD_HEARTBEAT              = b'.'
CMD_IMPEDANCE_ON           = b'Z'
CMD_IMPEDANCE_OFF          = b'z'
CMD_OPENBCI_SOFT_RESET     = b'v'

def cmd_set_downsampling(n):        return bytes([0x64, n])
def cmd_time_sync(epoch_sec):
    p = struct.pack('>I', epoch_sec)
    return b't' + p + bytes([crc8_sht75(p)])
def cmd_mode_openbci():             return b'po'
def cmd_mode_ies():                 return b'pi'
def cmd_print_eeg():                return b'pe'
def cmd_print_neck_accel():         return b'pa'
def cmd_print_ear_accel():          return b'pb'
def cmd_print_neck_gyro():          return b'pg'
def cmd_print_ear_gyro():           return b'ph'
def cmd_print_eda():                return b'pd'
```

---

### 4.10 CMD: OpenBCI Soft Reset (`v`)

```
┌──────────┐
│  0x76    │   1 byte total
└──────────┘
```

**Device response**: Sends the OpenBCI startup banner string immediately
(see Section 5.3). This is the only command that generates an immediate
response.

---

## 5. Device → Host: Data Packet Reference

All data packets are sent by the `btspp_send_task_fxn`. Both formats share:
- Start byte: `0xA0`
- End byte: `0xC0`
- 24-bit big-endian channel data values

There is **no reply/acknowledgement** mechanism for data packets. Data flow
is unidirectional once streaming starts.

---

### 5.1 iES Native Packet (default)

Variable length. Used when `openbci_compatible = false`.

```
Byte  Offset  Field                  Size   Notes
──────────────────────────────────────────────────────────────────────
  0     0     Start byte             1 B    Always 0xA0
  1     1     Frame counter          1 B    uint8, 0–255, wraps to 0
  2     2     Type + Channel byte    1 B    [7:4] = sample_type (4 bits)
                                            [3:0] = num_of_channels (4 bits)
  3     3     Channel 0 [23:16]      1 B    MSB of 24-bit signed int
  4     4     Channel 0 [15:8]       1 B
  5     5     Channel 0 [7:0]        1 B    LSB of 24-bit signed int
  6     6     Channel 1 [23:16]      1 B    (present if num_of_channels ≥ 2)
  7     7     Channel 1 [15:8]       1 B
  8     8     Channel 1 [7:0]        1 B
  ...         (3 bytes per additional channel)
  N     3+n*3 End byte               1 B    Always 0xC0
──────────────────────────────────────────────────────────────────────
Total packet length = 3 + (num_of_channels × 3) + 1
                    = 4 + num_of_channels × 3  bytes
```

**Packet sizes by sensor type** (from the `IES_SAMPLE_TYPE` enum):

| sample_type | Name       | num_of_channels | Total packet size |
|-------------|------------|-----------------|-------------------|
| 0           | EEG        | 2               | 10 bytes          |
| 1           | IMPEDANCE  | 2               | 10 bytes          |
| 2           | NECK_IMU   | 6               | 22 bytes          |
| 3           | EAR_IMU    | 6               | 22 bytes          |
| 4           | EDA        | 2               | 10 bytes          |
| 5           | BATT_INFO  | 1               | 7 bytes           |

**Byte 2 decoding**:
```c
// From btspp_send_task_fxn:
gp_buf[2] = (sample_rec.sample_type << 4) | (sample_rec.num_of_channels & 0x0F);

// Decoding on host:
sample_type     = (byte2 >> 4) & 0x0F;
num_of_channels = byte2 & 0x0F;
```

**24-bit signed channel data decoding** (Python):
```python
def decode_int24(msb, mid, lsb):
    val = (msb << 16) | (mid << 8) | lsb
    if val & 0x800000:          # sign-extend from bit 23
        val -= 0x1000000
    return val
```

**Full iES native packet decoder** (Python):
```python
def decode_ies_packet(data: bytes):
    assert data[0]  == 0xA0, "Bad start byte"
    assert data[-1] == 0xC0, "Bad end byte"

    frame_counter   = data[1]
    sample_type     = (data[2] >> 4) & 0x0F
    num_channels    = data[2] & 0x0F

    channels = []
    for i in range(num_channels):
        offset = 3 + i * 3
        val = decode_int24(data[offset], data[offset+1], data[offset+2])
        channels.append(val)

    return frame_counter, sample_type, channels
```

---

### 5.2 OpenBCI Compatible Packet

Fixed 33-byte packet. Used when `openbci_compatible = true`.

```
Byte  Offset  Field                  Size   Notes
──────────────────────────────────────────────────────────────────────
  0     0     Start byte             1 B    Always 0xA0
  1     1     Sample counter         1 B    uint8, 0–255, wraps to 0
  2     2     Ch 1 data [23:16]      1 B    Raw ADS1299 counts, signed 24-bit
  3     3     Ch 1 data [15:8]       1 B    big-endian, NOT gain-normalised
  4     4     Ch 1 data [7:0]        1 B
  5     5     Ch 2 data [23:16]      1 B    (zeroed if channel not populated)
  6     6     Ch 2 data [15:8]       1 B
  7     7     Ch 2 data [7:0]        1 B
  ...         (channels 3–10 followed in 3-byte groups, all zeroed)
 32    32     Stop byte              1 B    Always 0xC0
──────────────────────────────────────────────────────────────────────
Total: 33 bytes (fixed, matches OpenBCI V3 protocol)
```

Notes:
- The loop in `btspp_send_task_fxn` fills bytes 2 through `2 + num_of_channels*3 - 1`
  from `sample_rec.channel_data`, then places `0xC0` at **fixed offset 32**
  regardless of the number of channels actually populated.
- The buffer is zero-initialised (`memset(gp_buf, 0, sizeof(gp_buf))`) so
  unused channel slots contain `0x00`.
- In this mode, `raw_sample_rec` is used (raw 24-bit ADS1299 ADC counts),
  not µV-scaled values.

**OpenBCI packet decoder** (Python):
```python
def decode_openbci_packet(data: bytes):
    assert len(data) == 33
    assert data[0]  == 0xA0
    assert data[32] == 0xC0
    sample_counter = data[1]
    channels = []
    for i in range(10):              # 10 possible channels
        offset = 2 + i * 3
        val = decode_int24(data[offset], data[offset+1], data[offset+2])
        channels.append(val)
    return sample_counter, channels
```

---

### 5.3 OpenBCI Startup Banner (text)

Sent immediately in response to the `v` command. **Not binary** — this is a
null-terminated ASCII string transmitted as raw bytes.

```
"OpenBCI V3 16 channel\n"
"ADS1299 Device ID: 0x3E\n"
"LIS3DH Device ID: 0x33\n"
"$$$"
```

Total length: 68 bytes (including null terminator in `sizeof()`). The host
should read until it receives `"$$$"` to detect end of banner.

---

## 6. Payload Field Details by Sample Type

### 6.1 EEG (type 0)

| Channel index | Physical channel | Unit | Encoding           |
|---------------|-----------------|------|--------------------|
| 0             | ADS1299 Ch 3    | µV   | int24, big-endian  |
| 1             | ADS1299 Ch 4    | µV   | int24, big-endian  |

Conversion from raw ADS1299 counts to µV (applied before transmission in
iES mode):
```
µV = raw_counts × 0.5364418669 / gain

SCALE_FACTOR_UV = (4.5 × 10⁶) / (2²³ − 1) = 0.5364418669...

gain ∈ {1, 2, 4, 6, 8, 12, 24}  (ADS1299 PGA gain setting)
```

In **OpenBCI mode**, raw ADC counts are transmitted instead of µV values.

**Downsampling**: Only 1 in every `ies2btspp_down_sampling` EEG samples is
forwarded to BT-SPP (default: every 4th sample). All samples are written to
SD card without downsampling.

---

### 6.2 IMPEDANCE (type 1)

Same structure as EEG (2 channels). Sent when lead-off detection is active
on channels 3 and 4.

> Note: In the v0.3 source, impedance data is routed through the same
> `ADS_processChannelData` path as EEG. The `sample_type` field in
> `data_sample_rec` is set by the application; impedance mode must be handled
> at the application level when building a new system.

---

### 6.3 NECK_IMU (type 2)

| Channel index | Physical axis  | Unit      | Encoding           |
|---------------|---------------|----------|--------------------|
| 0             | Accel X        | milli-g  | int24, big-endian  |
| 1             | Accel Y        | milli-g  | int24, big-endian  |
| 2             | Accel Z        | milli-g  | int24, big-endian  |
| 3             | Gyro X         | milli-°/s| int24, big-endian  |
| 4             | Gyro Y         | milli-°/s| int24, big-endian  |
| 5             | Gyro Z         | milli-°/s| int24, big-endian  |

Encoding (from `imu_task_fxn`):
```c
channel_data[n] = int32_t(imu.accelX() * 1000);   // float × 1000 → int
```

All float IMU readings are multiplied by 1000 before being cast to int32_t.
To recover the float value on the host: `physical_value = channel_data[n] / 1000.0`.

Sampling rate: **50 Hz** (20 ms timer period).

---

### 6.4 EAR_IMU (type 3)

Same 6-channel structure as NECK_IMU.

**Channels 3–5 special case** (`use_analog_accel = true` by default):
When analog accelerometer mode is enabled, gyro channels are **replaced**
by ADC readings:

| Channel index | Field  | Source             | Unit | Notes                      |
|---------------|--------|--------------------|------|----------------------------|
| 0             | Accel X| MPU9250 digital    | milli-g | Same as neck IMU       |
| 1             | Accel Y| MPU9250 digital    | milli-g |                         |
| 2             | Accel Z| MPU9250 digital    | milli-g |                         |
| 3             | Analog X | Board_ADC0 (P5.5/A0) | mV | `adcValue_uV / 1000`   |
| 4             | Analog Y | Board_ADC1 (P5.4/A1) | mV | `adcValue_uV / 1000`   |
| 5             | Analog Z | Board_ADC2 (P5.4/A2) | mV | `adcValue_uV / 1000`   |

ADC reference: 2.5 V. Sampling rate: **50 Hz**.

---

### 6.5 EDA (type 4)

| Channel index | Electrode     | Unit         | Encoding           |
|---------------|---------------|--------------|--------------------|
| 0             | Left EDA      | nanosiemens  | int24, big-endian  |
| 1             | Right EDA     | nanosiemens  | int24, big-endian  |

Conversion from ADC voltage to skin conductance:
```
G_nS = ( 10 × V_uV ) / ( 2,500,000 − V_uV ) × 1000

where V_uV is the ADC reading in microvolts (reference = 2.5 V)
```

Safe division: if `V_uV == 0`, it is clamped to 1 to avoid divide-by-zero.

Sampling rate: **50 Hz**.

---

### 6.6 BATT_INFO (type 5)

| Channel index | Field                      | Unit         | Encoding           |
|---------------|----------------------------|--------------|--------------------|
| 0             | Battery state of charge    | 0.01 %       | int24, big-endian  |

```c
channel_data[0] = (int)(batt_fg.getSoC() * 100);
// e.g., 75.5% SoC → channel_data[0] = 7550
```

To recover percentage: `soc_percent = channel_data[0] / 100.0`

Update rate: every 500 IMU timer ticks ≈ **every 10 seconds**.

---

## 7. Frame Counter Behaviour

Both packet formats use a **rolling 8-bit frame counter** (`sample_counter`
in `btspp_send_task_fxn`), shared across **all** data packets regardless of
type.

```c
uint8_t sample_counter = 0;  // initialised to 0 when BTSPP send task starts
// ...
gp_buf[1] = sample_counter++;  // post-increment, wraps at 255 → 0
```

Key properties:
- The counter is **not** reset when streaming stops and restarts.
- The counter counts every **transmitted** packet (after downsampling), not
  every acquired sample.
- A gap in the counter (e.g., jumps from 5 to 10) indicates dropped packets
  in the BT-SPP queue.
- Mixed sensor types (EEG, IMU, EDA, Battery) all increment the same counter.
- On the host, to detect dropped packets: `gap = (current - previous) & 0xFF`.
  If `gap > 1`, packets were dropped.

---

## 8. Complete Byte-Map Tables

### H → D: All Command Byte Maps

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  0x62             │  'b'  │  Start Streaming (1 byte total)                  │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x73             │  's'  │  Stop Streaming (1 byte total)                   │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x74             │  't'  │  Time Sync (6 bytes total)                       │
│  0xXX 0xXX 0xXX 0xXX  │       epoch = 32-bit big-endian uint               │
│  0xXX             │       │  CRC-8/SHT75 of the 4 epoch bytes               │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x2E             │  '.'  │  Heartbeat (1 byte total)                        │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x64             │  'd'  │  Set Downsampling (2 bytes total)                │
│  0xNN             │       │  N = factor (uint8)                              │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x5A             │  'Z'  │  Impedance Check ON (1 byte total)               │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x7A             │  'z'  │  Impedance Check OFF (1 byte total)              │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x70  0x65       │  'pe' │  Print EEG to UART, BT-SPP off                   │
│  0x70  0x61       │  'pa' │  Print neck accel to UART, BT-SPP off            │
│  0x70  0x62       │  'pb' │  Print ear  accel to UART, BT-SPP off            │
│  0x70  0x67       │  'pg' │  Print neck gyro to UART, BT-SPP off             │
│  0x70  0x68       │  'ph' │  Print ear  gyro to UART, BT-SPP off             │
│  0x70  0x64       │  'pd' │  Print EDA to UART, BT-SPP off                   │
│  0x70  0x6F       │  'po' │  Switch to OpenBCI mode (BT-SPP on, SD off)      │
│  0x70  0x69       │  'pi' │  Switch to iES mode (BT-SPP on, SD on)           │
├──────────────────────────────────────────────────────────────────────────────┤
│  0x76             │  'v'  │  OpenBCI Soft Reset → device responds with banner│
└──────────────────────────────────────────────────────────────────────────────┘
```

### D → H: iES Native Packet Layouts

```
EEG / IMPEDANCE (10 bytes):
 A0  [FC]  [0T|02]  [C0H] [C0M] [C0L]  [C1H] [C1M] [C1L]  C0
  ↑    ↑      ↑↑        └── Ch 0 (µV) ──┘  └── Ch 1 (µV) ──┘  ↑
start frame type|ch=2                                           end

NECK_IMU / EAR_IMU (22 bytes):
 A0  [FC]  [2T|06]  [aX 3B]  [aY 3B]  [aZ 3B]  [gX 3B]  [gY 3B]  [gZ 3B]  C0

EDA (10 bytes):
 A0  [FC]  [4T|02]  [LEFT_nS 3B]  [RIGHT_nS 3B]  C0

BATT_INFO (7 bytes):
 A0  [FC]  [5T|01]  [SoC×100 3B]  C0

Legend:
  A0      = 0xA0 start byte
  [FC]    = frame counter (uint8)
  [TT|NN] = high nibble T=sample_type, low nibble N=num_channels
  [xH xM xL] = 3-byte big-endian 24-bit signed integer
  C0      = 0xC0 end byte
  T values: 0=EEG, 1=IMPEDANCE, 2=NECK_IMU, 3=EAR_IMU, 4=EDA, 5=BATT_INFO
```

### D → H: OpenBCI Packet Layout (33 bytes)

```
Offset:  00  01  02 03 04  05 06 07  08 09 0A  0B 0C 0D  ...  1E 1F 20  20
         A0  FC  [Ch1 3B]  [Ch2 3B]  [Ch3 3B]  [Ch4 3B]  ...  [-- 3B]   C0
```

---

## 9. Reuse Checklist and Extension Guidance

### Commands to reuse as-is

All command bytes can be reused verbatim on any UART/BT transport:

- `b` / `s` — universal start/stop for any biosignal streaming system.
- `t` + 5 bytes — clean time-sync with CRC-8 integrity check; reusable for
  any embedded RTC.
- `.` — heartbeat with no side effects.
- `d` + 1 byte — simple downsampling control; the SD-card threshold side
  effect (`N < 4`) should be revisited for new systems.
- `Z` / `z` — impedance on/off; useful for EEG/ECG electrode quality check.
- `p` + 1 byte — versatile 2-byte prefix command pattern, easy to extend with
  new sub-commands.
- `v` — mode/reset indicator; consider returning a structured version string
  rather than plain text in a new design.

### Data packet format to reuse

The iES native format has a well-designed header that supports multiple sensor
types in one stream. Reuse recommendations:

1. **Keep 0xA0 / 0xC0 framing** — widely used (OpenBCI-compatible).
2. **Keep the frame counter** at byte 1.
3. **Keep the type|channel nibble** at byte 2 — compact and self-describing.
4. **Keep int24 big-endian channel data** — matches OpenBCI clients.
5. **Extend `IES_SAMPLE_TYPE`** by using unused nibble values 6–15 for new
   sensor types.

### Suggested additions for a new design

| Gap in v0.3 design               | Suggested fix                                      |
|-----------------------------------|----------------------------------------------------|
| No acknowledgement for commands   | Add a 1-byte ACK `0xAA` / NACK `0x55` response    |
| No checksum on data packets       | Append CRC-8 byte before `0xC0` end byte           |
| Frame counter not reset on start  | Reset `sample_counter = 0` on `START_STREAMING`    |
| Downsampling has SD-card side effect | Separate SD-card control into its own command   |
| OpenBCI mode does not multiplex sensors | Add IMU/EDA to OpenBCI stream or use iES mode only |
| `connected()` always returns `true` | Implement hardware status pin or BT event callback |

---

## 10. Nicla Voice — Packet Type Nibble Registry

> **Scope:** This section is a Nicla Voice addition to the iES v0.3 spec.  
> The type nibble is a 4-bit field (values 0–15) shared by all `[0xA0]`-framed
> device → host packets. It is crucial that any new Nicla packet type is assigned
> from the **free** range to avoid misinterpretation by an iES v0.3 host.

### Full Nibble Dictionary

| Value | Hex byte (2-ch example) | Name | Origin | Status |
|-------|------------------------|------|--------|--------|
| **0** | `0x02` | `EEG` | iES v0.3 | ✅ In use (iES + Nicla) |
| **1** | `0x12` | `IMPEDANCE` | iES v0.3 | ✅ Reserved (iES); not yet used in Nicla |
| **2** | `0x22` | `NECK_IMU` | iES v0.3 | ✅ Reserved (iES); not applicable to Nicla hardware |
| **3** | `0x32` | `EAR_IMU` | iES v0.3 | ✅ Reserved (iES); not applicable to Nicla hardware |
| **4** | `0x42` | `EDA` | iES v0.3 | ✅ Reserved (iES); not applicable to Nicla hardware |
| **5** | `0x51` | `BATT_INFO` | iES v0.3 | ✅ Reserved (iES); not applicable to Nicla hardware |
| **6** | `0x6N` | `RESPONSE` | Nicla extension | ✅ In use — device → host command-acknowledgement frame |
| **7** | `0x71` | `TIME_SYNC` | Nicla extension | ✅ In use — proactive device → host timestamp frame |
| **8** | `0x81` | `ML_OUTPUT` | Nicla extension | 🔮 Planned (Phase Q8) — on-device inference result |
| **9–15** | — | *(unassigned)* | — | 🆓 Free for future use |

> **Critical rule:** Types 0–5 are owned by the iES v0.3 spec.  
> All Nicla-only packet types **must** use values 6–15 so that an unmodified iES
> host receives them as unknown types and silently discards them.

### Why Types 4 and 5 Must Not Be Used by Nicla

An early draft of the Nicla firmware assigned:
- Type **4** to `RESPONSE` frames (collision with iES `EDA`)
- Type **5** to `TIME_SYNC` frames (collision with iES `BATT_INFO`)

An iES host receiving a RESPONSE frame would have decoded it as an EDA packet
(typically 10 bytes, 2-channel nS values) — corrupting displayed data.  
An iES host receiving a TIME_SYNC frame would have decoded it as a BATT_INFO
packet — displaying a spurious battery percentage once per second.

Both collisions have been corrected by reassigning to types 6 and 7 respectively.

### RESPONSE Frame Format (type 6)

```
[0xA0][FC][0x6N][cmd_id 1B][status 1B][payload N bytes][0xC0]
       ↑     ↑
     frame  N = payload_len (lower nibble of byte 2)
     counter
```

Where `status` is the `CmdStatus` enum (`0x00` = OK, see `cmd.h`).

### TIME_SYNC Frame Format (type 7)

```
[0xA0][FC][0x71][ts_us 4B BE][sample_cnt 4B BE][0xC0]   = 12 bytes total
```

| Field | Size | Encoding | Description |
|-------|------|----------|-------------|
| `ts_us` | 4 B | uint32, big-endian | Microseconds since boot at the moment this frame is emitted |
| `sample_cnt` | 4 B | uint32, big-endian | Total EEG samples acquired since boot at same moment |

Emitted once per second by `PacketiserTask`. Host divides `ts_us` by
`sample_cnt` to compute the rolling mean sample interval (µs/sample), then
extrapolates timestamps for all samples in between.

**Note:** `ts_us` uses the Mbed OS monotonic clock (`us_ticker_read()`), not
wall-clock time. To correlate with wall clock, the host must combine this with
a one-time `'t'` command synchronisation (see Section 4.4).

### ML_OUTPUT Frame Format (type 8, future)

```
[0xA0][FC][0x81][label 1B][confidence 4B LE][0xC0]   = 9 bytes total
```

Structure is provisional and will be finalised in Phase Q8.

---

## 11. Nicla Voice — Host Receiver Requirements

> **Scope:** Mandatory rules for any host application receiving Nicla Voice data
> over **UART (USB CDC)** or **BLE**. Applies to Python test tools, `FirmwareTestApp`,
> and future mobile/desktop clients. See also `technical_notes.md` NOTE-010 and
> `ble_channel_design.md` §7.

### 11.1 Device-side framing vs host-side batching

| Layer | UART (USB CDC) | BLE (future) |
|-------|----------------|--------------|
| **Firmware TX** | One IES wireframe per `Serial.write()` — **no device batching** | Multiple frames aggregated into one ATT notification |
| **Host read path** | Windows USB CDC delivers bytes in **bursts** (virtual COM batching) | OS delivers notification payloads in bursts |

The firmware does **not** coalesce UART frames. Any apparent “gaps” measured with
host wall-clock time between USB reads are **transport artefacts**, not missing EEG
samples.

### 11.2 Mandatory host responsibilities

Every Windows (and generally every) receiver **must**:

1. **Debatch** — Parse the byte stream with a stateful wireframe parser
   (`WireframeParser` / equivalent). Never assume one read equals one frame.
2. **Detect drops via frame counter** — Byte 1 of every IES frame increments
   monotonically (mod 256). When comparing consecutive **EEG** frames, account for
   **non-EEG frames** (RESPONSE, TIME_SYNC, etc.) that consume counter steps.
   Do **not** use host arrival time or inter-read interval for continuity checks.
3. **Reconstruct timestamps from TIME_SYNC** — Use type-7 `device_ts_us` +
   wire-sample index (or `sample_cnt` anchor). Do **not** use USB/BLE arrival
   time for EEG sample timestamps.

Reference implementations: `firmware/uartLogger/ies_protocol.py`,
`firmware/uartLogger/plot.py`, `firmware/uartLogger/test_streaming.py`.

### 11.3 Timestamp reconstruction (summary)

On each TIME_SYNC frame (type 7):

```
wire_period_us = 1_000_000 / (ODR_SPS / downsampling_factor)
timestamp_us[k] = sync_ts_us + k × wire_period_us
```

where `k` is the zero-based index of EEG wireframes since that TIME_SYNC anchor.
For sub-sample alignment against the acquisition counter, use `sample_cnt` from
the TIME_SYNC payload (see §10, TIME_SYNC frame format).

### 11.4 BLE-specific note

BLE notifications may contain **multiple concatenated frames** and may split
mid-frame across notifications. The same debatching parser and frame-counter /
TIME_SYNC rules apply once bytes are extracted from each notification payload.

---

## 12. Nicla Voice — Host Protocol Mode (Modern vs Legacy/OpenVIBE)

> **Scope:** This section is a Nicla Voice addition to the iES v0.3 spec.
> It documents `HostProtocolMode` (`firmware/ADS1299NiclaFW/runtime_state.h`),
> added so ADS1299NiclaFW can be driven **unmodified** by the original
> OpenVIBE `CDriveriES` driver (`code_references/iES_OpenVIBE_driver/`)
> without changing default (Modern) behavior for current tooling.

### 12.1 Why a separate mode is needed

OpenVIBE's `CDriveriES` was written against the original iES v0.3 contract —
no command acknowledgements, exactly 2 EEG channels, 115200 baud — and has
one outright bug in its time-sync framing:

- **`'t'` never puts a CRC byte on the wire.** `resetBoard()` builds
  `char time_sync_command[6] = {'t', e1, e2, e3, e4, '\0'}` and sends it via a
  helper that computes length with `strlen()`. Since the buffer ends in
  `'\0'`, only **5 bytes** (`'t'` + 4 epoch bytes) ever reach the wire — the
  CRC the driver computes is silently dropped. An unmodified iES/Modern-Nicla
  receiver expecting 6 bytes for `'t'` would consume the next command's first
  byte as the (missing) CRC, permanently desyncing the parser.
- **The parser never reads the type/channel nibble (byte 2).** It always
  treats every `[0xA0]…[0xC0]` frame as a fixed 2-channel EEG sample. Any
  Nicla-only frame (`RESPONSE`, `TIME_SYNC`, `ML_OUTPUT` — §10) landing in the
  stream corrupts the next EEG frame's byte alignment.
- **Several commands are sent fire-and-forget** (`bWaitForResponse=false`)
  for `.`  and `t`, matching the original "no acknowledgement" contract — a
  Nicla `RESPONSE` frame in reply would be unread bytes sitting in the input
  buffer, corrupting the next parse.
- **Baud is hardcoded to `CBR_115200`** in the driver's port-open call.

`HostProtocolMode::LEGACY_IES` makes ADS1299NiclaFW emit byte-for-byte what
this driver expects, while `HostProtocolMode::MODERN` (default) keeps all
current behavior — `RESPONSE`/`TIME_SYNC` frames, 5-byte `'t'` with CRC,
460800 baud, arbitrary channel counts.

### 12.2 `CMD_SET_HOST_MODE` (`0x14`)

| Field | Size | Notes |
|-------|------|-------|
| Command byte | 1 B | `0x14` |
| Mode | 1 B | `0` = MODERN, `1` = LEGACY_IES |

Rejected with `ERR_NOT_ALLOWED` while streaming (it is not in the
`STOP`/`HEARTBEAT`/`TIME_SYNC` streaming-gate whitelist in
`cmd_handler.cpp::executeCommand()`) — a host must stop streaming first.

On entering `LEGACY_IES`, the device auto-normalizes once so a stale
session can't silently desync the driver:
- Channel mask forced to exactly 2 channels (factory default `0x0C` if the
  current mask doesn't already have a 2-channel popcount).
- Output mode forced to `OutputMode::IES` (µV integers — OpenVIBE never
  rescales raw ADC counts).

While `LEGACY_IES` is active, `CMD_SET_CHANNEL_MASK` rejects any mask whose
popcount ≠ 2, and `CMD_SET_OUTPUT_MODE(RAW)` is rejected with
`ERR_NOT_ALLOWED` — both would otherwise desync OpenVIBE's fixed-format
parser without it ever knowing. Likewise `CMD_SET_ODR` and `'d'`
(`CMD_DOWNSAMPLING`) reject any value other than `SAMPLE_RATE_250` / factor
`1` respectively, for the reason given in §12.3a.

### 12.3 Wire-format differences while `LEGACY_IES` is active

| Behavior | MODERN (default) | LEGACY_IES |
|----------|-------------------|------------|
| `RESPONSE` (type 6) frames | Emitted for every command | **Suppressed** — matches the original iES "no ack" contract. Exception: the ack for `CMD_SET_HOST_MODE` itself is always sent (see §12.4) |
| `TIME_SYNC` (type 7) frames | Emitted every 1 s | **Suppressed** (internal timer still advances, so switching back doesn't burst-emit a backlog) |
| `'t'` payload length | 5 B (4-byte epoch + 1-byte CRC-8, validated) | 4 B (epoch only, no CRC — matches what OpenVIBE actually transmits; see §12.1) |
| Baud rate | 460800 | 115200 (`CBR_115200`, matches OpenVIBE's hardcoded port config) |
| EEG channel count | Whatever `CMD_SET_CHANNEL_MASK` allows | Locked to exactly 2 |
| Output mode | RAW or IES | Locked to IES (µV integers) |

With these suppressed, every frame ADS1299NiclaFW emits in `LEGACY_IES` is a
plain 10-byte `[0xA0][cnt][0x02][ch0 3B][ch1 3B][0xC0]` EEG frame — exactly
what `CDriveriES::parseByte()` can consume, since it never reads the type
nibble and assumes 2 channels unconditionally.

### 12.3a Sample rate is locked to a true 250 SPS, no downsampling

115200 baud can sustain roughly 250 SPS × 2 channels of 10-byte iES µV
frames and no more. `RuntimeState`'s factory default runs the ADS1299 ADC at
1000 SPS and decimates by `downsampling_factor = 4` to reach the same
250 SPS *effective* wire rate — that combination is fine for `MODERN`, but
it means the ADC's internal digital filter is shaped for 1000 SPS while only
1 in 4 samples is ever transmitted, which is both wasteful and mismatched
with the fixed 250 Hz rate OpenVIBE is configured to expect.

`CommandHandlerTask::enforceLegacyDefaults()` therefore forces:

- `sample_rate = ADS1299_Library::SAMPLE_RATE_250` (ADC sampled directly at
  250 SPS), and
- `downsampling_factor = 1` (every sample transmitted, no decimation)

whenever `LEGACY_IES` becomes (or remains) active — i.e. on
`CMD_SET_HOST_MODE(1)`, and again after `CMD_DEMO`'s
`RuntimeState::initialize()` (which would otherwise silently revert to the
1000 SPS/DS×4 factory default while leaving `host_protocol_mode` itself
preserved). `CMD_SET_ODR` and `CMD_DOWNSAMPLING` reject any other value
while `LEGACY_IES` is active (see §12.2). As with ODR changes in general,
this only updates `RuntimeState` — the ADS1299 `CONFIG1` register is
written lazily at the next `cmdStartStreaming()`, matching the existing
"never touch CONFIG1 mid-stream" rule (§12.3).

### 12.4 Baud-rate switching sequence

ADS1299NiclaFW's `Serial` is native USB-CDC (not a bridged hardware UART —
see `firmware/ADS1299NiclaFW/config.h` §4), so there is no physical
bit-timing risk in changing the declared baud at runtime. It is still
switched to match `CBR_115200` because Windows' CDC-ACM driver stack has
shown baud-related quirks on this hardware in the past (`config.h`: "1 Mbaud
caused Windows CDC corruption").

Two moments decide the active baud:

1. **Boot time.** `ADS1299NiclaFW.ino::setup()` loads `PersistentConfig`
   (flash-only, no `Serial` dependency) **before** calling `Serial.begin()`,
   so a unit that was left in `LEGACY_IES` (see §12.5) boots directly at
   `SERIAL_BAUD_LEGACY` (115200) — no per-session provisioning step needed.
2. **Runtime**, via `CMD_SET_HOST_MODE`. Since OpenVIBE itself can never send
   this command (it doesn't know it exists), entering `LEGACY_IES` for the
   first time always requires one session with a *modern* tool at 460800:

   1. Host sends `CMD_SET_HOST_MODE(1)` at the current baud.
   2. `CommandHandlerTask` flips `RuntimeState`'s mode and arms a pending
      baud change on `UartChannelTask`, but does **not** touch `Serial`
      itself.
   3. `PacketiserTask` always forces this command's own ack through — even
      though `LEGACY_IES` normally suppresses `RESPONSE` frames — because the
      host needs positive confirmation before it reconfigures its port.
   4. `UartChannelTask` applies the pending baud change (`Serial.end()` /
      `Serial.begin(newBaud)`) only once its TX queue is empty, guaranteeing
      the ack above was fully flushed at the **old** baud first.
   5. The host must close and reopen its OS-level serial handle at the new
      baud after receiving the ack — this is a hard requirement for any CDC
      baud change, not optional. See `IesUartClient.switch_host_mode()` in
      `firmware/uartLogger/ies_protocol.py`.

   Switching back to `MODERN` is symmetric (ack at 115200, then
   `Serial.begin(460800)`).

### 12.5 Persistence

`HostProtocolMode` is persisted in the `EepromLayout.host_protocol_mode`
byte (`firmware/ADS1299NiclaFW/persistent_config.h`) — this repurposes what
was previously a `reserved` byte that is `0x00` (== `MODERN`) on every
already-flashed unit, so no schema-version bump or factory-reset migration
is needed. `CMD_QUERY_STATUS` reports the current mode as payload byte 10
(`payload_len` is now 11 — see `firmware/uartLogger/ies_protocol.py`
`decode_status_payload()`).

**Typical one-time provisioning workflow** for a unit that will live
permanently behind OpenVIBE:

```
1. Connect with a modern tool (460800 baud).
2. CMD_SET_HOST_MODE(legacy=1)  → device acks, tool reconnects at 115200.
3. CMD_SAVE_CONFIG               → persists LEGACY_IES to flash.
4. Disconnect. Launch OpenVIBE — it opens the port at its hardcoded 115200
   and the device is already speaking pure iES from the next boot onward.
```

### 12.6 Operational caveat: OpenVIBE's declared sample rate is static

OpenVIBE's own OpenViBE-Acquisition-Server config for `CDriveriES` declares
a fixed sampling frequency in its own settings file — it is **not** read
from the device. This cannot be fixed in firmware, but as of §12.3a the
value the operator must enter is no longer a moving target: ADS1299NiclaFW's
wire rate in `LEGACY_IES` is always exactly **250 Hz** (`SAMPLE_RATE_250`,
`downsampling_factor = 1`, enforced automatically and not user-configurable
while legacy mode is active), so OpenVIBE's sample rate setting only needs
to be set once and never revisited.

---

*End of document.*
