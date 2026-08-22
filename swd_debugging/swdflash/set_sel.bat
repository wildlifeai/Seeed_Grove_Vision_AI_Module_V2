@echo off
echo %0 %*
if "%1" == "" (
    echo Usage: set_sel.bat ^<slot^>
    echo   slot  0 = Slot A,  1 = Slot B
    exit /b 1
)
python set_selector.py %1
