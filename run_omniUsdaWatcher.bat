@echo off

pushd "%~dp0"
call _build\windows-x86_64\release\omniUsdaWatcher.exe %*
if errorlevel 1 ( echo Error running omniUsdaWatcher )
popd