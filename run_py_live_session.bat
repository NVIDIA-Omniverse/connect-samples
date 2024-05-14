@echo off

setlocal

pushd "%~dp0"

set ROOT_DIR=%~dp0

set CARB_APP_PATH=%ROOT_DIR%_build\windows-x86_64\release
set PYTHONHOME=%CARB_APP_PATH%\python-runtime
set PYTHON=%PYTHONHOME%\python.exe

set PATH=%PATH%;%PYTHONHOME%;%CARB_APP_PATH%
set PYTHONPATH=%CARB_APP_PATH%\python;%CARB_APP_PATH%\bindings-python

if not exist "%PYTHON%" (
    echo Python, USD, and Omniverse dependencies are missing. Run "repo.bat build" to configure them.
    popd
    exit /b
)

"%PYTHON%" -s source\pyHelloWorld\liveSession.py %*

popd

EXIT /B %ERRORLEVEL%
