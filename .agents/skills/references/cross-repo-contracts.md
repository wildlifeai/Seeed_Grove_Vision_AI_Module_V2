# Cross-repo contracts

What the mobile app and the nRF firmware depend on here. Never change one of these
unilaterally.

# 6. Cross-repo contracts

EXIF fields (Model, MakerNote), op-parameter indices/defaults, and BLE command syntax are
consumed by ww-mobile-app, ww-website and ww-backend, and mirrored in ww-hardware's
`aiProcessor.h`. Never change one unilaterally: file a `Decide:` issue, agree the
contract, and land the change with the consumers in view.

**The "Sleep" message has a size limit.** At sleep the AI processor sends every op parameter to
the BLE processor in one "Sleep" message (`if_task.c`), at most 244 bytes. Typical is about 110
bytes, and a busy camera about 160. But only up to 39 ops is it certain to fit whatever the values
(every value `65535` is 240 bytes). Beyond that, large values can cut the message short: safe, but
the BLE processor keeps stale values for the ops lost off the end. The BLE processor reads only the
ops it knows (`aiProcessorSaveStats()`), so a new op is ignored there until `aiProcessor.h` gains it.
