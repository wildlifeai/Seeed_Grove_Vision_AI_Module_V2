# Speed up Transfer Rate over FTDI TTL-232R-3V3 cable
#### CGP - 23/1/26

It is possible to get substantially faster data transfer rates - especially useful when using XMODEM to download the `output.img` file.

On Windows do this:

1. 	Open Device Manager
2. 	Locate the FTDI cables under `Ports (COM & LPT)`
3.	Open these one at a time and navigate to Port Settings > Advanced > Latency Timer (msec)
4.	Change this to 1ms
5.	Exit (click OK several times)