@echo off
echo %0 %*
if "%1" == "" (
    echo Usage: erase_app.bat ^<slot^>
    echo   slot  0 = Slot A,  1 = Slot B
    exit /b 1
)
python erase_app.py %1
