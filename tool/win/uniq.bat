@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2018 Toshiba Corporation

if not exist "%1" do exit /B 1

set PREV=
for /f "tokens=* delims=" %%i in ( %1 ) do (
    call :sub %%i
)
goto :exit 

:sub 
    if not "%PREV%"=="%*" (
        set PREV=%*
        echo %*
    )

:exit
exit /B 0
