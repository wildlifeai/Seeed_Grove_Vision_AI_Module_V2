# Cross-repo contracts

What the mobile app and the nRF firmware depend on here. Never change one of these
unilaterally.

# 6. Cross-repo contracts

EXIF fields (Model, MakerNote), op-parameter indices/defaults, and BLE command syntax are
consumed by ww-mobile-app, ww-website and ww-backend, and mirrored in ww-hardware's
`aiProcessor.h`. Never change one unilaterally: file a `Decide:` issue, agree the
contract, and land the change with the consumers in view.

* **Check the other repos' `dev` branches before asking for a change.** A value's meaning
  can already have moved there: on 8 Oct 2026 op 32 was "fixed" here to mean the normal
  ping, against the app, which already wrote 0 to mean "never join", and was reverted.
* **Op 32 is the LoRaWAN ping period in minutes, 0 = never join** (ww-hardware branch
  `261005_SaveState` reads it; the app writes 720 or 0, ww-mobile-app 8292cd53). The
  released app still writes 0 at every reset, so an nRF that reads op 32 must not ship
  before that app is in production.
* **The motion wake is `Motion <time>`** since #260 (it was `MD <time>`). The nRF knows only
  one name, so this firmware and ww-hardware 0.30.56 release as a pair; the app accepts both
  (ww-mobile-app #421).
* **The wake sequence and `setutc` change together with ww-hardware** (#56, 1 Nov 2026,
  `2026-11-01_AI_RTC/CLAUDE_wake_sequence_spec.md`). The AI processor sends the wake message
  first and holds settings commands until CONFIG.TXT has loaded; the BLE processor sends
  `setutc` every wake after `selftest`, and handles `NN±` in every state. `setutc` replies still
  start `RTC set ` (the BLE processor matches it) but the AI processor writes its RTC only at sleep. The two
  processors' firmware is always updated together.
* **`/IP_INT` is a held-low handshake** (3 Nov 2026, ww-hardware
  `2026-11-01_AI_RTC/IP_INT_handshake_spec.md`): low means a complete message is armed, the AI
  processor releases it when the read completes, and keeps it high at least 20 ms before the next.
  The BLE processor reads on the low. It replaced a pulse read on the rising edge. Update the AI
  processor first, then the BLE processor.
* **Self-test bits are one list in three places**: `selfTest.h` here and in ww-hardware, and the
  app. AI bit 14 (`0x4000`, BLE processor missed the first message) is kept until a final
  `selftest` reply has carried it, then cleared; nothing else clears it (#246, 4 Nov 2026). AI bit
  15 (`0x8000`, provisional): a reply sent before the SD card start-up finished (warm boot) has it
  set, and the AI processor then sends a final `selfTest` unprompted. The BLE processor must accept
  that in every awake state, and ship first (#290, 7 Nov 2026).

**The "Sleep" message has a size limit.** At sleep the AI processor sends every op parameter to
the BLE processor in one "Sleep" message (`if_task.c`), at most 244 bytes. Typical is about 110
bytes, and a busy camera about 160. But only up to 39 ops is it certain to fit whatever the values
(every value `65535` is 240 bytes). Beyond that, large values can cut the message short: safe, but
the BLE processor keeps stale values for the ops lost off the end. The BLE processor reads only the
ops it knows (`aiProcessorSaveStats()`), so a new op is ignored there until `aiProcessor.h` gains it.
