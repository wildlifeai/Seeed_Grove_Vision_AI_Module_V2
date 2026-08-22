@echo off
echo ===== Flash Dump =====
echo Dumping Slot A   (0x00000000, 1 MB) ...
python swdflash.py --operation dump --method Direct --bin dump_slot_a.bin   --addr 0x00000000 --dump_size 0x100000
if errorlevel 1 goto :fail

echo Dumping Slot B   (0x00100000, 1 MB) ...
python swdflash.py --operation dump --method Direct --bin dump_slot_b.bin   --addr 0x00100000 --dump_size 0x100000
if errorlevel 1 goto :fail

echo Dumping Selector (0x00FFF000, 4 KB) ...
python swdflash.py --operation dump --method Direct --bin dump_selector.bin --addr 0x00FFF000 --dump_size 0x1000
if errorlevel 1 goto :fail

echo ===== Done =====
goto :eof

:fail
echo ===== FAILED =====
exit /b 1
