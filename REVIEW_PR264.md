# PR #264: Improve speed for file transfers from SD card to app

#### File: `REVIEW_PR264.md`
#### Author: Charles Palmer (PR description), copied offline by Claude
#### Date: 7 October 2026

Offline copy of the description of
[PR #264](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/pull/264), branch
`261006_speedImageTx` into `dev`. One correction: the description pointed to
`BLE_speed_improvements.md` for the BLE processor work. The ready-to-implement brief is
`Fast_SD_to_app_transfer_BLE_processor_work.md`.

---

The speed of file transfers from the SD card to the app, that is, the transfer of JPEG
images, has been investigated.

Some improvements have been made to the AI processor code, and they are included here. They
suppress the console output of the binary data during a transfer, and add a timing summary at
the end of each transfer.

However, the bottleneck appears to be in the BLE processor code and perhaps the app, especially
the speed of the BLE connection. That work is documented here, and is probably ready to be
implemented:

`_Documentation\development reports\2026-10-06_speedImageTx\Fast_SD_to_app_transfer_BLE_processor_work.md`

The same folder, `development reports\2026-10-06_speedImageTx`, contains a record of the work
done.
