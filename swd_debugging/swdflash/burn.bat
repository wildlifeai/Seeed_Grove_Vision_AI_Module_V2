@echo off
echo %0
echo %1
if "%1" == "" (
    echo "Programming 'output.img'"
    python swdflash.py --bin="output.img" --addr=0x0
) else (
    python swdflash.py --bin="%1" --addr=0x0
)