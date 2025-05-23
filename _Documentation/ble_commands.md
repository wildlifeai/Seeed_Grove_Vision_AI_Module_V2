# WW500 BLE commands
#### CGP 19/3/25

When a smartphone app is connected to the WW500, text sent from the app to the WW500 is parsed,
and if the text represents known commands then the text is passed to appropriate functions for action. 
Responses are returned.

* Some commands (those beginning `AI `) are treated as commands for the AI processor, and passed on to 
the AI  processor (the HX6538 chip) for parsing and execution. In that case, responses are
transferred from the AI processor to the BLE processor before being retunred to the app.
* Commands not beginning `AI ` are treated as commands for the BLE processor (the MKL62BA module).
In that case, responses are generated by the BLE processor and returned to the app.
* Commands for the AI processor are processed by the command line interface (CLI) so the same commands
can be generated by the app or by typing at the console. The BLE processor does not have a CLI. 
 
This document describes these commands. Some commands are now redundant and can be removed. There is 
scope for adding additional commands.

Of course, it is possible to change the command syntax and responses - developers should request that.

In the table below the "Reqd?" column indicates whether the command show be implemented by the app, or notes below.
(Note that a 'Y' does not mean making the feature available to a user, but that it may be of value to
the app internal workings.)

| Command        | Parameter(s)  | Response                                      | Reqd? |
|----------------|---------------|-----------------------------------------------|-------|
| (unrecognised) |               | Err: unregognised command                     |  -    |
| id             |               | Device's BLE name                             | Y     |
| ver            |               | Device type, s/w versio, build date           | Y     |
| ping           |               | Response from attempted LoRaWAN ping          | Y     |
| enable         |               | "Sensor messages are enabled"                 | Y     |
| disable        |               | "Sensor messages are disabled"                | Y     |
| status         |               | Some status info                              | Y     |
| battery        |               | "Battery = nn%"                               | Y     |
| device         |               | Hardware type e.g. "WW500.A00"                | Y     |
| heatbeat       |               | Reports LoRaWAN heartbeat period e.g. "4h"    | y     |
| set heartbeat  | 30m, 2h etc   | "heartbeat is 22m" etc                        | Y     |
| get deveui     |               | 8-byte EUI                                    | Y     |
| get appeui     |               | 8-byte EUI                                    | Y     |
| get appkey     |               | "failed 2"                                    | 1     |
| deveui         | 8-byte EUI    |                                               | 2     |
| appeui         | 8-byte EUI    |                                               | 2     |
| appkey         | 16-byte key   |                                               | 2     |
| reset          |               | "device will reset after disconnecting"       | 2     |
| erase          |               | "device will erase after disconnecting"       | 2     |
| dis            |               | Disconnect BLE session                        | Y     |
| dfu            |               | Places device in DFU mode for s/w upgrade     | Y     |
| gpio           | GPIO pin      | Reports whether the GPIO pin is driven        | 2     |
| set gpio       | GPIO pin      | Sets a GPIO pin high                          | 2     |
| clr gpio       | GPIO pin      | Sets a GPIO pin low                           | 2     |
| get gpio       | GPIO pin      | Reads the state of a GPIO pin                 | 2     |
| send           | A delay       | Sets a delay between I2C tx and Rx            | 3     |
| wake           | A delay       | Pulses a pin to wake the HX6538 from DPD      | 3     |
| utc            | UTC string    | Sets BLE processor UTC time                   | 4     |
| AI status      |               | Reports some status (t.b.d.)                  | 5     |
| AI enable      |               | Enables something on AI processor             | 5     |
| AI disable     |               | Disabled something on AI processor            | 5     |
| AI capture     | nn mm         | Trigger capture on nn iamges at mm intervals  | 2     |
| AI utc         | UTC string    | Sets device UTC time                          | 6     |
| AT gps         | GPS string    | "Device GPS set"                              | Y, 7  |

Note that there are addition commands that could be run on the AI processor, not documented here.
These can be seen by typing "help" at the console. 

__Notes:__
1. The API does not premit reading the appkey
2. Not needed by app but used for development and testing purposes, accessed by nRFToolbox.
Perhaps I will remove these before shipping production units. 
3. Proably no longer needed and will be removed
4. Probably to be replaced by "AI utc"
5. Probably tto be given a real purpose in the future.
6. Not yet implemented - see below.
7. See below



## Setting UTC time

I think I know what to do here but have not yet implemented it. The AI processor needs the UTC for timestamping
images in EXIF data. I believe it has a calendar/clock feature (I need to confirm this). I think I can operate it as follows:
* The BLE processor has pretty accurate calendar clock (c. 20ppm)
* The AI processor has a less accurate calendar clock (accuracy uncertain but good enough to operate over minutes or hours without
noticable drift).
* The phone can get accurate UTC from the network and pass it to the WW500 when it connects.
* The LoRaWAN system can get accurate UTC from the network whenever it pings the network - say every 15 minutes, 1 hour etc.
* As a worst case the BLE processor will retain a reasonbly accurate time over weeks. It can re-sync the time from the LoRaWAN
network (if present). In either event it can pass this time to the AI processor every few minutes.
* The AI processor thus has a reasonably accurate time for purposes of timestamping images.
* From the app's point of view, it should send a string (format below) whenever it connects.
* The BLE processor takes the UTC time from either network source, updates its own time, and passes it to the AI processor. This
reamins to be coded, but I think it is straight-forward.
* The APP is to send time as string like this: `AI utc 2025-03-18T20:51:28Z` and I will deal with the rest of it.
* I will provide a function for creating the timestamp in a format suitable for inclusion in EXIF. I understand that 
EXIF uses an ASCII string like "YYYY:MM:DD HH:MM:SS\0"

In the event that the app has not provided the device with its time, we should probably not try to add a timestamp
 in the EXIF. I should probably add a function like `exif_utc_has_time()` that returns a boolean.

## Setting GPS location 

The location is required for EXIF metadata, and the format is somewhat complex - stored as a binary
representation in degrees, minutes and seconds (for latitude and longitude) and meters above or below sea level
for altitude.

I have already coded and tested this (to some extent), and it works from nRFToolbox.

What is required is for the app to send a GPS location in the right format. A string representation looks like this:
`"37°48'30.50\" N 122°25'10.22\" W 500.75 Above"` - note the enclosing quotes, the internal escaped double-quotes,
 the degrees characters and the spaces.
In our case the spaces need to be replaced (by underscore) so that this is treated as a single string, like this:
`"37°48'30.50\"_N_122°25'10.22\"_W_500.75_Above"`

Thus the app needs to send a command like this:

`AI gps "37°48'30.50\"_N_122°25'10.22\"_W_500.75_Above"`

In the event that the app has not provided the device with its location, we should probably not try to add a GPS location in the EXIF. 
I should probably add a function like `exif_gps_has_location()` that returns a boolean.

## App Behaviour on Connection

I think that the app should send a number of configuration messages as soon as it connects, and do this every time
(probably not just while configuring). These should include:
* Requests device ID, model, name, etc etc.
* Send UTC, provided that the phone has the time.
* Send GPS location, provided that the phone has this.
* Requests status, including battery voltage.