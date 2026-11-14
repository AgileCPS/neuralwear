# IMU and battery streaming — temporary design notes

## Data needs

### Battery level

**Modern mode.** The device sends the battery level only on request, or when the battery is going low.

**OpenVIBE mode.** The device sends one battery frame every 10 seconds, no matter what. This send can be commented out if it is incompatible with the host.

### IMU

Motion data is meaningful as a continuous segment at a sufficient rate, for example 50 Hz as in the previous iES stream. One sample returned for one request is not useful.

The stream can be enabled and disabled. While enabled, samples go out continuously at the specified sampling rate. While disabled, nothing goes out.

## The pattern EEG already uses

An EEG sample leaves the device in three steps. IMU and battery use the same three steps. After a frame exists, the gateway, UART, and BLE already carry it, so they get no new wiring.

1. **A task reads the sensor and hands samples to the packetiser.** `EegAcquisitionTask` does this for EEG. In `setup()` it is connected with:

```cpp
eegAcquisitionTask.subscribe(packetiserTask.getEegQueue());
```

2. **The packetiser builds one frame per sample.** `PacketiserTask` takes a sample from that queue and `serialiseEeg()` turns it into one `WireFrame`. It already does the same kind of job for command responses and, later, ML results.

3. **The frame goes out through the gateway.** The packetiser is already connected to the gateway, and the gateway is already connected to UART and BLE:

```cpp
packetiserTask.subscribe(gatewayTask.getDataQueue());
gatewayTask.setUartChannel(uartChannelTask.getTxQueue());
gatewayTask.setBleChannel(bleChannelTask.getTxQueue());
```

The packetiser chooses UART or BLE by setting the frame destination, the same way `serialiseEeg()` does.

Queue depths and task stack sizes live in `config.h`, and the RAM totals there include them. `setup()` starts the EEG task last.

There is no IMU read in the firmware today. The LIS3DH methods were removed from `ADS1299_Library`. `nicla::enableCharging(300)` in `setup()` only turns charging on. `_streamingEnabled` is the EEG streaming switch.

## Do the same for IMU and battery

Add one task per sensor, give the packetiser a queue for it, connect them in `setup()` the way EEG is connected, and add a serialiser next to `serialiseEeg()`.

### The tasks

`ImuTask` (`imu.h`, `imu.cpp`) produces `ImuSample` on a timer. The rate is not chosen yet. The earlier iES stream used 50 Hz. Send samples only while an IMU enable flag is on. That flag is not `_streamingEnabled`. While it is off, send nothing.

`BatteryTask` (`battery.h`, `battery.cpp`) produces `BatterySample`. Read the level in this task. In modern mode, send it when the host asks and when the level goes low, and do not send again on every loop while it stays low. The low threshold is not chosen yet, and no command is named for the request. In OpenVIBE mode, send one sample every 10 seconds with no other checks. That send can be commented out.

Both tasks have a lower priority than `EegAcquisitionTask`. Declare them the same way as `eegAcquisitionTask`: `extern` in the header, the global in the `.cpp`.

### `setup()` in `src.ino`

Include the two headers next to `eeg.h`. Connect the tasks next to the EEG line:

```cpp
imuTask.subscribe(packetiserTask.getImuQueue());
batteryTask.subscribe(packetiserTask.getBatteryQueue());
```

Start both tasks before `eegAcquisitionTask.start()`.

### `PacketiserTask`

Give it the same pieces the EEG queue already has:

- `_imuQueue` for `ImuSample`, and `getImuQueue()`
- `_batteryQueue` for `BatterySample`, and `getBatteryQueue()`
- `setOwner(this)` on both, with the existing queues

Leave the ML queue as it is.

When a sample is waiting, build one `WireFrame` and send it on, as with EEG. Do not apply the EEG downsampling to these samples.

Add `serialiseImu()` and `serialiseBattery()` next to `serialiseEeg()`. Each one advances the frame counter and sets the destination the same way. The battery frame type is `IES_SAMPLE_BATT_INFO`. The IMU frame type is `IES_SAMPLE_NECK_IMU` or `IES_SAMPLE_EAR_IMU`, not chosen yet. The payload bytes of both frames are not chosen yet.

### `config.h`

Add `FIFO_DEPTH_IMU_QUEUE` and `FIFO_DEPTH_BATTERY_QUEUE` next to the other queue depths, and `STACK_SIZE_IMU` and `STACK_SIZE_BATTERY` next to the other stack sizes. Include all four in the RAM totals.

EEG already passes through the steps below. IMU and battery are added onto these steps. Gateway, UART, and BLE stay as they are.

**1. The EEG task sends each sample to the packetiser.**

```cpp
eegAcquisitionTask.subscribe(packetiserTask.getEegQueue());
```

`getEegQueue()` is `_eegQueue`.

**2. The packetiser turns one waiting item into one frame.** `PacketiserTask::run()` checks these in order:

1. TIME_SYNC, when it is due.
2. A command response, from `_responseQueue`. Then `continue`.
3. An EEG sample, from `_eegQueue`. Then `continue`.
4. An ML result, from `_mlQueue`. Then `continue`.
5. If nothing was waiting, `sleepUntilNotified()`.

Step 3 calls `serialiseEeg()`. That function adds one to `_frameCnt` and sets `f.dest = activeStreamDest()`. Downsampling, `getDownsamplingFactor()` and `_sampleCount`, is used only in step 3.

Step 3 ends with `continue`, so the loop starts over. Code written after that `continue` does not run while `_eegQueue` still has samples. The ML pop in step 4 is already in that spot.

The packetiser already has `_responseQueue`, `_mlQueue`, `getEegQueue()`, and `setOwner(this)` on those queues.

**3. The packetiser sends each frame to the gateway.**

```cpp
packetiserTask.subscribe(gatewayTask.getDataQueue());
```

`getDataQueue()` is `_dataQueue`.

**4. The gateway sends the frame to UART or BLE.**

```cpp
gatewayTask.setUartChannel(uartChannelTask.getTxQueue());  // stored as _uartTxQueue
gatewayTask.setBleChannel(bleChannelTask.getTxQueue());    // stored as _bleTxQueue
```

`routeWireFrame()` looks at `frame.dest` and pushes to one of those two queues.

**Also already there**

- `config.h` section 1 lists queue depths. `FIFO_DEPTH_ML_QUEUE` is the line the new depths go next to.
- Section 3 lists stack sizes.
- Section 12 adds them up in `RAM_USAGE_FIFOS` and `RAM_USAGE_STACKS`.
- `setup()` starts `eegAcquisitionTask` last.
- `nicla::enableCharging(300)` in `setup()` turns charging on. It does not read the battery level.
- There is no IMU read. The LIS3DH methods were removed from `ADS1299_Library`.
- `_streamingEnabled` means EEG streaming is on. It is not an IMU on/off switch.

## What to add

### 1. Two new tasks

- [ ] Add `ImuTask`, the global `imuTask`, in `imu.h` and `imu.cpp`. The sample type is `ImuSample`.
- [ ] Add `BatteryTask`, the global `batteryTask`, in `battery.h` and `battery.cpp`. The sample type is `BatterySample`.
- [ ] Declare each with `extern` in the header, and define the global in the `.cpp`, the same way as `eegAcquisitionTask`.
- [ ] Make both a `ProducerTask`. Give them a priority below `EegAcquisitionTask` (`osPriorityRealtime`).
- [ ] `run()` loops on `while (!_stopRequested)`. When the task is idle it calls `sleepUntilNotified()`. The time between samples is a second sleep, separate from that idle wait.

**Inside `ImuTask::run()`.** Read the sensor, then wait out the sample period. The rate is not chosen yet.

- [ ] Before sending a sample, read an enable flag. This flag is not `_streamingEnabled`.
- [ ] When the flag is on, `distribute()` each sample. When it is off, do not `push()` the sample.

**Inside `BatteryTask::run()`.** Read the battery level here. Do not read it inside `PacketiserTask::run()`. Leave `nicla::enableCharging(300)` in `setup()`. After the read, check `g_runtimeState.isOpenVibeMode()`.

- [ ] Modern mode: `push()` when the level is low. The threshold is not chosen yet. If the next loops still see a low level, do not `push()` again.
- [ ] Modern mode, when the host asks for the level: no command name has been chosen yet.
- [ ] OpenVIBE mode: one block that `push()`es one sample every 10 seconds. That block does not check streaming and does not check the low level. It can be commented out.

### 2. Connect the tasks in `src.ino`

- [ ] Include `imu.h` and `battery.h` next to `#include "eeg.h"`.
- [ ] Under the EEG subscribe line, add:

```cpp
imuTask.subscribe(packetiserTask.getImuQueue());
batteryTask.subscribe(packetiserTask.getBatteryQueue());
```

- [ ] Call `imuTask.start()` and `batteryTask.start()` before `eegAcquisitionTask.start()`.

### 3. Inside `PacketiserTask`

Add the queues next to `_eegQueue`:

- [ ] `_imuQueue`, a `FifoQueue<ImuSample, FIFO_DEPTH_IMU_QUEUE>`.
- [ ] `_batteryQueue`, a `FifoQueue<BatterySample, FIFO_DEPTH_BATTERY_QUEUE>`.
- [ ] `getImuQueue()` and `getBatteryQueue()`, next to `getEegQueue()`.
- [ ] `setOwner(this)` on both new queues, next to `_eegQueue.setOwner(this)`.
- [ ] Leave `_mlQueue` as `FifoQueue<MLOutput, FIFO_DEPTH_ML_QUEUE>`.

In `run()`, take IMU and battery samples out before the EEG step's `continue`:

- [ ] Pop `_imuQueue` and `_batteryQueue` above that `continue`.
- [ ] Turn each sample into one `WireFrame`, then call `distribute(frame)`.
- [ ] Do not use `getDownsamplingFactor()` or `_sampleCount` here. Those stay in the EEG step.

Add the two serialisers next to `serialiseEeg()`:

- [ ] `serialiseImu()` and `serialiseBattery()`.
- [ ] Each one adds one to `_frameCnt` and sets `f.dest = activeStreamDest()`.
- [ ] Battery type byte: `IES_SAMPLE_BATT_INFO`.
- [ ] IMU type byte: `IES_SAMPLE_NECK_IMU` or `IES_SAMPLE_EAR_IMU`. Not chosen yet.
- [ ] The payload bytes of both frames are not chosen yet.

### 4. Numbers in `config.h`

- [ ] In section 1, next to `FIFO_DEPTH_ML_QUEUE`, add `FIFO_DEPTH_IMU_QUEUE` and `FIFO_DEPTH_BATTERY_QUEUE`.
- [ ] Add both to `RAM_USAGE_FIFOS`. Each term is the depth times the size of one sample.
- [ ] In section 3, add `STACK_SIZE_IMU` and `STACK_SIZE_BATTERY`.
- [ ] Add both stack sizes to `RAM_USAGE_STACKS`.
