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
