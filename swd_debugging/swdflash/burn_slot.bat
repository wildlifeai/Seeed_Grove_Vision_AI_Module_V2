@echo off
echo %0 %*
if "%1" == "" (
    echo Usage: burn_slot.bat ^<slot^> ^<file^>
    echo   slot  0 = Slot A,  1 = Slot B
    exit /b 1
)
if "%2" == "" (
    echo Usage: burn_slot.bat ^<slot^> ^<file^>
    echo   slot  0 = Slot A,  1 = Slot B
    exit /b 1
)
python burn_slot.py %1 %2
