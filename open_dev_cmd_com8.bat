@echo off
setlocal EnableExtensions

rem Open a persistent development cmd window for the current repo.
rem - /k keeps the window open, so cmd/doskey history stays alive after scripts.
rem - UTF-8 and ANSI/VT are enabled for readable colored output.

start "K210 ESP COM8 dev console" cmd.exe /k "chcp 65001 >nul && set PYTHONUTF8=1 && set PYTHONIOENCODING=utf-8 && doskey /listsize=9999 && reg add HKCU\Console /v VirtualTerminalLevel /t REG_DWORD /d 1 /f >nul 2>nul && cd /d %~dp0 && echo Dev console ready: %CD% && echo KSD COM8 @ 921600 && echo Use: call run_esp8285_rtos_hello_via_k210.bat COM8"
