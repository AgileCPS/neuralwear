# iES v0.3 Application Layer Protocol

**Source**: `code_references/iES_v0.3-master/`  
**MCU**: Texas Instruments MSP432P4111 running TI-RTOS (SYS/BIOS)  
**Authors**: Nhat Pham, University of Colorado Boulder, 2018

---

## Table of Contents

1. [System Architecture Overview](#1-system-architecture-overview)
2. [Task Structure and RTOS Design](#2-task-structure-and-rtos-design)
3. [Inter-Task Communication](#3-inter-task-communication)
4. [Bluetooth Transport Layer (BT-SPP)](#4-bluetooth-transport-layer-bt-spp)
5. [Command Protocol — Host to Device](#5-command-protocol--host-to-device)
6. [Data Streaming Protocol — Device to Host](#6-data-streaming-protocol--device-to-host)
7. [Sensor Data Types and Encoding](#7-sensor-data-types-and-encoding)
8. [EEG Signal Processing](#8-eeg-signal-processing)
9. [Downsampling and Flow Control](#9-downsampling-and-flow-control)
10. [SD Card Recording](#10-sd-card-recording)
11. [Adaptive Gain Control (AGC)](#11-adaptive-gain-control-agc)
12. [Operating Modes](#12-operating-modes)
13. [State Machine Summary](#13-state-machine-summary)
14. [Binary Encoding Utilities](#14-binary-encoding-utilities)

---

## 1. System Architecture Overview

The iES v0.3 device is a wearable biosignal acquisition platform. At the application layer, it:

- Acquires EEG signals via the ADS1299-4 (handled by the driver layer, not documented here).
- Acquires IMU data from one or two MPU9250 sensors (neck and ear positions).
- Acquires electrodermal activity (EDA) from two ADC channels.
- Optionally monitors battery state-of-charge via a MAX17043 fuel gauge.
- Streams all data wirelessly to a host computer or mobile device over **Bluetooth Classic SPP** (Serial Port Profile) using an **RN-42** Bluetooth module.
- Simultaneously logs data to a **microSD card** using FatFS.

```
┌─────────────────────────────────────────────────────────────┐
│                     MSP432P4111 (TI-RTOS)                   │
│                                                             │
│  ┌────────────┐  ┌────────────┐  ┌──────────┐  ┌────────┐  │
│  │  iES Main  │  │ IMU Task   │  │ EDA Task │  │Int Temp│  │
│  │  Task      │  │(MPU9250 +  │  │  Task    │  │  Task  │  │
│  │  (prio 3)  │  │ MAX17043)  │  │ (prio 2) │  │(prio 1)│  │
│  │            │  │  (prio 2)  │  │          │  │        │  │
│  └─────┬──────┘  └─────┬──────┘  └────┬─────┘  └────────┘  │
│        │               │              │                     │
│  ┌─────▼──────┐  ┌─────▼──────┐  ┌───▼──────┐              │
│  │ BTSPP Send │  │  IMU2IES   │  │ EDA2IES  │              │
│  │    Task    │  │   mqueue   │  │  mqueue  │              │
│  │  (prio 2)  │  └────────────┘  └──────────┘              │
│  └─────┬──────┘                                             │
│        │               ┌──────────────┐                     │
│  ┌─────▼──────┐        │ BTSPP Recv   │                     │
│  │ iES2BTSPP  │        │    Task      │                     │
│  │   mqueue   │        │   (prio 2)   │                     │
│  └─────┬──────┘        └──────┬───────┘                     │
└────────┼─────────────────────┼────────────────────────────┘
         │                     │
         ▼                     ▲
    ┌────────────────────────────┐
    │   RN-42 BT-SPP Module      │
    │   UART1 @ 115200 baud      │
    └────────────────────────────┘
         │
         ▼
    [ Host (PC / Mobile App) ]
```

---

## 2. Task Structure and RTOS Design

The firmware uses **POSIX threads** (pthread) on top of TI-RTOS. Six concurrent tasks run after startup.

| Task Function             | Priority | Stack  | Responsibilities                                              |
|---------------------------|----------|--------|---------------------------------------------------------------|
| `ies_task_fxn`            | 3        | 5120 B | Main coordinator: ADS1299 data, SD logging, event dispatch    |
| `btspp_recv_task_fxn`     | 2        | 1024 B | Receive and parse host commands over BT-SPP UART              |
| `btspp_send_task_fxn`     | 2        | 1024 B | Serialize data frames and transmit over BT-SPP UART           |
| `imu_task_fxn`            | 2        | 1024 B | Poll MPU9250 IMU(s) and MAX17043 battery gauge at 50 Hz       |
| `eda_task_fxn`            | 2        | 1024 B | Sample EDA electrodes at 50 Hz via ADC                        |
| `int_temp_task_fxn`       | 1        | 1024 B | Read MSP432 internal temperature sensor (optional, disabled)  |

### Startup Sequence

```
main()
  ├── Power_init(), GPIO_init(), UART_init(), SPI_init(),
  │   I2C_init(), ADC_init(), ADCBuf_init(), Timer_init()
  └── ies_create_tasks()
        └── ies_task_fxn() starts, then spawns:
              ├── ADS1299 initialisation (driver layer)
              ├── SD card mount (if save_to_sd_card)
              ├── BT-SPP module init (BPLib::begin)
              ├── btspp_create_tasks()  → btspp_recv_task, btspp_send_task
              ├── Sampling timer init (50 Hz, TIMER_CONTINUOUS_CB)
              ├── imu_create_task()     → imu_task
              ├── eda_create_task()     → eda_task  (if use_eda)
              └── int_temp_create_task() → int_temp_task (if use_int_temp)
```

After initialisation the main task enters an **event-pend loop**
(`Event_pend(..., BIOS_WAIT_FOREVER)`), waking only when one or more
TI-RTOS events fire.

---

## 3. Inter-Task Communication

### TI-RTOS Events (`Event_Handle ies_event_handle`)

Events are posted by sensor tasks or by the BTSPP receive task and consumed
by the iES main task.

| Event Constant     | `Event_Id`    | Triggered by                        | iES Main Task Action                    |
|--------------------|---------------|-------------------------------------|-----------------------------------------|
| `START_STREAMING`  | `Event_Id_00` | BTSPP Recv (on `b` cmd)             | Open SD files, start ADS stream, timer  |
| `STOP_STREAMING`   | `Event_Id_01` | BTSPP Recv (on `s` cmd)             | Stop stream, close SD files             |
| `ADS_NEW_DATA`     | `Event_Id_02` | ADS1299 DRDY interrupt (driver)     | Read & process EEG samples              |
| `IMU_NEW_DATA`     | `Event_Id_03` | `imu_task_fxn`                      | Drain `IMU2IES` queue, log/forward data |
| `EDA_NEW_DATA`     | `Event_Id_04` | `eda_task_fxn`                      | Drain `EDA2IES` queue, log/forward data |
| `PRINT_EEG`        | `Event_Id_05` | BTSPP Recv (on `pe` cmd)            | Enable UART EEG print, disable BT-SPP TX|
| `PRINT_NECK_ACCEL` | `Event_Id_06` | BTSPP Recv (on `pa` cmd)            | Enable UART neck accel print            |
| `PRINT_EDA`        | `Event_Id_07` | BTSPP Recv (on `pd` cmd)            | Enable UART EDA print                   |
| `PRINT_NECK_GYRO`  | `Event_Id_08` | BTSPP Recv (on `pg` cmd)            | Enable UART neck gyro print             |
| `OPEN_BCI_MODE`    | `Event_Id_09` | BTSPP Recv (on `po` cmd)            | Switch to OpenBCI packet format         |
| `IES_MODE`         | `Event_Id_10` | BTSPP Recv (on `pi` cmd)            | Switch to iES packet format             |
| `PRINT_EAR_ACCEL`  | `Event_Id_11` | BTSPP Recv (on `pb` cmd)            | Enable UART ear accel print             |
| `PRINT_EAR_GYRO`   | `Event_Id_12` | BTSPP Recv (on `ph` cmd)            | Enable UART ear gyro print              |
| `RESTART_STREAMING`| `Event_Id_13` | Auto-save timer                     | Sync SD files (auto-save mid-session)   |
| `BATT_NEW_DATA`    | `Event_Id_14` | `imu_task_fxn` (via MAX17043)       | Drain `BATT2IES` queue, forward data    |

### POSIX Message Queues

| Queue Name    | Direction         | Capacity  | Message Size           |
|---------------|-------------------|-----------|------------------------|
| `IMU2IES`     | IMU → iES Main    | 128 msgs  | `sizeof(data_sample_rec)` |
| `BATT2IES`    | IMU → iES Main    | 8 msgs    | `sizeof(data_sample_rec)` |
| `EDA2IES`     | EDA → iES Main    | 128 msgs  | `sizeof(data_sample_rec)` |
| `iES2BTPP`    | iES Main → BT Send| 512 msgs  | `sizeof(data_sample_rec)` |

All message payloads use the same `data_sample_rec` structure:

```c
typedef struct sample_rec {
    uint8_t  sample_type;                         // IES_SAMPLE_TYPE enum
    uint8_t  num_of_channels;                     // 1..6
    int32_t  channel_data[SAMPLE_REC_MAX_CHANNELS]; // SAMPLE_REC_MAX_CHANNELS = 6
} data_sample_rec;
```

### BTSPP Send Semaphore

The iES main task posts `btspp_send_sem` (a POSIX semaphore) every time it
enqueues a message to `iES2BTPP`. The send task blocks on `sem_wait` and
only wakes when data is available, preventing busy-polling.

---

## 4. Bluetooth Transport Layer (BT-SPP)

The `BPLib` class wraps the **RN-42** Bluetooth Classic module, operated in
**SPP (Serial Port Profile)** mode over **UART1** at **115200 baud** (binary,
no echo).

### Module Initialisation (`BPLib::begin`)

```
1. Assert hardware RESET (GPIO low, 1 s)
2. Deassert RESET (GPIO high, 1 s)
3. Open UART1 @ 115200 baud, binary mode
4. Send "$$$"              → expect "CMD\r\n"   (enter command mode)
5. Send "---\r\n"          → expect "END\r\n"   (exit command mode)
```

The module is left in data mode. All subsequent communication is raw binary
or ASCII bytes directly over the SPP link.

### Raw Data Transfer API

| Function                             | Direction      | Description                          |
|--------------------------------------|----------------|--------------------------------------|
| `BPLib::sendBuffer(buf, size)`       | Device → Host  | Transmit binary buffer over UART     |
| `BPLib::readRaw(buf, size)`          | Host → Device  | Blocking read of `size` bytes        |
| `BPLib::connected()`                 | —              | Check if host is connected           |

---

## 5. Command Protocol — Host to Device

All commands from the host are **single ASCII bytes**. Some commands are
followed immediately by a multi-byte payload.

### Single-Byte Commands

| Byte | ASCII | Constant                  | Description                                          |
|------|-------|---------------------------|------------------------------------------------------|
| `0x62` | `b` | `IES_STREAM_START`        | Start biosignal streaming                            |
| `0x73` | `s` | `IES_STREAM_STOP`         | Stop streaming                                       |
| `0x74` | `t` | `IES_TIME_SYNC`           | Time synchronisation — **followed by 5-byte payload**|
| `0x2E` | `.` | `IES_BTSPP_HEART_BEAT`    | Keep-alive heartbeat (no action, acknowledged in log)|
| `0x70` | `p` | `IES_BTSPP_UART_PRINT_SEL`| UART print selection — **followed by 1-byte selector**|
| `0x64` | `d` | `IES_BTSPP_DOWNSAMPLING`  | Set downsampling factor — **followed by 1-byte value**|
| `0x5A` | `Z` | `IES_IMPEDANCE_CHECK_ON`  | Enable lead-off (impedance) detection on ch 3 & 4   |
| `0x7A` | `z` | `IES_IMPEDANCE_CHECK_OFF` | Disable lead-off detection on ch 3 & 4              |
| `0x76` | `v` | `OPEN_BCI_SOFT_RESET`     | Send OpenBCI startup banner string to host           |

### Multi-Byte Command: Time Synchronisation (`t` + 5 bytes)

Synchronises the real-time clock on the MSP432.

```
┌────────────────────────────────────────────────┐
│ Byte 0     │ 't' (0x74) – command byte         │
│ Bytes 1–4  │ Seconds since Unix epoch           │
│            │ 32-bit big-endian (MSB first)      │
│ Byte 5     │ CRC-8 checksum of bytes 1–4        │
└────────────────────────────────────────────────┘
```

The device validates `CRC-8(bytes[0..3]) == bytes[4]` before applying the
timestamp. On failure, a debug log entry is written and the command is
silently dropped.

### Multi-Byte Command: UART Print Selection (`p` + 1 byte)

Selects which data channel is printed to the debug UART. Sending any of
these redirects output away from BT-SPP streaming.

| Payload byte | ASCII | Effect                                                  |
|--------------|-------|---------------------------------------------------------|
| `0x65`       | `e`   | Print EEG channel data to UART                          |
| `0x61`       | `a`   | Print neck accelerometer (X, Y, Z) to UART              |
| `0x62`       | `b`   | Print ear accelerometer (X, Y, Z) to UART               |
| `0x67`       | `g`   | Print neck gyroscope (X, Y, Z) to UART                  |
| `0x68`       | `h`   | Print ear gyroscope (X, Y, Z) to UART                   |
| `0x64`       | `d`   | Print EDA channel data to UART                          |
| `0x6F`       | `o`   | Switch to **OpenBCI compatible** streaming mode (BT-SPP)|
| `0x69`       | `i`   | Switch to **iES native** streaming mode (BT-SPP)        |

### Multi-Byte Command: Downsampling (`d` + 1 byte)

Sets the EEG transmit downsampling factor for BT-SPP streaming.

```
┌──────────────────────────────────────────────────┐
│ Byte 0  │ 'd' (0x64) – command byte              │
│ Byte 1  │ Downsampling factor N (uint8_t)         │
│         │   N < 4  → SD card logging DISABLED     │
│         │   N ≥ 4  → SD card logging ENABLED      │
└──────────────────────────────────────────────────┘
```

Default downsampling factor is **4** (1 in every 4 EEG samples forwarded),
hardcoded as `ies2btspp_down_sampling = 4`.

---

## 6. Data Streaming Protocol — Device to Host

All data is sent as binary frames over BT-SPP. Two packet formats are
supported, selectable at runtime.

### 6.1 iES Native Packet Format (default)

```
 Byte  Offset   Field                          Value / Notes
 ─────────────────────────────────────────────────────────────────
  0             Start byte                     0xA0
  1             Frame counter                  0–255, wraps around
  2             Type / Channel byte            Bits [7:4] = sample_type
                                               Bits [3:0] = num_of_channels
  3             Ch 0 data [23:16]              ┐
  4             Ch 0 data [15:8]               │ 24-bit signed, big-endian
  5             Ch 0 data [7:0]                ┘
  6             Ch 1 data [23:16]              ┐ (if num_of_channels ≥ 2)
  7             Ch 1 data [15:8]               │
  8             Ch 1 data [7:0]                ┘
  ...           (3 bytes per additional channel)
  3+N*3         End byte                       0xC0
 ─────────────────────────────────────────────────────────────────
 Total = 3 + (num_of_channels × 3) + 1 bytes
```

**sample_type values** (high nibble of byte 2):

| Value | Enum constant | Data content                                      |
|-------|---------------|---------------------------------------------------|
| 0     | `EEG`         | EEG in µV (gain-normalised, fixed-point integer)  |
| 1     | `IMPEDANCE`   | Lead-off impedance (same channel mapping as EEG)  |
| 2     | `NECK_IMU`    | Neck IMU: [accelX, accelY, accelZ, gyroX, gyroY, gyroZ] × 1000 |
| 3     | `EAR_IMU`     | Ear IMU: [accelX, accelY, accelZ, ch3, ch4, ch5] × 1000 (ch3–5 are analog accel in mV when `use_analog_accel = true`) |
| 4     | `EDA`         | [left_nS, right_nS] — electrodermal activity in nanosiemens |
| 5     | `BATT_INFO`   | [SoC × 100] — battery state of charge in 0.01 % units |

**Example — 2-channel EEG packet:**

```
A0  07  02  FF D2 00  00 3A BC  C0
│   │   │   └────────┘  └────────┘  │
│   │   │    Ch0 (-11776 µV)  Ch1 (15036 µV)
│   │   └── type=0 (EEG), channels=2
│   └── frame counter = 7
└── start byte
                                    └── end byte
```

### 6.2 OpenBCI Compatible Packet Format

Enabled via the `po` command. Matches the OpenBCI V3 streaming protocol for
compatibility with existing OpenBCI host software.

```
 Byte  Offset   Field                          Value / Notes
 ─────────────────────────────────────────────────────────────────
  0             Start byte                     0xA0
  1             Sample counter                 0–255, wraps around
  2–4           Ch 1 raw ADC (24-bit, MSB)     ┐
  5–7           Ch 2 raw ADC (24-bit, MSB)     │ Raw ADS1299 counts,
  ...                                          │ not gain-normalised
  29–31         Ch 10 raw ADC (24-bit, MSB)    ┘
  32            Stop byte                      0xC0
 ─────────────────────────────────────────────────────────────────
 Total: 33 bytes (fixed)
```

In OpenBCI mode, **raw ADC counts** are used (`raw_sample_rec`), not
µV-converted values.

The startup banner sent on `v` command:
```
OpenBCI V3 16 channel\n
ADS1299 Device ID: 0x3E\n
LIS3DH Device ID: 0x33\n
$$$
```

---

## 7. Sensor Data Types and Encoding

### 7.1 EEG (ADS1299-4)

- **Channels streamed**: Channels 3 and 4 of the ADS1299-4 only  
  (`OPENBCI_NUMBER_CHANNELS_TO_STREAM = 2`, `OPENBCI_CHANNEL_DATA_INDEX = 3`)
- **Data value**: Signed 32-bit integer in **µV** (gain-normalised)
- **Encoding in packet**: 24-bit signed, big-endian (truncated from 32-bit)
- **Sampling rate**: 1 kSPS (from ADS1299 DRDY interrupt)
- **Channels 1 and 2** are deactivated at startup

### 7.2 IMU — Neck and Ear (MPU9250)

Both IMU instances use the same `sample_rec` structure:

```
channel_data[0] = accelX × 1000   (integer milliG or mm/s²)
channel_data[1] = accelY × 1000
channel_data[2] = accelZ × 1000
channel_data[3] = gyroX  × 1000   (or analog accel X in mV for ear IMU)
channel_data[4] = gyroY  × 1000   (or analog accel Y in mV for ear IMU)
channel_data[5] = gyroZ  × 1000   (or analog accel Z in mV for ear IMU)
```

- **Sampling timer**: 20,000 µs period → **50 Hz**
- **I2C bus**: `Board_I2C0` at 100 kHz
- When `use_analog_accel = true` (default), the ear IMU channels 3–5 are
  replaced with ADC readings (P5.5/A0, P5.4/A1, P5.4/A2) converted to mV.
  The ADC reference is 2.5 V.

### 7.3 EDA — Electrodermal Activity

Two ADC channels: left EDA (`Board_ADC0`), right EDA (`Board_ADC1`).

The EDA circuit uses a fixed resistor network. The conversion formula applied
internally is:

```
eda_nS = (10 × voltage_uV) / (REF_uV − voltage_uV) × 1000
```

where `REF_uV = 2,500,000` (2.5 V reference), giving conductance in **nanosiemens**.

Packet encoding:
```
channel_data[0] = left_eda_nS    (int32_t)
channel_data[1] = right_eda_nS   (int32_t)
```

### 7.4 Battery — MAX17043

- Polled at every `batt_fg_downsampling = 500` IMU timer ticks → approx. **every 10 seconds**
- Packet encoding:
  ```
  channel_data[0] = SoC × 100    (int32_t, units: 0.01 %)
  ```

---

## 8. EEG Signal Processing

### Gain Normalisation

Raw 24-bit two's-complement ADS1299 values are converted to µV using:

```
µV = raw_counts × SCALE_FACTOR_UV / gain

SCALE_FACTOR_UV = (4.5 × 10⁶) / (2²³ − 1) ≈ 0.5364 µV/count
```

This represents the ADS1299 full-scale range (±VREF/gain, VREF = 2.5 V)
normalised across the 24-bit range.

The gain is read at runtime via `ads1299.getGainInt(channel)` and can be
1, 2, 4, 6, 8, 12, or 24.

### Gain-Changed Marker

When the Adaptive Gain Control (AGC) changes a channel's gain during streaming:
- The `gain_changed` flag is set to `true` for that sample.
- On the SD card path: **4 duplicate copies** of the current sample are
  written immediately after the real sample, acting as a resync marker for
  post-processing.
- The BT-SPP path is unaffected (the packet itself carries the scaled µV
  value, which already reflects the new gain).

---

## 9. Downsampling and Flow Control

EEG data arrives at up to 1 kSPS. The BT-SPP link at 115200 baud cannot
sustain this rate continuously for multiple sensor types. Downsampling is
applied selectively:

| Path             | Downsampling applied?           | Factor               |
|------------------|---------------------------------|----------------------|
| EEG → BT-SPP     | Yes                             | `ies2btspp_down_sampling` (default 4) |
| EEG → SD card    | No (all samples written)        | —                    |
| IMU → BT-SPP     | No (50 Hz inherently limited)   | —                    |
| EDA → BT-SPP     | No (50 Hz inherently limited)   | —                    |
| Battery → BT-SPP | No (`batt_fg_downsampling` = 500 ticks in IMU task) | — |

The downsampling counter `downsampling_count` is a rolling modulo counter
over all EEG samples, checked per sample:

```c
if (downsampling_count % ies2btspp_down_sampling == 0) {
    // enqueue to iES2BTPP queue and post semaphore
}
downsampling_count++;
```

---

## 10. SD Card Recording

When `save_to_sd_card = true`, each streaming session creates a new directory
named with the start timestamp:

```
Format:  DDHHMMSSMMY
Example: 15143022.10   (15th day, 14:30:22, Oct, year ending in 0)
```

Files within the directory:

| File          | Content                                         | Format |
|---------------|-------------------------------------------------|--------|
| `eeg.csv`     | Ch 3 and Ch 4 µV values                         | `val1, val2\n` per sample |
| `eda.csv`     | Left and right EDA in nS                        | `val1, val2\n` per sample |
| `neck_imu.csv`| Neck IMU (accelX, Y, Z, gyroX, Y, Z)           | `v0, v1, v2, v3, v4, v5\n` |
| `ear_imu.csv` | Ear IMU (same 6 columns)                        | `v0, v1, v2, v3, v4, v5\n` |
| `info.txt`    | Session start and stop timestamps               | Human-readable text |

### Auto-Save Mode

When `auto_save = true`, the streaming session automatically restarts every
`AUTOSAVE_TIMER_PERIODIC_EVT_PERIOD = 60` seconds:

1. `RESTART_STREAMING` event fires → SD files are flushed (`f_sync`).
2. `STOP_STREAMING` event fires → SD files are closed.
3. `START_STREAMING` event fires → new timestamped directory and files opened.

This prevents data loss if the device crashes or the SD card fills up.

---

## 11. Adaptive Gain Control (AGC)

The AGC is an optional feature (`adaptive_gain = false` by default, disabled
at runtime in `ies_task_fxn`). When enabled it automatically adjusts the
ADS1299 per-channel gain to keep the signal within range.

### Algorithm

1. Maintain a **peak detector** over a sliding window of samples.
2. At the end of each window, compare the peak to thresholds:

   | Condition                                          | Action          |
   |----------------------------------------------------|-----------------|
   | peak > `AGC_MAX_RANGE × AGC_HIGH_THRESHOLD / gain` | Decrease gain   |
   | peak < `AGC_MAX_RANGE × AGC_LOW_THRESHOLD / gain`  | Increase gain   |

3. State machine tracks signal character:

   | `agc_state` | Assumed signal | Window size (`agc_ped_window_*`) |
   |-------------|----------------|----------------------------------|
   | 0           | EEG/EOG        | 10 samples                       |
   | > 0         | EMG            | 3000 samples                     |

### Constants

| Constant           | Value     | Meaning                               |
|--------------------|-----------|---------------------------------------|
| `AGC_WINDOW_SIZE`  | 10        | Ring buffer size (not the peak window)|
| `AGC_HIGH_THRESHOLD`| 0.7      | 70% of full-scale → decrease gain     |
| `AGC_LOW_THRESHOLD` | 0.3      | 30% of full-scale → increase gain     |
| `AGC_MAX_RANGE`    | 4,500,000 | Full-scale range in µV (gain = 1)     |

Gain steps follow the ADS1299 gain settings: 1, 2, 4, 6, 8, 12, 24.

---

## 12. Operating Modes

The device can operate in two output modes, switchable at runtime via the
`po` and `pi` sub-commands.

### iES Native Mode (default, `openbci_compatible = false`)

- Variable-length packets with sample-type tag.
- µV-scaled EEG values.
- All sensor types (EEG, IMU, EDA, Battery) multiplexed in the same stream.
- SD card logging enabled by default.

Runtime flags:
```
send_to_btspp   = true
openbci_compatible = false
save_to_sd_card = true
```

### OpenBCI Compatible Mode (`openbci_compatible = true`)

- Fixed 33-byte packets matching OpenBCI V3 format.
- Raw 24-bit ADS1299 counts (no gain normalisation).
- Only EEG data is transmitted; other sensor types are not included in the
  OpenBCI packet format.
- SD card logging disabled.

Runtime flags:
```
send_to_btspp   = true
openbci_compatible = true
save_to_sd_card = false
```

### UART Debug Mode (triggered by `pe`, `pa`, `pb`, `pg`, `ph`, `pd`)

- BT-SPP streaming is disabled (`send_to_btspp = false`).
- Selected sensor data is printed to the debug UART as ASCII CSV.
- Mutually exclusive: only one sensor type is active at a time.

---

## 13. State Machine Summary

The top-level state machine inside `ies_task_fxn`:

```
         ┌──────────────────────────────────────────┐
         │             IDLE state                    │
         │  GREEN LED on, RED LED off                │
         │  Waiting for 'b' (START_STREAMING)        │
         └─────────────────┬────────────────────────┘
                           │  START_STREAMING event
                           ▼
         ┌──────────────────────────────────────────┐
         │           STREAMING state                 │
         │  RED LED on, GREEN LED off                │
         │  ADS streaming active (driver)            │
         │  50 Hz IMU/EDA timer active               │
         │  SD files open (if save_to_sd_card)       │
         │  BT-SPP TX active (if send_to_btspp)      │
         │                                           │
         │  Events processed:                        │
         │   ADS_NEW_DATA  → EEG sample pipeline     │
         │   IMU_NEW_DATA  → IMU forward/log         │
         │   EDA_NEW_DATA  → EDA forward/log         │
         │   BATT_NEW_DATA → Battery forward         │
         └─────────────────┬────────────────────────┘
                           │  STOP_STREAMING event
                           ▼
         ┌──────────────────────────────────────────┐
         │             IDLE state                    │
         │  SD files closed, stream counters reset   │
         └──────────────────────────────────────────┘

         ──────── Mode-change events (any state) ────
         OPEN_BCI_MODE  → set openbci_compatible=true, SD off
         IES_MODE       → set openbci_compatible=false, SD on
         PRINT_*        → send_to_btspp=false, UART echo mode
```

---

## 14. Binary Encoding Utilities

All multi-byte integer values on the wire use **big-endian (MSB-first)**
byte order, encoded by the utility functions in `ies_misc.cpp`.

| Function              | Direction     | Bytes | Notes                                  |
|-----------------------|---------------|-------|----------------------------------------|
| `uint16_to_buffer`    | int → bytes   | 2     | Big-endian                             |
| `uint24_to_buffer`    | int → bytes   | 3     | Big-endian, lower 24 bits of uint32_t  |
| `uint32_to_buffer`    | int → bytes   | 4     | Big-endian                             |
| `uint64_to_buffer`    | int → bytes   | 8     | Big-endian                             |
| `buffer_to_uint16`    | bytes → int   | 2     | Big-endian                             |
| `buffer_to_uint24`    | bytes → int   | 3     | Big-endian                             |
| `buffer_to_uint32`    | bytes → int   | 4     | Big-endian                             |
| `buffer_to_uint64`    | bytes → int   | 8     | Big-endian                             |
| `float_to_buffer`     | float → bytes | 4     | 2-byte integer part + 2-byte fraction (2 decimal digits) |
| `buffer_to_float`     | bytes → float | 4     | Inverse of `float_to_buffer`           |

### CRC-8 Checksum (Time Sync only)

The checksum library (`checksum.h` / `crc8.cpp`) provides `crc_8()` using:

```
Polynomial : 0x07 (CRC-8/SMBUS)
Initial value: 0x00
```

Only the Time Sync command uses a checksum at the application layer. All
other commands rely on BT-SPP link-layer reliability.

---

*End of document. Driver-level details (SPI transactions, ADS1299 register
programming, MPU9250 register access) are outside the scope of this document.*
