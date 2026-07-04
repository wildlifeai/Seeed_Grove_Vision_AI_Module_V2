# Format of .bmp files
#### CGP - 11 March 2026

According to Chat GPT (topic is 640 x 480 mono image): a BMP consists of:

* File header (14 bytes)
* DIB header (40 bytes)
* 256-entry grayscale palette (1024 bytes)
* Pixel data (bottom-up)

| Block | size | Total |
|-------| ----|--------|
| Header | 14 + 40 + 1024 | 1078 bytes |
| Image data | 640 × 480 | 307,200 bytes |
| Total file size | 1078 + 307,200 | 308,278 bytes |

BMP is stored bottom-to-top, so you either:

* Write rows in reverse order
* Or set negative height in DIB header (modern trick)
