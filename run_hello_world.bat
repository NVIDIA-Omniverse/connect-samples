@echo off

set CARB_APP_PATH=%~dp0\_build\windows-x86_64\release

pushd "%~dp0"
call "%CARB_APP_PATH%\HelloWorld.exe" %*
if errorlevel 1 ( echo Error running HelloWorld )
popd

EXIT /B %ERRORLEVEL%
