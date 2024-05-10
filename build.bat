:: This file is a quick way to access the "repo build" tool since the samples have always had "build.bat"
@echo off
@setlocal

:: making sure the errorlevel is 0
if  %errorlevel% neq 0 ( cd > nul )

call "%~dp0tools\packman\python.bat" %~dp0tools\repoman\repoman.py build %*
if %errorlevel% neq 0 ( goto Error )

:Success
exit /b 0

:Error
exit /b %errorlevel%
