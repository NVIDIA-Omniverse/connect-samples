@echo off

setlocal

set ROOT_DIR=%~dp0..
pushd "%ROOT_DIR%"

set PYTHON=%ROOT_DIR%\_build\target-deps\python\python.exe
set PYTHONPATH=%ROOT_DIR%\source\pyHelloWorld

if not exist "%PYTHON%" (
    echo Python, USD, and Omniverse dependencies are missing. Run "repo.bat build" to configure them.
    popd
    exit /b
)

"%PYTHON%" -s tests\test_all.py %*

popd

EXIT /B %ERRORLEVEL%
