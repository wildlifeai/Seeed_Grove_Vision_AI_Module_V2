# Experiments with Sensecraft
#### CGP - 28 November 2025

## Top-Level problem Statement

It would be very helpful if we could get a live stream of video from the WW500 onto a laptop, so that we could review:
* Image quality in general
* In particular to correct auto-exposure for the RP camera
* Effect of LED flash in low-light conditions (esp. is the flash on at the right time).
* Maybe we could extend the output to show the HM0360 motion detection in operation (as a 16x16 array of cells,
each of which may have detected motion). This could help with MD sensitivity.

The standard Seeed Grove Vision AI Module V2 supports live video feeds, in the context of:

* Standard pre-programmed firmware image.
* A web-browser environment that is coupled with model downloads and other features.

Can we get our own board to stream video? Can we display that in the Seeed browser or do we need a different web page?

__Seeed Documentation__ The Seeed Studio pages on the [Grove Vision AI Module V2
](https://wiki.seeedstudio.com/grove_vision_ai_v2_software_support/#-no-code-getting-started-with-sensecraft-ai-) says how this should work.

__New Information__ As I was editing this I came across an independent web page that covers this material 
[Machine Learning Systems website](https://www.mlsysbook.ai/contents/labs/seeed/grove_vision_ai_v2/setup_and_no_code_apps/setup_and_no_code_apps.html)

__Edge-Impulse-model-on-Himax-AI__ See [Deploy custom trained Edge Impulse models on Himax-AI web toolkit](https://github.com/HimaxWiseEyePlus/Edge-Impulse-model-on-Himax-AI?tab=readme-ov-file)

This explains that live video can be viewed with three tools (use with __Chrome__ or __Edge__ browsers):

1. __SenseCraft AI Studio__ This is the [SenseCraft website](https://sensecraft.seeed.cc/ai/home)
2. __SenseCraft Web Toolkit__ Available on [github](https://github.com/Seeed-Studio/SenseCraft-Web-Toolkit)
and already running at [the website](https://seeed-studio.github.io/SenseCraft-Web-Toolkit/#/setup/process) 
This is a simplified version of the SenseCraft AI Studio. The SenseCraft Web Toolkit is based on the Himax AI Web Toolkit.
3. __Himax AI Web Toolkit__ Available as a 
[Himax_AI_web_toolkit.zip](https://github.com/HimaxWiseEyePlus/Edge-Impulse-model-on-Himax-AI/blob/main/Himax_AI_web_toolkit.zip)
 or [Himax_AI_web_toolkit.zip](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/releases/download/v1.1/Himax_AI_web_toolkit.zip)
 Once downloaded and unzipped to the local PC, double-click index.html to run it locally.

Below I explore each of these.

## Programming and Running the Default Image

The [default image file](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/blob/6bd00a3a633ab57ec4f03850ccb7a7f1ccc2edc8/Seeed_SenseCraft_AI_2023_12_19T183401_402.img)
 is provided in the Himax github respository. It is here (on my computer - your path will be different):
[Seeed_SenseCraft_AI_2023_12_19T181301_402.img](D:\Development\wildlife.ai\Seeed_Grove_Vision_AI_Module_V2/Seeed_SenseCraft_AI_2023_12_19T181301_402.img)

#### Program the default image:

- Connect a RP v1 camera to the the Grove Vision AI Module V2 and plug it into a laptop via the USB cable.
- Open Teraterm, select the device and set the baud rate to 921600.
- Hold down a key on the keyboard (not enter).
- Press and release the reset button (black) on the board.
- Select option 1: "Xmodem download and burn FW image"
- On the Teraterm console navigate here: File > Transfer > XMODEM > send
- Browse to the .img file (located as indicated above).
- When the download is complete, type 'y' to run your new image
- Shut down Teraterm (as it will be controlling the serial port needed for the next stages).

#### Run on a the 'SenseCraft AI Studio'

1. Open a __Chrome__ or __Edge__ browser and go to the [SenseCraft AI Studio](https://sensecraft.seeed.cc/ai/#/home)
(I had already set up an account, which is probably mandatory).
2. Navigate to the Workspace tab and select Grove Vision AI V2.
3. In the pane click "Connect" and select your serial port.
4. Hopefully you will see the camera output in the Preview pane and text like the following in the Device Logger pane:
```
perf:{"preprocess":9,"inference":80,"postprocess":0}
boxes:[]

perf:{"preprocess":9,"inference":80,"postprocess":0}
boxes:[[193,86,51,43,74,1]]

perf:{"preprocess":9,"inference":80,"postprocess":0}
boxes:[[193,86,47,42,66,1]]
```
5. You can also click on the Output tab, select a condition, enable the "Light up the LED" button and click send.
Hopefully you will see the second LED on the device light up when the condition is met.

#### Run on a the 'SenseCraft Web Toolkit' at the Website

1. Open a __Chrome__ or __Edge__ browser and go to the [SenseCraft Web Toolkit website](https://seeed-studio.github.io/SenseCraft-Web-Toolkit/#/setup/process) 
2. In the top RH corner select Grove Vision AI V2, click "Connect" and select your serial port.
3. Hopefully you will see the camera output in the Preview pane 
4. Click on one of the icons above the image to show the Device Logger pane.

#### Run on a the 'SenseCraft Web Toolkit' Locally

I seem to have a local copy of [SenseCraft Web Toolkit](https://github.com/Seeed-Studio/SenseCraft-Web-Toolkit) on my laptop.
I guess I installed it according to the [README.md](https://github.com/Seeed-Studio/SenseCraft-Web-Toolkit/blob/main/README.md) instructions.

1. Open a __Chrome__ or __Edge__ browser and go to the index.html file (your location will differ!):
```
D:/Development/wildlife.ai/SenseCraft-Web-Toolkit/index.html
```
 
2. I get a blank screen. I asked Copilot and got [this](https://copilot.microsoft.com/shares/RYL8pkitviUJCDpDHjCqZ) explanation.
3. I may return to this problem later.

#### Run on a the 'Himax Web Toolkit' Locally

1. Downloaded the [Himax AI web toolkit](Himax_AI_web_toolkit.zip) and place it on your laptop.
2. Open a __Chrome__ or __Edge__ browser and go to the index.html file (your location will differ!):
```
D:\Development\Himax_AI_web_toolkit\index.html
```
3. In the top RH corner select Grove Vision AI V2, click "Connect" and select your serial port.
3. Hopefully you will see the camera output in the Preview pane 
4. Click on one of the icons above the image to show the Device Logger pane.




## Experiments with Sensecraft and Grove Vision AI Module V2

After running the default image I tried some other experiments, following the [Deploy a pretrained Model]](https://sensecraft.seeed.cc/ai/home) instructions. 
Unless otherwise noted this was with the [SenseCraft AI Studio](https://sensecraft.seeed.cc/ai/home)

3. "Step 1: Discover Pretrained Models" - select `Grove Vision AI V2` (not `XIAO ESP32S3 Sense`).
I then selected the Gesture Detection (rock/paper/scissors) example.

4. "Step 2: Deploy and Preview Vision" - click the Deploy button and a window should open showing a serial port to select. Do this.
I then see a "Flashing" progress indication that must be downloading something (model only? Or main software also?). This took a while.
Then live video data appears in the "preview" pane on the RHS. Superimposed on this are bounding boxes that may include "Rock", "Paper" and Scissors" boxes. Note I do NOT see the console output that is shown in the web screen shots.

5. I scrolled down to "Workspace" and clicked "Explore Now". That took me to
[https://sensecraft.seeed.cc/ai/device/local/36](https://sensecraft.seeed.cc/ai/device/local/36).
I clicked on Select, then selected the serial port. Then I got not only the live image with "Rock", "Paper, "Scissors" boxes,
but also the "Device Logger" pane. This webpage is the `SenseCraft AI Studio` page referenced above.   

6. I disconnected the Grove Vision AI V2 on the `SenseCraft AI Studio` web page and re-connected in the `Himax AI Web Toolkit` webpage.
The live video and device logger worked bu the bounding boxes showed labels like "bicycle" and "car".

## Other Models

Other models are available and maybe these should be tried. I tried the Apple detection model which seemed to work.

## How is this working?

I imagine there is code running on the Grove Vision AI Module V2 that includes the gesture recognition model.

I imagine this is streaming data down the serial port and that this data includes a sequence of JPEG images 
plus metadata relating to the NN model output.

I imagine there is code running in the Chrome browser that parses the serial port stream and paints the screen appropriately.

I [asked ChatGPT](https://chatgpt.com/c/687d985f-31ac-8005-86af-9961de2a8dc6) some months ago and got answers that seemed
a bit vague. However in August I asked supplementary questions and got further information, including the advice to clone 
a `SenseCraft-Web-Toolkit` repository:

```
git clone https://github.com/Seeed-Studio/SenseCraft-Web-Toolkit.git
cd SenseCraft-Web-Toolkit
```
This code is [here](https://github.com/Seeed-Studio/SenseCraft-Web-Toolkit)

However a quick look at the ChatGPT advice did not seem to match the code I had downloaded - but I did not spend much time on this.

It might be worthwhile downloading that repository (or perhaps the Himax AI Toolkit, since that actually runs!), then asking Claude to analyse it and derive documentation for 
the serial protocol. Objectives:

1.	Understand the protocol so we can be certain that we have found the right C code to generate it (See below).
2.	Understand how to display the live data in a browser.
3.	Understand how we might extend the protocol, for example to send settings back to the WW500 that might 
adjust autoexposure registers, or flash LED timing settings.

Amongst other things the repository includes:

* [SenseCraft Model Assistant Overview](https://sensecraftma.seeed.cc/introduction/quick_start#quick-start)
* [SenseCraft Model Assistant](https://github.com/Seeed-Studio/ModelAssistant)

which might be useful tools in their own right. (Would these replace Edge Impulse?)

## C source code

I suspect that the [Himax repository](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2)
does not include the source of the default Grove Vision AI Module V2 app.

It does include many [Scenario app examples](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/tree/main/EPII_CM55M_APP_S/app/scenario_app)
 that can be selected to run on the Grove Vision AI Module V2. Do some of these send output that can be
 displayed on the browser?
 
 A search for `send_bytes` shows several apps have a `send_results.cpp` file that sends data in JSON form over the UART.
 In these there seem to be calls to `event_reply()` which sends JPEG data.
 
 Is this the code we should be adding to our WW500 apps to send jpeg data by UART?
 
 Again, maybe Claude could be given access to this repository and asked if the C code produces data streams that 
 are accepted by the Sensecraft code.
 
 #### Clues
 
 Although the text in the JSON example above does not appear in a text search of the scenarios_app examples, there are some clues that the 
 C source code generates a serial steam that the Chrome browser will expect.
 
 If you connect the board to a terminal emulator (e.g. Teraterm) and reset the board this outut appears:
 
 ```
 {"type": 0, "name": "INIT@STAT?", "code": 0, "data": {"boot_count": 0, "is_ready": 1}}
 ```
 
 Although `INIT@STAT` and `boot_count` is not in the source code, there are functions that will produce
 JSON with the same pattern, such as this:
 
 ```
     const auto& ss_model_info{concat_strings("\r{\"type\": 0, \"name\": \"",
                                  "MODEL?",
                                  "\", \"code\": ",
                                  std::to_string(EL_OK),
                                  ", \"data\": ",
                                  model_info_2_json_str(model_info),
                                  "}\n")};
    send_bytes(ss_model_info.c_str(), ss_model_info.size())
    
    
```

Also, the "SSCMA" name appears here:

```
char info[CONFIG_SSCMA_CMD_MAX_LENGTH]{};
```    
I looked at the `tflm_yolov8_od` scenario app. This has a [README.md](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/blob/main/EPII_CM55M_APP_S/app/scenario_app/tflm_yolov8_od/README.md) file 
that talks about connecting to the `Himax AI web toolkit` which is the same web interface that has worked successfully for us above.

So onwards:

EPII_CM55M_APP_S

## Running `tflm_yolov8_od` scenario app

I started with a fresh installation of the [Himax repository](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2)
and once I had got it building OK I selected the `tflm_yolov8_od` scenario app, built it and downloaded it 
to the Grove Vision AI V2.

Immediately the XMODEM download completed and I started the new firmware (typed 'y') I got this in Teraterm:

```
<snip>
YOLOv8n object detection
TA[3401f000]
Ethos-U55 device initialised
[ERROR] yolov8n_ob_model's schema version -1 is not equal to supported version 3
cis_OV5647_init
mclk DIV3, xshutdown_pin=82
Set PA1(AON_GPIO1) to High
hx_drv_cis_set_slaveID(0x36)
OV5647 Init Stream by app
WD1[34126464], WD2_J[34126464], WD3_RAW[3412af64], JPAuto[34126400]
MIPI TX CLK: 96M
MIPI CSI Init Enable
MIPI TX CLK: 96M
MIPI BITRATE 1LANE: 440M
MIPI DATA LANE: 2
MIPI PIXEL DEPTH: 10
MIPI LINE LENGTH: 640
MIPI FRAME LENGTH: 480
MIPI CONTINUOUSOUT: 1
MIPI DESKEW: 0
t_input: 7387ns
t_output: 6666ns
t_preload: 385ns
MIPI RX FIFO FILL: 10
MIPI TX FIFO FILL: 0
RESET MIPI CSI RX/TX
VMUTE: 0x00000000
0x53061000: 0x0606070D
0x53061004: 0x2B07201C
0x53061008: 0x00000505
0x5306100C: 0x00005500
0x53061010: 0x00000000
sensor_type: 15
Event loop init done
OV5647 on by app done
g_cursensorstream: 0
Event loop start
SENSORDPLIB_STATUS_XDMA_FRAME_READY 1
{"type": 0, "name": "NAME?", "code": 0, "data": "kris Grove Vision AI (WE2)"}
{"type": 0, "name": "VER?", "code": 0, "data": {"software": "Fri Nov 28 14:56:41 2025", "hardware": "kris 2024"}}
{"type": 0, "name": "ID?", "code": 0, "data": 1}
{"type": 0, "name": "INFO?", "code": 0, "data": {"crc16_maxim": 65535, "info": ""}}
{"type": 0, "name": "MODEL?", "code": 0, "data": {"id": 0, "type": 0, "address": 4294967295, "size": 4294967295}}
buffer_size: 2589 may be too small reallocating

if still fail, please modify linker script heap size

{"type": 1, "name": "INVOKE", "code": 0, "data": {"count": 0, "algo_tick": [[1]], "boxes": [], 
"image": "/9j/4AAQSkZJ
<snip>
AAAAAAAAAAAAAAAAAAAAAAA="}}
SENSORDPLIB_STATUS_XDMA_FRAME_READY 2
{"type": 0, "name": "NAME?", "code": 0, "data": "kris Grove Vision AI (WE2)"}
{"type": 0, "name": "VER?", "code": 0, "data": {"software": "Fri Nov 28 14:56:41 2025", "hardware": "kris 2024"}}
{"type": 0, "name": "ID?", "code": 0, "data": 1}
{"type": 0, "name": "INFO?", "code": 0, "data": {"crc16_maxim": 65535, "info": ""}}
{"type": 0, "name": "MODEL?", "code": 0, "data": {"id": 0, "type": 0, "address": 4294967295, "size": 4294967295}}
{"type": 1, "name": "INVOKE", "code": 0, "data": {"count": 0, "algo_tick": [[22140]], "boxes": [], 
"image": "/9j/4A
<snip>
SENSORDPLIB_STATUS_XDMA_FRAME_READY 73
{"type": 0, "name": "NAME?", "code": 0, "data": "kris Grove Vision AI (WE2)"}
{"type": 0, "name": "VER?", "code": 0, "data": {"software": "Fri Nov 28 14:56:41 2025", "hardware": "kris 2024"}}
{"type": 0, "name": "ID?", "code": 0, "data": 1}
{"type": 0, "name": "INFO?", "code": 0, "data": {"crc16_maxim": 65535, "info": ""}}
{"type": 0, "name": "MODEL?", "code": 0, "data": {"id": 0, "type": 0, "address": 4294967295, "size": 4294967295}}
{"type": 1, "name": "INVOKE", "code": 0, "data": {"count": 0, "algo_tick": [[22132]], "boxes": [], 
"image": "/9j/4AAQSkZJRgABAQEASABIAAD/
```

I closed Teraterm and connected via the `Himax Web Toolkit` (as described above).
I got live video in the web browser, and when I enabled the debug console I got the following repeated many times:

```
algo_tick: [[22140]]
boxes: []
```
(The number changes but was always 221nn). 

I tried the same software with the `SenseCraft Web Toolkit` and the `SenseCraft AI Studio` (both documented above).
Both showed live video but the console showed only this, repeated:
```
boxes: []
```
 
#### Adding the Gesture Recognition model
 
I assume that the previous experiemnt had no NN model installed.
 
I used the `SenseCraft AI Studio` to download the rock/paper/scissors model. This behaved as follows:

__SenseCraft AI Studio__ and __SenseCraft Web Toolkit__
Live video, bounding boxes with "Rock", "Paper", "Scissors", and console output like this:
```
perf:{"preprocess":6,"inference":50,"postprocess":0}
boxes:[[159,94,88,76,75,2]]

perf:{"preprocess":6,"inference":50,"postprocess":0}
boxes:[]

perf:{"preprocess":6,"inference":50,"postprocess":0}
boxes:[[111,146,198,174,74,2]]

perf:{"preprocess":6,"inference":50,"postprocess":0}
boxes:[]
```

__Himax AI Web Toolkit__

Live video, no bounding boxes and console output unchanged from above.:

__Teraterm Output__

I disconnected from the web browser and re-connected Teraterm, and reset the Grove Vision AI V2.
I saw only this (apart from the bootloader messages):
```
{"type": 0, "name": "INIT@STAT?", "code": 0, "data": {"boot_count": 0, "is_ready": 1}}
```
So I used the XMODEM download process again to install the same `output.img` file. As before, after 
typing 'y' to start the new image, Teraterm displayed live data.  
 
It looks like the XMODEM route provides insight into the protocol that is not otherwise (easily) available.
 
