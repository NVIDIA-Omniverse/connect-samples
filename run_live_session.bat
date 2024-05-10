@echo off

set CARB_APP_PATH=%~dp0\_build\windows-x86_64\release

pushd "%~dp0"
call "%CARB_APP_PATH%\LiveSession.exe" %*
if errorlevel 1 ( echo Error running LiveSession )
popd

EXIT /B %ERRORLEVEL%
