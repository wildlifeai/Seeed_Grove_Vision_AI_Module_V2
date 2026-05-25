XIP memeory chunks from faulty board WILD-QSMM
-----------------------------------------------

Board QSMM was resturned from Victor (and Tommy?) as 'dead' on 22 May 2026.

I created various tools to read and anaylyse the contents of the XIP flash in an attempt to diagnose what had gone wrong. see elsewhere for these investigations.

The files are:

* dump_slot_a.bin = 1M from the first firmware image
* dump_slot_b.bin = 1M from the second firmware image
* dump_selector.bin = 4k from the slot selector

It appears that:

* dump_slot_a.bin = corrupted (mostly empty, only early sectors present)
* dump_slot_b.bin = intact
* dump_selector.bin = points to slot b

So the question is why it is still dead...
