# NeuralWear - An open-source wearable platform to enhance human cognitive functions and well-being
This repo contains the NeuralWear project source code and hardware designs. Some development documents are also available if you want to dive deeper into the firmware development.

# Compiling and Flashing the NeuralWear1.0 firmware
**1. Getting the Arduino NICLA Voice framework**

NeuralWear 1.0 uses the Arduino NICLA Voice as the main controller, which is a tiny, ultra-low-power development board featuring an integrated microphone, 6-axis IMU, and a Syntiant NDP120 Neural Decision Processor for always-on, edge-based time-series signals (EEG/EOG/EMG) processing. Download the [Arduino IDE](https://docs.arduino.cc/software/ide/) and install the Arduino Mbed OS NICLA Boards from its board manager. 
Once the Arduino IDE and Mbed OS NICLA Boards package have been installed, open [src.ino](firmware/src/src.ino) with the Arduino IDE, compile and upload the project to the NeuralWear hardware.

**2. (Optional) Setting up VSCode and PlatformIO**

While it is totally fine to use Arduino IDE to develop and compile the code, it is much more convenient to use VSCode and PlatformIO. Follow the instructions on [PlatformIO](https://platformio.org/platformio-ide) to install the necessary packages. The PlatformIO configuration for working with our NeuralWear1.0 is provided in [platformio.ini](firmware/platformio.ini). Thus, open PlatformIO inside VSCode and point to the firmware folder to start development. You can compile, flash, and even debug the firmware right within PlatformIO without going back to Arduino IDE.

Debugging is a BIG bonus with PlatformIO. NeuralWear1.0 provides an on-board CMSIS-DAP debugger; thus, there is no need for external debugging tools, such as JLink. The debugging function should work out-of-the-box.

# Real-time data streaming with OpenVibe

**1. Overview**

<img width="467" height="296" alt="image" src="https://github.com/user-attachments/assets/4d21a98e-7bd5-421b-b5a5-36f159782b06" />

*Fig.1: Overview of the NeuralWear hardware and a control computer*

Figure 1 presents an overview of our NeuralWear devices, which consist of (1) a control computer, (2) a NeuralWear biosignals acquisition device, (3) a pair of Behind-the-ears (BTE) silicone earpieces, and (4) pre-gelled disposable snap-on electrodes. The Control Computer hosts software called OpenVibe, which provides the ability to stream biosignals and motion data from the NeuralWear device through Bluetooth, visualise the captured signals in real-time, and store data for later processing. The NeuralWear biosignals acquisition device provides the ability to capture biosignals and motion data and forward them to the Control Computer. It can also measure the current battery percentage and charge the battery with an internal charging circuit. The BTE silicone earpieces hold active electrode circuits that are covered in soft silicone, together with 4 snap-on connectors providing the connection to electrodes. The Philips SmartSleep pre-gelled disposable electrodes [*] are used together with our earpieces. These electrodes are chosen because of their curved shape to fit behind the subject's ears. However, other pre-gelled snap-on electrodes such as [**] can be used as alternatives as well.

[*] https://www.usa.philips.com/c-p/HH1601_01/smartsleep-deep-sleep-headband-30-replacement-sensors 

[**] https://bio-medical.com/covidien-kendall-disposable-surface-emg-ecg-ekg-electrodes-1-24mm-50pkg.html 

**2. The control computer**

<img width="468" height="312" alt="image" src="https://github.com/user-attachments/assets/4ca8874d-a200-4c6b-b258-9f0bf2ea0d4b" />

*Fig.2: The control computer*

Any Windows laptop with a Bluetooth 2.0 connection can act as a control computer. Download the provided OpenVIBE_with_dlls.zip file and extract it to the computer. On the Control Computer, we have some important files and folders:
1.	Openvibe-acquisition-server: This software creates the Bluetooth connection to the NeuralWear device and streams data from the device to the Computer.
2.	Openvibe-designer: This software runs the data visualisation and collection processing to plot the captured signal in real-time and store it to files.
3.	Earable_data_visualization_collection.xml: This file is used together with OpenVibe-designer for data visualisation and collection.
4.	Data collection folder: We can store all the collected study data in this folder.

**3. Setting up the earpieces and electrodes**

To set up the earpieces and electrodes on the subject, we have the following steps:
1.  Skin preparation. Thoroughly cleaning the skin behind the ears with alcohol will help to provide good signal quality during the data collection process.
2.  Attaching the electrodes. Attach 2 pre-gelled electrodes to each ear (i.e. 4 electrodes in total for 2 ears), with one in the upper area (the squamous process) behind the ear, and the other one in the lower area (the mastoid process). Depending on the size of the earpieces, the distance between the centres of the 2 electrodes is about 1-1.5 inches. Avoid putting the electrodes on the hair so they can make good contact with the skin. Figure 4 presents an example of attaching the electrodes behind the ear.
3.  Attaching the silicone earpieces. Attach the earpieces to the BTE electrodes as shown in Figure 5. We can adjust the locations of the electrodes and the silicone earpieces so that no pressure is put against the earlobe, which could cause discomfort during a long duration.
4.  Checking the contacts. We can visually check the contact of the electrodes to make sure that they adhere well to the skin (without hair in the middle) and no discomfort is created for the subject.

<img width="222" height="279" alt="image" src="https://github.com/user-attachments/assets/ef2d56f2-6ff1-44e9-91ab-d9ff4b026307" /> <img width="219" height="279" alt="image" src="https://github.com/user-attachments/assets/508ed368-0bd5-426d-ad2d-5764160b1e6f" />

*Fig.3: Attaching electrodes and the silicone earpieces to the ears*

**4. Start the data visualisation and collection.**
Turn on the NeuralWear device. We need to make sure that the battery connected to the device is fully charged before running the study. A fully charged battery can last for 8-10 hours of continuous data streaming. Turn on the device by sliding the Power Switch to the On position. The Power LED (Green) will light up, and the Bluetooth connection LED will blink red. Connect the Control Computer to the device. To connect the Computer to the device, we use the OpenVibe-acquisition-server software. Open it, and you will see the following screen:

<img width="468" height="218" alt="image" src="https://github.com/user-attachments/assets/326f6002-aef7-421a-9e8c-c0b89c254d56" />

*Fig.4: OpenVibe-acquisition-server*

On the Driver box, we choose the iES driver. Then, to choose the device to connect to, click on the Driver Properties box. We will have the following screen:

<img width="195" height="317" alt="image" src="https://github.com/user-attachments/assets/f20dde18-d1aa-4bac-9d24-83a8a8c2bcc2" />

*Fig.5: Driver Properties box*

Click on the Device box and choose the right COM port of the device (each device will have a sticker with a COM port to connect to). Click Apply to close the window. In the main window of the OpenVibe-acquisition-server (Figure 4), click Connect to connect to the NeuralWear device. If the connection is successful, the Play and Stop buttons will be available. If the connection is not successful after a few tries, we can use the Reset button on the NeuralWear device to reset it and try again. After the connection is successful, click on the Play button to start streaming data from the NeuralWear device to the Computer. The window will show that data is being streamed from the device, as shown in Figure 6.

<img width="468" height="151" alt="image" src="https://github.com/user-attachments/assets/839b3621-30a0-4d13-b8b9-0825d0f0c01e" />

*Fig.6: Data is being streamed from the device to the computer*

**5. Data Visualisation and Collection.**

Run the OpenVibe-Designer software and open the data_visualization_collection.xml file. We will have the following window:

<img width="468" height="378" alt="image" src="https://github.com/user-attachments/assets/990f51a6-dd0c-4d14-8cd3-6741db85bd30" />

*Fig.7: OpenVibe-Designer window*

Click on the Play button of the OpenVibe-Designer software to start data visualisation and collection. It will open another window like this:

<img width="468" height="244" alt="image" src="https://github.com/user-attachments/assets/5dbfff36-f59c-406a-a6bd-5b45d3a991e3" />

*Fig.8: Real-time data streaming*

On the left side, we have 3 boxes to visualise captured EOG (filtered from 0.3 to 10Hz), EMG (filtered from 10 to 100Hz), and EEG (filtered from 0.3 to 25Hz), respectively. On the right side, we have the Motion box to visualise data from the motion sensor and a battery (%) box which shows the current battery percentage of the NeuralWear device.
Data collection. While the visualisation is running, the data is being collected and written to files inside the DataCollection folder as well. Open the DataCollection folder; we will see that the collected data is stored in files with the name record-[date-start time].csv. Figure 9 presents an example of the collected data inside the DataCollection folder.

<img width="468" height="165" alt="image" src="https://github.com/user-attachments/assets/3afbd1c3-0cfd-45b3-ba3c-595aabfb9d1b" />

*Fig.9: Collected Data files inside the DataCollection folder*

**6.	Checking the signal quality**

Before leaving the data collection to run for a long duration, some data quality checks are needed to make sure that we are receiving good signal quality.

*Baseline test:* In this test, we ask the subject to stay still and do nothing for about 5s; the EEG/EOG/EMG signals should stay stable as in the following figure.

<img width="276" height="244" alt="image" src="https://github.com/user-attachments/assets/1fe14031-af44-4a96-9929-5b4ec001895a" />

*Fig.10: Baseline test.*

*Eye blink test:* In this test, we ask the subject to blink a few times (3-5 times) while staying still. The peaks of eye blinks could be observed in channel 1 of the EOG box.

<img width="468" height="172" alt="image" src="https://github.com/user-attachments/assets/dc8e3e6c-30af-405e-b2d2-c0a34f8d7242" />

*Fig.11: Eye blink test.*

*Eye Movements test:* In this test, we ask the subject to move their eyes to the left and right a few times (3-5 times) while staying still. The eye movement signal could be observed in channel 2 of the EOG box.

<img width="468" height="168" alt="image" src="https://github.com/user-attachments/assets/129de239-2789-4d51-9af3-765f50be3abe" />

*Fig.12: Eye movements test.*

*EMG test:* In this test, we ask the subject to grind their teeth strongly a few times (2-3 times). The EMG signals could be observed in both 2 channels of the EMG box.

<img width="468" height="168" alt="image" src="https://github.com/user-attachments/assets/fdca4033-72f4-448f-82e5-e2905abac962" />

*Fig.13: EMG test.*

**7.  Data collection.**

At this point, we are ready for long-term data collection. We just need to leave the visualisation window open on the Control Computer. The data will be stored in a file inside the DataCollection folder until we close the visualisation window.
During the data collection, we should keep track of the battery percentage of the device. When the battery percentage is <10%, we should (1) close the visualization window to save the collected data, (2) disconnect the OpenVibe-acquisition-sever from the Bluetooth of the NeuralWear device, (3) turn off the device, (4) swap the current battery with a fully-charged one, and (5) repeat step the previous steps (we don’t need to open another OpenVibe-acquisition-sever, just click Connect on the current one.).


















