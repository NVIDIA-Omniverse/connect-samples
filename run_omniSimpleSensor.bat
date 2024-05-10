@echo off

pushd "%~dp0"
call _build\windows-x86_64\release\omniSimpleSensor.exe %*
if errorlevel 1 ( goto end )
set /A t = 0
:loop
START _build\windows-x86_64\release\omniSensorThread.exe %1 %t% %3
set /A t = %t% + 1
if %t% LSS %2 goto loop
popd
:end