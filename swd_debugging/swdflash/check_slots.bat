@echo off
echo %0 %*
if "%1" == "" (
    python check_slots.py dump_slot_a.bin dump_slot_b.bin
) else (
    python check_slots.py %1 %2 %3
)
