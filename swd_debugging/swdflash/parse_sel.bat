@echo off
echo %0 %*
if "%1" == "" (
    python parse_selector.py dump_selector.bin
) else (
    python parse_selector.py "%1"
)
