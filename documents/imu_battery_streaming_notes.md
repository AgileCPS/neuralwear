# IMU and battery streaming — temporary design notes

## Data needs

### Battery level

**OpenVIBE mode.** The device sends one battery frame every 10 seconds, no matter what. This send can be commented out if it is incompatible with the host.

**Modern mode (NOT NOW).** The device sends the battery level only on request, or when the battery is going low.

OpenVIBE mode is the priority now. Modern mode will be implemented later.

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

`BatteryTask` (`battery.h`, `battery.cpp`) produces `BatterySample`. Read the level in this task. For now, implement OpenVIBE mode only: send one sample every 10 seconds with no other checks. That send can be commented out. Modern mode, on request and when the level goes low, comes later. The low threshold is not chosen yet, and no command is named for the request.

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
